package com.watermellonstudios.audio.domain.effect

import com.watermellonstudios.audio.api.EffectPreset

/**
 * Factory presets for guitar-focused delay and reverb effects.
 */
object GuitarEffectPresets {

    val SLAPBACK = EffectPreset(
        name = "Slapback",
        effectType = EffectType.TAPE_ECHO,
        parameters = mapOf(
            EffectConstants.TapeEcho.DELAY_TIME to 95f,
            EffectConstants.TapeEcho.FEEDBACK to 0.18f,
            EffectConstants.TapeEcho.WOW_FLUTTER to 0.08f,
            EffectConstants.TapeEcho.TAPE_AGE to 0.25f,
            EffectConstants.TapeEcho.SATURATION to 0.25f,
            EffectConstants.TapeEcho.MIX to 0.22f,
            EffectConstants.TapeEcho.NOISE_LEVEL to 0f
        ),
        isDefault = true
    )

    val ANALOG_LEAD = EffectPreset(
        name = "Analog Lead",
        effectType = EffectType.TAPE_ECHO,
        parameters = mapOf(
            EffectConstants.TapeEcho.DELAY_TIME to 360f,
            EffectConstants.TapeEcho.FEEDBACK to 0.42f,
            EffectConstants.TapeEcho.WOW_FLUTTER to 0.22f,
            EffectConstants.TapeEcho.TAPE_AGE to 0.45f,
            EffectConstants.TapeEcho.SATURATION to 0.35f,
            EffectConstants.TapeEcho.MIX to 0.32f,
            EffectConstants.TapeEcho.DUCKING to 0.25f,
            EffectConstants.TapeEcho.NOISE_LEVEL to 0.12f
        )
    )

    val DOTTED_EIGHTH = EffectPreset(
        name = "Dotted Eighth",
        effectType = EffectType.TAPE_ECHO,
        parameters = mapOf(
            EffectConstants.TapeEcho.SYNC to 1f,
            EffectConstants.TapeEcho.SUBDIVISION to EffectConstants.DelaySubdivision.DOTTED_EIGHTH.toFloat(),
            EffectConstants.TapeEcho.FEEDBACK to 0.38f,
            EffectConstants.TapeEcho.WOW_FLUTTER to 0.12f,
            EffectConstants.TapeEcho.TAPE_AGE to 0.35f,
            EffectConstants.TapeEcho.SATURATION to 0.2f,
            EffectConstants.TapeEcho.MIX to 0.36f,
            EffectConstants.TapeEcho.DUCKING to 0.3f
        )
    )

    val DARK_SOLO = EffectPreset(
        name = "Dark Solo",
        effectType = EffectType.HPF_DELAY,
        parameters = mapOf(
            EffectConstants.HpfDelay.HPF_CUTOFF to 180f,
            EffectConstants.HpfDelay.DELAY_TIME to 430f,
            EffectConstants.HpfDelay.FEEDBACK to 0.5f,
            EffectConstants.HpfDelay.MIX to 0.28f,
            EffectConstants.HpfDelay.LPF_CUTOFF to 5200f,
            EffectConstants.HpfDelay.DUCKING to 0.35f
        )
    )

    val AMBIENT_ECHO = EffectPreset(
        name = "Ambient Echo",
        effectType = EffectType.HPF_DELAY,
        parameters = mapOf(
            EffectConstants.HpfDelay.SYNC to 1f,
            EffectConstants.HpfDelay.SUBDIVISION to EffectConstants.DelaySubdivision.DOTTED_QUARTER.toFloat(),
            EffectConstants.HpfDelay.HPF_CUTOFF to 320f,
            EffectConstants.HpfDelay.FEEDBACK to 0.62f,
            EffectConstants.HpfDelay.MIX to 0.42f,
            EffectConstants.HpfDelay.PING_PONG to 1f,
            EffectConstants.HpfDelay.LPF_CUTOFF to 7600f,
            EffectConstants.HpfDelay.DUCKING to 0.25f
        )
    )

    val SPRING_COMBO = EffectPreset(
        name = "Spring Combo",
        effectType = EffectType.SPRING_REVERB,
        parameters = mapOf(
            EffectConstants.SpringReverb.DECAY to 1.9f,
            EffectConstants.SpringReverb.TONE to 0.55f,
            EffectConstants.SpringReverb.DRIP to 0.42f,
            EffectConstants.SpringReverb.TENSION to 0.48f,
            EffectConstants.SpringReverb.MIX to 0.24f
        ),
        isDefault = true
    )

    val PLATE_SOLO = EffectPreset(
        name = "Plate Solo",
        effectType = EffectType.PLATE_REVERB,
        parameters = mapOf(
            EffectConstants.PlateReverb.DECAY to 2.8f,
            EffectConstants.PlateReverb.PRE_DELAY to 28f,
            EffectConstants.PlateReverb.DAMPING to 0.32f,
            EffectConstants.PlateReverb.MODULATION to 0.14f,
            EffectConstants.PlateReverb.LOW_CUT to 140f,
            EffectConstants.PlateReverb.HIGH_CUT to 9800f,
            EffectConstants.PlateReverb.MIX to 0.3f
        )
    )

    val SMALL_ROOM = EffectPreset(
        name = "Small Room",
        effectType = EffectType.HALL_REVERB,
        parameters = mapOf(
            EffectConstants.HallReverb.DECAY_TIME to 1.1f,
            EffectConstants.HallReverb.SIZE to 0.28f,
            EffectConstants.HallReverb.PRE_DELAY to 8f,
            EffectConstants.HallReverb.DIFFUSION to 0.45f,
            EffectConstants.HallReverb.HF_DAMPING to 0.55f,
            EffectConstants.HallReverb.LF_DAMPING to 0.35f,
            EffectConstants.HallReverb.MODULATION to 0.05f,
            EffectConstants.HallReverb.MIX to 0.18f
        )
    )

    val DARK_HALL = EffectPreset(
        name = "Dark Hall",
        effectType = EffectType.HALL_REVERB,
        parameters = mapOf(
            EffectConstants.HallReverb.DECAY_TIME to 4.2f,
            EffectConstants.HallReverb.SIZE to 0.8f,
            EffectConstants.HallReverb.PRE_DELAY to 42f,
            EffectConstants.HallReverb.DIFFUSION to 0.82f,
            EffectConstants.HallReverb.HF_DAMPING to 0.72f,
            EffectConstants.HallReverb.LF_DAMPING to 0.42f,
            EffectConstants.HallReverb.MODULATION to 0.18f,
            EffectConstants.HallReverb.MIX to 0.32f
        )
    )

    val SHIMMER_PAD = EffectPreset(
        name = "Shimmer Pad",
        effectType = EffectType.SHIMMER_REVERB,
        parameters = mapOf(
            EffectConstants.ShimmerReverb.DECAY to 7.5f,
            EffectConstants.ShimmerReverb.SIZE to 0.9f,
            EffectConstants.ShimmerReverb.PITCH_SEMITONES to 12f,
            EffectConstants.ShimmerReverb.SHIMMER_AMOUNT to 0.48f,
            EffectConstants.ShimmerReverb.FEEDBACK to 0.42f,
            EffectConstants.ShimmerReverb.TONE to 0.68f,
            EffectConstants.ShimmerReverb.MIX to 0.38f
        )
    )

    val SHOEGAZE_WASH = EffectPreset(
        name = "Shoegaze Wash",
        effectType = EffectType.SHIMMER_REVERB,
        parameters = mapOf(
            EffectConstants.ShimmerReverb.DECAY to 10.0f,
            EffectConstants.ShimmerReverb.SIZE to 1.0f,
            EffectConstants.ShimmerReverb.PITCH_SEMITONES to 7f,
            EffectConstants.ShimmerReverb.SHIMMER_AMOUNT to 0.38f,
            EffectConstants.ShimmerReverb.FEEDBACK to 0.55f,
            EffectConstants.ShimmerReverb.TONE to 0.52f,
            EffectConstants.ShimmerReverb.MIX to 0.5f
        )
    )

    val DELAY_PRESETS = listOf(SLAPBACK, ANALOG_LEAD, DOTTED_EIGHTH, DARK_SOLO, AMBIENT_ECHO)
    val REVERB_PRESETS = listOf(SPRING_COMBO, PLATE_SOLO, SMALL_ROOM, DARK_HALL, SHIMMER_PAD, SHOEGAZE_WASH)
    val ALL_PRESETS = DELAY_PRESETS + REVERB_PRESETS

    fun forEffectType(type: EffectType): List<EffectPreset> =
        ALL_PRESETS.filter { it.effectType == type }
}
