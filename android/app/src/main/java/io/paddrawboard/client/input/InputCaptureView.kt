package io.paddrawboard.client.input

import android.content.Context
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceView
import io.paddrawboard.client.protocol.CapturedAction
import io.paddrawboard.client.protocol.CapturedInputSample
import io.paddrawboard.client.protocol.CapturedTool

class InputCaptureView(
    context: Context,
    private val sink: (List<CapturedInputSample>) -> Unit,
    palmEnabled: () -> Boolean,
    capabilitySink: (Long) -> Unit = {},
) : SurfaceView(context) {
    private val palm = PalmRejectionPolicy(palmEnabled)
    private val activePointers = linkedMapOf<Int, CapturedInputSample>()
    private val capabilityTracker = CapabilityTracker(capabilitySink)
    private var pressedButtons = 0
    private var lastStylusSample: CapturedInputSample? = null
    init {
        isFocusableInTouchMode = true
        isClickable = true
        setOnTouchListener { _, event -> captureMotionEvent(event) }
        requestFocus()
    }

    fun initializeCapabilities(bits: Long) = capabilityTracker.initialize(bits)
    fun currentCapabilityBits(): Long = capabilityTracker.currentBits()
    fun penKeyCapabilities(): Set<Int> = buildSet {
        val bits = capabilityTracker.currentBits()
        if (bits and io.paddrawboard.protocol.PdbProtocol.CAP_BUTTON_1 != 0L) add(1)
        if (bits and io.paddrawboard.protocol.PdbProtocol.CAP_BUTTON_2 != 0L) add(2)
        if (bits and io.paddrawboard.protocol.PdbProtocol.CAP_BUTTON_3 != 0L) add(3)
    }
    fun releasePointers(): List<CapturedInputSample> {
        val now = android.os.SystemClock.elapsedRealtime()
        val cancelled = activePointers.values.map { it.copy(eventTimeMs = now, action = CapturedAction.CANCEL) }
        activePointers.clear(); pressedButtons = 0; lastStylusSample = null; palm.reset()
        return cancelled
    }

    fun captureMotionEvent(event: MotionEvent): Boolean = capture(event)

    // Entry point used by the activity's dispatchKeyEvent: the view only receives
    // key events when it happens to hold focus, which it often does not while
    // the surface is displaying video.
    fun handlePenKeyEvent(event: KeyEvent): Boolean {
        if (!StylusButtonMapping.isSupportedKeyEvent(event)) return false
        val button = StylusButtonMapping.fromKeyCode(event.keyCode)
        if (button == 0) return false
        return when (event.action) {
            KeyEvent.ACTION_DOWN -> {
                capabilityTracker.observeButtons(button)
                if (event.repeatCount == 0) {
                    pressedButtons = pressedButtons or button
                    android.util.Log.i(
                        "PadDrawBoard",
                        "pen button down keyCode=" + event.keyCode + " device=" + event.device?.name +
                            " mapped=" + button + " pressed=" + pressedButtons,
                    )
                    emitButtonStateSample()
                }
                true
            }
            KeyEvent.ACTION_UP -> {
                pressedButtons = pressedButtons and button.inv()
                android.util.Log.i("PadDrawBoard", "pen button up mapped=" + button)
                emitButtonStateSample()
                true
            }
            else -> true
        }
    }
    override fun onTouchEvent(event: MotionEvent): Boolean = captureMotionEvent(event)
    override fun onGenericMotionEvent(event: MotionEvent): Boolean = captureMotionEvent(event)
    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val button = if (StylusButtonMapping.isSupportedKeyEvent(event)) StylusButtonMapping.fromKeyCode(keyCode) else 0
        if (button == 0) return super.onKeyDown(keyCode, event)
        capabilityTracker.observeButtons(button)
        if (event.repeatCount == 0) {
            pressedButtons = pressedButtons or button
            android.util.Log.i(
                "PadDrawBoard",
                "pen button down keyCode=" + keyCode + " device=" + event.device?.name +
                    " mapped=" + button + " pressed=" + pressedButtons,
            )
            emitButtonStateSample()
        }
        return true
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        val button = if (StylusButtonMapping.isSupportedKeyEvent(event)) StylusButtonMapping.fromKeyCode(keyCode) else 0
        if (button == 0) return super.onKeyUp(keyCode, event)
        pressedButtons = pressedButtons and button.inv()
        emitButtonStateSample()
        return true
    }

    private fun emitButtonStateSample() {
        val previous = lastStylusSample ?: return
        val now = android.os.SystemClock.elapsedRealtime()
        val sample = previous.copy(eventTimeMs = now, action = CapturedAction.MOVE, buttons = pressedButtons)
        activePointers[sample.pointerId] = sample
        sink(listOf(sample))
    }

    private fun capture(event: MotionEvent): Boolean {
        actionOf(event.actionMasked) ?: return false
        val stylusTool = event.getToolType(0) == MotionEvent.TOOL_TYPE_STYLUS || event.getToolType(0) == MotionEvent.TOOL_TYPE_ERASER
        val isHoverAction = event.actionMasked == MotionEvent.ACTION_HOVER_ENTER ||
            event.actionMasked == MotionEvent.ACTION_HOVER_MOVE || event.actionMasked == MotionEvent.ACTION_HOVER_EXIT
        if (stylusTool) {
            capabilityTracker.observeAxes(
                event.device?.getMotionRange(MotionEvent.AXIS_PRESSURE) != null,
                event.device?.getMotionRange(MotionEvent.AXIS_TILT) != null,
                event.device?.getMotionRange(MotionEvent.AXIS_DISTANCE) != null,
            )
            if (isHoverAction) capabilityTracker.observeHoverAction()
        }
        val samples = ArrayList<CapturedInputSample>()
        val historicalAction = historicalActionFor(event.actionMasked)
        for (history in 0 until event.historySize) for (index in 0 until event.pointerCount) {
            appendSample(samples, event, index, historicalAction, event.getHistoricalEventTime(history), history)
        }
        for (index in 0 until event.pointerCount) {
            appendSample(samples, event, index, actionForPointer(event.actionMasked, index, event.actionIndex), event.eventTime, -1)
        }
        if (samples.isNotEmpty()) sink(samples)
        return true
    }
    private fun appendSample(out: MutableList<CapturedInputSample>, event: MotionEvent, index: Int, action: CapturedAction, time: Long, history: Int) {
        val tool = toolOf(event.getToolType(index))
        if (tool == CapturedTool.FINGER && palm.suppressTouch(time)) return
        if (tool != CapturedTool.FINGER) {
            if (action == CapturedAction.DOWN || action == CapturedAction.MOVE) {
                palm.stylusObserved(time)
            } else {
                // Some Xiaomi styluses never emit HOVER_EXIT. UP and hover must
                // therefore release touch suppression instead of latching it.
                palm.stylusLeftRange(time)
            }
        }
        fun axis(axis: Int): Float = if (history >= 0) event.getHistoricalAxisValue(axis, index, history) else event.getAxisValue(axis, index)
        fun coordinate(x: Boolean): Float = if (history >= 0) if (x) event.getHistoricalX(index, history) else event.getHistoricalY(index, history) else if (x) event.getX(index) else event.getY(index)
        val mappedButtons = if (tool == CapturedTool.STYLUS || tool == CapturedTool.ERASER) {
            StylusButtonMapping.fromMotionEventButtonState(event.buttonState, true) or pressedButtons
        } else 0
        capabilityTracker.observeButtons(mappedButtons)
        val sample = CapturedInputSample(time, event.getPointerId(index), tool, action, coordinate(true), coordinate(false),
            axis(MotionEvent.AXIS_PRESSURE), axis(MotionEvent.AXIS_TILT), axis(MotionEvent.AXIS_ORIENTATION),
            axis(MotionEvent.AXIS_DISTANCE), mappedButtons)
        if (tool == CapturedTool.STYLUS || tool == CapturedTool.ERASER) lastStylusSample = sample
        when (action) { CapturedAction.DOWN, CapturedAction.MOVE, CapturedAction.HOVER -> activePointers[sample.pointerId] = sample; CapturedAction.UP, CapturedAction.CANCEL -> activePointers.remove(sample.pointerId) }
        out += sample
    }
    private fun actionOf(action: Int) = when (action) {
        MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> CapturedAction.DOWN
        MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> CapturedAction.UP
        MotionEvent.ACTION_MOVE -> CapturedAction.MOVE
        MotionEvent.ACTION_HOVER_ENTER, MotionEvent.ACTION_HOVER_MOVE, MotionEvent.ACTION_HOVER_EXIT -> CapturedAction.HOVER
        MotionEvent.ACTION_CANCEL -> CapturedAction.CANCEL
        else -> null
    }
    private fun toolOf(tool: Int) = when (tool) { MotionEvent.TOOL_TYPE_STYLUS -> CapturedTool.STYLUS; MotionEvent.TOOL_TYPE_ERASER -> CapturedTool.ERASER; else -> CapturedTool.FINGER }
}

/** Historical MotionEvent records describe movement leading up to the current record. */
internal fun historicalActionFor(actionMasked: Int): CapturedAction = when (actionMasked) {
    MotionEvent.ACTION_HOVER_ENTER, MotionEvent.ACTION_HOVER_MOVE, MotionEvent.ACTION_HOVER_EXIT -> CapturedAction.HOVER
    else -> CapturedAction.MOVE
}

internal fun actionForPointer(actionMasked: Int, pointerIndex: Int, actionIndex: Int): CapturedAction = when (actionMasked) {
    MotionEvent.ACTION_DOWN -> CapturedAction.DOWN
    MotionEvent.ACTION_UP -> CapturedAction.UP
    MotionEvent.ACTION_POINTER_DOWN -> if (pointerIndex == actionIndex) CapturedAction.DOWN else CapturedAction.MOVE
    MotionEvent.ACTION_POINTER_UP -> if (pointerIndex == actionIndex) CapturedAction.UP else CapturedAction.MOVE
    MotionEvent.ACTION_MOVE -> CapturedAction.MOVE
    MotionEvent.ACTION_HOVER_ENTER, MotionEvent.ACTION_HOVER_MOVE, MotionEvent.ACTION_HOVER_EXIT -> CapturedAction.HOVER
    MotionEvent.ACTION_CANCEL -> CapturedAction.CANCEL
    else -> CapturedAction.MOVE
}
