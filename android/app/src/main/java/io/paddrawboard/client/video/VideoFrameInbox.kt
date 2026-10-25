package io.paddrawboard.client.video

/** Bounded USB burst buffer; only one delivery task may be scheduled. */
internal class VideoFrameInbox {
    data class Frame(val bytes: ByteArray, val timestampUs: Long, val keyFrame: Boolean)
    data class Delivery(val frame: Frame, val dropped: Long)
    private val frames = ArrayDeque<Frame>()
    private var scheduled = false
    private var dropped = 0L
    private var closed = false

    @Synchronized fun offer(value: Frame): Boolean {
        if (closed) return false
        if (frames.size == 4) {
            dropped += frames.size
            frames.clear()
        }
        frames.addLast(value)
        val schedule = !scheduled
        scheduled = true
        return schedule
    }

    @Synchronized fun take(): Delivery? {
        val value = frames.removeFirstOrNull() ?: return null
        val result = Delivery(value, dropped)
        dropped = 0
        return result
    }

    @Synchronized fun finishDelivery(): Boolean {
        scheduled = frames.isNotEmpty()
        return scheduled
    }

    @Synchronized fun close() { closed = true; frames.clear(); dropped = 0; scheduled = false }
}
