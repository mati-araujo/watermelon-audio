#pragma once

#include "../core/graph/AudioNode.h"
#include "../dsp/ParameterSmoother.h"
#include <array>
#include <atomic>
#include <cmath>

/**
 * @file MixerNode.h
 * @brief Audio mixing node with level control and crossfade.
 *
 * Part of Stage 3: Mode System implementation.
 *
 * Supports up to MAX_INPUTS audio sources with:
 * - Per-input level control with smoothing
 * - Per-input panning (equal power)
 * - Per-input mute/solo
 * - Global crossfade between inputs 0 and 1
 * - Master output level
 *
 * Thread Safety:
 * - All parameters use std::atomic for lock-free updates
 * - process() is RT-safe (no allocations, no locks)
 */
class MixerNode : public AudioNode {
public:
    static constexpr int MAX_INPUTS = 4;

    // Standard input indices
    static constexpr int INPUT_OSCILLATOR = 0;
    static constexpr int INPUT_EXTERNAL = 1;
    static constexpr int INPUT_AUX_1 = 2;
    static constexpr int INPUT_AUX_2 = 3;

    MixerNode();
    ~MixerNode() override = default;

    // AudioNode interface
    NodeType getType() const override { return NodeType::MIXER; }
    const char* getName() const override { return "Mixer"; }
    void prepare(int sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(AudioBuffer& inputBuffer, int numFrames) override;

    /**
     * @brief Set input buffer reference for a specific input.
     * @param inputIndex Input slot (0 to MAX_INPUTS-1)
     * @param buffer Pointer to the source node's output buffer (can be nullptr)
     *
     * Call this when connecting/disconnecting nodes to the mixer.
     * The buffer pointer should remain valid during audio processing.
     */
    void setInputBuffer(int inputIndex, const AudioBuffer* buffer);

    /**
     * @brief Get the input buffer for a specific input.
     */
    const AudioBuffer* getInputBuffer(int inputIndex) const;

    // Per-input level control (0.0 to 2.0, default 1.0)
    void setInputLevel(int inputIndex, float level);
    float getInputLevel(int inputIndex) const;

    // Per-input panning (-1.0 = full left, 0.0 = center, 1.0 = full right)
    void setInputPan(int inputIndex, float pan);
    float getInputPan(int inputIndex) const;

    // Per-input mute
    void setInputMute(int inputIndex, bool mute);
    bool isInputMuted(int inputIndex) const;

    // Per-input solo (when any input is soloed, only soloed inputs are heard)
    void setInputSolo(int inputIndex, bool solo);
    bool isInputSoloed(int inputIndex) const;

    /**
     * @brief Set crossfade position between input 0 and input 1.
     * @param position 0.0 = full input 0, 1.0 = full input 1
     *
     * Uses equal-power crossfade curve for smooth transitions.
     */
    void setCrossfade(float position);
    float getCrossfade() const;

    /**
     * @brief Enable/disable crossfade mode.
     *
     * When enabled, crossfade position affects inputs 0 and 1.
     * When disabled, both inputs are mixed at their individual levels.
     */
    void setCrossfadeEnabled(bool enabled);
    bool isCrossfadeEnabled() const;

    // Master output level (0.0 to 2.0, default 1.0)
    void setMasterLevel(float level);
    float getMasterLevel() const;

    /**
     * @brief Get number of active (non-null) input buffers.
     */
    int getActiveInputCount() const;

private:
    void updateSoloState();
    float calculatePanGainLeft(float pan) const;
    float calculatePanGainRight(float pan) const;
    bool isInputValid(int inputIndex) const;

    /**
     * @brief Per-input channel state.
     */
    struct InputChannel {
        std::atomic<float> level{1.0f};
        std::atomic<float> pan{0.0f};
        std::atomic<bool> mute{false};
        std::atomic<bool> solo{false};
        ParameterSmoother levelSmoother{0.99f};
        ParameterSmoother panSmoother{0.99f};
        const AudioBuffer* inputBuffer{nullptr};

        InputChannel() = default;
    };

    std::array<InputChannel, MAX_INPUTS> mInputChannels;

    // Crossfade control
    std::atomic<float> mCrossfade{0.5f};
    std::atomic<bool> mCrossfadeEnabled{false};
    ParameterSmoother mCrossfadeSmoother{0.99f};

    // Master output
    std::atomic<float> mMasterLevel{1.0f};
    ParameterSmoother mMasterSmoother{0.99f};

    // Solo state cache
    std::atomic<bool> mAnySolo{false};

    // Pre-allocated temp buffer for mixing
    std::vector<float> mTempBuffer;
};
