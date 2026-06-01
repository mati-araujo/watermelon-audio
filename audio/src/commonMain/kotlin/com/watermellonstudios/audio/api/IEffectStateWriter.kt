package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.effect.EffectType

/**
 * Interface for writing effect state changes to the native audio engine.
 *
 * All operations return Result to handle potential errors gracefully.
 * Operations are suspend functions to support async execution.
 */
interface IEffectStateWriter {

    /**
     * Adds a new effect to the chain.
     *
     * @param type The type of effect to add
     * @return Result containing the index of the new effect, or failure with exception
     */
    suspend fun addEffect(type: EffectType): Result<Int>

    /**
     * Removes an effect from the chain.
     *
     * Note: This may cause indices of subsequent effects to shift.
     *
     * @param index The index of the effect to remove
     * @return Result.success if removed, or failure with exception
     */
    suspend fun removeEffect(index: Int): Result<Unit>

    /**
     * Sets a parameter value for an effect.
     *
     * @param effectIndex The index of the effect
     * @param paramId The parameter ID
     * @param value The new value
     * @return Result.success if set, or failure with exception
     */
    suspend fun setParameter(effectIndex: Int, paramId: Int, value: Float): Result<Unit>

    /**
     * Sets multiple parameters at once (batch operation).
     *
     * This is more efficient than calling setParameter multiple times
     * as it uses a single JNI call.
     *
     * @param effectIndex The index of the effect
     * @param parameters Map of parameter IDs to values
     * @return Result.success if all set, or failure with exception
     */
    suspend fun setParametersBatch(effectIndex: Int, parameters: Map<Int, Float>): Result<Unit>

    /**
     * Applies parameter updates across multiple effects in a single JNI call.
     *
     * The chain-wide batch path. Intended for scene loads where the caller
     * has a fully-formed list of updates spanning every effect. Implementations
     * must apply all updates under the chain's existing lock-free atomic
     * snapshot and emit a single state-version bump at the end, so subscribers
     * see one coherent post-batch state rather than N intermediate snapshots.
     *
     * @param updates Parallel updates targeting any effect/param in the chain
     * @return Result.success if applied, or failure with exception
     */
    suspend fun setMultipleEffectParameters(updates: List<EffectParameterUpdate>): Result<Unit>

    /**
     * Sets the bypass state for an effect.
     *
     * @param effectIndex The index of the effect
     * @param bypassed true to bypass, false to enable
     * @return Result.success if set, or failure with exception
     */
    suspend fun setBypass(effectIndex: Int, bypassed: Boolean): Result<Unit>

    /**
     * Reorders effects in the chain.
     *
     * @param fromIndex The current index of the effect to move
     * @param toIndex The target index
     * @return Result.success if reordered, or failure with exception
     */
    suspend fun reorderEffects(fromIndex: Int, toIndex: Int): Result<Unit>

    /**
     * Clears all effects from the chain.
     *
     * @return Result.success if cleared, or failure with exception
     */
    suspend fun clearAllEffects(): Result<Unit>
}
