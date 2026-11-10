package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.error.NativeBridgeException
import com.watermellonstudios.audio.internal.bridge.JniCoverage
import com.watermellonstudios.audio.internal.bridge.JniHarness
import org.junit.AfterClass
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertNotEquals
import kotlin.test.assertTrue

/**
 * REQ-055 S12b — las fábricas públicas del render offline: un consumidor arma un export y un master
 * con sólo `audio.api`, sin nombrar `audio.internal.bridge` (este archivo vive en `audio.api` y no
 * lo importa para operar: sólo el arnés de cobertura).
 *
 * 🔴 Backend FALSO adentro: valida la frontera JNI/Kotlin, no audio en un device. Ver `JniHarness`.
 */
class OfflineFactoriesJniTest {

    companion object {
        private const val OWNER = "OfflineFactoriesJniTest"

        private val COVERED = setOf(
            "nativeOfflineCreate",
            "nativeOfflineDestroy",
            "nativeOfflineStart",
            "nativeOfflineRender",
            "nativeOfflineStop",
            "nativeOfflineLooperSetCapabilities",
            "nativeMasterCreate",
            "nativeMasterConfigure",
            "nativeMasterProcess",
            "nativeMasterDestroy",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)

        private const val RATE = 48_000
        private const val BLOCK = 480
    }

    private fun hit(vararg natives: String) = natives.forEach { JniCoverage.record(OWNER, it) }

    private fun Result<*>.failure(): NativeBridgeException =
        exceptionOrNull() as? NativeBridgeException ?: error("se esperaba un NativeBridgeException, vino $this")

    // Bug plausible: la fábrica devuelve una instancia que no es la del handle (otra interfaz, un
    // wrapper sin el pasamanos de start/render/stop/capacidades): el ciclo de un export con sólo la
    // interfaz pública no funciona, o dos create() comparten estado.
    @Test
    fun `AC-055_12b la fabrica da una instancia independiente que arranca renderiza y se cierra por la interfaz publica`() {
        JniHarness.requireNativeLibrary()
        val a: IOfflineRenderer = OfflineRendererFactory.create().getOrThrow()
        val b: IOfflineRenderer = OfflineRendererFactory.create().getOrThrow()
        hit("nativeOfflineCreate")
        try {
            assertNotEquals(a, b)
            assertTrue(a.setLooperCapabilities(192L * 1024 * 1024, 12).isSuccess)
            hit("nativeOfflineLooperSetCapabilities")
            assertTrue(a.start(RATE, BLOCK).isSuccess)
            val out = FloatArray(BLOCK * 2) { 7f }
            assertTrue(a.render(out, BLOCK).isSuccess)
            assertTrue(out.none { it == 7f }, "render no sobreescribió el buffer")
            assertTrue(a.stop().isSuccess)
            hit("nativeOfflineStart", "nativeOfflineRender", "nativeOfflineStop")
            // lo pedido a `a` no llegó a `b`: la pista 8 sigue fuera de rango en la otra instancia
            assertTrue(b.start(RATE, BLOCK).isSuccess)
            assertTrue(a.mixSetTrack(8, 1f, 0f, 0f).isSuccess)
            assertIs<NativeBridgeException.ParameterOutOfRange>(b.mixSetTrack(8, 1f, 0f, 0f).failure())
        } finally {
            a.close()
            b.close()
            hit("nativeOfflineDestroy")
        }
        assertIs<NativeBridgeException.EngineNotInitialized>(a.start(RATE, BLOCK).failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(a.render(FloatArray(BLOCK * 2), BLOCK).failure())
        assertIs<NativeBridgeException.EngineNotInitialized>(a.setLooperCapabilities(1, 1).failure())
        a.close()
    }

    // Bug plausible: la fábrica del master deja pasar un rate fuera de 8000..192000 (el limitador
    // se prepara con un rate absurdo), o devuelve un procesador que no cierra.
    @Test
    fun `AC-055_12b la fabrica del master crea procesa y rechaza un sample rate fuera de rango`() {
        JniHarness.requireNativeLibrary()
        for (bad in listOf(0, 7_999, 192_001, -1)) {
            assertIs<NativeBridgeException.ParameterOutOfRange>(MasterProcessorFactory.create(bad).failure(), "rate $bad")
        }
        val m: IMasterProcessor = MasterProcessorFactory.create(RATE).getOrThrow()
        hit("nativeMasterCreate")
        try {
            assertTrue(m.configure(MasterParams(gainDb = 6f)).isSuccess)
            val buf = FloatArray(BLOCK * 2) { 0.5f }
            assertTrue(m.process(buf, BLOCK).isSuccess)
            hit("nativeMasterConfigure", "nativeMasterProcess")
            assertEquals(240, m.latencyFrames)
        } finally {
            m.close()
            hit("nativeMasterDestroy")
        }
        assertIs<NativeBridgeException.EngineNotInitialized>(m.process(FloatArray(2), 1).failure())
        m.close()
    }
}
