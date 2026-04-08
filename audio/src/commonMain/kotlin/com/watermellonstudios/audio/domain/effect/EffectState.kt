package com.watermellonstudios.audio.domain.effect

/**
 * Represents the state of a single effect in the chain.
 *
 * @property index Position in the effect chain (0-based)
 * @property type Type of effect
 * @property isBypassed Whether the effect is bypassed
 * @property parameters Current parameter values (paramId -> value)
 */
data class EffectState(
    val index: Int,
    val type: EffectType,
    val isBypassed: Boolean = false,
    val parameters: Map<Int, Float> = emptyMap()
)

/**
 * Represents the state of the entire effect chain.
 *
 * @property effects List of effects in order
 * @property maxEffects Maximum number of effects allowed
 */
data class EffectChainState(
    val effects: List<EffectState> = emptyList(),
    val maxEffects: Int = 12
) {
    val canAddEffect: Boolean get() = effects.size < maxEffects
    val isEmpty: Boolean get() = effects.isEmpty()
}
