package com.watermellonstudios.audio.internal.optimization

/**
 * Phase 4 Optimization Types
 *
 * Data classes and types needed for Phase 4 JNI optimizations.
 * These are defined here to prepare for future batch operations implementation.
 */

/**
 * Represents a single effect parameter update for batch operations (Phase 4.1).
 *
 * Usage:
 * ```kotlin
 * val updates = listOf(
 *     EffectParameterUpdate(effectIndex = 0, paramId = 1, value = 0.5f),
 *     EffectParameterUpdate(effectIndex = 1, paramId = 2, value = 0.8f)
 * )
 * bridge.setMultipleEffectParameters(updates)
 * ```
 *
 * @param effectIndex Index of the effect in the chain (0-based)
 * @param paramId Parameter ID within the effect
 * @param value New value for the parameter
 */
data class EffectParameterUpdate(
    val effectIndex: Int,
    val paramId: Int,
    val value: Float
)

/**
 * Statistics for a single JNI operation type (Phase 4.4).
 *
 * @param callCount Total number of calls
 * @param totalTimeMs Total time spent in milliseconds
 * @param avgTimeUs Average time per call in microseconds
 */
data class JniOperationStats(
    val callCount: Long,
    val totalTimeMs: Double,
    val avgTimeUs: Double
) {
    override fun toString(): String =
        "calls=$callCount, total=${String.format("%.2f", totalTimeMs)}ms, avg=${String.format("%.1f", avgTimeUs)}μs"
}

/**
 * Configuration for XY update coalescing (Phase 4.2).
 *
 * @param enabled Whether coalescing is enabled
 * @param windowMs Time window for coalescing in milliseconds (~16ms = 60fps)
 */
data class XYCoalescerConfig(
    val enabled: Boolean = false,
    val windowMs: Long = 16L
)

/**
 * Configuration for effect chain snapshot caching (Phase 4.3).
 *
 * @param enabled Whether caching is enabled
 * @param maxAge Maximum cache age in milliseconds (0 = use state version only)
 */
data class SnapshotCacheConfig(
    val enabled: Boolean = true,
    val maxAge: Long = 0L
)

/**
 * Master configuration for Phase 4 optimizations.
 *
 * This can be used to enable/disable optimizations per-device or for debugging.
 */
data class Phase4OptimizationConfig(
    val batchOperationsEnabled: Boolean = true,
    val xyCoalescer: XYCoalescerConfig = XYCoalescerConfig(),
    val snapshotCache: SnapshotCacheConfig = SnapshotCacheConfig(),
    val metricsEnabled: Boolean = false  // Only enable in debug builds
) {
    companion object {
        /** Default configuration with conservative settings */
        val Default = Phase4OptimizationConfig()

        /** Configuration for performance testing with all optimizations enabled */
        val PerformanceTest = Phase4OptimizationConfig(
            batchOperationsEnabled = true,
            xyCoalescer = XYCoalescerConfig(enabled = true, windowMs = 16L),
            snapshotCache = SnapshotCacheConfig(enabled = true),
            metricsEnabled = true
        )

        /** Configuration for debugging with metrics enabled */
        val Debug = Phase4OptimizationConfig(
            metricsEnabled = true
        )
    }
}
