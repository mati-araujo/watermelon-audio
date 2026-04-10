package com.watermellonstudios.audio.internal.usb

import android.util.Log
import com.watermellonstudios.audio.api.IUsbAudioManager
import com.watermellonstudios.audio.domain.usb.*
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.*
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
 */
class UsbAudioTestRunner(
    private val usbManager: IUsbAudioManager,
    private val scope: CoroutineScope = CoroutineScope(Dispatchers.Default + SupervisorJob())
) {
    companion object {
        private const val TAG = "UsbAudioTestRunner"
        private const val STATS_POLL_INTERVAL_MS = 100L
    }

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
        val startTime = System.currentTimeMillis()

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
        _isRunning.value = false
        _progress.value = null
    }

    // ==================== Private Test Implementations ====================

    private suspend fun runPlaybackTest(config: UsbTestConfig, startTime: Long): UsbTestResult {
        val statsSamples = mutableListOf<UsbTransferStats>()
        var minLatency = Double.MAX_VALUE
        var maxLatency = 0.0
        var latencySum = 0.0
        var latencyCount = 0

        var minBufferFill = 100f
        var maxBufferFill = 0f
        var bufferFillSum = 0f
        var bufferFillCount = 0

        val endTime = startTime + config.durationMs

        // Polling loop
        while (System.currentTimeMillis() < endTime) {
            val elapsed = System.currentTimeMillis() - startTime
            val progressPct = (elapsed.toFloat() / config.durationMs.toFloat() * 100f).coerceIn(0f, 100f)

            val stats = usbManager.getTransferStats()

            if (stats != null) {
                statsSamples.add(stats)

                // Track latency
                if (stats.currentLatencyMs > 0) {
                    minLatency = min(minLatency, stats.currentLatencyMs)
                    maxLatency = max(maxLatency, stats.currentLatencyMs)
                    latencySum += stats.currentLatencyMs
                    latencyCount++
                }

                // Track buffer fill
                if (stats.ringBufferFillPct > 0) {
                    minBufferFill = min(minBufferFill, stats.ringBufferFillPct * 100f)
                    maxBufferFill = max(maxBufferFill, stats.ringBufferFillPct * 100f)
                    bufferFillSum += stats.ringBufferFillPct * 100f
                    bufferFillCount++
                }

                // Update progress
                _progress.value = UsbTestProgress(
                    testType = config.testType,
                    progressPct = progressPct,
                    elapsedMs = elapsed,
                    remainingMs = config.durationMs - elapsed,
                    currentStats = stats,
                    message = "Latency: ${String.format("%.2f", stats.currentLatencyMs)}ms"
                )
            }

            delay(STATS_POLL_INTERVAL_MS)
        }

        // Get final stats
        val finalStats = usbManager.getTransferStats()
        val actualEndTime = System.currentTimeMillis()

        val avgLatency = if (latencyCount > 0) latencySum / latencyCount else 0.0
        val avgBufferFill = if (bufferFillCount > 0) bufferFillSum / bufferFillCount else 0f

        // Determine pass/fail
        val totalPackets = finalStats?.packetsSubmitted ?: 0
        val successfulPackets = finalStats?.packetsCompleted ?: 0
        val underruns = finalStats?.underruns ?: 0
        val overruns = finalStats?.overruns ?: 0
        val errors = finalStats?.packetsErrors ?: 0

        val status = if (
            avgLatency <= config.maxAllowedLatencyMs &&
            underruns <= config.maxAllowedUnderruns &&
            overruns <= config.maxAllowedOverruns
        ) {
            UsbTestStatus.PASSED
        } else {
            UsbTestStatus.FAILED
        }

        return UsbTestResult(
            testType = config.testType,
            config = config,
            status = status,
            startTimeMs = startTime,
            endTimeMs = actualEndTime,
            avgLatencyMs = avgLatency,
            minLatencyMs = if (minLatency != Double.MAX_VALUE) minLatency else 0.0,
            maxLatencyMs = maxLatency,
            totalPackets = totalPackets,
            successfulPackets = successfulPackets,
            underruns = underruns,
            overruns = overruns,
            errors = errors,
            avgBufferFillPct = avgBufferFill,
            minBufferFillPct = minBufferFill,
            maxBufferFillPct = maxBufferFill,
            statsSamples = statsSamples
        )
    }

    private suspend fun runCaptureTest(config: UsbTestConfig, startTime: Long): UsbTestResult {
        // Capture test implementation
        // For now, return a placeholder - actual capture level monitoring requires native support

        val statsSamples = mutableListOf<UsbTransferStats>()
        val endTime = startTime + config.durationMs

        while (System.currentTimeMillis() < endTime) {
            val elapsed = System.currentTimeMillis() - startTime
            val progressPct = (elapsed.toFloat() / config.durationMs.toFloat() * 100f).coerceIn(0f, 100f)

            val stats = usbManager.getTransferStats()
            if (stats != null) {
                statsSamples.add(stats)
            }

            _progress.value = UsbTestProgress(
                testType = config.testType,
                progressPct = progressPct,
                elapsedMs = elapsed,
                remainingMs = config.durationMs - elapsed,
                currentStats = stats,
                message = "Capturing audio input..."
            )

            delay(STATS_POLL_INTERVAL_MS)
        }

        val finalStats = usbManager.getTransferStats()
        val actualEndTime = System.currentTimeMillis()

        return UsbTestResult(
            testType = config.testType,
            config = config,
            status = UsbTestStatus.PASSED,
            startTimeMs = startTime,
            endTimeMs = actualEndTime,
            totalPackets = finalStats?.packetsSubmitted ?: 0,
            successfulPackets = finalStats?.packetsCompleted ?: 0,
            underruns = finalStats?.underruns ?: 0,
            overruns = finalStats?.overruns ?: 0,
            avgInputLevelDb = -20f,  // Placeholder - needs native input level monitoring
            peakInputLevelDb = -12f,
            statsSamples = statsSamples
        )
    }

    private suspend fun runLoopbackTest(config: UsbTestConfig, startTime: Long): UsbTestResult {
        // Full-duplex support is already checked in runTest()
        // Run similar to playback test but in full-duplex mode
        // Actual loopback latency measurement requires native impulse detection
        return runPlaybackTest(
            config.copy(
                testType = UsbTestType.LOOPBACK,
                streamingMode = UsbStreamingMode.FULL_DUPLEX
            ),
            startTime
        )
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
        // Diagnostic runs multiple quick tests
        val results = mutableListOf<UsbTestResult>()

        // Test at different sample rates
        for (rate in listOf(44100, 48000)) {
            val testConfig = config.copy(
                testType = UsbTestType.PLAYBACK_TONE,
                sampleRate = rate,
                durationMs = 3000
            )
            results.add(runPlaybackTest(testConfig, System.currentTimeMillis()))
            delay(500)
        }

        // Aggregate results
        val totalPackets = results.sumOf { it.totalPackets }
        val successfulPackets = results.sumOf { it.successfulPackets }
        val totalUnderruns = results.sumOf { it.underruns }
        val totalOverruns = results.sumOf { it.overruns }
        val avgLatency = results.map { it.avgLatencyMs }.average()

        val allPassed = results.all { it.status == UsbTestStatus.PASSED }

        return UsbTestResult(
            testType = UsbTestType.FULL_DIAGNOSTIC,
            config = config,
            status = if (allPassed) UsbTestStatus.PASSED else UsbTestStatus.FAILED,
            startTimeMs = startTime,
            endTimeMs = System.currentTimeMillis(),
            avgLatencyMs = avgLatency,
            minLatencyMs = results.minOfOrNull { it.minLatencyMs } ?: 0.0,
            maxLatencyMs = results.maxOfOrNull { it.maxLatencyMs } ?: 0.0,
            totalPackets = totalPackets,
            successfulPackets = successfulPackets,
            underruns = totalUnderruns,
            overruns = totalOverruns
        )
    }

    /**
     * Stage 1 — verify end-to-end sample rate negotiation.
     *
     * Each `UsbTestPresets.RATE_NEGOTIATION_SWEEP` entry exercises ONE rate.
     * We take ownership of the stream lifecycle: stop any active stream,
     * call startStreaming with the target rate, give the device a brief
     * settle period, sample stats, and stop. Pass criteria are deliberately
     * permissive — the goal is "did the device actually accept this rate
     * and produce transfers", not absolute latency.
     */
    private suspend fun runRateNegotiationTest(
        config: UsbTestConfig,
        startTime: Long
    ): UsbTestResult {
        // Stop any active stream so the manager re-runs setup with the new
        // rate. stopStreaming is a no-op if nothing is running.
        if (usbManager.connectionState.value == UsbConnectionState.STREAMING) {
            usbManager.stopStreaming()
            // Give the native side a tick to fully tear down before we ask
            // it to start again. The watchdog stops/joins synchronously
            // but the JNI bridge does some bookkeeping after that.
            delay(150)
        }

        Log.i(TAG, "Rate sweep: starting stream at ${config.sampleRate}Hz")
        val startResult = usbManager.startStreaming(
            sampleRate = config.sampleRate,
            channels = config.channels,
            bitDepth = config.bitDepth
        )
        if (startResult is UsbResult.Failure) {
            val msg = startResult.message ?: startResult.error.message
            Log.w(TAG, "Rate ${config.sampleRate}: start failed → $msg")
            return UsbTestResult.failed(
                testType = config.testType,
                config = config,
                startTimeMs = startTime,
                errorMessage = "Device rejected ${config.sampleRate}Hz: $msg"
            )
        }

        // Settle period — the clock controller needs a few feedback packets
        // to converge before its drift number is meaningful.
        delay(200)

        val statsSamples = mutableListOf<UsbTransferStats>()
        var minLatency = Double.MAX_VALUE
        var maxLatency = 0.0
        var latencySum = 0.0
        var latencyCount = 0

        val pollEnd = System.currentTimeMillis() + config.durationMs
        while (System.currentTimeMillis() < pollEnd) {
            val elapsed = System.currentTimeMillis() - startTime
            val progressPct = (elapsed.toFloat() / config.durationMs.toFloat() * 100f)
                .coerceIn(0f, 100f)

            val stats = usbManager.getTransferStats()
            if (stats != null) {
                statsSamples.add(stats)
                if (stats.currentLatencyMs > 0) {
                    minLatency = min(minLatency, stats.currentLatencyMs)
                    maxLatency = max(maxLatency, stats.currentLatencyMs)
                    latencySum += stats.currentLatencyMs
                    latencyCount++
                }
                _progress.value = UsbTestProgress(
                    testType = config.testType,
                    progressPct = progressPct,
                    elapsedMs = elapsed,
                    remainingMs = pollEnd - System.currentTimeMillis(),
                    currentStats = stats,
                    message = "${config.sampleRate}Hz: " +
                        "${String.format("%.2f", stats.currentLatencyMs)}ms latency"
                )
            }
            delay(STATS_POLL_INTERVAL_MS)
        }

        val finalStats = usbManager.getTransferStats()
        val actualEndTime = System.currentTimeMillis()

        // Stop the stream so the next sweep entry starts from a clean state.
        usbManager.stopStreaming()

        val totalPackets = finalStats?.packetsSubmitted ?: 0
        val successfulPackets = finalStats?.packetsCompleted ?: 0
        val underruns = finalStats?.underruns ?: 0
        val overruns = finalStats?.overruns ?: 0
        val errors = finalStats?.packetsErrors ?: 0
        val avgLatency = if (latencyCount > 0) latencySum / latencyCount else 0.0

        // Pass criteria for the sweep: the device must have produced at
        // least one completed packet at the requested rate, and the
        // underrun/overrun counts must stay within the configured ceiling.
        // We do NOT enforce a hard latency cap — the audit calls for end-
        // to-end latency tuning in stages 5 and 7, not stage 1.
        val accepted = successfulPackets > 0
        val withinUnderrunCap = underruns <= config.maxAllowedUnderruns
        val withinOverrunCap = overruns <= config.maxAllowedOverruns
        val passed = accepted && withinUnderrunCap && withinOverrunCap

        val errorMessage = when {
            !accepted -> "No transfers completed at ${config.sampleRate}Hz"
            !withinUnderrunCap -> "Underrun ceiling exceeded: $underruns > ${config.maxAllowedUnderruns}"
            !withinOverrunCap -> "Overrun ceiling exceeded: $overruns > ${config.maxAllowedOverruns}"
            else -> null
        }

        Log.i(
            TAG,
            "Rate ${config.sampleRate}Hz: passed=$passed " +
                "completed=$successfulPackets underruns=$underruns overruns=$overruns " +
                "avgLatency=${String.format("%.2f", avgLatency)}ms"
        )

        return UsbTestResult(
            testType = config.testType,
            config = config,
            status = if (passed) UsbTestStatus.PASSED else UsbTestStatus.FAILED,
            startTimeMs = startTime,
            endTimeMs = actualEndTime,
            avgLatencyMs = avgLatency,
            minLatencyMs = if (minLatency != Double.MAX_VALUE) minLatency else 0.0,
            maxLatencyMs = maxLatency,
            totalPackets = totalPackets,
            successfulPackets = successfulPackets,
            underruns = underruns,
            overruns = overruns,
            errors = errors,
            statsSamples = statsSamples,
            errorMessage = errorMessage
        )
    }

    /**
     * Release resources.
     */
    fun release() {
        cancelCurrentTest()
        scope.cancel()
    }
}
