#pragma once

#include "AudioMode.h"
#include "ModeConfigurations.h"
#include "XYMapper.h"
#include "../dsp/ParameterSmoother.h"
#include <atomic>
#include <mutex>
#include <functional>

// Forward declarations (global namespace - these are NOT in noisypad namespace)
class OscillatorNode;
class InputNode;
class MixerNode;
class EffectChainNode;
class OutputNode;

/**
 * @file ModeManager.h
 * @brief Central mode management system.
 *
 * Part of Stage 3: Mode System implementation.
 *
 * Manages audio mode transitions and coordinates:
 * - Signal routing configuration
 * - XY mapper configuration
 * - Smooth level transitions
 * - Node activation/deactivation
 */

namespace noisypad {

// Forward declarations for noisypad namespace
class AudioGraph;

/**
 * @brief Callback for mode change notifications.
 */
using ModeChangeCallback = std::function<void(AudioMode oldMode, AudioMode newMode)>;

/**
 * @brief RT-safe snapshot of mode configuration for audio thread.
 *
 * This structure is used with double-buffering to allow lock-free
 * reads from the audio thread while the UI thread can safely update
 * the inactive snapshot.
 */
struct ModeSnapshot {
    ModeConfiguration current;
    ModeConfiguration target;
    float transitionIncrement = 0.0f;
    bool inTransition = false;
};

/**
 * @class ModeManager
 * @brief Manages audio modes and transitions.
 *
 * Thread Safety:
 * - Mode changes initiated from UI thread
 * - updateTransition() called from audio thread
 * - All state uses atomic operations
 */
class ModeManager {
public:
    ModeManager();
    ~ModeManager() = default;

    // Non-copyable
    ModeManager(const ModeManager&) = delete;
    ModeManager& operator=(const ModeManager&) = delete;

    /**
     * @brief Set reference to audio graph (optional).
     */
    void setAudioGraph(AudioGraph* graph) { mGraph = graph; }

    /**
     * @brief Set reference to XY mapper.
     */
    void setXYMapper(XYMapper* mapper) { mXYMapper = mapper; }

    /**
     * @brief Set references to all audio nodes.
     */
    void setNodes(OscillatorNode* osc, InputNode* input,
                  MixerNode* mixer, EffectChainNode* effects,
                  OutputNode* output);

    /**
     * @brief Prepare the mode manager for audio processing.
     * @param sampleRate Current sample rate in Hz
     */
    void prepare(int sampleRate);

    /**
     * @brief Reset mode manager state.
     */
    void reset();

    // ========== Mode Control ==========

    /**
     * @brief Set mode with default transition time.
     * @param mode Target audio mode
     */
    void setMode(AudioMode mode);

    /**
     * @brief Set mode with custom transition time.
     * @param mode Target audio mode
     * @param transitionTimeMs Transition duration in milliseconds
     */
    void setModeWithTransition(AudioMode mode, float transitionTimeMs);

    /**
     * @brief Get current active mode.
     */
    AudioMode getCurrentMode() const {
        return mCurrentMode.load(std::memory_order_acquire);
    }

    /**
     * @brief Get target mode (during transition).
     */
    AudioMode getTargetMode() const {
        return mTargetMode.load(std::memory_order_acquire);
    }

    /**
     * @brief Check if mode transition is in progress.
     */
    bool isInTransition() const {
        return mInTransition.load(std::memory_order_acquire);
    }

    /**
     * @brief Get transition progress (0.0 to 1.0).
     */
    float getTransitionProgress() const {
        return mTransitionProgress.load(std::memory_order_acquire);
    }

    // ========== Configuration ==========

    /**
     * @brief Apply custom mode configuration.
     * @param config Custom configuration to apply
     */
    void setCustomConfiguration(const ModeConfiguration& config);

    /**
     * @brief Get current mode configuration.
     */
    ModeConfiguration getCurrentConfiguration() const;

    // ========== Audio Thread Interface ==========

    /**
     * @brief Update transition state (call from audio thread).
     * @param numSamples Number of samples in current block
     *
     * This updates level interpolation during mode transitions.
     * Must be called every audio block for smooth transitions.
     */
    void updateTransition(int numSamples);

    // ========== Callbacks ==========

    /**
     * @brief Set callback for mode change events.
     */
    void setModeChangeCallback(ModeChangeCallback callback);

    // ========== Level Queries ==========

    /**
     * @brief Get current oscillator level (after smoothing).
     */
    float getCurrentOscillatorLevel() const;

    /**
     * @brief Get current input level (after smoothing).
     */
    float getCurrentInputLevel() const;

private:
    /**
     * @brief Apply configuration to nodes.
     */
    void applyConfiguration(const ModeConfiguration& config, bool immediate);

    /**
     * @brief Configure graph routing for mode.
     */
    void configureGraphForMode(AudioMode mode);

    /**
     * @brief Start input stream if needed for mode.
     */
    void ensureInputStreamForMode(AudioMode mode);

    // Current state (atomic for thread safety)
    std::atomic<AudioMode> mCurrentMode{AudioMode::CHAOS_PAD};
    std::atomic<AudioMode> mTargetMode{AudioMode::CHAOS_PAD};
    std::atomic<bool> mInTransition{false};
    std::atomic<float> mTransitionProgress{0.0f};

    // Configuration (mutex-protected for UI thread writes)
    ModeConfiguration mCurrentConfig;
    ModeConfiguration mTargetConfig;
    mutable std::mutex mConfigMutex;

    // ========== RT-Safe Snapshot System ==========
    // Double-buffered snapshots for lock-free audio thread access
    ModeSnapshot mSnapshot1;
    ModeSnapshot mSnapshot2;
    std::atomic<ModeSnapshot*> mActiveSnapshot{&mSnapshot1};
    std::atomic<bool> mUsingSnapshot1{true};

    /**
     * @brief Update the inactive snapshot and atomically swap.
     *
     * Called from UI thread after modifying mCurrentConfig/mTargetConfig.
     * The audio thread will see the new configuration on next read.
     */
    void updateSnapshot();

    // Transition parameters
    float mTransitionIncrement = 0.0f;
    int mSampleRate = 48000;

    // Level smoothers for transition
    ParameterSmoother mOscLevelSmoother{0.99f};
    ParameterSmoother mInputLevelSmoother{0.99f};

    // Current smoothed levels (for queries)
    std::atomic<float> mCurrentOscLevel{1.0f};
    std::atomic<float> mCurrentInputLevel{0.0f};

    // Node references
    AudioGraph* mGraph = nullptr;
    XYMapper* mXYMapper = nullptr;
    OscillatorNode* mOscillatorNode = nullptr;
    InputNode* mInputNode = nullptr;
    MixerNode* mMixerNode = nullptr;
    EffectChainNode* mEffectChainNode = nullptr;
    OutputNode* mOutputNode = nullptr;

    // Callback
    ModeChangeCallback mModeChangeCallback;
    std::mutex mCallbackMutex;
};

} // namespace noisypad
