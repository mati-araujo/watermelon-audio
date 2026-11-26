package com.watermellonstudios.audio.harness.smoke

import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/** MINI-038 — esperar el Stopped NATIVO antes de tocar USB (medido en el g42: stop() vuelve antes del fade). */
class StopEngineAndWaitTest {

    private val running = 2

    /** Un motor nativo que pasa a Stopped después de [ticks] lecturas (null = nunca). */
    private class FakeNative(val ticks: Int?) {
        var reads = 0
        fun state(): Int {
            reads++
            return if (ticks != null && reads > ticks) ENGINE_STATE_STOPPED else 2
        }
    }

    @Test
    fun theNativeEngineReachesStoppedAfterSomeTicks() = runTest {
        val native = FakeNative(ticks = 3)
        var stops = 0
        var kotlin = true
        val r = stopEngineAndWait({ kotlin }, { stops++; kotlin = false; Result.success(Unit) }, native::state, pause = {})
        assertTrue(r.stopped)
        assertEquals(1, stops)
        assertTrue(r.waitedMs in STOP_POLL_MS..(3 * STOP_POLL_MS))
    }

    /** Bug que atrapa: reportar "parado" porque stop() devolvió éxito, con el nativo corriendo. */
    @Test
    fun aNativeEngineThatNeverStopsIsNotStoppedAndTheWaitIsBounded() = runTest {
        val native = FakeNative(ticks = null)
        var kotlin = true
        val r = stopEngineAndWait({ kotlin }, { kotlin = false; Result.success(Unit) }, native::state, pause = {})
        assertFalse(r.stopped)
        assertEquals(STOP_DEADLINE_MS, r.waitedMs)
        assertEquals(running, r.nativeState)
    }

    /**
     * Bug que atrapa: Kotlin dice parado y el nativo sigue en Running (fade en curso). Un segundo
     * stop() a mitad del fade lo cancela y reinicia a volumen pleno: no se llama, se espera.
     */
    @Test
    fun kotlinStoppedWithTheNativeFadingWaitsWithoutASecondStop() = runTest {
        val native = FakeNative(ticks = 2)
        var stops = 0
        val r = stopEngineAndWait({ false }, { stops++; Result.success(Unit) }, native::state, pause = {})
        assertTrue(r.stopped)
        assertEquals(0, stops)
    }

    /** Si el fade no termina en el plazo, recién ahí se insiste con stop(), y el resultado dice la verdad. */
    @Test
    fun aFadeThatNeverEndsGetsOneStopAfterTheDeadline() = runTest {
        val native = FakeNative(ticks = null)
        var stops = 0
        val r = stopEngineAndWait({ false }, { stops++; Result.success(Unit) }, native::state, pause = {})
        assertFalse(r.stopped)
        assertEquals(1, stops)
        assertEquals(2 * STOP_DEADLINE_MS, r.waitedMs)
    }
}
