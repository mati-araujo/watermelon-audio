package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import com.watermellonstudios.audio.internal.sync.StateSynchronizer
import com.watermellonstudios.audio.internal.sync.SyncConfig
import kotlinx.coroutines.CoroutineScope

/**
 * Factory for creating StateSynchronizer instances.
 *
 * This factory provides a clean public API for creating synchronizers
 * without exposing internal implementation details.
 *
 * Usage:
 * ```kotlin
 * val synchronizer = StateSynchronizerFactory.create(
 *     scope = viewModelScope,
 *     config = SyncConfig.DEFAULT
 * )
 *
 * // Start synchronization
 * synchronizer.startSync()
 *
 * // Observe state
 * synchronizer.syncedState.collect { state ->
 *     updateUI(state.effects)
 * }
 * ```
 *
 * @see StateSynchronizer for detailed usage documentation
 */
object StateSynchronizerFactory {

    /**
     * Creates a StateSynchronizer with the specified configuration.
     *
     * @param scope CoroutineScope for sync operations (typically viewModelScope or a dedicated scope)
     * @param config Configuration for polling interval and reconciliation strategy (default: SyncConfig.DEFAULT)
     * @return New instance of StateSynchronizer ready to be started
     */
    // El motor es el implementador del puente, no un consumidor: las factories
    // publicas se construyen encima de el. Ver [InternalWatermelonApi].
    @OptIn(InternalWatermelonApi::class)
    fun create(
        scope: CoroutineScope,
        config: SyncConfig = SyncConfig.DEFAULT
    ): StateSynchronizer {
        val stateProvider = getAudioBridge()
        return StateSynchronizer(
            stateProvider = stateProvider,
            scope = scope,
            config = config
        )
    }

    /**
     * Creates a StateSynchronizer with aggressive polling for responsive UI.
     *
     * Uses 16ms polling interval (60fps) for maximum responsiveness.
     * Higher CPU usage, use only when needed.
     *
     * @param scope CoroutineScope for sync operations
     * @return New instance with aggressive configuration
     */
    fun createAggressive(scope: CoroutineScope): StateSynchronizer {
        return create(scope, SyncConfig.AGGRESSIVE)
    }

    /**
     * Creates a StateSynchronizer with battery-saving configuration.
     *
     * Uses 100ms polling interval (10fps) to reduce CPU usage.
     * Good for background operation or when battery is low.
     *
     * @param scope CoroutineScope for sync operations
     * @return New instance with battery-saving configuration
     */
    fun createBatterySaver(scope: CoroutineScope): StateSynchronizer {
        return create(scope, SyncConfig.BATTERY_SAVER)
    }
}
