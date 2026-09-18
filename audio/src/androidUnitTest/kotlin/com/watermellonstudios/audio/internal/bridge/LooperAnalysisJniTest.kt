package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.domain.looper.LevelEnvelope
import com.watermellonstudios.audio.domain.looper.PitchSeries
import org.junit.AfterClass
import org.junit.Before
import java.io.File
import java.security.MessageDigest
import kotlin.math.log10
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import kotlin.test.fail

/**
 * REQ-043 S2 (AC-043.6, AC-043.7) — **las dos lecturas de análisis por pista cruzan la
 * frontera JNI con dato real**: la serie de pitch y la envolvente RMS, sobre los DOS
 * fixtures versionados del looper, entrando por `looperImportTrack` (sin bomba de render:
 * el import llena la pista sincrónicamente y la deja activa, como en MINI-030).
 *
 * ## Qué atrapa esto, y ningún otro gate
 *
 * `check-jni-signatures` ve que `nativeLooperAnalyzePitch` declara tres arrays y un `Float`
 * donde el C++ los espera; **no ve** que el bridge dimensione por la cota, que recorte al
 * retorno, que el hop cruce por el `IntArray(1)`, ni que un `0/0` del motor llegue exacto a
 * Kotlin. La suite de C++ (`test_track_analysis.cpp`) afirma el motor contra el oráculo del
 * fixture con umbrales medidos; acá NO se repite esa medición —sería el mismo número dos
 * veces— sino que se afirma **lo que el bridge puede romper solo**:
 *
 * - el **largo**: `size == floor((L − W) / hop) + 1`, ni la cota ni el buffer reservado.
 *   Mutante que mata: descartar el retorno de C y devolver los arrays de la cota ⇒ 451 en
 *   vez de 447 ⇒ rojo en [laSerieDePitchDelGlideCruzaConSuEjeYSusCerosExactos].
 * - el **eje**: primer `frame == loopStart + W/2`, paso exacto `hopFrames`.
 * - el **0/0 exacto** en el silencio: `hz == 0f && confidence == 0f`, sin epsilon, que es la
 *   diferencia entre "el motor lo decidió" y "cruzó basura de pinneo".
 * - el **rango** en el glide: 100–450 Hz donde el oráculo va de 110 a 440.
 * - la envolvente: `firstFrame == loopStart`, `hopFrames == 480`, `size == 400` (la cola
 *   de 0 frames no agrega bin), y el golpe del frame 0 por encima de −35 dB con el hueco
 *   entre golpes por debajo de −45 dB.
 * - **determinismo a través de la frontera**: dos cruces, `contentEquals`.
 *
 * ## El fixture es el versionado, con su sha256
 *
 * Se lee del árbol (`looper/tests/testdata/`) y se verifica contra el sha del
 * `MANIFEST.txt` **antes** de afirmar nada: un test que mide contra un fixture tiene que
 * poder decir contra QUÉ archivo. Si alguien regenera el glide, esto se pone rojo y ése es
 * el diff de la revisión.
 *
 * 🔴 Verde acá NO significa "la voz del video anda": es un backend FALSO y un fixture
 * sintético. La voz real la mide NoisyPad (I-2, el criterio de muerte de REQ-043).
 */
class LooperAnalysisJniTest {

    companion object {
        private const val OWNER = "LooperAnalysisJniTest"
        private const val RATE = 48_000

        /** La pista del glide y la del audiograma: dos, porque JUnit no promete orden. */
        private const val TRACK_GLIDE = 0
        private const val TRACK_GOLPES = 1

        /** Una tercera que nadie llena: el caso "0 elementos" (AC-043.6). */
        private const val TRACK_VACIA = 2

        /** `W = 40 ms` del preset de voz del motor (`kVoiceWindowMs`), en frames a 48 kHz. */
        private const val W_FRAMES = 1_920

        /** 10 ms a 48 kHz: el hop de arranque que acordó NoisyPad. */
        private const val HOP_FRAMES = 480

        // glide-voz.wav: 216 000 frames, fronteras 0 · 96000 · 120000 · 192000 · 216000.
        private const val GLIDE_FRAMES = 216_000
        private const val SILENCIO_INI = 96_000
        private const val SILENCIO_FIN = 120_000

        // audiograma-prueba.wav: 192 000 frames, 8 golpes cada 24 000.
        private const val GOLPES_FRAMES = 192_000

        private const val GLIDE_SHA256 = "f7bbc0123988674ee301640308da38121baf8e1fbf1d7a163c92af3c98f4af9a"
        private const val GOLPES_SHA256 = "82f44736e3b28bf81836bba7f3d89df1e059e5535cec394f83cbedac5804eb75"

        /**
         * Lo que esta clase declara cubrir. **Trinquete bidireccional** (`JniCoverage.ratchet`):
         * ejercer de menos es rojo, y de más también, para que sumar cobertura aparezca en el
         * diff del PR.
         */
        private val COVERED = setOf(
            "nativeStartTuner",
            "nativeLooperImportTrack",
            "nativeLooperIsTrackActive",
            "nativeLooperGetTrackLoopStart", "nativeLooperGetTrackLoopEnd",
            "nativeLooperAnalyzePitch",
            "nativeLooperGetLevelEnvelope",
            "nativeLooperDetectOnsets",
            "nativeLooperClearTrack",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)

        private fun db(linear: Float): Double = 20.0 * log10(linear.toDouble().coerceAtLeast(1e-9))

        /**
         * Sube desde el directorio de trabajo buscando `looper/tests/testdata`, igual que
         * `JniExports.locateJniSources`: el cwd de la task de test es del build, no de este
         * archivo, y una ruta relativa cableada es lo que rompe el día que alguien mueve el
         * módulo.
         */
        private fun fixture(name: String, sha256: String): File {
            val relative = File("audio/src/main/cpp/looper/tests/testdata")
            var dir: File? = File(System.getProperty("user.dir")).absoluteFile
            while (dir != null) {
                val candidate = File(dir, relative.path)
                if (candidate.isDirectory) {
                    val file = File(candidate, name)
                    if (!file.isFile) fail("el fixture $name no está en $candidate")
                    val digest = MessageDigest.getInstance("SHA-256").digest(file.readBytes())
                    val actual = digest.joinToString("") { "%02x".format(it) }
                    assertEquals(
                        sha256, actual,
                        "el sha256 de $name no es el del MANIFEST.txt: este test midió contra OTRO archivo. " +
                            "Si lo regeneraste a propósito, ese diff es la revisión",
                    )
                    return file
                }
                dir = dir.parentFile
            }
            fail("no encontré '${relative.path}' subiendo desde '${System.getProperty("user.dir")}'")
        }
    }

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name, call)

    private fun importar(track: Int, name: String, sha256: String) {
        val ruta = fixture(name, sha256).absolutePath
        assertTrue(
            jni("nativeLooperImportTrack") { it.looperImportTrack(track, ruta, RATE) },
            "el fixture $name no importó: revisá wav::readWav contra el MANIFEST",
        )
        assertTrue(
            jni("nativeLooperIsTrackActive") { it.looperIsTrackActive(track) },
            "importó y la pista $track no quedó activa: lo de abajo no hablaría de 'con dato'",
        )
    }

    @Before
    fun engineUp() {
        assertTrue(jni("nativeStartTuner") { it.startTunerSync() }, "el motor no arrancó")
    }

    /**
     * AC-043.7 — **la serie de pitch del glide cruza con su eje y sus ceros exactos.**
     *
     * Lo que se afirma es lo que el BRIDGE puede romper (el motor ya está medido en C++):
     * el largo real, el origen en `loopStart + W/2`, el paso exacto, el 0/0 sin epsilon en
     * el silencio, el rango en el glide y que dos cruces den lo mismo byte a byte.
     */
    @Test
    fun `la serie de pitch del glide cruza con su eje y sus ceros exactos`() {
        importar(TRACK_GLIDE, "glide-voz.wav", GLIDE_SHA256)
        try {
            val loopStart = jni("nativeLooperGetTrackLoopStart") { it.looperGetTrackLoopStart(TRACK_GLIDE) }
            val loopEnd = jni("nativeLooperGetTrackLoopEnd") { it.looperGetTrackLoopEnd(TRACK_GLIDE) }
            assertEquals(0, loopStart, "el import deja la región en el buffer entero")
            assertEquals(GLIDE_FRAMES, loopEnd, "el import deja la región en el buffer entero")

            val serie: PitchSeries = jni("nativeLooperAnalyzePitch") { it.looperAnalyzePitch(TRACK_GLIDE, 10.0) }

            assertEquals(HOP_FRAMES, serie.hopFrames, "hop 10 ms a 48 kHz son 480 frames, redondeado una vez")
            assertTrue(serie.size > 0, "el glide tiene 4,5 s de material y la serie vino vacía")

            // EL LARGO: el número real de puntos, no la cota ni el buffer reservado. Con
            // ventanas enteras dentro de la región: floor((L − W) / hop) + 1 = 447. La cota
            // con la que el bridge reserva es L / hop + 1 = 451: un bridge que devuelva lo
            // reservado muere acá, con 4 puntos de más que serían 0/0 inventados.
            val esperados = (GLIDE_FRAMES - W_FRAMES) / HOP_FRAMES + 1
            val cota = GLIDE_FRAMES / HOP_FRAMES + 1
            assertEquals(
                esperados, serie.size,
                "la serie tiene que medir los puntos ESCRITOS ($esperados); la cota es $cota — " +
                    "si dio $cota, el bridge devolvió el buffer reservado en vez de recortar al retorno",
            )
            assertEquals(esperados, serie.hz.size)
            assertEquals(esperados, serie.confidence.size)

            // EL EJE: origen en loopStart + W/2, paso exacto.
            assertEquals(
                loopStart + W_FRAMES / 2, serie.frames.first(),
                "el primer punto va al CENTRO de la primera ventana entera: loopStart + W/2",
            )
            for (k in 1 until serie.size) {
                assertEquals(
                    serie.frames[k - 1] + HOP_FRAMES, serie.frames[k],
                    "los puntos distan exactamente hopFrames; el $k no",
                )
            }
            assertTrue(serie.frames.last() <= loopEnd - W_FRAMES / 2, "el último punto va a ≤ loopEnd − W/2")
            assertTrue(
                serie.frames.all { it >= loopStart + W_FRAMES / 2 },
                "ningún frame por debajo de loopStart + W/2: eso sería relleno con ceros",
            )

            // EL 0/0 EXACTO en el silencio, excluyendo ±W/2 alrededor de las fronteras (ahí
            // la ventana mezcla dos regímenes por construcción: es lo que la spec excluye).
            var enSilencio = 0
            for (k in 0 until serie.size) {
                val f = serie.frames[k]
                if (f < SILENCIO_INI + W_FRAMES / 2 || f > SILENCIO_FIN - W_FRAMES / 2) continue
                enSilencio++
                assertEquals(0.0f, serie.hz[k], "frame $f está en el silencio y hz no es 0 exacto")
                assertEquals(0.0f, serie.confidence[k], "frame $f está en el silencio y confidence no es 0 exacto")
            }
            assertTrue(enSilencio >= 40, "el tramo de silencio tiene ~47 puntos; se vieron $enSilencio")

            // EL RANGO en el glide (110 → 440 Hz, con vibrato ±15 c): 100–450, con claridad.
            var enGlide = 0
            for (k in 0 until serie.size) {
                val f = serie.frames[k]
                if (f < loopStart + W_FRAMES / 2 || f > SILENCIO_INI - W_FRAMES / 2) continue
                enGlide++
                assertTrue(
                    serie.hz[k] in 100.0f..450.0f,
                    "frame $f del glide: hz=${serie.hz[k]} fuera de 100–450 (un hueco o una octava cruzó)",
                )
                assertTrue(
                    serie.confidence[k] > 0.5f && serie.confidence[k] <= 1.0f,
                    "frame $f del glide: confidence=${serie.confidence[k]} no es una claridad NSDF con altura",
                )
            }
            assertTrue(enGlide >= 190, "el glide tiene ~197 puntos; se vieron $enGlide")

            // DETERMINISMO a través de la frontera: byte a byte.
            val otraVez = jni("nativeLooperAnalyzePitch") { it.looperAnalyzePitch(TRACK_GLIDE, 10.0) }
            assertEquals(serie.hopFrames, otraVez.hopFrames)
            assertContentEquals(serie.frames, otraVez.frames, "los frames cambiaron entre dos cruces")
            assertContentEquals(serie.hz, otraVez.hz, "los hz cambiaron entre dos cruces")
            assertContentEquals(serie.confidence, otraVez.confidence, "la claridad cambió entre dos cruces")
        } finally {
            jni("nativeLooperClearTrack") { it.looperClearTrack(TRACK_GLIDE) }
        }
    }

    /**
     * AC-043.7 — **la envolvente del audiograma cruza con su origen, su hop y su largo.**
     *
     * `firstFrame == loopStart` (y NO `loopStart + W/2`: las dos series tienen origen
     * distinto, y es un delta que se le dice a NoisyPad), `hopFrames == 480`, `size == 400`
     * = `floor(192000 / 480)` —la cota es 401: un bridge que devuelva lo reservado muere
     * acá—, el golpe del frame 0 por encima de −35 dB y el hueco entre golpes por debajo de
     * −45 dB (los dos umbrales de AC-043.5). Y los onsets de `detectOnsets` caen a ≤ 2 bins
     * de un bin con señal: la misma cruza que hace el test de C++, más laxa, porque acá lo
     * que se afirma es que los DOS ejes cruzan la frontera en los mismos frames.
     */
    @Test
    fun `la envolvente del audiograma cruza con su origen, su hop y su largo`() {
        importar(TRACK_GOLPES, "audiograma-prueba.wav", GOLPES_SHA256)
        try {
            val loopStart = jni("nativeLooperGetTrackLoopStart") { it.looperGetTrackLoopStart(TRACK_GOLPES) }
            assertEquals(0, loopStart)

            val env: LevelEnvelope = jni("nativeLooperGetLevelEnvelope") {
                it.looperGetLevelEnvelope(TRACK_GOLPES, 100.0)
            }

            assertEquals(HOP_FRAMES, env.hopFrames, "100 bins/s a 48 kHz son 480 frames por bin")
            assertEquals(loopStart, env.firstFrame, "la envolvente arranca en loopStart EXACTO, sin W/2")
            val esperados = GOLPES_FRAMES / HOP_FRAMES
            assertEquals(
                esperados, env.size,
                "floor(192000 / 480) = $esperados bins; la cota reservada es ${esperados + 1} — " +
                    "si dio eso, el bridge devolvió el buffer en vez de recortar al retorno",
            )
            assertTrue(env.rms.all { it.isFinite() && it >= 0.0f && it <= 1.0f }, "RMS lineal [0, 1], sin NaN")

            // El golpe del frame 0 (par: ~300 ms a −28 dB) y el hueco antes del segundo (−56 dB).
            assertTrue(db(env.rms[0]) > -35.0, "bin 0 = golpe del frame 0: ${db(env.rms[0])} dB no supera −35")
            assertTrue(db(env.rms[40]) < -45.0, "bin 40 (400 ms) está entre golpes: ${db(env.rms[40])} dB no baja de −45")

            // Los onsets y la envolvente comparten el eje del buffer.
            val onsets = jni("nativeLooperDetectOnsets") { it.looperDetectOnsets(TRACK_GOLPES, 64, 256, 1.0f) }
            assertTrue(onsets.size >= 7, "detectOnsets ve 7 de los 8 golpes (ciego al del frame 0); vio ${onsets.size}")
            for (onset in onsets) {
                val bin = onset / HOP_FRAMES
                val ventana = (bin - 2..bin + 2).filter { it in env.rms.indices }
                assertTrue(
                    ventana.any { db(env.rms[it]) > -35.0 },
                    "onset en el frame $onset (bin $bin) sin un bin > −35 dB a ±2: los dos ejes no coinciden",
                )
            }

            // Determinismo a través de la frontera.
            val otraVez = jni("nativeLooperGetLevelEnvelope") { it.looperGetLevelEnvelope(TRACK_GOLPES, 100.0) }
            assertEquals(env.firstFrame, otraVez.firstFrame)
            assertEquals(env.hopFrames, otraVez.hopFrames)
            assertContentEquals(env.rms, otraVez.rms, "la envolvente cambió entre dos cruces")
        } finally {
            jni("nativeLooperClearTrack") { it.looperClearTrack(TRACK_GOLPES) }
        }
    }

    /**
     * AC-043.6 — **una pista inactiva devuelve 0 elementos en las dos lecturas** (R-API-59),
     * y el hop viene igual: el motor lo calcula antes de mirar la pista, para dimensionar.
     * Y con un hop inválido, ni hop: `hopFrames == 0` y vacío.
     *
     * El `isTrackActive` es el control: si diera `true`, el tamaño 0 hablaría de otra cosa.
     */
    @Test
    fun `una pista inactiva devuelve cero elementos en las dos lecturas`() {
        assertFalse(
            jni("nativeLooperIsTrackActive") { it.looperIsTrackActive(TRACK_VACIA) },
            "la pista tiene que estar INACTIVA para que esto hable de 'sin dato'",
        )

        val serie = jni("nativeLooperAnalyzePitch") { it.looperAnalyzePitch(TRACK_VACIA) }
        assertEquals(0, serie.size, "pista inactiva: 0 puntos, no ${serie.size} ceros")
        assertTrue(serie.isEmpty())
        assertEquals(HOP_FRAMES, serie.hopFrames, "el hop se devuelve aunque no haya dato: sirve para dimensionar")

        val env = jni("nativeLooperGetLevelEnvelope") { it.looperGetLevelEnvelope(TRACK_VACIA) }
        assertEquals(0, env.size, "pista inactiva: 0 bins, no ${env.size} ceros")
        assertEquals(0, env.firstFrame, "sin bins, firstFrame no dice nada y vale 0")
        assertEquals(HOP_FRAMES, env.hopFrames, "el hop se devuelve aunque no haya dato")

        val sinHop = jni("nativeLooperAnalyzePitch") { it.looperAnalyzePitch(TRACK_VACIA, hopMs = 0.0) }
        assertEquals(0, sinHop.size)
        assertEquals(0, sinHop.hopFrames, "un hop inválido no tiene hopFrames")
        val sinBins = jni("nativeLooperGetLevelEnvelope") { it.looperGetLevelEnvelope(TRACK_VACIA, binsPerSecond = 0.0) }
        assertEquals(0, sinBins.size)
        assertEquals(0, sinBins.hopFrames, "un binsPerSecond inválido no tiene hopFrames")
    }
}
