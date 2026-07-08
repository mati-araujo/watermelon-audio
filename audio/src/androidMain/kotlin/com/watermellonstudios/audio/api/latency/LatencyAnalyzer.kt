package com.watermellonstudios.audio.api.latency

import com.watermellonstudios.audio.internal.bridge.AudioNativeBridge
import com.watermellonstudios.audio.domain.usb.RoundTripTestState
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow

/**
 * High-level API for audio latency analysis and optimization.
 *
 * Provides:
 * - Detailed latency measurements
 * - Optimization recommendations
 * - Round-trip latency testing
 * - Configuration suggestions
 *
 * Usage:
 * ```kotlin
 * val analyzer = LatencyAnalyzer()
 * val info = analyzer.getLatencyInfo()
 * println("Output latency: ${info.outputLatencyMs} ms")
 *
 * // Run optimization test
 * val result = analyzer.runOptimizationTest()
 * if (result.hasImprovement) {
 *     println("Could improve by ${result.improvementPercent}%")
 * }
 * ```
 */
class LatencyAnalyzer {

    /**
     * Detailed latency information.
     */
    data class LatencyInfo(
        val outputLatencyMs: Float,
        val inputLatencyMs: Float,
        val totalLatencyMs: Float,
        val sampleRate: Int,
        val framesPerBurst: Int,
        val bufferCapacity: Int,
        val isAAudio: Boolean,
        val isExclusiveMode: Boolean,
        val isLowLatencyMode: Boolean,
        val burstLatencyMs: Float,
        val bufferLatencyMs: Float
    ) {
        val apiName: String get() = if (isAAudio) "AAudio" else "OpenSL ES"
        val sharingModeName: String get() = if (isExclusiveMode) "Exclusive" else "Shared"
        val performanceModeName: String get() = if (isLowLatencyMode) "LowLatency" else "Normal"

        fun toReportString(): String = buildString {
            appendLine("=== Audio Latency Report ===")
            appendLine("API: $apiName")
            appendLine("Sharing Mode: $sharingModeName")
            appendLine("Performance Mode: $performanceModeName")
            appendLine("Sample Rate: $sampleRate Hz")
            appendLine("Frames Per Burst: $framesPerBurst")
            appendLine("Buffer Capacity: $bufferCapacity frames")
            appendLine("Output Latency: ${"%.1f".format(outputLatencyMs)} ms")
            if (inputLatencyMs > 0) {
                appendLine("Input Latency: ${"%.1f".format(inputLatencyMs)} ms")
                appendLine("Total (est): ${"%.1f".format(totalLatencyMs)} ms")
            }
            appendLine("Burst Latency: ${"%.2f".format(burstLatencyMs)} ms")
            appendLine("Buffer Latency: ${"%.2f".format(bufferLatencyMs)} ms")
        }

        companion object {
            val EMPTY = LatencyInfo(
                outputLatencyMs = -1f,
                inputLatencyMs = -1f,
                totalLatencyMs = -1f,
                sampleRate = 0,
                framesPerBurst = 0,
                bufferCapacity = 0,
                isAAudio = false,
                isExclusiveMode = false,
                isLowLatencyMode = false,
                burstLatencyMs = 0f,
                bufferLatencyMs = 0f
            )
        }
    }

    /**
     * Optimization test result.
     */
    data class OptimizationResult(
        val bestLatencyMs: Float,
        val currentLatencyMs: Float,
        val canUseExclusive: Boolean,
        val improvementMs: Float,
        val improvementPercent: Float
    ) {
        val hasImprovement: Boolean get() = improvementMs > 0.5f

        val recommendation: String get() = when {
            !canUseExclusive && improvementPercent < 5 ->
                "Your current configuration is optimal for this device."
            canUseExclusive && improvementPercent > 10 ->
                "Exclusive mode could reduce latency by ${"%.0f".format(improvementPercent)}%. " +
                "Consider enabling it for professional use."
            improvementPercent in 5.0..10.0 ->
                "Minor improvements possible (${"%.0f".format(improvementPercent)}%). " +
                "Current config is acceptable."
            else ->
                "Latency is already well optimized."
        }

        companion object {
            val EMPTY = OptimizationResult(
                bestLatencyMs = -1f,
                currentLatencyMs = -1f,
                canUseExclusive = false,
                improvementMs = 0f,
                improvementPercent = 0f
            )
        }
    }

    /**
     * Round-trip test state.
     */
    enum class RoundTripState(val code: Int) {
        IDLE(0),
        RUNNING(1),
        COMPLETE(2),
        TIMEOUT(3);

        companion object {
            fun fromCode(code: Int) = entries.find { it.code == code } ?: IDLE
        }
    }

    /**
     * Round-trip test result.
     */
    data class RoundTripResult(
        val latencyMs: Float,
        val state: RoundTripState
    ) {
        val isComplete: Boolean get() = state == RoundTripState.COMPLETE
        val isRunning: Boolean get() = state == RoundTripState.RUNNING
        val wasSuccessful: Boolean get() = isComplete && latencyMs > 0

        companion object {
            val IDLE = RoundTripResult(-1f, RoundTripState.IDLE)
        }
    }

    /**
     * Get current latency information.
     */
    fun getLatencyInfo(): LatencyInfo {
        val data = AudioNativeBridge.getInstance().getDetailedLatencyInfo() ?: return LatencyInfo.EMPTY

        if (data.size < 8) return LatencyInfo.EMPTY

        val sampleRate = data[2].toInt()
        val framesPerBurst = data[3].toInt()
        val bufferCapacity = data[4].toInt()

        val burstLatencyMs = if (sampleRate > 0) {
            (framesPerBurst.toFloat() / sampleRate) * 1000f
        } else 0f

        val bufferLatencyMs = if (sampleRate > 0) {
            (bufferCapacity.toFloat() / sampleRate) * 1000f
        } else 0f

        val outputLatency = data[0]
        val inputLatency = data[1]
        val totalLatency = if (inputLatency > 0) outputLatency + inputLatency else outputLatency

        return LatencyInfo(
            outputLatencyMs = outputLatency,
            inputLatencyMs = inputLatency,
            totalLatencyMs = totalLatency,
            sampleRate = sampleRate,
            framesPerBurst = framesPerBurst,
            bufferCapacity = bufferCapacity,
            isAAudio = data[5] > 0.5f,
            isExclusiveMode = data[6] > 0.5f,
            isLowLatencyMode = data[7] > 0.5f,
            burstLatencyMs = burstLatencyMs,
            bufferLatencyMs = bufferLatencyMs
        )
    }

    /**
     * Run optimization test to find potential improvements.
     *
     * Note: This briefly opens test streams which may cause audio interruption.
     */
    fun runOptimizationTest(): OptimizationResult {
        val data = AudioNativeBridge.getInstance().runLatencyOptimizationTest() ?: return OptimizationResult.EMPTY

        if (data.size < 4) return OptimizationResult.EMPTY

        val bestLatency = data[0]
        val currentLatency = data[1]
        val canUseExclusive = data[2] > 0.5f
        val improvementPercent = data[3]

        val improvementMs = if (bestLatency > 0 && currentLatency > 0) {
            currentLatency - bestLatency
        } else 0f

        return OptimizationResult(
            bestLatencyMs = bestLatency,
            currentLatencyMs = currentLatency,
            canUseExclusive = canUseExclusive,
            improvementMs = improvementMs,
            improvementPercent = improvementPercent
        )
    }

    /**
     * Check if AAudio is available on this device.
     */
    fun isAAudioAvailable(): Boolean = AudioNativeBridge.getInstance().isAAudioAvailable()

    /**
     * Get recommended buffer size for target latency.
     *
     * @param targetLatencyMs Desired latency in milliseconds
     * @return Recommended buffer size in frames, or -1 if not achievable
     */
    fun getRecommendedBufferSize(targetLatencyMs: Float): Int {
        return AudioNativeBridge.getInstance().getRecommendedBufferSize(targetLatencyMs)
    }

    /**
     * Get formatted latency report string.
     */
    fun getLatencyReport(): String = AudioNativeBridge.getInstance().getLatencyReport()

    // ========== Round-Trip Testing ==========

    /**
     * Start a round-trip latency measurement.
     *
     * Requirements:
     * - Audio output must be connected to audio input (loopback)
     * - Both input and output streams must be running
     *
     * @return true if test started successfully
     */
    fun startRoundTripTest(): Boolean = AudioNativeBridge.getInstance().startRoundTripTest()

    /**
     * Get current round-trip test result.
     */
    fun getRoundTripResult(): RoundTripResult {
        val data = AudioNativeBridge.getInstance().getRoundTripResult() ?: return RoundTripResult.IDLE

        if (data.size < 2) return RoundTripResult.IDLE

        return RoundTripResult(
            latencyMs = data[0],
            state = RoundTripState.fromCode(data[1].toInt())
        )
    }

    /**
     * Cancel ongoing round-trip test.
     */
    fun cancelRoundTripTest() = AudioNativeBridge.getInstance().cancelRoundTripTest()

    /**
     * Run round-trip test as a Flow that emits progress updates.
     *
     * Fase 5 migration: this now drives the USB analog-loopback measurer
     * (RoundTripMeasurer) instead of the never-implemented Oboe scaffolding. It
     * requires an active FULL_DUPLEX USB stream with OUT wired to IN. The legacy
     * single-shot [startRoundTripTest]/[getRoundTripResult]/[cancelRoundTripTest]
     * remain as deprecated no-ops.
     *
     * @param timeoutMs Maximum time to wait for result (floored to fit the test)
     * @param pollIntervalMs How often to poll the measurer
     */
    fun runRoundTripTestFlow(
        timeoutMs: Long = 5000,
        pollIntervalMs: Long = 100
    ): Flow<RoundTripResult> = flow {
        val bridge = AudioNativeBridge.getInstance()
        // [burstCount, burstIntervalMs, amplitude, searchWindowMs]
        val config = floatArrayOf(10f, 300f, 0.25f, 250f)
        if (!bridge.usbRoundTripStart(config)) {
            emit(RoundTripResult(-1f, RoundTripState.IDLE))
            return@flow
        }

        // The measurement itself takes ~burstCount×interval + analysis; never
        // time out before it can finish.
        val effectiveTimeout = maxOf(timeoutMs, 10L * 300 + 6000)
        val startTime = System.currentTimeMillis()

        while (true) {
            val data = bridge.usbRoundTripPoll()
            if (data != null && data.size >= 10) {
                val mapped = when (data[0].toInt()) {
                    RoundTripTestState.COMPLETE.code -> RoundTripState.COMPLETE
                    RoundTripTestState.ERROR.code -> RoundTripState.TIMEOUT
                    RoundTripTestState.IDLE.code -> RoundTripState.IDLE
                    else -> RoundTripState.RUNNING
                }
                val latency = if (mapped == RoundTripState.COMPLETE) data[3] else -1f
                emit(RoundTripResult(latency, mapped))
                if (mapped == RoundTripState.COMPLETE || mapped == RoundTripState.TIMEOUT) break
            }

            if (System.currentTimeMillis() - startTime > effectiveTimeout) {
                bridge.usbRoundTripCancel()
                emit(RoundTripResult(-1f, RoundTripState.TIMEOUT))
                break
            }

            delay(pollIntervalMs)
        }
    }

    // ========== Latency Classification ==========

    /**
     * Classify latency quality.
     */
    enum class LatencyQuality {
        EXCELLENT,  // < 10ms - Professional monitoring
        GOOD,       // 10-20ms - Good for most use cases
        ACCEPTABLE, // 20-40ms - Noticeable but usable
        POOR        // > 40ms - Significant delay
    }

    /**
     * Classify the quality of the current latency.
     */
    fun classifyLatency(latencyMs: Float): LatencyQuality = when {
        latencyMs < 10 -> LatencyQuality.EXCELLENT
        latencyMs < 20 -> LatencyQuality.GOOD
        latencyMs < 40 -> LatencyQuality.ACCEPTABLE
        else -> LatencyQuality.POOR
    }

    /**
     * Get human-readable description of latency quality.
     */
    fun getLatencyQualityDescription(quality: LatencyQuality): String = when (quality) {
        LatencyQuality.EXCELLENT ->
            "Excellent (<10ms) - Professional quality, suitable for real-time monitoring"
        LatencyQuality.GOOD ->
            "Good (10-20ms) - Very responsive, suitable for most applications"
        LatencyQuality.ACCEPTABLE ->
            "Acceptable (20-40ms) - Slight delay noticeable, but usable"
        LatencyQuality.POOR ->
            "Poor (>40ms) - Significant delay, may affect playability"
    }

    companion object {
        /** Minimum achievable latency on most Android devices (ms) */
        const val MIN_PRACTICAL_LATENCY_MS = 5f

        /** Latency threshold for "professional" quality (ms) */
        const val PROFESSIONAL_LATENCY_THRESHOLD_MS = 10f

        /** Latency threshold for "acceptable" quality (ms) */
        const val ACCEPTABLE_LATENCY_THRESHOLD_MS = 40f
    }
}
