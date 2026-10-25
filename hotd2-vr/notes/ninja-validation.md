# Ninja Assault VR validation

Release 1.0.0 targets World NJA2 Ver.A. Binary source commit:
`c7a94cd021a83ba06811ae2bb69fc4ea32419377`. Public-page updates after that commit
change documentation and promotional assets only. Physical headset validation
and later-stage coverage remain pending.

## Automated checks

- 401 focused checks pass: identity 8, Ninja input 15, inherited input 23,
  calibration 32, native state 23, panel units 14, dual-hand routing 243 and
  synthetic packaging 43.
- 31 Android setup JVM checks pass, covering complete/missing/malformed sets,
  cartridge CRC/size metadata, BIOS shadowing/embedding, explicit paths and status.
- VR Java, resources and the merged standalone Android manifest compile.
- All 53 original-word guards match the private original World RAM sample.

## Native game and simulator evidence

Private baseline, free-play, combat and wide-grid runs measured the game's own
camera, calibration, accepted rounds, eight-round magazines and reload behavior.
Seven causal aiming samples across both players matched native 2D coordinates
within one pixel. Wider rendering changes draw focal from 718.744 to 288.137
while preserving the original collision lens.

`ninja_native_ray_check.py` compares stock combat RAM with widened-grid RAM:
the input scalar remains bit-identical at 1.3891451358795166 and collision focal
at 718.7440795898438. World-ray errors are 2.87445e-8 and 6.99672e-9, with zero
origin error. Drawing and hit-test lens behavior were measured independently
from Confidential Mission and Lupin.

The 73-second `ninja-xr-gameplay` run produced exactly nine accepted-round kicks
and two empty clicks per player. Both-hand single-player transfer, default
P1-left/P2-right joining, B/Y reloads, story/combat transitions and menu opening
and closing passed. Held joins, live dual-hand reversal and return to single
play have production routing/menu/label regression coverage.

Review found and corrected the profile-unit HUD distance bug. Panel placement,
flat rendering, layer comparisons and gun-ray intersection use consistent units.
The default panel distance is 4.4 m; the setting range is 1.65–13.2 m. Fourteen
panel checks cover finite positive depths and monotonic snapping. A final
63-second run loaded the actual licensed Arcade gun and controller bundle,
displayed the HUD/menu and registered a forward flat-panel hit at 395 cm from
the extended gun. Five screenshots were visually inspected.

## Model investigation

A neutral 175-second attract run yielded 12 rips at 60–170 seconds and 12
screenshots at 61–171 seconds. Inspected coverage includes story/protagonists,
a sword confrontation, title and rule-instruction demo. The sampled protagonist
weapons resolve to blades/scabbards, including a visible katana.

No complete eligible native firearm model was identified in those samples.
This does not establish that none exists elsewhere in the game. Native firearm
extraction is unavailable; licensed Arcade/Zapper and custom GLBs are supported.

## Release artifacts

Clean-source Windows and Quest builds completed. Strict packaging verified:

- Android package `com.vrlightgunhub.ninjaassault`, versionCode 1 and versionName
  `1.0.0-gc7a94cd02`.
- Release APK signature with public signer name `CN=VR Light Gun Hub`.
- PC executable and APK native library both attest the binary source commit.
- Both packages include the same nine licensed model/credit files and software
  licence notices, with no game data, extracted game models or signing material.
- SHA-256 checksums are supplied with the release.

Physical headset installation, tracking, comfort, sustained performance and full
later-stage behavior have not been validated. See the
[headset checklist](ninja-vr-headset-checklist.md) and
[measured findings](ninja-findings.md).
