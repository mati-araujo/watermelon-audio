package com.watermellonstudios.audio.internal.bridge

import cnames.structs.WmaEngine
import com.watermellonstudios.audio.internal.cinterop.wma_engine_create
import com.watermellonstudios.audio.internal.cinterop.wma_engine_destroy
import com.watermellonstudios.audio.internal.cinterop.wma_looper_find_content_bounds
import kotlinx.cinterop.CPointer
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.IntVar
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.alloc
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.usePinned
import kotlinx.cinterop.value
import kotlinx.coroutines.test.runTest
import platform.Foundation.NSTemporaryDirectory
import platform.posix.fclose
import platform.posix.fopen
import platform.posix.fwrite
import platform.posix.remove
import kotlin.random.Random
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
 * progreso, los onsets, los bordes de contenido. Se verifican igual porque descartan
 * que el binding devuelva basura, pero no prueban comportamiento. La forma de onda
 * dejó de estar en este grupo con MINI-030: `looperImportTrack` llena la pista
 * sincrónicamente sin CoreAudio, así que el caso "con contenido" sí se observa acá.
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

    /** Valor sembrado en los out-params. No es 0 ni -1: los dos son respuestas plausibles. */
    private val CENTINELA = -7

    private companion object {
        /** El rate del fixture de MINI-030, igual al que se le pasa al import: sin resampleo. */
        const val FIXTURE_RATE = 48_000

        /** 2 s a 48 kHz, y no es potencia de dos. */
        const val FIXTURE_FRAMES = 96_000

        /** Amplitud de la ráfaga: `5/8`, exacta en float y no potencia de dos. */
        const val AMP = 0.625f

        /**
         * Más bins que `AudioLooper::MAX_WAVEFORM_BINS_CACHE` (512): el motor escribe 512,
         * rellena el resto con 0 y devuelve 512. No es potencia de dos.
         */
        const val BINS_SOBRE_EL_TECHO = 600

        /** Período de la onda cuadrada, en frames. */
        const val SQUARE_PERIOD = 16
    }

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
     * AC-M030.1 — **una pista sin contenido devuelve CERO elementos**, no `numBins` ceros.
     *
     * Hasta MINI-030 este test afirmaba `size == 24 / 8 / 64` sobre esta misma pista
     * vacía: el bridge descartaba el retorno de `wma_looper_get_track_waveform` (0 bins
     * escritos con la pista inactiva) y devolvía el array entero en cero. Eso no es
     * silencio, es **la ausencia disfrazada de silencio** — del lado del consumidor "sin
     * señal" y "en silencio" eran indistinguibles (carta de NoisyPad WV-3 §1). El
     * `isTrackActive` es el control: si diera `true`, el tamaño 0 hablaría de otra cosa.
     *
     * `numBins = 0` sigue dando vacío por la guarda del bridge, sin llegar a C.
     */
    @Test
    fun anEmptyTrackReturnsZeroElementsNotZeros() {
        assertFalse(bridge.looperIsTrackActive(0), "la pista tiene que estar INACTIVA para hablar de 'sin dato'")

        assertEquals(0, bridge.looperGetTrackWaveform(0).size, "con el default de 24 bins: vacío, no 24 ceros")
        assertEquals(0, bridge.looperGetTrackWaveform(0, numBins = 8).size, "vacío, no 8 ceros")
        assertEquals(0, bridge.looperGetTrackWaveform(0, numBins = 64).size, "vacío, no 64 ceros")

        assertTrue(
            bridge.looperGetTrackWaveform(0, numBins = 0).isEmpty(),
            "0 bins tiene que dar un array vacío, no una lectura fuera de rango",
        )
    }

    /**
     * AC-M030.1, el gemelo — **con contenido, el array tiene `numBins` elementos y trae
     * señal**; y si C escribe menos bins que los pedidos, el resto es relleno de
     * silencio y el largo sigue siendo `numBins`.
     *
     * Sin este test, "devuelve tamaño 0" no se distingue de un bridge que devuelve
     * `FloatArray(0)` siempre. La pista se llena por [ILooperBridge.looperImportTrack]
     * de un WAV float32 con una ráfaga de onda cuadrada de `±0,625` (`5/8`: exacto en
     * float y no potencia de dos): `LooperExporter::importTrack` lee el archivo y llena
     * la pista **sincrónicamente, en este thread**, y la deja activa — no hace falta
     * CoreAudio. Es el mismo camino que usa `LooperIoJniTest` en el arnés de Android.
     *
     * Dos pedidos:
     *  - `64` bins: C escribe los 64 ⇒ largo 64, pico exactamente `0,625` y el bin 0 en
     *    silencio exacto (los primeros 6000 frames son ceros). El pico es lo que distingue
     *    "cruzó el dato" de "cruzó el largo".
     *  - `600` bins: el motor tiene un techo de 512 (`MAX_WAVEFORM_BINS_CACHE`), escribe
     *    512, rellena 88 con 0 y **devuelve 512** ⇒ el bridge tiene que devolver los 600
     *    igual. Un `copyOf(escritos)` pasa el primer pedido y muere acá.
     */
    @OptIn(ExperimentalForeignApi::class)
    @Test
    fun aTrackWithContentReturnsNumBinsWithSignalEvenWhenTheEngineWritesFewer() {
        val wav = NSTemporaryDirectory() + "mini030-waveform-${randomSuffix()}.wav"
        writeFloatStereoSquareBurst(
            path = wav,
            frames = FIXTURE_FRAMES,
            burstStart = 6_000,
            burstEndExclusive = 42_000,
        )
        try {
            assertTrue(
                bridge.looperImportTrack(0, wav, FIXTURE_RATE),
                "el fixture no importó: revisá el escritor de WAV de este test contra wav::readWav",
            )
            assertTrue(bridge.looperIsTrackActive(0), "importó y la pista no quedó activa")

            val forma = bridge.looperGetTrackWaveform(0, numBins = 64)
            assertEquals(64, forma.size, "con contenido el array tiene que medir lo pedido")
            assertTrue(forma.all { it.isFinite() }, "la forma trajo NaN o infinito: basura de pinneo")
            val pico = forma.max()
            assertTrue(pico > 0.0f, "la pista tiene una ráfaga de ±$AMP y la forma no trae ningún pico")
            assertEquals(AMP, pico, "el pico de una onda cuadrada de ±$AMP tiene que ser exactamente eso")
            assertEquals(0.0f, forma.first(), "los primeros 6000 frames son silencio exacto: el bin 0 es 0")

            val sobreElTecho = bridge.looperGetTrackWaveform(0, numBins = BINS_SOBRE_EL_TECHO)
            assertEquals(
                BINS_SOBRE_EL_TECHO,
                sobreElTecho.size,
                "el motor escribe como mucho 512 bins y rellena el resto con 0: el bridge tiene " +
                    "que devolver los $BINS_SOBRE_EL_TECHO pedidos, no los que el motor escribió",
            )
            assertTrue(sobreElTecho.take(512).any { it > 0.0f }, "los 512 escritos tienen que traer señal")
            assertTrue(sobreElTecho.drop(512).all { it == 0.0f }, "más allá del techo el relleno es silencio exacto")
        } finally {
            bridge.looperClearTrack(0)
            remove(wav)
        }
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

    /**
     * La rama `false` de los out-params: `(0, 0)`, el mismo par que devuelve Android.
     *
     * 🔴 **Hasta MINI-016 este test pasaba por el camino EQUIVOCADO.** Se documentaba a sí
     * mismo como la rama `false` y no la ejercía: `wma_looper_find_content_bounds` hacía
     * `return true` incondicional, así que la C API respondía **éxito** con `(0, 0)` escrito
     * en los out-params y el `if (!...)` de [IosAudioBridge.looperFindContentBounds] nunca
     * corría. La aserción era correcta y la razón era falsa — teatro.
     *
     * Ahora sí: una pista vacía no tiene contenido audible, C devuelve `false`, y el `0 to 0`
     * de abajo sale de la rama de rechazo del bridge y no de leer los out-params. Y ese par
     * **no es ambiguo**: un resultado exitoso exige `outLast > outFirst`, o sea que `(0, 0)`
     * está reservado para "no hay nada acá".
     */
    @Test
    fun findContentBoundsRefusesAnEmptyTrackInsteadOfAnsweringZeroZero() {
        assertEquals(0 to 0, bridge.looperFindContentBounds(0, thresholdRatio = 0.03f))
    }

    /**
     * El gemelo: un índice fuera de rango también cae por la rama de rechazo.
     *
     * Sin él, "devuelve `(0, 0)`" no distinguiría el rechazo de un bridge que devuelve ese
     * par pase lo que pase — que es exactamente el estado en que estaba el test de arriba.
     */
    @Test
    fun findContentBoundsRefusesAnOutOfRangeTrack() {
        assertEquals(0 to 0, bridge.looperFindContentBounds(99, thresholdRatio = 0.03f))
        assertEquals(0 to 0, bridge.looperFindContentBounds(-1, thresholdRatio = 0.03f))
    }

    /**
     * 🔴 **El único test de este archivo que DISTINGUE el arreglo de MINI-016**, y por eso
     * entra por debajo del bridge en vez de por su API.
     *
     * Los dos de arriba no pueden distinguirlo y hay que decirlo: el bridge colapsa las dos
     * situaciones en `(0, 0)` —la rama de rechazo devuelve ese par, y el camino de éxito
     * con `(0, 0)` escrito en los out-params devolvía el mismo—, así que pasaban tanto con
     * el `return true` incondicional de antes como con el rechazo de ahora. Lo que
     * afirman es el contrato de SALIDA del bridge, que es útil y no es esto.
     *
     * Acá se llama a la C API **directo por cinterop**, con los out-params sembrados en un
     * centinela: si el rechazo no se propaga, la implementación los pisa con `(0, 0)` y el
     * assert se cae. Es además lo único que ejerce el marshalling de dos `int*` de salida
     * sobre la ABI de iOS — el equivalente de
     * `CApiLooperTest.ContentBoundsRefusesAnInvalidIndexAndASilentTrack`, que corre en el
     * host y no toca cinterop.
     *
     * El motor es **propio** y se destruye al salir: el del bridge es privado, y tomarlo
     * prestado acoplaría este test a un detalle de implementación de [IosAudioBridge].
     */
    @OptIn(ExperimentalForeignApi::class)
    @Test
    fun findContentBoundsLeavesTheOutParamsAloneWhenItRefuses() {
        val engine: CPointer<WmaEngine> = requireNotNull(wma_engine_create()) {
            "wma_engine_create() devolvió null"
        }
        try {
            memScoped {
                val first = alloc<IntVar>().also { it.value = CENTINELA }
                val last = alloc<IntVar>().also { it.value = CENTINELA }

                assertFalse(
                    wma_looper_find_content_bounds(engine, 99, 0.03f, first.ptr, last.ptr),
                    "una pista fuera de rango no tiene bordes que informar",
                )
                assertEquals(CENTINELA, first.value, "los out-params tienen que quedar intactos al rechazar")
                assertEquals(CENTINELA, last.value)

                assertFalse(
                    wma_looper_find_content_bounds(engine, 0, 0.03f, first.ptr, last.ptr),
                    "una pista sin contenido audible tampoco",
                )
                assertEquals(CENTINELA, first.value, "los out-params tienen que quedar intactos al rechazar")
                assertEquals(CENTINELA, last.value)
            }
        } finally {
            wma_engine_destroy(engine)
        }
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

    // ==================== El fixture de MINI-030 ====================

    private fun randomSuffix(): String = Random.nextInt(0, Int.MAX_VALUE).toString(16)

    /**
     * Un `.wav` float32 estéreo de [frames] frames: onda cuadrada de `±`[AMP] entre
     * [burstStart] y [burstEndExclusive], **ceros exactos** afuera. Es el mismo formato
     * (y la misma cabecera de 44 bytes) que `MinimalWav` en el arnés de Android, escrito
     * a mano porque el fixture no puede depender de una librería.
     *
     * Falla el test si el archivo no se puede abrir o queda corto: un fixture a medias
     * haría fallar el import con un mensaje que acusaría al motor.
     */
    @OptIn(ExperimentalForeignApi::class)
    private fun writeFloatStereoSquareBurst(path: String, frames: Int, burstStart: Int, burstEndExclusive: Int) {
        val channels = 2
        val blockAlign = channels * 4
        val dataSize = frames * blockAlign
        val bytes = ByteArray(44 + dataSize)
        var at = 0
        fun ascii(s: String) { for (c in s) bytes[at++] = c.code.toByte() }
        fun u16(v: Int) { bytes[at++] = (v and 0xFF).toByte(); bytes[at++] = ((v ushr 8) and 0xFF).toByte() }
        fun u32(v: Int) { u16(v and 0xFFFF); u16((v ushr 16) and 0xFFFF) }
        fun f32(v: Float) = u32(v.toRawBits())

        ascii("RIFF"); u32(36 + dataSize); ascii("WAVE")
        ascii("fmt "); u32(16); u16(3); u16(channels); u32(FIXTURE_RATE); u32(FIXTURE_RATE * blockAlign)
        u16(blockAlign); u16(32)
        ascii("data"); u32(dataSize)
        for (i in 0 until frames) {
            val enRafaga = i >= burstStart && i < burstEndExclusive
            if (!enRafaga) { f32(0.0f); f32(0.0f); continue }
            val alto = (i - burstStart) % SQUARE_PERIOD < SQUARE_PERIOD / 2
            val v = if (alto) AMP else -AMP
            f32(v); f32(v)
        }
        check(at == bytes.size) { "el fixture quedó en $at bytes de ${bytes.size}" }

        val file = requireNotNull(fopen(path, "wb")) { "no se pudo abrir $path para escribir el fixture" }
        try {
            val written = bytes.usePinned { pinned ->
                fwrite(pinned.addressOf(0), 1uL, bytes.size.toULong(), file)
            }
            check(written == bytes.size.toULong()) { "el fixture quedó corto: $written de ${bytes.size} bytes" }
        } finally {
            fclose(file)
        }
    }
}
