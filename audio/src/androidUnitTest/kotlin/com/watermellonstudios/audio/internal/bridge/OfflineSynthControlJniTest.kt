package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.IOfflineSynthControl
import com.watermellonstudios.audio.api.OnsetAnalysis
import com.watermellonstudios.audio.domain.error.NativeBridgeException
import org.junit.AfterClass
import java.io.File
import kotlin.math.PI
import kotlin.math.exp
import kotlin.math.sin
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * REQ-055 S15 — **la configuración del re-render y los onsets de un buffer, cruzando de verdad**:
 * las 12 `nativeOffline*` nuevas ([IOfflineSynthControl]) y `nativeDetectOnsets`, contra un
 * `JNIEnv` real y la C API detrás.
 *
 * Lo que sólo se ve cruzando: el rechazo de rangos ANTES de aplicar nada, el handle muerto, el
 * `jintArray` de las consultas del font (y su `null`), y que la entrada no confiable de
 * `nativeDetectOnsets` (tamaños, `out` corto) nunca lee ni escribe fuera de sus arrays. El
 * determinismo y el aislamiento del vivo los mide `core/tests/test_offline_rerender.cpp`.
 *
 * 🔴 Backend FALSO adentro: valida la frontera JNI/Kotlin, no audio en un device. Ver [JniHarness].
 */
class OfflineSynthControlJniTest {

    companion object {
        private const val OWNER = "OfflineSynthControlJniTest"

        private val COVERED = setOf(
            "nativeOfflineCreate",
            "nativeOfflineDestroy",
            "nativeOfflineStart",
            "nativeOfflineRender",
            "nativeOfflineLoadSoundFontFromPath",
            "nativeOfflineAddEffect",
            "nativeOfflineSetOscillatorType",
            "nativeOfflineSetModulatorType",
            "nativeOfflineSetFrequencyRange",
            "nativeOfflineSetBpm",
            "nativeOfflineSetEffectBypass",
            "nativeOfflineSetRoutingMode",
            "nativeOfflineSetParallelMix",
            "nativeOfflineSetFeedbackAmount",
            "nativeOfflineSfSetTouchExpression",
            "nativeOfflineSfNoteOffAll",
            "nativeOfflineGetSoundFontPresetKeyRange",
            "nativeOfflineGetSoundFontPresetBankProgram",
            "nativeDetectOnsets",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)

        private const val RATE = 48_000
        private const val BLOCK = 128
        private const val ERROR_NOT_INITIALIZED = -1
        private const val ERROR_INVALID_EFFECT_INDEX = -2
        private const val ERROR_INVALID_PARAMETER_ID = -3
        private const val ERROR_OUT_OF_RANGE = -4
        private const val DELAY = 2

        private val NAN = Float.NaN
        private val INF = Float.POSITIVE_INFINITY
    }

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name, call)

    private fun newRenderer(): OfflineRenderer {
        JniHarness.requireNativeLibrary()
        val r = OfflineRenderer.create().getOrThrow()
        JniCoverage.record(OWNER, "nativeOfflineCreate")
        return r
    }

    private fun OfflineRenderer.closed() {
        close()
        JniCoverage.record(OWNER, "nativeOfflineDestroy")
    }

    private fun Result<*>.failure(): NativeBridgeException =
        exceptionOrNull() as? NativeBridgeException ?: error("se esperaba un NativeBridgeException, vino $this")

    private fun Result<Unit>.recorded(name: String): Result<Unit> = also { JniCoverage.record(OWNER, name) }

    private fun sf2(): File = File.createTempFile("s15-rerender", ".sf2").apply {
        writeBytes(MinimalSoundFont.bytes())
        deleteOnExit()
    }

    // Bug plausible: un setter vive en la interfaz pero el renderer no la implementa (o la
    // implementa con otro orden de argumentos): NoisyPad compila contra la interfaz y falla en runtime.
    @Test
    fun `AC-055_15 el renderer implementa la interfaz de configuracion y cada setter valido cruza`() {
        newRenderer().let { r ->
            try {
                val control: IOfflineSynthControl = r
                assertTrue(r.start(RATE, BLOCK).recorded("nativeOfflineStart").isSuccess)
                assertTrue(control.setOscillatorType(2).recorded("nativeOfflineSetOscillatorType").isSuccess)
                assertTrue(control.setModulatorType(0).recorded("nativeOfflineSetModulatorType").isSuccess)
                assertTrue(control.setFrequencyRange(110f, 1760f).recorded("nativeOfflineSetFrequencyRange").isSuccess)
                assertTrue(control.setBpm(90f).recorded("nativeOfflineSetBpm").isSuccess)
                assertTrue(control.setBpm(1_000f).recorded("nativeOfflineSetBpm").isSuccess, "el motor recorta, no rechaza")
                assertEquals(0, control.addEffect(DELAY).also { JniCoverage.record(OWNER, "nativeOfflineAddEffect") }.getOrThrow())
                assertTrue(control.setEffectBypass(0, true).recorded("nativeOfflineSetEffectBypass").isSuccess)
                assertTrue(control.setRoutingMode(5).recorded("nativeOfflineSetRoutingMode").isSuccess)
                assertTrue(control.setParallelMix(0.3f).recorded("nativeOfflineSetParallelMix").isSuccess)
                assertTrue(control.setFeedbackAmount(0.4f).recorded("nativeOfflineSetFeedbackAmount").isSuccess)
                assertTrue(control.sfSetTouchExpression(1, 0.5f).recorded("nativeOfflineSfSetTouchExpression").isSuccess)
                assertTrue(control.sfSetTouchExpression(999, 0.5f).recorded("nativeOfflineSfSetTouchExpression").isSuccess,
                    "un toque inexistente lo ignora la C API")
                assertTrue(control.sfNoteOffAll().recorded("nativeOfflineSfNoteOffAll").isSuccess)
                // y el render sigue funcionando con todo eso configurado
                val block = FloatArray(BLOCK * 2)
                assertTrue(r.render(block, BLOCK).recorded("nativeOfflineRender").isSuccess)
            } finally {
                r.closed()
            }
        }
    }

    // Bug plausible: un valor fuera de rango se descarta en silencio (como en vivo) y el re-render
    // suena como otra toma sin avisar; o un no finito llega al clamp de la C API, que deja pasar NaN.
    @Test
    fun `AC-055_15 un valor fuera de rango o no finito se rechaza tipado`() {
        val r = newRenderer()
        try {
            for (t in listOf(-1, 5, Int.MAX_VALUE, Int.MIN_VALUE)) {
                assertIs<NativeBridgeException.ParameterOutOfRange>(
                    r.setOscillatorType(t).recorded("nativeOfflineSetOscillatorType").failure(), "oscilador $t")
            }
            for (m in listOf(-1, 6, Int.MAX_VALUE, Int.MIN_VALUE)) {
                assertIs<NativeBridgeException.ParameterOutOfRange>(
                    r.setRoutingMode(m).recorded("nativeOfflineSetRoutingMode").failure(), "ruteo $m")
            }
            for ((lo, hi) in listOf(100f to 100f, 200f to 100f, NAN to 100f, 100f to NAN, INF to INF, 100f to INF)) {
                assertIs<NativeBridgeException.ParameterOutOfRange>(
                    r.setFrequencyRange(lo, hi).recorded("nativeOfflineSetFrequencyRange").failure(), "rango $lo..$hi")
            }
            assertIs<NativeBridgeException.ParameterOutOfRange>(r.setBpm(NAN).recorded("nativeOfflineSetBpm").failure())
            assertIs<NativeBridgeException.ParameterOutOfRange>(r.setBpm(INF).recorded("nativeOfflineSetBpm").failure())
            assertIs<NativeBridgeException.ParameterOutOfRange>(r.setParallelMix(NAN).recorded("nativeOfflineSetParallelMix").failure())
            assertIs<NativeBridgeException.ParameterOutOfRange>(r.setFeedbackAmount(INF).recorded("nativeOfflineSetFeedbackAmount").failure())
            assertIs<NativeBridgeException.ParameterOutOfRange>(
                r.sfSetTouchExpression(0, NAN).recorded("nativeOfflineSfSetTouchExpression").failure())
            // el modulador devuelve el código de la C API tal cual
            assertIs<NativeBridgeException.InvalidParameterId>(r.setModulatorType(8).recorded("nativeOfflineSetModulatorType").failure())
            assertIs<NativeBridgeException.InvalidParameterId>(r.setModulatorType(-1).recorded("nativeOfflineSetModulatorType").failure())
            // y los bordes válidos pasan
            for (t in 0..4) assertTrue(r.setOscillatorType(t).recorded("nativeOfflineSetOscillatorType").isSuccess)
            for (m in 0..5) assertTrue(r.setRoutingMode(m).recorded("nativeOfflineSetRoutingMode").isSuccess)
            for (t in 0..7) assertTrue(r.setModulatorType(t).recorded("nativeOfflineSetModulatorType").isSuccess)
        } finally {
            r.closed()
        }
    }

    // Bug plausible: el bypass de un índice inexistente pasa como éxito (la cadena no cambia y el
    // re-render cree haber bypasseado), o toca otro efecto por un índice desbordado.
    @Test
    fun `AC-055_15 el bypass de un indice inexistente falla y no toca la cadena`() {
        val r = newRenderer()
        try {
            assertIs<NativeBridgeException.InvalidEffectIndex>(
                r.setEffectBypass(0, true).recorded("nativeOfflineSetEffectBypass").failure(), "cadena vacía")
            assertEquals(0, r.addEffect(DELAY).also { JniCoverage.record(OWNER, "nativeOfflineAddEffect") }.getOrThrow())
            for (i in listOf(1, -1, Int.MAX_VALUE, Int.MIN_VALUE)) {
                assertIs<NativeBridgeException.InvalidEffectIndex>(
                    r.setEffectBypass(i, true).recorded("nativeOfflineSetEffectBypass").failure(), "índice $i")
            }
            assertTrue(r.setEffectBypass(0, true).recorded("nativeOfflineSetEffectBypass").isSuccess)
            assertTrue(r.setEffectBypass(0, false).recorded("nativeOfflineSetEffectBypass").isSuccess)
        } finally {
            r.closed()
        }
    }

    // Bug plausible: las consultas leen el font del motor VIVO (o devuelven el preset 0 siempre) en
    // vez del de la instancia, o un índice inválido devuelve basura en lugar de null.
    @Test
    fun `AC-055_15 las consultas del font devuelven los dos presets del fixture y null sin font`() {
        val r = newRenderer()
        try {
            assertNull(r.getSoundFontPresetKeyRange(0).also { JniCoverage.record(OWNER, "nativeOfflineGetSoundFontPresetKeyRange") },
                "sin font")
            assertNull(r.getSoundFontPresetBankProgram(0).also { JniCoverage.record(OWNER, "nativeOfflineGetSoundFontPresetBankProgram") },
                "sin font")

            assertTrue(r.loadSoundFont(sf2().absolutePath).also { JniCoverage.record(OWNER, "nativeOfflineLoadSoundFontFromPath") }.isSuccess)
            MinimalSoundFont.PRESETS.forEachIndexed { i, p ->
                assertContentEquals(intArrayOf(p.teclaMin, p.teclaMax),
                    r.getSoundFontPresetKeyRange(i).also { JniCoverage.record(OWNER, "nativeOfflineGetSoundFontPresetKeyRange") },
                    "rango del preset $i")
                assertContentEquals(intArrayOf(p.banco, p.programa),
                    r.getSoundFontPresetBankProgram(i).also { JniCoverage.record(OWNER, "nativeOfflineGetSoundFontPresetBankProgram") },
                    "banco/programa del preset $i")
            }
            for (bad in listOf(-1, MinimalSoundFont.PRESETS.size, Int.MAX_VALUE, Int.MIN_VALUE)) {
                assertNull(r.getSoundFontPresetKeyRange(bad), "rango del índice $bad")
                assertNull(r.getSoundFontPresetBankProgram(bad), "banco/programa del índice $bad")
            }
            // otra instancia, sin font: no hereda el de esta
            val other = newRenderer()
            try {
                assertNull(other.getSoundFontPresetKeyRange(0), "el font es por instancia")
            } finally {
                other.closed()
            }
        } finally {
            r.closed()
        }
    }

    // Bug plausible: un handle cerrado cruza igual (use-after-free) o uno desconocido opera sobre otra
    // instancia: las doce nuevas dan -1 / null con el id muerto, 0 o inexistente, aun con el valor fuera de rango.
    @Test
    fun `AC-055_15 con el handle cerrado o desconocido todo falla sin cruzar y las consultas dan null`() {
        val r = newRenderer()
        val stale = r.nativeHandle
        r.closed()
        // el dueño ni siquiera cruza
        assertIs<NativeBridgeException.EngineNotInitialized>(r.setOscillatorType(1).failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(r.setModulatorType(1).failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(r.setFrequencyRange(100f, 200f).failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(r.setBpm(120f).failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(r.setEffectBypass(0, true).failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(r.setRoutingMode(1).failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(r.setParallelMix(0.5f).failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(r.setFeedbackAmount(0.5f).failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(r.sfSetTouchExpression(0, 1f).failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(r.sfNoteOffAll().failure())
        assertNull(r.getSoundFontPresetKeyRange(0))
        assertNull(r.getSoundFontPresetBankProgram(0))

        // el bridge crudo con el id destruido, con 0 y con uno que nunca existió: -1; y el handle
        // muerto gana al rango (nunca se mira el valor de una instancia que no existe)
        for (h in listOf(stale, 0L, Long.MAX_VALUE)) {
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineSetOscillatorType") { it.offlineSetOscillatorType(h, 99) }, "$h")
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineSetModulatorType") { it.offlineSetModulatorType(h, 99) }, "$h")
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineSetFrequencyRange") { it.offlineSetFrequencyRange(h, NAN, 1f) }, "$h")
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineSetBpm") { it.offlineSetBpm(h, NAN) }, "$h")
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineSetEffectBypass") { it.offlineSetEffectBypass(h, 0, true) }, "$h")
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineSetRoutingMode") { it.offlineSetRoutingMode(h, 99) }, "$h")
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineSetParallelMix") { it.offlineSetParallelMix(h, NAN) }, "$h")
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineSetFeedbackAmount") { it.offlineSetFeedbackAmount(h, NAN) }, "$h")
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineSfSetTouchExpression") { it.offlineSfSetTouchExpression(h, 0, 1f) }, "$h")
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineSfNoteOffAll") { it.offlineSfNoteOffAll(h) }, "$h")
            assertNull(jni("nativeOfflineGetSoundFontPresetKeyRange") { it.offlineGetSoundFontPresetKeyRange(h, 0) }, "$h")
            assertNull(jni("nativeOfflineGetSoundFontPresetBankProgram") { it.offlineGetSoundFontPresetBankProgram(h, 0) }, "$h")
        }
    }

    // ---- nativeDetectOnsets / OnsetAnalysis ----

    private fun hits(frames: Int, at: List<Int>): FloatArray {
        val stereo = FloatArray(frames * 2)
        for (n in 0 until frames) {
            val noise = 0.0005f * sin(0.37f * n)
            stereo[2 * n] = noise
            stereo[2 * n + 1] = noise
        }
        for (start in at) {
            for (i in 0 until 6_000) {
                val n = start + i
                if (n >= frames) break
                val v = 0.6f * exp(-i / 1200f) * sin(2f * PI.toFloat() * 220f * i / RATE)
                stereo[2 * n] += v
                stereo[2 * n + 1] += v
            }
        }
        return stereo
    }

    private val hitFrames = listOf(24_000, 36_000, 48_000, 60_000)
    private val totalFrames = 72_000

    // Bug plausible: la ida y vuelta pierde el array de salida (SetIntArrayRegion con otro largo) o
    // el recorte a los escritos falla: el re-render mediría contra onsets inexistentes o ceros de relleno.
    @Test
    fun `AC-055_15 OnsetAnalysis encuentra los golpes por JNI y recorta el array a los escritos`() {
        JniHarness.requireNativeLibrary()
        val stereo = hits(totalFrames, hitFrames)
        val onsets = OnsetAnalysis.detect(stereo, totalFrames, RATE).also { JniCoverage.record(OWNER, "nativeDetectOnsets") }.getOrThrow()
        assertEquals(hitFrames.size, onsets.size, "un onset por golpe y nada de relleno")
        hitFrames.forEachIndexed { k, hit ->
            // el detector ubica el golpe hasta 3 hops ANTES de su frame (ver test_c_api_detect_onsets.cpp)
            assertTrue(onsets[k] in (hit - 3 * 256)..hit, "golpe $k en $hit, detectado en ${onsets[k]}")
        }
        assertTrue(onsets.toList() == onsets.sorted(), "ascendentes")

        // un tope menor al hallado devuelve los PRIMEROS, sin escribir de más
        val first2 = OnsetAnalysis.detect(stereo, totalFrames, RATE, maxOnsets = 2)
            .also { JniCoverage.record(OWNER, "nativeDetectOnsets") }.getOrThrow()
        assertContentEquals(onsets.copyOf(2), first2)

        // silencio: no es error, es vacío
        val silent = OnsetAnalysis.detect(FloatArray(totalFrames * 2), totalFrames, RATE)
            .also { JniCoverage.record(OWNER, "nativeDetectOnsets") }.getOrThrow()
        assertEquals(0, silent.size)
    }

    // Bug plausible: la validación falta o llega tarde: un buffer corto se lee más allá de su largo
    // (heap over-read por JNI) o un frames * 2 desbordado pasa el chequeo de tamaño.
    @Test
    fun `AC-055_15 OnsetAnalysis y nativeDetectOnsets rechazan entradas invalidas sin leer ni escribir fuera`() {
        JniHarness.requireNativeLibrary()
        val stereo = hits(totalFrames, hitFrames)
        fun rejected(message: String, r: Result<IntArray>) =
            assertIs<NativeBridgeException.ParameterOutOfRange>(r.exceptionOrNull(), message)

        rejected("frames 0", OnsetAnalysis.detect(stereo, 0, RATE))
        rejected("frames negativo", OnsetAnalysis.detect(stereo, -1, RATE))
        rejected("buffer corto", OnsetAnalysis.detect(stereo, totalFrames + 1, RATE))
        rejected("frames * 2 desborda Int", OnsetAnalysis.detect(stereo, Int.MAX_VALUE, RATE))
        rejected("rate bajo", OnsetAnalysis.detect(stereo, totalFrames, 7_999))
        rejected("rate alto", OnsetAnalysis.detect(stereo, totalFrames, 192_001))
        rejected("maxOnsets 0", OnsetAnalysis.detect(stereo, totalFrames, RATE, maxOnsets = 0))
        rejected("maxOnsets negativo", OnsetAnalysis.detect(stereo, totalFrames, RATE, maxOnsets = -3))
        // los bordes válidos pasan
        assertTrue(OnsetAnalysis.detect(stereo, totalFrames, 8_000).isSuccess)
        assertTrue(OnsetAnalysis.detect(stereo, totalFrames, 192_000).isSuccess)
        // un maxOnsets enorme no reserva de más ni falla
        assertTrue(OnsetAnalysis.detect(stereo, totalFrames, RATE, maxOnsets = Int.MAX_VALUE).isSuccess)

        // El bridge crudo, saltando la validación de Kotlin: el nativo la repite sobre los arrays REALES.
        val out = IntArray(8) { -7 }
        val bridge = AudioNativeBridge.getInstance()
        assertEquals(ERROR_OUT_OF_RANGE, jni("nativeDetectOnsets") { it.detectOnsets(stereo, totalFrames + 1, RATE, 256, 1f, out) })
        assertEquals(ERROR_OUT_OF_RANGE, bridge.detectOnsets(stereo, Int.MAX_VALUE, RATE, 256, 1f, out), "frames * 2 en 64 bits")
        assertEquals(ERROR_OUT_OF_RANGE, bridge.detectOnsets(stereo, 0, RATE, 256, 1f, out))
        assertEquals(ERROR_OUT_OF_RANGE, bridge.detectOnsets(stereo, totalFrames, RATE, 256, 1f, IntArray(0)), "out vacío")
        assertTrue(out.all { it == -7 }, "un rechazo escribió en out")

        // la capacidad es el largo REAL de out (no un entero aparte): con out de 2 salen exactamente 2,
        // los primeros del buffer completo, y el nativo no escribe un tercero (SetIntArrayRegion lo
        // tiraría como excepción pendiente: el cruce volvería con una ArrayIndexOutOfBounds)
        val small = IntArray(2) { -7 }
        val n = bridge.detectOnsets(stereo, totalFrames, RATE, 256, 1f, small)
        JniCoverage.record(OWNER, "nativeDetectOnsets")
        assertEquals(2, n, "escribe a lo sumo el largo de out")
        assertContentEquals(OnsetAnalysis.detect(stereo, totalFrames, RATE).getOrThrow().copyOf(2), small)
    }

    // Bug plausible: el hop huge desborda `hop * 4` en el guard (UB) y el análisis lee fuera del
    // buffer: con un hop absurdo tiene que dar vacío, no crash.
    @Test
    fun `AC-055_15 un hop absurdo da vacio y no desborda`() {
        JniHarness.requireNativeLibrary()
        val stereo = hits(totalFrames, hitFrames)
        for (hop in listOf(Int.MAX_VALUE, Int.MAX_VALUE / 4 + 1, Int.MIN_VALUE, 0, -5)) {
            val r = OnsetAnalysis.detect(stereo, totalFrames, RATE, hopFrames = hop)
                .also { JniCoverage.record(OWNER, "nativeDetectOnsets") }
            assertNotNull(r.getOrNull(), "hop $hop: ${r.exceptionOrNull()}")
            if (hop > 100_000) assertEquals(0, r.getOrThrow().size, "hop $hop")
        }
    }
}
