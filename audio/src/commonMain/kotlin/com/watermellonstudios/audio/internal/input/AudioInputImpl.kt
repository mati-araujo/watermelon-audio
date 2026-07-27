package com.watermellonstudios.audio.internal.input

import com.watermellonstudios.audio.api.AudioInput
import com.watermellonstudios.audio.api.IInputBridge
import com.watermellonstudios.audio.domain.input.InputMetering
import com.watermellonstudios.audio.domain.input.InputSource
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow

/**
 * [AudioInput] sobre el bridge nativo.
 *
 * Deliberadamente delgada: cada propiedad es un par de llamadas al bridge, y el
 * bridge ya es el mismo contrato en las dos plataformas. Lo único que esta clase
 * agrega es lo que **conviene escribir una sola vez**: el mapeo del enum, el
 * armado del snapshot, y el muestreo del medidor.
 */
internal class AudioInputImpl(
    private val bridge: IInputBridge,
) : AudioInput {

    override fun start(): Boolean = bridge.startInputStreamSync()

    override fun stop() = bridge.stopInputStreamSync()

    override val isRunning: Boolean get() = bridge.isInputStreamRunning()
    override val isStarting: Boolean get() = bridge.isInputStarting()

    override var source: InputSource
        get() = InputSource.fromId(bridge.getInputSource())
        set(value) = bridge.setInputSourceSync(value.id)

    override var gainDb: Float
        get() = bridge.getInputGain()
        set(value) = bridge.setInputGain(value)

    override var monitoringEnabled: Boolean
        get() = bridge.isMonitoringEnabled()
        set(value) = bridge.setMonitoringEnabledSync(value)

    override var monitoringVolume: Float
        get() = bridge.getMonitoringVolume()
        set(value) = bridge.setMonitoringVolume(value.coerceIn(0f, 1f))

    override var noiseGateEnabled: Boolean
        get() = bridge.isNoiseGateEnabled()
        set(value) = bridge.setNoiseGateEnabled(value)

    override fun setNoiseGateThresholdDb(thresholdDb: Float) =
        bridge.setNoiseGateThreshold(thresholdDb)

    override val latencyMs: Float get() = bridge.getInputLatencyMs()

    /**
     * Un solo cruce de frontera por lectura, vía el snapshot.
     *
     * `null` se propaga tal cual: si no hay nodo de entrada, el contrato es
     * decirlo, no devolver ceros que parecen una medición de silencio.
     */
    override fun metering(): InputMetering? =
        bridge.getInputMeteringSnapshot()?.let(InputMetering::fromNative)

    override fun meteringFlow(intervalMs: Long): Flow<InputMetering> = flow {
        val period = intervalMs.coerceAtLeast(1)
        while (true) {
            // Sólo se emite lo que se pudo medir. Sin nodo de entrada el flujo
            // se queda quieto en vez de emitir silencio inventado.
            metering()?.let { emit(it) }
            delay(period)
        }
    }

    override fun release() = bridge.releaseInputNodeSync()
}
