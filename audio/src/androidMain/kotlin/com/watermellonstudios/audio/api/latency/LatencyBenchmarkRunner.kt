package com.watermellonstudios.audio.api.latency

import android.util.Log
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Simple utility to run latency benchmarks and log results.
 *
 * Usage:
 * ```kotlin
 * // In your Activity/ViewModel after audio starts:
 * LatencyBenchmarkRunner.runAndLog()
 *
 * // Or observe results:
 * LatencyBenchmarkRunner.results.collect { result ->
 *     // Update UI
 * }
 * ```
 */
object LatencyBenchmarkRunner {
    private const val TAG = "LatencyBenchmark"

    private val analyzer = LatencyAnalyzer()

    private val _results = MutableStateFlow<BenchmarkResult?>(null)
    val results: StateFlow<BenchmarkResult?> = _results.asStateFlow()

    private val _isRunning = MutableStateFlow(false)
    val isRunning: StateFlow<Boolean> = _isRunning.asStateFlow()

    data class BenchmarkResult(
        val latencyInfo: LatencyAnalyzer.LatencyInfo,
        val quality: LatencyAnalyzer.LatencyQuality,
        val qualityDescription: String,
        val optimization: LatencyAnalyzer.OptimizationResult?,
        val report: String,
        val timestamp: Long = System.currentTimeMillis()
    )

    /**
     * Run benchmark and log results to Logcat.
     * Call this after audio engine has started.
     */
    fun runAndLog() {
        if (_isRunning.value) {
            Log.w(TAG, "Benchmark already running")
            return
        }

        _isRunning.value = true

        Log.i(TAG, "")
        Log.i(TAG, "╔════════════════════════════════════════════════════════════╗")
        Log.i(TAG, "║           LATENCY BENCHMARK - Starting...                  ║")
        Log.i(TAG, "╚════════════════════════════════════════════════════════════╝")

        try {
            // 1. Get current latency info
            val info = analyzer.getLatencyInfo()

            Log.i(TAG, "")
            Log.i(TAG, "┌─── Current Configuration ───────────────────────────────────┐")
            Log.i(TAG, "│ API:              ${info.apiName.padEnd(42)}│")
            Log.i(TAG, "│ Sharing Mode:     ${info.sharingModeName.padEnd(42)}│")
            Log.i(TAG, "│ Performance Mode: ${info.performanceModeName.padEnd(42)}│")
            Log.i(TAG, "│ Sample Rate:      ${(info.sampleRate.toString() + " Hz").padEnd(42)}│")
            Log.i(TAG, "│ Frames/Burst:     ${info.framesPerBurst.toString().padEnd(42)}│")
            Log.i(TAG, "│ Buffer Capacity:  ${(info.bufferCapacity.toString() + " frames").padEnd(42)}│")
            Log.i(TAG, "└──────────────────────────────────────────────────────────────┘")

            Log.i(TAG, "")
            Log.i(TAG, "┌─── Latency Measurements ────────────────────────────────────┐")
            Log.i(TAG, "│ Output Latency:   ${formatMs(info.outputLatencyMs).padEnd(42)}│")
            if (info.inputLatencyMs > 0) {
                Log.i(TAG, "│ Input Latency:    ${formatMs(info.inputLatencyMs).padEnd(42)}│")
                Log.i(TAG, "│ Total (est):      ${formatMs(info.totalLatencyMs).padEnd(42)}│")
            }
            Log.i(TAG, "│ Burst Latency:    ${formatMs(info.burstLatencyMs).padEnd(42)}│")
            Log.i(TAG, "│ Buffer Latency:   ${formatMs(info.bufferLatencyMs).padEnd(42)}│")
            Log.i(TAG, "└──────────────────────────────────────────────────────────────┘")

            // 2. Classify quality
            val quality = analyzer.classifyLatency(info.outputLatencyMs)
            val qualityDesc = analyzer.getLatencyQualityDescription(quality)

            val qualityEmoji = when (quality) {
                LatencyAnalyzer.LatencyQuality.EXCELLENT -> "🟢"
                LatencyAnalyzer.LatencyQuality.GOOD -> "🟡"
                LatencyAnalyzer.LatencyQuality.ACCEPTABLE -> "🟠"
                LatencyAnalyzer.LatencyQuality.POOR -> "🔴"
            }

            Log.i(TAG, "")
            Log.i(TAG, "┌─── Quality Assessment ──────────────────────────────────────┐")
            Log.i(TAG, "│ Rating: $qualityEmoji ${quality.name.padEnd(52)}│")
            Log.i(TAG, "│ ${qualityDesc.take(60).padEnd(60)}│")
            Log.i(TAG, "└──────────────────────────────────────────────────────────────┘")

            // 3. Run optimization test (optional, takes a moment)
            Log.i(TAG, "")
            Log.i(TAG, "Running optimization test...")

            val optimization = try {
                analyzer.runOptimizationTest()
            } catch (e: Exception) {
                Log.w(TAG, "Optimization test failed: ${e.message}")
                null
            }

            if (optimization != null && optimization.currentLatencyMs > 0) {
                Log.i(TAG, "")
                Log.i(TAG, "┌─── Optimization Analysis ───────────────────────────────────┐")
                Log.i(TAG, "│ Current Latency:  ${formatMs(optimization.currentLatencyMs).padEnd(42)}│")
                Log.i(TAG, "│ Best Possible:    ${formatMs(optimization.bestLatencyMs).padEnd(42)}│")
                Log.i(TAG, "│ Exclusive Mode:   ${(if (optimization.canUseExclusive) "Available ✓" else "Not available ✗").padEnd(42)}│")
                if (optimization.hasImprovement) {
                    Log.i(TAG, "│ Improvement:      ${("%.1f%% (%.1f ms)".format(optimization.improvementPercent, optimization.improvementMs)).padEnd(42)}│")
                }
                Log.i(TAG, "├──────────────────────────────────────────────────────────────┤")
                Log.i(TAG, "│ ${optimization.recommendation.take(60).padEnd(60)}│")
                Log.i(TAG, "└──────────────────────────────────────────────────────────────┘")
            }

            // 4. Summary
            Log.i(TAG, "")
            Log.i(TAG, "╔════════════════════════════════════════════════════════════╗")
            Log.i(TAG, "║  SUMMARY: ${formatMs(info.outputLatencyMs)} output latency - $quality".padEnd(59) + "║")
            Log.i(TAG, "╚════════════════════════════════════════════════════════════╝")
            Log.i(TAG, "")

            // Store result
            _results.value = BenchmarkResult(
                latencyInfo = info,
                quality = quality,
                qualityDescription = qualityDesc,
                optimization = optimization,
                report = analyzer.getLatencyReport()
            )

        } catch (e: Exception) {
            Log.e(TAG, "Benchmark failed: ${e.message}", e)
        } finally {
            _isRunning.value = false
        }
    }

    /**
     * Run benchmark in a coroutine scope.
     */
    fun runAsync(scope: CoroutineScope = CoroutineScope(Dispatchers.IO)) {
        scope.launch {
            // Small delay to ensure audio is fully started
            delay(500)
            withContext(Dispatchers.Main) {
                runAndLog()
            }
        }
    }

    /**
     * Quick check - just log basic info without optimization test.
     */
    fun quickCheck() {
        val info = analyzer.getLatencyInfo()
        val quality = analyzer.classifyLatency(info.outputLatencyMs)

        Log.i(TAG, "Quick Check: ${formatMs(info.outputLatencyMs)} | ${info.apiName} | ${info.sharingModeName} | $quality")
    }

    /**
     * Get the raw latency report string.
     */
    fun getReport(): String = analyzer.getLatencyReport()

    private fun formatMs(ms: Float): String {
        return if (ms < 0) "N/A" else "%.1f ms".format(ms)
    }

    /**
     * Clear stored results.
     */
    fun clearResults() {
        _results.value = null
    }
}
