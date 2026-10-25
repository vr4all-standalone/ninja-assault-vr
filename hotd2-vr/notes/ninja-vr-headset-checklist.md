# Ninja Assault VR physical headset checklist

All items are unverified on a physical headset. Build, simulator, metadata and packaging
checks do not establish tracking, comfort, controller feel or sustained performance. Record
the final packaged source commit, version, headset, runtime and controller model before testing.
App ID: com.vrlightgunhub.ninjaassault. Keep game data and private captures/logs out of source.

## Setup and lifecycle

- [ ] Ninja has its own library entry, settings and saves beside CM and Lupin without changing either.
- [ ] Complete World ninjaslt.zip and supported naomi.zip produce READY and two file marks. Missing, malformed, incomplete and wrong-region sets disable Play with useful instructions.
- [ ] All files access recovers after Allow; app-private files work without it. Look again notices restored files.
- [ ] NA icon, text, expanded instructions and controls fit the panel and stay readable.
- [ ] Play enters VR; confirmed Quit returns to setup; Play starts a fresh game process.
- [ ] Home, focus loss, sleep and repeated library launches leave no duplicate process or stuck input.

## Start, single player and dual hands

- [ ] A starts single-player through native free play without a coin action; X starts/joins P2.
- [ ] Before P2 joins, either trigger fires/transfers P1's single gun. Dual hands does not change single-player Start/Reload buttons or transfer behavior.
- [ ] Dual hands defaults to P1 left/P2 right. With two players active, X/Y control P1 Start/Reload and A/B control P2 Start/Reload.
- [ ] Selecting the opposite dual assignment updates both guns, labels and Start/Reload buttons only for dual mode; the setting survives relaunch.
- [ ] Swapping dual assignment preserves players/ammunition; held triggers wait for release. Returning to single-player restores trigger transfer.
- [ ] Touch, Index, Vive and WMR labels meet the controls for each actually tested profile; Trigger appears above Stick where present.

## Paused menu

- [ ] Thirteen rows appear in order: Resume, Recenter, Gun, Gun angle, Reload, Dual hands, Laser, Haptics, World size, Panel distance, Wider view, How to play, Quit.
- [ ] Either pointer/trigger and stick repeat work. Opening settings pauses simulation and audio while head movement stays live.
- [ ] Resume, B, rapid toggles, focus transitions and recenter do not fire, reload or leave a stuck input.
- [ ] Menu pointers remain visible with Laser off. How to play shows the matching controller labels and restores the prior view on closing.
- [ ] Quit requires a second selection within three seconds and expired confirmation must be armed again.

## Guns and models

- [ ] Arcade, Zapper and valid user GLBs cycle and preview; invalid custom models fall back without blocking launch.
- [ ] Grip, muzzle, laser and visual feedback align in both hands. Gun angle stops at -30/+30 degrees and the chosen angle saves.
- [ ] User guns/controllers override matching bundled files; a missing controller side falls back independently.
- [ ] In-game Ninja weapon availability is reported honestly. Until a complete eligible Ninja weapon is measured, a fallback is used and another game's model is never presented as native.
- [ ] If a future eligible Ninja model is prepared, verify completeness, skip/retry, interrupted/truncated cache recovery and good-cache preservation separately; record actual preparation time.

## Aiming, image and comfort

- [ ] Center, edges and corners hit the intended target for both players through changing head poses and game zoom.
- [ ] Wider view changes drawing without changing the stock hit-test frame. Grey margins produce neither a hit nor a reload.
- [ ] Head motion gives stable stereo; floors/walls and near geometry do not streak, float or clip unexpectedly.
- [ ] HUD, ammunition, subtitles, instructions, scores/ranking and continue notices remain readable without missing glyphs.
- [ ] Native shot markers and VR impacts do not duplicate; fades, damage effects and story transitions remain correct.
- [ ] World size, gun size, panel distance and recenter are comfortable. Record settings and known geometry limits.

## Rounds, reload and player state

- [ ] Use the ammunition counts measured in Ninja findings, not another game's counts. Accepted rounds give one recoil/flash/haptic event in the correct hand.
- [ ] Empty pulls, reloads, menus and player transitions do not invent shot feedback. Haptics Off/Light/Medium/Strong match their labels.
- [ ] Shoot off-screen reloads only on a qualifying pull; pointing away alone does not reload.
- [ ] Point off-screen reloads once per armed exit after return on-screen; the grey ring and focus/menu transitions do not reload.
- [ ] Holding an on-screen pull while sweeping away does not add a shot/reload; holding an off-screen reload while sweeping back does not fire.
- [ ] P2 joins/leaves the actual game, keeps independent gun/ammunition/lives/score and returns to single-player behavior on expiry.
- [ ] Noninteractive scenes leave empty hands; guns return promptly for play without combat flicker. Skipping movies and later stages remain stable.

## Persistence, performance and PC

- [ ] Relaunch preserves all saved options in Ninja's own config; reference-port settings/caches stay untouched.
- [ ] Play at least 30 minutes with busy scenes, both players and head motion; record refresh rate, frame rate, thermals and stalls.
- [ ] Play Ninja Assault.cmd uses its own World ZIP/config and the active OpenXR runtime, then quits cleanly.
- [ ] Complete later stages and repeat on every headset/controller combination before claiming it verified.

For failures record build commit/version, scene, player/hand, reproduction steps and private
log/capture paths. Leave every check not actually performed unchecked. See
[native findings](ninja-findings.md) and the [validation record](ninja-validation.md).
