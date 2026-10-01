package com.watermellonstudios.audio.domain.usb

import com.watermellonstudios.audio.internal.util.epochMillis
import com.watermellonstudios.audio.internal.util.fmt
import com.watermellonstudios.audio.internal.util.formatEpochMillisUtc

/**
 * USB Audio Test Suite - Result Types
 *
 * Data classes for representing test configurations, results, and reports.
 */

/**
 * Types of USB audio tests available.
 */
enum class UsbTestType(val id: Int, val displayName: String, val description: String) {
    PLAYBACK_TONE(
        id = 0,
        displayName = "Playback Test",
        description = "Output 440Hz tone to USB, measure latency and underruns"
    ),
    CAPTURE_LEVEL(
        id = 1,
        displayName = "Capture Test",
        description = "Record from USB input, measure level and overruns"
    ),
    LOOPBACK(
        id = 2,
        displayName = "Loopback Test",
        description = "USB IN -> pass-through -> USB OUT, measure round-trip latency"
    ),
    STRESS_TEST(
        id = 3,
        displayName = "Stress Test",
        description = "Extended streaming test, report stability metrics"
    ),
    FULL_DIAGNOSTIC(
        id = 4,
        displayName = "Full Diagnostic",
        description = "Run all tests and generate comprehensive report"
    ),
    RATE_NEGOTIATION_SWEEP(
        id = 5,
        displayName = "Rate Negotiation Sweep",
        description = "Iterate common sample rates and verify SET_CUR is honored end-to-end"
    );

    companion object {
        fun fromId(id: Int): UsbTestType = entries.find { it.id == id } ?: PLAYBACK_TONE
    }
}

/**
 * Status of a test run.
 */
enum class UsbTestStatus {
    NOT_STARTED,
    RUNNING,
    PASSED,
    FAILED,
    CANCELLED,
    SKIPPED,

    /**
     * REQ-050 (AC-050.8, D5, D19): the device's descriptors do not offer what the row declares
     * (its sample rate, bit depth or channel count), so the row was NOT measured and the stream
     * was not touched. It is never a pass: [UsbTestReport.allPassed] is false with one of these.
     *
     * A rate the descriptors DO offer but the device then coerces to another one is
     * [FAILED], not this ("offered X, ran at Y").
     */
    NOT_APPLICABLE
}

/**
 * REQ-050 (AC-050.8, D18) — what happened to the consumer's stream after a row that had to
 * reopen it at its own configuration.
 */
enum class UsbStreamRestore {
    /** The stream already ran at the row's configuration: it was measured as it was. */
    NOT_NEEDED,

    /** The row reopened the stream and it is back at the configuration it had before. */
    RESTORED,

    /**
     * The row reopened the stream and could not bring it back (see
     * [UsbTestResult.streamRestoreMessage]). The stream may be stopped, or at the row's
     * configuration.
     */
    FAILED
}

/**
 * Configuration for a USB audio test.
 */
data class UsbTestConfig(
    // Audio parameters
    val sampleRate: Int = 48000,
    val channels: Int = 2,
    val bitDepth: Int = 16,
    val bufferSizeFrames: Int = 256,

    // Test parameters
    val testType: UsbTestType = UsbTestType.PLAYBACK_TONE,
    val durationMs: Long = 5000,
    val streamingMode: UsbStreamingMode = UsbStreamingMode.PLAYBACK_ONLY,

    // Tone generator settings (for playback tests)
    val toneFrequencyHz: Float = 440f,
    val toneAmplitude: Float = 0.5f,

    // Pass/fail thresholds
    /**
     * An EXTRA latency limit for the row, on top of the one the backend declares
     * ([UsbTransferStats.declaredMaxLatencyMs]). The default, +∞, means "the backend's ceiling
     * decides" (REQ-050, AC-050.9): the old fixed 20 ms failed by construction against the
     * libusb backend, which in its SAFE profile sits at ~37 ms with perfect traffic.
     */
    val maxAllowedLatencyMs: Double = Double.POSITIVE_INFINITY,
    val maxAllowedUnderruns: Long = 0,
    val maxAllowedOverruns: Long = 0,
    val minSuccessRatePct: Float = 99.9f
) {
    companion object {
        /** Default configuration for quick tests */
        val DEFAULT = UsbTestConfig()

        /**
         * Low latency configuration. Its latency limit is the backend's declared ceiling, like
         * every other preset (REQ-050, AC-050.9): a fixed 10 ms failed by construction against
         * the SAFE profile. To test the LOW_LATENCY profile, select it with
         * `IUsbAudioManager.setLatencyProfile` before streaming; the ceiling follows it.
         */
        val LOW_LATENCY = UsbTestConfig(
            sampleRate = 48000,
            channels = 2,
            bitDepth = 16,
            bufferSizeFrames = 128,
        )

        /** High quality configuration. Latency limit: the backend's declared ceiling (it was a fixed 30 ms). */
        val HIGH_QUALITY = UsbTestConfig(
            sampleRate = 96000,
            channels = 2,
            bitDepth = 24,
            bufferSizeFrames = 512,
        )

        /** Stress test configuration */
        val STRESS = UsbTestConfig(
            testType = UsbTestType.STRESS_TEST,
            durationMs = 60000,  // 1 minute
            maxAllowedUnderruns = 5,
            maxAllowedOverruns = 5
        )
    }
}

/**
 * Result of a single USB audio test.
 */
data class UsbTestResult(
    // Test identification
    val testType: UsbTestType,
    val config: UsbTestConfig,
    val status: UsbTestStatus,

    // Timing
    val startTimeMs: Long,
    val endTimeMs: Long,
    val actualDurationMs: Long = endTimeMs - startTimeMs,

    // Performance metrics
    val avgLatencyMs: Double = 0.0,
    val minLatencyMs: Double = 0.0,
    val maxLatencyMs: Double = 0.0,
    val latencyJitterMs: Double = maxLatencyMs - minLatencyMs,

    // Buffer health
    val totalPackets: Long = 0,
    val successfulPackets: Long = 0,
    val underruns: Long = 0,
    val overruns: Long = 0,
    val errors: Long = 0,

    // Buffer utilization
    val avgBufferFillPct: Float = 0f,
    val minBufferFillPct: Float = 0f,
    val maxBufferFillPct: Float = 0f,

    // Capture-specific (for capture tests)
    val avgInputLevelDb: Float = Float.NEGATIVE_INFINITY,
    val peakInputLevelDb: Float = Float.NEGATIVE_INFINITY,

    // Error message if failed
    val errorMessage: String? = null,

    // Collected stats samples for detailed analysis
    val statsSamples: List<UsbTransferStats> = emptyList(),

    // Physical loopback round-trip result (LOOPBACK test only; Fase 5).
    val roundTrip: RoundTripTestResult? = null,

    /**
     * REQ-050 (AC-050.9, D16): output packets the backend keeps in flight. They were submitted
     * and have no fate yet, so [successRate] does not count them as lost.
     */
    val packetsInFlight: Long = 0,

    /**
     * REQ-050 (AC-050.9): the latency limit the row was judged against, in ms — the ceiling the
     * backend declared, or [UsbTestConfig.maxAllowedLatencyMs] if that is stricter. 0 if no
     * latency was judged.
     */
    val latencyLimitMs: Double = 0.0,

    /**
     * REQ-050 (AC-050.8): the sample rate the stream actually ran at during the row, as the
     * engine reports it (after any coercion by the device). 0 if unknown.
     */
    val streamSampleRateHz: Int = 0,

    /** REQ-050 (AC-050.8, D18): what happened to the consumer's stream after the row. */
    val streamRestore: UsbStreamRestore = UsbStreamRestore.NOT_NEEDED,

    /** Why [streamRestore] is [UsbStreamRestore.FAILED]; null otherwise. */
    val streamRestoreMessage: String? = null,
) {
    /**
     * Check if the test passed based on thresholds.
     */
    val passed: Boolean
        get() = status == UsbTestStatus.PASSED ||
                (status == UsbTestStatus.RUNNING && meetsThresholds(config))

    /**
     * Success rate in percent: completed packets over the packets whose fate is known — sent
     * minus [packetsInFlight] (REQ-050, AC-050.9, D10). Counting the in-flight ones as lost made
     * a perfect stream fail the 99.9 % criterion by construction (MINI-039: 192 of 57 152).
     *
     * Without resolved packets it is 0, not 100: no traffic is not a success.
     *
     * The counters are the backend's: `packetsSubmitted` counts OUTPUT packets only, and in
     * full duplex `packetsCompleted` also counts input completions, so there this can exceed
     * 100 %. It is meaningful for playback rows.
     */
    val successRate: Float
        get() {
            val resolved = totalPackets - packetsInFlight
            return if (resolved > 0) successfulPackets.toFloat() / resolved.toFloat() * 100f else 0f
        }

    /**
     * Check if there are any errors in the result.
     */
    val hasErrors: Boolean
        get() = underruns > 0 || overruns > 0 || errors > 0

    /**
     * Check if results meet the configured thresholds.
     */
    fun meetsThresholds(config: UsbTestConfig): Boolean {
        val limit = if (latencyLimitMs > 0.0) minOf(latencyLimitMs, config.maxAllowedLatencyMs)
                    else config.maxAllowedLatencyMs
        return avgLatencyMs <= limit &&
                underruns <= config.maxAllowedUnderruns &&
                overruns <= config.maxAllowedOverruns &&
                successRate >= config.minSuccessRatePct
    }

    /**
     * Generate a human-readable summary.
     */
    fun generateSummary(): String {
        return buildString {
            appendLine("=== ${testType.displayName} ===")
            appendLine("Status: $status")
            appendLine("Duration: ${actualDurationMs}ms")
            appendLine()
            appendLine("Latency:")
            appendLine("  Average: ${avgLatencyMs.fmt(2)}ms")
            appendLine("  Min/Max: ${minLatencyMs.fmt(2)}/${maxLatencyMs.fmt(2)}ms")
            appendLine("  Jitter:  ${latencyJitterMs.fmt(2)}ms")
            appendLine()
            appendLine("Packets:")
            appendLine("  Total:      $totalPackets")
            appendLine("  Successful: $successfulPackets (${successRate.fmt(2)}%)")
            appendLine("  Underruns:  $underruns")
            appendLine("  Overruns:   $overruns")
            appendLine("  Errors:     $errors")
            if (packetsInFlight > 0) appendLine("  In flight:  $packetsInFlight")
            if (streamSampleRateHz > 0) appendLine("Stream rate: ${streamSampleRateHz}Hz")
            if (streamRestore != UsbStreamRestore.NOT_NEEDED) {
                appendLine("Stream restore: $streamRestore" + (streamRestoreMessage?.let { " ($it)" } ?: ""))
            }

            if (avgInputLevelDb != Float.NEGATIVE_INFINITY) {
                appendLine()
                appendLine("Input Level:")
                appendLine("  Average: ${avgInputLevelDb.fmt(1)} dB")
                appendLine("  Peak:    ${peakInputLevelDb.fmt(1)} dB")
            }

            errorMessage?.let {
                appendLine()
                appendLine("Error: $it")
            }
        }
    }

    companion object {
        /**
         * Create a result for a test that hasn't started.
         */
        fun notStarted(testType: UsbTestType, config: UsbTestConfig) = UsbTestResult(
            testType = testType,
            config = config,
            status = UsbTestStatus.NOT_STARTED,
            startTimeMs = 0,
            endTimeMs = 0
        )

        /**
         * Create a result for a cancelled test.
         */
        fun cancelled(testType: UsbTestType, config: UsbTestConfig, startTimeMs: Long) = UsbTestResult(
            testType = testType,
            config = config,
            status = UsbTestStatus.CANCELLED,
            startTimeMs = startTimeMs,
            endTimeMs = epochMillis()
        )

        /**
         * Create a result for a failed test.
         */
        fun failed(
            testType: UsbTestType,
            config: UsbTestConfig,
            startTimeMs: Long,
            errorMessage: String
        ) = UsbTestResult(
            testType = testType,
            config = config,
            status = UsbTestStatus.FAILED,
            startTimeMs = startTimeMs,
            endTimeMs = epochMillis(),
            errorMessage = errorMessage
        )
    }
}

/**
 * Complete test report containing multiple test results.
 */
data class UsbTestReport(
    // Device info
    val deviceName: String,
    val deviceVidPid: String,
    val uacVersion: Int,

    // Test results
    val results: List<UsbTestResult>,

    // Report metadata
    val generatedAt: Long = epochMillis(),
    val totalDurationMs: Long = results.sumOf { it.actualDurationMs }
) {
    /**
     * Check if all tests passed.
     */
    val allPassed: Boolean
        get() = results.all { it.status == UsbTestStatus.PASSED }

    /**
     * Count of passed tests.
     */
    val passedCount: Int
        get() = results.count { it.status == UsbTestStatus.PASSED }

    /**
     * Count of failed tests.
     */
    val failedCount: Int
        get() = results.count { it.status == UsbTestStatus.FAILED }

    /** Rows the device does not offer (REQ-050, AC-050.8): neither passed nor failed. */
    val notApplicableCount: Int
        get() = results.count { it.status == UsbTestStatus.NOT_APPLICABLE }

    /**
     * Overall status summary.
     */
    val statusSummary: String
        get() = "$passedCount/${results.size} tests passed" +
            if (notApplicableCount > 0) " ($notApplicableCount not applicable)" else ""

    /**
     * Generate a full report text.
     */
    fun generateFullReport(): String {
        return buildString {
            appendLine("╔══════════════════════════════════════════════════════════════╗")
            appendLine("║           USB AUDIO TEST REPORT                               ║")
            appendLine("╚══════════════════════════════════════════════════════════════╝")
            appendLine()
            appendLine("Device: $deviceName")
            appendLine("VID:PID: $deviceVidPid")
            appendLine("UAC Version: $uacVersion")
            appendLine("Generated: ${formatEpochMillisUtc(generatedAt)}")
            appendLine()
            appendLine("═══════════════════════════════════════════════════════════════")
            appendLine("SUMMARY: $statusSummary")
            appendLine("Total Duration: ${totalDurationMs}ms")
            appendLine("═══════════════════════════════════════════════════════════════")

            results.forEach { result ->
                appendLine()
                append(result.generateSummary())
            }

            appendLine()
            appendLine("═══════════════════════════════════════════════════════════════")
            appendLine("END OF REPORT")
        }
    }
}

/**
 * Progress update during a running test.
 */
data class UsbTestProgress(
    val testType: UsbTestType,
    val progressPct: Float,
    val elapsedMs: Long,
    val remainingMs: Long,
    val currentStats: UsbTransferStats?,
    val message: String = ""
) {
    val isComplete: Boolean get() = progressPct >= 100f
}

/**
 * Configuration presets for common test scenarios.
 */
object UsbTestPresets {
    /** Quick verification test */
    val QUICK_TEST = listOf(
        UsbTestConfig(
            testType = UsbTestType.PLAYBACK_TONE,
            durationMs = 3000
        )
    )

    /** Standard test suite */
    val STANDARD_SUITE = listOf(
        UsbTestConfig(
            testType = UsbTestType.PLAYBACK_TONE,
            durationMs = 5000
        ),
        UsbTestConfig(
            testType = UsbTestType.PLAYBACK_TONE,
            sampleRate = 44100,
            durationMs = 5000
        ),
        UsbTestConfig(
            testType = UsbTestType.PLAYBACK_TONE,
            sampleRate = 96000,
            bitDepth = 24,
            durationMs = 5000
        )
    )

    /** Full diagnostic suite */
    val FULL_DIAGNOSTIC = listOf(
        // Playback tests at different rates
        UsbTestConfig(testType = UsbTestType.PLAYBACK_TONE, sampleRate = 44100, durationMs = 5000),
        UsbTestConfig(testType = UsbTestType.PLAYBACK_TONE, sampleRate = 48000, durationMs = 5000),
        UsbTestConfig(testType = UsbTestType.PLAYBACK_TONE, sampleRate = 96000, durationMs = 5000),
        // Buffer size tests
        UsbTestConfig(testType = UsbTestType.PLAYBACK_TONE, bufferSizeFrames = 64, durationMs = 5000),
        UsbTestConfig(testType = UsbTestType.PLAYBACK_TONE, bufferSizeFrames = 128, durationMs = 5000),
        UsbTestConfig(testType = UsbTestType.PLAYBACK_TONE, bufferSizeFrames = 256, durationMs = 5000),
        UsbTestConfig(testType = UsbTestType.PLAYBACK_TONE, bufferSizeFrames = 512, durationMs = 5000),
        // Stress test
        UsbTestConfig(testType = UsbTestType.STRESS_TEST, durationMs = 30000)
    )

    /** Capture test suite (requires device with input) */
    val CAPTURE_SUITE = listOf(
        UsbTestConfig(
            testType = UsbTestType.CAPTURE_LEVEL,
            streamingMode = UsbStreamingMode.CAPTURE_ONLY,
            durationMs = 5000
        )
    )

    /** Loopback test suite (requires full-duplex device) */
    val LOOPBACK_SUITE = listOf(
        UsbTestConfig(
            testType = UsbTestType.LOOPBACK,
            streamingMode = UsbStreamingMode.FULL_DUPLEX,
            durationMs = 10000
        )
    )

    /**
     * Stage 1 — sample-rate sweep. Verifies that the host's class-specific
     * SET_CUR negotiation is actually honored by the device end-to-end.
     * The runner re-creates the stream at each rate for ~2s, validates that
     * transfers are completing and that the underrun count stays bounded.
     *
     * Each entry is the same configuration except for sampleRate. The runner
     * is responsible for collapsing the suite into a single
     * RATE_NEGOTIATION_SWEEP test result with per-rate stats embedded in
     * `statsSamples`.
     */
    val RATE_NEGOTIATION_SWEEP = listOf(
        UsbTestConfig(
            testType = UsbTestType.RATE_NEGOTIATION_SWEEP,
            sampleRate = 44100,
            bitDepth = 24,
            durationMs = 2000,
            maxAllowedLatencyMs = 30.0,
            maxAllowedUnderruns = 5
        ),
        UsbTestConfig(
            testType = UsbTestType.RATE_NEGOTIATION_SWEEP,
            sampleRate = 48000,
            bitDepth = 24,
            durationMs = 2000,
            maxAllowedLatencyMs = 30.0,
            maxAllowedUnderruns = 5
        ),
        UsbTestConfig(
            testType = UsbTestType.RATE_NEGOTIATION_SWEEP,
            sampleRate = 88200,
            bitDepth = 24,
            durationMs = 2000,
            maxAllowedLatencyMs = 30.0,
            maxAllowedUnderruns = 5
        ),
        UsbTestConfig(
            testType = UsbTestType.RATE_NEGOTIATION_SWEEP,
            sampleRate = 96000,
            bitDepth = 24,
            durationMs = 2000,
            maxAllowedLatencyMs = 30.0,
            maxAllowedUnderruns = 5
        )
    )
}
