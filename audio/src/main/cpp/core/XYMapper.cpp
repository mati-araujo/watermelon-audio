#include "XYMapper.h"
#include "../nodes/OscillatorNode.h"
#include "../nodes/InputNode.h"
#include "../nodes/MixerNode.h"
#include "../nodes/EffectChainNode.h"
#include <cmath>
#include <algorithm>

namespace watermelon_audio {

XYMapper::XYMapper() {
    // Default configuration is set in XYMappingConfig constructor

    // Initialize both config snapshots with default configuration
    mConfigSnapshot1.config = mConfig;
    mConfigSnapshot1.secondaryMappings.clear();
    mConfigSnapshot1.hasCallback = false;

    mConfigSnapshot2 = mConfigSnapshot1;

    // Set active config snapshot
    mActiveConfig.store(&mConfigSnapshot1, std::memory_order_release);
    mUsingConfigSnapshot1.store(true, std::memory_order_release);
}

void XYMapper::updateConfigSnapshot() {
    // PRECONDITION: mConfigMutex is locked by caller

    // Determine which snapshot is inactive
    bool usingSnapshot1 = mUsingConfigSnapshot1.load(std::memory_order_acquire);
    XYConfigSnapshot* inactiveSnapshot = usingSnapshot1 ? &mConfigSnapshot2 : &mConfigSnapshot1;

    // Update inactive snapshot with current configuration
    inactiveSnapshot->config = mConfig;

    // Copy secondary mappings (need to acquire secondary mutex)
    {
        std::lock_guard<std::mutex> lock(mSecondaryMutex);
        inactiveSnapshot->secondaryMappings = mSecondaryMappings;
    }

    // Check if callback is set (need callback mutex)
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        inactiveSnapshot->hasCallback = (mParameterCallback != nullptr);
    }

    // Atomic swap: audio thread will now see new configuration
    mActiveConfig.store(inactiveSnapshot, std::memory_order_release);
    mUsingConfigSnapshot1.store(!usingSnapshot1, std::memory_order_release);
}

void XYMapper::processXY(float x, float y) {
    // ========== RT-SAFE: Lock-free read from atomic snapshot ==========
    // This function can be called from the audio thread.
    // We read the snapshot atomically without any locks.

    // Clamp inputs to valid range
    x = std::clamp(x, 0.0f, 1.0f);
    y = std::clamp(y, 0.0f, 1.0f);

    // Store raw values
    mLastX.store(x, std::memory_order_release);
    mLastY.store(y, std::memory_order_release);

    // Lock-free read of configuration snapshot
    XYConfigSnapshot* snapshot = mActiveConfig.load(std::memory_order_acquire);
    if (!snapshot) {
        return;
    }

    // Read configuration from snapshot (no locks!)
    const XYMappingConfig& config = snapshot->config;

    // Process X axis
    float processedX = applyCurve(x, config.xAxis.curve, config.xAxis.inverted);
    float mappedX;
    if (config.xAxis.curve == CurveType::LOGARITHMIC) {
        mappedX = config.xAxis.range.mapLog(processedX);
    } else {
        mappedX = config.xAxis.range.map(processedX);
    }

    // Process Y axis
    float processedY = applyCurve(y, config.yAxis.curve, config.yAxis.inverted);
    float mappedY;
    if (config.yAxis.curve == CurveType::LOGARITHMIC) {
        mappedY = config.yAxis.range.mapLog(processedY);
    } else {
        mappedY = config.yAxis.range.map(processedY);
    }

    // Store mapped values
    mLastMappedX.store(mappedX, std::memory_order_release);
    mLastMappedY.store(mappedY, std::memory_order_release);

    // Apply to primary targets
    applyToTarget(config.xAxis.target, mappedX);
    applyToTarget(config.yAxis.target, mappedY);

    // Apply secondary mappings from snapshot (no locks!)
    for (const auto& mapping : snapshot->secondaryMappings) {
        float rawValue = mapping.isXAxis ? x : y;
        float processed = applyCurve(rawValue, mapping.curve, mapping.inverted);
        float mapped;
        if (mapping.curve == CurveType::LOGARITHMIC) {
            mapped = mapping.range.mapLog(processed);
        } else {
            mapped = mapping.range.map(processed);
        }
        applyToTarget(mapping.target, mapped);
    }
}

float XYMapper::applyCurve(float value, CurveType curve, bool inverted) const {
    float v = std::clamp(value, 0.0f, 1.0f);

    if (inverted) {
        v = 1.0f - v;
    }

    switch (curve) {
        case CurveType::LINEAR:
            return v;

        case CurveType::LOGARITHMIC:
            // For logarithmic target ranges, we just use the linear value
            // since mapLog() handles the logarithmic conversion
            return v;

        case CurveType::EXPONENTIAL:
            // Quadratic curve (better for amplitude perception)
            return v * v;

        case CurveType::S_CURVE:
            // Smoothstep (smooth transitions at extremes)
            return v * v * (3.0f - 2.0f * v);

        default:
            return v;
    }
}

void XYMapper::applyToTarget(XYTarget target, float value) {
    // Check for custom callback first
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        if (mParameterCallback) {
            mParameterCallback(target, value);
            return;
        }
    }

    // Direct application to nodes
    switch (target) {
        // ========== Oscillator targets ==========
        case XYTarget::FREQUENCY:
            if (mOscillatorNode) {
                // Get current amplitude, set new frequency
                // Note: we need to call the method that sets both
                mOscillatorNode->setFrequencyAndAmplitude(
                    value,
                    mLastMappedY.load(std::memory_order_acquire)
                );
            }
            break;

        case XYTarget::AMPLITUDE:
            if (mOscillatorNode) {
                mOscillatorNode->setFrequencyAndAmplitude(
                    mLastMappedX.load(std::memory_order_acquire),
                    value
                );
            }
            break;

        case XYTarget::OSCILLATOR_TYPE:
            if (mOscillatorNode) {
                // Map 0-1 to oscillator type index (0-5)
                int type = static_cast<int>(value * 5.0f);
                type = std::clamp(type, 0, 5);
                mOscillatorNode->setOscillatorType(type);
            }
            break;

        // ========== Input targets ==========
        case XYTarget::INPUT_GAIN:
            if (mInputNode) {
                // Value is already in dB range (-20 to +20)
                mInputNode->setInputGain(value);
            }
            break;

        // ========== Effect targets ==========
        case XYTarget::EFFECT_PARAM_1:
        case XYTarget::FILTER_CUTOFF:
            if (mEffectChainNode && mEffectChainNode->getEffectCount() > 0) {
                // Parameter 0 of effect 0 (typically filter cutoff)
                mEffectChainNode->setEffectParameter(0, 0, value);
            }
            break;

        case XYTarget::EFFECT_PARAM_2:
        case XYTarget::FILTER_RESONANCE:
            if (mEffectChainNode && mEffectChainNode->getEffectCount() > 0) {
                // Parameter 1 of effect 0 (typically filter resonance)
                mEffectChainNode->setEffectParameter(0, 1, value);
            }
            break;

        case XYTarget::EFFECT_WET_DRY:
            if (mEffectChainNode) {
                mEffectChainNode->setWetDryMix(value);
            }
            break;

        case XYTarget::DELAY_TIME:
            if (mEffectChainNode) {
                // Look for delay effect (type 2) and set time
                // Assuming delay might be at index 1
                mEffectChainNode->setEffectParameter(1, 0, value);
            }
            break;

        case XYTarget::DELAY_FEEDBACK:
            if (mEffectChainNode) {
                mEffectChainNode->setEffectParameter(1, 1, value);
            }
            break;

        case XYTarget::REVERB_SIZE:
            if (mEffectChainNode) {
                // Look for reverb effect (type 1)
                // Assuming reverb might be at index 2 or 1
                mEffectChainNode->setEffectParameter(2, 1, value);
            }
            break;

        case XYTarget::REVERB_DAMPING:
            if (mEffectChainNode) {
                mEffectChainNode->setEffectParameter(2, 4, value);
            }
            break;

        case XYTarget::NONE:
        default:
            // No action
            break;
    }
}

void XYMapper::addSecondaryMapping(const SecondaryMapping& mapping) {
    {
        std::lock_guard<std::mutex> lock(mSecondaryMutex);
        mSecondaryMappings.push_back(mapping);
    }

    // Update RT-safe snapshot
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        updateConfigSnapshot();
    }
}

void XYMapper::clearSecondaryMappings() {
    {
        std::lock_guard<std::mutex> lock(mSecondaryMutex);
        mSecondaryMappings.clear();
    }

    // Update RT-safe snapshot
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        updateConfigSnapshot();
    }
}

void XYMapper::setParameterCallback(ParameterCallback callback) {
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        mParameterCallback = std::move(callback);
    }

    // Update RT-safe snapshot
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        updateConfigSnapshot();
    }
}

void XYMapper::clearParameterCallback() {
    {
        std::lock_guard<std::mutex> lock(mCallbackMutex);
        mParameterCallback = nullptr;
    }

    // Update RT-safe snapshot
    {
        std::lock_guard<std::mutex> lock(mConfigMutex);
        updateConfigSnapshot();
    }
}

} // namespace watermelon_audio
