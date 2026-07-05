package com.watermellonstudios.audio.domain.effect

/**
 * Canonical constants for audio effects.
 *
 * This file consolidates all effect-related constants that were previously
 * scattered across AudioEngineBridge (app module) and feature-effects.
 *
 * These constants match the C++ native engine definitions.
 *
 * MIGRATION NOTE (Phase 7.0):
 * - Migrated from app/audio/AudioEngineBridge.kt
 * - Migrated from feature-effects/EffectConstants.kt
 * - EffectParameterIds.kt contains vocoder and distortion params (kept separate for clarity)
 */
object EffectConstants {

    // ===========================================
    // EFFECT TYPE IDs
    // ===========================================

    /** Filter effect type ID */
    const val FILTER = 0

    /** Reverb effect type ID */
    const val REVERB = 1

    /** Delay effect type ID */
    const val DELAY = 2

    /** Vocoder effect type ID */
    const val VOCODER = 3

    /** Distortion effect type ID */
    const val DISTORTION = 4

    /** Decimator effect type ID (KORG NTS-3) */
    const val DECIMATOR = 10

    /** Deci-HPF effect type ID (KORG NTS-3) */
    const val DECI_HPF = 11

    /** AutoPan effect type ID (KORG NTS-3) */
    const val AUTO_PAN = 12

    /** ComplexTrem effect type ID (KORG NTS-3) */
    const val COMPLEX_TREM = 13

    /** RandomReso effect type ID (KORG NTS-3) */
    const val RANDOM_RESO = 14

    /** HPF-Delay effect type ID (KORG NTS-3) */
    const val HPF_DELAY = 15

    /** TapeEcho effect type ID (KORG NTS-3) */
    const val TAPE_ECHO = 16

    /** Hall Reverb effect type ID (KORG NTS-3) */
    const val HALL_REVERB = 17

    /** Riser Reverb effect type ID (KORG NTS-3) */
    const val RISER_REVERB = 18

    /** BeatGrain effect type ID (KORG NTS-3) */
    const val BEAT_GRAIN = 19

    /** Spring Reverb effect type ID (guitar) */
    const val SPRING_REVERB = 20

    /** Plate Reverb effect type ID (guitar) */
    const val PLATE_REVERB = 21

    /** Shimmer Reverb effect type ID (guitar) */
    const val SHIMMER_REVERB = 22

    // ===========================================
    // FILTER PARAMETERS
    // ===========================================

    object Filter {
        /** Filter cutoff frequency (20-20000 Hz) */
        const val FREQUENCY = 0

        /** Filter resonance (0.0-1.0) */
        const val RESONANCE = 1

        /** Filter type (LP, HP, BP) */
        const val TYPE = 2
    }

    // ===========================================
    // REVERB PARAMETERS
    // Professional Reverb with 12 parameters total
    // ===========================================

    object Reverb {
        // Basic parameters (backward compatible)
        /** Reverb decay time RT60 (0.1-5.0s) */
        const val DECAY = 0

        /** Reverb room size (0.5-2.0) */
        const val SIZE = 1

        /** Reverb wet/dry mix (0.0-1.0) */
        const val MIX = 2

        // Iteration 1: Pre-delay, Tone, Stereo
        /** Pre-delay before reverb (0-100ms) */
        const val PRE_DELAY = 3

        /** High frequency damping (0.0-1.0) */
        const val DAMPING = 4

        /** Early reflections density (0.0-1.0) */
        const val DIFFUSION = 5

        /** Stereo field width (0.0-1.0) */
        const val STEREO_WIDTH = 6

        /** Balance early vs late reverb (0.0-1.0) */
        const val EARLY_LATE_MIX = 7

        // Iteration 3: Modulation (shimmer effect)
        /** Modulation depth for shimmer (0.0-1.0) */
        const val MOD_DEPTH = 8

        /** Modulation rate for shimmer (0.1-5.0Hz) */
        const val MOD_RATE = 9

        /** HPF frequency (20-500Hz) */
        const val LOW_CUT = 10

        /** LPF frequency (1000-20000Hz) */
        const val HIGH_CUT = 11
    }

    // ===========================================
    // DELAY PARAMETERS
    // ===========================================

    object Delay {
        /** Delay time in ms (1-2000ms) or sync value */
        const val TIME = 0

        /** Delay feedback (0.0-0.9) */
        const val FEEDBACK = 1

        /** Delay wet/dry mix (0.0-1.0) */
        const val WET = 2

        /** BPM for sync mode (60-200) */
        const val BPM = 3

        /** Note division for sync (1-32) */
        const val NOTE_DIVISION = 4

        /** Sync on/off (0=off, 1=on) */
        const val SYNC = 5
    }

    // ===========================================
    // VOCODER PARAMETERS
    // (Also defined in EffectParameterIds for historical reasons)
    // ===========================================

    object Vocoder {
        /** Number of vocoder bands (4-32) */
        const val BAND_COUNT = 0

        /** Formant shift in semitones (-24 to +24) */
        const val FORMANT_SHIFT = 1

        /** Envelope attack (0.1-100 ms) */
        const val ATTACK = 2

        /** Envelope release (1-500 ms) */
        const val RELEASE = 3

        /** Wet/dry mix (0.0-1.0) */
        const val MIX = 4

        /** Internal carrier level (0.0-1.0) */
        const val CARRIER_LEVEL = 5

        /** Modulator source (0=internal/self, 1=external mic) */
        const val MOD_SOURCE = 6

        /** Carrier source (0=input signal, 1=internal oscillator) */
        const val CARRIER_SOURCE = 7

        /** Internal oscillator frequency (50-500 Hz) */
        const val CARRIER_FREQ = 8
    }

    // ===========================================
    // DISTORTION PARAMETERS
    // IDs match C++ DistortionEffect::Param enum
    // (Also defined in EffectParameterIds for historical reasons)
    // ===========================================

    object Distortion {
        // === Universal Parameters ===
        /** Input drive (0.0-1.0) */
        const val DRIVE = 0

        /** Tone control (0.0-1.0, dark to bright) */
        const val TONE = 1

        /** Output level (0.0-1.0) */
        const val LEVEL = 2

        /** Wet/dry mix (0.0-1.0) */
        const val MIX = 3

        /** Pedal type (see Pedals object) */
        const val ALGORITHM = 4

        // === Pedal-Specific Parameters (meaning varies by pedal) ===
        /** Pedal-specific param A */
        const val PARAM_A = 5

        /** Pedal-specific param B */
        const val PARAM_B = 6

        /** Pedal-specific param C */
        const val PARAM_C = 7

        // === Advanced Parameters ===
        /** Oversampling (0=1x, 1=2x, 2=4x) */
        const val OVERSAMPLE = 8

        /** Pre-distortion HPF (20-500 Hz) */
        const val PRE_LOW_CUT = 9

        /** Post-distortion LPF (1k-20k Hz) */
        const val POST_HIGH_CUT = 10

        /** Voltage sag simulation (0.0-1.0) */
        const val SAG = 11

        /** Transistor bias for fuzz (0.0-1.0) */
        const val BIAS = 12

        /** Noise gate threshold (0.0-1.0, 0=off) */
        const val GATE = 13
    }

    // ===========================================
    // DECIMATOR PARAMETERS (KORG NTS-3 FX-002)
    // ===========================================

    object Decimator {
        /** Bit depth (1-24 bits) */
        const val BIT_DEPTH = 0

        /** Target sample rate (100-48000 Hz, log scale) */
        const val SAMPLE_RATE = 1

        /** Wet/dry mix (0.0-1.0) */
        const val MIX = 2
    }

    // ===========================================
    // DECI-HPF PARAMETERS (KORG NTS-3 FX-005)
    // ===========================================

    object DeciHpf {
        /** Bit depth (1-24 bits) */
        const val BIT_DEPTH = 0

        /** HPF cutoff frequency (20-8000 Hz, log scale) */
        const val HPF_CUTOFF = 1

        /** Target sample rate (100-48000 Hz, log scale) */
        const val SAMPLE_RATE = 2

        /** Wet/dry mix (0.0-1.0) */
        const val MIX = 3
    }

    // ===========================================
    // AUTO PAN PARAMETERS (KORG NTS-3 FX-008)
    // ===========================================

    object AutoPan {
        /** LFO rate (0.1-20 Hz) */
        const val RATE = 0

        /** Pan depth (0.0-1.0) */
        const val DEPTH = 1

        /** Waveform (0=Sine, 1=Triangle, 2=Square) */
        const val WAVEFORM = 2

        /** Phase offset (0-360 degrees) */
        const val PHASE_OFFSET = 3

        /** Wet/dry mix (0.0-1.0) */
        const val MIX = 4
    }

    // ===========================================
    // COMPLEX TREM PARAMETERS (KORG NTS-3 FX-007)
    // ===========================================

    object ComplexTrem {
        /** LFO 1 rate (0.1-20 Hz) */
        const val RATE1 = 0

        /** LFO 2 rate (0.1-20 Hz) */
        const val RATE2 = 1

        /** Tremolo depth (0.0-1.0) */
        const val DEPTH = 2

        /** Waveform (0=Sine, 1=Triangle, 2=Square, 3=Sawtooth) */
        const val WAVEFORM = 3

        /** Stereo phase offset (0-180 degrees) */
        const val STEREO_PHASE = 4

        /** Wet/dry mix (0.0-1.0) */
        const val MIX = 5
    }

    // ===========================================
    // RANDOM RESO PARAMETERS (KORG NTS-3 FX-001)
    // ===========================================

    object RandomReso {
        /** Center frequency (80-12000 Hz, log scale) */
        const val CENTER_FREQ = 0

        /** Resonance / Q factor (0.5-30) */
        const val RESONANCE = 1

        /** LFO rate (0.1-20 Hz) */
        const val LFO_RATE = 2

        /** LFO depth (0-1, mapped to 0-4 octaves) */
        const val LFO_DEPTH = 3

        /** Wet/dry mix (0.0-1.0) */
        const val MIX = 4
    }

    // ===========================================
    // HPF-DELAY PARAMETERS (KORG NTS-3 FX-004)
    // ===========================================

    object HpfDelay {
        /** HPF cutoff frequency (20-8000 Hz, log scale) */
        const val HPF_CUTOFF = 0

        /** Delay time (10-2000 ms) */
        const val DELAY_TIME = 1

        /** Feedback (0-0.95) */
        const val FEEDBACK = 2

        /** Wet/dry mix (0.0-1.0) */
        const val MIX = 3

        const val SYNC = 4
        const val SUBDIVISION = 5
        const val PING_PONG = 6
        const val DUCKING = 7
        const val LPF_CUTOFF = 8
    }

    // ===========================================
    // TAPE ECHO PARAMETERS (KORG NTS-3 FX-006)
    // ===========================================

    object TapeEcho {
        /** Delay time in ms (50-2000) */
        const val DELAY_TIME = 0

        /** Feedback (0-0.95) */
        const val FEEDBACK = 1

        /** Wow/Flutter intensity (0-1) */
        const val WOW_FLUTTER = 2

        /** Tape age: controls LPF cutoff + hiss (0-1) */
        const val TAPE_AGE = 3

        /** Tape saturation in feedback (0-1) */
        const val SATURATION = 4

        /** Wet/dry mix (0-1) */
        const val MIX = 5

        const val SYNC = 6
        const val SUBDIVISION = 7
        const val PING_PONG = 8
        const val DUCKING = 9
        const val NOISE_LEVEL = 10
    }

    // ===========================================
    // HALL REVERB PARAMETERS (KORG NTS-3 FX-010)
    // ===========================================

    object HallReverb {
        /** Decay time in seconds (0.5-15) */
        const val DECAY_TIME = 0

        /** Room size (0.1-1.0) */
        const val SIZE = 1

        /** Pre-delay in ms (0-150) */
        const val PRE_DELAY = 2

        /** Diffusion / early-late balance (0-1) */
        const val DIFFUSION = 3

        /** High-frequency damping (0-1) */
        const val HF_DAMPING = 4

        /** Low-frequency damping (0-1) */
        const val LF_DAMPING = 5

        /** FDN modulation depth (0-1) */
        const val MODULATION = 6

        /** Wet/dry mix (0-1) */
        const val MIX = 7
    }

    // ===========================================
    // RISER REVERB PARAMETERS (KORG NTS-3 FX-009)
    // ===========================================

    object RiserReverb {
        /** Attack/rise time in ms (100-3000) */
        const val ATTACK_TIME = 0

        /** Decay time in seconds (0.5-10) */
        const val DECAY = 1

        /** Room size (0.1-1.0) */
        const val SIZE = 2

        /** Allpass diffusion (0-1) */
        const val DIFFUSION = 3

        /** HF damping (0-1) */
        const val DAMPING = 4

        /** Wet/dry mix (0-1) */
        const val MIX = 5
    }

    // ===========================================
    // BEAT GRAIN PARAMETERS (KORG NTS-3 FX-003)
    // ===========================================

    object BeatGrain {
        /** Grain size in ms (1-200) */
        const val GRAIN_SIZE = 0

        /** Density / rhythmic subdivision (0=1/4, 1=1/8, 2=1/16, 3=1/32) */
        const val DENSITY = 1

        /** Position spread randomization (0-1) */
        const val POSITION_SPREAD = 2

        /** Pitch shift in semitones (-12 to +12) */
        const val PITCH_SHIFT = 3

        /** Buffer length in seconds (0.5-4) */
        const val BUFFER_LENGTH = 4

        /** Wet/dry mix (0-1) */
        const val MIX = 5
    }

    object DelaySubdivision {
        const val QUARTER = 0
        const val EIGHTH = 1
        const val DOTTED_EIGHTH = 2
        const val EIGHTH_TRIPLET = 3
        const val SIXTEENTH = 4
        const val DOTTED_QUARTER = 5
    }

    object SpringReverb {
        const val DECAY = 0
        const val TONE = 1
        const val DRIP = 2
        const val TENSION = 3
        const val MIX = 4
    }

    object PlateReverb {
        const val DECAY = 0
        const val PRE_DELAY = 1
        const val DAMPING = 2
        const val MODULATION = 3
        const val LOW_CUT = 4
        const val HIGH_CUT = 5
        const val MIX = 6
    }

    object ShimmerReverb {
        const val DECAY = 0
        const val SIZE = 1
        const val PITCH_SEMITONES = 2
        const val SHIMMER_AMOUNT = 3
        const val FEEDBACK = 4
        const val TONE = 5
        const val MIX = 6
    }

    // ===========================================
    // PEDAL TYPES (for Distortion.ALGORITHM)
    // Match C++ DistortionVariants enum
    // (Also defined in EffectParameterIds for historical reasons)
    // ===========================================

    object Pedals {
        // Overdrive pedals (0-3)
        /** Ibanez TS-808/TS9 style */
        const val TUBE_SCREAMER = 0

        /** Boss OD-1/SD-1 style */
        const val BOSS_OVERDRIVE = 1

        /** Klon Centaur style */
        const val KLON = 2

        /** Fulltone OCD style */
        const val OCD = 3

        // Distortion pedals (4-7)
        /** Boss DS-1 style */
        const val BOSS_DS1 = 4

        /** ProCo RAT style */
        const val RAT = 5

        /** MXR Distortion+ style */
        const val DIST_PLUS = 6

        /** Boss MT-2 style */
        const val METAL_ZONE = 7

        // Fuzz pedals (8-11)
        /** EHX Big Muff Pi style */
        const val BIG_MUFF = 8

        /** Fuzz Face Germanium */
        const val FUZZ_FACE_GERM = 9

        /** Fuzz Face Silicon */
        const val FUZZ_FACE_SI = 10

        /** Octavia style */
        const val OCTAVE_FUZZ = 11

        // Special pedals (12-13)
        /** Boss HM-2 Swedish death metal */
        const val HM2_CHAINSAW = 12

        /** Sunn/Acapulco Gold style */
        const val DOOM_FUZZ = 13

        // Legacy algorithms (100+) - backward compatibility
        const val LEGACY_SOFT_CLIP = 100
        const val LEGACY_HARD_CLIP = 101
        const val LEGACY_TUBE_SIM = 102
        const val LEGACY_FOLDBACK = 103
        const val LEGACY_BITCRUSH = 104
    }

    // ===========================================
    // COMPATIBILITY ALIASES
    // For backward compatibility with feature-effects code
    // ===========================================

    // Filter parameter aliases
    const val PARAM_FREQUENCY = Filter.FREQUENCY
    const val PARAM_RESONANCE = Filter.RESONANCE

    // Reverb parameter aliases
    const val PARAM_DECAY = Reverb.DECAY
    const val PARAM_SIZE = Reverb.SIZE
    const val PARAM_MIX = Reverb.MIX
    const val PARAM_DAMPING = Reverb.DAMPING
    const val PARAM_DIFFUSION = Reverb.DIFFUSION

    // Delay parameter aliases
    const val PARAM_TIME = Delay.TIME
    const val PARAM_FEEDBACK = Delay.FEEDBACK
    const val PARAM_WET = Delay.WET
    const val PARAM_BPM = Delay.BPM
    const val PARAM_SYNC = Delay.SYNC

    // Vocoder parameter aliases
    const val PARAM_BANDS = Vocoder.BAND_COUNT
    const val PARAM_FORMANT_SHIFT = Vocoder.FORMANT_SHIFT

    // Distortion parameter aliases
    const val PARAM_DRIVE = Distortion.DRIVE
    const val PARAM_TONE = Distortion.TONE
    const val PARAM_LEVEL = Distortion.LEVEL
}
