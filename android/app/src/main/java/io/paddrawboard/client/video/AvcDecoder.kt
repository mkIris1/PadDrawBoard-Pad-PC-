package io.paddrawboard.client.video

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.view.Surface
import java.nio.ByteBuffer

/**
 * Asynchronous AVC decoder whose codec and all mutable codec state live on one HandlerThread.
 * Public methods only enqueue commands, so network callbacks cannot race codec release.
 */
class AvcDecoder(
    private var surface: Surface,
    private val requestIdr: () -> Unit,
    private val dropped: (Long) -> Unit,
    private val createDecoder: () -> MediaCodec = { MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC) },
) : AutoCloseable {
    private val inbox = VideoFrameInbox()

    private val callbackThread = HandlerThread("PadDrawBoardAvc").also { it.start() }
    private val handler = Handler(callbackThread.looper)
    private val recovery = DecoderRecoveryState()
    private var codec: MediaCodec? = null
    private var pending: VideoFrameInbox.Frame? = null
    private val availableInputs = ArrayDeque<Int>()
    private var configuredWidth = 0
    private var configuredHeight = 0
    @Volatile private var closed = false
    private var idrRequested = false

    fun configure(width: Int, height: Int) {
        post {
            if (closed) return@post
            configuredWidth = width
            configuredHeight = height
            releaseCodecOnThread()
            idrRequested = false
            recovery.configured(awaitingIdr = true)
            createCodecOnThread()
            requestIdrOnceOnThread()
        }
    }

    fun reconfigure(surface: Surface, width: Int, height: Int) {
        post {
            if (closed) return@post
            this.surface = surface
            configuredWidth = width
            configuredHeight = height
            releaseCodecOnThread()
            idrRequested = false
            recovery.configured(awaitingIdr = true)
            createCodecOnThread()
            requestIdrOnceOnThread()
        }
    }

    fun queue(accessUnit: ByteArray, timestampUs: Long, keyFrame: Boolean) {
        if (!inbox.offer(VideoFrameInbox.Frame(accessUnit, timestampUs, keyFrame))) return
        post(::deliverNextOnThread)
    }

    private fun deliverNextOnThread() {
        if (closed) return
        // Let the asynchronous input callback consume the pending frame first.
        if (pending != null) {
            handler.postDelayed(::deliverNextOnThread, 2)
            return
        }
        try {
            val delivery = inbox.take() ?: return
            val frame = delivery.frame
            if (closed) return
            if (delivery.dropped > 0) {
                dropped(delivery.dropped)
                recoverReferencesOnThread()
            }
            if (codec == null && frame.keyFrame) createCodecOnThread()
            if (codec == null || !recovery.acceptFrame(frame.keyFrame)) {
                dropped(1)
                requestIdrOnceOnThread()
                return
            }
            if (pending != null && !recovery.canReplacePending(pending!!.keyFrame, frame.keyFrame)) {
                // A missing reference invalidates all following inter frames.
                dropped(1)
                recoverReferencesOnThread()
                return
            }
            if (pending != null) {
                pending = null
                dropped(1)
            }
            pending = frame
            drainInputOnThread()
        } finally {
            if (inbox.finishDelivery()) post(::deliverNextOnThread)
        }
    }

    private fun post(command: () -> Unit) {
        if (!closed) handler.post(command)
    }

    private fun createCodecOnThread() {
        if (closed || configuredWidth <= 0 || configuredHeight <= 0) return
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, configuredWidth, configuredHeight).apply {
            if (Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            if (Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_ALLOW_FRAME_DROP, 1)
        }
        try {
            val decoder = createDecoder()
            // Own the instance before any callback/configuration/start can fail.
            codec = decoder
            decoder.setCallback(object : MediaCodec.Callback() {
                override fun onInputBufferAvailable(callbackCodec: MediaCodec, index: Int) {
                    if (callbackCodec !== codec || closed) return
                    availableInputs.addLast(index)
                    drainInputOnThread()
                }

                override fun onOutputBufferAvailable(callbackCodec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                    if (callbackCodec !== codec || closed) return
                    runCatching { callbackCodec.releaseOutputBuffer(index, true) }
                        .onFailure { handleCodecErrorOnThread() }
                }

                override fun onOutputFormatChanged(callbackCodec: MediaCodec, format: MediaFormat) {
                    if (callbackCodec !== codec || closed) return
                }

                override fun onError(callbackCodec: MediaCodec, exception: MediaCodec.CodecException) {
                    if (callbackCodec !== codec || closed) return
                    handleCodecErrorOnThread()
                }
            }, handler)
            decoder.configure(format, surface, null, 0)
            decoder.start()
        } catch (_: Throwable) {
            recovery.codecError()
            releaseCodecOnThread()
            requestIdrOnceOnThread()
        }
    }

    private fun drainInputOnThread() {
        val decoder = codec ?: return
        val frame = pending ?: return
        val inputIndex = availableInputs.removeFirstOrNull() ?: return
        try {
            val buffer: ByteBuffer = decoder.getInputBuffer(inputIndex) ?: throw IllegalStateException("decoder input buffer unavailable")
            if (frame.bytes.size > buffer.capacity()) {
                // Recreate to return the acquired input slot and reset references.
                handleCodecErrorOnThread()
                return
            }
            buffer.clear()
            buffer.put(frame.bytes)
            decoder.queueInputBuffer(inputIndex, 0, frame.bytes.size, frame.timestampUs, 0)
            pending = null
            if (recovery.onFrameQueued(frame.keyFrame) && frame.keyFrame) idrRequested = false
        } catch (_: Throwable) {
            handleCodecErrorOnThread()
        }
    }

    private fun handleCodecErrorOnThread() {
        if (closed) return
        recovery.codecError()
        if (pending != null) dropped(1)
        pending = null
        availableInputs.clear()
        idrRequested = false
        releaseCodecOnThread()
        createCodecOnThread()
        requestIdrOnceOnThread()
    }

    private fun recoverReferencesOnThread() {
        // IDR resets AVC references without destroying the codec or its input slots.
        // Recreating here stalls callbacks and makes the next burst overflow again.
        recovery.codecError()
        if (pending != null) dropped(1)
        pending = null
        idrRequested = false
        requestIdrOnceOnThread()
    }

    private fun requestIdrOnceOnThread() {
        if (!idrRequested) {
            idrRequested = true
            requestIdr()
        }
    }

    private fun releaseCodecOnThread() {
        pending = null
        availableInputs.clear()
        codec?.let { decoder ->
            runCatching { decoder.stop() }
            runCatching { decoder.release() }
        }
        codec = null
    }

    override fun close() {
        if (closed) return
        closed = true
        inbox.close()
        handler.post {
            recovery.close()
            releaseCodecOnThread()
            callbackThread.quitSafely()
        }
    }
}
