package com.flycast.emulator;

import org.junit.Test;
import static org.junit.Assert.*;

public class QuestStartButtonPolicyTest {
    @Test public void questStartButtonsUseOnlyOpenXrThroughoutBootAndPlay() {
        // Quest 3S observed vendor/source; events may themselves report keyboard source.
        assertTrue(QuestStartButtonPolicy.useOpenXr(true, 0x2833, 0x01000511, 99));
        assertTrue(QuestStartButtonPolicy.useOpenXr(true, 0x2833, 0x401, 99));
        assertTrue(QuestStartButtonPolicy.useOpenXr(true, 0x2833, 0x401, 96));
        // Routing is deliberately independent of session focus and key action, so
        // boot presses, repeats and releases all have the same OpenXR owner.
    }
    @Test public void unrelatedKeysAndDevicesKeepTheirMappings() {
        assertFalse(QuestStartButtonPolicy.useOpenXr(false, 0x2833, 0x401, 99));
        assertFalse(QuestStartButtonPolicy.useOpenXr(true, 0, 0, 99));
        assertFalse(QuestStartButtonPolicy.useOpenXr(true, 0x045e, 0x401, 99));
        assertFalse(QuestStartButtonPolicy.useOpenXr(true, 0x2833, 0x101, 99));
        for (int key : new int[]{4, 24, 25, 52, 97, 100, 108})
            assertFalse("key " + key, QuestStartButtonPolicy.useOpenXr(true, 0x2833, 0x401, key));
    }
}
