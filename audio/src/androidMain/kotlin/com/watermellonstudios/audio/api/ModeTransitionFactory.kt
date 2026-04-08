package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.internal.mode.ModeTransitionManagerImpl
import com.watermellonstudios.audio.internal.mode.NativeModeStateWriter
import kotlinx.coroutines.CoroutineScope

/**
 * Factory for creating [IModeTransitionHandler] instances.
 *
 * Example usage:
 * ```kotlin
 * val modeHandler = ModeTransitionFactory.create(
 *     scope = viewModelScope,
 *     effectManager = effectManager  // Optional, for effect reconfiguration
 * )
 * ```
 */
object ModeTransitionFactory {

    /**
     * Default configuration for mode transitions.
     */
    val DEFAULT_CONFIG = ModeTransitionConfig()

    /**
     * Creates a new [IModeTransitionHandler] with default configuration.
     *
     * @param scope CoroutineScope for StateFlow operations
     * @param effectManager Optional effect manager for reconfiguring effects during mode changes
     * @return New IModeTransitionHandler instance
     */
    fun create(
        scope: CoroutineScope,
        effectManager: IEffectManager? = null
    ): IModeTransitionHandler {
        return create(scope, DEFAULT_CONFIG, effectManager)
    }

    /**
     * Creates a new [IModeTransitionHandler] with custom configuration.
     *
     * @param scope CoroutineScope for StateFlow operations
     * @param config Custom configuration
     * @param effectManager Optional effect manager for reconfiguring effects during mode changes
     * @return New IModeTransitionHandler instance
     */
    fun create(
        scope: CoroutineScope,
        config: ModeTransitionConfig,
        effectManager: IEffectManager? = null
    ): IModeTransitionHandler {
        val stateWriter = NativeModeStateWriter()
        return ModeTransitionManagerImpl(
            stateWriter = stateWriter,
            effectManager = effectManager,
            scope = scope,
            config = config
        )
    }
}

/**
 * Configuration for mode transitions.
 *
 * @property modeChangeTimeoutMs Timeout for waiting on native mode change confirmation
 * @property fadeDurationMs Duration of fade in/out during transitions
 * @property pollingIntervalMs Interval for polling native mode state
 */
data class ModeTransitionConfig(
    val modeChangeTimeoutMs: Long = 2000L,
    val fadeDurationMs: Int = 100,
    val pollingIntervalMs: Long = 16L
)
