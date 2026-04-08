package com.watermellonstudios.audio.internal.sync

/**
 * Configuration for state synchronization between Kotlin and C++.
 *
 * @property pollInterval Interval in milliseconds between state polls (default 50ms = 20 fps)
 * @property reconciliationStrategy Strategy for resolving state divergence
 * @property maxDivergenceBeforeWarning Number of consecutive divergences before logging warning
 */
data class SyncConfig(
    val pollInterval: Long = 50L,
    val reconciliationStrategy: ReconciliationStrategy = ReconciliationStrategy.ACCEPT_NATIVE,
    val maxDivergenceBeforeWarning: Int = 5
) {
    companion object {
        /** Default configuration with 50ms polling (20 fps) */
        val DEFAULT = SyncConfig()

        /** Aggressive polling at 16ms (60 fps) for responsive UI */
        val AGGRESSIVE = SyncConfig(pollInterval = 16L)

        /** Battery-saving configuration with 100ms polling (10 fps) */
        val BATTERY_SAVER = SyncConfig(pollInterval = 100L)
    }

    init {
        require(pollInterval > 0) { "pollInterval must be positive" }
        require(maxDivergenceBeforeWarning > 0) { "maxDivergenceBeforeWarning must be positive" }
    }
}

/**
 * Strategies for reconciling state divergence between Kotlin and C++.
 */
enum class ReconciliationStrategy {
    /**
     * Native (C++) state always wins.
     * Recommended strategy as C++ is the source of truth for audio.
     */
    ACCEPT_NATIVE,

    /**
     * Local (Kotlin) state wins and is pushed to native.
     * NOT IMPLEMENTED - falls back to ACCEPT_NATIVE.
     */
    @Deprecated("Not implemented. Falls back to ACCEPT_NATIVE.", level = DeprecationLevel.WARNING)
    ACCEPT_LOCAL,

    /**
     * Attempt to merge states intelligently.
     * NOT IMPLEMENTED - falls back to ACCEPT_NATIVE.
     */
    @Deprecated("Not implemented. Falls back to ACCEPT_NATIVE.", level = DeprecationLevel.WARNING)
    MERGE
}
