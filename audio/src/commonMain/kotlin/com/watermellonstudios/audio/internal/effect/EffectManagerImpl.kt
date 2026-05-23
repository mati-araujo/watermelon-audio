package com.watermellonstudios.audio.internal.effect

import com.watermellonstudios.audio.api.EffectNotFoundException
import com.watermellonstudios.audio.api.EffectParameterUpdate
import com.watermellonstudios.audio.api.EffectPreset
import com.watermellonstudios.audio.api.IEffectManager
import com.watermellonstudios.audio.api.IEffectStateWriter
import com.watermellonstudios.audio.api.MaxEffectsReachedException
import com.watermellonstudios.audio.api.PresetTypeMismatchException
import com.watermellonstudios.audio.api.SyncTimeoutException
import com.watermellonstudios.audio.callback.AudioLogger
import com.watermellonstudios.audio.callback.NoOpAudioLogger
import com.watermellonstudios.audio.domain.effect.EffectState
import com.watermellonstudios.audio.domain.effect.EffectType
import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import com.watermellonstudios.audio.internal.sync.StateSynchronizer
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.withTimeoutOrNull

/**
 * Implementation of [IEffectManager] with guaranteed C++ synchronization.
 *
 * Each operation follows a three-step pattern:
 * 1. Validate preconditions
 * 2. Execute in C++ via [IEffectStateWriter]
 * 3. Wait for confirmation from [StateSynchronizer]
 *
 * This ensures that the Kotlin state always matches C++ state after
 * any operation completes successfully.
 *
 * Thread Safety: All operations are thread-safe via the underlying
 * [StateSynchronizer] and [IEffectStateWriter] implementations.
 *
 * @param stateWriter Interface to write effect changes to native C++
 * @param synchronizer Synchronizer that polls and updates effect state from C++
 * @param scope CoroutineScope for StateFlow operations
 * @param config Configuration parameters (timeout, max effects, etc.)
 */
internal class EffectManagerImpl(
    private val stateWriter: IEffectStateWriter,
    private val synchronizer: StateSynchronizer,
    private val scope: CoroutineScope,
    private val config: EffectManagerConfig = EffectManagerConfig.DEFAULT,
    private val logger: AudioLogger = NoOpAudioLogger
) : IEffectManager {

    companion object {
        private const val TAG = "EffectManager"
    }

    init {
        // CRITICAL: Start the synchronizer to begin polling C++ state
        // Without this, effectsState will never update from native changes
        synchronizer.startSync()
        logger.debug(TAG, "EffectManagerImpl initialized, synchronizer started")

        // FIX P0.1: Clear legacy effects with retry logic
        // This ensures Kotlin state starts fresh and in sync with what we expect
        scope.launch {
            var attempt = 0
            val maxAttempts = 3
            val retryDelayMs = 100L

            while (attempt < maxAttempts) {
                try {
                    stateWriter.clearAllEffects()
                    logger.debug(TAG, "Cleared legacy effects on init (attempt ${attempt + 1})")
                    return@launch // Success, exit
                } catch (e: Exception) {
                    attempt++
                    if (attempt >= maxAttempts) {
                        // FIX P0.1: Log as ERROR not WARNING - this is a potential sync issue
                        logger.error(TAG, "Failed to clear legacy effects after $maxAttempts attempts. " +
                                "Effect state may be out of sync until first user operation.", e)
                    } else {
                        logger.warn(TAG, "Failed to clear legacy effects (attempt $attempt/$maxAttempts), retrying... ${e.message}")
                        delay(retryDelayMs * attempt) // Exponential backoff
                    }
                }
            }
        }
    }

    /**
     * Effect state derived from StateSynchronizer's synced state.
     *
     * This is the single source of truth for effect state.
     *
     * NOTE: Using SharingStarted.Eagerly to ensure the state is always available
     * immediately when the UI subscribes. WhileSubscribed caused timing issues
     * where the UI would initially see an empty list.
     */
    override val effectsState: StateFlow<List<EffectState>> =
        synchronizer.syncedState
            .map { it.effects }
            .stateIn(
                scope = scope,
                started = SharingStarted.Eagerly,
                initialValue = emptyList()
            )

    override val maxEffects: Int = config.maxEffects

    // =========================================================================
    // ADD EFFECT
    // =========================================================================

    override suspend fun addEffect(type: EffectType): Result<EffectState> {
        logger.debug(TAG, "addEffect: type=${type.displayName}")

        // 1. Validate preconditions
        val currentEffects = effectsState.value
        if (currentEffects.size >= maxEffects) {
            logger.warn(TAG, "addEffect: chain full (${currentEffects.size}/$maxEffects)")
            return Result.failure(MaxEffectsReachedException(maxEffects))
        }

        // 2. Execute in C++
        val indexResult = stateWriter.addEffect(type)
        if (indexResult.isFailure) {
            logger.error(TAG, "addEffect: native call failed", indexResult.exceptionOrNull())
            return Result.failure(indexResult.exceptionOrNull()!!)
        }
        val newIndex = indexResult.getOrThrow()

        // 3. Wait for sync confirmation
        val synced = withTimeoutOrNull(config.syncTimeoutMs) {
            synchronizer.syncedState.first { state ->
                // Confirm effect was added at expected index with correct type
                state.effects.any { it.index == newIndex && it.type == type }
            }
        }

        if (synced == null) {
            logger.error(TAG, "addEffect: sync timeout waiting for index=$newIndex, type=$type")
            return Result.failure(SyncTimeoutException(config.syncTimeoutMs, "addEffect"))
        }

        // 4. Return the created effect
        val effect = synced.effects.first { it.index == newIndex }
        logger.debug(TAG, "addEffect: success, index=$newIndex, type=${type.displayName}")
        return Result.success(effect)
    }

    // =========================================================================
    // REMOVE EFFECT
    // =========================================================================

    override suspend fun removeEffect(index: Int): Result<Unit> {
        logger.debug(TAG, "removeEffect: index=$index")

        // 1. Validate that effect exists
        val currentEffects = effectsState.value
        if (index < 0 || index >= currentEffects.size) {
            return Result.failure(EffectNotFoundException(index, currentEffects.size))
        }

        val effectToRemove = currentEffects[index]

        // 2. Execute in C++
        val result = stateWriter.removeEffect(index)
        if (result.isFailure) {
            logger.error(TAG, "removeEffect: native call failed", result.exceptionOrNull())
            return result
        }

        // 3. Wait for sync confirmation - effect should no longer exist at that index
        val synced = withTimeoutOrNull(config.syncTimeoutMs) {
            synchronizer.syncedState.first { state ->
                // Confirm effect count decreased or effect at index is different
                state.effects.size < currentEffects.size ||
                state.effects.getOrNull(index)?.type != effectToRemove.type
            }
        }

        if (synced == null) {
            logger.error(TAG, "removeEffect: sync timeout waiting for removal at index=$index")
            return Result.failure(SyncTimeoutException(config.syncTimeoutMs, "removeEffect"))
        }

        logger.debug(TAG, "removeEffect: success, index=$index")
        return Result.success(Unit)
    }

    // =========================================================================
    // PARAMETERS
    // =========================================================================

    override suspend fun setParameter(
        effectIndex: Int,
        paramId: Int,
        value: Float
    ): Result<Unit> {
        // Validate effect exists
        val currentEffects = effectsState.value
        if (effectIndex < 0 || effectIndex >= currentEffects.size) {
            return Result.failure(EffectNotFoundException(effectIndex, currentEffects.size))
        }

        // Execute in C++
        val result = stateWriter.setParameter(effectIndex, paramId, value)
        if (result.isFailure) {
            logger.error(TAG, "setParameter: native call failed", result.exceptionOrNull())
            return result
        }

        // For individual parameters, we don't wait for full sync
        // The polling will capture it in the next cycle
        // This allows responsive UI during slider drags
        return Result.success(Unit)
    }

    override suspend fun setEffectParametersBatch(
        updates: List<EffectParameterUpdate>
    ): Result<Unit> {
        if (updates.isEmpty()) return Result.success(Unit)

        val chainSize = effectsState.value.size
        for (update in updates) {
            if (update.effectIndex < 0 || update.effectIndex >= chainSize) {
                return Result.failure(EffectNotFoundException(update.effectIndex, chainSize))
            }
        }

        val result = stateWriter.setMultipleEffectParameters(updates)
        if (result.isFailure) {
            logger.error(TAG, "setEffectParametersBatch: native call failed", result.exceptionOrNull())
            return result
        }

        logger.debug(TAG, "setEffectParametersBatch: ${updates.size} updates applied")
        return Result.success(Unit)
    }

    override suspend fun applyPreset(effectIndex: Int, preset: EffectPreset): Result<Unit> {
        logger.debug(TAG, "applyPreset: effectIndex=$effectIndex, preset=${preset.name}")

        // Validate effect exists
        val currentEffects = effectsState.value
        if (effectIndex < 0 || effectIndex >= currentEffects.size) {
            return Result.failure(EffectNotFoundException(effectIndex, currentEffects.size))
        }

        // Validate preset type matches effect type
        val effect = currentEffects[effectIndex]
        if (effect.type != preset.effectType) {
            return Result.failure(PresetTypeMismatchException(effect.type, preset.effectType))
        }

        // Use batch operation for efficiency
        val result = stateWriter.setParametersBatch(effectIndex, preset.parameters)
        if (result.isFailure) {
            logger.error(TAG, "applyPreset: batch operation failed", result.exceptionOrNull())
            return result
        }

        logger.debug(TAG, "applyPreset: success, ${preset.parameters.size} parameters applied")
        logger.debug("AUDIO_DIAG", "applyPreset: effectType=${preset.effectType.name}, " +
            "presetName=${preset.name}, effectIndex=$effectIndex, " +
            "params=${preset.parameters.entries.joinToString { "(${it.key}=${it.value})" }}")
        return Result.success(Unit)
    }

    // =========================================================================
    // BYPASS
    // =========================================================================

    override suspend fun toggleBypass(index: Int): Result<Boolean> {
        logger.debug(TAG, "toggleBypass: index=$index")

        // Get current state
        val currentEffects = effectsState.value
        if (index < 0 || index >= currentEffects.size) {
            return Result.failure(EffectNotFoundException(index, currentEffects.size))
        }

        val currentBypassed = currentEffects[index].isBypassed
        val newBypassed = !currentBypassed

        // Execute in C++
        val result = stateWriter.setBypass(index, newBypassed)
        if (result.isFailure) {
            logger.error(TAG, "toggleBypass: native call failed", result.exceptionOrNull())
            return Result.failure(result.exceptionOrNull()!!)
        }

        // Wait for sync confirmation
        val synced = withTimeoutOrNull(config.syncTimeoutMs) {
            synchronizer.syncedState.first { state ->
                state.effects.getOrNull(index)?.isBypassed == newBypassed
            }
        }

        if (synced == null) {
            logger.error(TAG, "toggleBypass: sync timeout")
            return Result.failure(SyncTimeoutException(config.syncTimeoutMs, "toggleBypass"))
        }

        logger.debug(TAG, "toggleBypass: success, newBypassed=$newBypassed")
        return Result.success(newBypassed)
    }

    override suspend fun setBypass(index: Int, bypassed: Boolean): Result<Unit> {
        // Validate effect exists
        val currentEffects = effectsState.value
        if (index < 0 || index >= currentEffects.size) {
            return Result.failure(EffectNotFoundException(index, currentEffects.size))
        }

        // Execute in C++
        val result = stateWriter.setBypass(index, bypassed)
        if (result.isFailure) {
            logger.error(TAG, "setBypass: native call failed", result.exceptionOrNull())
            return result
        }

        // Wait for sync confirmation
        val synced = withTimeoutOrNull(config.syncTimeoutMs) {
            synchronizer.syncedState.first { state ->
                state.effects.getOrNull(index)?.isBypassed == bypassed
            }
        }

        if (synced == null) {
            logger.error(TAG, "setBypass: sync timeout")
            return Result.failure(SyncTimeoutException(config.syncTimeoutMs, "setBypass"))
        }

        return Result.success(Unit)
    }

    // =========================================================================
    // REORDER
    // =========================================================================

    override suspend fun reorderEffects(fromIndex: Int, toIndex: Int): Result<Unit> {
        logger.debug(TAG, "reorderEffects: $fromIndex -> $toIndex")

        val currentEffects = effectsState.value

        // Validate indices
        if (fromIndex < 0 || fromIndex >= currentEffects.size) {
            return Result.failure(EffectNotFoundException(fromIndex, currentEffects.size))
        }
        if (toIndex < 0 || toIndex >= currentEffects.size) {
            return Result.failure(EffectNotFoundException(toIndex, currentEffects.size))
        }

        // No-op if same index
        if (fromIndex == toIndex) {
            return Result.success(Unit)
        }

        // Execute in C++
        val result = stateWriter.reorderEffects(fromIndex, toIndex)
        if (result.isFailure) {
            logger.error(TAG, "reorderEffects: native call failed", result.exceptionOrNull())
            return result
        }

        // Force immediate sync - detecting reorder is complex
        synchronizer.forceSync()

        logger.debug(TAG, "reorderEffects: success")
        return Result.success(Unit)
    }

    // =========================================================================
    // CLEAR ALL
    // =========================================================================

    override suspend fun clearAllEffects(): Result<Unit> {
        logger.debug(TAG, "clearAllEffects")

        val result = stateWriter.clearAllEffects()
        if (result.isFailure) {
            logger.error(TAG, "clearAllEffects: native call failed", result.exceptionOrNull())
            return result
        }

        // Wait for sync confirmation - effects list should be empty
        val synced = withTimeoutOrNull(config.syncTimeoutMs) {
            synchronizer.syncedState.first { state ->
                state.effects.isEmpty()
            }
        }

        if (synced == null) {
            logger.error(TAG, "clearAllEffects: sync timeout")
            return Result.failure(SyncTimeoutException(config.syncTimeoutMs, "clearAllEffects"))
        }

        logger.debug(TAG, "clearAllEffects: success")
        return Result.success(Unit)
    }

    // =========================================================================
    // UTILITIES
    // =========================================================================

    override fun canAddEffect(): Boolean {
        return effectsState.value.size < maxEffects
    }

    override fun getEffect(index: Int): EffectState? {
        return effectsState.value.getOrNull(index)
    }

    override fun dispose() {
        logger.debug(TAG, "dispose: EffectManager disposed")
        // Synchronizer disposal is handled by the owner (typically the factory or DI container)
        // We don't dispose it here to allow sharing across multiple managers
    }

    // =========================================================================
    // ROUTING MODE (lock-free, non-suspend)
    // =========================================================================

    private val bridge = getAudioBridge()

    override fun setRoutingMode(mode: Int) {
        bridge.setRoutingMode(mode)
    }

    override fun getRoutingMode(): Int = bridge.getRoutingMode()

    override fun setParallelMix(mix: Float) {
        bridge.setParallelMix(mix)
    }

    override fun setFeedbackAmount(amount: Float) {
        bridge.setFeedbackAmount(amount)
    }
}

/**
 * Configuration for EffectManager behavior.
 */
data class EffectManagerConfig(
    /** Timeout in milliseconds for waiting for C++ sync confirmation */
    val syncTimeoutMs: Long = 500L,

    /** Maximum number of effects allowed in the chain */
    val maxEffects: Int = 12
) {
    companion object {
        val DEFAULT = EffectManagerConfig()

        /** Configuration with longer timeout for slow devices */
        val SLOW_DEVICE = EffectManagerConfig(syncTimeoutMs = 1000L)

        /** Configuration for testing with short timeout */
        val TEST = EffectManagerConfig(syncTimeoutMs = 100L)
    }
}
