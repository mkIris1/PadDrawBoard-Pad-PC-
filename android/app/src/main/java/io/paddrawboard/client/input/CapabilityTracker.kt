package io.paddrawboard.client.input

import io.paddrawboard.protocol.PdbProtocol

/**
 * Tracks capabilities observed by the client. A bitset is emitted once whenever it
 * changes; repeated events for the same bitset are deliberately silent.
 */
class CapabilityTracker(private val onChanged: (Long) -> Unit = {}) {
    private var bits = 0L
    private var lastEmitted: Long? = null

    @Synchronized fun initialize(initialBits: Long) {
        bits = bits or (initialBits and PdbProtocol.KNOWN_CAPABILITIES)
        if (lastEmitted == null) lastEmitted = bits
    }

    @Synchronized fun observeAxes(pressure: Boolean, tilt: Boolean, distance: Boolean) {
        if (pressure) bits = bits or PdbProtocol.CAP_PRESSURE
        if (tilt) bits = bits or PdbProtocol.CAP_TILT
        if (distance) bits = bits or PdbProtocol.CAP_DISTANCE
        emitIfChanged()
    }

    @Synchronized fun observeHoverAction() {
        bits = bits or PdbProtocol.CAP_HOVER
        emitIfChanged()
    }

    @Synchronized fun observeButtons(buttons: Int) {
        if (buttons and PdbProtocol.BUTTON_1 != 0) bits = bits or PdbProtocol.CAP_BUTTON_1
        if (buttons and PdbProtocol.BUTTON_2 != 0) bits = bits or PdbProtocol.CAP_BUTTON_2
        if (buttons and PdbProtocol.BUTTON_3 != 0) bits = bits or PdbProtocol.CAP_BUTTON_3
        emitIfChanged()
    }

    @Synchronized fun currentBits(): Long = bits

    private fun emitIfChanged() {
        if (lastEmitted == bits) return
        lastEmitted = bits
        onChanged(bits)
    }
}
