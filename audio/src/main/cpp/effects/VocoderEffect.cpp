#include "VocoderEffect.h"
#include <algorithm>
#include "../platform/Logger.h"
#include <cassert>

#define LOG_TAG "VocoderEffect"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

// Maximum buffer size
constexpr int MAX_BUFFER_SIZE = 4096;

VocoderEffect::VocoderEffect()
    : mVocoderBank(16)
    , mInputHPF_L(48000.0f)
    , mInputHPF_R(48000.0f)
    , mOutputLPF_L(48000.0f)
    , mOutputLPF_R(48000.0f) {

    // Pre-allocate buffers
    mModulatorBuffer.resize(MAX_BUFFER_SIZE, 0.0f);
    mCarrierMono.resize(MAX_BUFFER_SIZE, 0.0f);
    mModulatorMono.resize(MAX_BUFFER_SIZE, 0.0f);
    mOutputMono.resize(MAX_BUFFER_SIZE, 0.0f);
    mInternalCarrier.resize(MAX_BUFFER_SIZE, 0.0f);

    // Initialize filters
    mInputHPF_L.setHighpass(80.0f, 0.707f);  // Remove low rumble
    mInputHPF_R.setHighpass(80.0f, 0.707f);
    mOutputLPF_L.setLowpass(12000.0f, 0.707f);  // Smooth output
    mOutputLPF_R.setLowpass(12000.0f, 0.707f);

    // Initialize band envelopes to zero
    mBandEnvelopes.fill(0.0f);

    LOGI("VocoderEffect created: %d bands", mBandCount.load());
}

void VocoderEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;

    // Update vocoder bank
    mVocoderBank.prepare(static_cast<float>(sampleRate));

    // Update filters
    mInputHPF_L.setSampleRate(static_cast<float>(sampleRate));
    mInputHPF_R.setSampleRate(static_cast<float>(sampleRate));
    mOutputLPF_L.setSampleRate(static_cast<float>(sampleRate));
    mOutputLPF_R.setSampleRate(static_cast<float>(sampleRate));

    // Reset filter states
    mInputHPF_L.reset();
    mInputHPF_R.reset();
    mOutputLPF_L.reset();
    mOutputLPF_R.reset();

    LOGI("VocoderEffect sample rate set to %d", sampleRate);
}

void VocoderEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case BAND_COUNT: {
            int bands = static_cast<int>(std::clamp(value, 4.0f, 32.0f));
            mBandCount.store(bands, std::memory_order_relaxed);
            mVocoderBank.setNumBands(bands);
            LOGI("VocoderEffect bands set to %d", bands);
            break;
        }
        case FORMANT_SHIFT: {
            float shift = std::clamp(value, -24.0f, 24.0f);
            mFormantShift.store(shift, std::memory_order_relaxed);
            mVocoderBank.setFormantShift(shift);
            break;
        }
        case ATTACK: {
            float attack = std::clamp(value, 0.1f, 100.0f);
            mAttackMs.store(attack, std::memory_order_relaxed);
            mVocoderBank.setAttack(attack);
            break;
        }
        case RELEASE: {
            float release = std::clamp(value, 1.0f, 500.0f);
            mReleaseMs.store(release, std::memory_order_relaxed);
            mVocoderBank.setRelease(release);
            break;
        }
        case MIX: {
            float mix = std::clamp(value, 0.0f, 1.0f);
            mMix.store(mix, std::memory_order_relaxed);
            break;
        }
        case CARRIER_LEVEL: {
            float level = std::clamp(value, 0.0f, 1.0f);
            mCarrierLevel.store(level, std::memory_order_relaxed);
            break;
        }
        case MOD_SOURCE: {
            int source = static_cast<int>(std::clamp(value, 0.0f, 1.0f));
            mModSource.store(source, std::memory_order_relaxed);
            LOGI("VocoderEffect mod source set to %d", source);
            break;
        }
        case CARRIER_SOURCE: {
            int source = static_cast<int>(std::clamp(value, 0.0f, 1.0f));
            mCarrierSource.store(source, std::memory_order_relaxed);
            LOGI("VocoderEffect carrier source set to %d (0=input, 1=internal)", source);
            break;
        }
        case CARRIER_FREQ: {
            // Expanded range for musical use (was 50-500, now 20-2000Hz)
            float freq = std::clamp(value, 20.0f, 2000.0f);
            mCarrierFreqParam.store(freq, std::memory_order_relaxed);
            mCarrierFrequency.store(freq, std::memory_order_relaxed);
            LOGI("VocoderEffect carrier freq set to %.1f Hz", freq);
            break;
        }
        default:
            LOGE("VocoderEffect: unknown param %d", paramId);
            break;
    }
}

float VocoderEffect::getParam(int paramId) {
    switch (paramId) {
        case BAND_COUNT:
            return static_cast<float>(mBandCount.load(std::memory_order_relaxed));
        case FORMANT_SHIFT:
            return mFormantShift.load(std::memory_order_relaxed);
        case ATTACK:
            return mAttackMs.load(std::memory_order_relaxed);
        case RELEASE:
            return mReleaseMs.load(std::memory_order_relaxed);
        case MIX:
            return mMix.load(std::memory_order_relaxed);
        case CARRIER_LEVEL:
            return mCarrierLevel.load(std::memory_order_relaxed);
        case MOD_SOURCE:
            return static_cast<float>(mModSource.load(std::memory_order_relaxed));
        case CARRIER_SOURCE:
            return static_cast<float>(mCarrierSource.load(std::memory_order_relaxed));
        case CARRIER_FREQ:
            return mCarrierFreqParam.load(std::memory_order_relaxed);
        default:
            return 0.0f;
    }
}

void VocoderEffect::setModulatorBuffer(const float* modulator, int numSamples) {
    if (numSamples <= 0 || modulator == nullptr) {
        mHasExternalMod.store(false, std::memory_order_release);
        return;
    }

    // Buffers pre-allocated to MAX_BUFFER_SIZE in constructor.
    // Clamp to avoid out-of-bounds; assert in debug.
    assert(static_cast<int>(mModulatorBuffer.size()) >= numSamples &&
           "Modulator buffer too small — increase MAX_BUFFER_SIZE");
    if (static_cast<int>(mModulatorBuffer.size()) < numSamples) {
        numSamples = static_cast<int>(mModulatorBuffer.size());
    }

    // Copy modulator data
    std::copy(modulator, modulator + numSamples, mModulatorBuffer.data());
    mModulatorSamples.store(numSamples, std::memory_order_relaxed);
    mHasExternalMod.store(true, std::memory_order_release);
}

void VocoderEffect::process(float* input, float* output, int numFrames) {
    if (numFrames <= 0) {
        return;
    }

    // Buffers pre-allocated to MAX_BUFFER_SIZE in constructor.
    assert(static_cast<int>(mCarrierMono.size()) >= numFrames &&
           "Working buffer too small — increase MAX_BUFFER_SIZE");
    if (numFrames > static_cast<int>(mCarrierMono.size())) {
        numFrames = static_cast<int>(mCarrierMono.size());
    }

    // Load parameters
    float mix = mMix.load(std::memory_order_acquire);
    float carrierLevel = mCarrierLevel.load(std::memory_order_acquire);
    int modSource = mModSource.load(std::memory_order_acquire);
    int carrierSource = mCarrierSource.load(std::memory_order_acquire);
    bool hasExtMod = mHasExternalMod.load(std::memory_order_acquire);

    // Convert input to mono (used as carrier or modulator depending on mode)
    for (int i = 0; i < numFrames; ++i) {
        float left = mInputHPF_L.process(input[i * 2]);
        float right = mInputHPF_R.process(input[i * 2 + 1]);
        mCarrierMono[i] = (left + right) * 0.5f;
    }

    // ========== CARRIER SELECTION ==========
    // carrierSource: 0 = use input as carrier, 1 = use internal oscillator
    float* carrierSignal;
    if (carrierSource == 1) {
        // INPUT_FX MODE: Use internal oscillator as carrier
        // The input (mic) becomes the modulator
        generateCarrier(mInternalCarrier.data(), numFrames);

        // Apply carrier level to internal oscillator
        for (int i = 0; i < numFrames; ++i) {
            mInternalCarrier[i] *= carrierLevel;
        }
        carrierSignal = mInternalCarrier.data();

        // Input is the modulator (voice shapes the synth)
        std::copy(mCarrierMono.data(), mCarrierMono.data() + numFrames,
                  mModulatorMono.data());
    } else {
        // OSCILLATOR MODE: Use input as carrier
        // Apply carrier level
        for (int i = 0; i < numFrames; ++i) {
            mCarrierMono[i] *= carrierLevel;
        }
        carrierSignal = mCarrierMono.data();

        // ========== MODULATOR SELECTION ==========
        // modSource: 0 = self-vocoding, 1 = external mic
        if (modSource == 1 && hasExtMod) {
            // External modulator (mic via setModulatorBuffer)
            int modSamples = mModulatorSamples.load(std::memory_order_acquire);
            int copyCount = std::min(modSamples, numFrames);

            std::copy(mModulatorBuffer.data(), mModulatorBuffer.data() + copyCount,
                      mModulatorMono.data());

            if (copyCount < numFrames) {
                std::fill(mModulatorMono.data() + copyCount,
                          mModulatorMono.data() + numFrames, 0.0f);
            }
        } else {
            // Self-vocoding: carrier = modulator
            std::copy(mCarrierMono.data(), mCarrierMono.data() + numFrames,
                      mModulatorMono.data());
        }
    }

    // ========== VOCODER PROCESSING ==========
    // Analyze modulator -> extract band envelopes
    mVocoderBank.analyze(mModulatorMono.data(), numFrames, mBandEnvelopes);

    // Synthesize output using carrier and band envelopes
    mVocoderBank.synthesize(carrierSignal, mOutputMono.data(), numFrames,
                            mBandEnvelopes);

    // ========== OUTPUT MIXING ==========
    for (int i = 0; i < numFrames; ++i) {
        float dry = (input[i * 2] + input[i * 2 + 1]) * 0.5f;
        float wet = mOutputMono[i];

        // Mix wet/dry
        float mixed = DSPMath::crossfade(dry, wet, mix);

        // Apply output lowpass for smoothness
        float outL = mOutputLPF_L.process(mixed);
        float outR = mOutputLPF_R.process(mixed);

        output[i * 2] = outL;
        output[i * 2 + 1] = outR;
    }
}

void VocoderEffect::generateCarrier(float* buffer, int numSamples) {
    float freq = mCarrierFrequency.load(std::memory_order_relaxed);
    float phaseIncrement = freq / static_cast<float>(mSampleRate);

    for (int i = 0; i < numSamples; ++i) {
        // Sawtooth wave: -1 to 1 linearly
        buffer[i] = 2.0f * mCarrierPhase - 1.0f;

        // Advance phase
        mCarrierPhase += phaseIncrement;
        if (mCarrierPhase >= 1.0f) {
            mCarrierPhase -= 1.0f;
        }
    }
}

void VocoderEffect::stereoToMono(const float* stereo, float* mono, int numFrames) {
    for (int i = 0; i < numFrames; ++i) {
        mono[i] = (stereo[i * 2] + stereo[i * 2 + 1]) * 0.5f;
    }
}

void VocoderEffect::monoToStereo(const float* mono, float* stereo, int numFrames) {
    for (int i = 0; i < numFrames; ++i) {
        stereo[i * 2] = mono[i];
        stereo[i * 2 + 1] = mono[i];
    }
}
