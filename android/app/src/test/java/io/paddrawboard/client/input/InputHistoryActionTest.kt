package io.paddrawboard.client.input

import android.view.MotionEvent
import io.paddrawboard.client.protocol.CapturedAction
import org.junit.Assert.assertEquals
import org.junit.Test

class InputHistoryActionTest {
    @Test fun lifecycleActionsNeverLeakIntoOrdinaryHistory() {
        assertEquals(CapturedAction.MOVE, historicalActionFor(MotionEvent.ACTION_DOWN))
        assertEquals(CapturedAction.MOVE, historicalActionFor(MotionEvent.ACTION_POINTER_UP))
        assertEquals(CapturedAction.MOVE, historicalActionFor(MotionEvent.ACTION_CANCEL))
        assertEquals(CapturedAction.MOVE, historicalActionFor(MotionEvent.ACTION_MOVE))
    }

    @Test fun hoverHistoryRemainsHover() {
        assertEquals(CapturedAction.HOVER, historicalActionFor(MotionEvent.ACTION_HOVER_ENTER))
        assertEquals(CapturedAction.HOVER, historicalActionFor(MotionEvent.ACTION_HOVER_MOVE))
        assertEquals(CapturedAction.HOVER, historicalActionFor(MotionEvent.ACTION_HOVER_EXIT))
    }

    @Test fun pointerTransitionsOnlyApplyToTheActionPointer() {
        assertEquals(CapturedAction.MOVE, actionForPointer(MotionEvent.ACTION_POINTER_DOWN, 0, 1))
        assertEquals(CapturedAction.DOWN, actionForPointer(MotionEvent.ACTION_POINTER_DOWN, 1, 1))
        assertEquals(CapturedAction.MOVE, actionForPointer(MotionEvent.ACTION_POINTER_UP, 0, 1))
        assertEquals(CapturedAction.UP, actionForPointer(MotionEvent.ACTION_POINTER_UP, 1, 1))
    }
}
