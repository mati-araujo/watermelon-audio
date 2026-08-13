package com.watermellonstudios.audio.internal.engine

import com.watermellonstudios.audio.api.config.AudioEngineConfig
import com.watermellonstudios.audio.domain.state.EngineLifecycle
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertContains
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * WA-1.5 — los primeros tests de [AudioEngineImpl].
 *
 * **Hasta el 2026-08-13 esta clase tenía CERO cobertura, y no era olvido: era una
 * imposibilidad.** El puente entraba cableado por `getAudioBridge()`, que es `expect`
 * y cuyo actual de JVM es `AudioNativeBridge.getInstance()` — necesita la librería
 * nativa, así que la clase no se podía ni construir en un test. Recién con el puente
 * por constructor (default `getAudioBridge()`, cero call sites tocados) se volvió
 * alcanzable.
 *
 * Lo que se cubre acá es el **comportamiento observable del contrato público**: qué
 * queda en `state`, qué se le pide al puente y —sobre todo— **qué NO se le pide**.
 *
 * Lo que NO se cubre, dicho a propósito en vez de dejarlo implícito: el camino feliz
 * completo de `start()`. Arranca `startStatePolling()`, que lanza una corrutina sobre
 * `Dispatchers.Default` y sondea con `delay`; afirmar sobre eso desde `runTest` mezcla
 * tiempo virtual con un dispatcher real y da un test que depende del reloj. Es el
 * mismo problema que ya está documentado con `NSNotificationCenter` en iOS. Cubrirlo
 * pide inyectar el dispatcher, que es otro cambio de producción y no estaba en el
 * alcance acordado.
 */
class AudioEngineImplTest {

    @Test
    fun startDoesNotTouchTheEngineWhenInitializationFailed() = runTest {
        // El motor tiene que rendirse ANTES de arrancar nada si la inicialización
        // nativa falló por memoria. Es el único camino de `start()` que no es el
        // camino feliz y que no depende del polling.
        val bridge = FakeAudioNativeBridge(initializationFailed = true)
        val engine = AudioEngineImpl(AudioEngineConfig(), bridge)

        engine.start(fadeMs = 0)

        // La afirmación fuerte no es que haya error: es que NO se arrancó el motor.
        assertEquals(listOf("hasInitializationFailed"), bridge.calls)
        assertFalse(engine.isRunning)
        assertEquals(EngineLifecycle.STOPPED, engine.state.value.lifecycle)

        val error = assertNotNull(engine.state.value.error)
        assertFalse(error.isRecoverable, "quedarse sin memoria al inicializar no se reintenta")
        assertContains(error.message, "memory")
    }

    @Test
    fun aThrowingBridgeLeavesTheEngineStoppedAndReportsTheError() = runTest {
        // El `catch` de `start()` existe para que una falla del puente no deje al
        // motor diciendo STARTING para siempre. Si alguien saca el catch —o cambia
        // el estado que deja— esto se pone rojo.
        val boom = IllegalStateException("el backend no abrió")
        val bridge = FakeAudioNativeBridge(throwOnStart = boom)
        val engine = AudioEngineImpl(AudioEngineConfig(), bridge)

        engine.start(fadeMs = 0)

        assertEquals(EngineLifecycle.STOPPED, engine.state.value.lifecycle)
        assertFalse(engine.isRunning)
        val error = assertNotNull(engine.state.value.error)
        assertTrue(error.isRecoverable, "una falla al arrancar sí se puede reintentar")
        assertContains(error.message, "el backend no abrió")

        // Y llegó hasta donde tenía que llegar: preguntó, arrancó, y ahí explotó.
        assertEquals(listOf("hasInitializationFailed", "startEngineWithFadeSync"), bridge.calls)
    }

    @Test
    fun theEffectCapComesFromTheConfigAndNotFromTheDefault() = runTest {
        // WA-1.2: `AudioEngineConfig.tunedFor()` recorta `maxEffects` para gama baja,
        // y ese número tiene que llegar a `EffectChainState` — que trae 12 por su
        // cuenta. Mientras no se sembró, el recorte no se aplicaba nunca y la cadena
        // aceptaba 7 efectos con el tope en 6, medido en el AVD el 2026-07-28.
        //
        // No toca el puente: es estado de construcción, así que el doble no registra
        // una sola llamada.
        val bridge = FakeAudioNativeBridge()
        val engine = AudioEngineImpl(AudioEngineConfig(maxEffects = 3), bridge)

        assertEquals(3, engine.state.value.effectChain.maxEffects)
        assertEquals(emptyList(), bridge.calls)
        assertNull(engine.state.value.error)
    }
}
