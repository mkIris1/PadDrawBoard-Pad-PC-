package io.paddrawboard.client.video

/** Small platform-independent state machine used by the asynchronous decoder. */
internal enum class DecoderPhase { IDLE, RUNNING, AWAITING_IDR, CLOSED }

internal class DecoderRecoveryState {
    var phase: DecoderPhase = DecoderPhase.IDLE
        private set

    fun configured(awaitingIdr: Boolean) {
        if (phase != DecoderPhase.CLOSED) phase = if (awaitingIdr) DecoderPhase.AWAITING_IDR else DecoderPhase.RUNNING
    }

    fun acceptFrame(keyFrame: Boolean): Boolean {
        if (phase == DecoderPhase.CLOSED || phase == DecoderPhase.IDLE) return false
        if (phase == DecoderPhase.AWAITING_IDR && !keyFrame) return false
        return true
    }

    /**
     * Only an IDR can replace an unsubmitted frame without breaking references.
     */
    fun canReplacePending(pendingKeyFrame: Boolean, incomingKeyFrame: Boolean): Boolean =
        incomingKeyFrame

    /** Advances recovery only after MediaCodec accepted the input buffer. */
    fun onFrameQueued(keyFrame: Boolean): Boolean {
        if (phase == DecoderPhase.CLOSED || phase == DecoderPhase.IDLE) return false
        if (keyFrame) phase = DecoderPhase.RUNNING
        return true
    }

    fun codecError(): Boolean {
        if (phase == DecoderPhase.CLOSED) return false
        phase = DecoderPhase.AWAITING_IDR
        return true
    }

    fun close() { phase = DecoderPhase.CLOSED }
}
