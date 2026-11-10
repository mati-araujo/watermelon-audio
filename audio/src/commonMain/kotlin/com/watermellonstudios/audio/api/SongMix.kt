package com.watermellonstudios.audio.api

/**
 * Dónde toma el motor su salida (REQ-055 S12, ADR-0013). El código es el de `WmaOutputProfile`
 * de la C API.
 */
enum class OutputProfile(val code: Int) {
    /** El default: la cadena de protección de siempre (límite, soft clip, dither). */
    PROTECTED(0),

    /** El master de Studio: ganancia + limitador true-peak, y DESPUÉS el volumen de monitoreo. */
    MASTER(1),

    /** Nada. Sólo una instancia offline (un motor con device da `INVALID_OPERATION`). */
    RAW(2),
}

/**
 * El bus «Espacio» de la canción: un reverb 100 % wet en paralelo con un delay 100 % wet,
 * cada uno con su retorno. Rangos de la C API (fuera de rango ⇒ `PARAMETER_OUT_OF_RANGE`, sin
 * acotar): retornos 0..1, tamaño 0,1..2, decay 0,1..10 s, damping 0..0,95, predelay 0..100 ms,
 * cortes 20..500 Hz y 1000..20000 Hz, delay 1..2000 ms (nunca sincronizado al tempo: lo calcula
 * quien llama), feedback 0..0,9 (el interpolador del delay llega a 1,0887: con más de ~0,9186
 * un tiempo fraccional diverge; con damping 1 la reverb nunca se apaga).
 */
data class SongSpace(
    val reverbReturn: Float,
    val reverbSize: Float,
    val reverbDecaySeconds: Float,
    val reverbDamping: Float,
    val reverbPreDelayMs: Float,
    val reverbLowCutHz: Float,
    val reverbHighCutHz: Float,
    val delayReturn: Float,
    val delayTimeMs: Float,
    val delayFeedback: Float,
)

/**
 * El master: ganancia en dB (−60..+40, rampa lineal por bloque) y limitador brickwall true-peak
 * (techo −20..0 dBTP, release 1..2000 ms).
 */
data class MasterParams(
    val gainDb: Float,
    val ceilingDbtp: Float = DEFAULT_CEILING_DBTP,
    val releaseMs: Float = DEFAULT_RELEASE_MS,
) {
    companion object {
        const val DEFAULT_CEILING_DBTP = -1.0f
        const val DEFAULT_RELEASE_MS = 150.0f
    }
}
