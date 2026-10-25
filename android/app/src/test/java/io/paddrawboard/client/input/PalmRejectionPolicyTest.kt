package io.paddrawboard.client.input

import org.junit.Assert.*
import org.junit.Test

class PalmRejectionPolicyTest {
    @Test fun touchIsSuppressedOnlyWhileStylusTipIsActive() { val policy = PalmRejectionPolicy({ true }); policy.stylusObserved(100); assertTrue(policy.suppressTouch(100)); policy.stylusLeftRange(200); assertFalse(policy.suppressTouch(200)); assertFalse(policy.suppressTouch(201)) }
    @Test fun optionalReleaseDelayRemainsSupported() { val policy = PalmRejectionPolicy({ true }, 150); policy.stylusObserved(100); policy.stylusLeftRange(200); assertTrue(policy.suppressTouch(349)); assertFalse(policy.suppressTouch(350)) }
    @Test fun settingCanDisableSuppression() { val policy = PalmRejectionPolicy({ false }); policy.stylusObserved(1); assertFalse(policy.suppressTouch(1)) }
}
