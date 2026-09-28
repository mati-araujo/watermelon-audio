package com.watermellonstudios.audio.internal.engine

import com.watermellonstudios.audio.api.config.AudioEngineConfig
import com.watermellonstudios.audio.domain.state.EngineLifecycle
import com.watermellonstudios.audio.callback.AudioAnalyticsListener
import com.watermellonstudios.audio.callback.AudioLogger
import com.watermellonstudios.audio.callback.NoOpAudioAnalytics
import com.watermellonstudios.audio.callback.NoOpAudioLogger
import com.watermellonstudios.audio.domain.error.NativeBridgeException
import com.watermellonstudios.audio.domain.state.AudioError
import com.watermellonstudios.audio.domain.state.StreamInfo
import com.watermellonstudios.audio.internal.bridge.BridgeConcurrency
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertContains
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertSame
import kotlin.test.assertIs
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

        // Cuatro operaciones DISTINTAS, para que el orden sea legible en la lista, y las
        // cuatro de la categoría LIFECYCLE, así que comparten el mutex.
        //
        // 🔴 `start()` VA PRIMERA y no es una más (M7): es la única con la secuencia de
        // cinco llamadas al bridge más tres escrituras de `state`, o sea la que de verdad
        // justifica que el mutex viva en el motor y no sólo dentro del bridge. Sin ella,
        // este test lo pasaría un motor con el mutex sólo en las operaciones de UNA
        // llamada, donde no hay nada que intercalar.
        launch { engine.start(fadeMs = 0) }
        runCurrent()
        launch { engine.pause(fadeMs = 0) }
        runCurrent()
        launch { engine.resume(fadeMs = 0) }
        runCurrent()
        launch { engine.stop(fadeMs = 0) }
        runCurrent()

        // Premisa: sólo la PRIMERA llegó al bridge. Si acá hubiera más de una entrada, el
        // mutex no existe y el resto del test no probaría nada.
        assertEquals(
            listOf("hasInitializationFailed", "startEngineWithFade:in"),
            bridge.calls,
            "más de una operación entró al bridge con el mutex tomado: no se serializó",
        )

        gate.complete(Unit)
        advanceUntilIdle()

        assertEquals(
            listOf(
                "startEngineWithFade:in", "startEngineWithFade:out",
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

    // ============ el espía de analytics, para los tests que afirman AUSENCIA ============

    /**
     * Anota los eventos que le llegan. Es lo que permite afirmar que una cancelación
     * **no** publica un error — y una ausencia sólo se puede afirmar contra algo que sí
     * registra, si no es verde por vacío.
     */
    private class AnalyticsSpy : AudioAnalyticsListener by NoOpAudioAnalytics {
        val errores = mutableListOf<AudioError>()
        val sesionesIniciadas = mutableListOf<StreamInfo>()
        override fun onError(error: AudioError) { errores += error }
        override fun onSessionStarted(streamInfo: StreamInfo) { sesionesIniciadas += streamInfo }
    }

    /**
     * Anota los `error()` que le llegan al logger. Hace falta para
     * [cancellingPauseMidFadeRethrowsInsteadOfPublishingAFailure]: el catch genérico de
     * `transicion()` no toca ni `state.error` ni `analytics.onError` — así que, a
     * diferencia de `start()`/`stop()`, esas dos ausencias son verdes con o sin el
     * mutante (`job.cancel()` deja al job en Cancelled de cualquier forma, lo trague o
     * no el catch, porque `withContext` fuerza la cancelación al completar sobre un Job
     * ya cancelado). Lo único que SÍ cambia es si se ejecuta el `logger.error("$que
     * audio lanzó", e)` del catch genérico: con el catch correcto nunca se llega ahí.
     */
    private class LoggerSpy : AudioLogger by NoOpAudioLogger {
        val errores = mutableListOf<Pair<String, Throwable?>>()
        override fun error(tag: String, message: String, throwable: Throwable?, params: Map<String, Any>) {
            errores += message to throwable
        }
    }

    /**
     * **1.10 — cancelar `start()` NO es fallar, y no puede publicarse como si lo fuera.**
     *
     * `CancellationException` es una `Exception`, así que el `catch (e: Exception)` de
     * `startLocked` se la comía **antes** de que `BridgeConcurrency.guarded` pudiera
     * relanzarla: es el bug exacto que el KDoc de `guarded` dice que WA-1.4 vino a
     * arreglar para los 22 `catch` del bridge, reintroducido un nivel más arriba.
     *
     * ¿Qué bug plausible atrapa? El medido: se cancela `start()` durante el `delay(fade)`
     * —el motor nativo YA arrancó— y el catch publicaba `STOPPED` + `analytics.onError`
     * sobre un motor sonando. El poller volvía a escribir `RUNNING` un tick después: un
     * parpadeo en el estado del consumidor y una métrica de error que no describe ningún
     * error. Es el ENG-14 de NoisyPad.
     *
     * 🔴 La cancelación se dispara **a mitad del fade y por la compuerta del scheduler**,
     * no con un `sleep`: `advanceTimeBy` mueve el reloj virtual la mitad del fade, ahí se
     * cancela, y `advanceUntilIdle` deja que todo termine.
     */
    @Test
    fun cancellingStartMidFadeRethrowsInsteadOfPublishingAFailure() = runTest {
        val spy = AnalyticsSpy()
        val bridge = FakeAudioNativeBridge()
        val engine = AudioEngineImpl(
            AudioEngineConfig(analyticsListener = spy),
            bridge,
            BridgeConcurrency(dispatcher = StandardTestDispatcher(testScheduler)),
        )

        val trabajo = launch { engine.start(fadeMs = 400) }
        // Hasta la mitad del fade: el motor nativo ya arrancó y `start()` está en el delay.
        advanceTimeBy(200)
        assertTrue(
            bridge.calls.contains("startEngineWithFade:out"),
            "premisa: el motor nativo tiene que estar YA arrancado cuando se cancela, " +
                "si no esto no prueba nada (calls=${bridge.calls})",
        )

        trabajo.cancel()
        advanceUntilIdle()

        assertTrue(trabajo.isCancelled, "la cancelación no llegó al job: se la tragó alguien")

        // Lo que NO tiene que haber pasado. Las tres aserciones son la propiedad.
        assertEquals(
            emptyList(),
            spy.errores,
            "publicó un error de analytics por una CANCELACIÓN: eso no es un fallo del motor",
        )
        assertNotEquals(
            EngineLifecycle.STOPPED,
            engine.state.value.lifecycle,
            "publicó STOPPED sobre un motor que quedó arrancado — el parpadeo del ENG-14",
        )
        assertNull(engine.state.value.error, "publicó un AudioError por una cancelación")
    }

    /**
     * **1.10 — el mismo bug, en `stopLocked`.**
     *
     * El `catch (e: CancellationException)` de acá relanza, pero el mutante que lo
     * cambia por `ArithmeticException` SOBREVIVÍA: nada ejercitaba una cancelación a
     * mitad del fade de `stop()`. Es el mismo defecto que
     * [cancellingStartMidFadeRethrowsInsteadOfPublishingAFailure], del lado del apagado
     * — y ahí el motor YA paró (el bridge dijo que sí) cuando se cancela, así que
     * publicar `STOPPED`/un error de analytics sería la misma mentira sobre un motor
     * que en este caso SÍ terminó de parar, sólo que el `stop()` que lo pidió nunca se
     * enteró.
     */
    @Test
    fun cancellingStopMidFadeRethrowsInsteadOfPublishingAFailure() = runTest {
        val spy = AnalyticsSpy()
        val bridge = FakeAudioNativeBridge()
        val engine = AudioEngineImpl(
            AudioEngineConfig(analyticsListener = spy),
            bridge,
            BridgeConcurrency(dispatcher = StandardTestDispatcher(testScheduler)),
        )
        assertTrue(
            engine.start(fadeMs = 0).isSuccess,
            "premisa: el motor tiene que estar arrancado antes de poder cancelar su stop",
        )

        val trabajo = launch { engine.stop(fadeMs = 400) }
        // Hasta la mitad del fade: el bridge ya dijo que paró y `stop()` está en el delay.
        advanceTimeBy(200)
        assertTrue(
            bridge.calls.contains("stopEngineWithFade:out"),
            "premisa: el motor nativo tiene que haber dicho YA que paró cuando se " +
                "cancela, si no esto no prueba nada (calls=${bridge.calls})",
        )

        trabajo.cancel()
        advanceUntilIdle()

        assertTrue(trabajo.isCancelled, "la cancelación no llegó al job: se la tragó alguien")
        assertEquals(
            emptyList(),
            spy.errores,
            "publicó un error de analytics por una CANCELACIÓN: eso no es un fallo del motor",
        )
        assertNull(engine.state.value.error, "publicó un AudioError por una cancelación")
    }

    /**
     * **1.10 — el mismo bug, en `transicion` (pause/resume).**
     *
     * Mismo `catch` compartido por `pause()` y `resume()`. Se cubre `pause()`, que es
     * el camino que un consumidor real cancela más seguido (la UI se va a background
     * a mitad de un fade de pausa).
     *
     * 🔴 Acá NO alcanza con `spy.errores`/`state.error`: el catch genérico de
     * `transicion()` no los toca (a diferencia del de `start`/`stop`), así que esas dos
     * ausencias son verdes CON o SIN el mutante — medido: `trabajo.isCancelled` también
     * da `true` en los dos casos, porque `withContext` fuerza la cancelación al
     * completar sobre un job que ya la pidió, la trague o no el catch de adentro. Lo
     * único que sí cambia es si se llega a ejecutar el `logger.error(...)` del catch
     * genérico — con el catch correcto nunca se llega ahí — y eso es lo que afirma
     * [LoggerSpy].
     */
    @Test
    fun cancellingPauseMidFadeRethrowsInsteadOfPublishingAFailure() = runTest {
        val spy = AnalyticsSpy()
        val loggerSpy = LoggerSpy()
        val bridge = FakeAudioNativeBridge()
        val engine = AudioEngineImpl(
            AudioEngineConfig(analyticsListener = spy, logger = loggerSpy),
            bridge,
            BridgeConcurrency(dispatcher = StandardTestDispatcher(testScheduler)),
        )
        assertTrue(
            engine.start(fadeMs = 0).isSuccess,
            "premisa: el motor tiene que estar arrancado antes de poder pausarlo",
        )
        loggerSpy.errores.clear() // limpiar lo que haya logueado el arranque

        val trabajo = launch { engine.pause(fadeMs = 400) }
        // Hasta la mitad del fade: el bridge ya dijo que sí y `pause()` está en el delay.
        advanceTimeBy(200)
        assertTrue(
            bridge.calls.contains("pauseEngineWithFade:out"),
            "premisa: el motor nativo tiene que haber contestado YA cuando se cancela, " +
                "si no esto no prueba nada (calls=${bridge.calls})",
        )

        trabajo.cancel()
        advanceUntilIdle()

        assertTrue(trabajo.isCancelled, "la cancelación no llegó al job: se la tragó alguien")
        assertEquals(
            emptyList(),
            spy.errores,
            "publicó un error de analytics por una CANCELACIÓN: eso no es un fallo del motor",
        )
        assertNull(engine.state.value.error, "publicó un AudioError por una cancelación")
        assertTrue(
            loggerSpy.errores.none { (_, throwable) -> throwable is kotlinx.coroutines.CancellationException },
            "el catch genérico logueó la cancelación como si fuera un fallo del motor " +
                "(loggerSpy.errores=${loggerSpy.errores})",
        )
    }

    /**
     * **M4 — el gemelo del `stop()` que falla.**
     *
     * El motor dice que no paró: `Result.failure`, error publicado, y el `lifecycle` lo
     * sigue escribiendo el **poller** con lo que el nativo reporta. Eso es lo honesto —
     * publicar `STOPPED` sería la mentira al revés, y apagar el poller dejaría el estado
     * congelado en lo último que alcanzó a escribir.
     *
     * ¿Qué bug plausible atrapa? Que alguien "arregle" esto publicando `STOPPED` en el
     * camino de fallo, o que apague el polling antes de saber si el motor paró. La
     * primera version de este test sólo miraba la firma del fallo: nunca arrancaba el
     * motor, así que el poller no existía y `stopStatePolling()` era un no-op — el
     * mutante que agrega un `stopStatePolling()` de más antes del `return
     * Result.failure(causa)` (M4) SOBREVIVÍA porque no había nada que apagar. Acá el
     * poller EXISTE (el motor arranca primero) y se observa la propiedad que el
     * comentario de producción promete: el `lifecycle` publicado sigue cambiando con lo
     * que el nativo reporta DESPUÉS del stop fallido.
     *
     * 🔴 Va con `runBlocking` y **espera por CONDICIÓN con techo**, misma razón que
     * [theLatencyThatArrivesLateIsPickedUpByThePoller]: el poller corre en el scope
     * propio del motor, sobre `Dispatchers.Default`, y ése no se inyecta — un `delay`
     * fijo y afirmar después dependería del reloj real de la máquina que corre el test.
     */
    @Test
    fun aFailedStopKeepsThePollerSoTheStateFollowsTheEngine() = runBlocking {
        val causa = NativeBridgeException.InvalidOperation("stop", "Running")
        val bridge = FakeAudioNativeBridge(
            lifecycleResults = mapOf("stopEngineWithFade" to Result.failure(causa)),
            // Latencia YA presente desde la primera lectura: así el poller no vuelve a
            // llamar `getStreamInfoArray()` en cada tick (esa es la propiedad de 1.11,
            // no la de este test) y el tramo de `calls` posterior al arranque queda
            // limpio para afirmar exactamente lo que pasó durante el stop.
            streamInfoReadings = listOf(floatArrayOf(48000f, 240f, 8.5f, 2f, 1f)),
        )
        val engine = AudioEngineImpl(AudioEngineConfig(), bridge)

        try {
            assertTrue(
                engine.start(fadeMs = 0).isSuccess,
                "premisa: el motor tiene que estar ARRANCADO, si no el poller no existe " +
                    "y apagarlo es un no-op — que es justo lo que dejaba sobrevivir al mutante",
            )
            val llamadasAlArrancar = bridge.calls.size

            val result = engine.stop(fadeMs = 0)

            assertTrue(result.isFailure, "el motor dijo que no paró y stop() devolvió éxito")
            assertSame(causa, result.exceptionOrNull(), "la causa se re-derivó en vez de transportarse")
            assertNotNull(engine.state.value.error, "un stop que falló tiene que dejar el error a la vista")
            assertNotEquals(
                EngineLifecycle.STOPPED,
                engine.state.value.lifecycle,
                "publicó STOPPED sobre un motor que dijo que no paró",
            )
            // Y NO siguió con el resto del apagado: el doble tira ante lo no modelado, así que
            // un paso de más sería rojo. Lo que se ve, sobre el TRAMO posterior al arranque,
            // es la llamada de stop y nada después.
            assertEquals(
                listOf("stopEngineWithFade:in", "stopEngineWithFade:out"),
                bridge.calls.drop(llamadasAlArrancar),
                "siguió apagando cosas después de que el motor dijo que no paró " +
                    "(calls=${bridge.calls})",
            )

            // El poller SIGUE VIVO y el lifecycle publicado sigue al motor: cambia lo que
            // el nativo reporta y esperamos por CONDICIÓN a que el poller lo levante.
            //
            // 🔴 El código NO puede ser `3` (STOPPING): `stopLocked` YA publicó
            // STOPPING de forma directa, antes de saber si el bridge iba a fallar —
            // así que `engine.state.first { it.lifecycle == STOPPING }` daría VERDE
            // de inmediato sin que el poller hiciera nada, midiendo cero. `1`
            // (STARTING) no lo escribe nadie más en este punto: la única forma de que
            // vuelva a aparecer es que el poller vuelva a preguntarle al nativo.
            bridge.nativeEngineState = 1 // STARTING en EngineLifecycle.fromNativeCode
            val siguioAlMotor = withTimeoutOrNull(5_000) {
                engine.state.first { it.lifecycle == EngineLifecycle.STARTING }
            }
            // `assertTrue`, no `assertNotNull`: acá es la ÚLTIMA expresión del bloque
            // `try`, y `assertNotNull` devuelve el valor no-nulo — el tipo del `try`
            // (y por lo tanto el de `runBlocking`) dejaría de ser `Unit`, y JUnit
            // rechaza un método de test que no devuelve `void`.
            assertTrue(
                siguioAlMotor != null,
                "el lifecycle publicado nunca reflejó el nuevo estado del nativo: el " +
                    "poller se apagó (o murió) después de un stop fallido",
            )
        } finally {
            engine.release()
        }
    }

    /**
     * **1.11 — la latencia llega TARDE, y el poller la va a buscar.**
     *
     * Medido en el moto g42: en el camino Oboe directo —el que shippea en Android—
     * `calculateLatencyMillis()` no tiene dato justo después de `requestStart()`, así que
     * la única lectura que hacía `start()` traía `-1` y nadie volvía a preguntar. El
     * consumidor veía `StreamInfo(sampleRate=48000, bufferSizeInFrames=240,
     * channelCount=2, latencyMillis=-1.0, isLowLatency=true)` para toda la sesión, y ese
     * `-1` llegaba a `analytics.onSessionStarted`.
     *
     * ¿Qué bug plausible atrapa? Los dos: que un `-1` se publique como latencia, y que se
     * lea una sola vez y quede ausente para siempre.
     *
     * 🔴 Va con `runBlocking` y **espera por CONDICIÓN con techo**, no con un `sleep`: el
     * poller corre en el scope propio del motor, sobre `Dispatchers.Default`, y ése no se
     * inyecta. Un techo agotado es rojo, no un salteo.
     */
    @Test
    fun theLatencyThatArrivesLateIsPickedUpByThePoller() = runBlocking {
        val spy = AnalyticsSpy()
        val sinLatencia = floatArrayOf(48000f, 240f, -1f, 2f, 1f)
        val conLatencia = floatArrayOf(48000f, 240f, 8.5f, 2f, 1f)
        val bridge = FakeAudioNativeBridge(
            streamInfoReadings = listOf(sinLatencia, sinLatencia, conLatencia),
        )
        val engine = AudioEngineImpl(AudioEngineConfig(analyticsListener = spy), bridge)

        try {
            assertTrue(engine.start(fadeMs = 0).isSuccess, "el motor aceptó y start() falló")

            // Lo primero que se publica: la latencia AUSENTE, nunca -1.0.
            val primera = assertNotNull(engine.state.value.streamInfo)
            assertNull(
                primera.latencyMillis,
                "publicó ${primera.latencyMillis} como latencia: un negativo no es una " +
                    "medición, es la ausencia de una",
            )
            assertEquals(48000, primera.sampleRate, "y el resto del stream info sí viajó")
            assertEquals(
                listOf(null),
                spy.sesionesIniciadas.map { it.latencyMillis },
                "onSessionStarted recibió un -1 disfrazado de latencia",
            )

            // Y el poller vuelve a preguntar hasta que llega. Espera por CONDICIÓN.
            val llegada = withTimeoutOrNull(5_000) {
                engine.state.first { it.streamInfo?.latencyMillis != null }
            }
            assertNotNull(
                llegada,
                "la latencia nunca llegó: el poller no vuelve a preguntar, así que un -1 " +
                    "inicial queda ausente para toda la sesión (lecturas=${bridge.streamInfoReads})",
            )
            assertEquals(8.5, llegada.streamInfo?.latencyMillis)
        } finally {
            engine.release()
        }
    }

    /**
     * **B8 — después de `release()`, nadie puede decir que sí.**
     *
     * `release()` destruye el motor nativo y **no puede tomar el mutex** (es
     * `override fun`, no `suspend`, en la superficie pública). Sin una marca, las cuatro
     * del ciclo de vida seguían devolviendo `success` sobre un motor destruido.
     *
     * ¿Qué bug plausible atrapa? El uso-después-de-liberar más común de una librería de
     * audio: la UI se va, alguien libera, y un callback tardío llama `start()`.
     */
    @Test
    fun afterReleaseEveryLifecycleCallIsARejectionWithATypedCause() = runTest {
        val bridge = FakeAudioNativeBridge()
        val engine = AudioEngineImpl(AudioEngineConfig(), bridge)

        engine.release()
        val despues = bridge.calls.size

        for ((que, llamada) in listOf<Pair<String, suspend () -> Result<Unit>>>(
            "start" to { engine.start(fadeMs = 0) },
            "stop" to { engine.stop(fadeMs = 0) },
            "pause" to { engine.pause(fadeMs = 0) },
            "resume" to { engine.resume(fadeMs = 0) },
        )) {
            val r = llamada()
            assertTrue(r.isFailure, "$que devolvió éxito sobre un motor ya liberado")
            val causa = assertIs<NativeBridgeException.InvalidOperation>(
                r.exceptionOrNull(),
                "$que falló pero sin causa tipada de 'motor liberado': ${r.exceptionOrNull()}",
            )
            assertEquals("RELEASED", causa.currentState, "la causa no nombra el estado real")
        }

        // Y no tocó el motor NI UNA vez: el rechazo es antes del bridge.
        assertEquals(
            despues,
            bridge.calls.size,
            "le pidió algo al motor después de liberarlo (calls=${bridge.calls})",
        )
    }

    /**
     * **B8, la mitad difícil — un `start()` EN VUELO que termina después de `release()`.**
     *
     * El rechazo de la entrada no alcanza: una operación que ya tomó el mutex antes del
     * release no lo ve al entrar. Tiene que volver a mirar antes de devolver `success`,
     * porque a esa altura el motor que iba a afirmar que está andando ya no existe.
     */
    @Test
    fun aStartInFlightWhenReleaseHappensCannotReportSuccess() = runTest {
        val gate = CompletableDeferred<Unit>()
        val bridge = FakeAudioNativeBridge(gate = gate)
        val engine = AudioEngineImpl(
            AudioEngineConfig(),
            bridge,
            BridgeConcurrency(dispatcher = StandardTestDispatcher(testScheduler)),
        )

        var resultado: Result<Unit>? = null
        launch { resultado = engine.start(fadeMs = 0) }
        runCurrent()
        assertEquals(
            listOf("hasInitializationFailed", "startEngineWithFade:in"),
            bridge.calls,
            "premisa: el start tiene que estar EN VUELO y adentro del bridge",
        )

        engine.release()
        gate.complete(Unit)
        advanceUntilIdle()

        val r = assertNotNull(resultado, "el start nunca terminó")
        assertTrue(
            r.isFailure,
            "un start que terminó DESPUÉS del release devolvió éxito: estaría afirmando que " +
                "un motor destruido está andando",
        )
    }

    /**
     * **B8, la mitad difícil — el gemelo de `aStartInFlightWhenReleaseHappensCannotReportSuccess`
     * para `pause()`.**
     *
     * El chequeo post-fade de `transicion()` —
     * `if (released) return liberado(if (pausado) "pause" else "resume")` — es el que
     * borra este mutante. `pause()` no necesita el motor arrancado para tener algo que
     * verificar: `transicion()` llama al bridge sin mirar `isRunning`, así que alcanza
     * con dejarla EN VUELO y liberar mientras espera adentro.
     */
    @Test
    fun aPauseInFlightWhenReleaseHappensCannotReportSuccess() = runTest {
        val gate = CompletableDeferred<Unit>()
        val bridge = FakeAudioNativeBridge(gate = gate)
        val engine = AudioEngineImpl(
            AudioEngineConfig(),
            bridge,
            BridgeConcurrency(dispatcher = StandardTestDispatcher(testScheduler)),
        )

        var resultado: Result<Unit>? = null
        launch { resultado = engine.pause(fadeMs = 0) }
        runCurrent()
        assertEquals(
            listOf("pauseEngineWithFade:in"),
            bridge.calls,
            "premisa: el pause tiene que estar EN VUELO y adentro del bridge",
        )

        engine.release()
        gate.complete(Unit)
        advanceUntilIdle()

        val r = assertNotNull(resultado, "el pause nunca terminó")
        assertTrue(
            r.isFailure,
            "un pause que terminó DESPUÉS del release devolvió éxito: estaría afirmando " +
                "que un motor destruido quedó pausado",
        )
    }

    /**
     * **B8, la mitad difícil — el mismo gemelo para `stop()`.**
     *
     * El chequeo post-fade de `stopLocked` —`if (released) return liberado("stop")`—
     * ni siquiera estaba mutado en la tanda anterior: no había ningún test que dejara
     * un `stop()` en vuelo cruzándose con un `release()`.
     */
    @Test
    fun aStopInFlightWhenReleaseHappensCannotReportSuccess() = runTest {
        val gate = CompletableDeferred<Unit>()
        val bridge = FakeAudioNativeBridge(gate = gate)
        val engine = AudioEngineImpl(
            AudioEngineConfig(),
            bridge,
            BridgeConcurrency(dispatcher = StandardTestDispatcher(testScheduler)),
        )

        var resultado: Result<Unit>? = null
        launch { resultado = engine.stop(fadeMs = 0) }
        runCurrent()
        assertEquals(
            listOf("stopEngineWithFade:in"),
            bridge.calls,
            "premisa: el stop tiene que estar EN VUELO y adentro del bridge",
        )

        engine.release()
        gate.complete(Unit)
        advanceUntilIdle()

        val r = assertNotNull(resultado, "el stop nunca terminó")
        assertTrue(
            r.isFailure,
            "un stop que terminó DESPUÉS del release devolvió éxito: estaría afirmando " +
                "que un motor destruido paró",
        )
    }
}
