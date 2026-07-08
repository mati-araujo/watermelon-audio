package com.watermellonstudios.audio.domain.usb

import kotlinx.coroutines.flow.StateFlow

/**
 * Physical loopback round-trip latency test (Fase 5).
 *
 * The library owns the entire measurement (chirp stimulus, capture, correlation,
 * statistics, state machine); a UI only consumes this API. See
 * `RoundTripMeasurer.{h,cpp}` and `docs/usb_latency/fase_5_test_roundtrip_dispositivo.md`.
 *
 * commonMain contract: no android.* types. StateFlow is from kotlinx-coroutines.
 */

/** Measurement phase, mirrors the native RoundTripMeasurer::Phase ordinals. */
enum class RoundTripTestState(val code: Int) {
    IDLE(0),
    CALIBRATING(1),
    MEASURING(2),
    ANALYZING(3),
    COMPLETE(4),
    ERROR(5);

    companion object {
        fun fromCode(code: Int): RoundTripTestState = entries.find { it.code == code } ?: IDLE
    }
}

/** Terminal error, mirrors the native RoundTripMeasurer::Error ordinals. */
enum class RoundTripTestError(val code: Int) {
    NONE(0),
    NO_SIGNAL(1),
    CLIPPING(2),
    UNRELIABLE(3),
    REQUIRES_FULL_DUPLEX(4),
    STREAM_LOST(5),
    TIMEOUT(6);

    companion object {
        fun fromCode(code: Int): RoundTripTestError = entries.find { it.code == code } ?: NONE
    }
}

/** Live progress emitted during a run. */
data class RoundTripTestProgress(
    val state: RoundTripTestState = RoundTripTestState.IDLE,
    val progressPct: Float = 0f,        // 0..100 (calibration 0-10, bursts 10-90, analysis 90-100)
    val currentBurst: Int = 0,
    val totalBursts: Int = 0,
)

/** Final measurement. */
data class RoundTripTestResult(
    val medianMs: Float,
    val jitterMs: Float,                // = MAD
    val minMs: Float,
    val maxMs: Float,
    val confidence: Float,
    val validBursts: Int,
    val totalBursts: Int,
    val softwareOutputMs: Float,
    val softwareInputMs: Float,
    val residualMs: Float,              // median − (out + in) ≈ converters + URB sched + analog
    val sampleRate: Int,
    val profile: UsbLatencyProfile,
    val error: RoundTripTestError,
) {
    val isSuccess: Boolean get() = error == RoundTripTestError.NONE

    companion object {
        fun error(err: RoundTripTestError, profile: UsbLatencyProfile = UsbLatencyProfile.SAFE) =
            RoundTripTestResult(
                medianMs = -1f, jitterMs = 0f, minMs = 0f, maxMs = 0f, confidence = 0f,
                validBursts = 0, totalBursts = 0, softwareOutputMs = 0f, softwareInputMs = 0f,
                residualMs = 0f, sampleRate = 0, profile = profile, error = err,
            )
    }
}

/** Tunable parameters for a run. */
data class RoundTripTestConfig(
    val burstCount: Int = 10,
    val burstIntervalMs: Int = 300,
    val amplitude: Float = 0.25f,
    val searchWindowMs: Int = 250,
) {
    /** Packed encoding consumed by the native start call. */
    fun toFloatArray(): FloatArray =
        floatArrayOf(burstCount.toFloat(), burstIntervalMs.toFloat(), amplitude, searchWindowMs.toFloat())
}

/**
 * Drives one round-trip measurement over the running USB backend. The
 * implementation polls the native measurer; the UI collects [progress] and
 * awaits [run].
 */
interface IRoundTripLatencyTester {
    val progress: StateFlow<RoundTripTestProgress>

    /**
     * Run a full measurement, suspending until it reaches a terminal state.
     * Returns a result whose [RoundTripTestResult.error] is non-NONE on failure
     * (e.g. [RoundTripTestError.REQUIRES_FULL_DUPLEX] if the stream isn't duplex).
     */
    suspend fun run(config: RoundTripTestConfig = RoundTripTestConfig()): RoundTripTestResult

    /** Cancel an in-flight run; restores the backend's original callback. */
    fun cancel()
}
