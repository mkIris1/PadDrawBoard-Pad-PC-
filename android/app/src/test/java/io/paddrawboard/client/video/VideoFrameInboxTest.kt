package io.paddrawboard.client.video

import org.junit.Assert.*
import org.junit.Test

class VideoFrameInboxTest {
    @Test fun blockedHandlerKeepsFourFramesAndReportsEveryLostReference() {
        val inbox = VideoFrameInbox()
        var scheduled = 0
        repeat(10_000) { index ->
            if (inbox.offer(VideoFrameInbox.Frame(byteArrayOf(1), index.toLong(), index == 0))) scheduled++
        }
        assertEquals(1, scheduled)
        val delivery = inbox.take()!!
        assertEquals(9_996L, delivery.frame.timestampUs)
        assertEquals(9_996L, delivery.dropped)
        assertFalse(delivery.frame.keyFrame)
        repeat(3) { assertEquals(9_997L + it, inbox.take()!!.frame.timestampUs) }
        assertNull(inbox.take())
        assertFalse(inbox.finishDelivery())
        assertTrue(inbox.offer(VideoFrameInbox.Frame(byteArrayOf(2), 10_000, true)))
        assertEquals(0L, inbox.take()!!.dropped)
    }

    @Test fun ordinaryUsbBurstPreservesIdrAndFollowingFrames() {
        val inbox = VideoFrameInbox()
        repeat(4) { index ->
            assertEquals(index == 0, inbox.offer(VideoFrameInbox.Frame(byteArrayOf(1), index.toLong(), index == 0)))
        }
        repeat(4) { index ->
            val delivery = inbox.take()!!
            assertEquals(index.toLong(), delivery.frame.timestampUs)
            assertEquals(0L, delivery.dropped)
            assertEquals(index < 3, inbox.finishDelivery())
        }
    }

    @Test fun closeReleasesQueuedDataAndRejectsFurtherScheduling() {
        val inbox = VideoFrameInbox()
        inbox.offer(VideoFrameInbox.Frame(ByteArray(1024), 1, true))
        inbox.close()
        assertNull(inbox.take())
        assertFalse(inbox.offer(VideoFrameInbox.Frame(byteArrayOf(1), 2, false)))
    }
}
