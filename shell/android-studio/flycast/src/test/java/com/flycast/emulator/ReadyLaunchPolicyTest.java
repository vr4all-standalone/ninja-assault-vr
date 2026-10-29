package com.flycast.emulator;

import org.junit.Test;
import static org.junit.Assert.*;

/** Exercises production decisions, independent of Android rendering and native startup. */
public class ReadyLaunchPolicyTest {
    private ReadyLaunchPolicy resumed() {
        ReadyLaunchPolicy policy = new ReadyLaunchPolicy(false, false);
        policy.onResume();
        return policy;
    }

    @Test public void readyValidationAutomaticallyRequestsLaunch() {
        assertTrue(resumed().shouldAutoStart(true));
    }
    @Test public void missingOrInvalidFilesKeepSetupAvailable() {
        ReadyLaunchPolicy p = resumed();
        assertFalse(p.shouldAutoStart(false));
        assertFalse(p.beginLaunch(false));
        assertFalse(p.autoStartConsumed());
    }
    @Test public void permissionReturnChecksFreshReadiness() {
        ReadyLaunchPolicy p = resumed();
        assertFalse(p.shouldAutoStart(false));
        p.onPause();
        assertFalse(p.shouldAutoStart(true));
        p.onResume();
        assertTrue(p.shouldAutoStart(true));
    }
    @Test public void creationBeforeResumeCannotStart() {
        ReadyLaunchPolicy p = new ReadyLaunchPolicy(false, false);
        assertFalse(p.shouldAutoStart(true));
        assertFalse(p.beginLaunch(true));
    }
    @Test public void reentrantReadyChecksRequestOnlyOnce() {
        ReadyLaunchPolicy p = resumed();
        assertTrue(p.shouldAutoStart(true));
        assertFalse(p.shouldAutoStart(true));
        assertTrue(p.beginLaunch(true));
        assertFalse(p.beginLaunch(true));
    }
    @Test public void backgroundReadyCannotLaunchUntilResumed() {
        ReadyLaunchPolicy p = resumed();
        p.onPause();
        assertFalse(p.shouldAutoStart(true));
        assertFalse(p.beginLaunch(true));
        p.onResume();
        assertTrue(p.shouldAutoStart(true));
    }
    @Test public void recreationAfterAutomaticAttemptCannotLaunchAgain() {
        ReadyLaunchPolicy p = resumed();
        assertTrue(p.shouldAutoStart(true));
        ReadyLaunchPolicy restored = new ReadyLaunchPolicy(false, p.autoStartConsumed());
        restored.onResume();
        assertFalse(restored.shouldAutoStart(true));
        assertTrue(restored.beginLaunch(true)); // explicit manual retry remains available
    }
    @Test public void recreationBeforeReadinessStillAutomaticallyStarts() {
        ReadyLaunchPolicy p = resumed();
        assertFalse(p.shouldAutoStart(false));
        ReadyLaunchPolicy restored = new ReadyLaunchPolicy(false, p.autoStartConsumed());
        restored.onResume();
        assertTrue(restored.shouldAutoStart(true));
    }
    @Test public void explicitQuitReturnSuppressesAutomaticButAllowsPlay() {
        ReadyLaunchPolicy p = new ReadyLaunchPolicy(true, false);
        p.onResume();
        assertFalse(p.shouldAutoStart(true));
        assertTrue(p.autoStartConsumed());
        assertTrue(p.beginLaunch(true));
        assertFalse(p.beginLaunch(true));
    }
    @Test public void quitSuppressionSurvivesRecreation() {
        ReadyLaunchPolicy p = new ReadyLaunchPolicy(true, false);
        ReadyLaunchPolicy restored = new ReadyLaunchPolicy(false, p.autoStartConsumed());
        restored.onResume();
        assertFalse(restored.shouldAutoStart(true));
    }
    @Test public void disappearingFileBeforeHandoffDoesNotLaunch() {
        ReadyLaunchPolicy p = resumed();
        assertTrue(p.shouldAutoStart(true));
        assertFalse(p.beginLaunch(false));
        assertFalse(p.shouldAutoStart(true));
        assertTrue(p.beginLaunch(true)); // manual revalidation after files are repaired
    }
    @Test public void manualLaunchAlsoConsumesAutomaticAndRejectsDoubleTap() {
        ReadyLaunchPolicy p = resumed();
        assertTrue(p.beginLaunch(true));
        assertTrue(p.autoStartConsumed());
        assertFalse(p.shouldAutoStart(true));
        assertFalse(p.beginLaunch(true));
        p.onPause(); p.onResume();
        assertFalse(p.shouldAutoStart(true));
    }
    @Test public void freshNormalEntryAfterQuitRearmsAutomaticEntry() {
        ReadyLaunchPolicy p = new ReadyLaunchPolicy(true, false);
        p.onResume();
        assertFalse(p.shouldAutoStart(true));
        p.onNewEntry(false);
        assertTrue(p.shouldAutoStart(true));
    }
    @Test public void freshNormalEntryAfterFailedValidationRearmsAutomaticEntry() {
        ReadyLaunchPolicy p = resumed();
        assertTrue(p.shouldAutoStart(true));
        assertFalse(p.beginLaunch(false));
        p.onNewEntry(false);
        assertTrue(p.shouldAutoStart(true));
    }
    @Test public void suppressedNewReturnKeepsReadyPanelManual() {
        ReadyLaunchPolicy p = resumed();
        p.onNewEntry(true);
        assertFalse(p.shouldAutoStart(true));
        assertTrue(p.autoStartConsumed());
        assertTrue(p.beginLaunch(true));
    }
    @Test public void normalNewIntentWhilePausedWaitsForResume() {
        ReadyLaunchPolicy p = new ReadyLaunchPolicy(true, false);
        p.onResume(); p.onPause();
        p.onNewEntry(false);
        assertFalse(p.shouldAutoStart(true));
        assertFalse(p.beginLaunch(true));
        p.onResume();
        assertTrue(p.shouldAutoStart(true));
    }
    @Test public void newEntryStillAllowsOnlyOneAutomaticAttempt() {
        ReadyLaunchPolicy p = new ReadyLaunchPolicy(true, false);
        p.onResume();
        p.onNewEntry(false);
        assertTrue(p.shouldAutoStart(true));
        assertFalse(p.shouldAutoStart(true));
        p.onPause(); p.onResume();
        assertFalse(p.shouldAutoStart(true));
    }
    @Test public void newIntentDuringHandoffCannotStartAnotherGame() {
        ReadyLaunchPolicy p = resumed();
        assertTrue(p.beginLaunch(true));
        p.onNewEntry(false);
        assertFalse(p.shouldAutoStart(true));
        assertFalse(p.beginLaunch(true));
    }
}
