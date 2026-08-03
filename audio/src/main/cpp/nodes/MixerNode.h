#pragma once

#include "../core/graph/AudioNode.h"
#include "../dsp/ParameterSmoother.h"
#include <array>
#include <atomic>
#include <cmath>

/**
 * @file MixerNode.h
 * @brief Audio mixing node with level control.
 *
 * Part of Stage 3: Mode System implementation.
 *
 * Supports up to MAX_INPUTS audio sources with:
 * - Per-input level control with smoothing
 * - Per-input panning (equal power)
 * - Per-input mute/solo
 * - Master output level
 *
 * @warning INPUT_OSCILLATOR is NOT the oscillator. The only caller of this node
 * is AudioEngine::handleMixMonitoring, which runs AFTER applyEffectsAndOutput,
 * so what it copies into input 0 is the finished master bus: synth + FX + LOOPS,
 * already scaled by master volume. Anything applied to input 0 scales the loops
 * too. This bit us once already: a "setMixerOscillatorLevel" existed on
 * AudioEngine and would have done exactly that had anyone exposed it.
 *
 * It also had an equal-power crossfade between inputs 0 and 1, removed because
 * it was unreachable in all five layers and nothing could ever enable it
 * (mCrossfadeEnabled defaulted to false and had no reachable writer). The
 * instrument-side level lives where it always applies instead — see
 * AudioEngine::setSynthVolume, applied next to the fade, upstream of the looper.
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

    // Master output
    std::atomic<float> mMasterLevel{1.0f};
    ParameterSmoother mMasterSmoother{0.99f};

    // Solo state cache
    std::atomic<bool> mAnySolo{false};

    // Pre-allocated temp buffer for mixing
    std::vector<float> mTempBuffer;
};
