@file:Suppress("unused")

package com.watermellonstudios.audio.api

// Re-export internal sync types for public API access
// This allows consumers to use these types without depending on internal package

/**
 * Configuration for state synchronization between Kotlin and C++.
 *
 * @see com.watermellonstudios.audio.internal.sync.SyncConfig
 */
typealias SyncConfig = com.watermellonstudios.audio.internal.sync.SyncConfig

/**
 * Events emitted during state synchronization.
 *
 * @see com.watermellonstudios.audio.internal.sync.SyncEvent
 */
typealias SyncEvent = com.watermellonstudios.audio.internal.sync.SyncEvent

/**
 * Synchronized audio state - the single source of truth for UI.
 *
 * @see com.watermellonstudios.audio.internal.sync.SyncedAudioState
 */
typealias SyncedAudioState = com.watermellonstudios.audio.internal.sync.SyncedAudioState

/**
 * Represents detected divergence between local (Kotlin) and native (C++) state.
 *
 * @see com.watermellonstudios.audio.internal.sync.StateDivergence
 */
typealias StateDivergence = com.watermellonstudios.audio.internal.sync.StateDivergence

/**
 * Strategies for reconciling state divergence.
 *
 * @see com.watermellonstudios.audio.internal.sync.ReconciliationStrategy
 */
typealias ReconciliationStrategy = com.watermellonstudios.audio.internal.sync.ReconciliationStrategy

/**
 * Bidirectional state synchronizer between Kotlin and C++.
 *
 * @see com.watermellonstudios.audio.internal.sync.StateSynchronizer
 */
typealias StateSynchronizer = com.watermellonstudios.audio.internal.sync.StateSynchronizer
