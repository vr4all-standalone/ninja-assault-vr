# Ninja Assault VR development notes

## Scope and status

The initial standalone target is World NJA2 Ver.A, ninjaslt.zip. The base app version is 1.0.1;
Quest identity is com.vrlightgunhub.ninjaassault and the title is Ninja Assault VR. This fork
preserves the finished CM/Lupin infrastructure but remains experimental. A shared boot ID does
not authorize another regional revision's memory writes. See [Ninja findings](ninja-findings.md)
and the [validation record](ninja-validation.md) for measured guards and final native results.
Do not infer gameplay correctness from setup, build or packaging checks.

Keep each checkout's configuration, saves and generated caches separate. Original user ROMs,
CM/Lupin reference checkouts and their models/settings must remain untouched. Research uses
private copies, an owned process and isolated UDP port 27046. Captures, extracted assets and
ROM data do not belong in source or distribution archives.

## Cartridge layout and readiness

Quest uses Download/ninjaslt.zip and Download/naomi.zip. App-private games folders also work.
PC uses roms/ninjaslt.zip and roms/naomi.zip beside the portable player. A supported BIOS may
be embedded in the game archive. Keep canonical lowercase names and keep the ZIP packed.

NinjaFiles checks every required World cartridge entry using expected CRC and uncompressed
size from core/hw/naomi/naomi_roms.cpp: three 8 MiB entries and nine 16 MiB entries. It accepts
a renamed entry only when the native CRC lookup can identify the expected data. BIOS entries
need supported 2 MiB metadata. Conflicting known BIOS names are rejected across game and BIOS
archives because native filename fallback can otherwise shadow a valid later revision.

These are bounded ZIP central-directory checks, without extracting or hashing payloads.
Malformed or incomplete sets do not enable Play. Explicit requests cannot fall through to a
complete sibling archive. Regional clones are rejected until both dependencies and native
revision guards are validated. No disc image, PIC key or separate track folder is required.

## Controllers and menu

Single-player defaults remain A Start, B Reload, X P2 Start/join and Y Reload (P1). Either trigger
can shoot and transfer P1's single gun before P2 joins. Native free play uses Start directly;
there is no separate coin action.

The Two players setting applies only with two active players. vr.DualP1Left defaults true:
P1 left/P2 right, X/Y for P1 Start/Reload and A/B for P2 Start/Reload. The opposite choice swaps
dual assignments. Single-player Start buttons and trigger transfer do not follow this setting.
Verify held-trigger release gating and relaunch persistence with the final native build.

The thirteen menu rows are Resume, Recenter, Gun, Gun angle, Reload, Two players, Laser, Haptics,
World size, Panel distance, Wider view, How to play, Quit. The shared engine supports either
pointer, stick repeat, a gun preview and three-second Quit confirmation. Simulation/audio
pause while head tracking remains live. How to play uses Touch, Index, Vive and WMR labels.
Ninja-specific state, cutscene visibility, HUD and accepted-round feedback need native evidence.

## User models

Guns use guns/<name>.glb plus a matching JSON sidecar. Controllers use controllers/left.glb and
controllers/right.glb with matching sidecars. Quest reads folders beside the game ZIP, such as
Download/guns/. PC searches beside flycast.exe, then beside the game ZIP, then data/models/.
User files override matching bundled names; each controller side can fall back independently.
Only add files you have rights to use.

An example gun sidecar supplies a display name and grip, muzzle, up vector and length in metres:

```json
{ "name": "My gun", "grip": [0, 0, 0], "muzzle": [0, 0.03, -0.18],
  "up": [0, 1, 0], "length": 0.22 }
```

This describes an arbitrary user model, not a measured Ninja weapon. The inherited conversion
helper is hotd2-vr/tools/make-gun.py. Arcade, the licensed Zapper and user guns remain available.
A complete eligible in-game Ninja weapon has not yet been located; no successful Ninja gun
extraction or completed cache is claimed. Another game's extracted model must not stand in for
it. Keep preparation/fallback nonblocking and retry incomplete work when eligible data exists.

The existing nine-file licensed bundle is copied unchanged into untracked hotd2-vr/models.
It contains two gun GLBs, two controller GLBs, their JSON sidecars and CREDITS.txt. Preserve all
model author, source, licence and transformation notices. Never put game-generated caches there.

## Builds and focused checks

Windows uses hotd2-vr/build-win.cmd Release. Quest uses hotd2-vr/build-quest.cmd and the optimized
arm64 vr Gradle variant. Standalone native configuration needs VR_STANDALONE_NINJA=ON and
VR_STANDALONE_LUPIN=OFF. The Android helper respects explicit JAVA_HOME, ANDROID_HOME and
GRADLE_USER_HOME, with local toolchain fallbacks. Android needs NDK 29.0.14206865, CMake 3.22.1
and its Gradle-managed/requested JDK 21 daemon toolchain as well as a launcher JDK.

The initial Quest build appears at shell/android-studio/flycast/build/outputs/apk/vr/flycast-vr.apk.
Without the existing external release-signing configuration, Gradle warns and uses a public
debug key. Do not treat that as a release. Do not create, print, copy or commit private signing
material for this work.

```powershell
# Synthetic checks only: no native builds, real game data, signing keys or emulator launches.
powershell -NoProfile -ExecutionPolicy Bypass -File hotd2-vr/package-release-tests.ps1

# Plain Java cartridge/setup/status suite.
cd shell/android-studio
.\gradlew.bat :flycast:testDebugUnitTest
```

The Android suite is NinjaFilesTest. VR Java/resources/merged identity have also compiled.
A read-only directory probe of the owned World cartridge and BIOS returned READY; this is
metadata evidence only. Use the root validation record for native checks and final artifacts.
Physical Quest relaunch/Quit, tracking, comfort and sustained performance are still unverified.

## Release gates

The target outputs are dist/ninja-assault-vr-quest.apk and dist/ninja-assault-vr-pcvr.zip. The PC
launcher is Play Ninja Assault.cmd. Its portable emu.cfg enables pvr.rend=0,
rend.NativeDepthInterpolation=yes and vr.Xr=yes.

package-release.ps1 retains clean-source, HEAD resolution, binary commit attestations, release
signing, APK package/label/version/sole setup launcher, controller-model permission, exact PC
archive, matching bundled model lists, GPL-3.0/cgltf notices and per-model credit checks. Raw
cartridge entries, BIOS archives, game-generated caches, rips, captures and signing files are
excluded. Cartridge-specific names are added to the inherited game-data bans.

-CheckOnly performs archive content/licence/model-credit checks; it does not check APK identity,
provenance or the signer. -SkipBuild uses existing artifacts but preserves release provenance
requirements. -AllowDebugKey produces test packaging and relaxes signer/provenance gates; it
never certifies a release. A binary built before a new commit needs rebuilding to embed the
current source hash. Distribution needs that corresponding source commit.

Publication, headset installation, new keys and automations are outside this port task. Mark
physical checks only when actually performed in the [headset checklist](ninja-vr-headset-checklist.md).

## Native Mach Gun (1.0.1)

Source::NinjaGun uses the shared neutral attract-gun preparation policy and its
own gun-na-mach.bin cache. It watches for the complete64-strip texture0x4af000
gun, separates the two spatial instances at10gameunits and scales it to0.40m.
Preparation is bounded, Bskips to Arcade for this session, and strict incomplete/
corrupt-cache checks permit retry. The actual cache is generated from each user's
own game. Never commit or package it. The labelled menu choice is In-game Mach Gun.
