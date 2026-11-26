package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.error.NativeBridgeException
import com.watermellonstudios.audio.domain.error.NativeErrorCode
import com.watermellonstudios.audio.internal.bridge.AudioNativeBridge

/**
 * Onsets (golpes) de un buffer estéreo del llamador (REQ-055 S15): **el mismo algoritmo** que usa el
 * looper para derivar el tempo de una toma libre (`wma_looper_detect_onsets`), sin motor, sin handle
 * y sin estado. Es lo que un re-render usa para medir contra la toma original (hop 256, calibrado en
 * S2: `docs/dev/render-offline.md`).
 *
 * No es RT ni liviano: recorre todo el buffer y reserva tres vectores de `frames / hopFrames`
 * floats; se llama desde un hilo de trabajo, nunca desde un callback de audio.
 */
object OnsetAnalysis {
    private const val MIN_SAMPLE_RATE = 8_000
    private const val MAX_SAMPLE_RATE = 192_000
    private const val MIN_HOP_FRAMES = 32

    /**
     * Los frames de cada onset de [stereo] (estéreo intercalado: `frames * 2` floats, sólo se
     * lee), ascendentes y relativos al inicio del buffer; el array tiene exactamente los onsets
     * hallados, hasta [maxOnsets].
     *
     * [hopFrames] es la ventana de análisis (256 ≈ 5,3 ms a 48 kHz; el motor sube lo menor que 32 a
     * 32). [sensitivity] > 1 da más onsets, < 1 menos.
     *
     * Falla con `ParameterOutOfRange` si [frames] <= 0, si [stereo] tiene menos de `frames * 2`
     * floats, si [sampleRate] no está en 8000..192000 o si [maxOnsets] <= 0. Un buffer silencioso
     * o demasiado corto (menos de cuatro ventanas) no es un error: da un array vacío.
     */
    fun detect(
        stereo: FloatArray,
        frames: Int,
        sampleRate: Int,
        hopFrames: Int = 256,
        sensitivity: Float = 1f,
        maxOnsets: Int = 4096,
    ): Result<IntArray> = detect(AudioNativeBridge.getInstance(), stereo, frames, sampleRate, hopFrames, sensitivity, maxOnsets)

    internal fun detect(
        bridge: AudioNativeBridge,
        stereo: FloatArray,
        frames: Int,
        sampleRate: Int,
        hopFrames: Int,
        sensitivity: Float,
        maxOnsets: Int,
    ): Result<IntArray> {
        if (frames <= 0 || stereo.size.toLong() < frames.toLong() * 2L ||
            sampleRate !in MIN_SAMPLE_RATE..MAX_SAMPLE_RATE || maxOnsets <= 0
        ) {
            return outOfRange("OnsetAnalysis.detect")
        }
        // Nunca hay más onsets que ventanas (hop >= 32): un maxOnsets enorme no reserva de más.
        val capacity = minOf(maxOnsets.toLong(), frames.toLong() / MIN_HOP_FRAMES + 1L).toInt()
        val out = IntArray(capacity)
        val code = bridge.detectOnsets(stereo, frames, sampleRate, hopFrames, sensitivity, out)
        return if (code in 0..capacity) {
            Result.success(if (code == capacity) out else out.copyOf(code))
        } else {
            Result.failure(NativeBridgeException.fromCode(if (code < 0) code else NativeErrorCode.UNKNOWN_ERROR.code, "OnsetAnalysis.detect"))
        }
    }

    private fun outOfRange(operation: String): Result<IntArray> =
        Result.failure(NativeBridgeException.fromCode(NativeErrorCode.PARAMETER_OUT_OF_RANGE.code, operation))
}
