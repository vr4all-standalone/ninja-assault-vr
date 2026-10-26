# Quest startup from passthrough Home

The setup panel and immersive game now use separate Android tasks. The actual
`NativeGLActivity` also declares its own MAIN/DEFAULT/Quest VR/OpenXR immersive
intent filter, alongside the existing `MainActivity` alias. After Play starts the
fresh game task, setup calls `finishAndRemoveTask()` to complete the exclusive
2D-to-VR handoff. The alias, selected game path, and fresh-process restart remain
compatible with the installer and existing shortcuts.

This fixes the case where Play leaves Quest Home passthrough visible while game
audio runs. The same startup change is applied to all seven standalone ports.
It follows Meta's [hybrid activity guidance](https://developers.meta.com/vr/documentation/spatial-sdk/hybrid-apps-overview/)
and preserves regular opaque VR rendering.

## Validation

- The real Play path was reproduced on a connected Quest 3S. Task separation
  alone reached OpenXR FOCUSED but still showed passthrough to the user.
- With the actual VR activity filter added, the user confirmed the splash/game
  was visible and that startup worked. A fresh app-only eye capture also showed
  the running game. Focus and frame submission alone were not treated as visible
  success.
- `hotd2-vr/quest-launch-test.ps1` checks activity classification, task
  separation, selected-game forwarding, and exclusive panel removal. All seven
  ports failed the missing-startup requirements before the change and pass after.
- All seven existing Android unit suites passed during investigation (441 tests).
  Physical visibility was confirmed for Confidential Mission; the other six use
  the same corrected startup path and require their own full gameplay testing.

Version 1.0.4, Android standalone versionCode 5. Package identity,
update signing, game controls, P1/P2 assignment, purple menus and the VR4ALL splash
are preserved. Existing game compatibility limits still apply.

## P2 membership and X

P1 must already be active in the native game before a fresh X press can request
P2. Loading-time presses are consumed and require release/repress. The request
sends only P2 Start; the second gun and saved hand layout activate after the
existing per-game native reader confirms P2 membership. Rejected requests expire
after four seconds. Native P2 exit removes the gun. Pending requests clear on a
new game, configuration disable or netplay; temporary controller pose loss cannot
discard an acknowledgment. Survivor retains its single-player cabinet behavior.

The Quest Android gamepad's duplicate X delivery is consumed before legacy
mapping and keyboard dispatch in the standalone VR activity. OpenXR still owns
X as P2 Start. The observed Quest 3S device uses Oculus vendor 0x2833 with gamepad
capability; filtering is limited to that vendor, gamepad capability and X. Back,
volume, other buttons, other vendors, unknown devices and ordinary Android builds
keep their input route. Legacy pre-v3 mappings can translate btn_c/X to Service;
this guard prevents that duplicate delivery from reaching those mappings.

Automated coverage: 115 production hand/join/reset/gesture checks per game (805 total),
77 startup contract checks across seven, and all 455 Android unit tests. Existing
Ninja hand-safety (243) and MOK reload/render/profile (19) checks also pass.

## Deliberate Service access

On Quest Touch/Touch Pro/Touch Plus controllers and Index controllers with a
physical A button, a short A press emits a bounded Start pulse on release. It
keeps the player who owned that button when pressed. Holding A for three seconds
shows a purple VR4ALL progress notice and emits one 200 ms cabinet Test pulse,
with no ordinary Start on release. Release is required for another service hold.
The service gesture is offline only; online play retains synchronized Start.
Vive/WMR menu-button mappings retain their existing Start behavior.

A/X native Start is blocked until the existing guarded game reader reports that
the title's code/state is loaded. A button held during loading must be released
before a fresh Start can pass. Focus loss, action sync failure, settings,
preparation, new games, loaded states and termination cancel gesture state.
A separate atomic Test request is merged after game-specific cabinet ownership,
including Survivor; unrelated existing keyboard/gamepad cabinet controls remain
available. No camera data is collected by this feature.

The Quest Android duplicate-event guard now covers physical A and X. The user
confirmed corrected P2 joining on the connected Quest 3S. Physical verification
of the final short-A / hold-A Service gesture is recorded with the release run.
