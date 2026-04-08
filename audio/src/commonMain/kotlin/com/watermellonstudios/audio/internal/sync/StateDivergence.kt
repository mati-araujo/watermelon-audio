package com.watermellonstudios.audio.internal.sync

/**
 * Represents detected divergence between local (Kotlin) and native (C++) state.
 *
 * @property effectCountMismatch True if the number of effects differs
 * @property parameterDivergences List of parameter value mismatches
 * @property bypassDivergences List of bypass state mismatches
 * @property orderDivergence True if effect order differs (same effects, different positions)
 * @property typeMismatches List of effect type mismatches at same index
 */
data class StateDivergence(
    val effectCountMismatch: Boolean = false,
    val parameterDivergences: List<ParameterDivergence> = emptyList(),
    val bypassDivergences: List<BypassDivergence> = emptyList(),
    val orderDivergence: Boolean = false,
    val typeMismatches: List<TypeMismatch> = emptyList()
) {
    /**
     * True if any divergence exists.
     */
    val hasDivergence: Boolean
        get() = effectCountMismatch ||
                parameterDivergences.isNotEmpty() ||
                bypassDivergences.isNotEmpty() ||
                orderDivergence ||
                typeMismatches.isNotEmpty()

    /**
     * Total count of individual divergences.
     */
    val divergenceCount: Int
        get() = (if (effectCountMismatch) 1 else 0) +
                parameterDivergences.size +
                bypassDivergences.size +
                (if (orderDivergence) 1 else 0) +
                typeMismatches.size

    companion object {
        /** No divergence detected */
        val NONE = StateDivergence()
    }
}

/**
 * Represents a parameter value mismatch between local and native state.
 *
 * @property effectIndex Index of the effect in the chain
 * @property paramId Parameter ID that diverged
 * @property localValue Value in Kotlin state
 * @property nativeValue Value in C++ state
 */
data class ParameterDivergence(
    val effectIndex: Int,
    val paramId: Int,
    val localValue: Float,
    val nativeValue: Float
) {
    /**
     * Absolute difference between values.
     */
    val difference: Float
        get() = kotlin.math.abs(localValue - nativeValue)
}

/**
 * Represents a bypass state mismatch between local and native state.
 *
 * @property effectIndex Index of the effect in the chain
 * @property localBypassed Bypass state in Kotlin
 * @property nativeBypassed Bypass state in C++
 */
data class BypassDivergence(
    val effectIndex: Int,
    val localBypassed: Boolean,
    val nativeBypassed: Boolean
)

/**
 * Represents an effect type mismatch at the same index.
 *
 * @property effectIndex Index where mismatch occurred
 * @property localTypeId Type ID in Kotlin state
 * @property nativeTypeId Type ID in C++ state
 */
data class TypeMismatch(
    val effectIndex: Int,
    val localTypeId: Int,
    val nativeTypeId: Int
)
