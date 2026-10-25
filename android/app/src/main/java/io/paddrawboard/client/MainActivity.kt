package io.paddrawboard.client

import android.app.Activity
import android.content.pm.ActivityInfo
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.view.WindowInsets
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import io.paddrawboard.client.diagnostics.CapabilityProbe
import io.paddrawboard.client.input.InputCaptureView
import io.paddrawboard.client.protocol.*
import io.paddrawboard.client.transport.AdbSession
import io.paddrawboard.client.transport.SessionAuth
import io.paddrawboard.client.video.AvcDecoder
import io.paddrawboard.protocol.PdbProtocol

class MainActivity : Activity() {
    private val state = AppState()
    private val inputGestureRouter = InputGestureRouter()
    private lateinit var capture: InputCaptureView
    private lateinit var status: TextView
    private var session: AdbSession? = null
    private var decoder: AvcDecoder? = null
    @Volatile private var latestConfig: PdbProtocol.ServerConfig? = null
    @Volatile private var surfaceReady = false
    private var sessionToken: ByteArray? = null
    // Close the app when the desktop side disappears (crash, forced kill, USB
    // unplug). Without this the tablet would sit on a frozen frame forever and,
    // because it requested landscape, keep the display rotated.
    private val closeWatchdog = Handler(Looper.getMainLooper())
    // The transport retries every second or two and reports "not connected" on
    // each attempt. Re-arming the timer on every report would postpone it
    // forever, so it is armed once per outage.
    // Puts the panel back to portrait before the app goes away. The desktop
    // side used to do this by writing settings, which cannot happen when the
    // desktop is force-killed; the app can do it itself as long as it holds
    // WRITE_SETTINGS (granted over adb).
    private fun lockPortraitForExit() {
        val resolver = contentResolver
        for (attempt in 1..3) {
            try {
                if (!android.provider.Settings.System.canWrite(this)) {
                    android.util.Log.w("PadDrawBoard", "WRITE_SETTINGS not granted; portrait lock skipped")
                    return
                }
                android.provider.Settings.System.putInt(
                    resolver, android.provider.Settings.System.ACCELEROMETER_ROTATION, 0,
                )
                android.provider.Settings.System.putInt(
                    resolver, android.provider.Settings.System.USER_ROTATION, 0,
                )
                val accel = android.provider.Settings.System.getInt(
                    resolver, android.provider.Settings.System.ACCELEROMETER_ROTATION, -1,
                )
                val rotation = android.provider.Settings.System.getInt(
                    resolver, android.provider.Settings.System.USER_ROTATION, -1,
                )
                android.util.Log.i(
                    "PadDrawBoard",
                    "portrait lock attempt=" + attempt + " accel=" + accel + " rotation=" + rotation,
                )
                if (accel == 0 && rotation == 0) return
            } catch (error: Throwable) {
                android.util.Log.w("PadDrawBoard", "portrait lock failed", error)
            }
            try {
                Thread.sleep(250)
            } catch (ignored: InterruptedException) {
                return
            }
        }
    }

    // A plain periodic check beats a callback-driven timer here: the transport
    // reconnects in a loop and reports state changes from several threads, which
    // kept re-arming (and therefore never firing) a delayed callback.
    private var connectedAtLeastOnce = false
    private var lastConnectedAt = 0L
    private val disconnectWatchdog = object : Runnable {
        override fun run() {
            val connected = state.snapshot().connection == ConnectionState.CONNECTED
            val now = System.currentTimeMillis()
            if (connected) {
                connectedAtLeastOnce = true
                lastConnectedAt = now
            } else if (connectedAtLeastOnce && now - lastConnectedAt > 10_000L) {
                android.util.Log.i("PadDrawBoard", "disconnected 10s; closing and locking portrait")
                lockPortraitForExit()
                finishAndRemoveTask()
                try {
                    Thread.sleep(400)
                } catch (ignored: InterruptedException) {
                }
                android.os.Process.killProcess(android.os.Process.myPid())
                return
            }
            closeWatchdog.postDelayed(this, 1_000L)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) { super.onCreate(savedInstanceState)
        // Ask for landscape at the activity level instead of changing the
        // system rotation setting. The window is rotated for as long as this
        // activity lives, so the surface is 2560 px wide (the desktop requires
        // at least 1920) and it snaps back to portrait as soon as the app goes
        // away - even if the desktop process was killed outright.
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val root = FrameLayout(this); capture = InputCaptureView(this,
            { session?.sendInput(it, capture.width, capture.height) },
            { state.snapshot().palmRejectionEnabled },
            { bits -> session?.sendControl(PdbProtocol.CapabilityChanged(bits)) },
        ); root.addView(capture, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT,
        ))
        status = TextView(this).apply { setBackgroundColor(0x99000000.toInt()); setTextColor(-1); setPadding(20, 20, 20, 20) }
        root.addView(status, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.WRAP_CONTENT,
            android.view.Gravity.TOP,
        )); setContentView(root); requestImmersive()
        closeWatchdog.postDelayed(disconnectWatchdog, 1_000L)
        state.observe { snapshot -> runOnUiThread {
            status.text = buildString {
                append("${snapshot.connection}: ${snapshot.status}\n${snapshot.capabilitySummary}")
                snapshot.lastError?.takeIf { it.isNotBlank() }?.let { append("\nError: $it") }
            }
        } }
        sessionToken = SessionAuth.parseTokenHex(intent.getStringExtra(SessionAuth.INTENT_EXTRA))
        if (sessionToken == null) {
            state.update { it.copy(connection = ConnectionState.ERROR, status = "Invalid or missing session authentication token") }
        }
        status.setOnClickListener {
            val capabilities = CapabilityProbe(this).inspect(capture.width, capture.height, rotationDegrees(), capture.penKeyCapabilities())
            val log = CapabilityProbe(this).export(capabilities)
            state.update { current -> current.copy(capabilitySummary = "${capabilities}\nExported: ${log.name}") }
        }
        capture.holder.addCallback(object : android.view.SurfaceHolder.Callback {
            override fun surfaceCreated(holder: android.view.SurfaceHolder) { surfaceReady = true }
            override fun surfaceChanged(holder: android.view.SurfaceHolder, format: Int, width: Int, height: Int) { if (session == null) start() else rotationChanged() }
            override fun surfaceDestroyed(holder: android.view.SurfaceHolder) { surfaceReady = false; stop() }
        })
    }
    private fun start() { if (session != null) return
        val token = sessionToken
        if (token == null) {
            state.update { it.copy(connection = ConnectionState.ERROR, status = "Invalid or missing session authentication token") }
            return
        }
        val probed = CapabilityProbe(this).inspect(capture.width, capture.height, rotationDegrees(), capture.penKeyCapabilities())
        val codec = PdbCodec()
        capture.initializeCapabilities(codec.capabilityBits(probed))
        val caps = codec.withCapabilityBits(probed, capture.currentCapabilityBits())
        if (!caps.h264Decoder) { state.update { it.copy(connection = ConnectionState.ERROR, status = "H.264 decoder unavailable", capabilitySummary = caps.toString()) }; return }
        state.update { it.copy(connection = ConnectionState.CONNECTING, capabilitySummary = caps.toString()) }
        session = AdbSession(object : AdbSession.Listener {
            override fun onControl(frame: PdbProtocol.Frame) { if (frame.payload is PdbProtocol.ServerConfig) {
                val config = frame.payload as PdbProtocol.ServerConfig
                latestConfig = config
                if (surfaceReady) configureDecoder(config)
            } }
            override fun onVideo(frame: PdbProtocol.Frame) { if (frame.payload is PdbProtocol.VideoFrame) { val video = frame.payload as PdbProtocol.VideoFrame; session?.recordVideoFrame(video.presentationTimestampNs); decoder?.queue(video.accessUnit, video.presentationTimestampNs / 1_000, frame.header.flags and PdbProtocol.VIDEO_FLAG_IDR != 0L) } }
            override fun onState(status: String, connected: Boolean) {
                state.update { it.copy(connection = if (connected) ConnectionState.CONNECTED else ConnectionState.RECONNECTING, status = status, lastError = if (connected) null else it.lastError) }
            }
            override fun onFailure(error: Throwable) {
                cancelActivePointers()
                state.update { it.copy(connection = ConnectionState.ERROR, status = "Session error", lastError = error.message) }
            }
        }, token).also { it.start(caps) }
    }
    private fun cancelActivePointers() {
        capture.releasePointers().takeIf { it.isNotEmpty() }?.let { session?.sendInput(it, capture.width, capture.height) }
    }
    private fun configureDecoder(config: PdbProtocol.ServerConfig) {
        if (!surfaceReady) return
        val current = decoder
        if (current == null) {
            decoder = AvcDecoder(capture.holder.surface, { session?.sendControl(PdbProtocol.RequestIdr) }, { count ->
                session?.recordDroppedVideoFrame(count)
                state.update { value -> value.copy(droppedVideoFrames = value.droppedVideoFrames + count) }
            })
            decoder?.configure(config.videoWidth, config.videoHeight)
        } else {
            current.reconfigure(capture.holder.surface, config.videoWidth, config.videoHeight)
        }
    }
    private fun rotationChanged() {
        cancelActivePointers()
        session?.sendControl(PdbProtocol.OrientationChanged(rotationDegrees()))
        val config = latestConfig
        if (config != null && surfaceReady) {
            // Rebind to the current Surface and await a fresh IDR after rotation.
            decoder?.reconfigure(capture.holder.surface, config.videoWidth, config.videoHeight)
                ?: configureDecoder(config)
        } else {
            session?.sendControl(PdbProtocol.RequestIdr)
        }
    }
    private fun rotationDegrees(): Int = when (display?.rotation) { android.view.Surface.ROTATION_90 -> 90; android.view.Surface.ROTATION_180 -> 180; android.view.Surface.ROTATION_270 -> 270; else -> 0 }
    private fun stop() { cancelActivePointers(); decoder?.close(); decoder = null; latestConfig = null; session?.close(); session = null; state.update { it.copy(connection = ConnectionState.STOPPED, status = "Stopped") } }
    private fun requestImmersive() {
        val decor = window.decorView
        decor.post {
            if (isFinishing || isDestroyed || !decor.isAttachedToWindow) return@post
            val controller = decor.windowInsetsController ?: return@post
            controller.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
            controller.systemBarsBehavior = android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }
    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean {
        if (::capture.isInitialized && capture.handlePenKeyEvent(event)) return true
        return super.dispatchKeyEvent(event)
    }
    override fun dispatchTouchEvent(event: android.view.MotionEvent): Boolean {
        if (::capture.isInitialized && ::status.isInitialized &&
            inputGestureRouter.shouldCapture(event.actionMasked, event.y, status.bottom)) {
            return capture.captureMotionEvent(event)
        }
        return super.dispatchTouchEvent(event)
    }
    override fun dispatchGenericMotionEvent(event: android.view.MotionEvent): Boolean {
        if (::capture.isInitialized && ::status.isInitialized && event.y >= status.bottom &&
            capture.captureMotionEvent(event)) {
            return true
        }
        return super.dispatchGenericMotionEvent(event)
    }
    override fun onWindowFocusChanged(hasFocus: Boolean) { super.onWindowFocusChanged(hasFocus); if (hasFocus) requestImmersive() }
    override fun onDestroy() { closeWatchdog.removeCallbacks(disconnectWatchdog); stop(); super.onDestroy() }
}

internal class InputGestureRouter {
    private var capturing = false

    fun shouldCapture(actionMasked: Int, y: Float, statusBottom: Int): Boolean {
        if (actionMasked == android.view.MotionEvent.ACTION_DOWN) {
            capturing = y >= statusBottom
        }
        val result = capturing
        if (actionMasked == android.view.MotionEvent.ACTION_UP ||
            actionMasked == android.view.MotionEvent.ACTION_CANCEL) {
            capturing = false
        }
        return result
    }
}
