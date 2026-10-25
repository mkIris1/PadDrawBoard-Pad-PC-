package io.paddrawboard.client.transport

import io.paddrawboard.client.protocol.CapturedInputSample
import io.paddrawboard.client.protocol.ClientCapabilities
import io.paddrawboard.client.protocol.PdbCodec
import io.paddrawboard.protocol.PdbProtocol
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.CopyOnWriteArraySet
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import android.os.SystemClock
import android.util.Log

/** Connects device loopback ports that adb reverse maps to the desktop listeners. */
class AdbSession(private val listener: Listener, sessionToken: ByteArray,
    private val ports: List<Int> = listOf(48100, 48101, 48102),
) : AutoCloseable {
    interface Listener { fun onControl(frame: PdbProtocol.Frame); fun onVideo(frame: PdbProtocol.Frame); fun onState(status: String, connected: Boolean); fun onFailure(error: Throwable) }
    private val codec = PdbCodec(); private val running = AtomicBoolean(); private val reconnecting = AtomicBoolean()
    private val sessionToken = sessionToken.clone()
    private val executor = Executors.newScheduledThreadPool(3); private val sockets = CopyOnWriteArraySet<Socket>()
    private val inputExecutor = boundedWriter()
    private val controlExecutor = boundedWriter()
    private val generation = AtomicLong()
    private val controlSequence = AtomicLong(); private val inputSequence = AtomicLong()
    private val controlLock = Any(); private val inputLock = Any()
    private val telemetryClock = TelemetryClock { SystemClock.elapsedRealtimeNanos() }
    private val droppedVideoFrames = AtomicLong()
    private val failureNotified = AtomicBoolean()
    @Volatile private var input: java.io.OutputStream? = null
    @Volatile private var control: java.io.OutputStream? = null
    @Volatile private var lastHello: ClientCapabilities? = null
    private var reconnect: ScheduledFuture<*>? = null
    private var telemetry: ScheduledFuture<*>? = null
    fun start(hello: ClientCapabilities) { if (running.compareAndSet(false, true)) { lastHello = hello; connect(hello) } }
    private fun connect(hello: ClientCapabilities) { executor.execute {
        val attempt = generation.get()
        try {
            if (!running.get()) return@execute
            failureNotified.set(false)
            listener.onState("Connecting through adb reverse", false)
            val controlSocket = open(ports[0], attempt); val videoSocket = open(ports[1], attempt); val inputSocket = open(ports[2], attempt)
            val controlOutput = controlSocket.getOutputStream()
            val videoOutput = videoSocket.getOutputStream()
            val inputOutput = inputSocket.getOutputStream()
            SessionAuth.writePreface(controlOutput, sessionToken, SessionAuth.Channel.CONTROL)
            SessionAuth.writePreface(videoOutput, sessionToken, SessionAuth.Channel.VIDEO)
            SessionAuth.writePreface(inputOutput, sessionToken, SessionAuth.Channel.INPUT)
            synchronized(sockets) {
                if (!running.get() || generation.get() != attempt) return@execute
                input = inputOutput; control = controlOutput
            }
            if (!writeControl(codec.hello(hello, controlSequence.incrementAndGet()))) {
                scheduleReconnect(hello)
                return@execute
            }
            listener.onState("Connected", true); executor.execute { read(controlSocket, listener::onControl, hello, true) }; executor.execute { read(videoSocket, listener::onVideo, hello, false) }
            telemetry?.cancel(false)
            telemetry = executor.scheduleAtFixedRate({
                val request = telemetryClock.request(SystemClock.elapsedRealtimeNanos())
                sendControl(request)
                sendControl(telemetryClock.telemetry(droppedVideoFrames.get()))
            }, 1, 1, TimeUnit.SECONDS)
        } catch (error: Throwable) { if (running.get() && generation.get() == attempt) { notifyTransportFailure(error, hello) } }
    } }
    fun sendInput(samples: List<CapturedInputSample>, width: Int, height: Int) {
        if (samples.isEmpty()) return
        val stream = input ?: return
        val pendingSamples = samples.toList()
        try {
            inputExecutor.execute {
                synchronized(inputLock) {
                    if (input !== stream) return@synchronized
                    runCatching {
                        // Keep MotionEvent history ordered while respecting the protocol limit.
                        pendingSamples.chunked(PdbProtocol.MAX_INPUT_SAMPLES).forEach { chunk ->
                            codec.write(stream, codec.input(chunk, width, height, inputSequence.incrementAndGet()))
                        }
                    }.onFailure { if (input === stream) notifyTransportFailure(it) }
                }
            }
        } catch (error: RejectedExecutionException) {
            if (running.get()) notifyTransportFailure(error)
        }
    }
    fun sendControl(message: PdbProtocol.Control) {
        val stream = control ?: return
        try {
            controlExecutor.execute {
                synchronized(controlLock) {
                    if (control !== stream) return@synchronized
                    runCatching { codec.write(stream, codec.control(message, controlSequence.incrementAndGet())) }
                        .onFailure { if (control === stream) notifyTransportFailure(it) }
                }
            }
        } catch (error: RejectedExecutionException) {
            if (running.get()) notifyTransportFailure(error)
        }
    }
    fun recordDroppedVideoFrame(count: Long = 1) { droppedVideoFrames.addAndGet(count) }
    fun recordVideoFrame(hostPresentationTimestampNs: Long, receivedNs: Long = SystemClock.elapsedRealtimeNanos()) {
        telemetryClock.recordVideo(hostPresentationTimestampNs, receivedNs)
    }
    private fun writeControl(frame: PdbProtocol.Frame): Boolean {
        synchronized(controlLock) {
            val stream = control ?: return false
            val result = runCatching { codec.write(stream, frame) }
            result.onFailure { if (control === stream) notifyTransportFailure(it) }
            return result.isSuccess
        }
    }
    private fun open(port: Int, attempt: Long): Socket {
        val socket = synchronized(sockets) {
            check(running.get() && generation.get() == attempt) { "Connection attempt cancelled" }
            // Register before connecting: partial initialization and close() own it too.
            Socket().also { sockets.add(it) }
        }
        try {
            socket.tcpNoDelay = true
            socket.connect(InetSocketAddress("127.0.0.1", port), 2_000)
            return socket
        } catch (error: Throwable) {
            sockets.remove(socket)
            runCatching { socket.close() }
            throw error
        }
    }
    private fun read(socket: Socket, consume: (PdbProtocol.Frame) -> Unit, hello: ClientCapabilities, controlChannel: Boolean) {
        try {
            socket.getInputStream().use {
                while (running.get()) {
                    val frame = codec.read(it)
                    if (controlChannel) handleControl(frame)
                    consume(frame)
                }
            }
        } catch (error: Throwable) { if (running.get() && sockets.contains(socket)) notifyTransportFailure(error, hello) }
    }
    private fun handleControl(frame: PdbProtocol.Frame) {
        val control = (frame.payload as? PdbProtocol.ControlPayload)?.control ?: return
        when (control) {
            is PdbProtocol.ClockSyncRequest -> {
                val receive = SystemClock.elapsedRealtimeNanos()
                sendControl(PdbProtocol.ClockSyncResponse(control.clientSendTimestampNs, receive, SystemClock.elapsedRealtimeNanos()))
            }
            is PdbProtocol.ClockSyncResponse -> {
                // Capture t4 before any further work, then return all four timestamps.
                val deviceReceiveNs = SystemClock.elapsedRealtimeNanos()
                sendControl(telemetryClock.acceptAndComplete(control, deviceReceiveNs))
            }
            else -> Unit
        }
    }
    private fun notifyTransportFailure(error: Throwable, hello: ClientCapabilities? = lastHello) {
        if (!running.get() || !failureNotified.compareAndSet(false, true)) return
        Log.e("PadDrawBoard", "Transport failure; reconnecting", error)
        if (hello != null) scheduleReconnect(hello)
        listener.onFailure(error)
    }
    private fun scheduleReconnect(hello: ClientCapabilities) {
        if (!reconnecting.compareAndSet(false, true)) return
        telemetry?.cancel(false); telemetry = null
        closeSockets()
        listener.onState("Reconnecting", false)
        reconnect?.cancel(false); reconnect = executor.schedule({ reconnecting.set(false); if (running.get()) connect(hello) }, 1, TimeUnit.SECONDS)
    }
    private fun closeSockets() = synchronized(sockets) {
        generation.incrementAndGet()
        input = null; control = null
        val owned = sockets.toList()
        sockets.clear()
        owned.forEach { runCatching { it.close() } }
        inputExecutor.queue.clear(); controlExecutor.queue.clear()
    }
    override fun close() { running.set(false); telemetry?.cancel(true); reconnect?.cancel(true); closeSockets(); inputExecutor.shutdownNow(); controlExecutor.shutdownNow(); executor.shutdownNow() }
}
