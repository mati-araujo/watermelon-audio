#include "ModeManager.h"
#include "../nodes/OscillatorNode.h"
#include "../nodes/InputNode.h"
#include "../nodes/MixerNode.h"
#include "../nodes/EffectChainNode.h"
#include "../backends/BackendManager.h"

namespace watermelon_audio {

ModeManager::ModeManager() {
    // Initialize with ChaosPad configuration
    mCurrentConfig = ModeConfigurations::getChaosPad();
    mTargetConfig = mCurrentConfig;

    // Initialize both snapshots with default configuration
    mSnapshot1.current = mCurrentConfig;
    mSnapshot1.target = mTargetConfig;
    mSnapshot1.transitionIncrement = 0.0f;
    mSnapshot1.inTransition = false;

    mSnapshot2 = mSnapshot1;  // Copy to second snapshot

    // Set active snapshot
    mActiveSnapshot.store(&mSnapshot1, std::memory_order_release);
    mUsingSnapshot1.store(true, std::memory_order_release);

    // Set default smoothing times (~50ms for transitions)
    mOscLevelSmoother.setCoefficient(0.99f);
    mInputLevelSmoother.setCoefficient(0.99f);
}

void ModeManager::updateSnapshot() {
    // PRECONDITION: mConfigMutex is locked by caller

    // Determine which snapshot is inactive
    bool usingSnapshot1 = mUsingSnapshot1.load(std::memory_order_acquire);
    ModeSnapshot* inactiveSnapshot = usingSnapshot1 ? &mSnapshot2 : &mSnapshot1;

    // Update inactive snapshot with current configuration
    inactiveSnapshot->current = mCurrentConfig;
    inactiveSnapshot->target = mTargetConfig;
    inactiveSnapshot->transitionIncrement = mTransitionIncrement;
    inactiveSnapshot->inTransition = mInTransition.load(std::memory_order_acquire);

    // Atomic swap: audio thread will now see new configuration
    mActiveSnapshot.store(inactiveSnapshot, std::memory_order_release);
    mUsingSnapshot1.store(!usingSnapshot1, std::memory_order_release);
}

void ModeManager::setNodes(OscillatorNode* osc, InputNode* input,
                            MixerNode* mixer, EffectChainNode* effects) {
    mOscillatorNode = osc;
    mInputNode = input;
    mMixerNode = mixer;
    mEffectChainNode = effects;
}

void ModeManager::prepare(int sampleRate) {
    mSampleRate = sampleRate;

    // Set smoothing times based on sample rate
    const float transitionSmoothTimeMs = 50.0f;
    mOscLevelSmoother.setSmoothingTime(transitionSmoothTimeMs, static_cast<float>(sampleRate));
    mInputLevelSmoother.setSmoothingTime(transitionSmoothTimeMs, static_cast<float>(sampleRate));

    // Initialize smoothers with current config values
    mOscLevelSmoother.reset(mCurrentConfig.oscillatorLevel);
    mInputLevelSmoother.reset(mCurrentConfig.inputLevel);

    mCurrentOscLevel.store(mCurrentConfig.oscillatorLevel, std::memory_order_release);
    mCurrentInputLevel.store(mCurrentConfig.inputLevel, std::memory_order_release);

    // Initialize snapshot with current configuration
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        updateSnapshot();
    }
}

void ModeManager::reset() {
    ModeConfiguration config;
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        config = mCurrentConfig;

        // Clear transition state
        mInTransition.store(false, std::memory_order_release);
        mTransitionProgress.store(0.0f, std::memory_order_release);
        mTransitionIncrement = 0.0f;

        // Update RT-safe snapshot
        updateSnapshot();
    }

    mOscLevelSmoother.reset(config.oscillatorLevel);
    mInputLevelSmoother.reset(config.inputLevel);

    mCurrentOscLevel.store(config.oscillatorLevel, std::memory_order_release);
    mCurrentInputLevel.store(config.inputLevel, std::memory_order_release);
}

void ModeManager::setMode(AudioMode mode) {
    float transitionTime = ModeConfigurations::getConfiguration(mode).transitionTimeMs;
    setModeWithTransition(mode, transitionTime);
}

void ModeManager::setModeWithTransition(AudioMode mode, float transitionTimeMs) {
    AudioMode currentMode = mCurrentMode.load(std::memory_order_acquire);

    // Skip if already in target mode and not transitioning
    if (currentMode == mode && !mInTransition.load(std::memory_order_acquire)) {
        return;
    }

    AudioMode oldMode = currentMode;
    mTargetMode.store(mode, std::memory_order_release);

    // Get new configuration
    ModeConfiguration newConfig = ModeConfigurations::getConfiguration(mode);
    newConfig.transitionTimeMs = transitionTimeMs;

    // Calculate transition increment
    float transitionSamples = transitionTimeMs * 0.001f * static_cast<float>(mSampleRate);
    mTransitionIncrement = transitionSamples > 0.0f ? (1.0f / transitionSamples) : 1.0f;

    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        mTargetConfig = newConfig;

        // Update atomic flags before snapshot
        mTransitionProgress.store(0.0f, std::memory_order_release);
        mInTransition.store(true, std::memory_order_release);

        // Update RT-safe snapshot (audio thread will see this on next read)
        updateSnapshot();
    }

    // Configure graph routing for new mode
    configureGraphForMode(mode);

    // Update XY mapper configuration
    if (mXYMapper) {
        mXYMapper->setConfiguration(newConfig.xyMapping);
    }

    // Ensure input stream is running if needed
    ensureInputStreamForMode(mode);

    // Invoke callback
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        if (mModeChangeCallback) {
            mModeChangeCallback(oldMode, mode);
        }
    }
}

void ModeManager::setCustomConfiguration(const ModeConfiguration& config) {
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        mCurrentConfig = config;
        mTargetConfig = config;

        // Not in transition when setting custom config
        mInTransition.store(false, std::memory_order_release);
        mTransitionIncrement = 0.0f;

        // Update RT-safe snapshot
        updateSnapshot();
    }

    mCurrentMode.store(config.mode, std::memory_order_release);
    mTargetMode.store(config.mode, std::memory_order_release);

    applyConfiguration(config, true);
}

ModeConfiguration ModeManager::getCurrentConfiguration() const {
    std::lock_guard<std::mutex> lock(mConfigMutex);
    return mCurrentConfig;
}

void ModeManager::updateTransition(int numSamples) {
    // ========== RT-SAFE: Lock-free read from atomic snapshot ==========
    // This function is called from the audio thread every block.
    // We read the snapshot atomically without any locks.

    if (!mInTransition.load(std::memory_order_acquire)) {
        return;
    }

    // Lock-free read of snapshot
    ModeSnapshot* snapshot = mActiveSnapshot.load(std::memory_order_acquire);
    if (!snapshot) {
        return;
    }

    // Read configuration from snapshot (no locks!)
    const ModeConfiguration& current = snapshot->current;
    const ModeConfiguration& target = snapshot->target;
    float transitionIncrement = snapshot->transitionIncrement;

    // Advance transition progress
    float progress = mTransitionProgress.load(std::memory_order_acquire);
    progress += transitionIncrement * static_cast<float>(numSamples);

    if (progress >= 1.0f) {
        // Transition complete
        progress = 1.0f;
        mTransitionProgress.store(progress, std::memory_order_release);
        mInTransition.store(false, std::memory_order_release);
        mCurrentMode.store(mTargetMode.load(std::memory_order_acquire), std::memory_order_release);

        // Note: The UI thread will update mCurrentConfig when it sees
        // mInTransition == false. We don't acquire locks here.

        // Apply final levels immediately
        if (mMixerNode) {
            mMixerNode->setInputLevel(MixerNode::INPUT_OSCILLATOR, target.oscillatorLevel);
            mMixerNode->setInputLevel(MixerNode::INPUT_EXTERNAL, target.inputLevel);
        }

        // Store final levels for queries
        mCurrentOscLevel.store(target.oscillatorLevel, std::memory_order_release);
        mCurrentInputLevel.store(target.inputLevel, std::memory_order_release);
    } else {
        mTransitionProgress.store(progress, std::memory_order_release);

        // Interpolate levels
        float oscLevel = current.oscillatorLevel * (1.0f - progress) +
                         target.oscillatorLevel * progress;
        float inputLevel = current.inputLevel * (1.0f - progress) +
                           target.inputLevel * progress;

        // Apply smoothed levels to mixer
        if (mMixerNode) {
            mMixerNode->setInputLevel(MixerNode::INPUT_OSCILLATOR, oscLevel);
            mMixerNode->setInputLevel(MixerNode::INPUT_EXTERNAL, inputLevel);
        }

        // Store current levels for queries
        mCurrentOscLevel.store(oscLevel, std::memory_order_release);
        mCurrentInputLevel.store(inputLevel, std::memory_order_release);
    }
}

void ModeManager::configureGraphForMode(AudioMode mode) {
    switch (mode) {
        case AudioMode::CHAOS_PAD:
            // Oscillator only
            if (mOscillatorNode) {
                mOscillatorNode->setActive(true);
            }
            if (mInputNode) {
                mInputNode->setActive(false);
                // Don't stop input stream immediately - let transition complete
            }
            if (mMixerNode) {
                mMixerNode->setCrossfadeEnabled(false);
            }
            break;

        case AudioMode::INPUT_FX:
            // Input only
            if (mOscillatorNode) {
                mOscillatorNode->setActive(false);
            }
            if (mInputNode) {
                mInputNode->setActive(true);
            }
            if (mMixerNode) {
                mMixerNode->setCrossfadeEnabled(false);
            }
            break;

        case AudioMode::MIX:
            // Both sources with crossfade
            if (mOscillatorNode) {
                mOscillatorNode->setActive(true);
            }
            if (mInputNode) {
                mInputNode->setActive(true);
            }
            if (mMixerNode) {
                mMixerNode->setCrossfadeEnabled(true);
                mMixerNode->setCrossfade(0.5f);  // Start at 50/50 mix
            }
            break;
    }
}

void ModeManager::ensureInputStreamForMode(AudioMode mode) {
    if (!mInputNode) return;

    bool needsInput = ModeUtils::requiresInput(mode);

    if (needsInput) {
        // Check if USB backend is active - if so, DON'T start Oboe input stream
        // USB provides input via feedExternalInput(), not via Oboe mic capture
        auto& backendManager = BackendManager::getInstance();
        bool isUsbActive = (backendManager.getCurrentType() == BackendType::LIBUSB);

        if (isUsbActive) {
            // USB provides input - stop Oboe input if running
            if (mInputNode->isInputStreamRunning()) {
                mInputNode->stopInputStream();
            }
        } else {
            // Non-USB mode - start Oboe input stream for mic capture
            if (!mInputNode->isInputStreamRunning()) {
                mInputNode->startInputStream();
            }
        }
    } else {
        // Stop input stream when transitioning away from input modes
        // Only stop after transition completes to avoid glitches
        // The actual stop will happen in the transition completion
    }
}

void ModeManager::applyConfiguration(const ModeConfiguration& config, bool immediate) {
    // Apply to mixer
    if (mMixerNode) {
        if (immediate) {
            mMixerNode->setInputLevel(MixerNode::INPUT_OSCILLATOR, config.oscillatorLevel);
            mMixerNode->setInputLevel(MixerNode::INPUT_EXTERNAL, config.inputLevel);
        }
    }

    // Apply to XY mapper
    if (mXYMapper) {
        mXYMapper->setConfiguration(config.xyMapping);
    }

    // Update stored levels
    mCurrentOscLevel.store(config.oscillatorLevel, std::memory_order_release);
    mCurrentInputLevel.store(config.inputLevel, std::memory_order_release);

    // Stop input stream if not needed anymore
    if (immediate && !config.inputActive && mInputNode) {
        if (mInputNode->isInputStreamRunning()) {
            mInputNode->stopInputStream();
        }
    }
}

void ModeManager::setModeChangeCallback(ModeChangeCallback callback) {
    std::lock_guard<std::mutex> lock(mCallbackMutex);
    mModeChangeCallback = std::move(callback);
}

float ModeManager::getCurrentOscillatorLevel() const {
    return mCurrentOscLevel.load(std::memory_order_acquire);
}

float ModeManager::getCurrentInputLevel() const {
    return mCurrentInputLevel.load(std::memory_order_acquire);
}

} // namespace watermelon_audio
