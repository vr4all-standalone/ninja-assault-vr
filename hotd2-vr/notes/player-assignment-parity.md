# P1/P2 hand assignment parity

All seven ports were audited against Confidential Mission's player assignment, separately from each game's other controls.

- Solo A/right starts P1. Solo X/left joins P2 when local dual play is available, both controllers are tracked and the game profile supports joining.
- Joining immediately applies the saved **Two players** layout: P1 left / P2 right by default, or the selected reverse layout. Solo gun transfer still follows either trigger.
- A held physical Start keeps its original player until release through joining, leaving and live layout changes. Simultaneous A/X presses remain separate players.
- Releases and fresh presses inside settings update Start ownership while game input stays paused. A genuinely held Start keeps its player; a released and re-pressed Start takes the new layout.
- The menu shows the saved dual layout while solo without changing the solo Start roles.

HOTD2 Arcade and Death Crimson OX needed solo/dual separation and held-Start ownership. Ninja Assault needed complete held-Start ownership instead of its join-only latch. Confidential Mission needed release sampling inside settings. Lupin and The Maze of the Kings already matched. Survivor 2's shared hand policy already matched; its native single-player controls remain game-specific.

Existing saved layout settings, gun visibility rules, native controls, reload gestures, trigger-release safeguards and purple UI/splash remain intact.

## Verification

The checked-in `player-assignment-test.ps1` compiles the production policy, Start ownership helper, join/leave block, menu cases and actual menu early return. It checks both layouts, P2-first/simultaneous/held Starts, exit/rejoin, blocked join conditions, live swapping, solo transfer, and released versus held Starts inside settings (58 checks per port). The cross-repository audit runs those checks against all seven implementations. Baseline failures were observed before the fixes.

Windows C++ tools are required for the regression script. Package validation additionally checks APK identity, version, signing continuity and source commit. Simulator checks do not constitute physical headset testing.
