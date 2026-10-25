# Ninja Assault validation

Experimental local port of World NJA2 Ver.A; physical headset validation has not been performed.

Measured evidence is in ninja-findings.md. Completed development checks at this point:
- Android setup:31 JVM tests; VR Java, resources and merged manifest pass. Owned World cartridge/BIOS structural metadata reportsREADY.
- Loaded-game identity8, inputadapter15 plus inherited23, calibrationinverse32, accepted-roundstate23 checks pass.
- Dual-only hand assignment enabledbydefault; production routing/menu/persistence/controller-label tests243 checks pass (single A/X stays, eithertriggertransfer, immediate dualroles, heldjoin, livefliprelease/latches).
- Packaging synthetic checks43 pass; copied licensed models/credits are identical to reference, no extracted game models supplied.
- Original-word guards:53 matched the private originalWorld RAM sample.
- Private native runs: baseline65s, freeplay125s, early71s, combat76s, widegrid70s. Widened-grid7causal samples acrossbothplayers matched native2Dgamecoordinates within1pixel. Originalcamera-recordfocal718.744 remained whiledrawprojection288.137. Native3Dhit-testunprojection is a separate check.
- NativefreeplayStart, independent8roundmagazines, acceptedprojectilerings, manualoffscreenreload, P2join and story/combat transitions observed.

Remaining before delivery: final XRsim runtime/menu/empty/reload/hand checks, complete neutral attract/model investigation, branch review, final clean-source Quest/PC builds and package/signature/source-version/content verification. Native weapon extraction remains unavailable unless an actual eligible firearm is identified; Arcade/Zapper/customguns are independent supported options. Full laterstage, stereo comfort and sustained physical headset behavior remain unverified.

## Final source verification before artifact builds

All focused native, input, state, panel, dual-hand and synthetic distribution checks pass;
Android NinjaFiles/NinjaStartStatus suites pass31tests. Runtime ninja-xr-gameplay73s
produced exactly9accepted-roundkicks and2emptyclicks perplayer; singleleft/righttransfer,
defaultP1left/P2rightjoin, B/Yreload, storyon/off and menuopen/close allmatched.

Fresh review found and fixed the profile-unit HUD distance bug. Paneldistance nowconverts
referenceunitsconsistently in3Drendering, framebufferflatdisplay, 2Dlayercomparison and
ray/planeintersection. Native default40now4000Ninjaunits=4.4m, limits15..120=1.65..13.2m;
forwardmuzzlehitsarepositive. Newpaneltests wereRED10failures thenGREEN14checks. Small
HUDsnapping alsostayspositiveandmonotonicacrossitswholeprofileband. Runtime
ninja-panel-runtime63s loads actualArcade/controllerbundles, showsHUD/menu, and logs a
flat-map hit at395cm from theextendedgun. Fiveprivate screenshots visuallychecked.

Nativehit-testlensverification nowcausal: ninja_native_ray_check.py stockcombatRAM0002
versuswidegridRAM0005 provesbit-identicalinputscalar1.3891451358795166 andcollisionfocal
718.7440795898438 whiledrawing718.744049->288.136711. Worldrayreferenceerrors are
2.87445e-8/6.99672e-9, zerooriginerror. Sharedcameraandinputassumptionsweremeasured
independentlyofCM/Lupin.

Final signed artifact hashes/content checks are written to dist/validation-results.json
when packaging finishes. No public release, headset installation or VR4ALL listing is part
of this local delivery. Nativefirearm extraction remains unavailable; allphysicalheadset
andlater-stage items remain on ninja-vr-headset-checklist.md.
