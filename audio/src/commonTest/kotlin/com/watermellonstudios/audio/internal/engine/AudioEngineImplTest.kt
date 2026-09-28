package com.watermellonstudios.audio.internal.engine

import com.watermellonstudios.audio.api.config.AudioEngineConfig
import com.watermellonstudios.audio.domain.state.EngineLifecycle
import com.watermellonstudios.audio.domain.error.NativeBridgeException
import com.watermellonstudios.audio.domain.state.StreamInfo
import com.watermellonstudios.audio.internal.bridge.BridgeConcurrency
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertContains
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertSame
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
        //
        // `startEngineWithFade:in` sin su `:out` ES la afirmación: entró al bridge por
        // la variante `suspend` —la `*Sync` quedó deprecada en REQ-045, porque un `Unit`
        // no puede transportar el fallo del motor— y no volvió.
        assertEquals(listOf("hasInitializationFailed", "startEngineWithFade:in"), bridge.calls)
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

    // ================= REQ-045 S1 — el ciclo de vida que dice la verdad =================

    /**
     * **AC-045.2 — un fallo del motor NO publica `RUNNING`.**
     *
     * Era el defecto W1 del lado Kotlin (D1b): `AudioEngineImpl` llamaba
     * `startEngineWithFadeSync`, que devuelve `Unit`, así que el `WMA_ERROR_STREAM` no
     * llegaba nunca y `state.lifecycle` pasaba a `RUNNING` con el stream cerrado. Un
     * consumidor que decidiera por `isRunning` lo hacía sobre una afirmación que nadie
     * verificó.
     *
     * ¿Qué bug plausible atrapa? Justo el mutante de la spec: volver a `Result.success`
     * incondicional, o publicar `RUNNING` antes de mirar el resultado.
     */
    @Test
    fun startDoesNotPublishRunningWhenTheNativeEngineRefused() = runTest {
        val causa = NativeBridgeException.StreamError(streamErrorCode = -7)
        val bridge = FakeAudioNativeBridge(
            lifecycleResults = mapOf("startEngineWithFade" to Result.failure(causa)),
        )
        val engine = AudioEngineImpl(AudioEngineConfig(), bridge)

        val result = engine.start(fadeMs = 0)

        assertTrue(result.isFailure, "el motor dijo que no y start() devolvió éxito")
        assertSame(causa, result.exceptionOrNull(), "la causa se re-derivó en vez de transportarse")

        assertEquals(EngineLifecycle.STOPPED, engine.state.value.lifecycle)
        assertFalse(engine.isRunning, "publicó RUNNING con el stream cerrado")
        assertNotNull(engine.state.value.error)

        // Y NO siguió con la secuencia de arranque: el oscilador y los efectos por
        // default no se le piden a un motor que no arrancó. La lista completa es la
        // aserción — el doble tira ante cualquier miembro no modelado, así que un paso
        // de más también sería rojo.
        assertEquals(
            listOf("hasInitializationFailed", "startEngineWithFade:in", "startEngineWithFade:out"),
            bridge.calls,
        )
    }

    /**
     * **El gemelo**: con el motor diciendo que sí, `pause`/`resume` publican `isPaused`.
     *
     * Sin esto, todo lo de arriba lo pasaría un motor que devolviera `failure` siempre —
     * la forma tonta de "no mentir", que este repo ya aprendió a exigir en pares.
     */
    @Test
    fun pauseAndResumePublishIsPausedOnlyWhenTheEngineDidIt() = runTest {
        val bridge = FakeAudioNativeBridge(
            lifecycleResults = mapOf(
                "resumeEngineWithFade" to Result.failure(NativeBridgeException.EngineNotInitialized()),
            ),
        )
        val engine = AudioEngineImpl(AudioEngineConfig(), bridge)

        assertTrue(engine.pause(fadeMs = 0).isSuccess, "pausar contra un motor que aceptó falló")
        assertTrue(engine.isPaused, "el motor pausó y `isPaused` no lo dice")

        // Y al revés: el motor rechaza reanudar, así que `isPaused` NO puede cambiar.
        val fallo = engine.resume(fadeMs = 0)
        assertTrue(fallo.isFailure, "el motor rechazó reanudar y resume() devolvió éxito")
        assertTrue(
            engine.isPaused,
            "publicó isPaused=false con el motor todavía pausado: la mentira al revés",
        )
    }

    /**
     * **AC-045.2 — N llamadas concurrentes se ejecutan en el orden en que tomaron el mutex.**
     *
     * `start()` no es una llamada: son cinco al bridge más tres escrituras de `state`.
     * Sin serialización, dos operaciones concurrentes intercalan esa secuencia y el
     * `state` final depende de quién escriba último.
     *
     * 🔴 **El interleaving se FUERZA con una compuerta, no con iteraciones ni con
     * `sleep`.** El primer llamador se queda suspendido DENTRO del bridge con el mutex
     * tomado; `runCurrent()` después de cada `launch` garantiza que cada corrutina
     * corrió hasta su punto de suspensión —o sea, hasta encolarse en el mutex— antes de
     * que arranque la siguiente. El `Mutex` de kotlinx es FIFO, así que el orden queda
     * determinado por el de encolado, sin depender del reloj.
     *
     * Lo que afirma la lista es lo que ninguna marca sola puede: `[a:in, a:out, b:in,
     * b:out, …]`. Un `[a:in, b:in, …]` sería el intercalado, y es exactamente lo que
     * daría este test sin el mutex del motor.
     */
    @Test
    fun concurrentLifecycleCallsRunInTheOrderTheyTookTheMutex() = runTest {
        val gate = CompletableDeferred<Unit>()
        val bridge = FakeAudioNativeBridge(gate = gate)
        val engine = AudioEngineImpl(
            AudioEngineConfig(),
            bridge,
            // El dispatcher del test, para que el orden sea una propiedad del mutex y no
            // del scheduler de la máquina.
            BridgeConcurrency(dispatcher = StandardTestDispatcher(testScheduler)),
        )

        // Tres operaciones DISTINTAS, para que el orden sea legible en la lista. Las
        // tres son de la categoría LIFECYCLE, así que comparten el mutex.
        launch { engine.pause(fadeMs = 0) }
        runCurrent()
        launch { engine.resume(fadeMs = 0) }
        runCurrent()
        launch { engine.stop(fadeMs = 0) }
        runCurrent()

        // Premisa: sólo la PRIMERA llegó al bridge. Si acá hubiera tres entradas, el
        // mutex no existe y el resto del test no probaría nada.
        assertEquals(
            listOf("pauseEngineWithFade:in"),
            bridge.calls,
            "más de una operación entró al bridge con el mutex tomado: no se serializó",
        )

        gate.complete(Unit)
        advanceUntilIdle()

        assertEquals(
            listOf(
                "pauseEngineWithFade:in", "pauseEngineWithFade:out",
                "resumeEngineWithFade:in", "resumeEngineWithFade:out",
                "stopEngineWithFade:in", "stopEngineWithFade:out",
            ),
            // Sólo las marcas del ciclo de vida: lo que se afirma es el ORDEN de las
            // tres operaciones, no todo lo que `stop()` hace después de parar el motor.
            bridge.calls.filter { it.endsWith(":in") || it.endsWith(":out") },
            "las tres no corrieron completas y en el orden en que tomaron el mutex",
        )
    }

    /**
     * **AC-045.3 (D10) — `channelCount` e `isLowLatency` son medidos o AUSENTES.**
     *
     * `fromNativeArray` los rellenaba con `2` y `true` mientras el nativo le pasaba tres
     * números: dos valores **inventados en Kotlin** que un consumidor leía como medidos.
     *
     * ¿Qué bug plausible atrapa? El mutante de la spec: reponer los defaults. Con un
     * array de tres, un default los volvería no-nulos; con uno de cinco, los taparía.
     */
    @Test
    fun streamInfoReportsChannelsAndLowLatencyOnlyWhenTheNativeSideDid() {
        // Lo que el nativo mandaba ANTES de REQ-045: tres números y nada más.
        val viejo = assertNotNull(StreamInfo.fromNativeArray(floatArrayOf(44100f, 96f, 7.5f)))
        assertNull(viejo.channelCount, "con tres valores los canales son AUSENTES, no 2")
        assertNull(viejo.isLowLatency, "con tres valores el modo es AUSENTE, no true")
        assertEquals(44100, viejo.sampleRate, "y los tres de siempre siguen en su índice")

        // Lo que manda ahora, con los dos medidos. Ninguno coincide con el default que
        // se inventaba: con 2 canales y `true` un valor que viajó no se distinguiría de
        // uno fabricado.
        val medido = assertNotNull(
            StreamInfo.fromNativeArray(floatArrayOf(48000f, 192f, 4.5f, 1f, 0f)),
        )
        assertEquals(1, medido.channelCount, "los canales no viajaron")
        assertEquals(false, medido.isLowLatency, "el modo no viajó: si dice true es el default")

        // Y el tri-estado: el nativo puede decir "no sé" con el stream ABIERTO —Core
        // Audio no tiene un modo análogo al `PerformanceMode` de Oboe— y eso tiene que
        // llegar como ausencia, no como `false`.
        val desconocido = assertNotNull(
            StreamInfo.fromNativeArray(floatArrayOf(48000f, 192f, 4.5f, 2f, -1f)),
        )
        assertNull(desconocido.isLowLatency, "-1 es 'no sé' y tiene que llegar como null")
        assertEquals(2, desconocido.channelCount, "los canales sí los sabía")

        // Cero canales también es "no sé" del lado nativo, no un stream mudo.
        val sinCanales = assertNotNull(
            StreamInfo.fromNativeArray(floatArrayOf(48000f, 192f, 4.5f, 0f, 1f)),
        )
        assertNull(sinCanales.channelCount, "0 canales es ausencia, no un valor")
        assertEquals(true, sinCanales.isLowLatency)

        // Y el default del data class no puede inventar tampoco.
        assertNull(StreamInfo.EMPTY.channelCount)
        assertNull(StreamInfo.EMPTY.isLowLatency)
    }
}
