#include "MixerNode.h"
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

MixerNode::MixerNode() {
    mNumInputChannels = 2;
    mNumOutputChannels = 2;

    // Initialize smoothers with default smoothing time (~10ms at 48kHz)
    for (auto& channel : mInputChannels) {
        channel.levelSmoother.setCoefficient(0.995f);
        channel.panSmoother.setCoefficient(0.995f);
    }
    mCrossfadeSmoother.setCoefficient(0.99f);
    mMasterSmoother.setCoefficient(0.995f);
}

void MixerNode::prepare(int sampleRate, int maxBlockSize) {
    AudioNode::prepare(sampleRate, maxBlockSize);

    // Set smoothing times based on sample rate
    const float levelSmoothTimeMs = 10.0f;
    const float panSmoothTimeMs = 10.0f;
    const float crossfadeSmoothTimeMs = 20.0f;
    const float masterSmoothTimeMs = 10.0f;

    for (auto& channel : mInputChannels) {
        channel.levelSmoother.setSmoothingTime(levelSmoothTimeMs, static_cast<float>(sampleRate));
        channel.panSmoother.setSmoothingTime(panSmoothTimeMs, static_cast<float>(sampleRate));
    }
    mCrossfadeSmoother.setSmoothingTime(crossfadeSmoothTimeMs, static_cast<float>(sampleRate));
    mMasterSmoother.setSmoothingTime(masterSmoothTimeMs, static_cast<float>(sampleRate));

    // Pre-allocate temp buffer
    mTempBuffer.resize(maxBlockSize * 2);
}

void MixerNode::reset() {
    AudioNode::reset();

    for (auto& channel : mInputChannels) {
        channel.levelSmoother.reset(channel.level.load());
        channel.panSmoother.reset(channel.pan.load());
    }
    mCrossfadeSmoother.reset(mCrossfade.load());
    mMasterSmoother.reset(mMasterLevel.load());
}

void MixerNode::process(AudioBuffer& inputBuffer, int numFrames) {
    if (!isActive()) {
        mBuffer.clear();
        return;
    }

    // Clear output buffer
    mBuffer.clear();

    float* outLeft = mBuffer.getWritePointer(0);
    float* outRight = mBuffer.getWritePointer(1);

    const bool anySolo = mAnySolo.load(std::memory_order_acquire);
    const bool crossfadeEnabled = mCrossfadeEnabled.load(std::memory_order_acquire);
    const float targetCrossfade = mCrossfade.load(std::memory_order_acquire);

    // Process each input channel
    for (int inputIdx = 0; inputIdx < MAX_INPUTS; ++inputIdx) {
        InputChannel& channel = mInputChannels[inputIdx];

        // Skip if no input buffer assigned
        if (!channel.inputBuffer) continue;

        // Skip if muted
        if (channel.mute.load(std::memory_order_acquire)) continue;

        // Skip if another channel is soloed and this one isn't
        if (anySolo && !channel.solo.load(std::memory_order_acquire)) continue;

        const float* srcLeft = channel.inputBuffer->getReadPointer(0);
        const float* srcRight = channel.inputBuffer->getReadPointer(1);

        float targetLevel = channel.level.load(std::memory_order_acquire);
        const float targetPan = channel.pan.load(std::memory_order_acquire);

        // Apply crossfade if enabled and this is input 0 or 1
        if (crossfadeEnabled) {
            if (inputIdx == INPUT_OSCILLATOR) {
                // Input 0: level decreases as crossfade increases
                // Use equal-power curve: cos(crossfade * pi/2)
                const float smoothedCrossfade = mCrossfadeSmoother.process(targetCrossfade);
                targetLevel *= std::cos(smoothedCrossfade * static_cast<float>(M_PI) * 0.5f);
            } else if (inputIdx == INPUT_EXTERNAL) {
                // Input 1: level increases as crossfade increases
                // Use equal-power curve: sin(crossfade * pi/2)
                const float smoothedCrossfade = mCrossfadeSmoother.process(targetCrossfade);
                targetLevel *= std::sin(smoothedCrossfade * static_cast<float>(M_PI) * 0.5f);
            }
        }

        // Process sample by sample with smoothing
        for (int i = 0; i < numFrames; ++i) {
            const float smoothedLevel = channel.levelSmoother.process(targetLevel);
            const float smoothedPan = channel.panSmoother.process(targetPan);

            const float leftGain = smoothedLevel * calculatePanGainLeft(smoothedPan);
            const float rightGain = smoothedLevel * calculatePanGainRight(smoothedPan);

            outLeft[i] += srcLeft[i] * leftGain;
            outRight[i] += srcRight[i] * rightGain;
        }
    }

    // Apply master level
    const float targetMaster = mMasterLevel.load(std::memory_order_acquire);
    for (int i = 0; i < numFrames; ++i) {
        const float smoothedMaster = mMasterSmoother.process(targetMaster);
        outLeft[i] *= smoothedMaster;
        outRight[i] *= smoothedMaster;
    }
}

void MixerNode::setInputBuffer(int inputIndex, const AudioBuffer* buffer) {
    if (isInputValid(inputIndex)) {
        mInputChannels[inputIndex].inputBuffer = buffer;
    }
}

const AudioBuffer* MixerNode::getInputBuffer(int inputIndex) const {
    if (isInputValid(inputIndex)) {
        return mInputChannels[inputIndex].inputBuffer;
    }
    return nullptr;
}

void MixerNode::setInputLevel(int inputIndex, float level) {
    if (isInputValid(inputIndex)) {
        mInputChannels[inputIndex].level.store(
            std::clamp(level, 0.0f, 2.0f),
            std::memory_order_release
        );
    }
}

float MixerNode::getInputLevel(int inputIndex) const {
    if (isInputValid(inputIndex)) {
        return mInputChannels[inputIndex].level.load(std::memory_order_acquire);
    }
    return 0.0f;
}

void MixerNode::setInputPan(int inputIndex, float pan) {
    if (isInputValid(inputIndex)) {
        mInputChannels[inputIndex].pan.store(
            std::clamp(pan, -1.0f, 1.0f),
            std::memory_order_release
        );
    }
}

float MixerNode::getInputPan(int inputIndex) const {
    if (isInputValid(inputIndex)) {
        return mInputChannels[inputIndex].pan.load(std::memory_order_acquire);
    }
    return 0.0f;
}

void MixerNode::setInputMute(int inputIndex, bool mute) {
    if (isInputValid(inputIndex)) {
        mInputChannels[inputIndex].mute.store(mute, std::memory_order_release);
    }
}

bool MixerNode::isInputMuted(int inputIndex) const {
    if (isInputValid(inputIndex)) {
        return mInputChannels[inputIndex].mute.load(std::memory_order_acquire);
    }
    return false;
}

void MixerNode::setInputSolo(int inputIndex, bool solo) {
    if (isInputValid(inputIndex)) {
        mInputChannels[inputIndex].solo.store(solo, std::memory_order_release);
        updateSoloState();
    }
}

bool MixerNode::isInputSoloed(int inputIndex) const {
    if (isInputValid(inputIndex)) {
        return mInputChannels[inputIndex].solo.load(std::memory_order_acquire);
    }
    return false;
}

void MixerNode::setCrossfade(float position) {
    mCrossfade.store(std::clamp(position, 0.0f, 1.0f), std::memory_order_release);
}

float MixerNode::getCrossfade() const {
    return mCrossfade.load(std::memory_order_acquire);
}

void MixerNode::setCrossfadeEnabled(bool enabled) {
    mCrossfadeEnabled.store(enabled, std::memory_order_release);
}

bool MixerNode::isCrossfadeEnabled() const {
    return mCrossfadeEnabled.load(std::memory_order_acquire);
}

void MixerNode::setMasterLevel(float level) {
    mMasterLevel.store(std::clamp(level, 0.0f, 2.0f), std::memory_order_release);
}

float MixerNode::getMasterLevel() const {
    return mMasterLevel.load(std::memory_order_acquire);
}

int MixerNode::getActiveInputCount() const {
    int count = 0;
    for (const auto& channel : mInputChannels) {
        if (channel.inputBuffer != nullptr) {
            ++count;
        }
    }
    return count;
}

void MixerNode::updateSoloState() {
    bool anySolo = false;
    for (const auto& channel : mInputChannels) {
        if (channel.solo.load(std::memory_order_acquire)) {
            anySolo = true;
            break;
        }
    }
    mAnySolo.store(anySolo, std::memory_order_release);
}

float MixerNode::calculatePanGainLeft(float pan) const {
    // Equal power panning
    // pan: -1.0 = full left, 0.0 = center, 1.0 = full right
    // angle: 0 to PI/2 (left to right)
    const float angle = (pan + 1.0f) * 0.25f * static_cast<float>(M_PI);
    return std::cos(angle);
}

float MixerNode::calculatePanGainRight(float pan) const {
    // Equal power panning
    const float angle = (pan + 1.0f) * 0.25f * static_cast<float>(M_PI);
    return std::sin(angle);
}

bool MixerNode::isInputValid(int inputIndex) const {
    return inputIndex >= 0 && inputIndex < MAX_INPUTS;
}
