package com.watermellonstudios.audio.domain.usb

/**
 * Complete topology snapshot of a USB Audio device, as extracted by the native
 * descriptor parser and serialized across JNI via UsbSnapshotCodec.
 *
 * This replaces the hardcoded capabilities from parseBasicCapabilities() with
 * data derived from the actual device descriptors.
 *
 * Stage 2 — USB Audio Discovery & Directed Selection.
 */
data class UsbCapabilitySnapshot(
    val vendorId: Int,
    val productId: Int,
    val productName: String,
    val manufacturer: String,
    val serialNumber: String,
    val uacVersion: Int,

    val playbackAltsettings: List<AltsettingInfo>,
    val captureAltsettings: List<AltsettingInfo>,

    val clockSources: List<ClockSourceInfo>,
    val featureUnits: List<FeatureUnitInfo>,
) {
    val isFullDuplex: Boolean
        get() = playbackAltsettings.isNotEmpty() && captureAltsettings.isNotEmpty()

    val hasAsyncFeedback: Boolean
        get() = playbackAltsettings.any { it.hasFeedbackEndpoint }

    /**
     * All distinct sample rates supported by the playback path.
     *
     * UAC 1.0 populates rates directly on each Format Type I descriptor.
     * UAC 2.0 keeps them on the clock source (exposed via RANGE request,
     * populated in stage 3). If altsetting rates are empty, fall back to
     * the union of clock source rates.
     */
    val effectiveOutputSampleRates: List<Int>
        get() {
            val fromAlts = playbackAltsettings.flatMap { alt ->
                alt.formats.flatMap { it.sampleRates }
            }.distinct().sorted()
            if (fromAlts.isNotEmpty()) return fromAlts
            return clockSources.flatMap { it.sampleRates }.distinct().sorted()
        }

    /** All distinct bit depths across all playback altsettings. */
    val effectiveOutputBitDepths: List<Int>
        get() = playbackAltsettings.flatMap { alt ->
            alt.formats.map { it.bitResolution }
        }.filter { it > 0 }.distinct().sorted()

    /** All distinct sample rates supported by the capture path (see output docs). */
    val effectiveInputSampleRates: List<Int>
        get() {
            val fromAlts = captureAltsettings.flatMap { alt ->
                alt.formats.flatMap { it.sampleRates }
            }.distinct().sorted()
            if (fromAlts.isNotEmpty()) return fromAlts
            return clockSources.flatMap { it.sampleRates }.distinct().sorted()
        }

    /** All distinct bit depths across all capture altsettings. */
    val effectiveInputBitDepths: List<Int>
        get() = captureAltsettings.flatMap { alt ->
            alt.formats.map { it.bitResolution }
        }.filter { it > 0 }.distinct().sorted()
}

data class AltsettingInfo(
    val interfaceNumber: Int,
    val alternateSetting: Int,
    val formats: List<AudioFormatInfo>,
    val syncType: UsbSyncMode,
    val hasFeedbackEndpoint: Boolean,
    val hasImplicitFeedback: Boolean,
    val dataEndpointAddress: Int,
    val terminalLinkId: Int,
)

data class AudioFormatInfo(
    val channels: Int,
    val bitResolution: Int,
    val bytesPerSample: Int,
    val sampleRates: List<Int>,
    val hasContinuousRates: Boolean,
    val minSampleRate: Int,
    val maxSampleRate: Int,
)

data class ClockSourceInfo(
    val clockId: Int,
    val type: ClockSourceType,
    val syncedToSof: Boolean,
    val hasFrequencyControl: Boolean,
    val hasValidityControl: Boolean,
    // Stage 3: sample rates from the UAC2 RANGE query
    val sampleRates: List<Int> = emptyList(),
    val hasContinuousRates: Boolean = false,
    val minSampleRate: Int = 0,
    val maxSampleRate: Int = 0,
)

enum class ClockSourceType {
    EXTERNAL,
    INTERNAL_FIXED,
    INTERNAL_VARIABLE,
    INTERNAL_PROGRAMMABLE,
    UNKNOWN;

    companion object {
        fun fromId(id: Int): ClockSourceType = when (id) {
            0 -> EXTERNAL
            1 -> INTERNAL_FIXED
            2 -> INTERNAL_VARIABLE
            3 -> INTERNAL_PROGRAMMABLE
            else -> UNKNOWN
        }
    }
}

data class FeatureUnitInfo(
    val unitId: Int,
    val sourceId: Int,
    val channelCount: Int,
    val hasMasterVolume: Boolean,
    val hasMasterMute: Boolean,
    val perChannelVolume: List<Boolean>,
    val perChannelMute: List<Boolean>,
)

data class ScoredAltsetting(
    val altsetting: AltsettingInfo,
    val format: AudioFormatInfo,
    val formatIndex: Int,
    val score: Double,
    val recommendation: String,
)

/**
 * Stream preference for altsetting selection.
 * Passed to the native selector to influence which altsetting is picked.
 */
data class StreamPreference(
    val preferredSampleRate: Int = 48000,
    val minChannels: Int = 2,
    val requireFeedback: Boolean = false,
    val profile: Profile = Profile.DEFAULT_PRO,
) {
    enum class Profile { DEFAULT_PRO, LOWEST_LATENCY, HIGHEST_FIDELITY, CUSTOM }
}
