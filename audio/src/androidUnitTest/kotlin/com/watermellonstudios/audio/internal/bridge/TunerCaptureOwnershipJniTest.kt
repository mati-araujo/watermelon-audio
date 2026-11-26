package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.IInputBridge
import com.watermellonstudios.audio.api.ITunerBridge
import com.watermellonstudios.audio.domain.tuning.Tuning
import com.watermellonstudios.audio.domain.tuning.TuningConfiguration
import com.watermellonstudios.audio.internal.input.AudioInputImpl
import com.watermellonstudios.audio.internal.tuner.TunerImpl
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.AfterClass
import org.junit.Before
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * REQ-054.1 — **quién es dueño de la captura, visto desde Kotlin.**
 *
 * `AudioInputImpl` y `TunerImpl` REALES (los que arma la factory pública) sobre el
 * `AudioNativeBridge` real, cruzando la frontera JNI hasta `wma_input_*` / `wma_tuner_*` /
 * `wma_set_audio_mode`. Lo que se afirma sale de los criterios de aceptación y se observa en el
 * `FakeCaptureStream` (aperturas, cierres, abierto, violaciones), que acá se ENCIENDE por test
 * con [HostTestHooks.enableFakeCapture].
 *
 * Mapa AC → test:
 *  - AC-054.1: `AC054_1_*` (sólo el afinador: `start()` abre, `stop()` cierra antes de devolver).
 *  - AC-054.2: `AC054_2_*` en los DOS órdenes de arranque, con el modo INPUT_FX adjunto, y la
 *    mitad simétrica (`AudioInput.stop()` con el afinador corriendo ya no cierra su captura, ni
 *    siquiera sin `start()` previo: la suplencia de Tunio).
 *  - AC-054.4 (sólo la REAPERTURA vía un `ITuner` nuevo): `AC054_4_*`. Que el análisis vuelva a
 *    avanzar lo mide la suite de C++ (`test_capture_ownership.cpp`): acá no hay un hilo RT que
 *    entregue audio.
 *  - AC-054.3 (backend que entrega la entrada por callback) NO se cubre acá: el host JNI no puede
 *    construir un backend LIBUSB. Lo cubre la suite de C++.
 *
 * ## Cobertura del arnés
 *
 * Los dos puentes que se le dan a `AudioInputImpl`/`TunerImpl` delegan en el real y pasan cada
 * llamada por [JniHarness.exercise], así que lo que se declara en [COVERED] es lo que de verdad
 * cruzó la frontera, no un string escrito a mano.
 *
 * ## Qué hace con el "hueco declarado" de `InputNodeJniTest`
 *
 * El stream vivo no se abre en el host porque la fábrica del fake está APAGADA por defecto. Este
 * archivo lo enciende SÓLO adentro de sus tests y lo apaga en `@After`; como el arnés corre una
 * JVM por clase, `InputNodeJniTest` sigue viendo el fake apagado y su hueco declarado no se
 * toca. Lo que este archivo sí cierra, parcialmente, es la observación del **ciclo de vida** del
 * stream (abre/cierra) con un stream falso: NO prueba audio ni Oboe.
 *
 * 🔴 Backend FALSO adentro: frontera JNI/Kotlin, no audio en dispositivo.
 */
class TunerCaptureOwnershipJniTest {

    companion object {
        private const val OWNER = "TunerCaptureOwnershipJniTest"
        private const val MODE_INPUT_FX = 1

        /** Trinquete bidireccional — ver `JniCoverage.ratchet`. */
        private val COVERED = setOf(
            "nativeStartTuner", "nativeStopTuner",
            "nativeStartInputStream", "nativeStopInputStream",
            "nativeSetAudioMode",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    private lateinit var input: AudioInputImpl
    private lateinit var tuner: TunerImpl

    @Before
    fun setUp() {
        JniHarness.requireNativeLibrary()
        HostTestHooks.enableFakeCapture()
        input = AudioInputImpl(recordingInputBridge())
        tuner = newTuner()
    }

    /** El motor se destruye PRIMERO (cierra lo que haya quedado) y recién ahí se apaga el fake. */
    @After
    fun tearDown() {
        HostTestHooks.resetEngine()
        HostTestHooks.disableFakeCapture()
    }

    private fun newTuner() = TunerImpl(recordingTunerBridge(), TuningConfiguration(Tuning.GUITAR_STANDARD))

    private fun recordingInputBridge(): IInputBridge {
        val real = AudioNativeBridge.getInstance()
        return object : IInputBridge by real {
            override fun startInputStreamSync(): Boolean =
                JniHarness.exercise(OWNER, "nativeStartInputStream") { it.startInputStreamSync() }

            override fun stopInputStreamSync() =
                JniHarness.exercise(OWNER, "nativeStopInputStream") { it.stopInputStreamSync() }
        }
    }

    private fun recordingTunerBridge(): ITunerBridge {
        val real = AudioNativeBridge.getInstance()
        return object : ITunerBridge by real {
            override fun startTunerSync(): Boolean =
                JniHarness.exercise(OWNER, "nativeStartTuner") { it.startTunerSync() }

            override fun stopTunerSync() =
                JniHarness.exercise(OWNER, "nativeStopTuner") { it.stopTunerSync() }
        }
    }

    private fun setAudioMode(mode: Int) {
        val result = JniHarness.exercise(OWNER, "nativeSetAudioMode") { bridge ->
            runBlocking { bridge.setAudioMode(mode) }
        }
        assertTrue(result.isSuccess, "setAudioMode($mode) falló: ${result.exceptionOrNull()}")
    }

    private fun assertOpen(opens: Int, closes: Int, where: String) {
        assertTrue(HostTestHooks.fakeCaptureIsOpen(), "$where: la captura tenía que seguir ABIERTA")
        assertEquals(opens, HostTestHooks.fakeCaptureOpens(), "$where: aperturas")
        assertEquals(closes, HostTestHooks.fakeCaptureCloses(), "$where: cierres")
    }

    private fun assertClosed(opens: Int, closes: Int, where: String) {
        assertFalse(HostTestHooks.fakeCaptureIsOpen(), "$where: la captura tenía que estar CERRADA")
        assertEquals(opens, HostTestHooks.fakeCaptureOpens(), "$where: aperturas")
        assertEquals(closes, HostTestHooks.fakeCaptureCloses(), "$where: cierres")
    }

    private fun assertNoViolations() = assertEquals(
        0,
        HostTestHooks.fakeCaptureViolations(),
        "el stream de captura vio abrir abierto / arrancar cerrado / cerrar cerrado / destruirse abierto",
    )

    // ---- AC-054.1 -------------------------------------------------------------------------------

    /** Bug que atrapa: `ITuner.stop()` no cierra la captura que abrió `ITuner.start()`. */
    @Test
    fun `AC054_1 sólo el afinador - start abre y stop cierra antes de devolver`() {
        assertTrue(tuner.start(), "ITuner.start() tenía que arrancar")
        assertOpen(1, 0, "tras start")

        tuner.stop()
        assertClosed(1, 1, "tras stop")
        assertNoViolations()
    }

    // ---- AC-054.2, los dos órdenes, el modo, la mitad simétrica ---------------------------------

    /** Bug que atrapa: `ITuner.stop()` cierra la captura que el consumidor arrancó ANTES. */
    @Test
    fun `AC054_2 AudioInput primero - ITuner stop deja la captura y la cierra el ultimo dueño`() {
        assertTrue(input.start(), "AudioInput.start() tenía que arrancar")
        assertTrue(tuner.start())
        assertOpen(1, 0, "ambos arrancados")

        tuner.stop()
        assertOpen(1, 0, "tras ITuner.stop: la tiene el consumidor")

        input.stop()
        assertClosed(1, 1, "tras AudioInput.stop: último dueño")
        assertNoViolations()
    }

    /** Bug que atrapa: el afinador arrancado PRIMERO cierra igual al soltarse (el orden decide). */
    @Test
    fun `AC054_2 ITuner primero - ITuner stop deja la captura y la cierra el ultimo dueño`() {
        assertTrue(tuner.start())
        assertTrue(input.start(), "AudioInput.start() tenía que arrancar")
        assertOpen(1, 0, "ambos arrancados, sin reabrir")

        tuner.stop()
        assertOpen(1, 0, "tras ITuner.stop: la tiene el consumidor")

        input.stop()
        assertClosed(1, 1, "tras AudioInput.stop: último dueño")
        assertNoViolations()
    }

    /** Bug que atrapa: el modo INPUT_FX adjunto no cuenta como Consumer (o es un dueño que nunca suelta). */
    @Test
    fun `AC054_2 con el modo INPUT_FX adjunto - ITuner stop deja la captura y AudioInput stop la cierra`() {
        setAudioMode(MODE_INPUT_FX)
        assertOpen(1, 0, "el modo abre la captura")
        assertTrue(tuner.start())

        tuner.stop()
        assertOpen(1, 0, "tras ITuner.stop: la tiene el modo")

        input.stop()
        assertClosed(1, 1, "tras AudioInput.stop: el modo no queda colgado")
        assertNoViolations()
    }

    /**
     * Mitad simétrica. Bug que atrapa: `AudioInput.stop()` cierra SIEMPRE y deja al afinador
     * corriendo sobre una captura muerta.
     */
    @Test
    fun `AC054_2 simetrica con el afinador primero - AudioInput stop ya no cierra la captura del afinador`() {
        assertTrue(tuner.start())
        assertTrue(input.start())

        input.stop()
        assertOpen(1, 0, "tras AudioInput.stop: la tiene el afinador")

        tuner.stop()
        assertClosed(1, 1, "tras ITuner.stop: último dueño")
        assertNoViolations()
    }

    /** Bug que atrapa: igual con el consumidor primero (el orden no cambia quién suelta qué). */
    @Test
    fun `AC054_2 simetrica con AudioInput primero - AudioInput stop ya no cierra la captura del afinador`() {
        assertTrue(input.start())
        assertTrue(tuner.start())

        input.stop()
        assertOpen(1, 0, "tras AudioInput.stop: la tiene el afinador")

        tuner.stop()
        assertClosed(1, 1, "tras ITuner.stop: último dueño")
        assertNoViolations()
    }

    /**
     * La suplencia de Tunio. Bug que atrapa: `AudioInput.stop()` sin `start()` previo (sólo para
     * "asegurar" que no hay captura) le cierra la captura al afinador que está corriendo.
     */
    @Test
    fun `AC054_2 AudioInput stop sin start previo no toca la captura del afinador`() {
        assertTrue(tuner.start())

        input.stop()
        assertOpen(1, 0, "AudioInput.stop sin start")

        tuner.stop()
        assertClosed(1, 1, "tras ITuner.stop")
        assertNoViolations()
    }

    // ---- AC-054.4: la reapertura (el análisis lo mide la suite de C++) --------------------------

    /**
     * El ciclo de Tunio: al fondo, al frente, un `ITuner` NUEVO. Bug que atrapa: tras cerrar la
     * captura, el `start()` del `ITuner` nuevo cree que sigue viva y no la reabre.
     */
    @Test
    fun `AC054_4 un ITuner nuevo tras el stop del anterior reabre la captura`() {
        assertTrue(tuner.start())
        tuner.stop()
        assertClosed(1, 1, "tras el primer stop")

        val second = newTuner()
        assertTrue(second.start(), "el ITuner nuevo tenía que arrancar")
        assertOpen(2, 1, "tras el start del ITuner nuevo: reabierta")

        second.stop()
        assertClosed(2, 2, "fin")
        assertNoViolations()
    }
}
