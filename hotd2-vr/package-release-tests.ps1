<# Synthetic release checks only. Never builds, launches Flycast, reads signing keys, or packages real game data. #>
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$packageScript = Join-Path $PSScriptRoot 'package-release.ps1'
$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($packageScript, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
# Load the real validation functions and literal identity/banning configuration, without executing
# the build, SDK, signing, packaging or git-attestation orchestration.
foreach ($statement in $ast.EndBlock.Statements) {
    if ($statement -is [Management.Automation.Language.FunctionDefinitionAst]) {
        . ([scriptblock]::Create($statement.Extent.Text))
    } elseif ($statement -is [Management.Automation.Language.AssignmentStatementAst] -and
        $statement.Left.VariablePath.UserPath -in @('appId', 'appLabel', 'appVersion', 'appLauncher', 'playCmd', 'bannedNames', 'bannedDirs')) {
        . ([scriptblock]::Create($statement.Extent.Text))
    }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$script:checks = 0; $script:failures = 0
function Check([string]$name, [bool]$condition) {
    $script:checks++
    if ($condition) { Write-Host "PASS $name" } else { $script:failures++; Write-Host "FAIL $name" -ForegroundColor Red }
}
$work = Join-Path $repo ('.cache\ninja-package-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null
function Archive([string]$name, [hashtable]$entries) {
    $path = Join-Path $work $name
    $file = [IO.File]::Open($path, 'CreateNew')
    $zip = New-Object IO.Compression.ZipArchive($file, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($entryName in $entries.Keys) {
            $entry = $zip.CreateEntry($entryName)
            $stream = $entry.Open()
            try {
                $bytes = [Text.Encoding]::UTF8.GetBytes([string]$entries[$entryName])
                $stream.Write($bytes, 0, $bytes.Length)
            } finally { $stream.Dispose() }
        }
    } finally { $zip.Dispose(); $file.Dispose() }
    $path
}
try {
    Check 'Ninja application ID' ($appId -eq 'com.vrlightgunhub.ninjaassault')
    Check 'Ninja label' ($appLabel -eq 'Ninja Assault - VR4ALL')
    Check 'Ninja release base version' ($appVersion -eq '1.0.5')
    Check 'Ninja setup launcher' ($appLauncher -eq 'com.flycast.emulator.NinjaStartActivity')
    Check 'Ninja PC launcher name' ($playCmd -eq 'Play Ninja Assault.cmd')
    $banned = @('roms/NINJASLT.ZIP','roms/lupinshoo/gds-0018.chd','gds-0018.cue','gds-0018.gdi',
        '317-0325-jpn.pic','data/gun-lp-pistol.bin','rip/frame.txt','shot_00001.png','naomi.zip')
    $entries = @{}; foreach ($name in $banned) { $entries[$name] = 'synthetic fixture' }
    $archive = Archive 'game-data.zip' $entries
    $found = @(Find-GameData $archive)
    Check 'all inherited game-data bans retained' ($found.Count -eq $banned.Count)
    $cartridgeNames = @('roms/nja2vera.2d', 'assets/nja1vera.2c', 'assets/nja1ma10.4b',
        'assets/jyu1_prg0a.ic3', 'assets/epr-21578h.ic27', 'data/gun-nj-pistol.bin')
    $cartridgeEntries = @{}; foreach ($name in $cartridgeNames) { $cartridgeEntries[$name] = 'synthetic fixture' }
    $archive = Archive 'cartridge-data.zip' $cartridgeEntries
    Check 'raw Ninja cartridge BIOS and generated gun names rejected' (@(Find-GameData $archive).Count -eq $cartridgeNames.Count)
    $archive = Archive 'secrets.zip' @{'release.jks'='synthetic'; 'release-signing.properties'='synthetic'; 'keys/debug.keystore'='synthetic'; '.vrlightgunhub/note.txt'='synthetic'}
    Check 'release signing material is excluded' (@(Find-GameData $archive).Count -eq 4)
    $archive = Archive 'uncredited.zip' @{'data/models/guns/fake.glb'='synthetic'}
    Check 'uncredited model rejected' (@(Find-UncreditedModels $archive).Count -eq 1)
    $archive = Archive 'credited.zip' @{'data/models/guns/fake.glb'='synthetic'; 'CREDITS.txt'="File: guns/fake.glb`nSynthetic test model, no game data"}
    Check 'credited model accepted' (@(Find-UncreditedModels $archive).Count -eq 0)
    $gpl = Get-Content -LiteralPath (Join-Path $repo 'core/deps/picotcp/LICENSE.GPLv3') -Raw
    $mit = Get-Content -LiteralPath (Join-Path $repo 'core/deps/cgltf/LICENSE') -Raw
    $licence = "Ninja Assault - VR4ALL`n$gpl`n$mit"
    $archive = Archive 'licensed.zip' @{'LICENSE.txt'=$licence}
    Check 'GPL-3 and cgltf notice accepted' (-not (Find-LicenceFault $archive))
    $archive = Archive 'missing-mit.apk' @{'assets/LICENSE.txt'=$gpl}
    Check 'missing cgltf notice rejected' ([bool](Find-LicenceFault $archive))
    $archive = Archive 'missing-gpl.apk' @{'assets/LICENSE.txt'=$mit}
    Check 'missing GPL-3 rejected' ([bool](Find-LicenceFault $archive))
    $archive = Archive 'missing-license.zip' @{'readme.txt'='synthetic'}
    Check 'missing licence rejected' ([bool](Find-LicenceFault $archive))
    Check 'matching binary commit accepted' (Test-NamesCommit 'synthetic binary' ([Text.Encoding]::ASCII.GetBytes('v0-1-gabc123')) 'abc123')
    Check 'different binary commit rejected' (-not (Test-NamesCommit 'synthetic binary' ([Text.Encoding]::ASCII.GetBytes('v0-1-gdef456')) 'abc123'))
    Check 'longer commit with matching prefix rejected' (-not (Test-NamesCommit 'synthetic binary' ([Text.Encoding]::ASCII.GetBytes('v0-1-gabc123f')) 'abc123'))

    $tree = @('E: manifest', '  E: uses-permission', '    A: android:name(0x01010003)="com.oculus.permission.RENDER_MODEL"',
        '  E: application', '    E: activity', '      A: android:name(0x01010003)="com.flycast.emulator.NinjaStartActivity"',
        '      E: intent-filter', '        E: category', '          A: android:name(0x01010003)="android.intent.category.LAUNCHER"')
    Check 'manifest launcher parser uses activity owner' (@(Get-LauncherOwners $tree) -join ',' -eq 'com.flycast.emulator.NinjaStartActivity')
    $badging = @("package: name='com.vrlightgunhub.ninjaassault' versionCode='6' versionName='1.0.5-gabc123'", "application-label:'Ninja Assault - VR4ALL'")
    $hasIdentityCheck = [bool](Get-Command Find-AppIdentityFault -ErrorAction SilentlyContinue)
    Check 'APK metadata validator is independently testable' $hasIdentityCheck
    if ($hasIdentityCheck) {
        Check 'valid Ninja APK metadata accepted' (@(Find-AppIdentityFault $badging $tree 'abc123' $false).Count -eq 0)
        foreach ($case in @(
            @('package', 'com.vrlightgunhub.ninjaassault', 'com.vrlightgunhub.confmiss'),
            @('version code', "versionCode='6'", "versionCode='9'"),
            @('base version', '1.0.5-gabc123', '1.2.0-gabc123'),
            @('commit version', '1.0.5-gabc123', '1.0.5-gdef456'),
            @('label', 'Ninja Assault - VR4ALL', 'Confidential Mission VR'))) {
            $changed = @($badging | ForEach-Object { $_.Replace($case[1],$case[2]) })
            Check "wrong APK $($case[0]) rejected" (@(Find-AppIdentityFault $changed $tree 'abc123' $false).Count -gt 0)
        }
        $changed = @($tree | ForEach-Object { $_.Replace('NinjaStartActivity','CmStartActivity') })
        Check 'old setup activity rejected' (@(Find-AppIdentityFault $badging $changed 'abc123' $false).Count -gt 0)
        $changed = @($tree | Where-Object { $_ -notmatch 'RENDER_MODEL' })
        Check 'missing controller-model permission rejected' (@(Find-AppIdentityFault $badging $changed 'abc123' $false).Count -gt 0)
        $changed = @($badging | ForEach-Object { $_.Replace('abc123','def456') })
        Check 'test build keeps identity while allowing another commit' (@(Find-AppIdentityFault $changed $tree 'abc123' $true).Count -eq 0)
    }
    $hasPcCheck = [bool](Get-Command Find-PcArchiveFault -ErrorAction SilentlyContinue)
    Check 'portable PC archive validator is independently testable' $hasPcCheck
    if ($hasPcCheck) {
        $files = @('flycast.exe','emu.cfg','README.txt','LICENSE.txt','Play Ninja Assault.cmd')
        $readme = 'Ninja Assault - VR4ALL. Implementation in progress. World NJA2 Ver.A. roms\ninjaslt.zip roms\naomi.zip Play Ninja Assault.cmd'
        $entries = @{'flycast.exe'='synthetic'; 'emu.cfg'="[config]`npvr.rend = 0`nrend.NativeDepthInterpolation = yes`nvr.Xr = yes"; 'README.txt'=$readme; 'LICENSE.txt'=$licence; 'Play Ninja Assault.cmd'='start "" /D "%~dp0" "%~dp0flycast.exe" "%~dp0roms\ninjaslt.zip"'}
        $archive = Archive 'portable-valid.zip' $entries
        Check 'exact portable Ninja archive accepted' (@(Find-PcArchiveFault $archive $files).Count -eq 0)
        $goodConfig = $entries['emu.cfg']
        $entries['emu.cfg'] = $goodConfig.Replace('pvr.rend', 'pvrXrend')
        $archive = Archive 'portable-typo-default.zip' $entries
        Check 'misspelled renderer default rejected' (@(Find-PcArchiveFault $archive $files).Count -gt 0)
        $entries['emu.cfg'] = $goodConfig.Replace('vr.Xr = yes', 'vr.Xr = no')
        $archive = Archive 'portable-vr-off.zip' $entries
        Check 'VR-off portable default rejected' (@(Find-PcArchiveFault $archive $files).Count -gt 0)
        $entries['emu.cfg'] = $goodConfig
        $goodLauncher = $entries['Play Ninja Assault.cmd']
        $entries['Play Ninja Assault.cmd'] = $goodLauncher.Replace('ninjaslt.zip','confmiss.zip')
        $archive = Archive 'portable-wrong-launch.zip' $entries
        Check 'foreign-game launcher rejected' (@(Find-PcArchiveFault $archive $files).Count -gt 0)
        $entries['Play Ninja Assault.cmd'] = $goodLauncher
        # Exercise the actual literal README/config generators and actual launcher, without
        # running the package orchestration or consuming any build artefact.
        $readmeStatement = $ast.EndBlock.Statements | Where-Object {
            $_ -is [Management.Automation.Language.AssignmentStatementAst] -and $_.Left.VariablePath.UserPath -eq 'readme'
        } | Select-Object -First 1
        $generatedReadme = $readmeStatement.Right.Find({param($n) $n -is [Management.Automation.Language.StringConstantExpressionAst]}, $true).Value
        $generatedConfig = $ast.Find({param($n) $n -is [Management.Automation.Language.StringConstantExpressionAst] -and $n.Value.StartsWith('[config]')}, $true).Value
        $production = $entries.Clone()
        $production['README.txt'] = $generatedReadme
        $production['emu.cfg'] = $generatedConfig
        $production['Play Ninja Assault.cmd'] = if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'assets/Play Ninja Assault.cmd')) { Get-Content -LiteralPath (Join-Path $PSScriptRoot 'assets/Play Ninja Assault.cmd') -Raw } else { '' }
        $archive = Archive 'portable-production-templates.zip' $production
        Check 'production README config and launcher satisfy portable checks' (@(Find-PcArchiveFault $archive $files).Count -eq 0)
        Check 'packaged README explicitly marks implementation in progress' ($generatedReadme.Contains('Implementation in progress.'))
        $entries['README.txt'] = $readme.Replace('Ninja Assault - VR4ALL','Confidential Mission VR')
        $archive = Archive 'portable-wrong-readme.zip' $entries
        Check 'stale CM README rejected' (@(Find-PcArchiveFault $archive $files).Count -gt 0)
        $entries['README.txt'] = $readme + ' roms\lupinshoo\gds-0018.chd'
        $archive = Archive 'portable-stale-disc.zip' $entries
        Check 'disc-era setup instructions rejected for cartridge release' (@(Find-PcArchiveFault $archive $files).Count -gt 0)
        $entries['README.txt'] = $readme.Replace('Implementation in progress.', 'Complete release.')
        $archive = Archive 'portable-unqualified-release.zip' $entries
        Check 'README without experimental implementation status rejected' (@(Find-PcArchiveFault $archive $files).Count -gt 0)
        $entries['README.txt'] = $readme
        $entries['unexpected.txt'] = 'synthetic'
        $archive = Archive 'portable-extra.zip' $entries
        Check 'unexpected portable archive file rejected' (@(Find-PcArchiveFault $archive $files).Count -gt 0)
        $entries.Remove('unexpected.txt'); $entries.Remove('emu.cfg')
        $archive = Archive 'portable-missing.zip' $entries
        Check 'missing portable archive file rejected' (@(Find-PcArchiveFault $archive $files).Count -gt 0)
    }
    $launcher = Join-Path $PSScriptRoot 'assets/Play Ninja Assault.cmd'
    Check 'Ninja launcher exists' (Test-Path -LiteralPath $launcher)
    if (Test-Path -LiteralPath $launcher) {
        $text = Get-Content -LiteralPath $launcher -Raw
        Check 'launcher uses portable working directory' ($text -match 'start\s+""\s+/D\s+"%~dp0"')
        Check 'launcher selects original Ninja set' ($text.Contains('%~dp0roms\ninjaslt.zip'))
    }
    Write-Host "Release checks: $script:checks total, $script:failures failed"
} finally {
    $resolved = [IO.Path]::GetFullPath($work)
    $cache = [IO.Path]::GetFullPath((Join-Path $repo '.cache')) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($cache, [StringComparison]::OrdinalIgnoreCase)) { throw 'Test cleanup escaped private cache' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
if ($script:failures) { exit 1 }

exit 0
