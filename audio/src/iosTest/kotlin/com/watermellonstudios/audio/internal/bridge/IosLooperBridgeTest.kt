package com.watermellonstudios.audio.internal.bridge

import kotlinx.coroutines.test.runTest
import platform.Foundation.NSTemporaryDirectory
import kotlin.test.AfterTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * `ILooperBridge` sobre cinterop, corriendo en el simulador.
 *
 * ## Qué se puede verificar sin arrancar el motor, y qué no — medido, no supuesto
 *
 * Igual que las otras suites de este archivo, no se abre un stream de CoreAudio. Se
 * exploró primero qué queda observable en esas condiciones, y el reparto salió así:
 *
 * **Round-trips reales** (el estado vuelve por un atomic, sin pasar por el render):
 * volumen maestro, velocidad de pista y el par `saveUndoSnapshot`/`hasUndo`.
 *
 * **Sólo valor de reposo**: todo lo que depende de que exista audio grabado — el
 * progreso, la forma de onda, los onsets, los bordes de contenido. Se verifican igual
 * porque descartan que el binding devuelva basura, pero no prueban comportamiento.
 *
 * **Una sorpresa que conviene tener anotada**: `looperPrepareTrack` devuelve `true` y
 * sin embargo la pista **no** queda activa ni con largo — `looperIsTrackActive` sigue
 * en `false` y `looperGetTrackLengthFrames` en 0. La reserva se completa del lado del
 * thread de audio. Por eso también `looperSetTrackLoopRegion` no mueve
 * `looperGetTrackLoopStart`/`End`: no hay largo sobre el que fijar una región. Si
 * alguien viene a "arreglar" eso, esto es lo que va a encontrar.
 *
 * El comportamiento con el motor andando es WA-4.3, en device.
 */
class IosLooperBridgeTest {

    private val bridge = IosAudioBridge()

    @AfterTest
    fun cleanup() {
        bridge.looperClearAll()
        bridge.looperSetEnabled(false)
    }

    // ==================== Round-trips de verdad ====================

    /**
     * El volumen maestro vuelve tal cual se puso.
     *
     * Es el equivalente de `isArpEnabled` en esta interfaz: el que puede fallar con una
     * implementación rota y pasar con una buena. Un `Float` además cubre algo que un
     * `Boolean` no: que la convención de llamada para punto flotante sea la correcta.
     */
    @Test
    fun masterVolumeRoundTripsThroughTheEngine() {
        assertEquals(1.0f, bridge.looperGetMasterVolume(), "el volumen maestro arranca en 1.0")

        bridge.looperSetMasterVolume(0.42f)
        assertEquals(0.42f, bridge.looperGetMasterVolume(), "el volumen no llegó al motor")

        bridge.looperSetMasterVolume(1.0f)
        assertEquals(1.0f, bridge.looperGetMasterVolume(), "no se pudo restaurar")
    }

    /** Ídem por pista, y anda incluso sobre una pista sin preparar. */
    @Test
    fun trackSpeedRoundTripsThroughTheEngine() {
        assertEquals(1.0f, bridge.looperGetTrackSpeed(0), "la velocidad arranca en 1.0")

        bridge.looperSetTrackSpeed(0, 0.5f)
        assertEquals(0.5f, bridge.looperGetTrackSpeed(0), "la velocidad no llegó al motor")
    }

    /**
     * El único round-trip de ESTADO, y no de un valor que se guarda y se devuelve:
     * `hasUndo` pasa de `false` a `true` porque el motor guardó algo.
     */
    @Test
    fun savingAnUndoSnapshotFlipsHasUndo() {
        assertFalse(bridge.looperHasUndo(0), "no puede haber undo antes de guardar nada")

        assertTrue(bridge.looperSaveUndoSnapshot(0), "guardar el snapshot falló")
        assertTrue(bridge.looperHasUndo(0), "el snapshot se guardó pero hasUndo no lo ve")
    }

    // ==================== Lo que el motor parado no completa ====================

    /**
     * `looperPrepareTrack` dice que sí y la pista sigue vacía. Documentado arriba.
     *
     * El test existe para fijar lo observado: si esto empieza a fallar porque la pista
     * ahora **sí** queda activa, no es una regresión — es que la reserva dejó de
     * depender del thread de audio, y hay que actualizar el KDoc de la clase.
     */
    @Test
    fun prepareTrackSucceedsButTheAllocationNeedsTheAudioThread() {
        bridge.looperSetEnabled(true)
        bridge.looperSetCapabilities(budgetBytes = 64L * 1024 * 1024, maxTracks = 4, maxFreeSeconds = 30)

        assertTrue(
            bridge.looperPrepareTrack(trackIndex = 0, lengthFrames = 48_000, sampleRate = 48_000),
            "prepareTrack reportó fallo — el mapeo de WmaResult a Boolean se rompió",
        )
        assertFalse(bridge.looperIsTrackActive(0), "medido: sin render la pista no queda activa")
        assertEquals(0, bridge.looperGetTrackLengthFrames(0), "medido: el largo se completa después")
    }

    // ==================== Buffers de salida ====================

    /**
     * El array de forma de onda tiene **exactamente** los bins pedidos.
     *
     * Es lo que prueba que el par (puntero fijado, tamaño) llega bien: si el tamaño se
     * marshallara mal, C escribiría más allá del array de Kotlin.
     */
    @Test
    fun theWaveformArrayHonoursTheRequestedBinCount() {
        assertEquals(24, bridge.looperGetTrackWaveform(0).size, "el default de 24 bins no se aplicó")
        assertEquals(8, bridge.looperGetTrackWaveform(0, numBins = 8).size, "no respetó numBins")
        assertEquals(64, bridge.looperGetTrackWaveform(0, numBins = 64).size, "no respetó numBins")

        assertTrue(
            bridge.looperGetTrackWaveform(0, numBins = 0).isEmpty(),
            "0 bins tiene que dar un array vacío, no una lectura fuera de rango",
        )
    }

    /**
     * Los onsets salen con el largo REAL, no con el reservado.
     *
     * Es la diferencia deliberada con la forma de onda: un cero de relleno acá sería un
     * transitorio en el frame 0, o sea un dato inventado. Sobre una pista vacía el largo
     * real es 0.
     */
    @Test
    fun detectOnsetsReturnsTheRealCountAndNotTheReservedBuffer() {
        assertTrue(
            bridge.looperDetectOnsets(0, maxOnsets = 16).isEmpty(),
            "una pista vacía no tiene onsets, y el relleno no debe colarse",
        )
        assertTrue(
            bridge.looperDetectOnsets(0).isEmpty(),
            "ídem con el maxOnsets por defecto",
        )
        assertTrue(
            bridge.looperDetectOnsets(0, maxOnsets = 0).isEmpty(),
            "reservar 0 no puede alocar ni leer nada",
        )
    }

    /** La rama `false` de los out-params: `(0, 0)`, el mismo par que devuelve Android. */
    @Test
    fun findContentBoundsIsZeroZeroOnAnEmptyTrack() {
        assertEquals(0 to 0, bridge.looperFindContentBounds(0, thresholdRatio = 0.03f))
    }

    // ==================== Export ====================

    /**
     * El camino del export corre entero con metadatos y vuelve como fallo limpio.
     *
     * **Lo que este test SÍ prueba**: que `WmaExportOptions` se arma y se pasa sin
     * corromper nada, que los defaults de la interfaz llegan a la implementación (la
     * segunda llamada no pasa ninguno) y que el `suspend` no cuelga.
     *
     * **Lo que NO prueba, y hay que decirlo**: que los tres `const char*` de metadatos
     * sobrevivan hasta que C los lea. Sin pistas grabadas el export corta antes de
     * escribir el encabezado, así que esos punteros probablemente ni se dereferencian.
     * Verificarlo necesita una toma real, o sea el thread de audio — WA-4.3, en device.
     * Queda declarado como hueco, no tapado con una aserción que no lo cubre.
     */
    @Test
    fun theExportPathRunsWithMetadataAndFailsCleanlyWithNothingToExport() = runTest {
        bridge.looperSetEnabled(true)
        val wav = NSTemporaryDirectory() + "ios-looper-test-mix.wav"

        assertFalse(
            bridge.looperExportMixPro(
                filePath = wav,
                projectName = "proyecto",
                artist = "artista",
                comment = "comentario",
                bpm = 120,
            ),
            "sin pistas no hay mezcla que exportar",
        )
        assertFalse(bridge.looperExportMixPro(filePath = wav), "ídem con los valores por defecto")

        assertEquals(
            -1,
            bridge.looperExportStems(directory = NSTemporaryDirectory()),
            "sin pistas, el export de stems reporta -1",
        )
    }

    /** En reposo no hay export en vuelo, y cancelar lo que no existe no es un error. */
    @Test
    fun theIdleExportStateIsCoherentAndCancellingIsSafe() {
        assertFalse(bridge.looperIsExportInProgress(), "no debería haber un export en vuelo")
        assertEquals(0.0f, bridge.looperGetExportProgress(), "sin export el progreso es 0")

        bridge.looperCancelExport()

        assertFalse(bridge.looperIsExportInProgress(), "cancelar dejó el motor inconsistente")
    }
}
