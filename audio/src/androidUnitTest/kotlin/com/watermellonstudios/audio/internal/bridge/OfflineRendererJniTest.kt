package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.domain.error.NativeBridgeException
import org.junit.AfterClass
import java.io.File
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertNotEquals
import kotlin.test.assertTrue

/**
 * REQ-055 S2 (AC-055.15, base) — **el render offline por handle, cruzando de verdad**.
 *
 * Las 17 `nativeOffline*` se ejecutan contra un `JNIEnv` real, por [OfflineRenderer]
 * (el dueño del handle) y por el bridge crudo para los casos que el dueño no deja
 * llegar al nativo (handle 0 y handle destruido). Lo que sólo se ve cruzando:
 *
 * - el `jlong` del handle ida y vuelta (un `Int` del lado Kotlin lo truncaría);
 * - el `FloatArray` de salida: `SetFloatArrayRegion` con el largo correcto, y un
 *   array corto rechazado ANTES de escribir;
 * - el `jstring` de la ruta del SoundFont;
 * - y el contrato de ADR-0008 §2 visto desde la JVM: dos renders de la misma
 *   instancia, con [OfflineRenderer.start] en el medio, dan el mismo buffer bit a
 *   bit — que es lo que Studio va a hacer.
 *
 * 🔴 Esto valida la **frontera** sobre un backend falso. El determinismo por engine,
 * la invariancia de bloque, las dos instancias y el vivo intacto los mide la suite de
 * C++ (`core/tests/test_offline_handle.cpp`). Ver el KDoc de [JniHarness].
 */
class OfflineRendererJniTest {

    companion object {
        private const val OWNER = "OfflineRendererJniTest"

        private val COVERED = setOf(
            "nativeOfflineCreate",
            "nativeOfflineDestroy",
            "nativeOfflineStart",
            "nativeOfflineRender",
            "nativeOfflineStop",
            "nativeOfflineSetEngineType",
            "nativeOfflineSetEngineParameter",
            "nativeOfflineSetMasterVolume",
            "nativeOfflineSetXY",
            "nativeOfflineSetFrequencyAndAmplitude",
            "nativeOfflineLoadSoundFontFromPath",
            "nativeOfflineSetSoundFontPreset",
            "nativeOfflineSfNoteOn",
            "nativeOfflineSfNoteOff",
            "nativeOfflineAddEffect",
            "nativeOfflineClearAllEffects",
            "nativeOfflineSetEffectParameter",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)

        private const val RATE = 48_000
        private const val BLOCK = 256
        private const val BLOCKS = 48
        private const val ERROR_NOT_INITIALIZED = -1
        private const val ERROR_OUT_OF_RANGE = -4
        private const val FM = 2
        private const val SOUNDFONT = 6
        private const val HALL_REVERB = 17
        private const val DELAY = 2

        /** "Sonó" (skill medir-dsp): > 0,005. */
        private const val AUDIBLE = 0.005f
    }

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name, call)

    /** Crea por el dueño, anotando el cruce. */
    private fun newRenderer(): OfflineRenderer {
        JniHarness.requireNativeLibrary()
        val r = OfflineRenderer.create().getOrThrow()
        JniCoverage.record(OWNER, "nativeOfflineCreate")
        return r
    }

    private fun OfflineRenderer.op(name: String, call: OfflineRenderer.() -> Result<*>) {
        val result = call()
        JniCoverage.record(OWNER, name)
        assertTrue(result.isSuccess, "$name falló: ${result.exceptionOrNull()}")
    }

    /** Una agenda fija: gestos al principio de bloques fijos, y la cola al final. */
    private fun OfflineRenderer.renderSchedule(): FloatArray {
        op("nativeOfflineStart") { start(RATE, BLOCK) }
        val out = FloatArray(BLOCKS * BLOCK * 2)
        val block = FloatArray(BLOCK * 2)
        for (b in 0 until BLOCKS) {
            if (b % 4 == 0) {
                val amp = if (b < BLOCKS / 2) 0.6f else 0f
                op("nativeOfflineSetFrequencyAndAmplitude") { setFrequencyAndAmplitude(220f + 30f * (b / 4), amp) }
            }
            op("nativeOfflineRender") { render(block, BLOCK) }
            block.copyInto(out, b * BLOCK * 2)
        }
        op("nativeOfflineStop") { stop() }
        return out
    }

    private fun FloatArray.peak(): Float = maxOf { kotlin.math.abs(it) }

    @Test
    fun `AC-055_15 una instancia offline configurada por JNI rinde lo mismo dos veces`() {
        newRenderer().use { r ->
            r.op("nativeOfflineSetEngineType") { setEngineType(FM) }
            r.op("nativeOfflineSetEngineParameter") { setEngineParameter(0, 0.4f) }
            r.op("nativeOfflineSetMasterVolume") { setMasterVolume(0.8f) }
            r.op("nativeOfflineSetXY") { setXY(0.3f, 0.6f) }
            r.op("nativeOfflineClearAllEffects") { clearAllEffects() }
            val reverb = r.addEffect(HALL_REVERB).getOrThrow()
            JniCoverage.record(OWNER, "nativeOfflineAddEffect")
            assertEquals(0, reverb)
            assertEquals(1, r.addEffect(DELAY).getOrThrow())
            r.op("nativeOfflineSetEffectParameter") { setEffectParameter(reverb, 0, 0.7f) }

            val first = r.renderSchedule()
            val second = r.renderSchedule()

            assertTrue(first.peak() > AUDIBLE, "el render salió en silencio (pico ${first.peak()})")
            assertContentEquals(first, second, "el segundo render de la misma instancia no reproduce el primero")
        }
    }

    @Test
    fun `AC-055_15 el SoundFont se carga por ruta y suena por handle`() {
        val sf2 = File.createTempFile("s2-offline", ".sf2").apply {
            writeBytes(MinimalSoundFont.bytes())
            deleteOnExit()
        }
        newRenderer().use { r ->
            r.op("nativeOfflineLoadSoundFontFromPath") { loadSoundFont(sf2.absolutePath) }
            r.op("nativeOfflineSetSoundFontPreset") { setSoundFontPreset(0) }
            r.op("nativeOfflineSetEngineType") { setEngineType(SOUNDFONT) }
            r.op("nativeOfflineStart") { start(RATE, BLOCK) }
            r.op("nativeOfflineSfNoteOn") { soundFontNoteOn(1, 60, 0.9f) }
            val block = FloatArray(BLOCK * 2)
            var peak = 0f
            repeat(20) {
                r.op("nativeOfflineRender") { render(block, BLOCK) }
                peak = maxOf(peak, block.peak())
            }
            r.op("nativeOfflineSfNoteOff") { soundFontNoteOff(1) }
            assertTrue(peak > AUDIBLE, "la nota de SoundFont no sonó (pico $peak)")

            val missing = r.loadSoundFont("/no/existe/s2.sf2")
            assertIs<NativeBridgeException.IoError>(missing.exceptionOrNull())
            val badPreset = r.setSoundFontPreset(999)
            assertIs<NativeBridgeException.ParameterOutOfRange>(badPreset.exceptionOrNull())
        }
    }

    @Test
    fun `AC-055_15 un array corto o un bloque de mas se rechazan tipados y sin escribir`() {
        newRenderer().use { r ->
            r.op("nativeOfflineStart") { start(RATE, BLOCK) }
            val bridge = AudioNativeBridge.getInstance()
            val short = FloatArray(BLOCK) { 7f }   // la mitad de lo que hace falta
            val code = jni("nativeOfflineRender") { it.offlineRender(r.nativeHandle, short, BLOCK) }
            assertEquals(ERROR_OUT_OF_RANGE, code)
            assertTrue(short.all { it == 7f }, "un array corto se escribió igual")

            r.op("nativeOfflineStop") { stop() }
            assertEquals(-9, bridge.offlineRender(r.nativeHandle, FloatArray(BLOCK * 2), BLOCK),
                "render despues de stop: INVALID_OPERATION, sin tocar el motor")
            r.op("nativeOfflineStart") { start(RATE, BLOCK) }

            val big = FloatArray((BLOCK + 1) * 2)
            assertEquals(ERROR_OUT_OF_RANGE, bridge.offlineRender(r.nativeHandle, big, BLOCK + 1),
                "un bloque mayor que el declarado en start")
            assertEquals(ERROR_OUT_OF_RANGE, bridge.offlineRender(r.nativeHandle, big, Int.MAX_VALUE),
                "un frames enorme no puede desbordar la cuenta")
        }
    }

    @Test
    fun `AC-055_15 un handle cerrado o desconocido es un error tipado y nunca un crash`() {
        val r = newRenderer()
        val stale = r.nativeHandle
        assertNotEquals(0L, stale)
        r.close()
        JniCoverage.record(OWNER, "nativeOfflineDestroy")
        assertFalse(r.isOpen)
        r.close()   // idempotente: no cruza de nuevo

        // El dueño ni siquiera cruza.
        assertIs<NativeBridgeException.EngineNotInitialized>(r.start(RATE, BLOCK).exceptionOrNull())
        assertIs<NativeBridgeException.EngineNotInitialized>(
            r.render(FloatArray(BLOCK * 2), BLOCK).exceptionOrNull(),
        )

        // El bridge crudo con el id destruido y con 0: el registro nativo no lo conoce.
        for (h in listOf(stale, 0L, Long.MAX_VALUE)) {
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineStart") { it.offlineStart(h, RATE, BLOCK) }, "start $h")
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineRender") { it.offlineRender(h, FloatArray(BLOCK * 2), BLOCK) })
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineStop") { it.offlineStop(h) })
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineDestroy") { it.offlineDestroy(h) })
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineSetEngineType") { it.offlineSetEngineType(h, FM) })
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineAddEffect") { it.offlineAddEffect(h, DELAY) })
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineSfNoteOn") { it.offlineSfNoteOn(h, 1, 60, 1f) })
        }

        // Y un handle nuevo NO reusa el id del destruido: el viejo sigue muerto.
        newRenderer().use { fresh ->
            assertNotEquals(stale, fresh.nativeHandle)
            assertEquals(ERROR_NOT_INITIALIZED, AudioNativeBridge.getInstance().offlineStart(stale, RATE, BLOCK))
        }
    }

    @Test
    fun `AC-055_15 setters y close de otro hilo durante un render no rompen nada`() {
        // El review de S2 encontro que un `clearAllEffects` a la par de un `render`
        // liberaba los efectos que el render estaba procesando. Ahora toda operacion de
        // una instancia se serializa del lado nativo. 🔴 ESTO ES UN HUMO, NO EL TEST DEL
        // ARREGLO: `clearAllEffects` espera 20 ms antes de liberar y un bloque de 256 tarda
        // menos, asi que sacarle el `opMutex` a los setters NO lo pone rojo (review de
        // seguimiento). Afirma solo que no hay crash y que todo resultado es exito o el
        // error tipado del handle cerrado. La correccion del arreglo es por inspeccion.
        val r = newRenderer()
        r.op("nativeOfflineStart") { start(RATE, BLOCK) }
        val results = java.util.concurrent.ConcurrentLinkedQueue<Result<*>>()
        val renderer = Thread {
            val block = FloatArray(BLOCK * 2)
            repeat(400) { results.add(r.render(block, BLOCK)) }
        }
        renderer.start()
        repeat(50) {
            results.add(r.clearAllEffects())
            results.add(r.addEffect(HALL_REVERB))
            results.add(r.setEngineType(it % 6))
        }
        r.close()
        renderer.join()
        JniCoverage.record(OWNER, "nativeOfflineRender")
        val unexpected = results.filter {
            it.isFailure && it.exceptionOrNull() !is NativeBridgeException.EngineNotInitialized
        }
        assertTrue(unexpected.isEmpty(), "resultados que no son exito ni handle cerrado: $unexpected")
        assertTrue(results.any { it.isSuccess }, "nada llego a ejecutarse")
    }
}
