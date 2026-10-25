# Exercise production hand assignment, menu persistence and controller labels without an XR runtime.
param([string]$Output='')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if(-not $Output){$Output=Join-Path $repo 'build-win\vrtest\ninja-dual-hands'}
$out=[IO.Path]::GetFullPath($Output)
if(-not $out.StartsWith([IO.Path]::GetFullPath((Join-Path $repo 'build-win'))+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Test output must stay under build-win'}
New-Item -ItemType Directory -Path $out -Force | Out-Null
$hostSource=[IO.File]::ReadAllText((Join-Path $repo 'core\rend\vr\xr_host.cpp'))
$menuSource=[IO.File]::ReadAllText((Join-Path $repo 'core\rend\vr\xr_menu.cpp'))
$labelSource=[IO.File]::ReadAllText((Join-Path $repo 'core\rend\vr\xr_labels.cpp'))
$optionSource=[IO.File]::ReadAllText((Join-Path $repo 'core\cfg\option.cpp'))
function ProductionFunction([string]$Source,[string]$Signature){
 $begin=$Source.IndexOf($Signature+"`n{")
 if($begin -lt 0){$begin=$Source.IndexOf($Signature+"`r`n{")}
 if($begin -lt 0){throw "Missing production function $Signature"}
 $finish=$Source.IndexOf("`n}",$begin)
 if($finish -lt 0){throw "Missing end for $Signature"}
 return $Source.Substring($begin,$finish+2-$begin)
}
$code=@()
$defaultEnabled=$optionSource -match 'Option<bool>\s+VrDualP1Left\("vr\.DualP1Left",\s*true\)'
$oldSettingUsed=$hostSource -match 'config::VrP1Hand'
$menuNamed=$menuSource.Contains('"Dual hands"')
$code+="constexpr bool defaultEnabled=$($defaultEnabled.ToString().ToLower()), oldSettingUsed=$($oldSettingUsed.ToString().ToLower()), menuNamed=$($menuNamed.ToString().ToLower());"
$code+=ProductionFunction $hostSource 'int p1Hand()'
$code+=ProductionFunction $hostSource 'bool triggerHeld(const Gun& g, float pull)'
if($hostSource.Contains('bool dualWielding()')){$code+=ProductionFunction $hostSource 'bool dualWielding()'}else{$code+='bool dualWielding() { return dualWield; }'}
$waitBegin=$hostSource.IndexOf('if (g.triggerWait && !trigger)')
$waitEnd=$hostSource.IndexOf('// Trigger + B + Start', $waitBegin)
if($waitBegin -lt 0 -or $waitEnd -le $waitBegin){throw 'Missing production trigger-release gate'}
$code+="u32 releaseGate(Gun& g, bool trigger, u32 buttons) {`n"+$hostSource.Substring($waitBegin,$waitEnd-$waitBegin)+"`nreturn buttons;`n}"
$start=[regex]::Match($hostSource,'(?m)^\s*(?:const )?int hand1 = p1Hand\(\), hand2 = 1 - hand1;').Index
$finish=$hostSource.IndexOf("`n`t// The game's first Start of player 1's or trigger pull",$start)
if($start -le 0 -or $finish -le $start){throw 'Missing production hand assignment block'}
$code+="void updateHands(XrTime time, const bool tracked[2], const float pulls[2], bool leftStart, bool skip, u32 dpad=0) {`n"+$hostSource.Substring($start,$finish-$start)+"`nlastP1=p1; lastP2=p2;`n}"
$code+=[regex]::Match($menuSource,'const char \*const HandsNames\[2\] \{[^\r\n]+').Value
$matches=[regex]::Matches($menuSource,'(?s)case Hands:(.*?)case Laser:')
if($matches.Count -ne 2){throw 'Expected menu value and change cases'}
$code+="std::string handsMenuValue() {`nswitch(0) {case 0:"+$matches[0].Groups[1].Value+"}`nreturn {};`n}"
$code+="void toggleHandsMenu() {`nswitch(0) {case 0:"+$matches[1].Groups[1].Value+"}`n}"
$labelBegin=$labelSource.IndexOf('struct Label')
$labelEnd=$labelSource.IndexOf('// The log', $labelBegin)
if($labelBegin -lt 0 -or $labelEnd -le $labelBegin){throw 'Missing production controller label data'}
$code+=$labelSource.Substring($labelBegin,$labelEnd-$labelBegin)
$code+=ProductionFunction $labelSource 'const Words& wordsFor(const char *profile)'
[IO.File]::WriteAllText((Join-Path $out 'dual_hands_under_test.inc'),($code -join "`n"),[Text.UTF8Encoding]::new($false))
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsroot=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$envCmd=Join-Path $vsroot 'VC\Auxiliary\Build\vcvars64.bat'
$cpp=Join-Path $PSScriptRoot 're\ninja_dual_hands_test.cpp'
$exe=Join-Path $out 'ninja_dual_hands_test.exe'
$log=Join-Path $out 'build.log'
Push-Location $out
try {
 & $env:ComSpec /d /c "call `"$envCmd`" >nul && cl /nologo /std:c++17 /EHsc /O2 /W3 /I`"$out`" /Fe`"$exe`" `"$cpp`" >`"$log`" 2>&1"
 if($LASTEXITCODE -ne 0){Get-Content -LiteralPath $log;throw 'Dual hands test compilation failed'}
 & $exe
 $code=$LASTEXITCODE
}finally{Pop-Location}
exit $code
