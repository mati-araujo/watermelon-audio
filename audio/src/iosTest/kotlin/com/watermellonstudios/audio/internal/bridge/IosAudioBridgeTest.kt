package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.domain.effect.EffectType
import com.watermellonstudios.audio.domain.error.NativeBridgeException
import com.watermellonstudios.audio.domain.usb.UsbLatencyProfile
import kotlinx.coroutines.test.runTest
import kotlin.test.AfterTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * WA-3.2 — contrato de [IosAudioBridge] ejecutándose en el simulador.
 *
 * Esto es un escalón por encima de `CinteropSmokeTest`: aquel prueba que el
 * *marshalling* funciona, éste prueba que el **contrato de `IAudioNativeBridge`**
 * se cumple en iOS — `Result` donde corresponde, el mismo mapeo de errores que
 * Android, y las operaciones no soportadas fallando de forma explícita.
 *
 * No arranca el motor: `startEngine()` abre un stream de CoreAudio y volvería los
 * tests flaky por algo ajeno al bridge. Eso es WA-4.3, en device.
 */
class IosAudioBridgeTest {

    private val bridge = IosAudioBridge()

    @AfterTest
    fun cleanup() = kotlinx.coroutines.runBlocking {
        bridge.clearAllEffects()
        Unit
    }

    @Test
    fun theEngineIsInitializedOnConstruction() {
        // Si wma_engine_create() hubiera fallado, el constructor ya habría tirado.
        assertTrue(bridge.isEngineInitialized(), "el motor debería quedar inicializado")
        assertTrue(!bridge.hasInitializationFailed(), "no debería reportar fallo de init")
    }

    @Test
    fun effectChainRoundTripsThroughTheInterface() = runTest {
        assertEquals(0, bridge.getEffectCount())

        val added = bridge.addEffect(EffectType.REVERB)
        assertTrue(added.isSuccess, "addEffect falló: ${added.exceptionOrNull()}")
        assertEquals(0, added.getOrNull())

        assertEquals(1, bridge.getEffectCount())
        assertEquals(EffectType.REVERB, bridge.getEffectType(0))

        assertTrue(bridge.removeEffect(0).isSuccess)
        assertEquals(0, bridge.getEffectCount())
    }

    /**
     * El mapeo error-code→excepción tiene que dar **el mismo tipo** que en
     * Android: los códigos salen de la misma C API y `fromCode()` es el mismo de
     * commonMain. Si esto se rompe, iOS y Android reportan errores distintos para
     * la misma causa.
     */
    @Test
    fun invalidIndexProducesTheSameTypedFailureAsAndroid() = runTest {
        val failure = bridge.removeEffect(99)

        assertTrue(failure.isFailure)
        assertIs<NativeBridgeException>(
            failure.exceptionOrNull(),
            "el fallo debería ser un NativeBridgeException tipado",
        )
    }

    @Test
    fun parametersRoundTripThroughTheInterface() = runTest {
        bridge.addEffect(EffectType.FILTER)

        // Param 0 de FILTER es el cutoff en Hz (FilterEffect.cpp).
        assertTrue(bridge.setParameter(0, 0, 880f).isSuccess)

        val params = bridge.getEffectParameters(0)
        assertEquals(880f, params[0], "el cutoff no sobrevivió el round-trip")
    }

    @Test
    fun bypassRoundTripsThroughTheInterface() = runTest {
        bridge.addEffect(EffectType.DELAY)
        assertEquals(false, bridge.isEffectBypassed(0))

        assertTrue(bridge.setBypass(0, true).isSuccess)
        assertEquals(true, bridge.isEffectBypassed(0))
    }

    /** El snapshot se compone de varias llamadas C bajo un solo lock. */
    @Test
    fun snapshotReflectsTheWholeChain() = runTest {
        bridge.addEffect(EffectType.FILTER)
        bridge.addEffect(EffectType.REVERB)
        bridge.setBypass(1, true)

        val snapshot = bridge.getEffectChainSnapshot()

        assertEquals(2, snapshot.size)
        assertEquals(EffectType.FILTER.id, snapshot.effects[0].typeId)
        assertEquals(EffectType.REVERB.id, snapshot.effects[1].typeId)
        assertEquals(false, snapshot.effects[0].isBypassed)
        assertEquals(true, snapshot.effects[1].isBypassed)
        assertTrue(
            snapshot.effects[0].parameters.isNotEmpty(),
            "el snapshot debería traer los parámetros de cada efecto",
        )
    }

    @Test
    fun clearAllEmptiesTheChain() = runTest {
        bridge.addEffect(EffectType.FILTER)
        bridge.addEffect(EffectType.DELAY)
        assertEquals(2, bridge.getEffectCount())

        assertTrue(bridge.clearAllEffects().isSuccess)
        assertEquals(0, bridge.getEffectCount())
    }

    @Test
    fun batchParametersApplyUnderASingleLock() = runTest {
        bridge.addEffect(EffectType.FILTER)

        val result = bridge.setParametersBatch(0, mapOf(0 to 1500f))

        assertTrue(result.isSuccess, "batch falló: ${result.exceptionOrNull()}")
        assertEquals(1500f, bridge.getEffectParameters(0)[0])
    }

    @Test
    fun realTimeParametersAreAcceptedWithoutAnEngineRunning() {
        // El path de control no depende de que haya stream abierto.
        bridge.setXY(0.5f, 0.5f)
        bridge.setMasterVolume(0.8f)
        bridge.setBpm(128f)

        assertEquals(0.8f, bridge.getMasterVolume(), 1e-6f)
        assertEquals(128f, bridge.getBpm(), 1e-6f)
    }

    @Test
    fun stateVersionAdvancesWhenObservableStateChanges() = runTest {
        val before = bridge.getStateVersion()
        bridge.addEffect(EffectType.CHORUS)
        val after = bridge.getStateVersion()

        assertTrue(after > before, "la versión de estado debería avanzar: $before -> $after")
    }

    /**
     * USB está fuera de alcance en iOS (D4). Lo que importa acá no es que falle,
     * sino que falle **explícitamente**: un consumidor que cree haber configurado
     * un perfil de latencia USB y no lo tiene es peor que uno que sabe que no pudo.
     */
    @Test
    fun usbOperationsFailExplicitlyInsteadOfPretending() = runTest {
        assertEquals(false, bridge.isUsbBackendAvailable())
        assertEquals(false, bridge.createSplitBackend(0, 1))

        val profile = bridge.setUsbLatencyProfile(UsbLatencyProfile.entries.first())
        assertTrue(profile.isFailure, "setUsbLatencyProfile debería fallar en iOS")
        assertNotNull(profile.exceptionOrNull())
    }

    @Test
    fun waveformSamplesFitTheProvidedBuffer() {
        val buffer = FloatArray(64)

        val written = bridge.getWaveformSamples(buffer, buffer.size)

        assertTrue(written >= 0, "no debería devolver negativo")
        assertTrue(written <= buffer.size, "no debería escribir más allá del buffer")
    }

    /** Pedir más muestras que la capacidad no debe desbordar el array. */
    @Test
    fun waveformRequestIsClampedToTheBuffer() {
        val buffer = FloatArray(8)

        val written = bridge.getWaveformSamples(buffer, 1024)

        assertTrue(written <= buffer.size, "escribió $written en un buffer de ${buffer.size}")
    }

    // ==================== LOG CAPTURE ====================
    //
    // A diferencia de todo lo que toca el stream, esto SÍ se puede probar acá: el
    // anillo de logs es memoria del proceso y no necesita ni audio ni permisos.

    /**
     * El camino entero: prender, generar líneas, vaciarlas y que lleguen como
     * `String` de Kotlin.
     *
     * Se apoya en que el motor loguea al agregar un efecto — cualquier operación
     * que registre sirve; lo que se afirma es el **transporte**, no qué dice cada
     * línea. Por eso la assertion es sobre "llegó algo no vacío", que es
     * exactamente lo que se rompería si el `WmaLogBatch` se leyera mal.
     */
    @Test
    fun capturedLogsCrossTheBoundaryAsStrings() = runTest {
        bridge.setLogCaptureEnabled(true)
        try {
            bridge.drainCapturedLogs()  // arranca de cero: el drain es destructivo

            bridge.addEffect(EffectType.REVERB)

            val lines = bridge.drainCapturedLogs()

            assertTrue(lines.isNotEmpty(), "el motor logueó al agregar un efecto y no llegó nada")
            assertTrue(
                lines.all { it.isNotEmpty() },
                "alguna línea llegó vacía: el puntero del batch se leyó mal o se liberó antes",
            )
        } finally {
            bridge.setLogCaptureEnabled(false)
        }
    }

    /** Vaciar dos veces seguidas: la segunda no puede devolver lo mismo que la primera. */
    @Test
    fun drainingIsDestructive() = runTest {
        bridge.setLogCaptureEnabled(true)
        try {
            bridge.addEffect(EffectType.REVERB)
            val first = bridge.drainCapturedLogs()
            assertTrue(first.isNotEmpty(), "el primer drain debería traer algo")

            val second = bridge.drainCapturedLogs()

            assertTrue(
                second.size < first.size,
                "el segundo drain trajo ${second.size} sobre ${first.size}: las líneas no se " +
                    "fueron del anillo",
            )
        } finally {
            bridge.setLogCaptureEnabled(false)
        }
    }

    /**
     * Deshabilitada, la captura no junta nada.
     *
     * Es la mitad que un test de "prende y anda" no cubre, y la que importa para el
     * costo: si el flag no se respetara, toda app que nunca pide logs estaría
     * llenando un anillo de 4000 líneas igual.
     */
    @Test
    fun disabledCaptureCollectsNothing() = runTest {
        bridge.setLogCaptureEnabled(false)
        bridge.drainCapturedLogs()

        bridge.addEffect(EffectType.REVERB)

        assertEquals(0, bridge.drainCapturedLogs().size, "capturó con la captura apagada")
    }

    /** El contador de descartes es acumulado y no se resetea al vaciar. */
    @Test
    fun theDroppedCounterIsReadableAndNonNegative() {
        assertTrue(bridge.getLogCaptureDropped() >= 0, "el contador de descartes no puede ser negativo")
    }
}
