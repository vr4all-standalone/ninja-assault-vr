<#
  Isolated Lupin simulator UI checks. Run only when this checkout's vr-test harness is idle:
    .\hotd2-vr\lupin-ui-accept.ps1 -Only menu,labels,controls,index,vive,wmr,pointer,quit
    .\hotd2-vr\lupin-ui-accept.ps1 -Only persistence,pause
    .\hotd2-vr\lupin-ui-accept.ps1 -Only pause -Reuse
  -Plan emits JSON without launching, copying configuration, or touching game files.
  Uses the supplied original lupinshoo route and vr-test.ps1's owned-PID/save/config cleanup.
  No shared model folders are moved. The long pause case uses Lupin's measured 60/64/70 s starts.
  Log/image checks establish simulator behavior only, not physical-headset validation.
#>
param(
    [ValidateSet('menu','persistence','labels','controls','index','vive','wmr','pointer','quit','pause')]
    [string[]]$Only = @('menu','labels','controls','index','vive','wmr','pointer','quit','persistence','pause'),
    [switch]$Reuse,
    [switch]$Plan,
    [ValidateRange(1024,65535)][int]$Port = 27056
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$harness = Join-Path $PSScriptRoot 'vr-test.ps1'
$vrcheck = Join-Path $PSScriptRoot 're\vrcheck.py'
$inv = [Globalization.CultureInfo]::InvariantCulture
$base = 'config:vr.GameHands=no,config:vr.BuiltInGun=yes,config:vr.FovScale=2,config:vr.WidenFov=yes'
$defaults = ',config:vr.Gun=1,config:vr.GunScale=0.68,config:vr.GunAngle=0,config:vr.Reload=0,config:vr.Laser=yes,config:vr.Haptics=medium,config:vr.P1Hand=right,config:vr.WorldScale=0.11,config:vr.HudDepth=40,config:vr.Widen=yes'
$commonReject = @('XR: initialisation failed','Game load failed')
function T([double]$n) { $n.ToString('0.0##',$inv) }
# The measured 13-row panel is shared with CM. These pointer tests run before
# play, when gunInHand() returns null: both rays start at the controller pose.
function Get-MenuAim([int]$Row,[double]$U,[switch]$Left) {
    $tx=($U-.5)*.9; $ty=1.5+.369-$Row*.063; $tz=-1.2
    $hx=if($Left){-.18}else{.18}
    $dx=$tx-$hx; $dy=$ty-1.3; $dz=$tz-(-.35)
    $yaw=[Math]::Atan2(-$dx,-$dz); $pitch=[Math]::Atan2($dy,[Math]::Sqrt($dx*$dx+$dz*$dz))
    '{0} {1}' -f (T ($yaw*180/[Math]::PI)),(T ($pitch*180/[Math]::PI))
}
$scenarios=[ordered]@{
    menu=@{ Seconds=16; Config=$base+$defaults; At=@(
        '2=xr left/menu 1 150','2.4=stick right 0 -1 150','2.7=xr right/a 1 150',
        '3.1=stick right 0 -1 150','3.4=stick right 1 0 150','3.9=stick right 1 0 150',
        '4.4=stick right 0 -1 150','4.7=stick right 1 0 150',
        '5.2=stick right 0 -1 150','5.5=stick right 1 0 150',
        '6=stick right 0 -1 150','6.3=stick right 1 0 150','6.8=stick right 0 -1 150','7.1=stick right 1 0 150',
        '7.6=stick right 0 -1 150','7.9=stick right 1 0 150',
        '8.4=stick right 0 -1 150','8.7=stick right 1 0 150',
        '9.2=stick right 0 -1 150','9.5=stick right 1 0 150',
        '10=stick right 0 -1 150','10.3=stick right 1 0 150',
        '10.8=stick right 0 -1 150','11.1=xr right/a 1 150','11.6=xr right/a 1 150',
        '12.1=stick right 0 -1 150','12.4=xr right/a 1 150',
        '12.8=stick right 0 -1 150','13.1=xr right/a 1 150')
        Expect=@('XR: menu at 1\.20 m, 13 rows, row pitch 0\.063 m, top row \+0\.369 m',
            'XR: menu Recenter$','XR: menu Gun -> Arcade gun','XR: menu Gun -> Zapper',
            'XR: menu Gun angle -> \+1$','XR: menu Reload -> Point off-screen$',
            'XR: menu Two players -> P1 left / P2 right$','XR: menu Laser -> Off$','XR: menu Haptics -> Strong$','XR: menu World size -> 0\.121$',
            'XR: menu Panel distance -> 45$','XR: menu Wider view -> Off \(next start\)$',
            'XR: how to play on$','XR: how to play off \(chosen\)$','XR: menu quit armed$',
            'XR: menu Resume$','XR: menu closed \(resumed\)$')
        Reject=@('XR: menu quit$','XR: player [12] reload button','Light gun P[12] at .*start 1','XR: player [12] shot') }
    persistence=@{ Seconds=15; Config=$base; At=@('2=xr left/menu 1 150',
        '3=stick right 0 -1 150','3.4=stick right 0 -1 150','3.8=stick right 0 -1 150',
        '4.2=stick right 1 0 150','4.7=stick right 0 -1 150','5=stick right -1 0 150',
        '5.5=stick right 0 -1 150','6=stick right 1 0 150','6.3=stick right 0 -1 150','6.8=stick right 1 0 150',
        '7.3=stick right 0 -1 150','7.8=stick right -1 0 150',
        '8.3=stick right 0 -1 150','8.8=stick right 1 0 150',
        '9.3=stick right 0 -1 150','9.8=stick right 1 0 150',
        '10.3=stick right 0 -1 150','10.8=stick right 1 0 150','12.8=xr right/b 1 150')
        Expect=@('XR: menu Gun angle -> \+2$','XR: menu Reload -> Shoot off-screen$',
            'XR: menu Two players -> P1 right / P2 left$','XR: menu Laser -> On$','XR: menu Haptics -> Medium$','XR: menu World size -> 0\.133$',
            'XR: menu Panel distance -> 50$','XR: menu Wider view -> On \(next start\)$','XR: menu closed \(resumed\)$')
        Reject=@('XR: player [12] reload button'); Requires='menu' }
    labels=@{ Seconds=16; Config=$base+$defaults; Visible=$true; At=@('2=xr left/menu 1 150',
        '3=pose left -0.1 1.45 -0.3 0 0','4=shot labels-in','4.5=pose left -0.1 0.2 0.5 0 0','5=shot labels-out',
        '6=xr right/b 1 150','7=xr right/a 1 150','8=shot gun',
        '9=xr left/menu 1 150','10=stick right 0 1 150','10.5=stick right 0 1 150','11=xr right/a 1 150',
        '12=shot how-to-play','13=xr right/b 1 150','14=shot resumed')
        Expect=@('XR: controllers: /interaction_profiles/oculus/touch_controller$',
            'XR: controllers shown: left and right$','XR: controllers shown: none$',
            'XR: labels set: right: A - P1 Start, B - P1 Reload, Trigger - P1 Shoot, Stick - Laser$',
            'XR: labels set: left: X - P2 Start, Y - P2 Reload, Trigger - P2 Shoot, Stick - Recenter, Menu$',
            'XR: labels off \(start\)$','XR: menu How to play$','XR: how to play on$',
            'XR: how to play off \(menu closed\)$','XR: labels off \(menu closed\)$') }
    # Input routing only: X and the PC-profile taps wait until after the measured ~19 s VM guards.
    # Actual accepted P2 join/leave is covered separately by Lupin's gameplay/state scenarios.
    controls=@{ Seconds=29; Config=$base+$defaults; At=@('3=xr right/a 1 200',
        '5=xr left/thumbstick 1 200','6=xr right/thumbstick 1 200','7=xr right/b 1 200',
        '8=xr left/y 1 200','9=xr left/trigger 1 150','10=xr right/trigger 1 150',
        '22=xr left/x 1 200','23=xr left/y 1 200','25=xr left/menu 1 150','26=xr right/b 1 150')
        Expect=@('Light gun P1 at .*start 1','XR: recenter \(button\)$','XR: laser off$',
            'XR: player 1 reload button \(sensor\)','XR: the gun is in the left hand$',
            'XR: the gun is in the right hand$','Light gun P2 at .*start 1',
            'XR: player 2 joins: a gun in each hand$','XR: player 2 reload button \(sensor\)',
            'XR: menu open \(paused\)$','XR: menu closed \(resumed\)$') }
    index=@{ Seconds=9; Config=$base+$defaults+',config:vr.XrSimProfile=valve/index_controller';
        At=@('4=xr left/trackpad/force 1 300');
        Expect=@('XR: controllers: /interaction_profiles/valve/index_controller$',
            'XR: labels set: left: A - P2 Start, B - P2 Reload, Trigger - P2 Shoot, Stick - Recenter, Pad squeeze - Menu$',
            'XR: menu open \(paused\)$') }
    vive=@{ Seconds=29; Config=$base+$defaults+',config:vr.XrSimProfile=htc/vive_controller';
        At=@('22=xr left/menu 1 200','25=xr left/menu 1 1200');
        Expect=@('XR: controllers: /interaction_profiles/htc/vive_controller$',
            'XR: labels set: left: Menu - P2 Start \(hold: Menu\), Pad click - P2 Reload, Trigger - P2 Shoot$',
            'XR: menu open \(paused\)$') }
    wmr=@{ Seconds=29; Config=$base+$defaults+',config:vr.XrSimProfile=microsoft/motion_controller';
        At=@('22=xr left/menu 1 200','25=xr left/menu 1 1200');
        Expect=@('XR: controllers: /interaction_profiles/microsoft/motion_controller$',
            'XR: labels set: left: Menu - P2 Start \(hold: Menu\), Pad click - P2 Reload, Trigger - P2 Shoot, Stick - Recenter$',
            'XR: menu open \(paused\)$') }
    pointer=@{ Seconds=14; Config=$base+$defaults; Visible=$true;
        At=@('2=xr left/menu 1 150',"3=pose right 0.18 1.3 -0.35 $(Get-MenuAim 6 .75)",
            '3.5=xr right/trigger 1 150','4=shot laser-off',
            "5=pose left -0.18 1.3 -0.35 $(Get-MenuAim 4 .75 -Left)",'5.5=xr left/trigger 1 150','6=shot left-pointer',
            "7=pose right 0.18 1.3 -0.35 $(Get-MenuAim 11 .5)",'8=xr right/trigger 1 150','9=xr right/trigger 1 150',
            "10=pose left -0.18 1.3 -0.35 $(Get-MenuAim 0 .5 -Left)",'10.5=xr left/trigger 1 150')
        Expect=@('XR: menu pointer click on Laser .*\(right hand\)','XR: menu Laser -> Off$',
            'XR: menu pointer click on Reload .*\(left hand\)','XR: menu Reload -> Point off-screen$',
            'XR: menu pointer click on How to play .*\(right hand\)','XR: how to play on$',
            'XR: how to play off \(chosen\)$','XR: menu pointer click on Resume .*\(left hand\)',
            'XR: menu closed \(resumed\)$') }
    quit=@{ Seconds=11; Config=$base+$defaults; At=@('2=xr left/menu 1 150',
        '2.5=stick right 0 1 150','3=xr right/a 1 150','6.5=xr right/a 1 150','7.2=xr right/a 1 150');
        Expect=@('XR: menu quit armed$','XR: menu quit$','XR: quitting$'); Early=$true }
    pause=@{ Seconds=120; Config=$base+$defaults; Visible=$true; At=@(
        '60=xr right/a 1 200','64=xr right/a 1 1000','70=xr right/a 1 200',
        '88=xr left/menu 1 150','89=shot p1','90.5=shot p2','91=pose head 0 1.6 0 25 0','92=shot p3',
        '92.5=pose head 0 1.6 0 0 0','93=xr right/b 1 150','94=shot r1','96=shot r2') +
        @(foreach($t in 98..107){"$t=xr left/menu 1 150"}) + @('108=xr left/menu 1 150',
        '109=xrstate visible','111=xrstate focused','112=shot f1','113.5=shot f2',
        '114=xr left/menu 1 150','116=shot q1','118=shot q2')
        Expect=@('VR: P1 magazine 0 -> 7','XR: menu open \(paused\)$','XR: menu closed \(resumed\)$',
            'XR: paused, redrawing the last frame'); Reject=@('XR: player [12] reload button') }
}
function Get-Dir([string]$name) { Join-Path $repo "build-win\vrtest\lp-ui-$name" }
function Read-Log([string]$name) {
    $path=Join-Path (Get-Dir $name) 'flycast.log'
    if(-not(Test-Path -LiteralPath $path)){throw "Missing $path"}
    @(Get-Content -LiteralPath $path)
}
function Assert-Count([string]$name,[string]$pattern,[int]$count,[double]$From=0,[double]$To=1e9) {
    $found=@(Read-Log $name | Where-Object {$_ -cmatch $pattern -and (Get-Clock $_) -ge $From -and (Get-Clock $_) -lt $To})
    if($found.Count -ne $count){throw "$name has $($found.Count) '$pattern' lines, expected $count at $From..$To s"}
}
function Get-Clock([string]$line) {
    if($line -notmatch '^(\d+):(\d+):(\d+)'){return -1.0}
    [double]$Matches[1]*60+[double]$Matches[2]+[double]$Matches[3]/1000
}
function Assert-Config([string]$name,[hashtable]$want) {
    $path=Join-Path (Get-Dir $name) 'emu.cfg.after'
    if(-not(Test-Path -LiteralPath $path)){throw "Missing $path"}
    $cfg=Get-Content -LiteralPath $path
    foreach($key in $want.Keys) {
        $hit=@($cfg | Where-Object {$_ -cmatch ('^'+[regex]::Escape($key)+' = (.*)$')})
        if($hit.Count -ne 1){throw "$name must save one $key setting"}
        $value=($hit[0] -split ' = ',2)[1]
        if($want[$key] -is [double]) {
            if([Math]::Abs([double]::Parse($value,$inv)-$want[$key]) -gt .00001){throw "$name saved $key=$value, expected $($want[$key])"}
        } elseif($value -cne [string]$want[$key]) {throw "$name saved $key=$value, expected $($want[$key])"}
    }
}
function Assert-Restored([string]$name) {
    $before=Join-Path (Get-Dir $name) 'emu.cfg.before'; $cfg=Join-Path $repo 'build-win\emu.cfg'
    $ok=if(Test-Path -LiteralPath $before){(Test-Path -LiteralPath $cfg) -and (Get-FileHash -LiteralPath $before).Hash -eq (Get-FileHash -LiteralPath $cfg).Hash}else{-not(Test-Path -LiteralPath $cfg)}
    if(-not $ok){throw "$name did not restore build-win\emu.cfg"}
}
function Assert-Image([string]$name,[string[]]$Arguments) {
    $out=@(& python -I $vrcheck @Arguments)
    if($LASTEXITCODE -ne 0){throw "$name image check failed: $($out -join '; ')"}
    $out | ForEach-Object {Write-Host "  $_"}
}
function Shot([string]$name,[string]$file) {
    $path=Join-Path (Get-Dir $name) $file
    if(-not(Test-Path -LiteralPath $path)){throw "Missing screenshot $path"}
    $path
}
function Invoke-Scenario([string]$name) {
    $s=$scenarios[$name]; $run=@{Game='lupinshoo';Name="lp-ui-$name";XrSim=$true;Port=$Port;
        Seconds=$s.Seconds;Config=$s.Config;At=$s.At;Expect=@('VR: camera profile for LUPIN THE THIRD  -THE SHOOTING-')+$s.Expect;
        Reject=$commonReject+@($s.Reject | Where-Object {$_})}
    if($s.Visible){$run.Visible=$true}
    if(-not $Reuse) {
        $out=@(& $harness @run); $code=$LASTEXITCODE
        $out | ForEach-Object {Write-Host "  $_"}
        Set-Content -LiteralPath (Join-Path (Get-Dir $name) 'ui-harness.console.log') -Value $out
        Set-Content -LiteralPath (Join-Path (Get-Dir $name) 'ui-harness-exit.txt') -Value $code
        if($s.Early) {
            if($code -ne 2 -or -not($out -cmatch '^Flycast exited early, at .* \(code 0\)$')){throw "$name did not exit gracefully through Quit (harness $code)"}
        } elseif($code -ne 0){throw "$name harness failed with exit $code"}
    }
    $log=Read-Log $name
    foreach($pattern in $run.Expect){if(-not($log -cmatch $pattern)){throw "$name missing '$pattern'"}}
    foreach($pattern in $run.Reject | Where-Object {$_}){if($log -cmatch $pattern){throw "$name unexpectedly matched '$pattern'"}}
    if($Reuse -and $s.Early) {
        $saved=Get-Content -LiteralPath (Join-Path (Get-Dir $name) 'ui-harness.console.log')
        $code=Get-Content -LiteralPath (Join-Path (Get-Dir $name) 'ui-harness-exit.txt')
        if($code -ne '2' -or -not($saved -cmatch '^Flycast exited early, at .* \(code 0\)$')){throw 'Saved Quit run lacks graceful-exit evidence'}
    }
}
function Test-Menu {
    Invoke-Scenario 'menu'
    Assert-Config 'menu' @{'vr.GunAngle'='1';'vr.Reload'='1';'vr.Laser'='no';'vr.Haptics'='strong';'vr.P1Hand'='left';
        'vr.WorldScale'=[double]0.121;'vr.HudDepth'=[double]45;'vr.Widen'='no';'vr.GunFile'='zapper'}
    Assert-Count 'menu' 'XR: menu quit armed$' 1
    Assert-Count 'menu' 'XRSIM: haptic right 0\.072 1\.000' 1
    if(-not $Reuse){Assert-Restored 'menu'}
}
if($Plan) {
    @($Only | ForEach-Object {[pscustomobject]@{Scenario=$_;Name="lp-ui-$_";Port=$Port;Definition=$scenarios[$_]}}) | ConvertTo-Json -Depth 8
    exit 0
}
$failed=$false
$completed=@{}
foreach($name in $Only) {
    try {
        Write-Host "Running Lupin UI $name$(if($Reuse){' (saved evidence)'})"
        if($name -eq 'menu'){Test-Menu}
        elseif($name -eq 'persistence') {
            if(-not $completed.ContainsKey('menu')){Test-Menu; $completed['menu']=$true}
            $cfg=Join-Path $repo 'build-win\emu.cfg'; $had=Test-Path -LiteralPath $cfg
            $bytes=if($had){[IO.File]::ReadAllBytes($cfg)}else{$null}
            try {
                if(-not $Reuse){Copy-Item -LiteralPath (Join-Path (Get-Dir 'menu') 'emu.cfg.after') -Destination $cfg -Force}
                Invoke-Scenario $name
                Assert-Config $name @{'vr.GunAngle'='2';'vr.Reload'='0';'vr.Laser'='yes';'vr.Haptics'='medium';'vr.P1Hand'='right';
                    'vr.WorldScale'=[double]0.1331;'vr.HudDepth'=[double]50;'vr.Widen'='yes'}
            } finally {
                if(-not $Reuse){if($had){[IO.File]::WriteAllBytes($cfg,$bytes)}elseif(Test-Path -LiteralPath $cfg){Remove-Item -LiteralPath $cfg}}
            }
        } else {
            Invoke-Scenario $name
            switch($name) {
                'labels' {
                    Assert-Count $name 'XR: labels set: right:' 1; Assert-Count $name 'XR: labels set: left:' 1
                    Assert-Image $name @('differ',(Shot $name 't004-labels-in.png'),(Shot $name 't005-labels-out.png'),'--box','0.32,0.62,0.5,0.88','--min-diff','2')
                    foreach($file in 't008-gun.png','t012-how-to-play.png','t014-resumed.png'){[void](Shot $name $file)}
                }
                {$_ -in 'index','vive','wmr'} {
                    Assert-Count $name 'XR: menu button$' 1
                    if($name -in 'vive','wmr') {
                        Assert-Count $name 'Light gun P2 at .*start 1' 1 -From 22 -To 24
                        Assert-Count $name 'Light gun P2 at .*start 1' 0 -From 24
                    }
                }
                'quit' {
                    $arms=@(Read-Log $name | Where-Object {$_ -cmatch 'XR: menu quit armed$'}); $ends=@(Read-Log $name | Where-Object {$_ -cmatch 'XR: menu quit$'})
                    if($arms.Count -ne 2 -or $ends.Count -ne 1){throw 'Quit must arm twice across timeout and execute once'}
                    $first=Get-Clock $arms[0]; $second=Get-Clock $arms[1]; $end=Get-Clock $ends[0]
                    if($second-$first -lt 3 -or $end-$second -lt .3 -or $end-$second -gt 3){throw 'Quit confirmation timing violated'}
                }
                'pause' {
                    Assert-Count $name 'XR: \d+ black frames' 0 -From 80
                    foreach($pattern in 'XR: menu open \(paused\)$','XR: menu closed \(resumed\)$','XR: paused, redrawing the last frame'){Assert-Count $name $pattern 7}
                    Assert-Image $name @('same',(Shot $name 't089-p1.png'),(Shot $name 't090.5-p2.png'),'--max-diff','1.5')
                    Assert-Image $name @('same',(Shot $name 't112-f1.png'),(Shot $name 't113.5-f2.png'),'--max-diff','1.5')
                    Assert-Image $name @('differ',(Shot $name 't090.5-p2.png'),(Shot $name 't092-p3.png'),'--min-diff','2')
                    Assert-Image $name @('differ',(Shot $name 't094-r1.png'),(Shot $name 't096-r2.png'),'--min-diff','0.5')
                    Assert-Image $name @('differ',(Shot $name 't116-q1.png'),(Shot $name 't118-q2.png'),'--min-diff','0.5')
                    # p1 freezes the flat mission movie: black outside its central plane is intentional.
                    # f1 is the later train's 3D picture, whose left strip must contain the world.
                    Assert-Image $name @('blank',(Shot $name 't089-p1.png'),'--region','0.37,0.39,0.65,0.63','--max','0.95')
                    Assert-Image $name @('blank',(Shot $name 't112-f1.png'),'--region','0.12,0.25,0.35,0.8','--max','0.95')
                }
                'pointer' {foreach($file in 't004-laser-off.png','t006-left-pointer.png'){[void](Shot $name $file)}}
            }
        }
        $completed[$name]=$true
        Write-Host "PASS Lupin UI $name"
    } catch {Write-Host "FAIL Lupin UI ${name}: $_"; $failed=$true}
}
exit $(if($failed){1}else{0})
