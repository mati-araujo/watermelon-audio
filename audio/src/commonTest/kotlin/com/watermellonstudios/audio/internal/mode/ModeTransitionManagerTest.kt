package com.watermellonstudios.audio.internal.mode

import com.watermellonstudios.audio.api.IModeStateWriter
import com.watermellonstudios.audio.api.ModeTransitionConfig
import com.watermellonstudios.audio.domain.mode.AudioMode
import com.watermellonstudios.audio.domain.mode.ModeTransitionState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.test.runTest
import kotlin.test.AfterTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertTrue

/**
 * `ModeTransitionManagerImpl` — 363 líneas que hasta esta etapa **no tenían un solo
 * test**, y no por descuido: vivían en `androidMain` y construían su propio
 * `AudioNativeBridge.getInstance()`, o sea que no había forma de darles un doble.
 *
 * Bajarlas a `commonMain` no fue sólo para que iOS las tenga. Al pasar el
 * [IModeStateWriter] por constructor —seis métodos, no los ciento y pico de
 * `IAudioNativeBridge`— la máquina de estados quedó por fin aislable, y estos tests
 * corren en **las dos plataformas**. Ésa es la mitad del valor de la mudanza.
 *
 * Los tiempos van al mínimo por [fastConfig]: el flujo real espera fades de 100 ms y
 * sondea cada 16 ms, y esa espera es del contrato, no de la lógica que se quiere probar.
 */
class ModeTransitionManagerTest {

    private val scope = CoroutineScope(SupervisorJob())

    @AfterTest
    fun cleanup() {
        scope.cancel()
    }

    /**
     * Un [IModeStateWriter] de mentira que anota lo que le piden.
     *
     * Es exactamente lo que no se podía escribir antes de la mudanza.
     */
    private class FakeStateWriter(
        var failSetAudioMode: Boolean = false,
        var engineRunning: Boolean = true,
    ) : IModeStateWriter {
        var mode: Int = AudioMode.CHAOS_PAD.id
        val calls = mutableListOf<String>()

        override suspend fun setAudioMode(mode: AudioMode): Result<Unit> {
            calls += "setAudioMode(${mode.id})"
            if (failSetAudioMode) {
                return Result.failure(IllegalStateException("el motor dijo que no"))
            }
            this.mode = mode.id
            return Result.success(Unit)
        }

        override fun getAudioMode(): Int = mode

        override suspend fun pauseWithFade(fadeTimeMs: Int): Result<Unit> {
            calls += "pauseWithFade"
            return Result.success(Unit)
        }

        override suspend fun resumeWithFade(fadeTimeMs: Int): Result<Unit> {
            calls += "resumeWithFade"
            return Result.success(Unit)
        }

        override fun isEngineRunning(): Boolean = engineRunning
    }

    private fun managerOver(writer: FakeStateWriter) = ModeTransitionManagerImpl(
        stateWriter = writer,
        effectManager = null,
        scope = scope,
        config = fastConfig,
    )

    // ==================== El camino feliz ====================

    /**
     * Una transición completa avanza de 0 a 1, le pide el cambio al motor y termina
     * `Idle` en el modo destino.
     */
    @Test
    fun aCompleteTransitionEndsIdleAtTheTargetMode() = runTest {
        val writer = FakeStateWriter()
        val manager = managerOver(writer)

        val progress = manager.transitionTo(AudioMode.INPUT_FX).toList()

        assertTrue(progress.isNotEmpty(), "la transición no emitió progreso")
        assertEquals(1.0f, progress.last(), "la transición no llegó al final")
        assertTrue(
            progress.zipWithNext().all { (a, b) -> b >= a },
            "el progreso retrocedió en algún punto: $progress",
        )
        assertTrue(
            writer.calls.contains("setAudioMode(${AudioMode.INPUT_FX.id})"),
            "nunca se le pidió el cambio al motor: ${writer.calls}",
        )

        val state = manager.transitionState.value
        assertIs<ModeTransitionState.Idle>(state, "quedó en $state y no en Idle")
        assertEquals(AudioMode.INPUT_FX, state.currentMode)
    }

    /**
     * Pedir el modo en el que ya se está **no toca el motor**.
     *
     * No es una optimización: una transición de verdad hace fade out y fade in, así que
     * un pedido redundante que no cortocircuite se oye como un bache de audio.
     */
    @Test
    fun transitioningToTheCurrentModeIsANoOpThatNeverTouchesTheEngine() = runTest {
        val writer = FakeStateWriter()
        val manager = managerOver(writer)

        val progress = manager.transitionTo(AudioMode.CHAOS_PAD).toList()

        assertEquals(listOf(1.0f), progress, "un no-op no debería recorrer las fases")
        assertTrue(writer.calls.isEmpty(), "tocó el motor sin necesidad: ${writer.calls}")
    }

    // ==================== El camino de falla ====================

    /**
     * Si el motor rechaza el cambio, **el modo NO avanza**: se vuelve al de origen.
     *
     * Es el borde que importa. Quedar en el modo nuevo cuando el motor no lo adoptó deja
     * a la UI mostrando algo que no está pasando.
     *
     * ## BUG LATENTE QUE ESTE TEST DEJA FIJADO, y que NO se arregla acá
     *
     * El estado `Failed` **es inalcanzable desde afuera**. `transitionTo` lo publica y
     * enseguida `attemptRollback()` lo pisa con `Idle(origen)`; el `catch` que dejaría el
     * `Failed` en pie sólo corre si `setAudioMode` **tira**, y no tira nunca — devuelve
     * `Result.failure`. O sea que un consumidor no puede enterarse de que la transición
     * falló: ve un `Idle` en el modo viejo, indistinguible de un no-op.
     *
     * `retryLastTransition()` sigue funcionando —lee `lastFailedTransition`, que sí queda
     * puesto— así que la recuperación existe; lo que no existe es la señal.
     *
     * Se asevera el comportamiento REAL y no el deseable, a propósito: esta etapa mudó
     * código de source set y **no cambia comportamiento**. Arreglarlo es un ticket
     * aparte, y cuando llegue este test va a fallar, que es exactamente lo que tiene que
     * hacer.
     */
    @Test
    fun aRejectedModeChangeRollsBackToTheOriginalMode() = runTest {
        val writer = FakeStateWriter(failSetAudioMode = true)
        val manager = managerOver(writer)

        runCatching { manager.transitionTo(AudioMode.MIX).toList() }

        assertTrue(
            writer.calls.contains("setAudioMode(${AudioMode.MIX.id})"),
            "ni siquiera lo intentó: ${writer.calls}",
        )
        val state = manager.transitionState.value
        assertIs<ModeTransitionState.Idle>(
            state,
            "quedó en $state — si ahora es Failed, el bug latente se arregló y este " +
                "test hay que actualizarlo (ver el KDoc)",
        )
        assertEquals(
            AudioMode.CHAOS_PAD,
            state.currentMode,
            "una transición rechazada no puede dejar el modo avanzado",
        )
    }

    // ==================== Guardas ====================

    @Test
    fun canTransitionToIsFalseForTheCurrentModeAndTrueForAnother() {
        val manager = managerOver(FakeStateWriter())

        assertFalse(manager.canTransitionTo(AudioMode.CHAOS_PAD), "ya está en ese modo")
        assertTrue(manager.canTransitionTo(AudioMode.MIX))
    }

    // Acá vivían theCrossfadeIsRejectedOutsideMixMode y
    // inMixModeTheCrossfadeClampsAndDrivesBothLevels. Se fueron con
    // setCrossfadePosition en la 2.0.0, y vale decir POR QUÉ no se reemplazaron
    // por su equivalente: los dos ejercitaban el recorte y la complementariedad
    // de tres campos de un data class de Kotlin. Ninguno tocaba el motor, porque
    // no había con qué — el writer devolvía Result.success sobre un cuerpo vacío.
    // Eran verdes y no afirmaban nada sobre el audio.
    //
    // El nivel del instrumento se prueba donde sí se puede observar: en la suite
    // de C++, contra el gain que se aplica en applyEffectsAndOutput.

    /**
     * Los niveles que publica el modo siguen siendo coherentes sin el campo de
     * crossfade: MIX reparte, y los modos de una sola fuente van a los extremos.
     */
    @Test
    fun theModeLevelsStayCoherentWithoutTheCrossfadeField() = runTest {
        val manager = managerOver(FakeStateWriter())

        manager.transitionTo(AudioMode.MIX).toList()
        assertEquals(0.5f, manager.modeProperties.value.oscillatorLevel)
        assertEquals(0.5f, manager.modeProperties.value.inputLevel)

        manager.transitionTo(AudioMode.INPUT_FX).toList()
        assertEquals(0.0f, manager.modeProperties.value.oscillatorLevel)
        assertEquals(1.0f, manager.modeProperties.value.inputLevel)
    }

    private companion object {
        /**
         * Los tiempos del contrato, al mínimo. El default espera 100 ms de fade y sondea
         * cada 16, y esa espera no es lo que estos tests miden.
         */
        val fastConfig = ModeTransitionConfig(
            modeChangeTimeoutMs = 2_000L,
            fadeDurationMs = 1,
            pollingIntervalMs = 1,
        )
    }
}
