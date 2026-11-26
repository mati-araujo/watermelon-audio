package com.watermellonstudios.studiospike

/**
 * Shim JNI de la espiga (`libstudio_spike_jni.dylib`, src/main/native/spike_jni.cpp)
 * sobre `CoreAudioMacDiag.h`. Todo es lectura de atomics escritos por el thread de audio;
 * nada de esto corre en ese thread.
 */
object SpikeNative {
    /** [blocks, overBudget, discontinuities, sampleTimeInvalid, halOverloads,
     *  maxRenderNs, sumRenderNs, lastFrames, minFrames, maxFrames, touchMarks] */
    external fun renderStats(): LongArray?

    /** [deviceId, nominalRate, streamRate, bufReq, bufActual, bufMin, bufMax, devLat, safety,
     *  streamLat, presentationLatencySec, bufferSetStatus, listenerStatus] */
    external fun deviceInfo(): DoubleArray?

    external fun reset()
    external fun markTouch()
    external fun nowNs(): Long

    /** [touchNs, renderEntryNs, bufferHostNs, frames] x n; null si el reset no se aplicó aún. */
    external fun touchSamples(): LongArray?
}

data class RenderStats(
    val blocks: Long, val overBudget: Long, val discontinuities: Long,
    val sampleTimeInvalid: Long, val halOverloads: Long, val maxRenderNs: Long,
    val sumRenderNs: Long, val lastFrames: Long, val minFrames: Long, val maxFrames: Long,
    val touchMarks: Long,
) {
    val meanRenderUs: Double get() = if (blocks > 0) sumRenderNs / 1e3 / blocks else 0.0

    companion object {
        fun read(): RenderStats? = SpikeNative.renderStats()?.let {
            RenderStats(it[0], it[1], it[2], it[3], it[4], it[5], it[6], it[7], it[8], it[9], it[10])
        }
    }
}

data class DeviceInfo(
    val deviceId: Int, val nominalRate: Double, val rate: Double, val bufferRequested: Int, val bufferActual: Int,
    val bufferMin: Int, val bufferMax: Int, val deviceLatency: Int, val safetyOffset: Int,
    val streamLatency: Int, val presentationLatencySec: Double, val bufferSetStatus: Int,
    val listenerStatus: Int,
) {
    private fun ms(frames: Int) = frames / rate * 1000.0

    /** Del buffer entregado (mHostTime) al conversor: lo que el dispositivo declara. */
    val hostToDacMs: Double get() = ms(deviceLatency + streamLatency)

    /** Latencia de salida total que declara el HAL: dispositivo + safety + stream + buffer. */
    val totalOutputMs: Double get() = ms(deviceLatency + safetyOffset + streamLatency + bufferActual)

    companion object {
        fun read(): DeviceInfo? = SpikeNative.deviceInfo()?.let {
            DeviceInfo(
                it[0].toInt(), it[1], it[2], it[3].toInt(), it[4].toInt(), it[5].toInt(), it[6].toInt(),
                it[7].toInt(), it[8].toInt(), it[9].toInt(), it[10], it[11].toInt(), it[12].toInt(),
            )
        }
    }
}

data class TouchSample(val touchNs: Long, val renderEntryNs: Long, val bufferHostNs: Long, val frames: Int)

fun readTouchSamples(): List<TouchSample>? {
    val flat = SpikeNative.touchSamples() ?: return null
    return (0 until flat.size / 4).map { i ->
        TouchSample(flat[4 * i], flat[4 * i + 1], flat[4 * i + 2], flat[4 * i + 3].toInt())
    }
}
