package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.effect.EffectState
import com.watermellonstudios.audio.domain.effect.EffectType
import kotlinx.coroutines.flow.StateFlow

/**
 * Public interface for managing audio effects with guaranteed synchronization.
 *
 * Each operation follows a three-step pattern:
 * 1. Validate preconditions (effect count, index bounds, etc.)
 * 2. Execute in C++ via native bridge
 * 3. Wait for confirmation from StateSynchronizer
 *
 * All operations are suspend functions that return Result, allowing callers
 * to handle both success and failure cases explicitly.
 *
 * Thread Safety: All operations are thread-safe and can be called from any coroutine.
 *
 * Usage:
 * ```kotlin
 * val effectManager = EffectManagerFactory.create(scope)
 *
 * // Observe effect state
 * effectManager.effectsState.collect { effects ->
 *     updateUI(effects)
 * }
 *
 * // Add an effect
 * effectManager.addEffect(EffectType.REVERB)
 *     .onSuccess { effect -> showToast("Effect added") }
 *     .onFailure { error -> showError(error.message) }
 * ```
 *
 * @see EffectManagerFactory for creating instances
 */
interface IEffectManager {

    /**
     * Current state of effects synchronized with C++.
     *
     * This StateFlow is the single source of truth for effect state.
     * UI should observe this flow and not maintain separate state.
     */
    val effectsState: StateFlow<List<EffectState>>

    /**
     * Maximum number of effects allowed in the chain.
     */
    val maxEffects: Int

    /**
     * Adds a new effect to the chain.
     *
     * The effect is added with default parameters. Use [setParameter] or
     * [applyPreset] to customize after adding.
     *
     * @param type The type of effect to add
     * @return Result containing the created [EffectState], or failure with:
     *   - [MaxEffectsReachedException] if chain is full
     *   - [SyncTimeoutException] if sync confirmation times out
     *   - Other exceptions for native errors
     */
    suspend fun addEffect(type: EffectType): Result<EffectState>

    /**
     * Removes an effect from the chain.
     *
     * Note: Removing an effect causes subsequent effects to shift indices.
     * Always re-query [effectsState] after removal.
     *
     * @param index The index of the effect to remove
     * @return Result.success if removed, or failure with:
     *   - [EffectNotFoundException] if index is invalid
     *   - [SyncTimeoutException] if sync confirmation times out
     */
    suspend fun removeEffect(index: Int): Result<Unit>

    /**
     * Updates a single parameter on an effect.
     *
     * For updating multiple parameters at once, prefer [applyPreset] which
     * uses batch operations for better performance.
     *
     * @param effectIndex The index of the effect
     * @param paramId The parameter ID to update
     * @param value The new value
     * @return Result.success if updated, or failure with appropriate exception
     */
    suspend fun setParameter(effectIndex: Int, paramId: Int, value: Float): Result<Unit>

    /**
     * Applies a preset to an effect (batch parameter update).
     *
     * This is more efficient than calling [setParameter] multiple times
     * as it uses a single JNI call.
     *
     * @param effectIndex The index of the effect
     * @param preset The preset containing parameter values
     * @return Result.success if applied, or failure with appropriate exception
     */
    suspend fun applyPreset(effectIndex: Int, preset: EffectPreset): Result<Unit>

    /**
     * Applies parameter updates across multiple effects in a single JNI call.
     *
     * Use this for scene loads or any case where the caller already has a
     * fully-formed snapshot of "what every effect's params should be". With
     * 10 effects × 5 params, the per-effect [applyPreset] path crosses JNI 10
     * times; this path crosses once and triggers a single state-version bump,
     * so [effectsState] emits one coherent post-batch state instead of N
     * intermediate ones.
     *
     * Caller is responsible for ensuring [updates] target valid effect
     * indices in the current chain (indices outside the chain are silently
     * skipped in native code).
     *
     * @param updates Parameter updates targeting any effect/param in the chain
     * @return Result.success if applied, or failure with appropriate exception
     */
    suspend fun setEffectParametersBatch(updates: List<EffectParameterUpdate>): Result<Unit>

    /**
     * Toggles the bypass state of an effect.
     *
     * When bypassed, an effect passes audio through without processing.
     *
     * @param index The index of the effect
     * @return Result containing the new bypass state (true = bypassed), or failure
     */
    suspend fun toggleBypass(index: Int): Result<Boolean>

    /**
     * Sets the bypass state of an effect explicitly.
     *
     * @param index The index of the effect
     * @param bypassed true to bypass, false to enable
     * @return Result.success if set, or failure with appropriate exception
     */
    suspend fun setBypass(index: Int, bypassed: Boolean): Result<Unit>

    /**
     * Reorders effects in the chain.
     *
     * The effect at [fromIndex] is moved to [toIndex], shifting other
     * effects as needed.
     *
     * @param fromIndex The current index of the effect to move
     * @param toIndex The target index
     * @return Result.success if reordered, or failure with appropriate exception
     */
    suspend fun reorderEffects(fromIndex: Int, toIndex: Int): Result<Unit>

    /**
     * Removes all effects from the chain.
     *
     * @return Result.success if all effects removed, or failure
     */
    suspend fun clearAllEffects(): Result<Unit>

    /**
     * Checks if another effect can be added to the chain.
     *
     * @return true if [effectsState].size < [maxEffects]
     */
    fun canAddEffect(): Boolean

    /**
     * Gets the effect at the specified index, or null if invalid.
     *
     * This is a convenience method for quick access without suspending.
     *
     * @param index The effect index
     * @return The effect state, or null if index is out of bounds
     */
    fun getEffect(index: Int): EffectState?

    /**
     * Disposes the effect manager and releases resources.
     *
     * After calling dispose(), the manager should not be used.
     */
    fun dispose()

    // ========== ROUTING MODE (lock-free, non-suspend) ==========

    /**
     * Set the routing mode for the effect chain.
     * Lock-free: takes effect immediately with crossfade transition.
     *
     * @param mode RoutingMode ordinal (0=Serial, 1=Parallel, 2=Split2x2, 3=SerialParallel, 4=ParallelSerial, 5=Feedback)
     */
    fun setRoutingMode(mode: Int)

    /**
     * Get the current routing mode ordinal.
     */
    fun getRoutingMode(): Int

    /**
     * Set the parallel mix balance for routing modes with branches.
     * @param mix 0.0 = branch A only, 1.0 = branch B only
     */
    fun setParallelMix(mix: Float)

    /**
     * Set the feedback amount for Feedback routing mode.
     * @param amount 0.0 to 0.95 (clamped in C++)
     */
    fun setFeedbackAmount(amount: Float)
}

/**
 * Preset containing parameter values for an effect.
 *
 * @property name Display name of the preset
 * @property effectType The effect type this preset applies to
 * @property parameters Map of parameter IDs to values
 * @property isDefault True if this is the default preset for the effect type
 */
data class EffectPreset(
    val name: String,
    val effectType: EffectType,
    val parameters: Map<Int, Float>,
    val isDefault: Boolean = false
)

// ==================== Exceptions ====================

/**
 * Thrown when attempting to add an effect but the chain is full.
 */
class MaxEffectsReachedException(
    val maxEffects: Int = 12
) : Exception("Maximum number of effects reached ($maxEffects)")

/**
 * Thrown when an effect at the specified index does not exist.
 */
class EffectNotFoundException(
    val index: Int,
    val chainSize: Int = -1
) : Exception(
    if (chainSize >= 0) "Effect not found at index $index (chain size: $chainSize)"
    else "Effect not found at index $index"
)

/**
 * Thrown when waiting for C++ state synchronization times out.
 */
class SyncTimeoutException(
    val timeoutMs: Long = 500L,
    val operation: String = "unknown"
) : Exception("Sync timeout after ${timeoutMs}ms waiting for '$operation' confirmation")

/**
 * Thrown when the preset type doesn't match the effect type.
 */
class PresetTypeMismatchException(
    val expectedType: EffectType,
    val actualType: EffectType
) : Exception("Preset type mismatch: expected ${expectedType.displayName}, got ${actualType.displayName}")
