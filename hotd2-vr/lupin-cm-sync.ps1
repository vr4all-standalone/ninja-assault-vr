# Final CM feature acceptance, using Lupin's own game state and private harness.
param([ValidateSet('labels-left','p1-left','cutscene')][string[]]$Only=@('labels-left','p1-left','cutscene'),[switch]$Reuse)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$base='config:vr.GameHands=no,config:vr.BuiltInGun=yes,config:vr.Gun=1,config:vr.Haptics=medium,config:vr.P1Hand=left,config:vr.FovScale=2,config:vr.WidenFov=yes'
function Invoke-Run([hashtable]$run){
 if(-not $Reuse){$output=@(& (Join-Path $PSScriptRoot 'vr-test.ps1') @run);$code=$LASTEXITCODE;$output | ForEach-Object {Write-Host $_};if($code -ne 0){throw "$($run.Name) failed (exit $code)"}}
 $log=Join-Path $repo "build-win\vrtest\$($run.Name)\flycast.log"
 $text=Get-Content -LiteralPath $log -Raw
 foreach($pattern in $run.Expect){if($text -cnotmatch $pattern){throw "$($run.Name) missing $pattern"}}
 foreach($pattern in $run.Reject){if($text -cmatch $pattern){throw "$($run.Name) unexpectedly matched $pattern"}}
 return $text
}
function Clock([string]$line){if($line -match '^(\d+):(\d+):(\d+)'){return 60*[double]$matches[1]+[double]$matches[2]+[double]$matches[3]/1000};throw "No clock in $line"}
function Lines-In($lines,[string]$pattern,[double]$from,[double]$to){@($lines | Where-Object {$_ -cmatch $pattern -and (Clock $_) -ge $from -and (Clock $_) -lt $to})}
foreach($case in $Only){
 if($case -eq 'labels-left'){
  foreach($profile in @('oculus/touch_controller','valve/index_controller','htc/vive_controller','microsoft/motion_controller')){
   $kind=$profile.Split('/')[0];$start=if($kind -eq 'oculus'){'X'}elseif($kind -eq 'valve'){'A'}else{'Menu'}
   $run=@{Game='lupinshoo';Name="lp-sync-labels-$kind";Seconds=9;XrSim=$true;Port=27076;Visible=$true;Config=$base+",config:vr.XrSimProfile=$profile";At=@('3=pose left -0.15 1.45 -0.35 0 0','4=pose right 0.15 1.45 -0.35 0 0','6=shot labels');Expect=@("XR: labels set: left: $start - P1 Start",'XR: labels set: right: .*P2 Start','XR: labels set: left: .*P1 Shoot','XR: labels set: right: .*P2 Shoot');Reject=@('XR: initialisation failed','Game load failed')}
   [void](Invoke-Run $run);Write-Output "PASS P1-left labels $profile"
  }
 }elseif($case -eq 'p1-left'){
  $on=@('VR: P1 magazine 0 -> 7|4|xr left/trigger 1 150','VR: P1 magazine 0 -> 7|4.8|xr left/trigger 1 150','VR: P1 magazine 0 -> 7|5.6|xr left/y 1 150','VR: P1 magazine 0 -> 7|6.6|xr right/a 1 200',
   'VR: P2 magazine 0 -> 6|3|xr right/trigger 1 150','VR: P2 magazine 0 -> 6|3.8|xr right/trigger 1 150','VR: P2 magazine 0 -> 6|4.6|xr right/b 1 150','VR: P2 magazine 0 -> 6|5.4|xr left/trigger 1 150','VR: P2 magazine 0 -> 6|6.2|xr left/y 1 150',
   'VR: P2 magazine 0 -> 6|6.6|pose right 0.18 1.3 -0.35 0 -80','VR: P2 magazine 0 -> 6|6.7|pose left 0.18 1.3 -0.35 0 -80','VR: P2 magazine 0 -> 6|7|xr left/menu 1 150','VR: P2 magazine 0 -> 6|7.4|stick right 0 -1 150','VR: P2 magazine 0 -> 6|7.8|stick right 0 -1 150','VR: P2 magazine 0 -> 6|8.2|stick right 0 -1 150','VR: P2 magazine 0 -> 6|8.6|stick right 0 -1 150','VR: P2 magazine 0 -> 6|9|stick right 0 -1 150',
   'VR: P2 magazine 0 -> 6|9.5|stick right 1 0 150','VR: P2 magazine 0 -> 6|10|shot hands-menu','VR: P2 magazine 0 -> 6|10.4|xr right/trigger 1 2000','VR: P2 magazine 0 -> 6|11.5|xr right/b 1 150','VR: P2 magazine 0 -> 6|12.8|pose right 0.18 1.3 -0.35 0 0','VR: P2 magazine 0 -> 6|12.9|pose left 0.18 1.3 -0.35 0 0','VR: P2 magazine 0 -> 6|13.6|xr right/trigger 1 150','VR: P2 magazine 0 -> 6|14|xr left/trigger 1 150')
  $run=@{Game='lupinshoo';Name='lp-sync-p1-left';Seconds=126;XrSim=$true;Port=27076;Visible=$true;Config=$base+',config:vr.WatchRam=0ca4c808:4,0ca4c80c:4,0ca4c828:4,0ca4c82c:4';At=@('60=xr left/x 1 200','64=xr left/x 1 1000','70=xr left/x 1 200','80=pose left 0.18 1.3 -0.35 0 0');On=$on;Expect=@('XR: left hand: controller -> gun \(start\)','VR: P1 magazine 0 -> 7','VR: P2 magazine 0 -> 6','XR: player 2 joins: a gun in each hand','XR: player 1 reload button \(sensor\)','XR: player 2 reload button \(sensor\)','XR: menu Two players -> P1 right / P2 left','XR: the guns swap hands: player 1.s in the right');Reject=@('XR: initialisation failed','Game load failed')}
  $text=Invoke-Run $run;$lines=$text -split '\r?\n'
  $firstJoin=Clock (@($lines | Where-Object {$_ -match 'XR: player 2 joins:'})[0])
  if((Lines-In $lines 'Light gun P2 at .*start 1' 0 ($firstJoin-.2)).Count){throw 'X started P2 before right A joined'}
  if(-not (Lines-In $lines 'Light gun P1 at .*start 1' 59 72).Count){throw 'X did not start P1'}
  $swapLines=@($lines | Where-Object {$_ -match 'XR: the guns swap hands:'});if($swapLines.Count -ne 1){throw 'Expected exactly one hand swap'}
  $swap=Clock $swapLines[0]
  foreach($player in 1,2){
   $hand=if($player -eq 1){'left'}else{'right'};$other=if($player -eq 1){'right'}else{'left'}
   $kicks=(Lines-In $lines "XR: P$player kick:" 90 ($swap-1)).Count
   $pulses=(Lines-In $lines "XRSIM: haptic $hand 0\.048 1\.000" 90 ($swap-1)).Count
   if($kicks -lt 2 -or $pulses -ne $kicks){throw "P$player must have accepted rounds and matching $hand haptics ($kicks kicks, $pulses pulses)"}
  }
  foreach($player in 1,2){if((Lines-In $lines "XR: player $player pointed down \(reload\)" ($swap-6) $swap).Count -ne 1){throw "P$player did not establish the pre-menu downward reload latch"}}
  if((Lines-In $lines 'XR: player [12] pointed down \(reload\)' $swap ($swap+1)).Count){throw 'Held-down controllers reloaded again after hand swap'}
  if((Lines-In $lines 'XR: P[12] kick:' $swap ($swap+.7)).Count){throw 'Held menu trigger fired during swap'}
  if(-not (Lines-In $lines 'Light gun P1 at .*trigger 1' ($swap+.8) ($swap+3)).Count){throw 'Right trigger did not route to P1 after swap'}
  if(-not (Lines-In $lines 'Light gun P2 at .*trigger 1' ($swap+1.5) ($swap+4)).Count){throw 'Left trigger did not route to P2 after swap'}
  $cfg=Get-Content -LiteralPath (Join-Path $repo 'build-win\vrtest\lp-sync-p1-left\emu.cfg.after') -Raw
  if($cfg -cnotmatch '(?m)^vr\.P1Hand = right\r?$'){throw 'Hand choice was not persisted'}
  Write-Output 'PASS actual Lupin P1-left start, accepted rounds/haptic hands, independent reloads, P2 join and safe live hand swap with controllers held downward'
 }else{
  if(-not $Reuse){& (Join-Path $PSScriptRoot 'lupin-accept.ps1') -Only feedback;if($LASTEXITCODE -ne 0){throw 'Story feedback run failed'}}
  $lines=Get-Content -LiteralPath (Join-Path $repo 'build-win\vrtest\lp-feedback\flycast.log')
  $enter=Clock (@($lines | Where-Object {$_ -match 'XR: right hand: gun -> nothing \(cutscene\)'})[0])
  $leave=Clock (@($lines | Where-Object {$_ -match 'XR: right hand: nothing -> gun \(play\)'})[0])
  if($leave-$enter -lt 5){throw 'No sustained actual story interval'}
  if((Lines-In $lines 'XR: controllers shown: (left|right)' ($enter+.1) $leave).Count){throw 'Controller appeared during story'}
  if(-not (Lines-In $lines 'XR: in the hands: left nothing, right nothing' ($enter-.1) ($enter+.5)).Count){throw 'Story did not actually draw empty hands'}
  Write-Output "PASS actual Lupin story empty hands, no controller replacement, guns restored ($enter to $leave s)"
 }
}