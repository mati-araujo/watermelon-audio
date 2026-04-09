#pragma once

#include "AudioMode.h"

/**
 * @file ModeConfigurations.h
 * @brief Default configurations for each audio mode.
 *
 * Part of Stage 3: Mode System implementation.
 */

namespace watermelon_audio {

/**
 * @brief Factory class for mode configurations.
 *
 * Provides default configurations for each audio mode with
 * appropriate XY mappings and signal routing.
 */
class ModeConfigurations {
public:
    /**
     * @brief Get configuration for ChaosPad mode.
     *
     * Original oscillator-only mode with XY mapped to frequency/amplitude.
     */
    static ModeConfiguration getChaosPad() {
        ModeConfiguration config;
        config.mode = AudioMode::CHAOS_PAD;

        // Signal routing: oscillator only
        config.oscillatorActive = true;
        config.inputActive = false;
        config.oscillatorLevel = 1.0f;
        config.inputLevel = 0.0f;

        // XY mapping: X=frequency (log), Y=amplitude (linear)
        config.xyMapping.xAxis = {
            .target = XYTarget::FREQUENCY,
            .range = {50.0f, 2000.0f},
            .curve = CurveType::LOGARITHMIC,
            .inverted = false
        };
        config.xyMapping.yAxis = {
            .target = XYTarget::AMPLITUDE,
            .range = {0.0f, 1.0f},
            .curve = CurveType::LINEAR,
            .inverted = false
        };

        config.transitionTimeMs = 100.0f;
        return config;
    }

    /**
     * @brief Get configuration for Input FX mode.
     *
     * Audio input with effects, XY controls filter and wet/dry.
     */
    static ModeConfiguration getInputFX() {
        ModeConfiguration config;
        config.mode = AudioMode::INPUT_FX;

        // Signal routing: input only
        config.oscillatorActive = false;
        config.inputActive = true;
        config.oscillatorLevel = 0.0f;
        config.inputLevel = 1.0f;

        // XY mapping: X=filter cutoff, Y=wet/dry
        config.xyMapping.xAxis = {
            .target = XYTarget::FILTER_CUTOFF,
            .range = {100.0f, 10000.0f},
            .curve = CurveType::LOGARITHMIC,
            .inverted = false
        };
        config.xyMapping.yAxis = {
            .target = XYTarget::EFFECT_WET_DRY,
            .range = {0.0f, 1.0f},
            .curve = CurveType::LINEAR,
            .inverted = false
        };

        config.transitionTimeMs = 100.0f;
        return config;
    }

    /**
     * @brief Get configuration for Mix mode.
     *
     * Both oscillator and input with crossfade control.
     */
    static ModeConfiguration getMix() {
        ModeConfiguration config;
        config.mode = AudioMode::MIX;

        // Signal routing: both sources
        config.oscillatorActive = true;
        config.inputActive = true;
        config.oscillatorLevel = 0.5f;
        config.inputLevel = 0.5f;

        // XY mapping: X=crossfade, Y=frequency
        config.xyMapping.xAxis = {
            .target = XYTarget::CROSSFADE,
            .range = {0.0f, 1.0f},
            .curve = CurveType::LINEAR,
            .inverted = false
        };
        config.xyMapping.yAxis = {
            .target = XYTarget::FREQUENCY,
            .range = {50.0f, 2000.0f},
            .curve = CurveType::LOGARITHMIC,
            .inverted = false
        };

        config.transitionTimeMs = 150.0f;
        return config;
    }

    /**
     * @brief Get default configuration for specified mode.
     */
    static ModeConfiguration getConfiguration(AudioMode mode) {
        switch (mode) {
            case AudioMode::CHAOS_PAD:
                return getChaosPad();
            case AudioMode::INPUT_FX:
                return getInputFX();
            case AudioMode::MIX:
                return getMix();
            default:
                return getChaosPad();
        }
    }

    /**
     * @brief Create custom configuration based on existing mode.
     *
     * @param baseMode The base mode to derive configuration from.
     * @param xTarget Custom X axis target.
     * @param yTarget Custom Y axis target.
     * @return Customized configuration.
     */
    static ModeConfiguration createCustom(
            AudioMode baseMode,
            XYTarget xTarget,
            XYTarget yTarget) {

        ModeConfiguration config = getConfiguration(baseMode);

        // Update targets while preserving appropriate ranges
        config.xyMapping.xAxis.target = xTarget;
        config.xyMapping.yAxis.target = yTarget;

        // Set appropriate ranges based on targets
        config.xyMapping.xAxis.range = getRangeForTarget(xTarget);
        config.xyMapping.yAxis.range = getRangeForTarget(yTarget);

        // Set appropriate curves
        config.xyMapping.xAxis.curve = getCurveForTarget(xTarget);
        config.xyMapping.yAxis.curve = getCurveForTarget(yTarget);

        return config;
    }

private:
    /**
     * @brief Get appropriate range for a target parameter.
     */
    static Range getRangeForTarget(XYTarget target) {
        switch (target) {
            case XYTarget::FREQUENCY:
                return {50.0f, 2000.0f};

            case XYTarget::AMPLITUDE:
            case XYTarget::CROSSFADE:
            case XYTarget::OSCILLATOR_LEVEL:
            case XYTarget::INPUT_LEVEL:
            case XYTarget::EFFECT_WET_DRY:
                return {0.0f, 1.0f};

            case XYTarget::INPUT_GAIN:
                return {-20.0f, 20.0f};

            case XYTarget::FILTER_CUTOFF:
                return {100.0f, 10000.0f};

            case XYTarget::FILTER_RESONANCE:
                return {0.1f, 10.0f};

            case XYTarget::DELAY_TIME:
                return {10.0f, 1000.0f};

            case XYTarget::DELAY_FEEDBACK:
                return {0.0f, 0.9f};

            case XYTarget::REVERB_SIZE:
                return {0.1f, 2.0f};

            case XYTarget::REVERB_DAMPING:
                return {0.0f, 1.0f};

            default:
                return {0.0f, 1.0f};
        }
    }

    /**
     * @brief Get appropriate curve type for a target parameter.
     */
    static CurveType getCurveForTarget(XYTarget target) {
        switch (target) {
            case XYTarget::FREQUENCY:
            case XYTarget::FILTER_CUTOFF:
            case XYTarget::DELAY_TIME:
                return CurveType::LOGARITHMIC;

            case XYTarget::AMPLITUDE:
                return CurveType::EXPONENTIAL;

            default:
                return CurveType::LINEAR;
        }
    }
};

} // namespace watermelon_audio
