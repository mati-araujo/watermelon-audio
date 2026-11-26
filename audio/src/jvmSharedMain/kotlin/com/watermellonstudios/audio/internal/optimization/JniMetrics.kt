package com.watermellonstudios.audio.internal.optimization

import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicLong

/**
 * JNI Performance Metrics Collector (Phase 4.4)
 *
 * Tracks call counts and execution time for JNI operations.
 * Use with [measured] inline function for minimal overhead.
 *
 * Usage:
 * ```kotlin
 * // Wrap JNI calls
 * val result = JniMetrics.measured("setParameter") {
 *     nativeSetParameter(index, paramId, value)
 * }
 *
 * // Get statistics
 * val stats = JniMetrics.getStats()
 * stats.forEach { (op, stat) ->
 *     Log.d("JniMetrics", "$op: $stat")
 * }
 *
 * // Reset for new measurement period
 * JniMetrics.reset()
 * ```
 *
 * @see JniOperationStats
 */
object JniMetrics {

    private val callCounts = ConcurrentHashMap<String, AtomicLong>()
    private val totalTimeNs = ConcurrentHashMap<String, AtomicLong>()
    private val maxTimeNs = ConcurrentHashMap<String, AtomicLong>()

    @Volatile
    var enabled: Boolean = false

    /**
     * Record a JNI call with its duration.
     *
     * @param operation Operation name (e.g., "setParameter", "addEffect")
     * @param durationNs Duration in nanoseconds
     */
    fun recordCall(operation: String, durationNs: Long) {
        if (!enabled) return

        callCounts.getOrPut(operation) { AtomicLong(0) }.incrementAndGet()
        totalTimeNs.getOrPut(operation) { AtomicLong(0) }.addAndGet(durationNs)

        // Track max for detecting outliers
        maxTimeNs.getOrPut(operation) { AtomicLong(0) }.updateAndGet { current ->
            maxOf(current, durationNs)
        }
    }

    /**
     * Get statistics for all recorded operations.
     *
     * @return Map of operation name to statistics
     */
    fun getStats(): Map<String, JniOperationStats> {
        return callCounts.keys.associateWith { op ->
            val count = callCounts[op]?.get() ?: 0
            val totalNs = totalTimeNs[op]?.get() ?: 0
            JniOperationStats(
                callCount = count,
                totalTimeMs = totalNs / 1_000_000.0,
                avgTimeUs = if (count > 0) totalNs.toDouble() / count / 1000.0 else 0.0
            )
        }
    }

    /**
     * Get maximum call duration for an operation (useful for detecting spikes).
     *
     * @param operation Operation name
     * @return Maximum duration in microseconds, or 0 if not recorded
     */
    fun getMaxTimeUs(operation: String): Double {
        return (maxTimeNs[operation]?.get() ?: 0) / 1000.0
    }

    /**
     * Reset all statistics.
     * Call this when starting a new measurement period.
     */
    fun reset() {
        callCounts.clear()
        totalTimeNs.clear()
        maxTimeNs.clear()
    }

    /**
     * Get a formatted report of all metrics.
     *
     * @return Human-readable metrics report
     */
    fun getReport(): String = buildString {
        appendLine("=== JNI Metrics Report ===")
        appendLine()

        val stats = getStats()
        if (stats.isEmpty()) {
            appendLine("No metrics recorded")
            return@buildString
        }

        // Sort by total time (descending)
        stats.entries.sortedByDescending { it.value.totalTimeMs }.forEach { (op, stat) ->
            appendLine("$op:")
            appendLine("  calls: ${stat.callCount}")
            appendLine("  total: ${String.format("%.2f", stat.totalTimeMs)}ms")
            appendLine("  avg:   ${String.format("%.1f", stat.avgTimeUs)}μs")
            appendLine("  max:   ${String.format("%.1f", getMaxTimeUs(op))}μs")
            appendLine()
        }
    }

    /**
     * Inline function to measure a JNI operation with minimal overhead.
     *
     * When [enabled] is false, the block executes with no measurement overhead.
     *
     * Usage:
     * ```kotlin
     * val result = JniMetrics.measured("operation") {
     *     nativeOperation()
     * }
     * ```
     */
    inline fun <T> measured(operation: String, block: () -> T): T {
        if (!enabled) {
            return block()
        }

        val start = System.nanoTime()
        return try {
            block()
        } finally {
            recordCall(operation, System.nanoTime() - start)
        }
    }
}

/**
 * Extension function for cleaner syntax.
 *
 * Usage:
 * ```kotlin
 * nativeSetParameter(index, paramId, value).measuredAs("setParameter")
 * ```
 */
fun <T> T.measuredAs(operation: String, durationNs: Long): T {
    JniMetrics.recordCall(operation, durationNs)
    return this
}
