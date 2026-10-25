# Ninja Assault VR Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use executing-plans for native work; dispatching-parallel-agents permits independent Android work and read-only design review.

**Goal:** Separate Quest and PC Ninja Assault port with the finished CM/Lupin features.
**Architecture:** Retain Flycast and its shared VR infrastructure; add measured Ninja profile, own standalone policy and cartridge setup screen.
**Tech Stack:** C++/OpenXR/OpenGL, Java/Android Gradle, Python, PowerShell/MSVC/CMake.
**Spec:** docs/superpowers/specs/2026-10-25-ninja-assault-vr-design.md

## Global Constraints
- Preserve CM and Lupin reference checkouts and original ROMs.
- Separate identity com.vrlightgunhub.ninjaassault; base 1.0.0.
- No game files or extracted game assets in source/distributions.
- Camera/calibration/state hooks require live evidence and revision guards.
- No publishing or headset installation; label physical tests outstanding.

## Review Focus
- Missing/broken/split cartridge sets must not enable Play.
- Shared Ninja boot ID across regional revisions must not authorize incompatible RAM writes.
- Empty trigger pulls, reloads and player transitions must not invent gunshot haptics.
- Wider drawing projection must preserve the stock hit-test frame.
- Incomplete model caches must fall back and retry without blocking launch.

### Task 1: Independent app and cartridge setup
Files: core/standalone_game.h, CMakeLists.txt, shell/android-studio/flycast/build.gradle, shell/android-studio/flycast/src/vr, setup JVM tests.
Interface: standaloneGameError(const char*) permits exact Ninja boot ID; Java NinjaFiles/NinjaStartStatus supply validated paths and readiness.
- [ ] Add failing native identity and Java cartridge/BIOS/clone tests; verify expected failures.
- [ ] Implement separate Ninja identity, structural cartridge validation and inherited setup UI/lifecycle.
- [ ] Run JVM and standalone tests; build Windows unrestricted research binary.

### Task 2: Measure and adapt native VR behavior
Files: core/rend/vr_reproject.cpp, core/rend/vr/* as necessary, hotd2-vr/ninja-*.ps1 and hotd2-vr/notes/ninja-findings.md.
Interface: shared GameCamera/GunState/profile and model contracts remain unchanged unless evidence requires a tested extension.
- [ ] Capture private World attract/gameplay RAM and render passes using isolated harness, unique player process and UDP port 27046.
- [ ] Identify projection, stock lens, HUD, near plane, gun transport/calibration, player/round/lifecycle fields and eligible weapon geometry.
- [ ] Write regression tests before each behavioral extension and confirm failure.
- [ ] Implement guarded Ninja-specific hooks and test real aiming/reload/empty/player transitions in simulator.
- [ ] Run inherited menu/controller/profile and model-cache checks with Ninja scenarios.

### Task 3: Build and validate delivery
Files: hotd2-vr/build-win.cmd, build-quest.cmd, package-release.ps1, packaging checks, README and validation notes.
Interface: ninja-assault-vr-quest.apk and ninja-assault-vr-pcvr.zip; launcher Play Ninja Assault.cmd.
- [ ] Adapt packaging and identity checks, retain model credits/licences, reject game/cache data.
- [ ] Build both targets and verify identity, source version, signing, package contents and test results.
- [ ] Prepare separate local PC play folder with user files; preserve reference settings/saves.
- [ ] Fresh branch review and corrections, document outstanding physical headset checks, deliver links.
