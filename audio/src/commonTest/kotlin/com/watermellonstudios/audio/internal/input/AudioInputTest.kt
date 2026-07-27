package com.watermellonstudios.audio.internal.input

import com.watermellonstudios.audio.api.IInputBridge
import com.watermellonstudios.audio.domain.input.InputMetering
import com.watermellonstudios.audio.domain.input.InputSource
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * WA-5.5 — el camino de entrada visto desde `commonMain`.
 *
 * Lo que se prueba acá **no es el DSP ni la captura**: eso necesita hardware y es
 * lo que el harness va a mirar a mano. Lo que sí vive en commonMain, y hasta
 * ahora no existía, es la traducción entre la superficie nativa y algo con lo
 * que se pueda escribir una UI: el orden de los siete valores del snapshot, el
 * mapeo del enum de fuente, y —lo que más importa— **la distinción entre "no hay
 * medición" y "medición en cero"**.
 *
 * Esa distinción es la razón de ser de la mitad de este archivo. La C API deja
 * el buffer intacto cuando no hay nodo de entrada, con un comentario que dice
 * por qué: para que nadie lea ceros como si alguien los hubiera medido. Si esa
 * intención se pierde en cualquiera de las capas de arriba, el síntoma es un
 * medidor plano y convincente, que es exactamente el modo de falla más caro
 * para un harness cuyo trabajo es contestar "¿esto captura?".
 *
 * El fake implementa sólo [IInputBridge] — 21 métodos. Antes de partirla de
 * [com.watermellonstudios.audio.api.IAudioNativeBridge] habría hecho falta
 * implementar más de cien para escribir este archivo, que es la clase de
 * fricción por la que la lógica se queda sin test.
 */
class AudioInputTest {

    /** Bridge de mentira, con el estado mínimo para observar qué se le pidió. */
    private class FakeInputBridge : IInputBridge {
        var running = false
        var startSucceeds = true
        var sourceId = 0
        var gain = 0f
        var gateEnabled = false
        var gateThreshold: Float? = null
        var monitoring = false
        var monitorVolume = 0f
        var releaseCount = 0
        var snapshot: FloatArray? = null
        var snapshotCalls = 0

        override fun startInputStreamSync(): Boolean {
            running = startSucceeds
            return startSucceeds
        }

        override fun stopInputStreamSync() { running = false }
        override fun isInputStreamRunning(): Boolean = running

        /** El fake abre sincronicamente, asi que nunca queda "abriendo". */
        var starting = false
        override fun isInputStarting(): Boolean = starting
        override fun setInputSourceSync(source: Int) { sourceId = source }
        override fun getInputSource(): Int = sourceId
        override fun setInputGain(gainDb: Float) { gain = gainDb }
        override fun getInputGain(): Float = gain
        override fun setNoiseGateEnabled(enabled: Boolean) { gateEnabled = enabled }
        override fun isNoiseGateEnabled(): Boolean = gateEnabled
        override fun setNoiseGateThreshold(thresholdDb: Float) { gateThreshold = thresholdDb }
        override fun isNoiseGateOpen(): Boolean = false
        override fun getInputLevel(channel: Int): Float = 0f
        override fun getInputLevelLinear(channel: Int): Float = 0f
        override fun isInputClipping(): Boolean = false
        override fun getInputLatencyMs(): Float = 12.5f

        override fun getInputMeteringSnapshot(): FloatArray? {
            snapshotCalls++
            return snapshot
        }

        override fun setMonitoringEnabledSync(enabled: Boolean) { monitoring = enabled }
        override fun isMonitoringEnabled(): Boolean = monitoring
        override fun setMonitoringVolume(volume: Float) { monitorVolume = volume }
        override fun getMonitoringVolume(): Float = monitorVolume
        override fun releaseInputNodeSync() { releaseCount++ }
    }

    private fun inputOver(bridge: FakeInputBridge) = AudioInputImpl(bridge)

    // =======================================================================
    // "No hay medición" no es "medición en cero"
    // =======================================================================

    @Test
    fun meteringIsNullWhenThereIsNoInputNode() {
        val bridge = FakeInputBridge().apply { snapshot = null }

        assertNull(
            inputOver(bridge).metering(),
            "sin nodo de entrada el contrato es null, no un snapshot de ceros: " +
                "un medidor que dibuja ceros ahí está mostrando una medición que " +
                "nadie tomó",
        )
    }

    @Test
    fun aShortSnapshotIsRefusedInsteadOfPaddedWithZeros() {
        // Un array corto significa que el contrato con la capa nativa se rompió.
        // Rellenar con ceros convertiría un bug de frontera en un medidor que
        // anda "casi bien", que es peor que uno que no anda.
        assertNull(InputMetering.fromNative(floatArrayOf(1f, 2f, 3f)))
        assertNull(InputMetering.fromNative(FloatArray(InputMetering.VALUE_COUNT - 1)))
    }

    @Test
    fun theSevenValuesLandInTheOrderTheCApiDocuments() {
        // [0] dB L  [1] dB R  [2] lin L  [3] lin R  [4] clip  [5] gate  [6] latencia
        val m = InputMetering.fromNative(
            floatArrayOf(-6f, -12f, 0.5f, 0.25f, 1f, 1f, 8.5f)
        )

        requireNotNull(m)
        assertEquals(-6f, m.levelDbLeft)
        assertEquals(-12f, m.levelDbRight)
        assertEquals(0.5f, m.levelLeft)
        assertEquals(0.25f, m.levelRight)
        assertTrue(m.isClipping)
        assertTrue(m.isNoiseGateOpen)
        assertEquals(8.5f, m.latencyMs)
    }

    @Test
    fun theBooleanFlagsAreZeroVersusNonZeroNotOneVersusEverythingElse() {
        // El motor escribe 1.0/0.0, pero el contrato en C es "distinto de cero".
        // Fijarlo en == 1f haría que un backend que escriba 2.0 apagara el
        // indicador de clipping justo cuando más importa.
        val clipping = InputMetering.fromNative(
            floatArrayOf(0f, 0f, 0f, 0f, 2f, -1f, 0f)
        )
        requireNotNull(clipping)
        assertTrue(clipping.isClipping)
        assertTrue(clipping.isNoiseGateOpen)

        val quiet = InputMetering.fromNative(FloatArray(InputMetering.VALUE_COUNT))
        requireNotNull(quiet)
        assertFalse(quiet.isClipping)
        assertFalse(quiet.isNoiseGateOpen)
    }

    @Test
    fun peakIsTheLouderChannel() {
        val m = InputMetering.fromNative(floatArrayOf(-6f, -3f, 0.3f, 0.7f, 0f, 0f, 0f))
        requireNotNull(m)
        assertEquals(0.7f, m.peakLinear)
    }

    // =======================================================================
    // El medidor como flujo
    // =======================================================================

    @OptIn(ExperimentalCoroutinesApi::class)
    @Test
    fun theMeteringFlowEmitsOneReadingPerTick() = runTest {
        val bridge = FakeInputBridge().apply {
            snapshot = floatArrayOf(-6f, -6f, 0.5f, 0.5f, 0f, 0f, 10f)
        }

        val readings = inputOver(bridge).meteringFlow(intervalMs = 10).take(3).toList()

        assertEquals(3, readings.size)
        assertTrue(readings.all { it.peakLinear == 0.5f })
    }

    @OptIn(ExperimentalCoroutinesApi::class)
    @Test
    fun theFlowStaysSilentWhileThereIsNothingToMeasure() = runTest {
        // Con el nodo ausente el flujo NO emite silencio inventado: se queda
        // quieto. Un medidor congelado es información honesta; uno en cero
        // parece un micrófono mudo y manda a buscar el bug al lugar equivocado.
        val bridge = FakeInputBridge().apply { snapshot = null }

        var emissions = 0
        val job = backgroundScope.launch {
            inputOver(bridge).meteringFlow(intervalMs = 1).collect { emissions++ }
        }
        testScheduler.advanceTimeBy(50)
        job.cancel()

        assertEquals(0, emissions, "no debería emitir sin nodo de entrada")
        assertTrue(bridge.snapshotCalls > 0, "pero sí debería seguir preguntando")
    }

    // =======================================================================
    // Passthrough y traducción
    // =======================================================================

    @Test
    fun aFailedStartIsReportedAsFalseRatherThanThrowing() {
        // Es como se ve un permiso de micrófono denegado en las dos plataformas.
        val bridge = FakeInputBridge().apply { startSucceeds = false }
        val input = inputOver(bridge)

        assertFalse(input.start())
        assertFalse(input.isRunning)
    }

    @Test
    fun theSourceEnumRoundTripsThroughItsNativeId() {
        val bridge = FakeInputBridge()
        val input = inputOver(bridge)

        InputSource.entries.forEach { source ->
            input.source = source
            assertEquals(source.id, bridge.sourceId, "${source.name} escribió otro id")
            assertEquals(source, input.source)
        }
    }

    @Test
    fun anUnknownSourceIdFallsBackToMicInsteadOfCrashing() {
        val bridge = FakeInputBridge().apply { sourceId = 99 }
        assertEquals(InputSource.MIC, inputOver(bridge).source)
    }

    @Test
    fun theMonitoringVolumeIsClampedBeforeItReachesTheEngine() {
        val bridge = FakeInputBridge()
        val input = inputOver(bridge)

        input.monitoringVolume = 5f
        assertEquals(1f, bridge.monitorVolume)

        input.monitoringVolume = -1f
        assertEquals(0f, bridge.monitorVolume)
    }

    @Test
    fun gainIsNotClampedBecauseTheEngineOwnsThatRange() {
        // dB útiles pueden ser negativos y el rango válido lo define el motor.
        // Recortarlo acá sería inventar una política que la C API no tiene.
        val bridge = FakeInputBridge()
        inputOver(bridge).gainDb = -24f
        assertEquals(-24f, bridge.gain)
    }

    @Test
    fun theNoiseGateThresholdIsWriteOnlyAndSaysSo() {
        // No hay getter en ninguna capa — ni C API, ni JNI. Por eso es función y
        // no propiedad: un `var` tendría que inventar el valor de vuelta.
        val bridge = FakeInputBridge()
        inputOver(bridge).setNoiseGateThresholdDb(-42f)
        assertEquals(-42f, bridge.gateThreshold)
    }

    @Test
    fun stopAndReleaseAreDifferentOperations() {
        // stop() cierra el stream y deja el nodo listo para volver a arrancar;
        // release() suelta el nodo. Colapsarlos haría que un stop rutinario
        // tirara los buffers.
        val bridge = FakeInputBridge()
        val input = inputOver(bridge)

        input.start()
        input.stop()
        assertFalse(bridge.running)
        assertEquals(0, bridge.releaseCount, "stop() no debe soltar el nodo")

        input.release()
        assertEquals(1, bridge.releaseCount)
    }
}
