package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.domain.looper.ExportBitDepth
import kotlinx.coroutines.runBlocking
import org.junit.AfterClass
import org.junit.Before
import java.io.File
import java.nio.file.Files
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * REQ-026 S1 — **la cosecha: el camino de IO del looper, ejecutado.**
 *
 * ## Qué compra esto, ahora que existe el gate de FIRMAS
 *
 * 🔴 **Ya no compra firmas.** Desde REQ-025, `scripts/check-jni-signatures.py` verifica
 * aridad, anchos y retorno de las **309** en 0 s y sin `.so`. Lo que queda, y que ninguna
 * firma puede decir, es la **semántica del marshalling**: que el pinneo de una `jstring`
 * se libere, que un `null` no reviente, que el largo de un array de salida sea el que
 * corresponde, que no quede una excepción pendiente al volver.
 *
 * Este racimo es el de mayor densidad de `JNIEnv` de todo el hueco: **seis**
 * `GetStringUTFChars` sobre rutas de archivo, un `NewIntArray` + `SetIntArrayRegion` de
 * largo **variable**, y tres contadores `jlong`.
 *
 * ## Por qué es alcanzable en host, contra lo que decía el backlog
 *
 * El looper venía anotado como *"bloqueado por la bomba de render"*. Es falso para este
 * racimo: `LooperExporter::importTrack` (`LooperExporter.cpp:321`) lee el `.wav` y llena
 * el buffer de la pista **sincrónicamente, en el thread de control**, y
 * `finalizeRecording()` la deja activa. Todo lo que sigue —exportar, medir, contar— lee
 * ese contenido desde el mismo thread. `FakeAudioBackend` nunca invoca el callback y no
 * hace falta que lo haga.
 *
 * ## Los cinco contadores, y lo que se afirma de cada uno
 *
 * `looperGetTelemetry()` es el **único** accesor: una sola llamada de Kotlin cruza las
 * **cinco** `JNIEXPORT` de contadores. Eso no es una elección de este test, es la forma
 * del wrapper — así que las cinco quedan anotadas, y las cinco tienen que decir qué
 * afirman:
 *
 * - `exportsCompleted`, `exportsFailed`, `stemsWritten` se leen **después de moverlos**,
 *   con el incremento exacto afirmado, y vuelven a cero por `resetTelemetry`. Round-trip
 *   completo.
 * - 🔴 `framesDropped` y `armedTriggered` los escribe **sólo** `process()`
 *   (`AudioLooper.h:302` y `:222`), o sea el callback de audio, que `FakeAudioBackend`
 *   nunca invoca. De esos dos se afirma **únicamente** que valen 0, y eso es un test de
 *   *no mentir* **sin su gemelo**: en host no hay forma de distinguirlo de un contador
 *   roto que devuelve 0 siempre. Queda dicho acá en vez de escondido detrás de un
 *   `assertEquals` que se leería como cobertura real.
 *
 * 🔴 Verde acá NO significa "el IO del looper está probado": es la FRONTERA, sobre un
 * backend falso. Ver el KDoc de [JniHarness].
 */
class LooperIoJniTest {

    companion object {
        private const val OWNER = "LooperIoJniTest"
        private const val TRACK = 0

        /** Frames del fixture. 0,6875 s a 48 kHz, y no es potencia de dos. */
        private const val FRAMES = 33_000

        /** El tramo con contenido del fixture continuo. Ninguno de los dos es redondo. */
        private val CONTINUO = listOf(MinimalWav.Region(5_000, 27_000))

        /**
         * Cuatro ráfagas separadas, para que `detectOnsets` tenga transitorios reales que
         * encontrar. Cada arranque es un salto de energía desde silencio exacto.
         */
        private val TRANSITORIOS = listOf(
            MinimalWav.Region(3_000, 5_000),
            MinimalWav.Region(11_000, 13_000),
            MinimalWav.Region(19_000, 21_000),
            MinimalWav.Region(27_000, 29_000),
        )

        /** Ventana de análisis de onsets, y el techo. Ninguno es potencia de dos… salvo el hop. */
        private const val HOP = 512
        private const val MAX_ONSETS = 12
        private const val SENSIBILIDAD = 1.25f

        /**
         * Lo que esta clase declara cubrir. **Trinquete bidireccional** — ver
         * `JniCoverage.ratchet`: ejercer de menos es rojo, y ejercer de más también, para
         * que sumar cobertura aparezca en el diff del PR en vez de colarse.
         */
        private val COVERED = setOf(
            "nativeStartTuner",
            "nativeLooperClearTrack",
            "nativeLooperIsTrackActive",
            "nativeLooperGetTrackLengthFrames",
            "nativeLooperImportTrack",
            "nativeLooperExportTrack",
            "nativeLooperCaptureTrack",
            "nativeLooperExportMix",
            "nativeLooperExportMixV2",
            "nativeLooperExportStems",
            "nativeLooperDetectOnsets",
            // Las cinco de una: `looperGetTelemetry()` las cruza en una sola llamada.
            "nativeLooperGetFramesDropped",
            "nativeLooperGetExportsCompleted",
            "nativeLooperGetExportsFailed",
            "nativeLooperGetStemsWritten",
            "nativeLooperGetArmedTriggered",
            "nativeLooperResetTelemetry",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    private lateinit var dir: File

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name, call)

    /**
     * El motor tiene que existir, y la pista tiene que arrancar **vacía**.
     *
     * `nativeStartTuner` es el único de los 311 que crea el motor. El `clearTrack` no es
     * ceremonia: el motor nativo es un singleton de proceso y los métodos de esta clase
     * comparten la JVM, así que sin él un test leería el contenido que dejó otro y el
     * orden de ejecución de JUnit decidiría el veredicto.
     */
    @Before
    fun engineUpAndTrackEmpty() {
        assertTrue(jni("nativeStartTuner") { it.startTunerSync() }, "el motor no arrancó")
        jni("nativeLooperClearTrack") { it.looperClearTrack(TRACK) }
        assertFalse(
            jni("nativeLooperIsTrackActive") { it.looperIsTrackActive(TRACK) },
            "la pista arranca con contenido de otro test: todo lo de abajo mediría sobre él",
        )
        dir = Files.createTempDirectory("req026-io").toFile()
    }

    /**
     * AC-026.1 — **importar llena la pista sincrónicamente, sin render**, y la ruta cruza
     * intacta.
     *
     * El gemelo (un archivo que no es WAV) es el que hace afirmable el `true`: sin él,
     * "devolvió true" no se distingue de "devuelve true siempre". Y de paso ejerce el
     * camino en que `GetStringUTFChars` entrega una ruta **válida** cuyo contenido el
     * parser rechaza — que no es lo mismo que una ruta rota.
     */
    @Test
    fun `importar llena la pista y una ruta que no es WAV no`() {
        assertEquals(FRAMES, importar(CONTINUO), "el import no dejó los frames del fixture")

        val basura = File(dir, "no-es-un-wav.wav").apply { writeBytes(ByteArray(4_096) { 0x7B }) }
        assertFalse(
            jni("nativeLooperImportTrack") {
                it.looperImportTrack(TRACK, basura.absolutePath, MinimalWav.RATE)
            },
            "un archivo que no es RIFF/WAVE tiene que fallar: si esto da true, el import " +
                "no está leyendo la ruta que le pasaron",
        )
    }

    /**
     * AC-026.2 — las tres exportaciones de un solo archivo **vuelven a parsear**.
     *
     * Que el archivo exista y pese algo lo cumple también un motor que escribe basura. Lo
     * que se afirma es que [MinimalWav.parse] —el mismo formato que `wav::readWav` exige—
     * lo lee de vuelta con el rate y el conteo esperados.
     *
     * 🔴 Las profundidades **no son las mismas y eso es el punto**: `exportTrack` y
     * `exportMix` usan el default de la C API (**16 bits PCM**,
     * `wma_looper_export_options_default`), mientras que `captureTrack` recibe la suya por
     * parámetro. Pedirle 32 y leer 32 es lo que afirma que ese `jint` viajó.
     */
    @Test
    fun `exportar produce archivos que el lector vuelve a parsear`() {
        importar(CONTINUO)

        val pista = File(dir, "pista.wav")
        assertTrue(
            jni("nativeLooperExportTrack") { it.looperExportTrack(TRACK, pista.absolutePath) },
            "exportTrack falló",
        )
        MinimalWav.parse(pista).let { leido ->
            assertNotNull(leido, "exportTrack escribió algo que el lector de WAV no parsea")
            assertEquals(FRAMES, leido.frames, "exportTrack no escribió los frames de la pista")
            assertEquals(MinimalWav.RATE, leido.sampleRate, "exportTrack perdió el rate")
            assertEquals(16, leido.bitsPerSample, "el default de la C API es PCM de 16 bits")
        }

        val captura = File(dir, "captura.wav")
        assertTrue(
            jni("nativeLooperCaptureTrack") {
                it.looperCaptureTrack(TRACK, captura.absolutePath, ExportBitDepth.FLOAT_32.raw)
            },
            "captureTrack falló",
        )
        MinimalWav.parse(captura).let { leido ->
            assertNotNull(leido, "captureTrack escribió algo que el lector de WAV no parsea")
            assertEquals(FRAMES, leido.frames, "captureTrack escribe el buffer ENTERO")
            assertEquals(32, leido.bitsPerSample, "se pidió float32 y volvió otra cosa: el jint no viajó")
            assertEquals(3, leido.audioFormat, "float32 se escribe como formato 3 (IEEE float)")
        }

        val mezcla = File(dir, "mezcla.wav")
        assertTrue(
            jni("nativeLooperExportMix") { it.looperExportMix(mezcla.absolutePath) },
            "exportMix falló",
        )
        MinimalWav.parse(mezcla).let { leido ->
            assertNotNull(leido, "exportMix escribió algo que el lector de WAV no parsea")
            assertEquals(FRAMES, leido.frames, "la mezcla de una sola pista mide lo que la pista")
        }
    }

    /**
     * AC-026.3 — **los tres metadatos en `null`**, que es lo que ninguna firma ve.
     *
     * `nativeLooperExportMixV2` arma tres `ScopedUtfChars` y le pasa sus punteros a
     * `WmaExportOptions`. Con la `jstring` en `nullptr`, `GetStringUTFChars` **no se
     * llama** y el puntero queda en `nullptr` — o sea que el C++ de abajo tiene que
     * tratarlo como "sin metadato" y no desreferenciarlo. Eso es semántica, no firma.
     *
     * Los dos casos van juntos a propósito: sólo el nulo pasaría igual si el motor
     * ignorara los metadatos siempre, y sólo el presente no ejercería el camino nulo.
     */
    @Test
    fun `exportMixV2 exporta con los metadatos presentes y tambien en null`() {
        importar(CONTINUO)

        val conMetadatos = File(dir, "con-metadatos.wav")
        assertTrue(
            exportarV2(conMetadatos, proyecto = "Sandía", artista = "Watermelon", comentario = "REQ-026"),
            "exportMixV2 con los tres metadatos presentes falló",
        )
        val leidoCon = MinimalWav.parse(conMetadatos)
        assertNotNull(leidoCon, "el LIST/INFO va DESPUÉS del data: el lector tiene que caminar chunks")
        assertEquals(FRAMES, leidoCon.frames, "el chunk de metadatos corrió el conteo de frames")
        assertEquals(32, leidoCon.bitsPerSample, "se pidió float32")

        val sinMetadatos = File(dir, "sin-metadatos.wav")
        assertTrue(
            exportarV2(sinMetadatos, proyecto = null, artista = null, comentario = null),
            "exportMixV2 con los tres metadatos en null falló: el camino " +
                "ScopedUtfChars(env, nullptr) no está manejado",
        )
        val leidoSin = MinimalWav.parse(sinMetadatos)
        assertNotNull(leidoSin, "la exportación sin metadatos no parsea")
        assertEquals(FRAMES, leidoSin.frames, "sin metadatos el audio tiene que medir lo mismo")
        assertTrue(
            sinMetadatos.length() < conMetadatos.length(),
            "los dos archivos pesan igual: los metadatos no se escribieron ni con las tres " +
                "cadenas presentes, así que el caso nulo no probó nada",
        )
    }

    /**
     * AC-026.4 — el directorio cruza y el retorno **cuenta los archivos que hay**.
     *
     * `exportStems` nombra cada stem `track_<i>.wav`. Afirmar el retorno contra los
     * archivos que quedaron en disco es lo que impide que "devolvió 1" pase por bueno con
     * el directorio equivocado.
     */
    @Test
    fun `exportStems devuelve la cantidad de stems que dejo en el directorio`() {
        importar(CONTINUO)

        val stems = File(dir, "stems").apply { mkdirs() }
        val escritos = exportarStems(stems)
        assertEquals(1, escritos, "hay una sola pista activa, así que hay un solo stem")

        val enDisco = stems.listFiles { f -> f.name.endsWith(".wav") }.orEmpty()
        assertEquals(
            escritos,
            enDisco.size,
            "el retorno dice $escritos y en '${stems.absolutePath}' hay ${enDisco.size}: " +
                "la ruta del directorio no llegó entera",
        )
        assertEquals("track_$TRACK.wav", enDisco.single().name, "el stem no se llama como la pista")
        assertEquals(FRAMES, MinimalWav.parse(enDisco.single())?.frames, "el stem no mide lo que la pista")
    }

    /**
     * AC-026.5 — el `IntArray` de **largo variable**, con su gemelo.
     *
     * Es el único array de salida del racimo cuyo largo lo decide el C++ en tiempo de
     * ejecución (`NewIntArray(n)` + `SetIntArrayRegion(0, n)`), así que es donde un largo
     * mal calculado se manifiesta.
     *
     * 🔴 **No se afirma CUÁNTOS onsets hay.** Eso sería fijar un veredicto de DSP en un
     * test de frontera, y lo pagaría el día que alguien mueva el suavizado. Se afirma lo
     * que es del marshalling: no nulo, dentro del techo, estrictamente creciente y con
     * todos los valores adentro de la pista.
     *
     * Los **dos gemelos** son lo que hace que "largo 0" no sea el único desenlace que este
     * test ve nunca: silencio puro da 0, y `maxOnsets = 0` da 0 por el atajo de arriba de
     * todo — pero la pista con transitorios da un array **poblado**.
     */
    @Test
    fun `detectOnsets devuelve un array de largo variable, creciente y adentro de la pista`() {
        importar(TRANSITORIOS)

        val onsets = jni("nativeLooperDetectOnsets") {
            it.looperDetectOnsets(TRACK, MAX_ONSETS, HOP, SENSIBILIDAD)
        }
        assertTrue(
            onsets.isNotEmpty(),
            "cuatro ráfagas desde silencio exacto y no salió un solo onset: o el " +
                "SetIntArrayRegion no escribió, o el fixture no tiene los transitorios que dice",
        )
        assertTrue(onsets.size <= MAX_ONSETS, "devolvió ${onsets.size} onsets con un techo de $MAX_ONSETS")
        onsets.forEachIndexed { i, frame ->
            assertTrue(frame in 0 until FRAMES, "el onset $i cae en $frame, fuera de [0, $FRAMES)")
            if (i > 0) {
                assertTrue(
                    frame > onsets[i - 1],
                    "los onsets tienen que ser estrictamente crecientes: $frame después de ${onsets[i - 1]}",
                )
            }
        }

        assertEquals(
            0,
            jni("nativeLooperDetectOnsets") { it.looperDetectOnsets(TRACK, 0, HOP, SENSIBILIDAD) }.size,
            "con maxOnsets = 0 el C++ devuelve NewIntArray(0): un array vacío, no null",
        )

        importar(emptyList())
        assertEquals(
            0,
            jni("nativeLooperDetectOnsets") {
                it.looperDetectOnsets(TRACK, MAX_ONSETS, HOP, SENSIBILIDAD)
            }.size,
            "silencio exacto no tiene transitorios",
        )
    }

    /**
     * AC-026.6 — los contadores `jlong`, leídos **después de moverlos**, y devueltos a cero.
     *
     * Un contador leído en cero no distingue "no pasó nada" de "el `jlong` no cruza". Acá
     * se provoca un desenlace conocido de cada clase, se afirma el **incremento exacto**, y
     * después `resetTelemetry` los tiene que volver a cero — que es la otra mitad del
     * round-trip: sin ella, un contador que sube y nunca baja pasaría igual.
     *
     * 🔴 Los mueven `exportMix` / `exportMixV2` / `exportStems`, **no** `exportTrack` ni
     * `captureTrack`: el `fetch_add` vive en `exportMixInternal` (`LooperExporter.cpp:181`)
     * y en `exportStems` (`:252`). Medido antes de escribir el test, no después de verlo rojo.
     */
    @Test
    fun `los contadores de exportacion se mueven, y resetTelemetry los devuelve a cero`() {
        importar(CONTINUO)
        val antes = telemetria()

        assertTrue(
            jni("nativeLooperExportMix") { it.looperExportMix(File(dir, "ok.wav").absolutePath) },
            "la exportación que tenía que salir bien falló",
        )
        assertFalse(
            jni("nativeLooperExportMix") {
                it.looperExportMix(File(dir, "directorio-inexistente/mix.wav").absolutePath)
            },
            "exportar a un directorio que no existe tiene que fallar",
        )
        val escritos = exportarStems(File(dir, "stems-contados").apply { mkdirs() })
        assertEquals(1, escritos, "hay una sola pista activa")

        val despues = telemetria()
        assertEquals(
            2L,
            despues.exportsCompleted - antes.exportsCompleted,
            "una mezcla y una tanda de stems: completadas tiene que subir exactamente 2",
        )
        assertEquals(
            1L,
            despues.exportsFailed - antes.exportsFailed,
            "hubo exactamente una exportación fallida",
        )
        assertEquals(
            escritos.toLong(),
            despues.stemsWritten - antes.stemsWritten,
            "stemsWritten tiene que crecer lo que exportStems dijo haber escrito",
        )
        assertTrue(
            despues.exportsCompleted > 0 && despues.stemsWritten > 0,
            "los deltas podrían dar bien con los tres contadores clavados en cero si el " +
                "jlong no cruzara: el valor absoluto tiene que ser distinto de cero",
        )

        jni("nativeLooperResetTelemetry") { it.looperResetTelemetry() }
        val reseteada = telemetria()
        assertEquals(0L, reseteada.exportsCompleted, "resetTelemetry no bajó completadas")
        assertEquals(0L, reseteada.exportsFailed, "resetTelemetry no bajó fallidas")
        assertEquals(0L, reseteada.stemsWritten, "resetTelemetry no bajó stemsWritten")
    }

    /**
     * AC-026.6 (segunda mitad) — **lo que de estos dos NO se puede afirmar en host**.
     *
     * `framesDropped` y `armedTriggered` los escribe únicamente `process()`
     * (`AudioLooper.h:302` y `:222`), el callback de audio, que el backend falso jamás
     * invoca. O sea que este assert es *no mentir* **sin gemelo**: no distingue el
     * comportamiento correcto de un contador roto que devuelve 0 siempre.
     *
     * 🔴 Está escrito como test y no borrado porque las dos funciones **cruzan igual** —
     * `looperGetTelemetry()` las llama en la misma pasada que las otras tres— y una función
     * que cruza sin que nadie diga qué se afirma de ella es exactamente la cobertura de
     * mentira que este arnés existe para no ser. Lo que falta acá lo cubre la suite de C++,
     * que sí maneja el backend.
     */
    @Test
    fun `los dos contadores del callback quedan en cero, y eso NO prueba que anden`() {
        importar(CONTINUO)
        jni("nativeLooperResetTelemetry") { it.looperResetTelemetry() }

        val t = telemetria()
        assertEquals(0L, t.framesDropped, "sin render no hay frames que descartar")
        assertEquals(0L, t.armedTriggered, "sin render no hay armado que dispare")
    }

    // ---- helpers: cada uno anota UNA función, para que el trinquete siga siendo legible ----

    /**
     * Escribe el fixture, lo importa y devuelve el largo que quedó en la pista.
     *
     * Afirma acá adentro que el import salió bien y que la pista quedó **activa**: los
     * tests de abajo leen contenido, y sin esa precondición cualquiera de ellos podría
     * estar midiendo una pista vacía y aprobando igual.
     */
    private fun importar(regiones: List<MinimalWav.Region>): Int {
        val fuente = File(dir, "fuente-${regiones.size}.wav")
        val ruta = MinimalWav.writeTo(fuente, FRAMES, regiones)
        assertTrue(
            jni("nativeLooperImportTrack") { it.looperImportTrack(TRACK, ruta, MinimalWav.RATE) },
            "el fixture no importó: revisá MinimalWav contra wav::readWav",
        )
        assertTrue(
            jni("nativeLooperIsTrackActive") { it.looperIsTrackActive(TRACK) },
            "importó y la pista no quedó activa",
        )
        return jni("nativeLooperGetTrackLengthFrames") { it.looperGetTrackLengthFrames(TRACK) }
    }

    private fun exportarV2(destino: File, proyecto: String?, artista: String?, comentario: String?): Boolean =
        jni("nativeLooperExportMixV2") { bridge ->
            runBlocking {
                bridge.looperExportMixPro(
                    filePath = destino.absolutePath,
                    bitDepth = ExportBitDepth.FLOAT_32,
                    repeatLoops = 1,
                    countInBeats = 0,
                    applyLimiter = false,
                    projectName = proyecto,
                    artist = artista,
                    comment = comentario,
                    bpm = 0,
                )
            }
        }

    private fun exportarStems(destino: File): Int =
        jni("nativeLooperExportStems") { bridge ->
            runBlocking {
                bridge.looperExportStems(
                    directory = destino.absolutePath,
                    bitDepth = ExportBitDepth.FLOAT_32,
                    repeatLoops = 1,
                    countInBeats = 0,
                    applyLimiter = false,
                    bpm = 0,
                )
            }
        }

    /**
     * La telemetría entera, en **una** llamada — y cinco nombres anotados.
     *
     * `looperGetTelemetry()` es el único accesor público y llama a las cinco `JNIEXPORT`
     * de contadores en una pasada. Anotar sólo una y callar las otras cuatro sería
     * declarar de MENOS lo que de hecho cruzó, así que las cinco se anotan acá y el KDoc
     * de la clase dice qué se afirma de cada una.
     */
    private fun telemetria(): AudioNativeBridge.LooperTelemetry {
        val snapshot = jni("nativeLooperGetFramesDropped") { it.looperGetTelemetry() }
        listOf(
            "nativeLooperGetExportsCompleted",
            "nativeLooperGetExportsFailed",
            "nativeLooperGetStemsWritten",
            "nativeLooperGetArmedTriggered",
        ).forEach { JniCoverage.record(OWNER, it) }
        return snapshot
    }
}
