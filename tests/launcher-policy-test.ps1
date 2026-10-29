param(
    [string]$JavaHome = $env:JAVA_HOME,
    [string]$GradleCache = $env:GRADLE_USER_HOME,
    [string]$Output = (Join-Path $env:TEMP ('vr4all-launcher-tests-' + [guid]::NewGuid().ToString('N')))
)
$ErrorActionPreference = 'Stop'
if (-not $JavaHome) { throw 'Supply -JavaHome with an existing JDK (no dependency downloads).' }
if (-not $GradleCache) { $GradleCache = Join-Path $env:USERPROFILE '.gradle' }
$repo = Split-Path -Parent $PSScriptRoot
$main = Join-Path $repo 'shell/android-studio/flycast/src/main/java/com/flycast/emulator'
$test = Join-Path $repo 'shell/android-studio/flycast/src/test/java/com/flycast/emulator'
$junit = Get-ChildItem (Join-Path $GradleCache 'caches/modules-2/files-2.1/junit/junit') -Recurse -Filter 'junit-4.13.2.jar' | Select-Object -First 1
$hamcrest = Get-ChildItem (Join-Path $GradleCache 'caches/modules-2/files-2.1/org.hamcrest/hamcrest-core') -Recurse -Filter 'hamcrest-core-1.3.jar' | Select-Object -First 1
if (-not $junit -or -not $hamcrest) { throw 'Cached JUnit 4.13.2 and Hamcrest 1.3 are required.' }
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$sources = @(Get-ChildItem $main -File | Where-Object { $_.Name -match '^(ReadyLaunchPolicy|QuestStartButtonPolicy|.*Files|.*StartStatus)\.java$' } | ForEach-Object FullName)
$tests = @(Get-ChildItem $test -Filter '*Test.java' -File)
$sources += @($tests | ForEach-Object FullName)
$cp = $junit.FullName + ';' + $hamcrest.FullName
& (Join-Path $JavaHome 'bin/javac.exe') -encoding UTF-8 -cp $cp -d $Output @sources
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$classes = @($tests | ForEach-Object { 'com.flycast.emulator.' + $_.BaseName })
& (Join-Path $JavaHome 'bin/java.exe') -cp ($Output + ';' + $cp) org.junit.runner.JUnitCore @classes
exit $LASTEXITCODE
