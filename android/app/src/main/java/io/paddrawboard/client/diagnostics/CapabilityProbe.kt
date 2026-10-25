package io.paddrawboard.client.diagnostics

import android.content.Context
import android.media.MediaCodecList
import android.os.Build
import android.view.InputDevice
import io.paddrawboard.client.protocol.ClientCapabilities
import java.io.File

class CapabilityProbe(private val context: Context) {
    fun inspect(width: Int, height: Int, rotation: Int, penKeys: Set<Int>): ClientCapabilities {
        val devices = ArrayList<InputDevice>()
        for (id in InputDevice.getDeviceIds()) InputDevice.getDevice(id)?.let(devices::add)
        val stylus = devices.firstOrNull { device -> device.sources and InputDevice.SOURCE_STYLUS == InputDevice.SOURCE_STYLUS }
        fun axis(axis: Int) = stylus?.getMotionRange(axis) != null
        val decoder = MediaCodecList(MediaCodecList.ALL_CODECS).codecInfos.any { !it.isEncoder && it.supportedTypes.any { type -> type.equals("video/avc", true) } }
        val touch = devices.any { device -> device.sources and InputDevice.SOURCE_TOUCHSCREEN == InputDevice.SOURCE_TOUCHSCREEN }
        // Hover is an observed event property. AXIS_DISTANCE alone is not proof that
        // the device has delivered ACTION_HOVER_* events.
        return ClientCapabilities(Build.MODEL, Build.VERSION.RELEASE, width, height, rotation, decoder,
            axis(android.view.MotionEvent.AXIS_PRESSURE), false,
            axis(android.view.MotionEvent.AXIS_TILT), axis(android.view.MotionEvent.AXIS_DISTANCE), touch,
            penKeys.filter { it in 1..3 }.toSet())
    }
    fun export(capabilities: ClientCapabilities): File = File(context.cacheDir, "paddrawboard-capabilities.txt").apply { writeText(capabilities.toString()) }
}
