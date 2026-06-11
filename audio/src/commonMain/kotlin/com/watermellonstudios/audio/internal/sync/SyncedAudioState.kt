package com.watermellonstudios.audio.internal.sync

import com.watermellonstudios.audio.domain.effect.EffectState

/**
 * Synchronized audio state - the single source of truth for UI.
 *
 * This represents the last known state from C++ after synchronization.
 * UI should observe this state via StateFlow to get consistent views.
 *
 * @property effects Current list of effects in the chain
 * @property effectsBypassed Global effect-chain bypass state
 * @property lastSyncTimestamp Timestamp of last successful sync (System.nanoTime())
 * @property syncVersion Monotonically increasing version from C++
 * @property isSyncing True while a sync operation is in progress
 * @property lastError Last error encountered during sync, null if no error
 * @property divergenceCount Number of consecutive divergences detected
 */
data class SyncedAudioState(
    val effects: List<EffectState> = emptyList(),
    val effectsBypassed: Boolean = false,
    val lastSyncTimestamp: Long = 0L,
    val syncVersion: Long = 0L,
    val isSyncing: Boolean = false,
    val lastError: Throwable? = null,
    val divergenceCount: Int = 0
) {
    /**
     * True if state has been synchronized at least once.
     */
    val hasBeenSynced: Boolean
        get() = lastSyncTimestamp > 0

    /**
     * True if there's an error state.
     */
    val hasError: Boolean
        get() = lastError != null

    /**
     * Number of effects in the chain.
     */
    val effectCount: Int
        get() = effects.size

    /**
     * True if the effect chain is empty.
     */
    val isEmpty: Boolean
        get() = effects.isEmpty()

    /**
     * True if experiencing repeated divergence (potential sync issue).
     */
    val isUnstable: Boolean
        get() = divergenceCount > 3

    /**
     * Finds an effect by its index.
     */
    fun getEffect(index: Int): EffectState? = effects.getOrNull(index)

    /**
     * Creates a copy with cleared error state.
     */
    fun clearError(): SyncedAudioState = copy(lastError = null)

    /**
     * Creates a copy with incremented divergence count.
     */
    fun incrementDivergence(): SyncedAudioState = copy(divergenceCount = divergenceCount + 1)

    /**
     * Creates a copy with reset divergence count.
     */
    fun resetDivergence(): SyncedAudioState = copy(divergenceCount = 0)
}
