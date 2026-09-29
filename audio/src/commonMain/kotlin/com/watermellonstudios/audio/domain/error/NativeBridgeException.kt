package com.watermellonstudios.audio.domain.error

/**
 * Typed exceptions for native bridge errors.
 *
 * Each exception provides specific context about what went wrong,
 * making debugging and error handling more straightforward.
 */
sealed class NativeBridgeException(
    message: String,
    val errorCode: NativeErrorCode,
    cause: Throwable? = null
) : Exception(message, cause) {

    /**
     * Audio engine was not initialized when operation was attempted.
     */
    class EngineNotInitialized(
        message: String = "Audio engine not initialized"
    ) : NativeBridgeException(message, NativeErrorCode.ENGINE_NOT_INITIALIZED)

    /**
     * Effect index was out of bounds.
     *
     * @property index The invalid index that was used
     * @property chainSize Current size of the effect chain
     */
    class InvalidEffectIndex(
        val index: Int,
        val chainSize: Int = -1
    ) : NativeBridgeException(
        if (chainSize >= 0)
            "Invalid effect index: $index (chain size: $chainSize)"
        else
            "Invalid effect index: $index",
        NativeErrorCode.INVALID_EFFECT_INDEX
    )

    /**
     * Parameter ID was not recognized for the effect type.
     *
     * @property paramId The invalid parameter ID
     * @property effectType The effect type (if known)
     */
    class InvalidParameterId(
        val paramId: Int,
        val effectType: String? = null
    ) : NativeBridgeException(
        if (effectType != null)
            "Invalid parameter ID $paramId for effect type $effectType"
        else
            "Invalid parameter ID: $paramId",
        NativeErrorCode.INVALID_PARAMETER_ID
    )

    /**
     * Parameter value was outside the valid range.
     *
     * @property paramId The parameter ID
     * @property value The invalid value
     * @property minValue The minimum valid value
     * @property maxValue The maximum valid value
     */
    class ParameterOutOfRange(
        val paramId: Int,
        val value: Float,
        val minValue: Float,
        val maxValue: Float
    ) : NativeBridgeException(
        "Parameter $paramId value $value out of range [$minValue, $maxValue]",
        NativeErrorCode.PARAMETER_OUT_OF_RANGE
    )

    /**
     * Effect chain is full and cannot accept more effects.
     *
     * @property maxEffects The maximum number of effects allowed
     */
    class EffectChainFull(
        val maxEffects: Int = 12
    ) : NativeBridgeException(
        "Effect chain full (max $maxEffects effects)",
        NativeErrorCode.EFFECT_CHAIN_FULL
    )

    /**
     * Memory allocation failed in native code.
     */
    class MemoryAllocationFailed(
        message: String = "Memory allocation failed in native code"
    ) : NativeBridgeException(message, NativeErrorCode.MEMORY_ALLOCATION_FAILED)

    /**
     * Audio stream error occurred.
     *
     * @property streamErrorCode The specific stream error code from Oboe
     */
    class StreamError(
        val streamErrorCode: Int = 0,
        message: String = "Audio stream error (code: $streamErrorCode)"
    ) : NativeBridgeException(message, NativeErrorCode.STREAM_ERROR)

    /**
     * Operation cannot be performed because a mode transition is in progress.
     */
    class ModeTransitionInProgress(
        message: String = "Mode transition in progress"
    ) : NativeBridgeException(message, NativeErrorCode.MODE_TRANSITION_IN_PROGRESS)

    /**
     * Operation is not valid in the current state.
     *
     * @property operation The operation that was attempted
     * @property currentState The current state (if known)
     */
    class InvalidOperation(
        val operation: String,
        val currentState: String? = null
    ) : NativeBridgeException(
        if (currentState != null)
            "Invalid operation '$operation' in state '$currentState'"
        else
            "Invalid operation: $operation",
        NativeErrorCode.INVALID_OPERATION
    )

    /**
     * Effect type is not recognized.
     *
     * @property typeId The invalid type ID
     */
    class InvalidEffectType(
        val typeId: Int
    ) : NativeBridgeException(
        "Invalid effect type ID: $typeId",
        NativeErrorCode.INVALID_EFFECT_TYPE
    )

    /**
     * Operation timed out.
     *
     * @property operation The operation that timed out
     * @property timeoutMs The timeout value in milliseconds
     */
    class Timeout(
        val operation: String,
        val timeoutMs: Long
    ) : NativeBridgeException(
        "Operation '$operation' timed out after ${timeoutMs}ms",
        NativeErrorCode.TIMEOUT
    )

    /**
     * Lo que la operación necesitaba reservar no entra en el presupuesto de memoria
     * (REQ-045 D6). No es "no hay RAM": es "el tope configurado no lo permite", y por
     * eso la salida del consumidor es liberar pistas o subir el tier, no reintentar.
     *
     * @property neededBytes lo que hacía falta, si se conoce (-1 si no).
     * @property budgetBytes el tope vigente, si se conoce (-1 si no).
     */
    class MemoryBudgetExceeded(
        val neededBytes: Long = -1,
        val budgetBytes: Long = -1,
        message: String = "Memory budget exceeded" +
            if (neededBytes >= 0) " (needed $neededBytes B, budget $budgetBytes B)" else "",
    ) : NativeBridgeException(message, NativeErrorCode.MEMORY_BUDGET_EXCEEDED)

    /**
     * El archivo abrió y el motor no sabe decodificarlo (REQ-045 D6). WAV: PCM de 16 o
     * 24 bits e IEEE float de 32, 1 o 2 canales.
     */
    class UnsupportedFormat(
        val path: String? = null,
        message: String = "Unsupported audio format" + if (path != null) ": $path" else "",
    ) : NativeBridgeException(message, NativeErrorCode.UNSUPPORTED_FORMAT)

    /**
     * El archivo no abrió: falta, no se puede leer, o la ruta era nula (REQ-045 D6).
     * Distinto de [UnsupportedFormat] a propósito: la salida del consumidor es otra.
     */
    class IoError(
        val path: String? = null,
        message: String = "I/O error" + if (path != null) ": $path" else "",
    ) : NativeBridgeException(message, NativeErrorCode.IO_ERROR)

    /**
     * An unknown native error occurred.
     *
     * @property nativeCode The raw error code from native
     * @property details Additional details if available
     */
    class NativeError(
        val nativeCode: Int,
        val details: String = ""
    ) : NativeBridgeException(
        if (details.isNotEmpty())
            "Native error $nativeCode: $details"
        else
            "Native error: $nativeCode",
        NativeErrorCode.fromCode(nativeCode)
    )

    companion object {
        /**
         * Creates the appropriate exception from a native error code.
         */
        fun fromCode(code: Int, context: String = ""): NativeBridgeException {
            return when (NativeErrorCode.fromCode(code)) {
                NativeErrorCode.ENGINE_NOT_INITIALIZED -> EngineNotInitialized()
                NativeErrorCode.INVALID_EFFECT_INDEX -> InvalidEffectIndex(-1)
                NativeErrorCode.INVALID_PARAMETER_ID -> InvalidParameterId(-1)
                NativeErrorCode.PARAMETER_OUT_OF_RANGE ->
                    ParameterOutOfRange(-1, 0f, 0f, 0f)
                NativeErrorCode.EFFECT_CHAIN_FULL -> EffectChainFull()
                NativeErrorCode.MEMORY_ALLOCATION_FAILED -> MemoryAllocationFailed()
                NativeErrorCode.STREAM_ERROR -> StreamError(code)
                NativeErrorCode.MODE_TRANSITION_IN_PROGRESS -> ModeTransitionInProgress()
                NativeErrorCode.INVALID_OPERATION -> InvalidOperation(context)
                NativeErrorCode.INVALID_EFFECT_TYPE -> InvalidEffectType(-1)
                NativeErrorCode.TIMEOUT -> Timeout(context, 0)
                NativeErrorCode.MEMORY_BUDGET_EXCEEDED -> MemoryBudgetExceeded()
                NativeErrorCode.UNSUPPORTED_FORMAT -> UnsupportedFormat(context.ifEmpty { null })
                NativeErrorCode.IO_ERROR -> IoError(context.ifEmpty { null })
                else -> NativeError(code, context)
            }
        }
    }
}
