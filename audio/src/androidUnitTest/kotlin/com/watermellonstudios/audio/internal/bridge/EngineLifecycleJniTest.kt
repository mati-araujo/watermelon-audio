package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.domain.error.NativeBridgeException
import com.watermellonstudios.audio.domain.state.StreamInfo
import kotlinx.coroutines.runBlocking
import org.junit.AfterClass
import org.junit.FixMethodOrder
import org.junit.runners.MethodSorters
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * REQ-045 S1 — **el ciclo de vida que dice la verdad**, ejecutado contra un `JNIEnv` real.
 *
 * Las seis entradas del ciclo de vida descartaban el `WmaResult` de la C API y Kotlin
 * devolvía `Result.success(Unit)` **incondicional** (D1 del análisis de las 25 cartas de
 * NoisyPad). O sea: el motor decía que no y el consumidor leía un sí. iOS ya lo propagaba,
 * así que ni siquiera era un contrato coherente entre plataformas.
 *
 * ## Qué agarra esto que ningún gate puede
 *
 * `scripts/check-jni-results.py` (AC-045.9) agarra la FORMA —una `wma_*` que devuelve
 * `WmaResult` escrita como sentencia suelta— y eso es lo que impide reincidir. Lo que no
 * puede ver es si el código que viaja **llega y se traduce bien**: que un
 * `WMA_ERROR_STREAM` termine en `NativeBridgeException.StreamError` y no en un
 * `NativeError(-7)` genérico, y que el camino de éxito siga diciendo sí. Eso sólo se sabe
 * ejecutando la frontera.
 *
 * ## Los dos estímulos, y por qué hay DOS
 *
 * - **Sin motor**: `stop/pause/resume` no llaman a `ensureEngine()`, así que en una JVM
 *   virgen la C API contesta `WMA_ERROR_NOT_INITIALIZED`. Es el código que hoy se tiraba.
 * - **Con motor y el backend rechazando el stream**: [HostTestHooks.setStartFails] acciona
 *   el `FakeAudioBackend` que `test_platform_backends.cpp` le entrega al `BackendManager`,
 *   así que `AudioEngine::start()` devuelve false y la C API `WMA_ERROR_STREAM`. Es
 *   exactamente el escenario de la carta: *"arrancar el motor devuelve `Result.success`
 *   aunque el stream no abra"*.
 *
 * Y va con su **gemelo**: "no mentir" es trivial para un motor que dice que no a todo, así
 * que el camino de éxito se afirma igual de fuerte (`c -`).
 *
 * ## El orden importa, y por eso está fijado
 *
 * El motor nativo es un singleton de proceso: el estado "todavía no hay motor" existe UNA
 * sola vez por JVM. [FixMethodOrder] deja el test de ausencia primero y su mensaje de fallo
 * nombra la premisa — si otra clase arrancara el motor antes, esto se pone rojo diciendo eso
 * en vez de seguir en verde probando la mitad. Cada clase del arnés tiene su JVM
 * (`forkEvery = 1`), que existe por esto.
 *
 * 🔴 Backend FALSO adentro: valida la frontera JNI/Kotlin, **no** audio en dispositivo. Ver
 * el KDoc de [JniHarness].
 */
@FixMethodOrder(MethodSorters.NAME_ASCENDING)
class EngineLifecycleJniTest {

    companion object {
        private const val OWNER = "EngineLifecycleJniTest"

        /**
         * Fade CERO en todas las llamadas, y no es cosmético: con `fadeTimeMs > 0`,
         * `AudioEngine::stopWithFade` levanta un thread que duerme el fade y **después**
         * para. El test quedaría afirmando contra un motor que todavía no paró, y el
         * arreglo sería una espera — justo lo que REQ-002 prohíbe. Con 0 el camino es
         * sincrónico y no hay nada que esperar.
         */
        private const val NO_FADE = 0

        /**
         * Canales y modo que el "device" va a reportar, los dos DISTINTOS del default que
         * Kotlin inventaba (`channelCount = 2`, `isLowLatency = true`). Con los defaults,
         * un valor que VIAJÓ no se distingue de uno fabricado en Kotlin: los dos leerían
         * lo mismo. Ese era el defecto D10.
         */
        private const val CANALES = 1
        private const val BAJA_LATENCIA = false

        private val COVERED = setOf(
            "nativeStartEngine",
            "nativeStopEngine",
            "nativeStartEngineWithFade",
            "nativeStopEngineWithFade",
            "nativePauseEngineWithFade",
            "nativeResumeEngineWithFade",
            "nativeGetStreamInfo",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    private fun <T> jni(name: String, call: suspend (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name) { b -> runBlocking { call(b) } }

    /** La causa tipada de un `Result.failure`, o un fallo que la nombra. */
    private inline fun <reified E : NativeBridgeException> causa(
        resultado: Result<Unit>,
        que: String,
    ): E {
        val error = resultado.exceptionOrNull()
        assertNotNull(error, "$que devolvió ÉXITO: eso es exactamente el defecto D1.")
        return assertIs<E>(error, "$que falló, pero con la causa equivocada: $error")
    }

    /**
     * **AC-045.1 — sin motor, parar/pausar/reanudar dicen que no.**
     *
     * Las cuatro pasan el handle sin `ensureEngine()`, así que en una JVM virgen la C API
     * contesta `WMA_ERROR_NOT_INITIALIZED`. Hoy ese código se tiraba y Kotlin devolvía
     * `success`.
     *
     * 🔴 **Las aserciones SON la premisa.** No hay una `JNIEXPORT` que pregunte "¿existe el
     * motor?" sin crearlo, así que no se puede afirmar la ausencia aparte. Lo que la
     * sostiene es que este test sería rojo —no verde— si el motor ya existiera: con motor
     * las cuatro devuelven éxito. Por eso el mensaje nombra esa hipótesis.
     */
    @Test
    fun `a - sin motor, parar-pausar-reanudar no pueden decir que si`() {
        val premisa = "si esto dio éxito, alguien ya creó el motor en esta JVM antes que este " +
            "test (revisá que `forkEvery = 1` siga puesto)"

        causa<NativeBridgeException.EngineNotInitialized>(
            jni("nativeStopEngine") { it.stopEngine() },
            "stopEngine sin motor — $premisa",
        )
        causa<NativeBridgeException.EngineNotInitialized>(
            jni("nativeStopEngineWithFade") { it.stopEngineWithFade(NO_FADE) },
            "stopEngineWithFade sin motor — $premisa",
        )
        causa<NativeBridgeException.EngineNotInitialized>(
            jni("nativePauseEngineWithFade") { it.pauseEngineWithFade(NO_FADE) },
            "pauseEngineWithFade sin motor — $premisa",
        )
        causa<NativeBridgeException.EngineNotInitialized>(
            jni("nativeResumeEngineWithFade") { it.resumeEngineWithFade(NO_FADE) },
            "resumeEngineWithFade sin motor — $premisa",
        )
    }

    /**
     * **AC-045.1 — el arranque que el backend rechaza llega como `failure(StreamError)`.**
     *
     * El escenario literal de la carta. Las DOS entradas de arranque, porque son dos
     * `JNIEXPORT` distintas y cada una descartaba su propio resultado: arreglar una y no la
     * otra es el defecto a medias que este REQ prohíbe.
     */
    @Test
    fun `b - si el backend no abre el stream, arrancar devuelve failure con StreamError`() {
        HostTestHooks.setStartFails(true)

        causa<NativeBridgeException.StreamError>(
            jni("nativeStartEngineWithFade") { it.startEngineWithFade(NO_FADE) },
            "startEngineWithFade con el backend rechazando",
        )
        causa<NativeBridgeException.StreamError>(
            jni("nativeStartEngine") { it.startEngine() },
            "startEngine con el backend rechazando",
        )

        // Y no quedó corriendo: un motor que dijo que no y arrancó igual sería el defecto
        // espejo. `getStreamInfo` devuelve null cuando no hay stream que describir.
        assertNull(
            AudioNativeBridge.getInstance().getStreamInfoArray(),
            "dijo que no pudo arrancar y hay un stream abierto igual",
        )
    }

    /**
     * **El gemelo de `b`**: con el backend andando, las seis dicen que sí.
     *
     * Sin esto, todo lo de arriba lo pasaría un bridge que devolviera `failure` siempre —
     * que es la forma tonta de "no mentir" y no sirve de nada. Este repo ya lo aprendió:
     * un test de "no publiques de más" lo pasa un apagado total.
     */
    @Test
    fun `c - con el backend andando, las seis del ciclo de vida dicen que si`() {
        HostTestHooks.setStartFails(false)

        assertTrue(
            jni("nativeStartEngineWithFade") { it.startEngineWithFade(NO_FADE) }.isSuccess,
            "el backend acepta el stream y arrancar falló igual",
        )
        assertTrue(
            jni("nativePauseEngineWithFade") { it.pauseEngineWithFade(NO_FADE) }.isSuccess,
            "pausar un motor corriendo no puede fallar",
        )
        assertTrue(
            jni("nativeResumeEngineWithFade") { it.resumeEngineWithFade(NO_FADE) }.isSuccess,
            "reanudar un motor pausado no puede fallar",
        )
        assertTrue(
            jni("nativeStopEngineWithFade") { it.stopEngineWithFade(NO_FADE) }.isSuccess,
            "parar un motor corriendo no puede fallar",
        )

        // Y el par sin fade, que son otras dos JNIEXPORT.
        assertTrue(
            jni("nativeStartEngine") { it.startEngine() }.isSuccess,
            "startEngine sobre un motor parado falló",
        )
        assertTrue(
            jni("nativeStopEngine") { it.stopEngine() }.isSuccess,
            "stopEngine sobre un motor corriendo falló",
        )
    }

    /**
     * **AC-045.3 (D10) — canales y baja latencia salen del stream, no de un default.**
     *
     * `StreamInfo.fromNativeArray` rellenaba `channelCount = 2` e `isLowLatency = true`
     * mientras el nativo le pasaba tres números: los dos valores eran **inventados en
     * Kotlin** y un consumidor los leía como medidos.
     *
     * El fake reporta [CANALES] canales y `lowLatency = false`, los dos distintos de esos
     * defaults: si alguien repone los defaults, este test es rojo por los dos lados. Es la
     * razón de elegir valores que el default no puede imitar — la misma que el arnés ya
     * aplica con los valores que no son potencia de dos.
     */
    @Test
    fun `d - canales y baja latencia viajan desde el stream abierto`() {
        HostTestHooks.setStartFails(false)
        HostTestHooks.setNegotiatedStream(CANALES, BAJA_LATENCIA)

        assertTrue(
            jni("nativeStartEngineWithFade") { it.startEngineWithFade(NO_FADE) }.isSuccess,
            "sin stream abierto no hay nada que describir",
        )

        val crudo = assertNotNull(
            jni("nativeGetStreamInfo") { it.getStreamInfoArray() },
            "el stream está abierto y la frontera no devolvió info",
        )
        val info = assertNotNull(
            StreamInfo.fromNativeArray(crudo),
            "la frontera devolvió un array que StreamInfo no supo leer (${crudo.joinToString()})",
        )

        assertEquals(
            CANALES,
            info.channelCount,
            "los canales no viajaron desde el stream: leyó ${info.channelCount} donde el " +
                "device reporta $CANALES. Si dice 2, alguien repuso el default inventado.",
        )
        assertEquals(
            BAJA_LATENCIA,
            info.isLowLatency,
            "el modo de latencia no viajó: si dice true, es el default que Kotlin inventaba",
        )

        // Control positivo sobre el MISMO array: si el cruce estuviera roto, los dos de
        // arriba podrían coincidir por casualidad con basura.
        assertTrue(info.sampleRate > 0, "el rate del stream abierto no puede ser 0")

        assertTrue(jni("nativeStopEngineWithFade") { it.stopEngineWithFade(NO_FADE) }.isSuccess)
        assertNull(
            AudioNativeBridge.getInstance().getStreamInfoArray(),
            "con el stream cerrado la info tiene que ser ausente, no un array de defaults",
        )
    }
}
