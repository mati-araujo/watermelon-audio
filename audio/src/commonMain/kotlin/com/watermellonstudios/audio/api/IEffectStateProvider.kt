package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.effect.EffectChainState
import com.watermellonstudios.audio.domain.effect.EffectState
import com.watermellonstudios.audio.domain.effect.EffectType

/**
 * Interface for reading effect state from the native audio engine.
 *
 * This interface is used for polling state from C++ to keep Kotlin in sync.
 * All operations are suspend functions to support coroutine-based polling.
 */
interface IEffectStateProvider {

    /**
     * Gets a complete snapshot of the effect chain state.
     *
     * @return The current state of all effects in the chain
     */
    suspend fun getEffectChainSnapshot(): EffectChainSnapshot

    /**
     * Gets the current parameters for a specific effect.
     *
     * @param index The effect index in the chain
     * @return Map of parameter IDs to their current values
     * @throws com.watermellonstudios.audio.domain.error.NativeBridgeException.InvalidEffectIndex if index is invalid
     */
    suspend fun getEffectParameters(index: Int): Map<Int, Float>

    /**
     * Checks if a specific effect is bypassed.
     *
     * @param index The effect index in the chain
     * @return true if the effect is bypassed
     * @throws com.watermellonstudios.audio.domain.error.NativeBridgeException.InvalidEffectIndex if index is invalid
     */
    suspend fun isEffectBypassed(index: Int): Boolean

    /**
     * Gets the current number of effects in the chain.
     *
     * @return The number of effects
     */
    suspend fun getEffectCount(): Int

    /**
     * Gets the type of effect at a specific index.
     *
     * @param index The effect index in the chain
     * @return The effect type, or null if index is invalid
     */
    suspend fun getEffectType(index: Int): EffectType?
}

/**
 * Snapshot of the entire effect chain from native code.
 *
 * @property effects List of effect snapshots in order
 * @property version Monotonically increasing version number for change detection
 * @property timestamp Timestamp when snapshot was taken (System.nanoTime())
 */
data class EffectChainSnapshot(
    val effects: List<NativeEffectSnapshot>,
    val version: Long,
    val timestamp: Long = System.nanoTime()
) {
    val size: Int get() = effects.size
    val isEmpty: Boolean get() = effects.isEmpty()

    /**
     * Converts to domain EffectChainState.
     */
    fun toEffectChainState(): EffectChainState {
        return EffectChainState(
            effects = effects.map { it.toEffectState() }
        )
    }
}

/**
 * Snapshot of a single effect from native code.
 *
 * @property index Position in the chain
 * @property typeId Native type ID
 * @property isBypassed Whether the effect is bypassed
 * @property parameters Current parameter values
 */
data class NativeEffectSnapshot(
    val index: Int,
    val typeId: Int,
    val isBypassed: Boolean,
    val parameters: Map<Int, Float>
) {
    /**
     * Converts to domain EffectState.
     */
    fun toEffectState(): EffectState {
        return EffectState(
            index = index,
            type = EffectType.fromId(typeId) ?: EffectType.FILTER,
            isBypassed = isBypassed,
            parameters = parameters
        )
    }
}
