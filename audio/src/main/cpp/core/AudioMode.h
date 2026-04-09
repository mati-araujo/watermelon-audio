#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

/**
 * @file AudioMode.h
 * @brief Defines audio modes, XY mapping targets, and related configurations.
 *
 * Part of Stage 3: Mode System implementation.
 */

namespace watermelon_audio {

/**
 * @brief Available audio modes that define signal routing.
 */
enum class AudioMode : uint8_t {
    CHAOS_PAD = 0,    ///< Oscillators only (original mode)
    INPUT_FX = 1,     ///< Input audio with effects only
    MIX = 2           ///< Mix of oscillators and input
};

/**
 * @brief XY controller mapping targets.
 *
 * Each target represents a parameter that can be controlled by the XY pad.
 */
enum class XYTarget : uint8_t {
    // Oscillator targets (0-9)
    FREQUENCY = 0,
    AMPLITUDE = 1,
    OSCILLATOR_TYPE = 2,

    // Input targets (10-19)
    INPUT_GAIN = 10,

    // Mixer targets (20-29)
    CROSSFADE = 20,
    OSCILLATOR_LEVEL = 21,
    INPUT_LEVEL = 22,

    // Generic effect targets (30-39)
    EFFECT_PARAM_1 = 30,
    EFFECT_PARAM_2 = 31,
    EFFECT_WET_DRY = 32,

    // Filter specific (40-49)
    FILTER_CUTOFF = 40,
    FILTER_RESONANCE = 41,

    // Delay specific (50-59)
    DELAY_TIME = 50,
    DELAY_FEEDBACK = 51,

    // Reverb specific (60-69)
    REVERB_SIZE = 60,
    REVERB_DAMPING = 61,

    // Special
    NONE = 255
};

/**
 * @brief Curve types for XY value mapping.
 */
enum class CurveType : uint8_t {
    LINEAR = 0,
    LOGARITHMIC = 1,
    EXPONENTIAL = 2,
    S_CURVE = 3
};

/**
 * @brief Value range with mapping utilities.
 */
struct Range {
    float min = 0.0f;
    float max = 1.0f;

    /**
     * @brief Linear mapping from normalized [0,1] to range.
     */
    [[nodiscard]] constexpr float map(float normalizedValue) const {
        return min + normalizedValue * (max - min);
    }

    /**
     * @brief Logarithmic mapping (ideal for frequencies).
     */
    [[nodiscard]] float mapLog(float normalizedValue) const {
        const float safeMin = std::max(min, 0.001f);
        const float safeMax = std::max(max, 0.001f);
        const float logMin = std::log10(safeMin);
        const float logMax = std::log10(safeMax);
        const float logValue = logMin + normalizedValue * (logMax - logMin);
        return std::pow(10.0f, logValue);
    }

    /**
     * @brief Normalize value to [0,1] range.
     */
    [[nodiscard]] constexpr float normalize(float value) const {
        const float range = max - min;
        return range > 0.0f ? (value - min) / range : 0.0f;
    }
};

/**
 * @brief Configuration for XY controller axis mapping.
 */
struct XYAxisConfig {
    XYTarget target = XYTarget::NONE;
    Range range = {0.0f, 1.0f};
    CurveType curve = CurveType::LINEAR;
    bool inverted = false;
};

/**
 * @brief Complete XY mapping configuration.
 */
struct XYMappingConfig {
    XYAxisConfig xAxis = {
        .target = XYTarget::FREQUENCY,
        .range = {50.0f, 2000.0f},
        .curve = CurveType::LOGARITHMIC,
        .inverted = false
    };

    XYAxisConfig yAxis = {
        .target = XYTarget::AMPLITUDE,
        .range = {0.0f, 1.0f},
        .curve = CurveType::LINEAR,
        .inverted = false
    };
};

/**
 * @brief Complete mode configuration.
 */
struct ModeConfiguration {
    AudioMode mode = AudioMode::CHAOS_PAD;

    // Signal routing
    bool oscillatorActive = true;
    bool inputActive = false;
    float oscillatorLevel = 1.0f;
    float inputLevel = 0.0f;

    // XY mapping
    XYMappingConfig xyMapping;

    // Transition timing
    float transitionTimeMs = 100.0f;
};

/**
 * @brief Helper functions for mode system.
 */
namespace ModeUtils {

/**
 * @brief Get human-readable name for audio mode.
 */
inline const char* getModeName(AudioMode mode) {
    switch (mode) {
        case AudioMode::CHAOS_PAD: return "ChaosPad";
        case AudioMode::INPUT_FX: return "Input FX";
        case AudioMode::MIX: return "Mix";
        default: return "Unknown";
    }
}

/**
 * @brief Get human-readable name for XY target.
 */
inline const char* getTargetName(XYTarget target) {
    switch (target) {
        case XYTarget::FREQUENCY: return "Frequency";
        case XYTarget::AMPLITUDE: return "Amplitude";
        case XYTarget::INPUT_GAIN: return "Input Gain";
        case XYTarget::CROSSFADE: return "Crossfade";
        case XYTarget::FILTER_CUTOFF: return "Filter Cutoff";
        case XYTarget::FILTER_RESONANCE: return "Filter Resonance";
        case XYTarget::EFFECT_WET_DRY: return "Wet/Dry";
        case XYTarget::NONE: return "None";
        default: return "Parameter";
    }
}

/**
 * @brief Check if mode requires audio input.
 */
inline bool requiresInput(AudioMode mode) {
    return mode == AudioMode::INPUT_FX || mode == AudioMode::MIX;
}

/**
 * @brief Check if mode uses oscillator.
 */
inline bool usesOscillator(AudioMode mode) {
    return mode == AudioMode::CHAOS_PAD || mode == AudioMode::MIX;
}

} // namespace ModeUtils

} // namespace watermelon_audio