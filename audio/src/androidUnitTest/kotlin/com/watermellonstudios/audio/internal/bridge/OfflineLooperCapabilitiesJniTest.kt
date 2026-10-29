package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.api.ISongBridge
import com.watermellonstudios.audio.domain.error.NativeBridgeException
import org.junit.AfterClass
import java.io.File
import java.nio.file.Files
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertTrue

/**
 * REQ-055 S12b — **las capacidades del looper por handle offline** cruzando de verdad:
 * `nativeOfflineLooperSetCapabilities` contra un `JNIEnv` real y la C API detrás.
 *
 * Sin esto una instancia offline nace con 48 MiB de clips y 8 pistas aunque el vivo del
 * consumidor use 192 MiB. El motor de la cuenta (el presupuesto medido por carga) ya lo mide
 * `test_c_api_song.cpp`; acá se ve lo que sólo se ve cruzando: el `jlong` del presupuesto, el
 * rechazo de rangos ANTES de aplicar nada y el handle muerto.
 *
 * 🔴 Backend FALSO adentro: valida la frontera JNI/Kotlin, no audio en un device. Ver [JniHarness].
 */
class OfflineLooperCapabilitiesJniTest {

    companion object {
        private const val OWNER = "OfflineLooperCapabilitiesJniTest"

        private val COVERED = setOf(
            "nativeOfflineLooperSetCapabilities",
            "nativeOfflineCreate",
            "nativeOfflineDestroy",
            "nativeOfflineStart",
            "nativeOfflineSongLoadClip",
            "nativeOfflineSongSetSchedule",
            "nativeOfflineSongGetPlayFrame",
            "nativeOfflineMixSetTrack",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)

        private const val RATE = 48_000
        private const val BLOCK = 480
        private const val FILE_FRAMES = 4_800

        /** 12 MiB por clip (`frames * 2 * 4`): cuatro llenan los 48 MiB del default. */
        private const val CLIP_FRAMES = 1_572_864L
        private const val MIB = 1024L * 1024L
        private const val DEFAULT_BUDGET_CLIPS = 4

        private const val ERROR_NOT_INITIALIZED = -1
        private const val ERROR_OUT_OF_RANGE = -4

        private val dir: File by lazy { Files.createTempDirectory("s12b-caps").toFile().apply { deleteOnExit() } }

        /** Un archivo chico: el motor lo estira a `CLIP_FRAMES`, que es lo que cuenta el presupuesto. */
        private val clipPath: String by lazy {
            MinimalWav.writeTo(File(dir, "clip.wav").apply { deleteOnExit() }, FILE_FRAMES,
                listOf(MinimalWav.Region(0, FILE_FRAMES)))
        }
    }

    private fun <R> jni(name: String, call: (AudioNativeBridge) -> R): R = JniHarness.exercise(OWNER, name, call)

    private fun newRenderer(): OfflineRenderer {
        JniHarness.requireNativeLibrary()
        val r = OfflineRenderer.create().getOrThrow()
        JniCoverage.record(OWNER, "nativeOfflineCreate")
        assertTrue(r.start(RATE, BLOCK).isSuccess)
        JniCoverage.record(OWNER, "nativeOfflineStart")
        return r
    }

    private fun Result<*>.failure(): NativeBridgeException =
        exceptionOrNull() as? NativeBridgeException ?: error("se esperaba un NativeBridgeException, vino $this")

    private fun OfflineRenderer.load(): Result<Int> =
        songLoadClip(clipPath, RATE, 0, 0, CLIP_FRAMES).also { JniCoverage.record(OWNER, "nativeOfflineSongLoadClip") }

    private fun OfflineRenderer.track(track: Int): Result<Unit> =
        mixSetTrack(track, 1f, 0f, 0f).also { JniCoverage.record(OWNER, "nativeOfflineMixSetTrack") }

    private fun OfflineRenderer.fillDefaultBudget() {
        repeat(DEFAULT_BUDGET_CLIPS) { assertTrue(load().isSuccess, "clip $it") }
    }

    private fun caps(r: OfflineRenderer, bytes: Long, tracks: Int): Result<Unit> =
        r.setLooperCapabilities(bytes, tracks).also { JniCoverage.record(OWNER, "nativeOfflineLooperSetCapabilities") }

    // Bug plausible: el setter no llega al looper de LA INSTANCIA (se aplica al motor vivo, o se
    // trunca el jlong a int): el export de un proyecto grande sigue cayendo en 48 MiB. Con el default
    // el quinto clip de 12 MiB no entra; con 60 MiB entra el quinto y no el sexto; con 192 MiB, sí.
    @Test
    fun `AC-055_12b el presupuesto subido a 192 MiB deja cargar lo que con 48 MiB daba MemoryBudgetExceeded`() {
        val r = newRenderer()
        try {
            r.fillDefaultBudget()
            assertIs<NativeBridgeException.MemoryBudgetExceeded>(r.load().failure(), "con 48 MiB")
            // 60 MiB exactos: entra el quinto y el sexto ya no (el tope se aplica al byte, no se "desbloquea")
            assertTrue(caps(r, 60 * MIB, 0).isSuccess)
            assertTrue(r.load().isSuccess, "quinto clip con 60 MiB")
            assertIs<NativeBridgeException.MemoryBudgetExceeded>(r.load().failure(), "sexto clip con 60 MiB")
            assertTrue(caps(r, 192 * MIB, 0).isSuccess)
            assertTrue(r.load().isSuccess, "sexto clip con 192 MiB")
        } finally {
            r.close()
            JniCoverage.record(OWNER, "nativeOfflineDestroy")
        }
    }

    // Bug plausible: las pistas no cruzan o el 0 se toma por valor: la pista 8 sigue fuera de rango
    // con el default (8 pistas) y entra con 12; un 0 en pistas no las baja ni toca el presupuesto.
    @Test
    fun `AC-055_12b las pistas activas suben con el setter y el cero deja cada valor como estaba`() {
        val r = newRenderer()
        try {
            assertIs<NativeBridgeException.ParameterOutOfRange>(r.track(8).failure(), "default: 8 pistas")
            assertTrue(caps(r, 0, 12).isSuccess)
            assertTrue(r.track(8).isSuccess)
            assertTrue(r.track(11).isSuccess)
            assertIs<NativeBridgeException.ParameterOutOfRange>(r.track(12).failure())
            assertTrue(caps(r, 0, 0).isSuccess, "todo cero es un no-op")
            assertTrue(r.track(11).isSuccess, "el cero no bajó las pistas")
            r.fillDefaultBudget()
            assertIs<NativeBridgeException.MemoryBudgetExceeded>(r.load().failure(), "las pistas no tocaron el presupuesto")
        } finally {
            r.close()
            JniCoverage.record(OWNER, "nativeOfflineDestroy")
        }
    }

    // Bug plausible: un valor fuera de rango se recorta o se aplica a medias (el presupuesto pasa y
    // las pistas no). Cada uno rechaza TIPADO y no cambia NADA del otro.
    @Test
    fun `AC-055_12b un valor fuera de rango se rechaza tipado sin aplicar ninguno de los dos`() {
        val r = newRenderer()
        try {
            val sixtyFourGib = 64L * 1024 * MIB
            val bad = listOf(
                -1L to 0, Long.MIN_VALUE to 0, sixtyFourGib + 1 to 0, Long.MAX_VALUE to 0,
                192 * MIB to -1, 192 * MIB to ISongBridge.MAX_TRACKS + 1, 192 * MIB to Int.MAX_VALUE, 192 * MIB to Int.MIN_VALUE,
            )
            for ((bytes, tracks) in bad) {
                assertIs<NativeBridgeException.ParameterOutOfRange>(caps(r, bytes, tracks).failure(), "$bytes / $tracks")
            }
            // los dos del par válido no se aplicaron: el 5.o clip sigue sin entrar y la pista 8 sigue afuera
            r.fillDefaultBudget()
            assertIs<NativeBridgeException.MemoryBudgetExceeded>(r.load().failure(), "el presupuesto se aplicó a medias")
            assertIs<NativeBridgeException.ParameterOutOfRange>(r.track(8).failure(), "las pistas se aplicaron a medias")
            // y los bordes válidos sí pasan
            assertTrue(caps(r, 64L * 1024 * MIB, ISongBridge.MAX_TRACKS).isSuccess)
            assertTrue(r.track(15).isSuccess)
            assertTrue(r.load().isSuccess)
        } finally {
            r.close()
            JniCoverage.record(OWNER, "nativeOfflineDestroy")
        }
    }

    // Bug plausible: [start] reinicia el looper a los defaults (el export reconfigura el rate y pierde
    // el presupuesto) o bajar el presupuesto con clips adentro los suelta. Se documenta: sobrevive al
    // start, y bajarlo sólo frena las cargas siguientes.
    @Test
    fun `AC-055_12b el setter sobrevive al start y bajarlo con clips cargados no los suelta`() {
        val r = newRenderer()
        try {
            assertTrue(caps(r, 192 * MIB, 12).isSuccess)
            val a = r.load().getOrThrow()
            assertTrue(r.start(RATE, BLOCK).isSuccess)
            JniCoverage.record(OWNER, "nativeOfflineStart")
            // el start invalida los clips (sample rate); lo que importa es que el presupuesto siguió en 192
            repeat(DEFAULT_BUDGET_CLIPS + 1) { assertTrue(r.load().isSuccess, "tras el start, clip $it") }
            assertTrue(r.track(11).isSuccess, "las pistas sobrevivieron al start")

            assertTrue(caps(r, 1 * MIB, 0).isSuccess)
            assertIs<NativeBridgeException.MemoryBudgetExceeded>(r.load().failure(), "bajarlo frena la carga siguiente")
            val p0 = r.songGetPlayFrame()
            JniCoverage.record(OWNER, "nativeOfflineSongGetPlayFrame")
            val row = IntArray(ISongBridge.MAX_TRACKS) { ISongBridge.SILENCE }.also { it[0] = a }
            val scheduled = r.songSetSchedule(4_800, longArrayOf(p0 + 9_600), row, p0 + 96_000)
            JniCoverage.record(OWNER, "nativeOfflineSongSetSchedule")
            // `a` quedó invalidado por el start: la agenda lo rechaza. Un clip posterior al start sigue valiendo.
            assertTrue(scheduled.isFailure, "el clip anterior al start no se puede agendar")
        } finally {
            r.close()
            JniCoverage.record(OWNER, "nativeOfflineDestroy")
        }
    }

    // Bug plausible: un handle cerrado cruza igual, o uno reciclado opera sobre otra instancia.
    @Test
    fun `AC-055_12b con el handle cerrado el setter falla tipado y el id muerto no toca memoria`() {
        val r = newRenderer()
        val stale = r.nativeHandle
        r.close()
        JniCoverage.record(OWNER, "nativeOfflineDestroy")
        assertIs<NativeBridgeException.EngineNotInitialized>(r.setLooperCapabilities(192 * MIB, 12).failure())
        for (h in listOf(stale, 0L, Long.MAX_VALUE)) {
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineLooperSetCapabilities") { it.offlineLooperSetCapabilities(h, 192 * MIB, 12) }, "$h")
            // el handle muerto gana al rango: nunca se mira el valor de una instancia que no existe
            assertEquals(ERROR_NOT_INITIALIZED, jni("nativeOfflineLooperSetCapabilities") { it.offlineLooperSetCapabilities(h, -1, 99) }, "$h fuera de rango")
        }
        // una instancia nueva no hereda lo pedido a la muerta
        val fresh = newRenderer()
        try {
            fresh.fillDefaultBudget()
            assertIs<NativeBridgeException.MemoryBudgetExceeded>(fresh.load().failure())
            assertEquals(ERROR_OUT_OF_RANGE, jni("nativeOfflineLooperSetCapabilities") { it.offlineLooperSetCapabilities(fresh.nativeHandle, -1, 0) })
        } finally {
            fresh.close()
            JniCoverage.record(OWNER, "nativeOfflineDestroy")
        }
    }
}
