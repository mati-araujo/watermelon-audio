package com.watermellonstudios.audio.api

/**
 * Represents a single effect parameter update for batch operations.
 *
 * Used by [IEffectManager.setEffectParametersBatch] to apply many parameter
 * changes across multiple effects in a single JNI call. The intended use case
 * is scene loading, where 10 effects × 5 params (= 50 individual updates)
 * would otherwise translate to 50 JNI roundtrips.
 *
 * @param effectIndex Index of the effect in the chain (0-based)
 * @param paramId Parameter ID within the effect
 * @param value New value for the parameter
 */
data class EffectParameterUpdate(
    val effectIndex: Int,
    val paramId: Int,
    val value: Float
)
