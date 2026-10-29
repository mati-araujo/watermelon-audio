package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.ISongBridge
import com.watermellonstudios.audio.api.MasterParams
import com.watermellonstudios.audio.api.OutputProfile
import com.watermellonstudios.audio.api.SongSpace
import com.watermellonstudios.audio.domain.error.NativeBridgeException
import org.junit.AfterClass
import java.io.File
import java.lang.reflect.InvocationTargetException
import java.nio.file.Files
import kotlin.math.abs
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertNotEquals
import kotlin.test.assertTrue

/**
 * REQ-055 S12 (W5) — **la mezcla de la canción, los taps y el procesador de master cruzando de
 * verdad**: las `nativeMix*` del motor vivo, las `nativeOfflineMix*` y `nativeOfflineRenderTaps` por
 * handle y las seis `nativeMaster*` del procesador por id, contra un `JNIEnv` real.
 *
 * Lo que sólo se ve cruzando (el motor lo miden `test_song_mix.cpp`, `test_output_profiles.cpp` y
 * `test_c_api_mix.cpp`):
 *
 * - el `FloatArray` de taps `[premezcla | FX | pista 0 | …]`: la ida y vuelta y un array corto
 *   rechazado ANTES de renderizar o escribir (el motor no avanza ni un bloque);
 * - los `FloatArray` de espacio y de master: nulo o de otro largo se rechaza sin tocar nada;
 * - el id del procesador de master: ida y vuelta del `jlong`, cerrado/desconocido/destruido es
 *   `NOT_INITIALIZED` y nunca memoria, y un id nuevo no reusa el viejo;
 * - el perfil como `jint`: un valor que no es del enum se rechaza antes de convertirlo.
 *
 * 🔴 Backend FALSO adentro: valida la frontera JNI/Kotlin, no audio en un device. Ver [JniHarness].
 */
class SongMixJniTest {

    companion object {
        private const val OWNER = "SongMixJniTest"

        private val COVERED = setOf(
            "nativeMixSetTrack",
            "nativeMixSetSpace",
            "nativeMixSetMaster",
            "nativeMixSetOutputProfile",
            "nativeGetXRunCount",
            "nativeOfflineMixSetTrack",
            "nativeOfflineMixSetSpace",
            "nativeOfflineMixSetMaster",
            "nativeOfflineMixSetOutputProfile",
            "nativeOfflineRenderTaps",
            "nativeMasterCreate",
            "nativeMasterConfigure",
            "nativeMasterReset",
            "nativeMasterProcess",
            "nativeMasterLatency",
            "nativeMasterDestroy",
            "nativeOfflineCreate",
            "nativeOfflineDestroy",
            "nativeOfflineStart",
            "nativeOfflineSetFrequencyAndAmplitude",
            "nativeOfflineSongLoadClip",
            "nativeOfflineSongSetSchedule",
            "nativeOfflineSongGetPlayFrame",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)

        private const val RATE = 48_000
        private const val LOOP = 4_800L
        private const val BLOCK = 480
        private const val T = ISongBridge.MAX_TRACKS
        private const val S = ISongBridge.SILENCE

        private const val ERROR_NOT_INITIALIZED = -1
        private const val ERROR_OUT_OF_RANGE = -4
        private const val ERROR_INVALID_OPERATION = -9

        private val SPACE = SongSpace(0.6f, 0.8f, 1.5f, 0.4f, 12f, 90f, 9_000f, 0.5f, 125f, 0.4f)

        private val dir: File by lazy { Files.createTempDirectory("s12-mix").toFile().apply { deleteOnExit() } }

        private fun clipFile(name: String): String =
            MinimalWav.writeTo(File(dir, "$name.wav").apply { deleteOnExit() }, LOOP.toInt(),
                listOf(MinimalWav.Region(0, LOOP.toInt())))
    }

    private fun <R> jni(name: String, call: (AudioNativeBridge) -> R): R = JniHarness.exercise(OWNER, name, call)

    private fun newRenderer(): OfflineRenderer {
        JniHarness.requireNativeLibrary()
        val r = OfflineRenderer.create().getOrThrow()
        JniCoverage.record(OWNER, "nativeOfflineCreate")
        return r
    }

    private fun newMaster(sampleRate: Int = RATE): MasterProcessor {
        JniHarness.requireNativeLibrary()
        val m = MasterProcessor.create(sampleRate).getOrThrow()
        JniCoverage.record(OWNER, "nativeMasterCreate")
        return m
    }

    private fun row(vararg perTrack: Pair<Int, Int>): IntArray =
        IntArray(T) { S }.also { r -> perTrack.forEach { (track, clip) -> r[track] = clip } }

    private fun Result<*>.failure(): NativeBridgeException =
        exceptionOrNull() as? NativeBridgeException ?: error("se esperaba un NativeBridgeException, vino $this")

    /** Un renderer arrancado, sin instrumento, con una canción de dos pistas lista para sonar. */
    private fun OfflineRenderer.withSong(): OfflineRenderer {
        assertTrue(start(RATE, BLOCK).isSuccess)
        JniCoverage.record(OWNER, "nativeOfflineStart")
        assertTrue(setFrequencyAndAmplitude(440f, 0f).isSuccess)
        JniCoverage.record(OWNER, "nativeOfflineSetFrequencyAndAmplitude")
        val a = songLoadClip(clipFile("a"), RATE, 0, 0, LOOP).getOrThrow()
        JniCoverage.record(OWNER, "nativeOfflineSongLoadClip")
        val p0 = songGetPlayFrame()
        JniCoverage.record(OWNER, "nativeOfflineSongGetPlayFrame")
        assertTrue(songSetSchedule(LOOP, longArrayOf(p0 + 2 * LOOP), row(0 to a), p0 + 12 * LOOP).isSuccess)
        JniCoverage.record(OWNER, "nativeOfflineSongSetSchedule")
        return this
    }

    // ------------------------------------------------------------------------------------
    // El motor vivo: setters, rangos y arrays mal formados
    // ------------------------------------------------------------------------------------

    // Bug plausible: un setter vivo no llega a la C API (el array de espacio mal leído, el perfil
    // truncado), o un valor invalido se aplica en silencio. Cruza y devuelve el codigo tipado.
    @Test
    fun `AC-055_14 los setters de mezcla vivos cruzan y los valores invalidos se rechazan tipados`() {
        val b = AudioNativeBridge.getInstance()
        JniHarness.requireNativeLibrary()
        assertTrue(jni("nativeMixSetTrack") { it.mixSetTrack(0, 0.8f, -0.2f, 0.5f) }.isSuccess)
        assertTrue(jni("nativeMixSetSpace") { it.mixSetSpace(SPACE) }.isSuccess)
        assertTrue(jni("nativeMixSetMaster") { it.mixSetMaster(MasterParams(gainDb = 4f, ceilingDbtp = -2f, releaseMs = 100f)) }.isSuccess)
        assertTrue(jni("nativeMixSetOutputProfile") { it.mixSetOutputProfile(OutputProfile.MASTER) }.isSuccess)
        assertEquals(0, jni("nativeGetXRunCount") { it.getXRunCount() })

        assertIs<NativeBridgeException.ParameterOutOfRange>(
            jni("nativeMixSetTrack") { it.mixSetTrack(0, 2.5f, 0f, 0f) }.failure(), "ganancia fuera de rango")
        assertIs<NativeBridgeException.ParameterOutOfRange>(
            jni("nativeMixSetTrack") { it.mixSetTrack(0, Float.NaN, 0f, 0f) }.failure(), "NaN")
        assertIs<NativeBridgeException.ParameterOutOfRange>(
            jni("nativeMixSetTrack") { it.mixSetTrack(ISongBridge.MAX_TRACKS, 1f, 0f, 0f) }.failure(), "pista inexistente")
        assertIs<NativeBridgeException.ParameterOutOfRange>(
            jni("nativeMixSetSpace") { it.mixSetSpace(SPACE.copy(delayTimeMs = 5_000f)) }.failure())
        assertIs<NativeBridgeException.ParameterOutOfRange>(
            jni("nativeMixSetMaster") { it.mixSetMaster(MasterParams(gainDb = Float.POSITIVE_INFINITY)) }.failure())
        // un motor con device (o sin render offline) no acepta RAW
        assertIs<NativeBridgeException.InvalidOperation>(
            jni("nativeMixSetOutputProfile") { it.mixSetOutputProfile(OutputProfile.RAW) }.failure())
        b.mixSetOutputProfile(OutputProfile.PROTECTED)
    }

    private fun raw(name: String, types: Array<Class<*>>, vararg args: Any?): Any? {
        JniHarness.requireNativeLibrary()
        val m = AudioNativeBridge::class.java.getDeclaredMethod(name, *types).apply { isAccessible = true }
        try {
            return m.invoke(AudioNativeBridge.getInstance(), *args)
        } catch (e: InvocationTargetException) {
            throw AssertionError("$name lanzó al cruzar con un argumento inválido", e.targetException)
        }
    }

    // Bug plausible: GetFloatArrayRegion sobre un array nulo o mas corto que lo que el nativo espera
    // (sobrelectura del heap de la JVM) o una excepcion pendiente. Nulo o de otro largo: rechazo
    // tipado, sin tocar nada y sin excepcion.
    @Test
    fun `AC-055_14 un array de espacio o de master nulo o de otro largo se rechaza sin cruzar mas`() {
        val f = arrayOf<Class<*>>(FloatArray::class.java)
        for (bad in listOf<FloatArray?>(null, FloatArray(0), FloatArray(9), FloatArray(11))) {
            assertEquals(ERROR_OUT_OF_RANGE, raw("nativeMixSetSpace", f, bad), "espacio ${bad?.size}")
        }
        for (bad in listOf<FloatArray?>(null, FloatArray(0), FloatArray(2), FloatArray(4))) {
            assertEquals(ERROR_OUT_OF_RANGE, raw("nativeMixSetMaster", f, bad), "master ${bad?.size}")
        }
        val i = arrayOf<Class<*>>(Int::class.java)
        for (bad in listOf(-1, 3, 7, Int.MIN_VALUE, Int.MAX_VALUE)) {
            assertEquals(ERROR_OUT_OF_RANGE, raw("nativeMixSetOutputProfile", i, bad), "perfil $bad")
        }
        // por handle: lo mismo, y el handle desconocido gana sobre los argumentos
        newRenderer().use { r ->
            val h = r.nativeHandle
            val hf = arrayOf<Class<*>>(Long::class.java, FloatArray::class.java)
            assertEquals(ERROR_OUT_OF_RANGE, raw("nativeOfflineMixSetSpace", hf, h, FloatArray(3)))
            assertEquals(ERROR_OUT_OF_RANGE, raw("nativeOfflineMixSetMaster", hf, h, null))
            assertEquals(ERROR_OUT_OF_RANGE, raw("nativeOfflineMixSetOutputProfile", arrayOf<Class<*>>(Long::class.java, Int::class.java), h, 9))
            assertEquals(ERROR_NOT_INITIALIZED, raw("nativeOfflineMixSetSpace", hf, 0L, FloatArray(3)))
            assertEquals(ERROR_NOT_INITIALIZED, raw("nativeMasterConfigure", hf, 0L, FloatArray(3)))
        }
        // las llamadas de arriba cruzaron por reflexion: anotarlas como ejecutadas
        for (name in listOf("nativeMixSetSpace", "nativeMixSetMaster", "nativeMixSetOutputProfile",
            "nativeOfflineMixSetSpace", "nativeOfflineMixSetMaster", "nativeOfflineMixSetOutputProfile",
            "nativeMasterConfigure")) {
            JniCoverage.record(OWNER, name)
        }
    }

    // ------------------------------------------------------------------------------------
    // El handle: mezcla, taps, ida y vuelta
    // ------------------------------------------------------------------------------------

    // Bug plausible: las regiones del array de taps se pisan o se corren (la premezcla no es la suma
    // de las pistas mas el FX, una pista se lee de la region de otra, el largo se cuenta mal). Con
    // la pista 0 sonando y la 1 muda: la region de la pista 1 es cero, la de la 0 no, y
    // premezcla == FX + pista 0 + pista 1 (1e-6).
    @Test
    fun `AC-055_14 los taps cruzan en un solo array con las regiones en su lugar`() {
        newRenderer().use { r ->
            r.withSong()
            assertTrue(jni("nativeOfflineMixSetOutputProfile") { it.offlineMixSetOutputProfile(r.nativeHandle, OutputProfile.RAW.code) } == 0)
            assertTrue(jni("nativeOfflineMixSetTrack") { it.offlineMixSetTrack(r.nativeHandle, 0, 0.9f, -0.3f, 0.6f) } == 0)
            assertTrue(jni("nativeOfflineMixSetSpace") { it.offlineMixSetSpace(r.nativeHandle, SPACE.toNativeArray()) } == 0)
            assertTrue(jni("nativeOfflineMixSetMaster") { it.offlineMixSetMaster(r.nativeHandle, MasterParams(0f).toNativeArray()) } == 0)

            val tracks = 2
            val region = BLOCK * 2
            val buf = FloatArray(region * (2 + tracks))
            var peakMix = 0f
            var peakFx = 0f
            var peakT0 = 0f
            var peakT1 = 0f
            var worstSum = 0f
            for (b in 0 until (7 * LOOP / BLOCK).toInt()) {
                val result = r.renderTaps(buf, BLOCK, tracks)
                JniCoverage.record(OWNER, "nativeOfflineRenderTaps")
                assertTrue(result.isSuccess, "renderTaps falló: ${result.exceptionOrNull()}")
                for (i in 0 until region) {
                    val mix = buf[i]
                    val fx = buf[region + i]
                    val t0 = buf[2 * region + i]
                    val t1 = buf[3 * region + i]
                    peakMix = maxOf(peakMix, abs(mix)); peakFx = maxOf(peakFx, abs(fx))
                    peakT0 = maxOf(peakT0, abs(t0)); peakT1 = maxOf(peakT1, abs(t1))
                    worstSum = maxOf(worstSum, abs(mix - (fx + t0 + t1)))
                }
            }
            assertTrue(peakT0 > 0.1f, "la pista 0 no suena en su region")
            assertEquals(0f, peakT1, "la region de la pista 1 (muda) tiene señal: las regiones se pisan")
            assertTrue(peakFx > 0.02f, "el bus no devuelve nada")
            assertTrue(worstSum <= 1e-6f, "premezcla != FX + Σ pistas (peor $worstSum)")
            assertTrue(peakMix > 0.1f)
        }
    }

    // Bug plausible: un array de taps mas corto que frames*2*(2+tracks) se escribe igual
    // (sobreescritura del heap) o el motor avanza un bloque antes de enterarse del error. Se rechaza
    // ANTES de renderizar: ni el array ni el play frame cambian.
    @Test
    fun `AC-055_14 un array de taps insuficiente se rechaza sin escribir y sin avanzar el motor`() {
        newRenderer().use { r ->
            r.withSong()
            r.mixSetOutputProfile(OutputProfile.RAW)
            JniCoverage.record(OWNER, "nativeOfflineMixSetOutputProfile")
            val h = r.nativeHandle
            val frozenAt = r.songGetPlayFrame()
            val need = BLOCK * 2 * (2 + 3)
            val short = FloatArray(need - 1) { 7f }
            assertEquals(ERROR_OUT_OF_RANGE, jni("nativeOfflineRenderTaps") { it.offlineRenderTaps(h, short, BLOCK, 3) })
            assertTrue(short.all { it == 7f }, "un array corto se escribió")
            assertEquals(frozenAt, r.songGetPlayFrame(), "el motor avanzó pese al rechazo")
            // el dueño Kotlin ni siquiera cruza
            assertIs<NativeBridgeException.ParameterOutOfRange>(r.renderTaps(short, BLOCK, 3).failure())
            // cuentas que desbordarian un Int: frames enorme, trackCount negativo o mayor que las pistas
            val ok = FloatArray(need)
            assertEquals(ERROR_OUT_OF_RANGE, jni("nativeOfflineRenderTaps") { it.offlineRenderTaps(h, ok, Int.MAX_VALUE, 3) })
            assertEquals(ERROR_OUT_OF_RANGE, jni("nativeOfflineRenderTaps") { it.offlineRenderTaps(h, ok, BLOCK, -1) })
            assertEquals(ERROR_OUT_OF_RANGE, jni("nativeOfflineRenderTaps") { it.offlineRenderTaps(h, ok, BLOCK, 17) })
            assertEquals(ERROR_OUT_OF_RANGE, jni("nativeOfflineRenderTaps") { it.offlineRenderTaps(h, ok, BLOCK + 1, 3) },
                "mas frames que el maximo declarado en start")
            assertEquals(frozenAt, r.songGetPlayFrame())
            // y uno bien formado sigue andando: el rechazo no dejo nada a medias
            assertTrue(r.renderTaps(ok, BLOCK, 3).isSuccess)
            assertTrue(r.songGetPlayFrame() > frozenAt)
            // sin start no hay render
            r.stop()
            assertEquals(ERROR_INVALID_OPERATION, jni("nativeOfflineRenderTaps") { it.offlineRenderTaps(h, ok, BLOCK, 3) })
        }
    }

    // Bug plausible: un handle cerrado cruza igual (use-after-free del motor destruido) o un id
    // reciclado opera sobre otra instancia. Cerrado: EngineNotInitialized SIN cruzar; el id viejo
    // sigue muerto en el registro nativo.
    @Test
    fun `AC-055_14 con el handle cerrado la mezcla y los taps fallan tipados y sin cruzar`() {
        val r = newRenderer()
        val stale = r.nativeHandle
        r.close()
        JniCoverage.record(OWNER, "nativeOfflineDestroy")
        val buf = FloatArray(BLOCK * 2 * 3) { 3f }
        assertIs<NativeBridgeException.EngineNotInitialized>(r.mixSetTrack(0, 1f, 0f, 0f).failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(r.mixSetSpace(SPACE).failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(r.mixSetMaster(MasterParams(0f)).failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(r.mixSetOutputProfile(OutputProfile.RAW).failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(r.renderTaps(buf, BLOCK, 1).failure())
        assertEquals(0, r.getXRunCount())
        // el bridge crudo con el id destruido, con 0 y con uno que nunca existio
        for (h in listOf(stale, 0L, Long.MAX_VALUE)) {
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineRenderTaps") { it.offlineRenderTaps(h, buf, BLOCK, 1) }, "taps $h")
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineMixSetTrack") { it.offlineMixSetTrack(h, 0, 1f, 0f, 0f) })
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineMixSetSpace") { it.offlineMixSetSpace(h, SPACE.toNativeArray()) })
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineMixSetMaster") { it.offlineMixSetMaster(h, MasterParams(0f).toNativeArray()) })
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineMixSetOutputProfile") { it.offlineMixSetOutputProfile(h, 1) })
        }
        assertTrue(buf.all { it == 3f }, "un handle muerto escribió en el array")
    }

    // ------------------------------------------------------------------------------------
    // El procesador de master por id
    // ------------------------------------------------------------------------------------

    private fun sine(frames: Int, amp: Float): FloatArray {
        val out = FloatArray(frames * 2)
        for (n in 0 until frames) {
            val v = amp * kotlin.math.sin(2.0 * Math.PI * 440.0 * n / RATE).toFloat()
            out[n * 2] = v
            out[n * 2 + 1] = v
        }
        return out
    }

    // Bug plausible: el copiado del scratch al FloatArray pierde un tramo, o el estado se reparte
    // con el tamaño de bloque (el limitador ve bloques en vez de muestras). Mismo audio en bloques
    // de 64 y de 4096 => los mismos bits, el techo se sostiene, y la latencia declarada cruza.
    @Test
    fun `AC-055_14 el procesador de master es identico con cualquier bloque y respeta el techo`() {
        val params = MasterParams(gainDb = 9f, ceilingDbtp = -3f, releaseMs = 80f)
        val frames = 8_192
        val input = sine(frames, 0.7f)
        fun run(block: Int): FloatArray {
            newMaster().use { m ->
                assertTrue(jni("nativeMasterConfigure") { m.configure(params) }.isSuccess)
                assertTrue(jni("nativeMasterReset") { m.reset() }.isSuccess)
                val out = input.copyOf()
                var done = 0
                while (done < frames) {
                    val n = minOf(block, frames - done)
                    val chunk = out.copyOfRange(done * 2, (done + n) * 2)
                    val res = jni("nativeMasterProcess") { m.process(chunk, n) }
                    assertTrue(res.isSuccess, "process falló: ${res.exceptionOrNull()}")
                    chunk.copyInto(out, done * 2)
                    done += n
                }
                assertEquals(240, jni("nativeMasterLatency") { m.latencyFrames })
                return out
            }
        }
        val small = run(64)
        val large = run(4096)
        assertContentEquals(large, small, "el resultado depende del tamaño de bloque")
        val ceiling = Math.pow(10.0, -3.0 / 20.0).toFloat()
        assertTrue(small.all { abs(it) <= ceiling }, "una muestra pasó el techo de -3 dBTP")
        assertTrue(small.maxOf { abs(it) } > ceiling * 0.5f, "el master apagó la señal en vez de limitarla")
        assertNotEquals(input.toList(), small.toList())
    }

    // Bug plausible: un id destruido o inventado alcanza memoria muerta, un array corto se
    // sobreescribe, un frames enorme desborda la cuenta de bytes, o un id nuevo reusa el viejo.
    @Test
    fun `AC-055_14 los ids del master invalidos o cerrados no tocan memoria y el array corto no se escribe`() {
        val m = newMaster()
        val stale = m.nativeId
        assertNotEquals(0L, stale)
        val buf = FloatArray(256) { 5f }
        assertIs<NativeBridgeException.ParameterOutOfRange>(m.process(buf, 129).failure(), "frames*2 > size")
        assertTrue(buf.all { it == 5f })
        // por el bridge crudo: un array corto, un frames enorme o negativo
        assertEquals(ERROR_OUT_OF_RANGE, jni("nativeMasterProcess") { it.masterProcess(stale, buf, 129) })
        assertEquals(ERROR_OUT_OF_RANGE, jni("nativeMasterProcess") { it.masterProcess(stale, buf, Int.MAX_VALUE) })
        assertEquals(ERROR_OUT_OF_RANGE, jni("nativeMasterProcess") { it.masterProcess(stale, buf, 0) })
        assertEquals(ERROR_OUT_OF_RANGE, jni("nativeMasterProcess") { it.masterProcess(stale, buf, -4) })
        assertTrue(buf.all { it == 5f }, "un frames invalido escribió")
        assertEquals(ERROR_OUT_OF_RANGE, jni("nativeMasterConfigure") { it.masterConfigure(stale, floatArrayOf(Float.NaN, -1f, 150f)) })

        m.close()
        JniCoverage.record(OWNER, "nativeMasterDestroy")
        assertTrue(!m.isOpen)
        m.close()   // idempotente
        assertIs<NativeBridgeException.EngineNotInitialized>(m.configure(MasterParams(0f)).failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(m.reset().failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(m.process(buf, 64).failure())
        assertEquals(0, m.latencyFrames)
        for (id in listOf(stale, 0L, Long.MAX_VALUE, -1L)) {
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeMasterProcess") { it.masterProcess(id, buf, 64) }, "process $id")
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeMasterReset") { it.masterReset(id) }, "reset $id")
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeMasterConfigure") { it.masterConfigure(id, MasterParams(0f).toNativeArray()) })
            assertEquals(0, jni("nativeMasterLatency") { it.masterLatency(id) })
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeMasterDestroy") { it.masterDestroy(id) }, "destroy $id")
        }
        assertTrue(buf.all { it == 5f }, "un id muerto escribió en el array")

        newMaster().use { fresh ->
            assertNotEquals(stale, fresh.nativeId, "el id destruido se reusó")
            assertEquals(ERROR_NOT_INITIALIZED, AudioNativeBridge.getInstance().masterReset(stale))
        }
        // un sample rate que no sirve no crea nada
        assertEquals(0L, jni("nativeMasterCreate") { it.masterCreate(100) })
        assertEquals(0L, jni("nativeMasterCreate") { it.masterCreate(-48_000) })
        assertTrue(MasterProcessor.create(7_999).isFailure)
        MasterProcessor.create(192_000).getOrThrow().close()   // el techo del rango sirve
    }
}
