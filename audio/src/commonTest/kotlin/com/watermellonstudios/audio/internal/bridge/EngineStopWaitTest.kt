package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.domain.error.NativeBridgeException
import kotlinx.coroutines.test.currentTime
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertTrue

/**
 * REQ-050 S2 (D8, D11) — **la variante `suspend` de parar devuelve éxito recién con el motor
 * nativo en Stopped**, y con techo.
 *
 * El nativo para un fade + ~60 ms DESPUÉS de que `stopWithFade` devolvió (un worker duerme el
 * fade). Hasta acá el `success` de `stopEngineWithFade` significaba "el motor aceptó": quien
 * reconfiguraba enseguida (`setUseBackendManager`) lo encontraba todavía corriendo, y el nativo
 * lo rechazaba con un LOGE y nada más (MINI-041 #4).
 *
 * Corre con tiempo VIRTUAL (`runTest`): ningún test duerme el techo.
 */
class EngineStopWaitTest {

    private class FakeEngineState(private val stoppedAfterReads: Int) {
        var reads = 0
        fun read(): Int = if (++reads > stoppedAfterReads) STOPPED else RUNNING
    }

    /**
     * AC-050.6. El motor llega a Stopped después de unas lecturas: éxito, y no antes.
     *
     * Bug que atrapa: devolver éxito sin preguntar (el contrato de "aceptó").
     */
    @Test
    fun `AC-050_6 espera a que el motor quede Stopped antes de devolver exito`() = runTest {
        val engine = FakeEngineState(stoppedAfterReads = 5)

        val result = EngineStopWait.awaitStopped("stopEngineWithFade", 1_000L, engine::read)

        assertTrue(result.isSuccess, "resultado: $result")
        assertEquals(6, engine.reads, "devolvió antes de ver el Stopped")
    }

    /**
     * AC-050.6, el techo. Un motor que nunca para es `Timeout`, nunca éxito ni una espera eterna.
     *
     * Bug que atrapa: afirmar éxito al vencer el techo, o esperar sin techo (el LIFECYCLE quedaría
     * tomado para siempre).
     */
    @Test
    fun `AC-050_6 si el motor no para dentro del techo devuelve Timeout`() = runTest {
        val engine = FakeEngineState(stoppedAfterReads = Int.MAX_VALUE)

        val result = EngineStopWait.awaitStopped("stopEngineWithFade", 300L, engine::read)

        val error = assertIs<NativeBridgeException.Timeout>(result.exceptionOrNull(), "resultado: $result")
        assertEquals(300L, error.timeoutMs)
        assertTrue(currentTime in 300L..400L, "el techo no se respetó: venció a los $currentTime ms virtuales")
    }

    /** El gemelo: un motor ya parado devuelve éxito sin esperar. */
    @Test
    fun `AC-050_6 gemelo - con el motor ya parado devuelve exito de inmediato`() = runTest {
        val engine = FakeEngineState(stoppedAfterReads = 0)

        assertTrue(EngineStopWait.awaitStopped("stopEngine", 1_000L, engine::read).isSuccess)
        assertEquals(0L, currentTime)
    }

    /** El techo cubre el fade más lo que el nativo tarda en cerrar. */
    @Test
    fun `AC-050_6 el techo es el fade mas el margen de cierre`() {
        assertEquals(500L + EngineStopWait.SETTLE_MARGIN_MS, EngineStopWait.ceilingFor(500))
        assertEquals(EngineStopWait.SETTLE_MARGIN_MS, EngineStopWait.ceilingFor(-1))
    }

    private companion object {
        const val STOPPED = 0
        const val RUNNING = 2
    }
}
