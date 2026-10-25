# Ninja Assault measured findings (World NJA2 Ver.A)

User request: same VR feature set as finished CM/Lupin; latest steering makes dual P1-left/P2-right an enabled-by-default option only. Single-player either-trigger transfer stays.

Reference source: Lupin f67026046; isolated branch codex/ninja-assault-vr. Reference checkouts and original ROMs unchanged. Private test runs use ninja-vr-player.exe, UDP27046 and separate run/config/save roots.

## Files and input
World ninjaslt.zip cartridge, all12required files and CRC/size metadata from Flycast naomi_roms.cpp, plus own NAOMI BIOS; no CHD/PIC. Android structural/readiness tests cover incomplete/malformed archives and wrong revisions. Runtime boot ID NINJA ASSAULT alone does not prove regional code compatibility; RAM writes/reads have original-word guards.

Ninja uses Namco JYU command0x25 direct screen-coordinate u16 samples rather than Lupin analog0x22. Native calibration is signed (raw*scale+offset)/100. Per-player live table22C548 stride16:scaleX179,scaleY101,offsetX-9984,offsetY-6656. Native code1300B2..1300D2 and table literal130164 are guarded. Root ninja-combat t48raw192,144 ->243,78; t49raw320,240 ->472,175; raw480,288 falls outside640 and reloads. Separate inverse helper32checks covers every screen pixel for both players, nonzero u16 sentinel, inverted/invalid/overflowing coefficients; maximum measured arithmetic roundtrip error1pixel. Integration runtime checks follow.

Native free play u32RAM3B62AC: operator descriptor23CA18 and credit-handler code15C8C0..15C8D8/literal15C964. Private poke25s thenStart35s reachesStage1, screen saysFREEPLAY. Mod writes only the guarded low16bits of the known0/1flag; originals unchanged. B/Y uses bounded off-screen JYU reload; point-only offscreen leaves native neutral input until chosen reload pulse.

## Camera and HUD
Projection0E0600 (duplicate0E0850), viewport0E0728. Main original perspective builder08F8A0 receives full angle inr4, integer half at08F8C8. Current camera pointer3860F4 indexes9records at385818,stride252; angle+236. Guarded accessor10685A..106868 and pointer/base literals identify layout. Angle8738 yields stock718.744131pixel focal, captured matrix718.744049. Wider draws with guarded08F8C8 4D21->0009 (fullangle), stock hit-test lens remains independent camera record. Widening causal checks pending.

HUD layers Wmultiples1/65536, text1/1024, stage-map layers0.001007..0.001343, fullscreenUI0.002197. Near-world starts~1.0; never use it as HUD. SnapUIband0.0009..0.01 and keep far-out world geometry. Native camera height1300units in earlygameplay; builder world-unit factor0.01 relative to inherited0.11metres yields1.43m initialheight; World size remains adjustable. Physical scale/comfort need headset testing.

## Native state
P1struct3869BC, P2+0x30. Slot+0,lifecycle+1,active+2,healthu16+6,ammou16+8. Native magazineu16global386A30=8. Health max386A2E=256. Lifecycle1active,2continue,3pendingjoin (early story). Combat flagu16 3E6978==0story/1combat; do not infer this from globalmode480D00==5 (same both). Pure helper and runtime integration pending.

Accepted shot native code0FAB94/96/98 decrementsammo. Projectile ring481338/3C startsFFFFFFFFthen0,1,2,3,0 on accepted rounds; reload replenishesammo without ringadvance. ninja-combat confirms P1t48ammo8->7ring-1->0,t49ammo7->6ring0->1,t50outside reload6->8ring1unchanged,t51accepted8->7ring1->2,t53reload7->8unchanged; P2join55active1ammo8,shot57ammo7ring0. Empty and VRfeedback checks pending.

## Native weapon models
Current captures show sword enemies and castle story, no complete firearm. Texture4AA800 is katana grip/guard, not a pistol. Do not claim Lupin/CM caches as Ninja assets. Shared gun menu supports licensed Arcade/Zapper and custom GLBs; native firearm preparation is unavailable until eligible geometry is identified. Broader attract investigation pending.

## Evidence and limits
Baseline65s, freeplay125s, early71s, combat76s runs passed their existing profile/boot checks. Native identity8checks, manual input5checks and affine inverse32checks pass. Android31JVMchecks/VRJava/resources/manifest pass at2d3e672a7. These do not establish physical headset validation or later-stage coverage. Final validation report must name actual final builds and runtime tests.

Final additions: actualnativeinputscalar3B5880 andcollisionfocal3CEBDC staybitidentical
underwidening. Nativeworldraymatchesstockcamerarecord to<3e-8. P1/P2runtimefeedback each
9real-roundkicks/2emptyclicks. HUDpaneldepth uses referenceunits / profileunitScale inall
render/aim paths; tinyHUDlayercompression boundedpositive. Dual-only defaultP1left/P2right
passes243productionchecks; singletriggertransfer andstartup remainfixed. Nativeweapon
model notidentified incurrentcaptures andnot falselyoffered bythegunmenu.
