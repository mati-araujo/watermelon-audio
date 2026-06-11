#ifndef EFFECTTYPES_H
#define EFFECTTYPES_H

#include <cstddef>  // for size_t

/**
 * @file EffectTypes.h
 * @brief Effect type system with categories and variants for extensibility
 *
 * This header provides:
 * - EffectCategory: High-level classification of effects
 * - EffectType: Unique identifiers for each effect type
 * - Variant namespaces: Sub-types for effects with multiple algorithms
 *
 * Design Goals:
 * - Backward compatible (existing FILTER=0, REVERB=1, DELAY=2)
 * - Extensible for future effects (4-8 effect chains)
 * - Support for effect variants (e.g., "Distortion Boss", "Distortion X")
 */

/**
 * @enum EffectCategory
 * @brief High-level categorization of audio effects
 *
 * Used for UI organization and effect routing decisions.
 */
enum class EffectCategory {
    DYNAMICS,       ///< Compressors, limiters, gates, expanders
    DISTORTION,     ///< Distortion, overdrive, fuzz, saturation
    FILTER,         ///< EQ, filters, wah
    MODULATION,     ///< Chorus, flanger, phaser, tremolo
    DELAY,          ///< Delay, echo, multitap
    REVERB,         ///< Reverb, ambience, room simulation
    SPECTRAL,       ///< Vocoder, pitch shift, harmonizer, FFT-based
    UTILITY         ///< Gain, pan, meters, analyzers
};

/**
 * @enum EffectType
 * @brief Unique identifiers for effect types
 *
 * IMPORTANT: Maintain backward compatibility!
 * - FILTER, REVERB, DELAY IDs must remain 0, 1, 2
 * - New effects start from ID 3
 */
enum EffectType {
    // Legacy types (backward compatible)
    FILTER = 0,         ///< IIR Biquad filter (LPF, HPF, BPF)
    REVERB = 1,         ///< Algorithmic reverb (Freeverb-based)
    DELAY = 2,          ///< Delay with feedback and BPM sync

    // New types (Stage 2-3 implementation)
    VOCODER = 3,        ///< Spectral vocoder (mic modulator, synth carrier)
    DISTORTION = 4,     ///< Multi-algorithm distortion with oversampling

    // Phase 3: Core Guitar Effects
    COMPRESSOR = 5,     ///< Dynamics compressor with threshold, ratio, attack/release
    CHORUS = 6,         ///< Multi-voice chorus with LFO modulation
    PHASER = 7,         ///< All-pass phaser with variable stages

    // Phase 4: Amp & Cabinet Modeling
    AMP_SIM = 8,        ///< Tube amplifier simulator (preamp, tonestack, poweramp)
    CABINET = 9,        ///< Cabinet impulse response convolution (FFT-based)

    // KORG NTS-3 Effects (IDs 10-19)
    DECIMATOR = 10,     ///< Bit crusher + sample rate reducer (MOD)
    DECI_HPF = 11,      ///< Decimator with resonant HPF (MOD)
    AUTO_PAN = 12,      ///< LFO-driven stereo auto-panner (MOD)
    COMPLEX_TREM = 13,  ///< Multi-waveform complex tremolo (MOD)
    RANDOM_RESO = 14,   ///< Random resonant filter sweeps (MOD)
    HPF_DELAY = 15,     ///< High-pass filtered delay (DELAY)
    TAPE_ECHO = 16,     ///< Analog tape echo emulation (DELAY)
    HALL_REVERB = 17,   ///< Lush stereo hall reverb (REVERB)
    RISER_REVERB = 18,  ///< Shimmer/pitch-shifting reverb (REVERB)
    BEAT_GRAIN = 19,    ///< Beat-synced granular delay (DELAY)
    SPRING_REVERB = 20, ///< Guitar spring tank reverb (REVERB)
    PLATE_REVERB = 21,  ///< Guitar plate reverb (REVERB)
    SHIMMER_REVERB = 22,///< Pitch-shifted shimmer reverb (REVERB)

    EFFECT_TYPE_COUNT   ///< Number of effect types (for array sizing)
};

/**
 * @brief Get the category for an effect type
 * @param type Effect type
 * @return Corresponding category
 */
inline EffectCategory getEffectCategory(EffectType type) {
    switch (type) {
        case FILTER:
            return EffectCategory::FILTER;
        case REVERB:
            return EffectCategory::REVERB;
        case DELAY:
            return EffectCategory::DELAY;
        case VOCODER:
            return EffectCategory::SPECTRAL;
        case DISTORTION:
            return EffectCategory::DISTORTION;
        case COMPRESSOR:
            return EffectCategory::DYNAMICS;
        case CHORUS:
        case PHASER:
            return EffectCategory::MODULATION;
        case AMP_SIM:
            return EffectCategory::DISTORTION;  // Amp is categorized as distortion/drive
        case CABINET:
            return EffectCategory::FILTER;      // Cabinet IR is essentially filtering
        // KORG NTS-3 Effects
        case DECIMATOR:
        case DECI_HPF:
        case AUTO_PAN:
        case COMPLEX_TREM:
        case RANDOM_RESO:
            return EffectCategory::MODULATION;
        case HPF_DELAY:
        case TAPE_ECHO:
        case BEAT_GRAIN:
            return EffectCategory::DELAY;
        case HALL_REVERB:
        case RISER_REVERB:
        case SPRING_REVERB:
        case PLATE_REVERB:
        case SHIMMER_REVERB:
            return EffectCategory::REVERB;
        default:
            return EffectCategory::UTILITY;
    }
}

/**
 * @brief Get display name for an effect type
 * @param type Effect type
 * @return Human-readable name
 */
inline const char* getEffectTypeName(EffectType type) {
    switch (type) {
        case FILTER:     return "Filter";
        case REVERB:     return "Reverb";
        case DELAY:      return "Delay";
        case VOCODER:    return "Vocoder";
        case DISTORTION: return "Distortion";
        case COMPRESSOR: return "Compressor";
        case CHORUS:     return "Chorus";
        case PHASER:     return "Phaser";
        case AMP_SIM:    return "Amp Simulator";
        case CABINET:    return "Cabinet";
        // KORG NTS-3 Effects
        case DECIMATOR:     return "Decimator";
        case DECI_HPF:      return "Deci-HPF";
        case AUTO_PAN:      return "Auto Pan";
        case COMPLEX_TREM:  return "Complex Trem";
        case RANDOM_RESO:   return "Random Reso";
        case HPF_DELAY:     return "HPF Delay";
        case TAPE_ECHO:     return "Tape Echo";
        case HALL_REVERB:   return "Hall Reverb";
        case RISER_REVERB:  return "Riser Reverb";
        case BEAT_GRAIN:    return "Beat Grain";
        case SPRING_REVERB: return "Spring Reverb";
        case PLATE_REVERB:  return "Plate Reverb";
        case SHIMMER_REVERB:return "Shimmer Reverb";
        default:            return "Unknown";
    }
}

// ============================================================================
// VARIANT NAMESPACES
// ============================================================================
// These define sub-types/algorithms within each effect type.
// Variants allow for "Distortion Boss", "Distortion X" style presets
// while using the same underlying DistortionEffect class.

/**
 * @namespace DistortionVariants
 * @brief Algorithm variants for DistortionEffect
 *
 * Each variant represents a different waveshaping algorithm emulating
 * professional distortion pedals.
 * Selected via setParam(ALGORITHM, variant_id)
 */
namespace DistortionVariants {
    // ========== OVERDRIVE PEDALS ==========
    constexpr int TUBE_SCREAMER = 0;    ///< Ibanez TS-808/TS9 style - mid-hump, warm
    constexpr int BOSS_OVERDRIVE = 1;   ///< Boss OD-1/SD-1 style - bright, articulate
    constexpr int KLON = 2;             ///< Klon Centaur style - transparent, dynamic
    constexpr int OCD = 3;              ///< Fulltone OCD style - amp-like, versatile

    // ========== DISTORTION PEDALS ==========
    constexpr int BOSS_DS1 = 4;         ///< Boss DS-1 style - aggressive, cutting
    constexpr int RAT = 5;              ///< ProCo RAT style - gritty, saturated
    constexpr int DIST_PLUS = 6;        ///< MXR Distortion+ style - classic, mid-focused
    constexpr int METAL_ZONE = 7;       ///< Boss MT-2 style - high-gain, scooped

    // ========== FUZZ PEDALS ==========
    constexpr int BIG_MUFF = 8;         ///< EHX Big Muff Pi style - sustained, creamy
    constexpr int FUZZ_FACE_GERM = 9;   ///< Fuzz Face Germanium - warm, organic
    constexpr int FUZZ_FACE_SI = 10;    ///< Fuzz Face Silicon - bright, aggressive
    constexpr int OCTAVE_FUZZ = 11;     ///< Octavia style - ring-mod octave-up

    // ========== SPECIAL/EXTREME ==========
    constexpr int HM2_CHAINSAW = 12;    ///< Boss HM-2 style - Swedish death metal
    constexpr int DOOM_FUZZ = 13;       ///< Sunn/Acapulco style - massive doom fuzz

    // ========== LEGACY (backward compatibility) ==========
    constexpr int LEGACY_SOFT_CLIP = 100;   ///< Original soft clip algorithm
    constexpr int LEGACY_HARD_CLIP = 101;   ///< Original hard clip algorithm
    constexpr int LEGACY_TUBE_SIM = 102;    ///< Original tube sim algorithm
    constexpr int LEGACY_FOLDBACK = 103;    ///< Original foldback algorithm
    constexpr int LEGACY_BITCRUSH = 104;    ///< Original bitcrush algorithm

    constexpr int COUNT = 14;           ///< Number of pedal variants (excluding legacy)
    constexpr int TOTAL_COUNT = 19;     ///< Total including legacy

    /**
     * @brief Get pedal category
     */
    enum class PedalCategory {
        OVERDRIVE,
        DISTORTION,
        FUZZ,
        SPECIAL,
        LEGACY
    };

    inline PedalCategory getCategory(int variant) {
        if (variant >= 0 && variant <= 3) return PedalCategory::OVERDRIVE;
        if (variant >= 4 && variant <= 7) return PedalCategory::DISTORTION;
        if (variant >= 8 && variant <= 11) return PedalCategory::FUZZ;
        if (variant >= 12 && variant <= 13) return PedalCategory::SPECIAL;
        return PedalCategory::LEGACY;
    }

    inline const char* getName(int variant) {
        switch (variant) {
            // Overdrive
            case TUBE_SCREAMER:     return "Tube Screamer";
            case BOSS_OVERDRIVE:    return "Boss Overdrive";
            case KLON:              return "Klon";
            case OCD:               return "OCD";
            // Distortion
            case BOSS_DS1:          return "Boss DS-1";
            case RAT:               return "RAT";
            case DIST_PLUS:         return "Distortion+";
            case METAL_ZONE:        return "Metal Zone";
            // Fuzz
            case BIG_MUFF:          return "Big Muff";
            case FUZZ_FACE_GERM:    return "Fuzz Face Ge";
            case FUZZ_FACE_SI:      return "Fuzz Face Si";
            case OCTAVE_FUZZ:       return "Octave Fuzz";
            // Special
            case HM2_CHAINSAW:      return "HM-2 Chainsaw";
            case DOOM_FUZZ:         return "Doom Fuzz";
            // Legacy
            case LEGACY_SOFT_CLIP:  return "Soft Clip (Legacy)";
            case LEGACY_HARD_CLIP:  return "Hard Clip (Legacy)";
            case LEGACY_TUBE_SIM:   return "Tube Sim (Legacy)";
            case LEGACY_FOLDBACK:   return "Foldback (Legacy)";
            case LEGACY_BITCRUSH:   return "Bitcrush (Legacy)";
            default:                return "Unknown";
        }
    }

    inline const char* getDescription(int variant) {
        switch (variant) {
            case TUBE_SCREAMER:     return "Mid-hump overdrive, warm and smooth";
            case BOSS_OVERDRIVE:    return "Bright, articulate overdrive";
            case KLON:              return "Transparent, touch-sensitive overdrive";
            case OCD:               return "Amp-like crunch, dynamic response";
            case BOSS_DS1:          return "Aggressive, cutting distortion";
            case RAT:               return "Gritty, saturated distortion with filter";
            case DIST_PLUS:         return "Classic mid-focused distortion";
            case METAL_ZONE:        return "High-gain with parametric EQ";
            case BIG_MUFF:          return "Massive sustained fuzz, creamy";
            case FUZZ_FACE_GERM:    return "Warm organic fuzz, cleans up with volume";
            case FUZZ_FACE_SI:      return "Bright aggressive fuzz, tight";
            case OCTAVE_FUZZ:       return "Ring-mod style octave-up fuzz";
            case HM2_CHAINSAW:      return "Swedish death metal chainsaw tone";
            case DOOM_FUZZ:         return "Crushing doom fuzz, endless sustain";
            default:                return "Legacy distortion algorithm";
        }
    }
}

/**
 * @namespace VocoderVariants
 * @brief Algorithm variants for VocoderEffect
 *
 * Different vocoder modes optimized for various use cases.
 */
namespace VocoderVariants {
    constexpr int CLASSIC = 0;      ///< Traditional vocoder (16+ bands)
    constexpr int ROBOTIC = 1;      ///< Robot voice (fewer bands, fast envelope)
    constexpr int FORMANT = 2;      ///< Formant-preserving mode

    constexpr int COUNT = 3;        ///< Number of variants

    inline const char* getName(int variant) {
        switch (variant) {
            case CLASSIC: return "Classic";
            case ROBOTIC: return "Robotic";
            case FORMANT: return "Formant";
            default:      return "Unknown";
        }
    }
}

// ============================================================================
// ROUTING MODES
// ============================================================================

/**
 * @enum RoutingMode
 * @brief Topology modes for the effect chain
 *
 * Determines how effects are connected:
 * - SERIAL: A → B → C → D (default)
 * - PARALLEL: (A + B + C + D) / N
 * - SPLIT_2X2: (A→B) + (C→D)
 * - SERIAL_PARALLEL: A → B → (C + D)
 * - PARALLEL_SERIAL: (A + B) → C → D
 * - FEEDBACK: A → B → Out, Out → C → feedback → A
 */
enum class RoutingMode : int {
    SERIAL = 0,
    PARALLEL = 1,
    SPLIT_2X2 = 2,
    SERIAL_PARALLEL = 3,
    PARALLEL_SERIAL = 4,
    FEEDBACK = 5
};

// ============================================================================
// XY MAPPING TYPES
// ============================================================================

/**
 * @enum MappingCurveType
 * @brief Curve types for XY mapping transformation
 * IDs match Kotlin MappingCurveType in MappingModels.kt
 */
enum class MappingCurveType : int {
    LINEAR = 0,
    EXPONENTIAL = 1,
    LOGARITHMIC = 2,
    TOGGLE = 3
};

/**
 * @enum MappingPolarity
 * @brief Polarity modes for XY mapping
 * IDs match Kotlin PolarityType in MappingModels.kt
 */
enum class MappingPolarity : int {
    UNIPOLAR = 0,
    BIPOLAR = 1
};

// ============================================================================
// EFFECT CHAIN LIMITS
// ============================================================================

/**
 * @brief Maximum number of effects in the current chain
 *
 * Current implementation supports 8 effects for guitar mode chains
 */
constexpr size_t MAX_EFFECTS = 12;

/**
 * @brief Maximum chain size for future dynamic chains
 *
 * Architecture is prepared for 4-8 effects in future versions.
 */
constexpr size_t MAX_CHAIN_SIZE = 10;

#endif // EFFECTTYPES_H
