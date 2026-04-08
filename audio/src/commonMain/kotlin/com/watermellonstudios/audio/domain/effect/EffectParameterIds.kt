package com.watermellonstudios.audio.domain.effect

/**
 * Parameter IDs for audio effects.
 * These IDs match the C++ engine parameter indices.
 */
object EffectParameterIds {

    // ===========================================
    // VOCODER PARAMETERS
    // ===========================================
    const val PARAM_VOC_BAND_COUNT = 0     // Number of bands (4-32)
    const val PARAM_VOC_FORMANT_SHIFT = 1  // Formant shift in semitones (-24 to +24)
    const val PARAM_VOC_ATTACK = 2         // Envelope attack (0.1-100 ms)
    const val PARAM_VOC_RELEASE = 3        // Envelope release (1-500 ms)
    const val PARAM_VOC_MIX = 4            // Wet/dry mix (0.0-1.0)
    const val PARAM_VOC_CARRIER_LEVEL = 5  // Internal carrier level (0.0-1.0)
    const val PARAM_VOC_MOD_SOURCE = 6     // Modulator source (0=internal/self, 1=external mic)
    const val PARAM_VOC_CARRIER_SOURCE = 7 // Carrier source (0=input signal, 1=internal oscillator)
    const val PARAM_VOC_CARRIER_FREQ = 8   // Internal oscillator frequency (50-500 Hz)

    // ===========================================
    // DISTORTION PARAMETERS
    // ===========================================
    const val PARAM_DIST_DRIVE = 0        // Input drive (0.0-1.0)
    const val PARAM_DIST_TONE = 1         // Tone control (0.0-1.0, dark to bright)
    const val PARAM_DIST_LEVEL = 2        // Output level (0.0-1.0)
    const val PARAM_DIST_MIX = 3          // Wet/dry mix (0.0-1.0)
    const val PARAM_DIST_ALGORITHM = 4    // Pedal type (see PedalType enum)

    // Pedal-specific parameters
    const val PARAM_DIST_PARAM_A = 5      // Pedal-specific param A
    const val PARAM_DIST_PARAM_B = 6      // Pedal-specific param B
    const val PARAM_DIST_PARAM_C = 7      // Pedal-specific param C

    // Advanced parameters
    const val PARAM_DIST_OVERSAMPLE = 8   // Oversampling (0=1x, 1=2x, 2=4x)
    const val PARAM_DIST_PRE_LOW_CUT = 9  // Pre-distortion HPF (20-500 Hz)
    const val PARAM_DIST_POST_HIGH_CUT = 10 // Post-distortion LPF (1k-20k Hz)
    const val PARAM_DIST_SAG = 11         // Voltage sag simulation (0.0-1.0)
    const val PARAM_DIST_BIAS = 12        // Transistor bias for fuzz (0.0-1.0)
    const val PARAM_DIST_GATE = 13        // Noise gate threshold (0.0-1.0, 0=off)

    // ===========================================
    // PEDAL TYPES (for PARAM_DIST_ALGORITHM)
    // ===========================================

    // Overdrive pedals (0-3)
    const val PEDAL_TUBE_SCREAMER = 0     // Ibanez TS-808/TS9 style
    const val PEDAL_BOSS_OVERDRIVE = 1    // Boss OD-1/SD-1 style
    const val PEDAL_KLON = 2              // Klon Centaur style
    const val PEDAL_OCD = 3               // Fulltone OCD style

    // Distortion pedals (4-7)
    const val PEDAL_BOSS_DS1 = 4          // Boss DS-1 style
    const val PEDAL_RAT = 5               // ProCo RAT style
    const val PEDAL_DIST_PLUS = 6         // MXR Distortion+ style
    const val PEDAL_METAL_ZONE = 7        // Boss MT-2 style

    // Fuzz pedals (8-11)
    const val PEDAL_BIG_MUFF = 8          // EHX Big Muff Pi style
    const val PEDAL_FUZZ_FACE_GERM = 9    // Fuzz Face Germanium
    const val PEDAL_FUZZ_FACE_SI = 10     // Fuzz Face Silicon
    const val PEDAL_OCTAVE_FUZZ = 11      // Octavia style

    // Special pedals (12-13)
    const val PEDAL_HM2_CHAINSAW = 12     // Boss HM-2 Swedish death metal
    const val PEDAL_DOOM_FUZZ = 13        // Sunn/Acapulco Gold style

    // Legacy algorithms (100+)
    const val PEDAL_LEGACY_SOFT_CLIP = 100
    const val PEDAL_LEGACY_HARD_CLIP = 101
    const val PEDAL_LEGACY_TUBE_SIM = 102
    const val PEDAL_LEGACY_FOLDBACK = 103
    const val PEDAL_LEGACY_BITCRUSH = 104
}
