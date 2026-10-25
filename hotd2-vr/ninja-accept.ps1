# Ninja Assault reproducible checks; runtime fixtures remain private under build-win.
param([ValidateSet('units','gameplay')][string]$Only='units')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if($Only -eq 'units') {
 foreach($name in 'ninja_game_test','ninja_input_test','ninja_calibration_test','ninja_state_test','ninja_panel_test','gun_input_test') {
  & (Join-Path $PSScriptRoot "re\$name.cmd")
  if($LASTEXITCODE -ne 0){throw "$name failed"}
 }
 & (Join-Path $PSScriptRoot 'ninja-dual-hands-test.ps1')
 if($LASTEXITCODE -ne 0){throw 'Dual hand checks failed'}
 & (Join-Path $PSScriptRoot 'package-release-tests.ps1')
 if($LASTEXITCODE -ne 0){throw 'Package regression checks failed'}
 exit 0
}
if(-not $env:VRLGH_GAMES){throw 'Set VRLGH_GAMES to your own World ninjaslt.zip and naomi.zip folder.'}
$at=@('3=pose left 0 1.6 -0.25 0 0','3.2=pose right 0 1.6 -0.25 0 0',
 '35=xr right/a 1 200','40=xr right/a 1 200','45=xr right/a 1 200',
 '47=poke 0c3869c2 2 100','48=xr left/trigger 1 150','49=xr right/trigger 1 150',
 '51=xr left/x 1 400','51.8=poke 0c3869c2 2 100','51.9=poke 0c3869f2 2 100')
foreach($second in 52..57){$at+="$second=xr left/trigger 1 150"}
foreach($second in 52..59){$at+="$second.3=xr right/trigger 1 150"}
$at+=@('57.5=poke 0c3869c2 2 100','57.6=poke 0c3869f2 2 100',
 '58=xr left/trigger 1 150','58.7=xr left/trigger 1 150','59=xr left/y 1 150',
 '60=xr left/trigger 1 150','60.3=xr right/trigger 1 150','61.3=xr right/trigger 1 150',
 '62=xr right/b 1 150','63.3=xr right/trigger 1 150',
 '65=poke 0c3869c2 2 100','65.1=poke 0c3869f2 2 100',
 '66=xr left/menu 1 150','67=shot menu','68=xr left/menu 1 150',
 '69=pose head 0 1.6 0 25 -20','70=shot headturn')
# Health writes are explicitly test-only, to keep the measured first wave stable.
& (Join-Path $PSScriptRoot 'vr-test.ps1') -Game ninjaslt -Name ninja-xr-accept -Port 27046 -Seconds 73 -XrSim -Visible `
 -Config 'config:vr.GameHands=yes,config:vr.BuiltInGun=no,config:vr.WidenFov=yes,config:vr.FovScale=2' -At $at `
 -Expect 'XR: the gun is in the left hand','XR: the gun is in the right hand','XR: player 2 joins','XR: menu open','XR: menu closed','VR: cutscene on','VR: cutscene off'
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
$log=Join-Path $repo 'build-win\vrtest\ninja-xr-accept\flycast.log'
foreach($player in 1,2) {
 $kicks=@(Select-String -LiteralPath $log -Pattern "XR: P$player kick:").Count
 $hand=if($player -eq 1){'left'}else{'right'}
 $clicks=@(Select-String -LiteralPath $log -Pattern "XRSIM: haptic $hand 0\.019 0\.350").Count
 if($kicks -ne 9 -or $clicks -ne 2){throw "P$player expected9realrounds/2emptyclicks, got$kicks/$clicks"}
 Write-Output "PASS P$player 9 real-round kicks and 2 empty clicks"
}
exit 0
