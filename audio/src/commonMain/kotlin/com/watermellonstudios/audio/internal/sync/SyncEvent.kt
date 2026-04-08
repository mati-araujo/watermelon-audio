package com.watermellonstudios.audio.internal.sync

/**
 * Events emitted during state synchronization.
 *
 * These events can be observed by UI to show sync status indicators
 * or for debugging purposes.
 */
sealed class SyncEvent {

    /**
     * Emitted when divergence is detected between local and native state.
     *
     * @property divergence Details about the detected divergence
     */
    data class DivergenceDetected(
        val divergence: StateDivergence
    ) : SyncEvent()

    /**
     * Emitted when state has been successfully reconciled.
     *
     * @property version The new synchronized version number
     */
    data class Reconciled(
        val version: Long
    ) : SyncEvent()

    /**
     * Emitted when an error occurs during synchronization.
     *
     * @property error The exception that occurred
     */
    data class SyncError(
        val error: Throwable
    ) : SyncEvent()

    /**
     * Emitted when synchronization loop starts.
     */
    data object SyncStarted : SyncEvent()

    /**
     * Emitted when synchronization loop is paused.
     */
    data object SyncPaused : SyncEvent()

    /**
     * Emitted when synchronization loop is resumed.
     */
    data object SyncResumed : SyncEvent()

    /**
     * Emitted when state is updated without divergence.
     *
     * @property version The current synchronized version
     */
    data class StateUpdated(
        val version: Long
    ) : SyncEvent()
}
