package io.paddrawboard.client.input

import android.view.KeyEvent
import android.view.MotionEvent
import io.paddrawboard.protocol.PdbProtocol
import org.junit.Assert.assertEquals
import org.junit.Test

class StylusButtonMappingTest {
    @Test fun motionStylusButtonsAreTranslatedToProtocolBits() {
        assertEquals(PdbProtocol.BUTTON_1 or PdbProtocol.BUTTON_2,
            StylusButtonMapping.fromMotionEventButtonState(
                MotionEvent.BUTTON_STYLUS_PRIMARY or MotionEvent.BUTTON_STYLUS_SECONDARY, true))
        assertEquals(PdbProtocol.BUTTON_3,
            StylusButtonMapping.fromMotionEventButtonState(MotionEvent.BUTTON_TERTIARY, true))
    }

    @Test fun nonStylusMotionButtonsAreNotForwarded() {
        assertEquals(0, StylusButtonMapping.fromMotionEventButtonState(MotionEvent.BUTTON_PRIMARY or MotionEvent.BUTTON_TERTIARY, false))
    }

    @Test fun onlyDocumentedStylusKeyCodesMap() {
        assertEquals(PdbProtocol.BUTTON_1, StylusButtonMapping.fromKeyCode(KeyEvent.KEYCODE_STYLUS_BUTTON_PRIMARY))
        assertEquals(PdbProtocol.BUTTON_2, StylusButtonMapping.fromKeyCode(KeyEvent.KEYCODE_STYLUS_BUTTON_SECONDARY))
        assertEquals(PdbProtocol.BUTTON_3, StylusButtonMapping.fromKeyCode(KeyEvent.KEYCODE_STYLUS_BUTTON_TERTIARY))
        assertEquals(0, StylusButtonMapping.fromKeyCode(KeyEvent.KEYCODE_STYLUS_BUTTON_TAIL))
        assertEquals(0, StylusButtonMapping.fromKeyCode(0x7000))
    }
}
