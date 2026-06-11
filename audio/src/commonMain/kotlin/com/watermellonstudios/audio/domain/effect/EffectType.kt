package com.watermellonstudios.audio.domain.effect

/**
 * Types of audio effects available in the engine.
 *
 * @property id Native ID used by the C++ engine
 * @property displayName Human-readable name for UI
 * @property maxInstances Maximum instances allowed in chain
 */
enum class EffectType(
    val id: Int,
    val displayName: String,
    val maxInstances: Int = 1
) {
    FILTER(0, "Filter"),
    REVERB(1, "Reverb"),
    DELAY(2, "Delay"),
    VOCODER(3, "Vocoder"),
    DISTORTION(4, "Distortion"),
    COMPRESSOR(5, "Compressor"),
    CHORUS(6, "Chorus"),
    PHASER(7, "Phaser"),
    AMP_SIM(8, "Amp Simulator"),
    CABINET(9, "Cabinet"),
    DECIMATOR(10, "Decimator"),
    DECI_HPF(11, "Deci-HPF"),
    AUTO_PAN(12, "AutoPan"),
    COMPLEX_TREM(13, "ComplexTrem"),
    RANDOM_RESO(14, "RandomReso"),
    HPF_DELAY(15, "HPF-Delay"),
    TAPE_ECHO(16, "TapeEcho"),
    HALL_REVERB(17, "Hall Reverb"),
    RISER_REVERB(18, "Riser Reverb"),
    BEAT_GRAIN(19, "BeatGrain"),
    SPRING_REVERB(20, "Spring Reverb"),
    PLATE_REVERB(21, "Plate Reverb"),
    SHIMMER_REVERB(22, "Shimmer Reverb");

    companion object {
        fun fromId(id: Int): EffectType? = entries.find { it.id == id }

        /**
         * Alias for fromId for consistency with other enums.
         */
        fun fromNativeId(id: Int): EffectType? = fromId(id)
    }
}
