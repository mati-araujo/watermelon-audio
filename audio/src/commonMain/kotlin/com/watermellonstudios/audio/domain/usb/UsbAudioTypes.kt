package com.watermellonstudios.audio.domain.usb

/**
 * USB Audio domain types.
 *
 * Pure data classes without Android framework dependencies.
 */

/**
 * Represents a connected USB Audio device.
 */
data class UsbAudioDevice(
    val deviceId: String,
    val vendorId: Int,
    val productId: Int,
    val deviceName: String,
    val manufacturerName: String?,
    val serialNumber: String?,
    val capabilities: UsbAudioCapabilities
) {
    /**
     * Unique identifier for display purposes.
     */
    val displayName: String
        get() = manufacturerName?.let { "$it $deviceName" } ?: deviceName

    /**
     * VID:PID string for debugging.
     */
    val vidPid: String
        get() = String.format("%04X:%04X", vendorId, productId)
}

/**
 * Audio capabilities of a USB device.
 */
data class UsbAudioCapabilities(
    val supportedSampleRates: List<Int>,
    val supportedBitDepths: List<Int>,
    val maxChannelsOutput: Int,
    val maxChannelsInput: Int,
    val syncMode: UsbSyncMode,
    val supportsFullDuplex: Boolean,
    val uacVersion: Int = 1
) {
    /**
     * Check if device supports a specific sample rate.
     */
    fun supportsSampleRate(rate: Int): Boolean = supportedSampleRates.contains(rate)

    /**
     * Check if device has playback capability.
     */
    val hasPlayback: Boolean get() = maxChannelsOutput > 0

    /**
     * Check if device has capture capability.
     */
    val hasCapture: Boolean get() = maxChannelsInput > 0

    companion object {
        /**
         * Empty capabilities for unknown devices.
         */
        val UNKNOWN = UsbAudioCapabilities(
            supportedSampleRates = emptyList(),
            supportedBitDepths = emptyList(),
            maxChannelsOutput = 0,
            maxChannelsInput = 0,
            syncMode = UsbSyncMode.UNKNOWN,
            supportsFullDuplex = false
        )
    }
}

/**
 * USB Audio synchronization mode.
 */
enum class UsbSyncMode(val id: Int, val displayName: String) {
    UNKNOWN(0, "Unknown"),
    SYNCHRONOUS(1, "Synchronous"),
    ADAPTIVE(2, "Adaptive"),
    ASYNCHRONOUS(3, "Asynchronous");

    companion object {
        fun fromId(id: Int): UsbSyncMode = entries.find { it.id == id } ?: UNKNOWN
    }
}

/**
 * USB Audio backend type.
 */
enum class AudioBackendType(val id: Int, val displayName: String) {
    NONE(0, "None"),
    OBOE(1, "Oboe (System)"),
    LIBUSB(2, "USB Direct");

    companion object {
        fun fromId(id: Int): AudioBackendType = entries.find { it.id == id } ?: NONE
    }
}

/**
 * USB device connection state.
 */
enum class UsbConnectionState {
    DISCONNECTED,
    CONNECTING,
    PERMISSION_REQUESTED,
    PERMISSION_GRANTED,
    PERMISSION_DENIED,
    CONNECTED,
    STREAMING,
    ERROR
}

/**
 * USB audio stream configuration.
 */
data class UsbStreamConfig(
    val sampleRate: Int = 48000,
    val channels: Int = 2,
    val bitDepth: Int = 24,
    val framesPerBuffer: Int = 256,
    val numBuffers: Int = 8
) {
    companion object {
        val DEFAULT = UsbStreamConfig()

        val LOW_LATENCY = UsbStreamConfig(
            sampleRate = 48000,
            channels = 2,
            bitDepth = 16,
            framesPerBuffer = 128,
            numBuffers = 4
        )

        val HIGH_QUALITY = UsbStreamConfig(
            sampleRate = 96000,
            channels = 2,
            bitDepth = 24,
            framesPerBuffer = 512,
            numBuffers = 8
        )
    }
}

/**
 * USB transfer statistics.
 */
data class UsbTransferStats(
    // Packet counters
    val packetsSubmitted: Long = 0,
    val packetsCompleted: Long = 0,
    val packetsErrors: Long = 0,

    // Legacy aliases for compatibility
    val packetsTransferred: Long = packetsCompleted,
    val bytesTransferred: Long = 0,

    // Buffer health
    val underruns: Long = 0,
    val overruns: Long = 0,
    val errors: Long = packetsErrors,

    // Latency tracking (ms)
    val currentLatencyMs: Double = 0.0,
    val avgLatencyMs: Double = 0.0,
    val minLatencyMs: Double = 0.0,
    val maxLatencyMs: Double = 0.0,

    // Ring buffer state
    val ringBufferLevel: Int = 0,
    val ringBufferFillPct: Float = 0.0f,
    val ringBufferCapacity: Int = 0,

    // Adaptive buffer stats
    val bufferMs: Int = 0,              // Current ring buffer size in ms
    val healthScore: Float = 100f,      // System health score (0-100)
    val bufferAdjustments: Int = 0,     // Number of buffer size adjustments made

    // Timestamp for stats
    val timestampMs: Long = System.currentTimeMillis()
) {
    /**
     * Check if there are any errors in the stats.
     */
    val hasErrors: Boolean
        get() = underruns > 0 || overruns > 0 || packetsErrors > 0

    /**
     * Calculate packet success rate (0.0 - 1.0).
     */
    val successRate: Float
        get() = if (packetsSubmitted > 0) {
            packetsCompleted.toFloat() / packetsSubmitted.toFloat()
        } else 1.0f

    /**
     * Format latency as a display string.
     */
    val latencyDisplay: String
        get() = String.format("%.2fms (%.1f-%.1f)", avgLatencyMs, minLatencyMs, maxLatencyMs)

    /**
     * Format buffer level as a display string.
     */
    val bufferDisplay: String
        get() = String.format("%d/%d (%.0f%%)", ringBufferLevel, ringBufferCapacity, ringBufferFillPct * 100)
}

/**
 * USB Audio streaming mode.
 */
enum class UsbStreamingMode(val id: Int, val displayName: String) {
    PLAYBACK_ONLY(0, "Playback Only"),
    CAPTURE_ONLY(1, "Capture Only"),
    FULL_DUPLEX(2, "Full Duplex");

    companion object {
        fun fromId(id: Int): UsbStreamingMode = entries.find { it.id == id } ?: PLAYBACK_ONLY
    }
}

/**
 * UAC (USB Audio Class) version.
 */
enum class UacVersion(val id: Int, val displayName: String) {
    UAC_1_0(1, "UAC 1.0"),
    UAC_2_0(2, "UAC 2.0");

    companion object {
        fun fromId(id: Int): UacVersion = when (id) {
            2 -> UAC_2_0
            else -> UAC_1_0
        }
    }
}
