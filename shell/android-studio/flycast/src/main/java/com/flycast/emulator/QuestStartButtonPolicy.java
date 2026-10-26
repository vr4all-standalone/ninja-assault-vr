package com.flycast.emulator;

/** Keeps Quest A/X with OpenXR instead of delivering it a second time to legacy mappings. */
final class QuestStartButtonPolicy {
    static boolean useOpenXr(boolean vrApp, int vendorId, int deviceSources, int keyCode) {
        // Android's Quest gamepad mirrors the tracked controller's A/X. Old mappings
        // can translate that second delivery to cabinet Service/Test. OpenXR owns it.
        final int oculusVendor = 0x2833;
        final int sourceGamepad = 0x401;
        final int buttonA = 96, buttonX = 99;
        return vrApp && vendorId == oculusVendor
                && (deviceSources & sourceGamepad) == sourceGamepad && (keyCode == buttonA || keyCode == buttonX);
    }
}
