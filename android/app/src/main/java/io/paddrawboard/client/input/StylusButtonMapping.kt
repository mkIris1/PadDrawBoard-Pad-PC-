package io.paddrawboard.client.input

import android.view.KeyEvent
import android.view.MotionEvent
import io.paddrawboard.protocol.PdbProtocol

/** Maps only documented Android stylus controls to the three protocol button bits. */
object StylusButtonMapping {
    fun fromMotionEventButtonState(buttonState: Int, stylusTool: Boolean): Int {
        if (!stylusTool) return 0
        var buttons = 0
        if (buttonState and MotionEvent.BUTTON_STYLUS_PRIMARY != 0 ||
            buttonState and MotionEvent.BUTTON_PRIMARY != 0) buttons = buttons or PdbProtocol.BUTTON_1
        if (buttonState and MotionEvent.BUTTON_STYLUS_SECONDARY != 0 ||
            buttonState and MotionEvent.BUTTON_SECONDARY != 0) buttons = buttons or PdbProtocol.BUTTON_2
        // Android exposes the third generic button for stylus-class MotionEvents.
        if (buttonState and MotionEvent.BUTTON_TERTIARY != 0) buttons = buttons or PdbProtocol.BUTTON_3
        return buttons
    }

    // The Xiaomi Smart Pen (Pad 5 Pro) reports its two side buttons through a
    // Bluetooth keyboard device and sends plain PAGE_UP / PAGE_DOWN - verified
    // with `getevent -lt /dev/input/event13`. They must only be claimed when the
    // event really comes from the pen, otherwise a real keyboard's Page Up would
    // be swallowed.
    private fun isPenDevice(event: KeyEvent): Boolean {
        val name = event.device?.name ?: return false
        return name.contains("Pen", ignoreCase = true)
    }

    fun fromKeyCode(keyCode: Int): Int = when (keyCode) {
        KeyEvent.KEYCODE_STYLUS_BUTTON_PRIMARY -> PdbProtocol.BUTTON_1
        KeyEvent.KEYCODE_STYLUS_BUTTON_SECONDARY -> PdbProtocol.BUTTON_2
        KeyEvent.KEYCODE_STYLUS_BUTTON_TERTIARY -> PdbProtocol.BUTTON_3
        KeyEvent.KEYCODE_PAGE_UP -> PdbProtocol.BUTTON_1
        KeyEvent.KEYCODE_PAGE_DOWN -> PdbProtocol.BUTTON_2
        else -> 0
    }

    // The Android framework key code is the evidence. Some framework devices report
    // these documented stylus codes with a keyboard-like source, so do not require a
    // vendor-specific source bit and do not accept unknown vendor codes.
    fun isSupportedKeyEvent(event: KeyEvent): Boolean {
        if (fromKeyCode(event.keyCode) == 0) return false
        // Page Up / Page Down are generic keys: require the pen device.
        if (event.keyCode == KeyEvent.KEYCODE_PAGE_UP ||
            event.keyCode == KeyEvent.KEYCODE_PAGE_DOWN) {
            return isPenDevice(event)
        }
        return true
    }
}
