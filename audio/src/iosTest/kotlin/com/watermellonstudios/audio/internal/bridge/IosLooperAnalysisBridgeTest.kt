package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.domain.looper.LevelEnvelope
import com.watermellonstudios.audio.domain.looper.PitchSeries
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.usePinned
import platform.Foundation.NSTemporaryDirectory
import platform.posix.fclose
import platform.posix.fopen
import platform.posix.fwrite
import platform.posix.remove
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.log10
import kotlin.math.sin
import kotlin.random.Random
import kotlin.test.AfterTest
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertTrue

/**
 * REQ-043 S2 (AC-043.6, AC-043.7) — `looperAnalyzePitch` y `looperGetLevelEnvelope` sobre
 * cinterop, corriendo en el simulador, con **dato real**: la pista se llena por
 * [com.watermellonstudios.audio.api.ILooperBridge.looperImportTrack] de un WAV float32
 * escrito acá (patrón MINI-030: sin CoreAudio, el import es sincrónico y deja la pista
 * activa).
 *
 * ## Qué se afirma, y qué no
 *
 * El motor está medido contra el oráculo del glide en la suite de C++ y en el arnés JNI;
 * acá no se repite esa medición. Se afirma lo que **este bridge** puede romper solo:
 *
 * - el **largo**: `size == floor((L − W) / hop) + 1`, no la cota `L / hop + 1` con la que
 *   se reserva. Mutante que mata: devolver los arrays reservados en vez de `copyOf(escritos)`
 *   ⇒ 251 en vez de 247 ⇒ rojo en [thePitchSeriesCrossesWithItsAxisAndExactZeros].
 * - el **eje**: primer `frame == loopStart + W/2` (960), paso exacto `hopFrames` (480).
 * - el **0/0 exacto** en el tramo de ceros: sin epsilon.
 * - el pitch de dos tonos con armónicos (220 y 330 Hz) al 1 % con claridad > 0,5.
 * - la envolvente: `firstFrame == loopStart` (0, NO 960: origen distinto del de pitch),
 *   `hopFrames == 480`, `size == floor(L / hop)`, ceros **exactos** en el silencio (la
 *   entrada es cero exacto, así que el RMS también) y señal en los tonos.
 * - **determinismo** a través del binding: dos llamadas, `contentEquals`.
 *
 * El fixture es sintético y se escribe en cada corrida, así que no lleva sha256: su
 * identidad es el generador de abajo.
 */
class IosLooperAnalysisBridgeTest {

    private val bridge = IosAudioBridge()

    private companion object {
        const val RATE = 48_000

        /** `W = 40 ms` del preset de voz del motor, en frames a 48 kHz. */
        const val W_FRAMES = 1_920

        /** 10 ms / 100 bins·s⁻¹ a 48 kHz. */
        const val HOP_FRAMES = 480

        // El fixture: tono A (1,0 s) + CERO EXACTO (0,5 s) + tono B (1,0 s) = 120 000 frames.
        const val TONO_A_HZ = 220.0
        const val TONO_B_HZ = 330.0
        const val SILENCIO_INI = 48_000
        const val SILENCIO_FIN = 72_000
        const val FRAMES = 120_000

        /** 4 armónicos a −6 dB/oct, pico ≈ −20 dBFS: parecido al glide, para ejercer τ y el soporte. */
        const val ARMONICOS = 4
        const val AMPLITUD = 0.05f

        fun db(linear: Float): Double = 20.0 * log10(linear.toDouble().coerceAtLeast(1e-9))
    }

    @AfterTest
    fun cleanup() {
        bridge.looperClearAll()
        bridge.looperSetEnabled(false)
    }

    /**
     * AC-043.6 — una pista inactiva devuelve **0 elementos** en las dos lecturas, y el hop
     * viene igual (el motor lo calcula antes de mirar la pista, para dimensionar). Con un
     * hop inválido, ni hop.
     */
    @Test
    fun anInactiveTrackReturnsZeroElementsInBothReadings() {
        assertFalse(bridge.looperIsTrackActive(0), "la pista tiene que estar INACTIVA para hablar de 'sin dato'")

        val serie = bridge.looperAnalyzePitch(0)
        assertEquals(0, serie.size, "pista inactiva: 0 puntos, no ${serie.size} ceros")
        assertEquals(HOP_FRAMES, serie.hopFrames, "el hop se devuelve aunque no haya dato")

        val env = bridge.looperGetLevelEnvelope(0)
        assertEquals(0, env.size, "pista inactiva: 0 bins, no ${env.size} ceros")
        assertEquals(0, env.firstFrame, "sin bins, firstFrame vale 0")
        assertEquals(HOP_FRAMES, env.hopFrames, "el hop se devuelve aunque no haya dato")

        assertEquals(0, bridge.looperAnalyzePitch(0, hopMs = 0.0).hopFrames, "un hop inválido no tiene hopFrames")
        assertEquals(0, bridge.looperGetLevelEnvelope(0, binsPerSecond = 0.0).hopFrames, "ídem binsPerSecond")
    }

    /**
     * Auditoría de #343 — el piso de `hopMs` (1 ms) y el techo de `binsPerSecond` (1000) se
     * exigen con `require`, no se clampean; en el límite exacto pasan (hop 48 a 48 kHz).
     */
    @Test
    fun aTinyHopOrAnOversizedBinsPerSecondIsRejectedNotClamped() {
        assertFailsWith<IllegalArgumentException> { bridge.looperAnalyzePitch(0, hopMs = 0.5) }
        assertFailsWith<IllegalArgumentException> { bridge.looperGetLevelEnvelope(0, binsPerSecond = 2_000.0) }
        assertEquals(48, bridge.looperAnalyzePitch(0, hopMs = PitchSeries.MIN_HOP_MS).hopFrames)
        assertEquals(48, bridge.looperGetLevelEnvelope(0, binsPerSecond = LevelEnvelope.MAX_BINS_PER_SECOND).hopFrames)
    }

    /** AC-043.7 — la serie de pitch cruza el binding con su eje, su largo real y sus ceros exactos. */
    @OptIn(ExperimentalForeignApi::class)
    @Test
    fun thePitchSeriesCrossesWithItsAxisAndExactZeros() {
        val wav = NSTemporaryDirectory() + "req043-pitch-${randomSuffix()}.wav"
        writeFixture(wav)
        try {
            assertTrue(bridge.looperImportTrack(0, wav, RATE), "el fixture no importó: revisá el escritor contra wav::readWav")
            assertTrue(bridge.looperIsTrackActive(0), "importó y la pista no quedó activa")
            val loopStart = bridge.looperGetTrackLoopStart(0)
            val loopEnd = bridge.looperGetTrackLoopEnd(0)
            assertEquals(0, loopStart)
            assertEquals(FRAMES, loopEnd)

            val serie = bridge.looperAnalyzePitch(0, hopMs = 10.0)

            assertEquals(HOP_FRAMES, serie.hopFrames, "hop 10 ms a 48 kHz son 480 frames")
            val esperados = (FRAMES - W_FRAMES) / HOP_FRAMES + 1
            val cota = FRAMES / HOP_FRAMES + 1
            assertEquals(
                esperados, serie.size,
                "la serie mide los puntos ESCRITOS ($esperados); la cota reservada es $cota — si dio " +
                    "$cota, el bridge devolvió el buffer en vez de recortar al retorno",
            )
            assertEquals(loopStart + W_FRAMES / 2, serie.frames.first(), "el primer punto va a loopStart + W/2")
            for (k in 1 until serie.size) {
                assertEquals(serie.frames[k - 1] + HOP_FRAMES, serie.frames[k], "paso exacto hopFrames; el $k no")
            }
            assertTrue(serie.frames.last() <= loopEnd - W_FRAMES / 2, "el último punto va a ≤ loopEnd − W/2")

            var enSilencio = 0
            var enTonos = 0
            for (k in 0 until serie.size) {
                val f = serie.frames[k]
                val hz = serie.hz[k]
                val conf = serie.confidence[k]
                when {
                    f >= SILENCIO_INI + W_FRAMES / 2 && f <= SILENCIO_FIN - W_FRAMES / 2 -> {
                        enSilencio++
                        assertEquals(0.0f, hz, "frame $f está en el cero exacto y hz=$hz no es 0 exacto")
                        assertEquals(0.0f, conf, "frame $f está en el cero exacto y confidence=$conf no es 0 exacto")
                    }
                    f <= SILENCIO_INI - W_FRAMES / 2 -> {
                        enTonos++
                        assertTrue(abs(hz - TONO_A_HZ) / TONO_A_HZ < 0.01, "frame $f: hz=$hz no es $TONO_A_HZ al 1 %")
                        assertTrue(conf > 0.5f && conf <= 1.0f, "frame $f: confidence=$conf sin altura")
                    }
                    f >= SILENCIO_FIN + W_FRAMES / 2 -> {
                        enTonos++
                        assertTrue(abs(hz - TONO_B_HZ) / TONO_B_HZ < 0.01, "frame $f: hz=$hz no es $TONO_B_HZ al 1 %")
                        assertTrue(conf > 0.5f && conf <= 1.0f, "frame $f: confidence=$conf sin altura")
                    }
                    // ±W/2 alrededor de cada frontera: la ventana mezcla dos regímenes, se excluye.
                }
            }
            assertTrue(enSilencio >= 40, "el tramo de ceros tiene ~47 puntos; se vieron $enSilencio")
            assertTrue(enTonos >= 180, "los dos tonos tienen ~194 puntos; se vieron $enTonos")

            val otraVez = bridge.looperAnalyzePitch(0, hopMs = 10.0)
            assertContentEquals(serie.frames, otraVez.frames, "los frames cambiaron entre dos llamadas")
            assertContentEquals(serie.hz, otraVez.hz, "los hz cambiaron entre dos llamadas")
            assertContentEquals(serie.confidence, otraVez.confidence, "la claridad cambió entre dos llamadas")
        } finally {
            bridge.looperClearTrack(0)
            remove(wav)
        }
    }

    /** AC-043.7 — la envolvente cruza con su origen (`loopStart`, no `+ W/2`), su hop y su largo real. */
    @OptIn(ExperimentalForeignApi::class)
    @Test
    fun theLevelEnvelopeCrossesWithItsOriginHopAndLength() {
        val wav = NSTemporaryDirectory() + "req043-envelope-${randomSuffix()}.wav"
        writeFixture(wav)
        try {
            assertTrue(bridge.looperImportTrack(0, wav, RATE), "el fixture no importó")
            val loopStart = bridge.looperGetTrackLoopStart(0)

            val env = bridge.looperGetLevelEnvelope(0, binsPerSecond = 100.0)

            assertEquals(HOP_FRAMES, env.hopFrames, "100 bins/s a 48 kHz son 480 frames por bin")
            assertEquals(loopStart, env.firstFrame, "la envolvente arranca en loopStart EXACTO, sin W/2")
            val esperados = FRAMES / HOP_FRAMES
            assertEquals(
                esperados, env.size,
                "floor($FRAMES / $HOP_FRAMES) = $esperados bins; la cota reservada es ${esperados + 1} — si dio " +
                    "eso, el bridge devolvió el buffer en vez de recortar al retorno",
            )
            assertTrue(env.rms.all { it.isFinite() && it >= 0.0f && it <= 1.0f }, "RMS lineal [0, 1], sin NaN")

            for (k in 0 until env.size) {
                val ini = env.firstFrame + k * HOP_FRAMES
                when {
                    ini >= SILENCIO_INI && ini + HOP_FRAMES <= SILENCIO_FIN ->
                        assertEquals(0.0f, env.rms[k], "bin $k cubre ceros exactos y su RMS no es 0 exacto")
                    ini + HOP_FRAMES <= SILENCIO_INI || ini >= SILENCIO_FIN ->
                        assertTrue(db(env.rms[k]) > -35.0, "bin $k está en un tono y mide ${db(env.rms[k])} dB")
                }
            }

            val otraVez = bridge.looperGetLevelEnvelope(0, binsPerSecond = 100.0)
            assertEquals(env.firstFrame, otraVez.firstFrame)
            assertContentEquals(env.rms, otraVez.rms, "la envolvente cambió entre dos llamadas")
        } finally {
            bridge.looperClearTrack(0)
            remove(wav)
        }
    }

    // ==================== El fixture ====================

    private fun randomSuffix(): String = Random.nextInt(0, Int.MAX_VALUE).toString(16)

    /**
     * Un `.wav` float32 estéreo de [FRAMES] frames: tono A con [ARMONICOS] armónicos a
     * −6 dB/oct, ceros **exactos**, tono B. Fase por acumulador, sin discontinuidad dentro
     * de cada tramo. Misma cabecera de 44 bytes que `MinimalWav` en el arnés de Android.
     */
    @OptIn(ExperimentalForeignApi::class)
    private fun writeFixture(path: String) {
        val channels = 2
        val blockAlign = channels * 4
        val dataSize = FRAMES * blockAlign
        val bytes = ByteArray(44 + dataSize)
        var at = 0
        fun ascii(s: String) { for (c in s) bytes[at++] = c.code.toByte() }
        fun u16(v: Int) { bytes[at++] = (v and 0xFF).toByte(); bytes[at++] = ((v ushr 8) and 0xFF).toByte() }
        fun u32(v: Int) { u16(v and 0xFFFF); u16((v ushr 16) and 0xFFFF) }
        fun f32(v: Float) = u32(v.toRawBits())

        ascii("RIFF"); u32(36 + dataSize); ascii("WAVE")
        ascii("fmt "); u32(16); u16(3); u16(channels); u32(RATE); u32(RATE * blockAlign)
        u16(blockAlign); u16(32)
        ascii("data"); u32(dataSize)

        var fase = 0.0
        for (i in 0 until FRAMES) {
            val hz = when {
                i < SILENCIO_INI -> TONO_A_HZ
                i < SILENCIO_FIN -> 0.0
                else -> TONO_B_HZ
            }
            if (hz == 0.0) { f32(0.0f); f32(0.0f); fase = 0.0; continue }
            fase += 2.0 * PI * hz / RATE
            var v = 0.0
            for (h in 1..ARMONICOS) v += sin(h * fase) / h
            val muestra = (v * AMPLITUD).toFloat()
            f32(muestra); f32(muestra)
        }
        check(at == bytes.size) { "el fixture quedó en $at bytes de ${bytes.size}" }

        val file = requireNotNull(fopen(path, "wb")) { "no se pudo abrir $path para escribir el fixture" }
        try {
            val written = bytes.usePinned { pinned -> fwrite(pinned.addressOf(0), 1uL, bytes.size.toULong(), file) }
            check(written == bytes.size.toULong()) { "el fixture quedó corto: $written de ${bytes.size} bytes" }
        } finally {
            fclose(file)
        }
    }
}
