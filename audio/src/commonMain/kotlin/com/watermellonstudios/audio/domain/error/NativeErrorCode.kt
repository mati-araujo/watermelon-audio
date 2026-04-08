package com.watermellonstudios.audio.domain.error

/**
 * Error codes synchronized between Kotlin and C++.
 *
 * These codes are returned by JNI functions to provide structured error information.
 * Negative values indicate errors, 0 or positive values indicate success.
 *
 * @property code The integer code used in JNI communication
 */
enum class NativeErrorCode(val code: Int) {
    /** Operation completed successfully */
    SUCCESS(0),

    /** Audio engine not initialized */
    ENGINE_NOT_INITIALIZED(-1),

    /** Effect index out of bounds */
    INVALID_EFFECT_INDEX(-2),

    /** Parameter ID not recognized for the effect type */
    INVALID_PARAMETER_ID(-3),

    /** Parameter value outside valid range */
    PARAMETER_OUT_OF_RANGE(-4),

    /** Effect chain is full (max 3 effects) */
    EFFECT_CHAIN_FULL(-5),

    /** Memory allocation failed */
    MEMORY_ALLOCATION_FAILED(-6),

    /** Audio stream error */
    STREAM_ERROR(-7),

    /** Mode transition is in progress */
    MODE_TRANSITION_IN_PROGRESS(-8),

    /** Operation not valid in current state */
    INVALID_OPERATION(-9),

    /** Effect type not recognized */
    INVALID_EFFECT_TYPE(-10),

    /** Timeout waiting for operation */
    TIMEOUT(-11),

    /** Unknown error */
    UNKNOWN_ERROR(-99);

    val isSuccess: Boolean get() = code >= 0
    val isError: Boolean get() = code < 0

    companion object {
        /**
         * Converts a JNI return code to NativeErrorCode.
         *
         * @param code The code returned from JNI
         * @return The corresponding NativeErrorCode, or UNKNOWN_ERROR if not found
         */
        fun fromCode(code: Int): NativeErrorCode =
            entries.find { it.code == code } ?: UNKNOWN_ERROR

        /**
         * Checks if a JNI return code indicates success.
         */
        fun isSuccessCode(code: Int): Boolean = code >= 0
    }
}
