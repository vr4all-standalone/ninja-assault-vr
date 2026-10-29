package com.flycast.emulator;

/** Once-only automatic entry for a validated setup screen. All calls run on the UI thread. */
final class ReadyLaunchPolicy {
    private boolean resumed;
    private boolean consumed;
    private boolean launching;

    ReadyLaunchPolicy(boolean suppressed, boolean restoredConsumed) {
        consumed = suppressed || restoredConsumed;
    }

    /** Android may reuse the setup task for a new entry instead of calling onCreate(). */
    void onNewEntry(boolean suppressed) {
        // Once handoff begins this instance is finishing; another intent must not launch twice.
        if (!launching)
            consumed = suppressed;
    }

    void onResume() { resumed = true; }
    void onPause() { resumed = false; }

    /** Save both an automatic attempt and an explicit return; neither should reopen VR on restore. */
    boolean autoStartConsumed() { return consumed; }

    boolean shouldAutoStart(boolean ready) {
        if (!resumed || !ready || consumed || launching)
            return false;
        // Consume before calling play(), which revalidates and may trigger another ready check.
        consumed = true;
        return true;
    }

    /** Automatic and manual Play share the same final validation and duplicate-handoff guard. */
    boolean beginLaunch(boolean ready) {
        if (!resumed || !ready || launching)
            return false;
        launching = true;
        consumed = true;
        return true;
    }
}
