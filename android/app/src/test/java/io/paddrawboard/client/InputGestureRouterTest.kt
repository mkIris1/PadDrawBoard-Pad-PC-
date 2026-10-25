package io.paddrawboard.client

import android.view.MotionEvent
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class InputGestureRouterTest {
    @Test fun contentGestureRemainsCapturedUntilUp() {
        val router = InputGestureRouter()
        assertTrue(router.shouldCapture(MotionEvent.ACTION_DOWN, 300f, 144))
        assertTrue(router.shouldCapture(MotionEvent.ACTION_MOVE, 20f, 144))
        assertTrue(router.shouldCapture(MotionEvent.ACTION_UP, 20f, 144))
        assertFalse(router.shouldCapture(MotionEvent.ACTION_MOVE, 300f, 144))
    }

    @Test fun statusGestureStaysWithStatusView() {
        val router = InputGestureRouter()
        assertFalse(router.shouldCapture(MotionEvent.ACTION_DOWN, 40f, 144))
        assertFalse(router.shouldCapture(MotionEvent.ACTION_MOVE, 300f, 144))
        assertFalse(router.shouldCapture(MotionEvent.ACTION_UP, 300f, 144))
    }

    @Test fun cancellationResetsCapture() {
        val router = InputGestureRouter()
        assertTrue(router.shouldCapture(MotionEvent.ACTION_DOWN, 300f, 144))
        assertTrue(router.shouldCapture(MotionEvent.ACTION_CANCEL, 300f, 144))
        assertFalse(router.shouldCapture(MotionEvent.ACTION_MOVE, 300f, 144))
    }
}
