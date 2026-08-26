package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.LooperStateListener
import kotlin.test.AfterTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * `setLooperStateListener` en iOS: el único camino de la fachada por el que el motor
 * llama hacia arriba.
 *
 * ## Cómo se reparte la verificación entre este archivo y C++
 *
 * La entrega de verdad —thread de audio → cola lock-free → worker → callback— **se
 * verifica en C++**, en `test_c_api_looper.cpp`, porque allá el fixture puede empujar
 * bloques por `onAudioReady` y producir eventos reales. Acá eso no se puede: los
 * eventos nacen en el thread de audio y esta suite no arranca el motor.
 *
 * Lo que sí queda de este lado, y no es poco:
 *
 * - **El reparto de tipo a método** ([dispatchLooperEvent]), que es donde vive el bug
 *   silencioso de esta sección: cruzar progreso con pico no rompe nada, sólo hace que la
 *   UI muestre otra cosa. Se extrajo del `staticCFunction` justamente para poder
 *   probarlo.
 * - **El registro y su reemplazo**, incluido el `null`.
 *
 * ## El hueco declarado
 *
 * Que el trampolín de Kotlin/Native sobreviva a ser llamado desde un hilo foráneo **no
 * se prueba acá**. La cadena de C está cubierta y el nuevo MM de Kotlin/Native adjunta
 * esos hilos solo, pero eso es documentación, no medición. Se cierra en device o con el
 * harness, junto al resto de WA-4.3.
 */
class IosLooperStateListenerTest {

    private val bridge = IosAudioBridge()

    /** Anota lo que le llega, en orden. */
    private class Recorder : LooperStateListener {
        val calls = mutableListOf<String>()
        override fun onTrackProgress(trackIndex: Int, progress: Float) {
            calls += "progress($trackIndex, $progress)"
        }

        override fun onTrackPlayingChanged(trackIndex: Int, isPlaying: Boolean) {
            calls += "playing($trackIndex, $isPlaying)"
        }

        override fun onTrackPeakChanged(trackIndex: Int, peakLevel: Float) {
            calls += "peak($trackIndex, $peakLevel)"
        }

        override fun onTrackRecordProgress(trackIndex: Int, progress: Float) {
            calls += "record($trackIndex, $progress)"
        }

        override fun onTrackCompleted(trackIndex: Int) {
            calls += "completed($trackIndex)"
        }

        override fun onBeat(beatIndex: Int, nextBeatFrame: Int) {
            calls += "beat($beatIndex, $nextBeatFrame)"
        }
    }

    /** No sobrescribe onBeat: prueba que el default no-op alcanza (AC-017.6). */
    private class OldRecorder : LooperStateListener {
        val calls = mutableListOf<String>()
        override fun onTrackProgress(trackIndex: Int, progress: Float) {
            calls += "progress($trackIndex, $progress)"
        }

        override fun onTrackPlayingChanged(trackIndex: Int, isPlaying: Boolean) = Unit

        override fun onTrackPeakChanged(trackIndex: Int, peakLevel: Float) = Unit
    }

    @AfterTest
    fun cleanup() {
        bridge.setLooperStateListener(null)
    }

    // ==================== El reparto ====================

    /**
     * Cada tipo va a su método, con su índice y su valor.
     *
     * Los números son ABI: están fijados en `WmaLooperEventType` y este test es lo que
     * impide que se crucen sin que nadie lo note.
     */
    @Test
    fun everyEventTypeReachesItsOwnCallback() {
        val recorder = Recorder()

        dispatchLooperEvent(recorder, type = 0, trackIndex = 3, value = 0.5f)
        dispatchLooperEvent(recorder, type = 1, trackIndex = 4, value = 1.0f)
        dispatchLooperEvent(recorder, type = 2, trackIndex = 5, value = 0.8f)
        dispatchLooperEvent(recorder, type = 3, trackIndex = 6, value = 0.25f)
        dispatchLooperEvent(recorder, type = 4, trackIndex = 7, value = 0f)

        assertEquals(
            listOf(
                "progress(3, 0.5)",
                "playing(4, true)",
                "peak(5, 0.8)",
                "record(6, 0.25)",
                "completed(7)",
            ),
            recorder.calls,
        )
    }

    /** `value == 0` es "paró"; cualquier otra cosa es "está sonando". */
    @Test
    fun thePlayingFlagComesFromTheFloatBeingNonZero() {
        val recorder = Recorder()

        dispatchLooperEvent(recorder, type = 1, trackIndex = 0, value = 0f)
        dispatchLooperEvent(recorder, type = 1, trackIndex = 0, value = 1f)

        assertEquals(listOf("playing(0, false)", "playing(0, true)"), recorder.calls)
    }

    /**
     * El centinela negativo de `RecordProgress` **se pasa tal cual**, no se recorta.
     *
     * Es una decisión del contrato, no un descuido: un valor < 0 significa "la grabación
     * terminó", y recortarlo a 0 lo convertiría en "va por el 0 %" — el consumidor
     * nunca se enteraría de que puede limpiar su estado de "grabando…".
     */
    @Test
    fun theNegativeRecordProgressSentinelIsPassedThroughUntouched() {
        val recorder = Recorder()

        dispatchLooperEvent(recorder, type = 3, trackIndex = 2, value = -1.0f)

        assertEquals(listOf("record(2, -1.0)"), recorder.calls)
    }

    /**
     * Un tipo que esta versión no conoce se ignora en silencio.
     *
     * Importa para la compatibilidad hacia adelante: `WmaLooperEventType` puede crecer, y
     * una app con una fachada vieja contra un motor nuevo tiene que seguir andando en vez
     * de romper con un `when` sin rama.
     */
    /**
     * REQ-017 — el beat es el UNICO evento global, y el unico que cruza los campos.
     *
     * `value` trae el indice de beat y `trackIndex` el frame absoluto del proximo,
     * mientras que `onBeat` los recibe al reves: (beatIndex, nextBeatFrame). Si
     * alguien "simplifica" el despacho pasandolos en el orden del resto, el
     * consumidor recibe un frame donde espera un indice y viceversa — dos Int, sin
     * error de tipos, sin crash, y con el pulso dibujado en cualquier lado.
     */
    @Test
    fun aBeatEventArrivesWithTheIndexAndTheAnchorInThatOrder() {
        val recorder = Recorder()

        // value = indice de beat (2), trackIndex = frame del proximo beat (72000)
        dispatchLooperEvent(recorder, type = 5, trackIndex = 72000, value = 2f)

        assertEquals(listOf("beat(2, 72000)"), recorder.calls)
    }

    /** AC-017.6 — un listener que predate onBeat sigue compilando y no se entera. */
    @Test
    fun aListenerThatDoesNotOverrideOnBeatIsUnaffected() {
        val old = OldRecorder()

        dispatchLooperEvent(old, type = 5, trackIndex = 72000, value = 2f)

        assertTrue(old.calls.isEmpty(), "el default no-op no deberia registrar nada")
    }

    @Test
    fun anUnknownEventTypeIsIgnoredInsteadOfThrowing() {
        val recorder = Recorder()

        dispatchLooperEvent(recorder, type = 99, trackIndex = 0, value = 1f)
        dispatchLooperEvent(recorder, type = -1, trackIndex = 0, value = 1f)

        assertTrue(recorder.calls.isEmpty(), "un tipo desconocido no debería llamar a nada")
    }

    // ==================== El registro ====================

    @Test
    fun registeringAndClearingBothSucceed() {
        assertTrue(bridge.setLooperStateListener(Recorder()), "registrar debería andar")
        assertTrue(bridge.setLooperStateListener(null), "quitar siempre debería andar")
    }

    /**
     * Registrar de nuevo reemplaza, y quitar y volver a poner tampoco rompe.
     *
     * Recorre el ciclo que el holder tiene que sostener: el `StableRef` se crea una sola
     * vez y lo que se mueve es el campo. Si alguien "optimizara" liberándolo al quitar el
     * listener, este test seguiría pasando y el crash aparecería en producción — por eso
     * el porqué está escrito en el KDoc de `LooperListenerHolder` y no sólo acá.
     */
    @Test
    fun theListenerCanBeReplacedAndReinstalled() {
        assertTrue(bridge.setLooperStateListener(Recorder()))
        assertTrue(bridge.setLooperStateListener(Recorder()))
        assertTrue(bridge.setLooperStateListener(null))
        assertTrue(bridge.setLooperStateListener(Recorder()))
        assertTrue(bridge.setLooperStateListener(null))
    }
}
