package io.paddrawboard.client.video

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DecoderRecoveryStateTest {
    @Test fun recoveryDropsInterFramesUntilNextIdr() {
        val state = DecoderRecoveryState()
        state.configured(awaitingIdr = true)
        assertEquals(DecoderPhase.AWAITING_IDR, state.phase)
        assertFalse(state.acceptFrame(keyFrame = false))
        assertTrue(state.acceptFrame(keyFrame = true))
        assertEquals(DecoderPhase.AWAITING_IDR, state.phase)
        assertTrue(state.onFrameQueued(keyFrame = true))
        assertEquals(DecoderPhase.RUNNING, state.phase)
        assertTrue(state.acceptFrame(keyFrame = false))
    }

    @Test fun pendingIdrCannotBeReplacedByInterFrameBeforeQueue() {
        val state = DecoderRecoveryState()
        state.configured(awaitingIdr = true)

        assertTrue(state.acceptFrame(keyFrame = true))
        assertEquals(DecoderPhase.AWAITING_IDR, state.phase)
        assertFalse(state.canReplacePending(pendingKeyFrame = true, incomingKeyFrame = false))
        assertTrue(state.canReplacePending(pendingKeyFrame = true, incomingKeyFrame = true))

        state.onFrameQueued(keyFrame = true)
        assertEquals(DecoderPhase.RUNNING, state.phase)
        assertFalse(state.canReplacePending(pendingKeyFrame = false, incomingKeyFrame = false))
        state.codecError()
        assertFalse(state.acceptFrame(keyFrame = false))
        assertTrue(state.acceptFrame(keyFrame = true))
    }

    @Test fun acceptedIdrRemainsAwaitingUntilQueue() {
        val state = DecoderRecoveryState()
        state.configured(awaitingIdr = true)
        assertTrue(state.acceptFrame(keyFrame = true))
        assertEquals(DecoderPhase.AWAITING_IDR, state.phase)
    }

    @Test fun codecErrorReturnsToAwaitingIdrAndCloseIsTerminal() {
        val state = DecoderRecoveryState()
        state.configured(awaitingIdr = false)
        assertTrue(state.codecError())
        assertEquals(DecoderPhase.AWAITING_IDR, state.phase)
        state.close()
        assertFalse(state.codecError())
        assertFalse(state.acceptFrame(keyFrame = true))
    }
}
