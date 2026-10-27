# Native Service controls

The VR pause/options menu has Open Service Menu immediately above Quit. This is the
only XR-controller entry path; the A-hold shortcut has been removed. It becomes
available after initial game loading, with offline VR guns enabled. Entry queues a
single native Test pulse after the game resumes and consumes the selecting A/trigger.
Repeatedly choosing the row while Service is already active only resumes it.

In standard NAOMI Service, X sends cabinet Service/Next and A sends Test/Select. Independent bounded
pulses permit rapid taps without repeats or native Start/P2 requests. Focus, sync and
settings interruption release pulses and require fresh button releases. Triggers,
aiming and directions remain available for calibration and native input tests.

Native System EXIT boots the main game and automatically restores normal XR controls
and hides the purple guide. Submenu and Game Test transitions must preserve Service
controls. No B button is required. New game/session reset, disabled VR guns and online
play clear the mode. Keyboard/gamepad cabinet input remains additive.

Ninja Assault and Survivor use Namco's native operator menu: **X = Down**,
**A = Enter**, with the stick also available for Up/Down. While it is open,
the same VR menu row reads **Exit Service Menu** and sends the native return
command after resuming. The guide and ordinary gameplay controls restore
automatically after the game leaves its operator menu. A alone never opens or
closes Service during ordinary gameplay.
