# Ninja Assault VR

An experimental standalone Meta Quest and PC OpenXR port of **Ninja Assault**, built from
Flycast and the completed Confidential Mission and Lupin VR work. The initial game target is
**World NJA2 Ver.A**, MAME/Flycast set **ninjaslt**.

**Implementation in progress.** The shared VR menus, controllers and model infrastructure are
inherited. Ninja-specific camera, aiming, native shot feedback, reloads, player transitions and
later-stage behavior must be assessed against the current validation evidence. Full feature
parity is not complete. Physical Quest and PC VR headset testing remains outstanding.

This project includes no game, BIOS or extracted game model. Use your own files. It is an
unofficial fan project, not affiliated with Namco, Sega, Meta or the Flycast project. The CM and
Lupin reference projects and their settings are independent of this app.

## Game files

Use the complete **World NJA2 Ver.A cartridge set** and a supported NAOMI BIOS. Keep the ZIP
packed and retain the canonical lowercase filenames. Regional clones are not supported in the
initial standalone setup; renaming one to ninjaslt.zip does not make it the World revision.

| File | Quest location | PC portable location |
|---|---|---|
| ninjaslt.zip | Download/ninjaslt.zip | roms/ninjaslt.zip |
| naomi.zip | Download/naomi.zip | roms/naomi.zip |

A supported BIOS may instead be inside the game ZIP. All 12 required cartridge entries need
matching uncompressed sizes and CRC metadata. The Quest setup checks ZIP central-directory
metadata, rejects incomplete or malformed sets and conflicting known BIOS entries, and keeps
Play disabled until ready. This does not hash every payload byte or certify full integrity.
Keep your originals untouched and prepare a separate copy for this app.

## Quest

Target artifact: **ninja-assault-vr-quest.apk**. App: **Ninja Assault VR**,
package **com.vrlightgunhub.ninjaassault**, base version **1.0.0**.

Quest 3 is the primary target; Quest 3S, Quest 2 and Quest Pro remain compatibility targets
pending physical checks. Use a local APK produced by this source. No public release or
installer listing is implied by this development tree.

For manual setup, enable developer mode and install the APK with your preferred sideloading
tool. Copy your own files to the Download layout above. Open Ninja Assault VR under Unknown
sources, choose **Allow** if All files access is needed, then **Look again** and **Play** when
READY. Files in the app's own games folder do not need shared-storage access. Quit in VR returns
to the setup panel; Play starts a fresh game process.

Installation and physical lifecycle checks must be performed separately; building an APK does
not establish them. Settings and saves live under this app's own Android package.

## PC VR

Target artifact: **ninja-assault-vr-pcvr.zip**. Unpack it into a separate folder, add your own files,
and launch **Play Ninja Assault.cmd**:

```text
flycast.exe
emu.cfg
Play Ninja Assault.cmd
roms/
    ninjaslt.zip
    naomi.zip
```

The launcher uses this folder as its working directory and selects the World set. The portable
configuration enables OpenGL, native depth interpolation and OpenXR. Connect the headset to
the active OpenXR runtime before launch. Licensed bundled models, their CREDITS.txt and the
software licence accompany packaged builds; game data and generated caches do not.

## Controls

Single-player keeps the normal Start buttons and trigger-to-transfer behavior:

| Quest Touch input | Single-player action |
|---|---|
| A / B | P1 Start / Reload |
| X / Y | P2 Start or join / Reload |
| Either trigger | Shoot; transfer P1's single gun to that hand |
| Left Menu | Open or close paused settings |
| Left stick click | Recenter |
| Right stick click | Toggle laser |
| Grip + stick up/down | World size |
| Grip + stick left/right | Gun size |
| Stick | Game D-pad |

Native free play starts through Start; there is no separate coin action.

**Dual hands** applies only after two players are active. Its default is **P1 left / P2 right**:
X/Y then control P1 Start/Reload and A/B control P2 Start/Reload. The opposite option assigns
P1 right/P2 left. This choice does not change single-player Start buttons or trigger transfer.
Touch, Index, Vive and WMR use their controller-specific labels and bindings.

## Menu and inherited features

The thirteen paused rows are **Resume, Recenter, Gun, Gun angle, Reload, Dual hands, Laser,
Haptics, World size, Panel distance, Wider view, How to play, Quit**.

The shared menu supports either pointer, stick navigation and repeat, saved settings, a gun
preview and confirmed Quit. Simulation and audio pause while head tracking stays live. Gun
angle ranges from -30 to +30 degrees. Reload offers Shoot off-screen and Point off-screen;
haptics offers Off, Light, Medium and Strong. Ninja-specific behavior and round feedback need
the corresponding measured native validation, including empty pulls, reloads and both players.

Arcade, Zapper and user GLB guns remain available. **A complete eligible in-game Ninja weapon
has not yet been located.** Do not treat another game's extracted gun as native to Ninja Assault.
The model preparation and fallback infrastructure is inherited; completion of Ninja extraction
is not claimed. Incomplete preparation must leave a usable fallback and permit later retry.

Wider drawing and the stock hit-test lens must remain separate. Empty hands during story
scenes, HUD placement, stable reload edges and P2 join/leave are validation targets; check the
recorded Ninja results before treating them as verified.

## User models

Quest user folders sit beside the game ZIP, for example Download/guns/ and Download/controllers/.
On PC, guns/ and controllers/ beside flycast.exe take priority, followed by folders beside the
game ZIP, then licensed bundled data/models/. A same-named user file overrides the bundled
model; controller sides fall back independently. Gun GLBs need a matching JSON sidecar.
See [model format and build notes](hotd2-vr/notes/ninja-development.md#user-models).

## Evidence and remaining checks

The Android setup has 31 passing JVM checks covering ready/missing/malformed/incomplete sets,
CRC/size metadata, BIOS shadowing, embedded BIOS, explicit paths and status behavior. VR Java,
resources and the merged Android identity have compiled successfully. These checks do not
prove native gameplay, headset tracking, comfort, sustained frame rate or later-stage coverage.

Use the final build's recorded source commit and the root validation results:

- [Measured Ninja findings](hotd2-vr/notes/ninja-findings.md)
- [Validation record](hotd2-vr/notes/ninja-validation.md)
- [Physical headset checklist](hotd2-vr/notes/ninja-vr-headset-checklist.md)
- [Build and development notes](hotd2-vr/notes/ninja-development.md)

## Credits and licence

[Flycast](https://github.com/flyinghead/flycast) by flyinghead and contributors supplies the
emulator. [mikermak's VR fork](https://github.com/mikermak/hotd2-vr) supplies the original VR
rendering and tools. Confidential Mission VR and Lupin VR provide the standalone menu,
controller, gun and setup infrastructure adapted for this port.

The release CREDITS.txt records each licensed model's author, source, licence and changes.
Model files and notices are copied unchanged from the existing licensed bundle. Software
builds are distributed under **GNU GPL version 3**; see [LICENSE-RELEASE.txt](LICENSE-RELEASE.txt).
The packaged licence also preserves cgltf's MIT notice. Upstream source notices and history
retain their terms. Any distribution needs the corresponding source commit. Game data,
generated game models and private release-signing material are excluded.
