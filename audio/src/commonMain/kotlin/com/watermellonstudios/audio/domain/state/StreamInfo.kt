package com.watermellonstudios.audio.domain.state

/**
 * Information about the active audio stream.
 *
 * @property sampleRate Sample rate in Hz (e.g., 48000)
 * @property bufferSizeInFrames Buffer size in frames
 * @property channelCount Number of audio channels (typically 2 for stereo)
 * @property latencyMillis Estimated latency in milliseconds
 * @property isLowLatency Whether the stream is using low-latency mode
 */
data class StreamInfo(
    val sampleRate: Int = 48000,
    val bufferSizeInFrames: Int = 192,
    val channelCount: Int = 2,
    val latencyMillis: Double = 4.0,
    val isLowLatency: Boolean = true
) {
    companion object {
        val EMPTY = StreamInfo()

        fun fromNativeArray(array: FloatArray?): StreamInfo? {
            if (array == null || array.size < 3) return null
            return StreamInfo(
                sampleRate = array[0].toInt(),
                bufferSizeInFrames = array[1].toInt(),
                latencyMillis = array[2].toDouble()
            )
        }
    }
}
