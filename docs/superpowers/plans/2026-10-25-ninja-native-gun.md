# Ninja Assault native Mach Gun implementation plan

User requests the automatic in-game gun extraction used by the preceding ports.
The source is the user's own World NJA2 Ver.A game; extracted models stay private.

1. Confirm actual firearm geometry from the hero story, including separate complete
   instances, materials, orientation and scale. Existing C++ builder must produce a
   complete strict-valid HND1 cache before enabling a production target.
2. Add renderer-cache regressions first: Ninja must use an isolated cache and reject
   incomplete, corrupt, wrong-count and truncated files before the renderer parser.
3. Give Ninja its own Source kind and target. Reuse the neutral attract preparation,
   strict atomic cache/retry, B skip and fallback policy; preserve other games.
   Label the native choice Mach Gun. Preserve both-player hand and reload behavior.
4. Verify first-run extraction, cache reuse, corrupt-cache retry, skip/fallback, native
   menu preview and both-player gameplay with the actual built executable. Run the
   existing focused checks and shared synthetic model/cache regressions. Review changes.
5. Bump to1.0.1/versionCode2; rebuild signed Quest and PC artifacts from clean source,
   verify strict packaging and public release hashes, update page/docs and VR4ALL.

Measured target: texture0x4af000, TCW0x48095e00,64 strips/203 triangles per gun,
two independent instances separated at join distance10 game units. Private builder
probe produced284236 bytes, complete/strict-valid, at provisional0.4m gun length.
No game data or generated model is committed or distributed.
