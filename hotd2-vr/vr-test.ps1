<#
  Runs a game on the Windows build (build-win.cmd) for a scripted test, for adding a game
  (notes\adding-a-game.md, section 7): light-gun input over UDP at set times, window
  screenshots, rips, then checks over flycast.log. accept.ps1 runs the acceptance scenarios.

  .\hotd2-vr\vr-test.ps1 -Game confmiss -Name boot -Seconds 40 -Expect 'NAOMI GAME ID'
  .\hotd2-vr\vr-test.ps1 -Game mok -Seconds 90 -At '30=start','70=fire 0 5000 5000','72=shot','75=rip 1 1 1'
  .\hotd2-vr\vr-test.ps1 -Game $env:VRLGH_HOTD2 -Config 'config:vr.FovScale=3'

  -Port     private UDP input port (default 27026); never shared with the reference tests.
  -Visible  show the interactive research window for window captures; hidden by default.
  -Game     a MAME set name, as <set>.zip in $env:VRLGH_GAMES (with its CHD in <set>\ and
            naomi.zip), or the path of a disc image (its gun in port A).
  -At       "<seconds>=<action>", seconds from Flycast's start (checked before the run):
              start [p]           Start, held 200 ms (p: player 0-3, default 0)
              aim p x y           where the gun rests (0..10000 across and down the screen)
              fire p x y          aim there and pull the trigger for 100 ms
              reload [p]          off screen with the reload button, 200 ms
              button p bits ms    hold buttons (1 trigger, 2 reload, 4 start, 8-64 d-pad, 128 B)
              shot [label]        window screenshot t<seconds>[-label].png (label: letters, digits, . _ -)
              rip count interval ram [texhex polys]   write rip.request
              mark <text>         just the line in actions.log (every action gets one)
              poke addr size value   write RAM (PC; the UDP line POKE): size 1, 2 or 4 bytes of a
                                  hex value at a hex SH4 address, at the next VBlank, logged as
                                  "VR: poke 0c155804 = 03". Absolute values only (no add, no
                                  bit set): read the current value off a vr.WatchRam line first.
              mute s              the -At actions due in the next s seconds are left out
                                  ("(muted)" in actions.log): with -On, a trigger held through
                                  a scripted route (-On '...|0|mute 4', '...|0.2|button 0 1 3000')
            With -XrSim, the simulated headset and controllers (metres, degrees, in its stage space):
              xr <hand>/<input> <value> [ms]   left or right; a b x y menu thumbstick: .../<input>/click,
                                  trigger squeeze: .../<input>/value, one with a / as it is under
                                  /user/hand/<hand>/input/ (xr left/trackpad/force 1 300); with ms
                                  back to 0 after that long
              stick <hand> <x> <y> [ms]       the thumbstick (-1..1), back to the middle after ms
              pose <head|left|right> <x> <y> <z> <yaw> <pitch> [roll]   where it is and faces
              xrstate <focused|visible>       visible: focus lost (a system menu over the app)
            From the first gun action on, every player used gets "LG p x y buttons" every 50 ms;
            from the first simulator action on, the simulator gets its "XR <path> <value>",
            "XRPOSE ..." and "XRSTATE ..." lines every 50 ms.
  -On       "<regex>|<delay s>|<action>[|<times>]", scene keys: while Flycast runs, each new
            flycast.log line the regex matches (case-sensitive; it cannot hold a |: use a
            character class) schedules the action (as for -At) that many seconds later, for the
            first <times> such lines (default 1); actions.log gets "on <regex> -> <action>". A
            keyed shot is named after the time it runs. With the RAM and texture watches below:
              -On 'VR: ram 0c15580[4-9] .* -> 2d|0|rip 3 30 1'
            A "-> 2d" key matches byte watches only: 2- and 4-byte watches print 4 and 8 digits
            ("-> 002d", "-> 0000002d").
  -Config   more "config:x=y,..." settings, applied last. PC test aids (vr_reproject.cpp):
            vr.WatchRam=addr[:1|2|4],...  (hex SH4 addresses, at most 16) logs each change, read
            every VBlank, as "VR: ram 0c155808 06 -> 2d" (2 and 4 bytes: 4 and 8 hex digits);
            vr.WatchTex=tex,...  (hex VRAM addresses, riplib's .tex) logs "VR: tex 7e2800 shown
            (78 polys)" on the first pass with it and "VR: tex 7e2800 gone" 10 passes after.
            Their lists are quoted for Flycast here (a comma would end the setting).
  -DebugShots N   the game's own frames every N seconds of game time (shot_*.png).
  -Expect / -Reject   regexes (case-sensitive) that must / must not match a flycast.log line.
  -Xr       PC VR (OpenXR) instead of the flat window.
  -XrSim    the headset path against Flycast's built-in OpenXR simulator (vr.XrSim): the left
            eye in the window, controllers from the actions above.

  Everything goes to build-win\vrtest\<Name>\ (emptied first), Flycast's working directory:
  flycast.log, actions.log, screenshots, rip\. build-win\emu.cfg and the game's saves in
  build-win\data (<game file>.nvmem, .nvmem2, .eeprom) are put back as they were after the run
  (emu.cfg.before, saves.before there; a save the run made is deleted); the run's own emu.cfg
  is kept as emu.cfg.after. Exit 0 all checks pass, 1 a check failed,
  2 Flycast exited early.
#>
param(
	[Parameter(Mandatory = $true)][string]$Game,
	[string]$Name = 'test',
	[string]$Config = '',
	[string[]]$At = @(),
	[double]$Seconds = 60,
	[int]$DebugShots = 0,
	[string[]]$Expect = @(),
	[string[]]$Reject = @(),
	[string[]]$On = @(),
	[switch]$Xr,
	[switch]$XrSim,
    [ValidateRange(1024, 65535)][int]$Port = 27026,
    [switch]$Visible
)
$ErrorActionPreference = 'Stop'
# the run's folder gets emptied: nothing that could reach out of build-win\vrtest
if (-not $Name -or $Name -match '^\.+$|\.\.|[\\/:*?\[\]]') { throw "-Name '$Name': want a plain folder name" }
$repo = Split-Path -Parent $PSScriptRoot
$buildExe = Join-Path $repo 'build-win\flycast.exe'
$exe = Join-Path $repo 'build-win\ninja-vr-player.exe'
if (-not (Test-Path -LiteralPath $buildExe)) { throw 'Build the Windows version first: hotd2-vr\build-win.cmd' }
# A distinct name also protects this run from older external harnesses that stop every flycast.exe.
Copy-Item -LiteralPath $buildExe -Destination $exe -Force
if (-not (Test-Path $exe)) { throw "Build the Windows version first: hotd2-vr\build-win.cmd" }
$runRoot = [IO.Path]::GetFullPath((Join-Path $repo 'build-win\vrtest')) + [IO.Path]::DirectorySeparatorChar
$out = [IO.Path]::GetFullPath((Join-Path $runRoot $Name))
if (-not $out.StartsWith($runRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Run output is outside build-win\vrtest.' }
$dataDir = Join-Path $repo 'build-win\data'	# Flycast's writable data dir (winmain.cpp), where DebugShots go
# Port is supplied by the private -Port parameter (default 27026).

# OpenGL with native depth and the reprojection on: what the VR hooks need. Never the
# vr.ProjAddr/ViewportAddr overrides: they would beat the game's profile.
$cfg = "config:pvr.rend=0,config:rend.NativeDepthInterpolation=yes,config:vr.Reproject=yes,config:vr.Xr=$(if ($Xr -or $XrSim) { 'yes' } else { 'no' })"
$cfg += ",config:vr.XrSim=$(if ($XrSim) { 'yes' } else { 'no' })"
$cfg += ",config:aica.Volume=0,config:vr.UdpPort=$port,log:LogToFile=yes,log:Verbosity=4"
if ($Game -match '[\\/.]') {
	if (-not (Test-Path -LiteralPath $Game -PathType Leaf)) { throw "No disc image at $Game" }
	$content = (Resolve-Path -LiteralPath $Game).Path
	$cfg += ',input:device1=7'	# the light gun in port A
} else {
	if (-not $env:VRLGH_GAMES) { throw 'Set $env:VRLGH_GAMES to the folder with the MAME sets (<set>.zip, <set>\*.chd, naomi.zip).' }
	$games = $env:VRLGH_GAMES.TrimEnd('\')	# a trailing \ would escape the quote around -config
	$content = Join-Path $games "$Game.zip"
	if (-not (Test-Path -LiteralPath $content)) { throw "No $content" }
	$cfg += ",config:Dreamcast.BiosPath=$games,config:Dreamcast.ContentPath=$games"
}
if ($DebugShots -gt 0) { $cfg += ",config:vr.DebugShots=$DebugShots" }
# (a watch list's commas would end its setting in Flycast's -config: quoted, as it takes them)
if ($Config) { $cfg += ',' + ($Config -replace '(config:vr\.Watch(?:Ram|Tex)=)([0-9a-fA-F]+(?::\d)?(?:,[0-9a-fA-F]+(?::\d)?)*)(?=,|$)', '$1''$2''') }

# the timed actions, checked before anything starts: each action's form, numbers included
$forms = @{
	start = '^start( [0-3])?$'; aim = '^aim [0-3] -?\d+ -?\d+$'; fire = '^fire [0-3] -?\d+ -?\d+$'
	reload = '^reload( [0-3])?$'; button = '^button [0-3] \d+ \d+(\.\d+)?$'; shot = '^shot( [\w.-]+)?$'
	rip = '^rip \d+ \d+ \d+( [0-9a-f]+( \d+)?)?$'; mark = '^mark .'
	xr = '^xr (left|right)/(a|b|x|y|menu|thumbstick|trigger|squeeze|\w+(/\w+)+) -?\d+(\.\d+)?( \d+(\.\d+)?)?$'
	stick = '^stick (left|right) -?\d+(\.\d+)? -?\d+(\.\d+)?( \d+(\.\d+)?)?$'
	pose = '^pose (head|left|right)( -?\d+(\.\d+)?){5,6}$'; xrstate = '^xrstate (focused|visible)$'
	poke = '^poke [0-9a-fA-F]{1,8} [124] [0-9a-fA-F]{1,8}$'; mute = '^mute \d+(\.\d+)?$'
}
$actionList = 'start [p], aim p x y, fire p x y, reload [p], button p bits ms, shot [label], rip count interval ram [texhex polys], mark <text>, poke addr size value, mute s, xr <hand>/<input> <value> [ms], stick <hand> <x> <y> [ms], pose <head|left|right> x y z yaw pitch [roll], xrstate <focused|visible>'
function Test-ActionForm([string]$text) { $verb = ($text -split ' ')[0]; return $forms.ContainsKey($verb) -and $text -match $forms[$verb] }
$simActions = 'xr', 'stick', 'pose', 'xrstate'
$events = @()
foreach ($a in $At) {
	$time, $action = $a -split '=', 2
	$text = (-split $action) -join ' '
	$t = 0.0
	if ($time -notmatch '^\d+(\.\d+)?$' -or -not [double]::TryParse($time,[Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$t) -or
			-not (Test-ActionForm $text)) {
		throw "-At '$a': want <seconds>=<action>, action one of $actionList"
	}
	if (-not $XrSim -and ($text -split ' ')[0] -in $simActions) { throw "-At '$a': simulator actions need -XrSim" }
	if ($t -ge $Seconds) { Write-Warning "-At '$a' comes at or after -Seconds ${Seconds}: it will not run" }
	$events += [pscustomobject]@{ At = $t; Index = $events.Count; Text = $text; Words = @($text -split ' ') }
}
$events = @($events | Sort-Object At, Index)
# the scene keys (-On), checked the same way
$keys = @()
foreach ($o in $On) {
	$f = @($o -split '\|')
	$delay = 0.0
	$times = 1
	$text = $(if ($f.Count -ge 3) { (-split $f[2]) -join ' ' } else { '' })
	$re = $null
	if ($f.Count -ge 1 -and $f[0]) { try { $re = [regex]::new($f[0]) } catch { $re = $null } }
	if ($f.Count -notin 3, 4 -or $null -eq $re -or $f[1] -notmatch '^\d+(\.\d+)?$' -or
			-not [double]::TryParse($f[1], [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$delay) -or
			-not (Test-ActionForm $text) -or ($f.Count -eq 4 -and ($f[3] -notmatch '^\d+$' -or -not [int]::TryParse($f[3], [ref]$times) -or $times -lt 1))) {
		throw "-On '$o': want <regex>|<delay s>|<action>[|<times>] (the regex without a |), action one of $actionList"
	}
	if (-not $XrSim -and ($text -split ' ')[0] -in $simActions) { throw "-On '$o': simulator actions need -XrSim" }
	$keys += [pscustomobject]@{ Pattern = $f[0]; Re = $re; Delay = $delay; Text = $text; Words = @($text -split ' '); Left = $times }
}

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Win {
	[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
	[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
	[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
	[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
	[DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
	[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
}
'@
# Without this, a scaled display reports a shrunken client rect and shots get cropped.
[void][Win]::SetProcessDPIAware()

# The window comes from the process, never its title (which changes with the build).
function Save-WindowShot($proc, $file) {
	$proc.Refresh()
	$h = $proc.MainWindowHandle
	if ($h -eq [IntPtr]::Zero) { return $false }
	# a minimized window has no client area to print: show it again, without taking focus
	if ([Win]::IsIconic($h)) { [void][Win]::ShowWindow($h, 4); Start-Sleep -Milliseconds 300 }
	$r = New-Object Win+RECT
	[void][Win]::GetClientRect($h, [ref]$r)
	$bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.R)), ([Math]::Max(1, $r.B))
	$g = [System.Drawing.Graphics]::FromImage($bmp)
	$hdc = $g.GetHdc()
	[void][Win]::PrintWindow($h, $hdc, 3)   # client area, render full content
	$g.ReleaseHdc($hdc); $g.Dispose()
	$bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
	return $true
}

# Stop only $p, the process launched by this invocation, in finally below.
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory -Force -Path $out | Out-Null
if ($DebugShots -gt 0) { Get-ChildItem $dataDir -Filter 'shot_*.png' -ErrorAction SilentlyContinue | Remove-Item }
$actions = Join-Path $out 'actions.log'
Set-Content $actions "game $content`r`nconfig $cfg"

# the light gun of each player: where it rests, and the buttons held until a time
$guns = @{}
function Get-Gun($player) {
	$pl = [int]$player
	if (-not $guns.ContainsKey($pl)) { $guns[$pl] = @{ X = 5000; Y = 5000; Bits = 0; Until = 0.0; HoldX = $null; HoldY = $null } }
	return $guns[$pl]
}
function Set-Hold($gun, $bits, $ms, $now) { $gun.Bits = [int]$bits; $gun.Until = $now + $ms / 1000.0; $gun.HoldX = $null; $gun.HoldY = $null }

# the simulator's lines (-XrSim), by what each sets; numbers go on as written (no culture)
$xrLines = [ordered]@{}
function Get-XrPath($handInput) {
	$hand, $what = $handInput -split '/', 2
	$base = "/user/hand/$hand/input"
	if ($what -match '/') { return "$base/$what" }
	if ($what -in 'trigger', 'squeeze') { return "$base/$what/value" }
	return "$base/$what/click"
}
function Set-XrLine($key, $line, $release, $ms, $now) {
	$xrLines[$key] = @{ Line = $line; Release = $release; Until = $(if ($ms) { $now + $ms / 1000.0 } else { $null }) }
}

# The run's settings and the game's saves come and go with it: Flycast keeps the -config ones
# in emu.cfg, and the game's NVRAM / EEPROM (<content>.nvmem, .nvmem2, .eeprom in the data dir:
# scores, play counts) would otherwise drift from run to run and change what the game does.
# Copied before anything starts; put back in the finally below, a save the run made deleted.
$emuCfg = Join-Path $repo 'build-win\emu.cfg'
$emuBefore = Join-Path $out 'emu.cfg.before'
$hadCfg = Test-Path -LiteralPath $emuCfg
if ($hadCfg) { Copy-Item -LiteralPath $emuCfg $emuBefore }
$savesBefore = Join-Path $out 'saves.before'
$saves = @(foreach ($ext in 'nvmem', 'nvmem2', 'eeprom') {
	$path = Join-Path $dataDir "$(Split-Path -Leaf $content).$ext"
	$had = Test-Path -LiteralPath $path
	if ($had) {
		New-Item -ItemType Directory -Force $savesBefore | Out-Null
		Copy-Item -LiteralPath $path $savesBefore
	}
	@{ Path = $path; Had = $had }
})
# One action, timed (-At) or keyed (-On); a shot is named after the time it was due.
function Invoke-Action($ev, [double]$now) {
	$w = $ev.Words
	Add-Content $actions ('{0,7:0.00} {1}' -f $now, $ev.Text)
	switch ($w[0]) {
		'start' { Set-Hold (Get-Gun $(if ($w.Count -gt 1) { $w[1] } else { 0 })) 4 200 $now }
		'aim' { $g = Get-Gun $w[1]; $g.X = [int]$w[2]; $g.Y = [int]$w[3] }
		'fire' { $g = Get-Gun $w[1]; $g.X = [int]$w[2]; $g.Y = [int]$w[3]; Set-Hold $g 1 100 $now }
		'reload' { $g = Get-Gun $(if ($w.Count -gt 1) { $w[1] } else { 0 }); Set-Hold $g 2 200 $now; $g.HoldX = -10000; $g.HoldY = -10000 }
		'button' { Set-Hold (Get-Gun $w[1]) $w[2] ([double]$w[3]) $now }
		'shot' {
			$file = Join-Path $out ('t{0:000.#}{1}.png' -f $ev.At, $(if ($w.Count -gt 1) { "-$($w[1])" }))
			if (Save-WindowShot $p $file) { $script:shots++ } else { Add-Content $actions '        (no window yet: no shot)' }
		}
		'rip' { Set-Content -Path (Join-Path $out 'rip.request') -Value $ev.Text.Substring(4).Trim() -NoNewline }
		'poke' {
			$msg = [Text.Encoding]::ASCII.GetBytes("POKE $($w[1]) $($w[2]) $($w[3])")
			[void]$udp.Send($msg, $msg.Length, '127.0.0.1', $port)
		}
		'mute' { $script:muteUntil = $now + [double]$w[1] }
		'xr' {
			$path = Get-XrPath $w[1]
			Set-XrLine $path "XR $path $($w[2])" "XR $path 0" $(if ($w.Count -gt 3) { [double]$w[3] }) $now
		}
		'stick' {
			$ms = $(if ($w.Count -gt 4) { [double]$w[4] })
			foreach ($axis in @(@('x', $w[2]), @('y', $w[3]))) {
				$path = "/user/hand/$($w[1])/input/thumbstick/$($axis[0])"
				Set-XrLine $path "XR $path $($axis[1])" "XR $path 0" $ms $now
			}
		}
		'pose' { Set-XrLine "pose $($w[1])" ('XRPOSE ' + ($w[1..($w.Count - 1)] -join ' ')) $null $null $now }
		'xrstate' { Set-XrLine 'state' "XRSTATE $($w[1])" $null $null $now }
	}
}

$p = $null
$clock = $null
$udp = $null
$next = 0
$early = $null
$shots = 0
$muteUntil = -1.0
$keyed = 0
$pending = [Collections.Generic.List[object]]::new()
$logPath = Join-Path $out 'flycast.log'
$tail = $null	# flycast.log as it grows (Flycast flushes each line), for the scene keys
$tailBuf = New-Object byte[] 65536
$tailChars = New-Object char[] 65536
$tailDecoder = [Text.Encoding]::UTF8.GetDecoder()	# one across reads: a character split between two keeps its bytes
$partial = ''
try {
	$p = Start-Process -FilePath $exe -ArgumentList @('-config', "`"$cfg`"", "`"$content`"") -WorkingDirectory $out -WindowStyle $(if ($Visible) { 'Normal' } else { 'Hidden' }) -PassThru
	$null = $p.Handle	# (held from the start: without it ExitCode can be empty once it has exited)
	$clock = [Diagnostics.Stopwatch]::StartNew()
	$udp = New-Object System.Net.Sockets.UdpClient
	while ($true) {
		$now = $clock.Elapsed.TotalSeconds
		if ($now -ge $Seconds) { break }
		if ($p.HasExited) { $early = $now; break }
		while ($next -lt $events.Count -and $events[$next].At -le $now) {
			$ev = $events[$next++]
			if ($now -lt $muteUntil) { Add-Content $actions ('{0,7:0.00} (muted) {1}' -f $now, $ev.Text); continue }
			Invoke-Action $ev $now
		}
		if ($keys) {
			if ($null -eq $tail -and (Test-Path -LiteralPath $logPath)) {
				$tail = [IO.FileStream]::new($logPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
			}
			if ($null -ne $tail) {
				while (($n = $tail.Read($tailBuf, 0, $tailBuf.Length)) -gt 0) {
					$c = $tailDecoder.GetChars($tailBuf, 0, $n, $tailChars, 0)
					$partial += [string]::new($tailChars, 0, $c)
				}
				$lines = $partial -split "`n"
				$partial = $lines[-1]
				for ($i = 0; $i -lt $lines.Count - 1; $i++) {
					foreach ($k in $keys) {
						if ($k.Left -gt 0 -and $k.Re.IsMatch($lines[$i])) {
							$k.Left--
							Add-Content $actions ('{0,7:0.00} on {1} -> {2}' -f $now, $k.Pattern, $k.Text)
							$pending.Add([pscustomobject]@{ At = $now + $k.Delay; Text = $k.Text; Words = $k.Words })
						}
					}
				}
			}
			foreach ($ev in @($pending | Where-Object { $_.At -le $now } | Sort-Object At)) {
				[void]$pending.Remove($ev)
				$keyed++
				Invoke-Action $ev $now
			}
		}
		foreach ($key in @($xrLines.Keys)) {
			$x = $xrLines[$key]
			if ($null -ne $x.Until -and $now -ge $x.Until) { $x.Line = $x.Release; $x.Until = $null }
			$msg = [Text.Encoding]::ASCII.GetBytes($x.Line)
			[void]$udp.Send($msg, $msg.Length, '127.0.0.1', $port)
		}
		foreach ($pl in $guns.Keys) {
			$g = $guns[$pl]
			$held = $now -lt $g.Until
			$x, $y = if ($held -and $null -ne $g.HoldX) { $g.HoldX, $g.HoldY } else { $g.X, $g.Y }
			$msg = [Text.Encoding]::ASCII.GetBytes("LG $pl $x $y $(if ($held) { $g.Bits } else { 0 })")
			[void]$udp.Send($msg, $msg.Length, '127.0.0.1', $port)
		}
		Start-Sleep -Milliseconds 50
	}
} finally {
	$ran = $(if ($null -ne $clock) { $clock.Elapsed.TotalSeconds } else { 0 })
	# each step on its own: one failing must not keep the rest (above all the put-back) from running
	try { if ($null -ne $udp) { $udp.Close() } } catch { Write-Warning "UDP client: $_" }
	try { if ($null -ne $tail) { $tail.Dispose() } } catch { Write-Warning "flycast.log: $_" }
	try {
		if ($null -ne $p -and -not $p.HasExited) {
			$p.CloseMainWindow() | Out-Null; Start-Sleep 2
			if (-not $p.HasExited) { $p.Kill(); $p.WaitForExit(5000) | Out-Null }
		}
	} catch { Write-Warning "closing Flycast: $_" }
	try { if (Test-Path -LiteralPath $emuCfg) { Copy-Item -LiteralPath $emuCfg (Join-Path $out 'emu.cfg.after') -Force } } catch { Write-Warning "emu.cfg.after: $_" }
	$notBack = @()
	try {
		if ($hadCfg) { Copy-Item -LiteralPath $emuBefore $emuCfg -Force }
		elseif (Test-Path -LiteralPath $emuCfg) { Remove-Item -LiteralPath $emuCfg }
	} catch { $notBack += "emu.cfg: $_" }
	foreach ($save in $saves) {
		try {
			if ($save.Had) { Copy-Item -LiteralPath (Join-Path $savesBefore (Split-Path -Leaf $save.Path)) $save.Path -Force }
			elseif (Test-Path -LiteralPath $save.Path) { Remove-Item -LiteralPath $save.Path }
		} catch { $notBack += "$($save.Path): $_" }
	}
	if ($notBack) { throw "not put back after the run: $($notBack -join '; ')" }
}
if ($DebugShots -gt 0) { Get-ChildItem $dataDir -Filter 'shot_*.png' -ErrorAction SilentlyContinue | Move-Item -Destination $out }
Write-Output ('{0}: {1:0} s, {2} actions, {3} keyed, {4} screenshots -> {5}' -f $Name, $ran, $next, $keyed, $shots, $out)

$log = Join-Path $out 'flycast.log'
$failed = $false
if (-not (Test-Path $log)) {
	Write-Output "FAIL no flycast.log in $out"
	$failed = $true
} else {
	foreach ($re in $Expect) {
		if (Select-String -LiteralPath $log -Pattern $re -CaseSensitive -Quiet) { Write-Output "PASS expect $re" }
		else { Write-Output "FAIL expect $re"; $failed = $true }
	}
	foreach ($re in $Reject) {
		$hit = Select-String -LiteralPath $log -Pattern $re -CaseSensitive | Select-Object -First 1
		if ($hit) { Write-Output "FAIL reject $re (line $($hit.LineNumber))"; $failed = $true }
	}
}
if ($null -ne $early) {
	$p.WaitForExit(5000) | Out-Null
	Write-Output ('Flycast exited early, at {0:0.0} s (code {1})' -f $early, $(if ($null -ne $p.ExitCode) { $p.ExitCode } else { 'unknown' }))
	exit 2
}
exit $(if ($failed) { 1 } else { 0 })

