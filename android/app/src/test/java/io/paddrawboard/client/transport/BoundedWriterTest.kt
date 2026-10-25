package io.paddrawboard.client.transport

import java.util.concurrent.CountDownLatch
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.TimeUnit
import org.junit.Assert.*
import org.junit.Test

class BoundedWriterTest {
    @Test fun blockedWriteRejectsOverflowWithoutDiscardingQueuedTransitions() {
        val writer = boundedWriter()
        val started = CountDownLatch(1)
        val release = CountDownLatch(1)
        val received = mutableListOf<Int>()
        try {
            writer.execute { started.countDown(); release.await(5, TimeUnit.SECONDS) }
            assertTrue(started.await(5, TimeUnit.SECONDS))
            repeat(8) { index -> writer.execute { received.add(index) } }
            try {
                writer.execute { fail("Overflow must never execute") }
                fail("Expected explicit overflow rejection")
            } catch (_: RejectedExecutionException) { }
            assertEquals(8, writer.queue.size)
            release.countDown()
            writer.shutdown()
            assertTrue(writer.awaitTermination(5, TimeUnit.SECONDS))
            assertEquals((0..7).toList(), received)
        } finally { release.countDown(); writer.shutdownNow() }
    }
}
