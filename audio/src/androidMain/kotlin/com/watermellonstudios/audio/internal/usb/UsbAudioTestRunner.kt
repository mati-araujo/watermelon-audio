package com.watermellonstudios.audio.internal.usb

import android.util.Log
import com.watermellonstudios.audio.api.IUsbAudioManager
import com.watermellonstudios.audio.api.InternalWatermelonApi
import com.watermellonstudios.audio.domain.usb.*
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.*
import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

/**
 * USB Audio Test Runner
 *
 * Orchestrates USB audio tests with progress tracking and result collection.
 *
 * Supports:
 * - Playback tests (tone output)
 * - Capture tests (input level monitoring)
 * - Loopback tests (round-trip latency)
 * - Stress tests (extended streaming)
 * - Full diagnostic suites
 *
 * ## What a row measures (REQ-050, AC-050.8 / AC-050.9)
 *
 * - A playback, stress or rate-sweep row is measured AT THE CONFIGURATION IT DECLARES: sample
 *   rate, bit depth and channels. If the running stream is not provably at that configuration,
 *   the runner reopens it with `selectAltsetting`/`selectClockSource` (D17) and, after the row,
 *   puts the consumer's stream back as it was (D18, [UsbTestResult.streamRestore]).
 * - If the device's descriptors do not offer that configuration, the row is
 *   [UsbTestStatus.NOT_APPLICABLE] and the stream is not touched. If they offer it and the device
 *   runs at another rate anyway, the row is [UsbTestStatus.FAILED] ("offered X, ran at Y", D19).
 * - No row passes without traffic: completed packets have to grow during the row.
 * - The latency limit is the ceiling the backend declares ([UsbTransferStats.declaredMaxLatencyMs]),
 *   or [UsbTestConfig.maxAllowedLatencyMs] if stricter; the success rate does not count the
 *   packets still in flight as lost ([UsbTestResult.successRate]).
 *
 * Running a suite therefore stops and restarts the consumer's USB stream for the rows that need
 * it: an audible interruption is expected.
 */
class UsbAudioTestRunner internal constructor(
    private val usbManager: IUsbAudioManager,
    private val scope: CoroutineScope,
    private val roundTripTester: IRoundTripLatencyTester,
    /** The rate the stream actually runs at, as the engine reports it; null if there is no stream. */
    private val streamSampleRate: () -> Int?,
    private val clock: () -> Long,
) {
    constructor(
        usbManager: IUsbAudioManager,
        scope: CoroutineScope = CoroutineScope(Dispatchers.Default + SupervisorJob()),
        roundTripTester: IRoundTripLatencyTester = RoundTripLatencyTesterImpl(),
    ) : this(usbManager, scope, roundTripTester, ::engineStreamSampleRate, System::currentTimeMillis)

    companion object {
        private const val TAG = "UsbAudioTestRunner"
        private const val STATS_POLL_INTERVAL_MS = 100L
    }

    /**
     * Per-burst progress of the physical loopback round-trip test (Fase 5). The
     * UI can collect this during a LOOPBACK run to show the chirp countdown.
     */
    val roundTripProgress: StateFlow<RoundTripTestProgress>
        get() = roundTripTester.progress

    // Test state
    private var currentTestJob: Job? = null

    // Progress updates
    private val _progress = MutableStateFlow<UsbTestProgress?>(null)
    val progress: StateFlow<UsbTestProgress?> = _progress.asStateFlow()

    // Current result (updated during test)
    private val _currentResult = MutableStateFlow<UsbTestResult?>(null)
    val currentResult: StateFlow<UsbTestResult?> = _currentResult.asStateFlow()

    // Test running state
    private val _isRunning = MutableStateFlow(false)
    val isRunning: StateFlow<Boolean> = _isRunning.asStateFlow()

    /**
     * Run a single test with the given configuration.
     *
     * IMPORTANT: For Playback tests, the test tone must already be playing
     * (started via StartUsbTestTone intent). The test runner only monitors
     * stats and doesn't generate audio itself.
     *
     * @param config Test configuration
     * @return Test result
     */
    suspend fun runTest(config: UsbTestConfig): UsbTestResult {
        if (_isRunning.value) {
            Log.w(TAG, "Test already running, cancelling previous")
            cancelCurrentTest()
        }

        _isRunning.value = true
        val startTime = clock()

        Log.i(TAG, "Starting test: ${config.testType.displayName}")
        Log.d(TAG, "Config: ${config.sampleRate}Hz, ${config.channels}ch, ${config.bitDepth}bit, ${config.durationMs}ms")

        // Initialize result as running
        var result = UsbTestResult(
            testType = config.testType,
            config = config,
            status = UsbTestStatus.RUNNING,
            startTimeMs = startTime,
            endTimeMs = startTime
        )
        _currentResult.value = result

        try {
            // Pre-flight checks based on test type
            when (config.testType) {
                UsbTestType.LOOPBACK -> {
                    // Check full-duplex support BEFORE starting
                    if (!usbManager.supportsFullDuplex()) {
                        result = UsbTestResult.failed(
                            testType = config.testType,
                            config = config,
                            startTimeMs = startTime,
                            errorMessage = "Device does not support full-duplex mode. Loopback test requires a device with both input and output."
                        )
                        _currentResult.value = result
                        _isRunning.value = false
                        return result
                    }
                }
                UsbTestType.CAPTURE_LEVEL -> {
                    // Check capture support
                    if (!usbManager.hasCapture()) {
                        result = UsbTestResult.failed(
                            testType = config.testType,
                            config = config,
                            startTimeMs = startTime,
                            errorMessage = "Device does not have capture capability. Connect a device with audio input."
                        )
                        _currentResult.value = result
                        _isRunning.value = false
                        return result
                    }
                }
                UsbTestType.PLAYBACK_TONE, UsbTestType.STRESS_TEST -> {
                    // Check that streaming is active (test tone should be started first)
                    if (usbManager.connectionState.value != UsbConnectionState.STREAMING) {
                        result = UsbTestResult.failed(
                            testType = config.testType,
                            config = config,
                            startTimeMs = startTime,
                            errorMessage = "Please start the Test Tone first, then run the test."
                        )
                        _currentResult.value = result
                        _isRunning.value = false
                        return result
                    }
                }
                UsbTestType.RATE_NEGOTIATION_SWEEP -> {
                    // The rate sweep takes ownership of the stream lifecycle:
                    // it stops any active stream, starts a new one at the
                    // requested rate, samples stats, and stops it. So the
                    // device must be CONNECTED (or STREAMING) but not in
                    // an unrecoverable state.
                    val state = usbManager.connectionState.value
                    if (state != UsbConnectionState.CONNECTED &&
                        state != UsbConnectionState.STREAMING) {
                        result = UsbTestResult.failed(
                            testType = config.testType,
                            config = config,
                            startTimeMs = startTime,
                            errorMessage = "Connect the USB device first (current state: $state)."
                        )
                        _currentResult.value = result
                        _isRunning.value = false
                        return result
                    }
                }
                else -> { /* No pre-checks needed */ }
            }

            // Run the appropriate test based on type
            result = when (config.testType) {
                UsbTestType.PLAYBACK_TONE -> runPlaybackTest(config, startTime)
                UsbTestType.CAPTURE_LEVEL -> runCaptureTest(config, startTime)
                UsbTestType.LOOPBACK -> runLoopbackTest(config, startTime)
                UsbTestType.STRESS_TEST -> runStressTest(config, startTime)
                UsbTestType.FULL_DIAGNOSTIC -> runDiagnosticTest(config, startTime)
                UsbTestType.RATE_NEGOTIATION_SWEEP -> runRateNegotiationTest(config, startTime)
            }

            _currentResult.value = result

        } catch (e: CancellationException) {
            Log.i(TAG, "Test cancelled")
            result = UsbTestResult.cancelled(config.testType, config, startTime)
            _currentResult.value = result
        } catch (e: Exception) {
            Log.e(TAG, "Test failed with exception: ${e.message}", e)
            result = UsbTestResult.failed(config.testType, config, startTime, e.message ?: "Unknown error")
            _currentResult.value = result
        } finally {
            _isRunning.value = false
            _progress.value = null
        }

        return result
    }

    /**
     * Run a test suite (multiple tests sequentially).
     *
     * @param configs List of test configurations
     * @param deviceName Name of the device being tested
     * @param deviceVidPid VID:PID of the device
     * @param uacVersion UAC version of the device
     * @return Complete test report
     */
    suspend fun runTestSuite(
        configs: List<UsbTestConfig>,
        deviceName: String,
        deviceVidPid: String,
        uacVersion: Int
    ): UsbTestReport {
        val results = mutableListOf<UsbTestResult>()

        for ((index, config) in configs.withIndex()) {
            Log.i(TAG, "Running test ${index + 1}/${configs.size}: ${config.testType.displayName}")

            val result = runTest(config)
            results.add(result)

            // Stop early if test failed critically
            if (result.status == UsbTestStatus.CANCELLED) {
                break
            }

            // Small delay between tests
            delay(500)
        }

        return UsbTestReport(
            deviceName = deviceName,
            deviceVidPid = deviceVidPid,
            uacVersion = uacVersion,
            results = results
        )
    }

    /**
     * Cancel the currently running test.
     */
    fun cancelCurrentTest() {
        currentTestJob?.cancel()
        currentTestJob = null
        // Tear down any in-flight round-trip measurer and restore the backend
        // callback (no-op if none is running).
        roundTripTester.cancel()
        _isRunning.value = false
        _progress.value = null
    }

    // ==================== Private Test Implementations ====================

    /**
     * A playback row at the configuration it declares (AC-050.8, AC-050.9). See the class KDoc.
     */
    private suspend fun runPlaybackTest(config: UsbTestConfig, startTime: Long): UsbTestResult =
        withRowStream(config, startTime) { reopened -> measurePlayback(config, startTime, reopened) }

    /**
     * Opens the stream at [config] if it is not already there, runs [measure], and puts the
     * stream back (D18) — also if the row fails, throws or is cancelled. A row whose stream
     * could not be put back is FAILED: the report cannot say "all passed" with the consumer's
     * stream left stopped or at another configuration.
     */
    private suspend fun withRowStream(
        config: UsbTestConfig,
        startTime: Long,
        /** `reopened`: the row opened the stream itself, so its counters started with the row. */
        measure: suspend (reopened: Boolean) -> UsbTestResult,
    ): UsbTestResult {
        val snapshot = usbManager.getCurrentCapabilitySnapshot()
            ?: return UsbTestResult.failed(
                config.testType, config, startTime,
                "No capability snapshot: cannot tell whether the device offers ${describe(config)}",
            )
        val offered = findOfferedPlaybackFormat(snapshot, config.sampleRate, config.channels, config.bitDepth)
            ?: return UsbTestResult(
                testType = config.testType,
                config = config,
                status = UsbTestStatus.NOT_APPLICABLE,
                startTimeMs = startTime,
                endTimeMs = clock(),
                errorMessage = "The device does not offer ${describe(config)}",
            )

        // Without the library's manager the runner cannot know how the consumer's stream is
        // configured, so it could not put it back: it does not touch it.
        val source = usbManager as? UsbActiveStreamSource
            ?: return UsbTestResult.failed(
                config.testType, config, startTime,
                "The USB manager does not report its stream configuration, so the runner cannot reopen " +
                    "the stream at ${describe(config)} and restore it afterwards",
            )
        val wasStreaming = usbManager.connectionState.value == UsbConnectionState.STREAMING
        val previous = if (wasStreaming) source.activeStreamConfig() else null
        if (wasStreaming && previous == null) {
            return UsbTestResult.failed(
                config.testType, config, startTime,
                "The stream is running but its configuration is unknown: the runner does not reopen it",
            )
        }
        if (previous != null && previous.matches(config, offered)) return measure(false)

        val selectionBefore = source.manualSelection()
        var restore: Pair<UsbStreamRestore, String?> = UsbStreamRestore.NOT_NEEDED to null
        val result = try {
            val failure = reopenAt(config, offered)
            if (failure != null) {
                UsbTestResult.failed(config.testType, config, startTime, failure)
            } else {
                try {
                    measure(true)
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    Log.e(TAG, "Row ${describe(config)} threw: ${e.message}", e)
                    UsbTestResult.failed(config.testType, config, startTime, e.message ?: e.toString())
                }
            }
        } finally {
            restore = withContext(NonCancellable) { restoreStream(previous, selectionBefore) }
        }
        val withRestore = result.copy(streamRestore = restore.first, streamRestoreMessage = restore.second)
        if (restore.first != UsbStreamRestore.FAILED || withRestore.status == UsbTestStatus.FAILED) return withRestore
        return withRestore.copy(
            status = UsbTestStatus.FAILED,
            errorMessage = listOfNotNull(withRestore.errorMessage, "Stream not restored: ${restore.second}")
                .joinToString("; "),
        )
    }

    /** Stops the stream and reopens it at [config]. Returns why it could not, or null. */
    private suspend fun reopenAt(config: UsbTestConfig, offered: OfferedPlaybackFormat): String? {
        if (usbManager.connectionState.value == UsbConnectionState.STREAMING) {
            usbManager.stopStreaming()
        }
        val sel = offered.selection
        val alt = usbManager.selectAltsetting(sel.interfaceNumber, sel.alternateSetting, sel.formatIndex)
        if (alt is UsbResult.Failure) {
            return "Could not select IF${sel.interfaceNumber} Alt${sel.alternateSetting} format ${sel.formatIndex} " +
                "for ${describe(config)}: ${alt.message ?: alt.error.message}"
        }
        val clockId = offered.clockSourceId ?: UsbStreamSelection.AUTOMATIC_CLOCK_SOURCE
        val clockResult = usbManager.selectClockSource(clockId)
        if (clockResult is UsbResult.Failure) {
            return "Could not select clock source $clockId for ${describe(config)}: " +
                (clockResult.message ?: clockResult.error.message)
        }
        Log.i(TAG, "Reopening the stream at ${describe(config)}")
        val start = usbManager.startStreaming(config.sampleRate, config.channels, config.bitDepth, config.streamingMode)
        if (start is UsbResult.Failure) {
            return "Device rejected ${describe(config)}: ${start.message ?: start.error.message}"
        }
        return null
    }

    /**
     * D18: puts the consumer's stream back as it was before the row — the manual selection it had
     * pending ([selectionBefore], automatic where null) and, if it was streaming, its configuration.
     * Every step runs even if an earlier one failed, so a failed altsetting does not leave the row's
     * clock stuck; all the failures are reported.
     */
    private suspend fun restoreStream(
        previous: UsbActiveStreamConfig?,
        selectionBefore: UsbManualSelection,
    ): Pair<UsbStreamRestore, String?> {
        if (usbManager.connectionState.value == UsbConnectionState.STREAMING) {
            usbManager.stopStreaming()
        }
        val problems = mutableListOf<String>()
        val auto = UsbStreamSelection.AUTOMATIC_ALTSETTING
        val alt = selectionBefore.altsetting
        val altResult = if (alt != null) {
            usbManager.selectAltsetting(alt.interfaceNumber, alt.alternateSetting, alt.formatIndex)
        } else {
            usbManager.selectAltsetting(auto, auto, auto)
        }
        if (altResult is UsbResult.Failure) {
            problems += "altsetting selection: ${altResult.message ?: altResult.error.message}"
        }
        val clockResult = usbManager.selectClockSource(
            selectionBefore.clockSourceId ?: UsbStreamSelection.AUTOMATIC_CLOCK_SOURCE,
        )
        if (clockResult is UsbResult.Failure) {
            problems += "clock source selection: ${clockResult.message ?: clockResult.error.message}"
        }
        if (previous != null) {
            val start = usbManager.startStreaming(
                previous.sampleRate, previous.channels, previous.bitDepth, previous.streamingMode,
            )
            if (start is UsbResult.Failure) {
                problems += "restart at ${previous.sampleRate}Hz/${previous.bitDepth}bit/${previous.channels}ch: " +
                    (start.message ?: start.error.message)
            }
        }
        return if (problems.isEmpty()) UsbStreamRestore.RESTORED to null
               else UsbStreamRestore.FAILED to "Could not restore the " + problems.joinToString("; the ")
    }

    /** One stats sample and the runner-clock time it was taken at. */
    private class Sample(val atMs: Long, val stats: UsbTransferStats)

    /** What one measurement window saw. */
    private class Window(
        val samples: List<Sample>,
        val windowEndMs: Long,
        val final: UsbTransferStats?,
        val avgLatency: Double,
        val minLatency: Double,
        val maxLatency: Double,
        val avgBufferFill: Float,
        val minBufferFill: Float,
        val maxBufferFill: Float,
    ) {
        val stats: List<UsbTransferStats> get() = samples.map { it.stats }

        /**
         * AC-050.9: why the window does NOT show a live stream, or null if it does. Completed
         * packets have to grow, the stats have to last until the end of the window (null stats
         * mean the stream left STREAMING), and no stretch of [STALL_LIMIT_MS] — including the tail
         * of the window — may go by without a single completion. A stream that dies half-way
         * leaves "last > first" true and freezes every other counter, so that alone is not enough.
         */
        fun trafficProblem(): String? {
            if (samples.size < 2) return "No traffic during the row: ${samples.size} stats samples"
            if (samples.last().stats.packetsCompleted <= samples.first().stats.packetsCompleted) {
                return "No traffic during the row: completed packets did not grow (${samples.size} samples)"
            }
            if (final == null) return "The stream stopped reporting stats before the end of the row"
            var lastGrowthAt = samples.first().atMs
            var lastCompleted = samples.first().stats.packetsCompleted
            var longest = 0L
            for (s in samples.drop(1)) {
                longest = max(longest, s.atMs - lastGrowthAt)
                if (s.stats.packetsCompleted > lastCompleted) {
                    lastCompleted = s.stats.packetsCompleted
                    lastGrowthAt = s.atMs
                }
            }
            longest = max(longest, windowEndMs - lastGrowthAt)
            if (longest >= STALL_LIMIT_MS) {
                return "The stream stalled during the row: ${longest}ms without a completed packet"
            }
            return null
        }
    }

    /** Samples the stats for [durationMs] of the runner's clock (the measurement itself). */
    private suspend fun sampleWindow(config: UsbTestConfig, startTime: Long, label: String): Window {
        val samples = mutableListOf<Sample>()
        var minLatency = Double.MAX_VALUE
        var maxLatency = 0.0
        var latencySum = 0.0
        var latencyCount = 0
        var minBufferFill = 100f
        var maxBufferFill = 0f
        var bufferFillSum = 0f
        var bufferFillCount = 0

        val windowEnd = clock() + config.durationMs
        while (clock() < windowEnd) {
            val now = clock()
            val elapsed = now - startTime
            val stats = usbManager.getTransferStats()
            if (stats != null) {
                samples.add(Sample(now, stats))
                if (stats.currentLatencyMs > 0) {
                    minLatency = min(minLatency, stats.currentLatencyMs)
                    maxLatency = max(maxLatency, stats.currentLatencyMs)
                    latencySum += stats.currentLatencyMs
                    latencyCount++
                }
                if (stats.ringBufferFillPct > 0) {
                    minBufferFill = min(minBufferFill, stats.ringBufferFillPct * 100f)
                    maxBufferFill = max(maxBufferFill, stats.ringBufferFillPct * 100f)
                    bufferFillSum += stats.ringBufferFillPct * 100f
                    bufferFillCount++
                }
                _progress.value = UsbTestProgress(
                    testType = config.testType,
                    progressPct = (elapsed.toFloat() / config.durationMs.toFloat() * 100f).coerceIn(0f, 100f),
                    elapsedMs = elapsed,
                    remainingMs = windowEnd - now,
                    currentStats = stats,
                    message = "$label: ${String.format("%.2f", stats.currentLatencyMs)}ms latency",
                )
            }
            delay(STATS_POLL_INTERVAL_MS)
        }
        val final = usbManager.getTransferStats()
        if (final != null) samples.add(Sample(clock(), final))
        return Window(
            samples = samples,
            windowEndMs = windowEnd,
            final = final,
            avgLatency = if (latencyCount > 0) latencySum / latencyCount else 0.0,
            minLatency = if (minLatency != Double.MAX_VALUE) minLatency else 0.0,
            maxLatency = maxLatency,
            avgBufferFill = if (bufferFillCount > 0) bufferFillSum / bufferFillCount else 0f,
            minBufferFill = minBufferFill,
            maxBufferFill = maxBufferFill,
        )
    }

    /**
     * The stream has to be running at the row's rate before it is measured (D19): the device can
     * accept a rate it advertised and still coerce it to another one, and `startStreaming` reports
     * success at the coerced rate. Returns the rate, or why the row cannot be measured.
     */
    private fun checkStreamRate(config: UsbTestConfig): Pair<Int, String?> {
        val actual = streamSampleRate() ?: 0
        if (actual <= 0) return 0 to "The engine reports no stream rate for ${describe(config)}"
        if (actual != config.sampleRate) {
            return actual to "The device offered ${config.sampleRate}Hz and the stream ran at ${actual}Hz"
        }
        return actual to null
    }

    /**
     * The packet counters of the row (AC-050.9).
     *
     * - If the row opened the stream ([fromStart]), the backend's counters started with it: they
     *   are taken as they are, and the success rate is completed / (sent − in flight), D10.
     * - If not, the stream carries counters from its whole life, and an underrun from twenty
     *   minutes ago — or a loss diluted in an hour of traffic — would decide the row: they are
     *   taken as deltas over the window. The in-flight queue is still subtracted, which keeps a
     *   healthy stream from ever failing by construction but means the window cannot see up to one
     *   queue (192 packets in SAFE high-speed) of loss; underruns and errors are counted apart.
     */
    private class RowCounters(w: Window, fromStart: Boolean) {
        private val first = if (fromStart) UsbTransferStats() else w.samples.first().stats
        private val last = w.samples.last().stats
        val submitted = last.packetsSubmitted - first.packetsSubmitted
        val completed = last.packetsCompleted - first.packetsCompleted
        val errors = last.packetsErrors - first.packetsErrors
        val underruns = last.underruns - first.underruns
        val overruns = last.overruns - first.overruns
        val inFlight = last.packetsInFlight
    }

    private suspend fun measurePlayback(config: UsbTestConfig, startTime: Long, reopened: Boolean): UsbTestResult {
        val (streamRate, rateProblem) = checkStreamRate(config)
        if (rateProblem != null) {
            return UsbTestResult.failed(config.testType, config, startTime, rateProblem)
                .copy(streamSampleRateHz = streamRate)
        }

        val w = sampleWindow(config, startTime, "${config.sampleRate}Hz")
        val trafficProblem = w.trafficProblem()
        val c = if (w.samples.size >= 2) RowCounters(w, fromStart = reopened) else null
        val declared = (w.final ?: w.samples.lastOrNull()?.stats)?.declaredMaxLatencyMs ?: 0.0
        val limit = min(if (declared > 0.0) declared else Double.POSITIVE_INFINITY, config.maxAllowedLatencyMs)
        val measuredRate = w.samples.lastOrNull()?.stats?.currentSampleRateHz ?: 0f

        val partial = UsbTestResult(
            testType = config.testType,
            config = config,
            status = UsbTestStatus.RUNNING,
            startTimeMs = startTime,
            endTimeMs = clock(),
            avgLatencyMs = w.avgLatency,
            minLatencyMs = w.minLatency,
            maxLatencyMs = w.maxLatency,
            totalPackets = c?.submitted ?: 0,
            successfulPackets = c?.completed ?: 0,
            underruns = c?.underruns ?: 0,
            overruns = c?.overruns ?: 0,
            errors = c?.errors ?: 0,
            avgBufferFillPct = w.avgBufferFill,
            minBufferFillPct = w.minBufferFill,
            maxBufferFillPct = w.maxBufferFill,
            statsSamples = w.stats,
            packetsInFlight = c?.inFlight ?: 0,
            latencyLimitMs = if (limit.isFinite()) limit else 0.0,
            streamSampleRateHz = streamRate,
        )

        val problem = when {
            trafficProblem != null -> trafficProblem
            measuredRate > 0f && abs(measuredRate - config.sampleRate) > config.sampleRate * MEASURED_RATE_TOLERANCE ->
                "The device clock runs at ${String.format("%.1f", measuredRate)}Hz, not ${config.sampleRate}Hz"
            !limit.isFinite() ->
                "The backend declared no latency ceiling and the row sets no limit of its own"
            w.avgLatency > limit ->
                "Average latency ${String.format("%.2f", w.avgLatency)}ms exceeds ${String.format("%.2f", limit)}ms"
            partial.underruns > config.maxAllowedUnderruns -> "Underruns ${partial.underruns} > ${config.maxAllowedUnderruns}"
            partial.overruns > config.maxAllowedOverruns -> "Overruns ${partial.overruns} > ${config.maxAllowedOverruns}"
            partial.successRate < config.minSuccessRatePct ->
                "Success rate ${String.format("%.3f", partial.successRate)}% < ${config.minSuccessRatePct}% " +
                    "(${partial.successfulPackets} completed of ${partial.totalPackets - partial.packetsInFlight} " +
                    "resolved, ${partial.packetsInFlight} in flight)"
            else -> null
        }
        return partial.copy(
            status = if (problem == null) UsbTestStatus.PASSED else UsbTestStatus.FAILED,
            errorMessage = problem,
        )
    }

    private suspend fun runCaptureTest(config: UsbTestConfig, startTime: Long): UsbTestResult {
        // Capture level monitoring needs native support that does not exist yet: the levels below
        // are placeholders. This row is NOT reopened at its configuration (REQ-050 S3 covers the
        // playback rows); what it does assert is that a stream WITH capture is running and that it
        // has traffic (AC-050.9): the completed counter of a playback-only stream says nothing
        // about capture.
        val mode = (usbManager as? UsbActiveStreamSource)?.activeStreamConfig()?.streamingMode
        if (mode != UsbStreamingMode.CAPTURE_ONLY && mode != UsbStreamingMode.FULL_DUPLEX) {
            return UsbTestResult.failed(
                config.testType, config, startTime,
                "The capture row needs a running stream with capture (current mode: ${mode ?: "unknown"})",
            )
        }
        val w = sampleWindow(config, startTime, "Capturing")
        val problem = w.trafficProblem()
        val c = if (w.samples.size >= 2) RowCounters(w, fromStart = false) else null
        return UsbTestResult(
            testType = config.testType,
            config = config,
            status = if (problem == null) UsbTestStatus.PASSED else UsbTestStatus.FAILED,
            startTimeMs = startTime,
            endTimeMs = clock(),
            totalPackets = c?.submitted ?: 0,
            successfulPackets = c?.completed ?: 0,
            underruns = c?.underruns ?: 0,
            overruns = c?.overruns ?: 0,
            avgInputLevelDb = -20f,  // Placeholder - needs native input level monitoring
            peakInputLevelDb = -12f,
            statsSamples = w.stats,
            packetsInFlight = c?.inFlight ?: 0,
            errorMessage = problem,
        )
    }

    private suspend fun runLoopbackTest(config: UsbTestConfig, startTime: Long): UsbTestResult {
        // Full-duplex support is already checked in runTest(). Delegates the real
        // measurement to the native RoundTripMeasurer (Fase 5): chirp bursts over
        // the live stream + cross-correlation. The UI drives progress via the
        // runner's `progress` flow; here we just await the terminal result.
        val tester: IRoundTripLatencyTester = roundTripTester
        val rt = tester.run(
            RoundTripTestConfig(
                burstCount = 10,
                burstIntervalMs = 300,
                amplitude = config.toneAmplitude.coerceIn(0.05f, 0.5f),
            )
        )

        val endTime = System.currentTimeMillis()
        if (!rt.isSuccess) {
            return UsbTestResult(
                testType = UsbTestType.LOOPBACK,
                config = config,
                status = UsbTestStatus.FAILED,
                startTimeMs = startTime,
                endTimeMs = endTime,
                errorMessage = loopbackErrorMessage(rt.error),
                roundTrip = rt,
            )
        }

        return UsbTestResult(
            testType = UsbTestType.LOOPBACK,
            config = config,
            status = UsbTestStatus.PASSED,
            startTimeMs = startTime,
            endTimeMs = endTime,
            avgLatencyMs = rt.medianMs.toDouble(),
            minLatencyMs = rt.medianMs.toDouble() - rt.jitterMs,
            maxLatencyMs = rt.medianMs.toDouble() + rt.jitterMs,
            roundTrip = rt,
        )
    }

    private fun loopbackErrorMessage(error: RoundTripTestError): String = when (error) {
        RoundTripTestError.NO_SIGNAL ->
            "No loopback signal — check the OUT→IN cable and that the DAC volume isn't at zero."
        RoundTripTestError.CLIPPING ->
            "Loopback signal is clipping — lower the DAC output/monitor volume and retry."
        RoundTripTestError.UNRELIABLE ->
            "Measurement unreliable — likely electrical noise or a faulty cable."
        RoundTripTestError.REQUIRES_FULL_DUPLEX ->
            "Round-trip test requires an active full-duplex stream."
        RoundTripTestError.STREAM_LOST ->
            "The USB stream stopped during the test."
        RoundTripTestError.TIMEOUT ->
            "The round-trip test timed out."
        RoundTripTestError.NONE -> "Unknown error."
    }

    private suspend fun runStressTest(config: UsbTestConfig, startTime: Long): UsbTestResult {
        // Stress test is an extended playback test with relaxed thresholds
        return runPlaybackTest(
            config.copy(
                maxAllowedUnderruns = config.maxAllowedUnderruns.coerceAtLeast(10),
                maxAllowedOverruns = config.maxAllowedOverruns.coerceAtLeast(10)
            ),
            startTime
        )
    }

    private suspend fun runDiagnosticTest(config: UsbTestConfig, startTime: Long): UsbTestResult {
        // Diagnostic runs multiple quick playback rows, each at its own rate (AC-050.8).
        val results = mutableListOf<UsbTestResult>()
        for (rate in listOf(44100, 48000)) {
            val testConfig = config.copy(
                testType = UsbTestType.PLAYBACK_TONE,
                sampleRate = rate,
                durationMs = 3000
            )
            results.add(runPlaybackTest(testConfig, clock()))
            delay(500)
        }

        // A row the device does not offer is neither a pass nor a failure, and is not averaged in.
        val applicable = results.filter { it.status != UsbTestStatus.NOT_APPLICABLE }
        val status = when {
            applicable.isEmpty() -> UsbTestStatus.NOT_APPLICABLE
            applicable.all { it.status == UsbTestStatus.PASSED } -> UsbTestStatus.PASSED
            else -> UsbTestStatus.FAILED
        }
        val restoreFailure = results.firstOrNull { it.streamRestore == UsbStreamRestore.FAILED }
        return UsbTestResult(
            testType = UsbTestType.FULL_DIAGNOSTIC,
            config = config,
            status = status,
            startTimeMs = startTime,
            endTimeMs = clock(),
            avgLatencyMs = if (applicable.isEmpty()) 0.0 else applicable.map { it.avgLatencyMs }.average(),
            minLatencyMs = applicable.minOfOrNull { it.minLatencyMs } ?: 0.0,
            maxLatencyMs = applicable.maxOfOrNull { it.maxLatencyMs } ?: 0.0,
            totalPackets = applicable.sumOf { it.totalPackets },
            successfulPackets = applicable.sumOf { it.successfulPackets },
            underruns = applicable.sumOf { it.underruns },
            overruns = applicable.sumOf { it.overruns },
            packetsInFlight = applicable.sumOf { it.packetsInFlight },
            errorMessage = results.mapNotNull { r -> r.errorMessage?.let { "${r.config.sampleRate}Hz: $it" } }
                .joinToString("; ").ifEmpty { null },
            streamRestore = when {
                restoreFailure != null -> UsbStreamRestore.FAILED
                results.any { it.streamRestore == UsbStreamRestore.RESTORED } -> UsbStreamRestore.RESTORED
                else -> UsbStreamRestore.NOT_NEEDED
            },
            streamRestoreMessage = restoreFailure?.streamRestoreMessage,
        )
    }

    /**
     * Stage 1 — verify end-to-end sample rate negotiation.
     *
     * Each `UsbTestPresets.RATE_NEGOTIATION_SWEEP` entry exercises ONE rate (and its bit depth),
     * through the same path as a playback row (AC-050.8): not offered → NOT_APPLICABLE; reopened
     * with the altsetting/clock that offers it; coerced → FAILED; restored afterwards (D18). Pass
     * criteria are deliberately permissive — "did the device accept this rate and produce
     * transfers", with the underrun/overrun ceiling — and do NOT enforce a latency cap.
     */
    private suspend fun runRateNegotiationTest(
        config: UsbTestConfig,
        startTime: Long
    ): UsbTestResult = withRowStream(config, startTime) { reopened ->
        val (streamRate, rateProblem) = checkStreamRate(config)
        if (rateProblem != null) {
            UsbTestResult.failed(config.testType, config, startTime, rateProblem).copy(streamSampleRateHz = streamRate)
        } else {
            val w = sampleWindow(config, startTime, "${config.sampleRate}Hz")
            val c = if (w.samples.size >= 2) RowCounters(w, fromStart = reopened) else null
            val underruns = c?.underruns ?: 0
            val overruns = c?.overruns ?: 0
            val problem = w.trafficProblem() ?: when {
                underruns > config.maxAllowedUnderruns -> "Underrun ceiling exceeded: $underruns > ${config.maxAllowedUnderruns}"
                overruns > config.maxAllowedOverruns -> "Overrun ceiling exceeded: $overruns > ${config.maxAllowedOverruns}"
                else -> null
            }
            Log.i(TAG, "Rate ${config.sampleRate}Hz: passed=${problem == null} problem=$problem")
            UsbTestResult(
                testType = config.testType,
                config = config,
                status = if (problem == null) UsbTestStatus.PASSED else UsbTestStatus.FAILED,
                startTimeMs = startTime,
                endTimeMs = clock(),
                avgLatencyMs = w.avgLatency,
                minLatencyMs = w.minLatency,
                maxLatencyMs = w.maxLatency,
                totalPackets = c?.submitted ?: 0,
                successfulPackets = c?.completed ?: 0,
                underruns = underruns,
                overruns = overruns,
                errors = c?.errors ?: 0,
                statsSamples = w.stats,
                errorMessage = problem,
                packetsInFlight = c?.inFlight ?: 0,
                streamSampleRateHz = streamRate,
            )
        }
    }

    /**
     * Release resources.
     */
    fun release() {
        cancelCurrentTest()
        scope.cancel()
    }
}

/**
 * AC-050.9: this long without a single completed packet is a dead stream, not jitter. Transfers
 * complete every 8 ms in SAFE (1 ms in LOW_LATENCY) and the runner polls every 100 ms.
 */
private const val STALL_LIMIT_MS = 500L

/** Tolerance of the device-measured rate (feedback) against the row's: 1 %, for a USB clock's drift. */
private const val MEASURED_RATE_TOLERANCE = 0.01f

private fun describe(config: UsbTestConfig): String =
    "${config.sampleRate}Hz/${config.bitDepth}bit/${config.channels}ch"

/** The rate the engine's open stream runs at (after any device coercion), or null. */
@OptIn(InternalWatermelonApi::class)
private fun engineStreamSampleRate(): Int? =
    getAudioBridge().getStreamInfoArray()?.getOrNull(0)?.toInt()?.takeIf { it > 0 }

/**
 * Whether the running stream is provably at the row's configuration (D18). The bit depth is only
 * known through a manual altsetting selection — the native start does not apply the requested
 * bits by itself — so a stream opened without one never "matches" and the row reopens it.
 */
private fun UsbActiveStreamConfig.matches(config: UsbTestConfig, offered: OfferedPlaybackFormat): Boolean =
    sampleRate == config.sampleRate &&
        channels == config.channels &&
        streamingMode == config.streamingMode &&
        altsetting == offered.selection &&
        clockSourceId == offered.clockSourceId

/** A playback altsetting+format (and, in UAC2, the clock source) that offers a row's configuration. */
internal data class OfferedPlaybackFormat(
    val selection: UsbAltsettingSelection,
    val clockSourceId: Int?,
)

/**
 * REQ-050 S3 (AC-050.8): the first playback format whose channels and bit depth are exactly the
 * row's and that offers its rate, or null if the descriptors do not offer it.
 *
 * UAC1 lists the rates on the format. UAC2 keeps them on the clock source (the format's list is
 * empty), so the rate is looked up there and that clock is returned to be selected. A UAC1 format
 * without rates says nothing, and is not taken as offering anything.
 */
internal fun findOfferedPlaybackFormat(
    snapshot: UsbCapabilitySnapshot,
    sampleRate: Int,
    channels: Int,
    bitDepth: Int,
): OfferedPlaybackFormat? {
    val uac2 = snapshot.uacVersion == 2
    val offeringClocks = if (uac2) {
        snapshot.clockSources.filter {
            offersRate(it.sampleRates, it.hasContinuousRates, it.minSampleRate, it.maxSampleRate, sampleRate)
        }
    } else emptyList()
    val rateOnAClock = offeringClocks.isNotEmpty()
    // The snapshot does not carry the clock graph (which clock reaches the playback terminal), so a
    // clock is only FORCED when it is the device's only one. With several, the row goes back to the
    // automatic choice and the native picks the terminal's default source; if that one cannot run
    // the rate, the stream comes up at another rate and the row is FAILED by the rate check (D19).
    val clock = offeringClocks.singleOrNull()?.clockId?.takeIf { snapshot.clockSources.size == 1 }
    for (alt in snapshot.playbackAltsettings) {
        alt.formats.forEachIndexed { index, f ->
            if (f.channels != channels || f.bitResolution != bitDepth) return@forEachIndexed
            val formatHasRates = f.sampleRates.isNotEmpty() || f.hasContinuousRates
            val offered = if (formatHasRates) {
                offersRate(f.sampleRates, f.hasContinuousRates, f.minSampleRate, f.maxSampleRate, sampleRate)
            } else {
                rateOnAClock
            }
            if (offered) {
                return OfferedPlaybackFormat(
                    UsbAltsettingSelection(alt.interfaceNumber, alt.alternateSetting, index),
                    clock,
                )
            }
        }
    }
    return null
}

private fun offersRate(rates: List<Int>, continuous: Boolean, minRate: Int, maxRate: Int, rate: Int): Boolean =
    rate in rates || (continuous && rate in minRate..maxRate)
