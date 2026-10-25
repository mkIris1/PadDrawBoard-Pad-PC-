package io.paddrawboard.client

import android.media.MediaCodec
import android.media.MediaFormat
import android.graphics.SurfaceTexture
import android.view.Surface
import androidx.test.ext.junit.runners.AndroidJUnit4
import io.paddrawboard.client.protocol.ClientCapabilities
import io.paddrawboard.client.transport.AdbSession
import io.paddrawboard.client.video.AvcDecoder
import io.paddrawboard.protocol.PdbProtocol
import java.net.InetAddress
import java.net.ServerSocket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class ResourceRecoveryTest {
    @Test fun partialConnectionFailureClosesEarlierSocketsOnEveryRetry() {
        ServerSocket(0, 8, InetAddress.getByName("127.0.0.1")).use { server ->
            server.soTimeout = 8_000
            val unavailable = ServerSocket(0).use { it.localPort }
            val listener = object : AdbSession.Listener {
                override fun onControl(frame: PdbProtocol.Frame) { }
                override fun onVideo(frame: PdbProtocol.Frame) { }
                override fun onState(status: String, connected: Boolean) { }
                override fun onFailure(error: Throwable) { }
            }
            val session = AdbSession(listener, ByteArray(32), listOf(server.localPort, unavailable, server.localPort))
            try {
                session.start(ClientCapabilities("test", "test", 1280, 720, 0,
                    true, true, false, false, false, true, emptySet()))
                repeat(3) {
                    server.accept().use { peer ->
                        peer.soTimeout = 5_000
                        assertEquals("Earlier channel must close when next connect fails", -1, peer.getInputStream().read())
                    }
                }
            } finally { session.close() }
        }
    }

    @Test fun configureFailureReleasesOwnedCodec() {
        repeat(3) {
            val owned = AtomicReference<MediaCodec>()
            val finished = CountDownLatch(1)
            val texture = SurfaceTexture(false)
            val invalidSurface = Surface(texture).also { it.release() }
            texture.release()
            val decoder = AvcDecoder(invalidSurface, { finished.countDown() }, { _ -> }, {
                MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC).also { owned.set(it) }
            })
            try {
                decoder.configure(1280, 720)
                assertTrue(finished.await(8, TimeUnit.SECONDS))
                assertNotNull(owned.get())
                try {
                    owned.get().reset()
                    fail("Failed codec must already be released")
                } catch (_: IllegalStateException) { }
            } finally { decoder.close() }
        }
    }
}
