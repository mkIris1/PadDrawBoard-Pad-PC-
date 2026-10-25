package io.paddrawboard.client.input

import io.paddrawboard.protocol.PdbProtocol
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CapabilityTrackerTest {
    @Test fun distanceDoesNotClaimHoverUntilHoverEventIsObserved() {
        val changes = mutableListOf<Long>()
        val tracker = CapabilityTracker(changes::add)
        tracker.initialize(PdbProtocol.CAP_DISTANCE)
        tracker.observeAxes(pressure = false, tilt = false, distance = true)
        assertFalse(tracker.currentBits() and PdbProtocol.CAP_HOVER != 0L)
        assertTrue(changes.isEmpty())
        tracker.observeHoverAction()
        tracker.observeHoverAction()
        assertEquals(listOf(PdbProtocol.CAP_DISTANCE or PdbProtocol.CAP_HOVER), changes)
    }

    @Test fun axesAndButtonsTransitionIndependentlyAndOncePerBitset() {
        val changes = mutableListOf<Long>()
        val tracker = CapabilityTracker(changes::add)
        tracker.initialize(0)
        tracker.observeAxes(pressure = true, tilt = false, distance = false)
        tracker.observeAxes(pressure = true, tilt = false, distance = false)
        tracker.observeButtons(PdbProtocol.BUTTON_2)
        tracker.observeButtons(PdbProtocol.BUTTON_2)
        tracker.observeAxes(pressure = false, tilt = true, distance = false)
        assertEquals(listOf(PdbProtocol.CAP_PRESSURE,
            PdbProtocol.CAP_PRESSURE or PdbProtocol.CAP_BUTTON_2,
            PdbProtocol.CAP_PRESSURE or PdbProtocol.CAP_BUTTON_2 or PdbProtocol.CAP_TILT), changes)
    }
}
