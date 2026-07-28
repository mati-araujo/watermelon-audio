package com.watermellonstudios.audio.api

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
import kotlin.test.assertIs
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * `ModeTransitionFactory` en iOS — o sea, la mudanza probada donde importa.
 *
 * La máquina de estados se prueba en `commonTest` con un doble; lo que **sólo** se puede
 * verificar acá es lo que la mudanza destrabó: que la factory construya un handler
 * cableado al motor de verdad. Su `NativeModeStateWriter` resuelve el puente por
 * `getAudioBridge()`, que en iOS es `IosAudioBridge` sobre cinterop — el eslabón que
 * antes no existía, porque la factory vivía en `androidMain` y creaba el
 * `AudioNativeBridge` de Android.
 *
 * Antes de esto, un `ChaosPadViewModel` común recibía la función constructora
 * `(CoroutineScope) -> IModeTransitionHandler` desde afuera **porque en iOS no había
 * nadie que pudiera fabricarla**. Ahora hay.
 */
class IosModeTransitionFactoryTest {

    private val scope = CoroutineScope(SupervisorJob())

    @AfterTest
    fun cleanup() {
        scope.cancel()
    }

    /**
     * Construir el handler ejercita la cadena entera: la factory arma el
     * `NativeModeStateWriter`, que pide el puente por `getAudioBridge()`, que crea el
     * motor nativo. Si algún eslabón faltara en iOS, esto no llegaría a devolver.
     */
    @Test
    fun theFactoryBuildsAHandlerWiredToTheRealEngine() {
        val handler = ModeTransitionFactory.create(scope)

        assertNotNull(handler)
        val state = handler.transitionState.value
        assertIs<ModeTransitionState.Idle>(state, "debería arrancar en Idle, no en $state")
        assertEquals(AudioMode.CHAOS_PAD, state.currentMode)

        handler.dispose()
    }

    /** El overload con config también, que es el que usa quien quiere otros tiempos. */
    @Test
    fun theOverloadWithAnExplicitConfigAlsoWorks() {
        val handler = ModeTransitionFactory.create(
            scope = scope,
            config = ModeTransitionConfig(fadeDurationMs = 1, pollingIntervalMs = 1),
        )

        assertNotNull(handler)
        handler.dispose()
    }

    /**
     * Y una transición real contra el motor de verdad, no contra un doble.
     *
     * **No arranca el motor**, así que lo que se ejercita es el camino de Kotlin hasta el
     * puente y de vuelta: `setAudioMode` llega a `IosAudioBridge`, el sondeo lee
     * `getAudioMode()` de ahí, y la máquina de estados avanza con lo que el motor
     * conteste. Que termine `Idle` en el modo pedido prueba que el motor **aceptó** el
     * cambio — con el puente roto, esto quedaría en el timeout.
     */
    @Test
    fun aTransitionRunsAgainstTheRealBridge() = runTest {
        val handler = ModeTransitionFactory.create(
            scope = scope,
            config = ModeTransitionConfig(
                modeChangeTimeoutMs = 2_000L,
                fadeDurationMs = 1,
                pollingIntervalMs = 1,
            ),
        )

        val progress = handler.transitionTo(AudioMode.INPUT_FX).toList()

        assertTrue(progress.isNotEmpty(), "no emitió progreso")
        assertEquals(1.0f, progress.last(), "la transición no completó contra el motor real")

        val state = handler.transitionState.value
        assertIs<ModeTransitionState.Idle>(state, "quedó en $state")
        assertEquals(AudioMode.INPUT_FX, state.currentMode)

        handler.dispose()
    }
}
