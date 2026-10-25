# VR4ALL UI update — 1.0.2

Options, gun/staff preparation, completion panels and menu pointers use the same purple VR4ALL theme as the launch screen.

After Play starts immersive VR, the embedded VR4ALL headset and wordmark image is shown for three seconds of successfully presented, focused headset frames. Quest and PCVR use the same image and timer. Background time does not consume the splash. Resume from the options menu does not replay it; loading a new game does.

The splash temporarily suppresses VR game/menu input. Preparation acknowledgement resumes once the preparation panel is visible. Existing game-specific settings, models, native behavior and saved preferences are retained.

Verification includes a native timing regression, Windows and Quest builds, rendered simulator captures, and release-package checks. Physical headset validation is a separate check.
