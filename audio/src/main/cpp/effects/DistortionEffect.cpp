#include "DistortionEffect.h"
#include <algorithm>
#include "../platform/Logger.h"
#include <cassert>

#define LOG_TAG "DistortionEffect"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

// Maximum buffer size
constexpr int MAX_BUFFER_SIZE = 4096;

DistortionEffect::DistortionEffect()
    : mOversamplerL(MAX_BUFFER_SIZE)
    , mOversamplerR(MAX_BUFFER_SIZE)
    // Universal filters
    , mPreHPF_L(48000.0f)
    , mPreHPF_R(48000.0f)
    , mPostLPF_L(48000.0f)
    , mPostLPF_R(48000.0f)
    // Tube Screamer filters
    , mTSMidBoost_L(48000.0f)
    , mTSMidBoost_R(48000.0f)
    // RAT filters
    , mRATFilter_L(48000.0f)
    , mRATFilter_R(48000.0f)
    // Big Muff tone stack
    , mMuffToneLPF_L(48000.0f)
    , mMuffToneLPF_R(48000.0f)
    , mMuffToneHPF_L(48000.0f)
    , mMuffToneHPF_R(48000.0f)
    , mMuffMidScoop_L(48000.0f)
    , mMuffMidScoop_R(48000.0f)
    // HM-2 EQ stack
    , mHM2LowShelf_L(48000.0f)
    , mHM2LowShelf_R(48000.0f)
    , mHM2HighShelf_L(48000.0f)
    , mHM2HighShelf_R(48000.0f)
    , mHM2MidScoop_L(48000.0f)
    , mHM2MidScoop_R(48000.0f)
    , mHM2Presence_L(48000.0f)
    , mHM2Presence_R(48000.0f)
    // Metal Zone parametric EQ
    , mMTLowShelf_L(48000.0f)
    , mMTLowShelf_R(48000.0f)
    , mMTHighShelf_L(48000.0f)
    , mMTHighShelf_R(48000.0f)
    , mMTMidPeak_L(48000.0f)
    , mMTMidPeak_R(48000.0f)
    // Generic tone filters
    , mPreTone_L(48000.0f)
    , mPreTone_R(48000.0f)
    , mPostTone_L(48000.0f)
    , mPostTone_R(48000.0f) {

    // Pre-allocate buffers for 4x oversampling
    mUpsampledL.resize(MAX_BUFFER_SIZE * 4, 0.0f);
    mUpsampledR.resize(MAX_BUFFER_SIZE * 4, 0.0f);
    mProcessedL.resize(MAX_BUFFER_SIZE * 4, 0.0f);
    mProcessedR.resize(MAX_BUFFER_SIZE * 4, 0.0f);

    // Pre-allocate working buffers (RT-safe: no allocations in process())
    mInputL.resize(MAX_BUFFER_SIZE, 0.0f);
    mInputR.resize(MAX_BUFFER_SIZE, 0.0f);
    mDryL.resize(MAX_BUFFER_SIZE, 0.0f);
    mDryR.resize(MAX_BUFFER_SIZE, 0.0f);
    mOutputL.resize(MAX_BUFFER_SIZE, 0.0f);
    mOutputR.resize(MAX_BUFFER_SIZE, 0.0f);

    // Initialize filters
    updateFilters();

    LOGI("DistortionEffect created with professional pedal emulations");
}

void DistortionEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    float sr = static_cast<float>(sampleRate);

    // Update oversamplers
    mOversamplerL.prepare(sr, MAX_BUFFER_SIZE);
    mOversamplerR.prepare(sr, MAX_BUFFER_SIZE);

    // Update universal filters
    mPreHPF_L.setSampleRate(sr);
    mPreHPF_R.setSampleRate(sr);
    mPostLPF_L.setSampleRate(sr);
    mPostLPF_R.setSampleRate(sr);

    // Update Tube Screamer filters
    mTSMidBoost_L.setSampleRate(sr);
    mTSMidBoost_R.setSampleRate(sr);

    // Update RAT filters
    mRATFilter_L.setSampleRate(sr);
    mRATFilter_R.setSampleRate(sr);

    // Update Big Muff tone stack
    mMuffToneLPF_L.setSampleRate(sr);
    mMuffToneLPF_R.setSampleRate(sr);
    mMuffToneHPF_L.setSampleRate(sr);
    mMuffToneHPF_R.setSampleRate(sr);
    mMuffMidScoop_L.setSampleRate(sr);
    mMuffMidScoop_R.setSampleRate(sr);

    // Update HM-2 EQ stack
    mHM2LowShelf_L.setSampleRate(sr);
    mHM2LowShelf_R.setSampleRate(sr);
    mHM2HighShelf_L.setSampleRate(sr);
    mHM2HighShelf_R.setSampleRate(sr);
    mHM2MidScoop_L.setSampleRate(sr);
    mHM2MidScoop_R.setSampleRate(sr);
    mHM2Presence_L.setSampleRate(sr);
    mHM2Presence_R.setSampleRate(sr);

    // Update Metal Zone parametric EQ
    mMTLowShelf_L.setSampleRate(sr);
    mMTLowShelf_R.setSampleRate(sr);
    mMTHighShelf_L.setSampleRate(sr);
    mMTHighShelf_R.setSampleRate(sr);
    mMTMidPeak_L.setSampleRate(sr);
    mMTMidPeak_R.setSampleRate(sr);

    // Update generic tone filters
    mPreTone_L.setSampleRate(sr);
    mPreTone_R.setSampleRate(sr);
    mPostTone_L.setSampleRate(sr);
    mPostTone_R.setSampleRate(sr);

    // Update parameter smoothers for new sample rate
    mDriveSmoother.setSmoothingTime(10.0f, sr);
    mLevelSmoother.setSmoothingTime(10.0f, sr);
    mMixSmoother.setSmoothingTime(10.0f, sr);

    updateFilters();

    LOGI("DistortionEffect sample rate set to %d", sampleRate);
}

void DistortionEffect::reset() {
    mDriveSmoother.reset(mDrive.load(std::memory_order_relaxed));
    mLevelSmoother.reset(mLevel.load(std::memory_order_relaxed));
    mMixSmoother.reset(mMix.load(std::memory_order_relaxed));

    mLastSlewL = 0.0f;
    mLastSlewR = 0.0f;
    mSagVoltage = 1.0f;
    mOctavePhaseL = 0.0f;
    mOctavePhaseR = 0.0f;
    mBitcrushHoldL = 0.0f;
    mBitcrushHoldR = 0.0f;
    mBitcrushCounter = 0;

    mOversamplerL.reset();
    mOversamplerR.reset();

    mPreHPF_L.reset();
    mPreHPF_R.reset();
    mPostLPF_L.reset();
    mPostLPF_R.reset();
    mTSMidBoost_L.reset();
    mTSMidBoost_R.reset();
    mRATFilter_L.reset();
    mRATFilter_R.reset();
    mMuffToneLPF_L.reset();
    mMuffToneLPF_R.reset();
    mMuffToneHPF_L.reset();
    mMuffToneHPF_R.reset();
    mMuffMidScoop_L.reset();
    mMuffMidScoop_R.reset();
    mHM2LowShelf_L.reset();
    mHM2LowShelf_R.reset();
    mHM2HighShelf_L.reset();
    mHM2HighShelf_R.reset();
    mHM2MidScoop_L.reset();
    mHM2MidScoop_R.reset();
    mHM2Presence_L.reset();
    mHM2Presence_R.reset();
    mMTLowShelf_L.reset();
    mMTLowShelf_R.reset();
    mMTHighShelf_L.reset();
    mMTHighShelf_R.reset();
    mMTMidPeak_L.reset();
    mMTMidPeak_R.reset();
    mPreTone_L.reset();
    mPreTone_R.reset();
    mPostTone_L.reset();
    mPostTone_R.reset();
}

void DistortionEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case DRIVE: {
            float drive = std::clamp(value, 0.0f, 1.0f);
            mDrive.store(drive, std::memory_order_relaxed);
            break;
        }
        case TONE: {
            float tone = std::clamp(value, 0.0f, 1.0f);
            mTone.store(tone, std::memory_order_relaxed);
            updateFilters();
            break;
        }
        case LEVEL: {
            float level = std::clamp(value, 0.0f, 1.0f);
            mLevel.store(level, std::memory_order_relaxed);
            break;
        }
        case MIX: {
            float mix = std::clamp(value, 0.0f, 1.0f);
            mMix.store(mix, std::memory_order_relaxed);
            break;
        }
        case ALGORITHM: {
            // Support both new pedal types (0-13) and legacy (100-104)
            int algo = static_cast<int>(value);
            if ((algo >= 0 && algo < DistortionVariants::COUNT) ||
                (algo >= DistortionVariants::LEGACY_SOFT_CLIP && algo <= DistortionVariants::LEGACY_BITCRUSH)) {
                mAlgorithm.store(algo, std::memory_order_relaxed);
                updateFilters();
                LOGI("DistortionEffect pedal set to %s", DistortionVariants::getName(algo));
            } else {
                LOGE("DistortionEffect: invalid algorithm %d", algo);
            }
            break;
        }
        case PARAM_A: {
            float paramA = std::clamp(value, 0.0f, 1.0f);
            mParamA.store(paramA, std::memory_order_relaxed);
            updateFilters();
            break;
        }
        case PARAM_B: {
            float paramB = std::clamp(value, 0.0f, 1.0f);
            mParamB.store(paramB, std::memory_order_relaxed);
            updateFilters();
            break;
        }
        case PARAM_C: {
            float paramC = std::clamp(value, 0.0f, 1.0f);
            mParamC.store(paramC, std::memory_order_relaxed);
            updateFilters();
            break;
        }
        case OVERSAMPLE: {
            int factor = static_cast<int>(std::clamp(value, 0.0f, 2.0f));
            mOversampleFactor.store(factor, std::memory_order_relaxed);

            // Update oversamplers
            Oversampler::Factor osFactor = Oversampler::Factor::X1;
            if (factor == 1) osFactor = Oversampler::Factor::X2;
            else if (factor == 2) osFactor = Oversampler::Factor::X4;

            mOversamplerL.setFactor(osFactor);
            mOversamplerR.setFactor(osFactor);

            LOGI("DistortionEffect oversample set to %dx", (factor == 0) ? 1 : (factor == 1) ? 2 : 4);
            break;
        }
        case PRE_LOW_CUT: {
            float freq = std::clamp(value, 20.0f, 500.0f);
            mPreLowCut.store(freq, std::memory_order_relaxed);
            updateFilters();
            break;
        }
        case POST_HIGH_CUT: {
            float freq = std::clamp(value, 1000.0f, 20000.0f);
            mPostHighCut.store(freq, std::memory_order_relaxed);
            updateFilters();
            break;
        }
        case SAG: {
            float sag = std::clamp(value, 0.0f, 1.0f);
            mSag.store(sag, std::memory_order_relaxed);
            break;
        }
        case BIAS: {
            float bias = std::clamp(value, 0.0f, 1.0f);
            mBias.store(bias, std::memory_order_relaxed);
            break;
        }
        case GATE_THRESHOLD: {
            float gate = std::clamp(value, 0.0f, 1.0f);
            mGateThreshold.store(gate, std::memory_order_relaxed);
            break;
        }
        default:
            LOGE("DistortionEffect: unknown param %d", paramId);
            break;
    }
}

float DistortionEffect::getParam(int paramId) {
    switch (paramId) {
        case DRIVE:
            return mDrive.load(std::memory_order_relaxed);
        case TONE:
            return mTone.load(std::memory_order_relaxed);
        case LEVEL:
            return mLevel.load(std::memory_order_relaxed);
        case MIX:
            return mMix.load(std::memory_order_relaxed);
        case ALGORITHM:
            return static_cast<float>(mAlgorithm.load(std::memory_order_relaxed));
        case PARAM_A:
            return mParamA.load(std::memory_order_relaxed);
        case PARAM_B:
            return mParamB.load(std::memory_order_relaxed);
        case PARAM_C:
            return mParamC.load(std::memory_order_relaxed);
        case OVERSAMPLE:
            return static_cast<float>(mOversampleFactor.load(std::memory_order_relaxed));
        case PRE_LOW_CUT:
            return mPreLowCut.load(std::memory_order_relaxed);
        case POST_HIGH_CUT:
            return mPostHighCut.load(std::memory_order_relaxed);
        case SAG:
            return mSag.load(std::memory_order_relaxed);
        case BIAS:
            return mBias.load(std::memory_order_relaxed);
        case GATE_THRESHOLD:
            return mGateThreshold.load(std::memory_order_relaxed);
        default:
            return 0.0f;
    }
}

void DistortionEffect::process(float* input, float* output, int numFrames) {
    if (numFrames <= 0) {
        return;
    }

    // Load parameters (drive, level, mix smoothed per-sample below)
    float driveTarget = mDrive.load(std::memory_order_acquire);
    float mixTarget = mMix.load(std::memory_order_acquire);
    float levelTarget = mLevel.load(std::memory_order_acquire);
    int algorithm = mAlgorithm.load(std::memory_order_acquire);
    int osFactorInt = mOversampleFactor.load(std::memory_order_acquire);
    float gateThreshold = mGateThreshold.load(std::memory_order_acquire);

    // Smooth drive once per block (used in distortion loop at upsampled rate)
    float drive = mDriveSmoother.process(driveTarget);

    // Determine actual oversampling factor
    int osFactor = (osFactorInt == 0) ? 1 : (osFactorInt == 1) ? 2 : 4;
    int upsampledSize = numFrames * osFactor;

    // Buffers pre-allocated in constructor to MAX_BUFFER_SIZE (4096) and 4x for oversampling.
    // Assert in debug builds; in release, clamp to avoid out-of-bounds.
    assert(static_cast<int>(mUpsampledL.size()) >= upsampledSize &&
           "Buffer too small — increase MAX_BUFFER_SIZE");
    assert(static_cast<int>(mInputL.size()) >= numFrames &&
           "Buffer too small — increase MAX_BUFFER_SIZE");
    if (upsampledSize > static_cast<int>(mUpsampledL.size())) {
        numFrames = static_cast<int>(mUpsampledL.size()) / osFactor;
        upsampledSize = numFrames * osFactor;
    }

    for (int i = 0; i < numFrames; ++i) {
        // Store dry signal for mix
        mDryL[i] = input[i * 2];
        mDryR[i] = input[i * 2 + 1];

        // Apply pre-filtering (high-pass + tone)
        float preL = mPreHPF_L.process(input[i * 2]);
        float preR = mPreHPF_R.process(input[i * 2 + 1]);

        preL = mPreTone_L.process(preL);
        preR = mPreTone_R.process(preR);

        mInputL[i] = applyGate(preL, gateThreshold);
        mInputR[i] = applyGate(preR, gateThreshold);
    }

    // Upsample
    if (osFactor > 1) {
        mOversamplerL.upsample(mInputL.data(), mUpsampledL.data(), numFrames);
        mOversamplerR.upsample(mInputR.data(), mUpsampledR.data(), numFrames);
    } else {
        std::copy(mInputL.data(), mInputL.data() + numFrames, mUpsampledL.data());
        std::copy(mInputR.data(), mInputR.data() + numFrames, mUpsampledR.data());
    }

    // Apply distortion at upsampled rate
    for (int i = 0; i < upsampledSize; ++i) {
        float sagInputLevel = std::max(std::abs(mUpsampledL[i]), std::abs(mUpsampledR[i]));
        float saggedDrive = std::clamp(drive * applySag(sagInputLevel), 0.0f, 1.0f);
        mProcessedL[i] = applyDistortion(mUpsampledL[i], saggedDrive, algorithm, true);
        mProcessedR[i] = applyDistortion(mUpsampledR[i], saggedDrive, algorithm, false);
    }

    // Downsample
    if (osFactor > 1) {
        mOversamplerL.downsample(mProcessedL.data(), mOutputL.data(), numFrames);
        mOversamplerR.downsample(mProcessedR.data(), mOutputR.data(), numFrames);
    } else {
        std::copy(mProcessedL.data(), mProcessedL.data() + numFrames, mOutputL.data());
        std::copy(mProcessedR.data(), mProcessedR.data() + numFrames, mOutputR.data());
    }

    // Apply post-filtering and mix (with per-sample smoothing for level and mix)
    for (int i = 0; i < numFrames; ++i) {
        // Smooth level and mix per-sample to prevent clicks
        float level = mLevelSmoother.process(levelTarget);
        float mix = mMixSmoother.process(mixTarget);
        float outputGain = level * level * 1.5f;  // Quadratic curve for natural feel

        // Pedal-specific tone stacks, then universal post filtering.
        float postL = applyPedalToneStack(mOutputL[i], algorithm, true);
        float postR = applyPedalToneStack(mOutputR[i], algorithm, false);

        postL = mPostLPF_L.process(postL);
        postR = mPostLPF_R.process(postR);

        postL = mPostTone_L.process(postL);
        postR = mPostTone_R.process(postR);

        // Apply output gain
        postL *= outputGain;
        postR *= outputGain;

        // Mix wet/dry
        output[i * 2] = DSPMath::crossfade(mDryL[i], postL, mix);
        output[i * 2 + 1] = DSPMath::crossfade(mDryR[i], postR, mix);
    }
}

float DistortionEffect::applyDistortion(float input, float drive, int algorithm, bool isLeft) {
    using namespace DistortionVariants;

    switch (algorithm) {
        // ========== OVERDRIVE Pedals ==========
        case TUBE_SCREAMER:
            return processTubeScreamer(input, drive);
        case BOSS_OVERDRIVE:
            return processBossOverdrive(input, drive);
        case KLON:
            return processKlon(input, drive);
        case OCD:
            return processOCD(input, drive);

        // ========== DISTORTION Pedals ==========
        case BOSS_DS1:
            return processBossDS1(input, drive);
        case RAT:
            return processRAT(input, drive, isLeft);
        case DIST_PLUS:
            return processDistortionPlus(input, drive);
        case METAL_ZONE:
            return processMetalZone(input, drive);

        // ========== FUZZ Pedals ==========
        case BIG_MUFF:
            return processBigMuff(input, drive);
        case FUZZ_FACE_GERM:
            return processFuzzFace(input, drive, true);   // Germanium
        case FUZZ_FACE_SI:
            return processFuzzFace(input, drive, false);  // Silicon
        case OCTAVE_FUZZ:
            return processOctaveFuzz(input, drive, isLeft ? mOctavePhaseL : mOctavePhaseR);

        // ========== SPECIAL Pedals ==========
        case HM2_CHAINSAW:
            return processHM2(input, drive);
        case DOOM_FUZZ:
            return processDoomFuzz(input, drive);

        // ========== LEGACY Algorithms (backward compatibility) ==========
        case LEGACY_SOFT_CLIP:
            return processSoftClip(input, drive);
        case LEGACY_HARD_CLIP:
            return processHardClip(input, drive);
        case LEGACY_TUBE_SIM:
            return processTubeSim(input, drive);
        case LEGACY_FOLDBACK:
            return processFoldback(input, drive);
        case LEGACY_BITCRUSH:
            return processBitcrush(input, drive);

        default:
            return processTubeScreamer(input, drive);  // Default to Tube Screamer
    }
}

float DistortionEffect::applyPedalToneStack(float input, int algorithm, bool isLeft) {
    using namespace DistortionVariants;

    switch (algorithm) {
        case TUBE_SCREAMER:
            return isLeft ? mTSMidBoost_L.process(input) : mTSMidBoost_R.process(input);

        case RAT:
            return isLeft ? mRATFilter_L.process(input) : mRATFilter_R.process(input);

        case METAL_ZONE: {
            float x = input;
            if (isLeft) {
                x = mMTLowShelf_L.process(x);
                x = mMTMidPeak_L.process(x);
                x = mMTHighShelf_L.process(x);
            } else {
                x = mMTLowShelf_R.process(x);
                x = mMTMidPeak_R.process(x);
                x = mMTHighShelf_R.process(x);
            }
            return x;
        }

        case BIG_MUFF: {
            float lpf = isLeft ? mMuffToneLPF_L.process(input) : mMuffToneLPF_R.process(input);
            float hpf = isLeft ? mMuffToneHPF_L.process(input) : mMuffToneHPF_R.process(input);
            float tone = mTone.load(std::memory_order_relaxed);
            float x = DSPMath::crossfade(lpf, hpf, tone);
            return isLeft ? mMuffMidScoop_L.process(x) : mMuffMidScoop_R.process(x);
        }

        case HM2_CHAINSAW: {
            float x = input;
            if (isLeft) {
                x = mHM2LowShelf_L.process(x);
                x = mHM2MidScoop_L.process(x);
                x = mHM2HighShelf_L.process(x);
                x = mHM2Presence_L.process(x);
            } else {
                x = mHM2LowShelf_R.process(x);
                x = mHM2MidScoop_R.process(x);
                x = mHM2HighShelf_R.process(x);
                x = mHM2Presence_R.process(x);
            }
            return x;
        }

        default:
            return input;
    }
}

float DistortionEffect::processSoftClip(float input, float drive) {
    // Smooth saturation using tanh
    // Drive increases input gain before saturation
    float gain = 1.0f + drive * 10.0f;  // 1x to 11x gain
    float saturated = std::tanh(input * gain);

    // Normalize to maintain consistent output level
    float tanhMax = std::tanh(gain);
    if (tanhMax > 0.01f) {
        saturated /= tanhMax;
    }

    return saturated;
}

float DistortionEffect::processHardClip(float input, float drive) {
    // Hard clipping with polynomial knee for smoother transition
    float threshold = 1.0f - drive * 0.9f;  // 1.0 to 0.1
    float gain = 1.0f / std::max(threshold, 0.1f);
    float x = input * gain;

    float absX = std::abs(x);
    if (absX < threshold) {
        return x;
    } else {
        // Soft knee using polynomial
        float sign = (x > 0) ? 1.0f : -1.0f;
        float excess = absX - threshold;
        float knee = threshold + (1.0f - threshold) * std::tanh(excess * 3.0f);
        return sign * knee;
    }
}

float DistortionEffect::processTubeSim(float input, float drive) {
    // Asymmetric clipping simulating tube amplifier
    // Positive peaks compress more than negative
    float gain = 1.0f + drive * 8.0f;
    float x = input * gain;

    if (x >= 0) {
        // Softer positive saturation
        return std::tanh(x * 0.7f) * 1.2f;
    } else {
        // Harder negative saturation
        return std::tanh(x * 1.2f) * 0.85f;
    }
}

float DistortionEffect::processFoldback(float input, float drive) {
    // Wave folding - signal folds back when exceeding threshold
    float threshold = 1.0f - drive * 0.7f;  // 1.0 to 0.3
    float gain = 1.0f + drive * 5.0f;
    float x = input * gain;

    // Iterative folding (max 4 iterations to prevent infinite loop)
    for (int i = 0; i < 4; ++i) {
        if (x > threshold) {
            x = 2.0f * threshold - x;
        } else if (x < -threshold) {
            x = -2.0f * threshold - x;
        } else {
            break;
        }
    }

    // Normalize
    return x / std::max(threshold, 0.1f);
}

float DistortionEffect::processBitcrush(float input, float drive) {
    // Bit depth reduction
    // Drive controls bit depth: 16 bits down to 2 bits
    int bits = 16 - static_cast<int>(drive * 14.0f);  // 16 to 2 bits
    bits = std::clamp(bits, 2, 16);

    float levels = std::pow(2.0f, static_cast<float>(bits));
    float quantized = std::round(input * levels) / levels;

    return quantized;
}

// ============================================================================
// OVERDRIVE PEDAL ALGORITHMS
// ============================================================================

float DistortionEffect::processTubeScreamer(float input, float drive) {
    // Ibanez TS-808/TS9 emulation
    // Characteristics: Mid-hump, warm, smooth breakup
    // Uses symmetric diode clipping with input buffer

    // Input gain stage (like the JRC4558 op-amp)
    float gain = 1.0f + drive * 40.0f;  // 1x to 41x gain
    float x = input * gain;

    // Symmetric diode clipping (1N914 diodes)
    x = symmetricDiodeClip(x, 0.35f);

    // Apply mid-boost characteristic (done via filters in updateFilters)
    // The TS has a mid-hump around 720Hz

    // Output level compensation
    float compensation = 1.0f / (1.0f + drive * 2.0f);
    return x * compensation;
}

float DistortionEffect::processBossOverdrive(float input, float drive) {
    // Boss OD-1/SD-1 emulation
    // Characteristics: Bright, articulate, asymmetric clipping

    float gain = 1.0f + drive * 50.0f;
    float x = input * gain;

    // Asymmetric clipping (diode + transistor)
    x = asymmetricDiodeClip(x);

    // The OD-1 is brighter than TS, less mid-focused
    float compensation = 1.0f / (1.0f + drive * 2.5f);
    return x * compensation;
}

float DistortionEffect::processKlon(float input, float drive) {
    // Klon Centaur emulation
    // Characteristics: Transparent, dynamic, clean blend

    float paramB = mParamB.load(std::memory_order_relaxed);  // Clean blend

    // The Klon has dual clipping stages
    float gain = 1.0f + drive * 30.0f;
    float distorted = input * gain;

    // Germanium diode soft clipping
    distorted = std::tanh(distorted * 0.8f);

    // The magic of the Klon: blend clean signal with distorted
    // paramB controls how much clean signal is mixed in
    float cleanBlend = paramB;
    float output = distorted * (1.0f - cleanBlend * 0.7f) + input * cleanBlend * 0.5f;

    float compensation = 1.0f / (1.0f + drive * 1.5f);
    return output * compensation;
}

float DistortionEffect::processOCD(float input, float drive) {
    // Fulltone OCD emulation
    // Characteristics: Amp-like, dynamic, MOSFET clipping

    float gain = 1.0f + drive * 45.0f;
    float x = input * gain;

    // MOSFET-style clipping (softer than diodes)
    // Positive and negative halves clip slightly differently
    if (x > 0) {
        x = std::tanh(x * 0.9f) * 1.1f;
    } else {
        x = std::tanh(x * 1.0f);
    }

    // The OCD responds well to pick dynamics
    float compensation = 1.0f / (1.0f + drive * 2.0f);
    return x * compensation;
}

// ============================================================================
// DISTORTION PEDAL ALGORITHMS
// ============================================================================

float DistortionEffect::processBossDS1(float input, float drive) {
    // Boss DS-1 emulation
    // Characteristics: Aggressive, bright, cutting

    float gain = 1.0f + drive * 100.0f;  // DS-1 has massive gain
    float x = input * gain;

    // Op-amp clipping stage (asymmetric)
    if (x > 0) {
        // Hard clip positive
        x = std::min(x, 1.0f);
    } else {
        // Softer negative (diode in series)
        x = -std::tanh(-x * 0.8f) * 1.2f;
    }

    // DS-1 is known for being bright/harsh at high gain
    float compensation = 0.7f / (1.0f + drive * 3.0f);
    return x * compensation;
}

float DistortionEffect::processRAT(float input, float drive, bool isLeft) {
    // ProCo RAT emulation
    // Characteristics: Gritty, saturated, slew rate limited

    float paramB = mParamB.load(std::memory_order_relaxed);  // Turbo mode
    bool turboMode = paramB > 0.5f;

    // Slew rate limiting (LM308 op-amp characteristic)
    float maxSlew = 0.3f + (1.0f - drive) * 0.7f;
    float& lastSlew = isLeft ? mLastSlewL : mLastSlewR;
    float delta = input - lastSlew;
    delta = std::clamp(delta, -maxSlew, maxSlew);
    lastSlew += delta;
    float slewed = lastSlew;

    // Massive gain
    float gain = 1.0f + drive * 60.0f;
    float x = slewed * gain;

    // Clipping: Silicon diodes or LEDs (turbo mode)
    if (turboMode) {
        x = ledClip(x);  // LEDs = higher headroom, tighter
    } else {
        // Silicon diodes
        const float threshold = 0.6f;
        if (std::abs(x) > threshold) {
            float sign = x > 0 ? 1.0f : -1.0f;
            x = sign * threshold;
        }
    }

    float compensation = turboMode ? 0.6f : 0.8f;
    compensation /= (1.0f + drive * 2.0f);
    return x * compensation;
}

float DistortionEffect::processDistortionPlus(float input, float drive) {
    // MXR Distortion+ emulation
    // Characteristics: Classic, mid-focused, simple circuit

    float gain = 1.0f + drive * 35.0f;
    float x = input * gain;

    // Germanium diode clipping (original) or silicon (reissue)
    // Using softer germanium-style
    const float threshold = 0.3f;
    if (std::abs(x) < threshold) {
        // Linear region
    } else {
        float sign = x > 0 ? 1.0f : -1.0f;
        float excess = std::abs(x) - threshold;
        x = sign * (threshold + std::tanh(excess * 2.0f) * 0.4f);
    }

    float compensation = 1.0f / (1.0f + drive * 1.8f);
    return x * compensation;
}

float DistortionEffect::processMetalZone(float input, float drive) {
    // Boss MT-2 Metal Zone emulation
    // Characteristics: High-gain, scooped mids, parametric EQ

    // Triple gain stage
    float gain1 = 1.0f + drive * 20.0f;
    float gain2 = 1.0f + drive * 15.0f;
    float gain3 = 1.0f + drive * 10.0f;

    float x = input;

    // Stage 1
    x = x * gain1;
    x = std::tanh(x * 0.8f);

    // Stage 2
    x = x * gain2;
    x = std::tanh(x * 0.7f);

    // Stage 3
    x = x * gain3;
    x = std::tanh(x * 0.6f);

    // The MT-2 has built-in EQ but that's handled in updateFilters
    float compensation = 0.4f / (1.0f + drive * 2.0f);
    return x * compensation;
}

// ============================================================================
// FUZZ PEDAL ALGORITHMS
// ============================================================================

float DistortionEffect::processBigMuff(float input, float drive) {
    // Electro-Harmonix Big Muff Pi emulation
    // Characteristics: Massive sustain, creamy, mid-scoop

    float paramA = mParamA.load(std::memory_order_relaxed);  // Sustain
    float sustain = 0.5f + paramA * 0.5f;

    // Four cascaded gain stages (like the real Big Muff)
    float stage1Gain = 1.0f + drive * 10.0f * sustain;
    float stage2Gain = 1.0f + drive * 8.0f * sustain;
    float stage3Gain = 1.0f + drive * 6.0f * sustain;
    float stage4Gain = 1.0f + drive * 4.0f * sustain;

    float x = input;

    // Stage 1 - soft asymmetric
    x = x * stage1Gain;
    if (x > 0) {
        x = std::tanh(x * 1.2f);
    } else {
        x = std::tanh(x * 0.9f) * 1.1f;
    }

    // Stage 2
    x = x * stage2Gain;
    if (x > 0) {
        x = std::tanh(x * 1.1f);
    } else {
        x = std::tanh(x * 0.85f) * 1.15f;
    }

    // Stage 3
    x = x * stage3Gain;
    x = std::tanh(x);

    // Stage 4 - final saturation
    x = x * stage4Gain;
    x = std::tanh(x * 0.9f);

    // Big Muff has tons of sustain, relatively consistent output
    return x * 0.7f;
}

float DistortionEffect::processFuzzFace(float input, float drive, bool germanium) {
    // Dallas Arbiter Fuzz Face emulation
    // Characteristics: Organic, responsive to guitar volume

    float bias = mBias.load(std::memory_order_relaxed);
    float paramB = mParamB.load(std::memory_order_relaxed);  // Cleanup response

    // Transistor characteristics differ between Ge and Si
    float vbe = germanium ? 0.2f : 0.6f;      // Base-emitter voltage
    float hfe = germanium ? 70.0f : 150.0f;   // Current gain

    // Bias affects the operating point
    float biasPoint = bias * 0.5f + 0.25f;

    // First transistor stage (input)
    float fuzz = drive * 0.9f + 0.1f;
    float q1_input = input + biasPoint;
    float q1_output = transistorSaturate(q1_input, vbe, hfe * 0.8f);

    // Second transistor stage (more gain)
    float q2_input = q1_output * (1.0f + fuzz * 20.0f);
    float q2_output = transistorSaturate(q2_input, vbe, hfe);

    // Cleanup response (how fuzz responds to guitar volume roll-off)
    float cleanup = paramB;
    float dynamicGain = 1.0f - cleanup * (1.0f - std::abs(input) * 2.0f);
    dynamicGain = std::clamp(dynamicGain, 0.3f, 1.0f);

    float output = q2_output * dynamicGain;

    // Germanium is warmer, silicon is brighter and more aggressive
    float compensation = germanium ? 0.8f : 0.7f;
    return output * compensation;
}

float DistortionEffect::processOctaveFuzz(float input, float drive, float& phase) {
    // Octavia/Roger Mayer Octave Fuzz emulation
    // Characteristics: Ring-mod style octave-up, bell tones

    float gain = 1.0f + drive * 40.0f;
    float x = input * gain;

    // Full-wave rectification creates octave-up effect
    float rectified = std::abs(x);

    // Ring modulation component for the characteristic "bell" tone
    // Use the input signal's zero crossings to create octave effect
    phase += (input > 0 ? 0.01f : -0.01f);
    if (phase > 1.0f) phase = -1.0f;
    if (phase < -1.0f) phase = 1.0f;

    // Mix rectified (octave) with original fuzz
    float fuzzed = std::tanh(x * 0.7f);
    float octave = std::tanh(rectified * 0.6f) * (0.5f + drive * 0.5f);

    float output = fuzzed * 0.6f + octave * 0.5f;

    return output * 0.8f;
}

// ============================================================================
// SPECIAL/EXTREME PEDAL ALGORITHMS
// ============================================================================

float DistortionEffect::processHM2(float input, float drive) {
    // Boss HM-2 Heavy Metal emulation
    // Characteristics: Swedish death metal "chainsaw" tone

    // The HM-2 is basically gain stages into very aggressive EQ
    float gain1 = 1.0f + drive * 30.0f;
    float gain2 = 1.0f + drive * 20.0f;
    float gain3 = 1.0f + drive * 10.0f;

    float x = input;

    // Stage 1 - hard clip
    x = x * gain1;
    x = std::clamp(x, -1.0f, 1.0f);

    // Stage 2 - more clipping
    x = x * gain2;
    x = std::clamp(x, -1.0f, 1.0f);

    // Stage 3 - final saturation
    x = x * gain3;
    x = std::tanh(x * 1.5f);

    // The "chainsaw" sound comes from the EQ (handled in updateFilters):
    // - Boosted lows around 80Hz
    // - Scooped mids around 800Hz
    // - Boosted highs around 3kHz
    // - Presence peak around 5kHz

    return x * 0.5f;
}

float DistortionEffect::processDoomFuzz(float input, float drive) {
    // Sunn Model T / Acapulco Gold style emulation
    // Characteristics: Massive, crushing, endless sustain

    // This is essentially a cranked tube amp simulation
    float gain = 1.0f + drive * 80.0f;  // MASSIVE gain

    float x = input * gain;

    // Multiple saturation stages (like a cranked Sunn amp)
    x = std::tanh(x * 0.5f);
    x = x * 3.0f;
    x = std::tanh(x * 0.6f);
    x = x * 2.5f;
    x = std::tanh(x * 0.7f);
    x = x * 2.0f;
    x = std::tanh(x * 0.8f);

    // Doom fuzz has lots of low end, dark character
    // The EQ shaping is handled in updateFilters

    return x * 0.6f;
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

float DistortionEffect::symmetricDiodeClip(float input, float threshold) {
    // Symmetric diode clipping (like TS-808 with 1N914 diodes)
    if (std::abs(input) < threshold) {
        return input;
    }
    float sign = input > 0 ? 1.0f : -1.0f;
    return sign * (threshold + std::tanh((std::abs(input) - threshold) * 2.0f) * 0.3f);
}

float DistortionEffect::asymmetricDiodeClip(float input) {
    // Asymmetric clipping (different positive/negative behavior)
    if (input > 0) {
        // Harder positive clipping
        return std::min(input, 1.0f);
    } else {
        // Softer negative clipping
        return -std::tanh(-input * 0.8f) * 1.2f;
    }
}

float DistortionEffect::ledClip(float input) {
    // LED clipping (higher threshold ~1.5V, like RAT turbo)
    const float threshold = 1.5f;
    return std::tanh(input / threshold) * threshold * 0.67f;
}

float DistortionEffect::transistorSaturate(float input, float vbe, float hfe) {
    // Simplified BJT transistor saturation model
    float baseVoltage = input - vbe;
    if (baseVoltage < 0) {
        return 0.0f;  // Cutoff region
    }
    float collector = baseVoltage * hfe;
    // Soft saturation
    return std::tanh(collector * 0.1f) * 10.0f;
}

float DistortionEffect::applySag(float inputLevel) {
    // Voltage sag simulation (for tube-like behavior)
    float sagAmount = mSag.load(std::memory_order_relaxed);
    if (sagAmount <= 0.0f) {
        return 1.0f;
    }

    // Loud input pulls the virtual supply down; release is intentionally slower
    // than attack for a guitar-like compression feel.
    float level = std::clamp(inputLevel, 0.0f, 2.0f);
    float targetVoltage = 1.0f - sagAmount * std::min(level * 0.35f, 0.35f);
    float coeff = (targetVoltage < mSagVoltage) ? 0.995f : 0.9995f;
    mSagVoltage = coeff * mSagVoltage + (1.0f - coeff) * targetVoltage;
    return std::clamp(mSagVoltage, 0.65f, 1.0f);
}

float DistortionEffect::applyGate(float input, float threshold) {
    // Simple noise gate
    if (threshold <= 0.0f) {
        return input;
    }
    float absInput = std::abs(input);
    if (absInput < threshold * 0.1f) {
        return 0.0f;
    }
    return input;
}

// ============================================================================
// FILTER CONFIGURATION
// ============================================================================

void DistortionEffect::updateFilters() {
    float preLowCut = mPreLowCut.load(std::memory_order_relaxed);
    float postHighCut = mPostHighCut.load(std::memory_order_relaxed);
    int algorithm = mAlgorithm.load(std::memory_order_relaxed);

    // Universal pre-distortion high-pass (remove low rumble)
    mPreHPF_L.setHighpass(preLowCut, 0.707f);
    mPreHPF_R.setHighpass(preLowCut, 0.707f);

    // Universal post-distortion low-pass
    mPostLPF_L.setLowpass(postHighCut, 0.707f);
    mPostLPF_R.setLowpass(postHighCut, 0.707f);

    // Configure pedal-specific filters
    updateFiltersForPedal(algorithm);
}

void DistortionEffect::updateFiltersForPedal(int algorithm) {
    using namespace DistortionVariants;

    float tone = mTone.load(std::memory_order_relaxed);
    float paramA = mParamA.load(std::memory_order_relaxed);
    float paramB = mParamB.load(std::memory_order_relaxed);
    float paramC = mParamC.load(std::memory_order_relaxed);

    switch (algorithm) {
        // ========== TUBE SCREAMER ==========
        case TUBE_SCREAMER: {
            // The TS-808 has a characteristic mid-hump around 720Hz
            // PARAM_A controls mid frequency (520-920 Hz)
            // PARAM_B controls mid Q (0.5-1.0)
            float midFreq = 520.0f + paramA * 400.0f;
            float midQ = 0.5f + paramB * 0.5f;
            float midBoostDb = 6.0f + tone * 3.0f;  // 6-9 dB boost

            mTSMidBoost_L.setPeaking(midFreq, midQ, midBoostDb);
            mTSMidBoost_R.setPeaking(midFreq, midQ, midBoostDb);

            // TS has a relatively dark tone control
            float toneFreq = 700.0f + tone * 4300.0f;  // 700Hz - 5kHz
            mPreTone_L.setLowpass(toneFreq, 0.707f);
            mPreTone_R.setLowpass(toneFreq, 0.707f);

            // Post tone - slight presence
            float postGainDb = (tone - 0.5f) * 4.0f;
            mPostTone_L.setPeaking(2000.0f, 1.0f, postGainDb);
            mPostTone_R.setPeaking(2000.0f, 1.0f, postGainDb);
            break;
        }

        // ========== BOSS OVERDRIVE ==========
        case BOSS_OVERDRIVE: {
            // OD-1 is brighter than TS, less mid-focused
            float toneFreq = 1000.0f + tone * 5000.0f;
            mPreTone_L.setLowpass(toneFreq, 0.707f);
            mPreTone_R.setLowpass(toneFreq, 0.707f);

            // Slight high boost for brightness
            float highBoostDb = tone * 4.0f;
            mPostTone_L.setHighShelf(3000.0f, 0.707f, highBoostDb);
            mPostTone_R.setHighShelf(3000.0f, 0.707f, highBoostDb);
            break;
        }

        // ========== KLON ==========
        case KLON: {
            // Klon is very transparent, minimal coloring
            // PARAM_A = treble boost
            float trebleBoost = paramA * 6.0f;  // 0-6 dB
            mPreTone_L.setHighShelf(2000.0f, 0.707f, trebleBoost);
            mPreTone_R.setHighShelf(2000.0f, 0.707f, trebleBoost);

            // Tone control affects high frequencies
            float toneFreq = 2000.0f + tone * 6000.0f;
            mPostTone_L.setLowpass(toneFreq, 0.707f);
            mPostTone_R.setLowpass(toneFreq, 0.707f);
            break;
        }

        // ========== OCD ==========
        case OCD: {
            // OCD has a versatile tone control
            float toneFreq = 800.0f + tone * 7000.0f;
            mPreTone_L.setLowpass(toneFreq, 0.707f);
            mPreTone_R.setLowpass(toneFreq, 0.707f);

            // Slight mid presence
            float midBoostDb = (tone - 0.5f) * 4.0f;
            mPostTone_L.setPeaking(1500.0f, 1.5f, midBoostDb);
            mPostTone_R.setPeaking(1500.0f, 1.5f, midBoostDb);
            break;
        }

        // ========== BOSS DS-1 ==========
        case BOSS_DS1: {
            // DS-1 has an active tone stack, can get very bright
            float toneFreq = 100.0f + tone * 900.0f;  // HPF: 100Hz-1kHz
            mPreTone_L.setHighpass(toneFreq, 0.707f);
            mPreTone_R.setHighpass(toneFreq, 0.707f);

            // High presence boost
            float presenceDb = tone * 6.0f;
            mPostTone_L.setPeaking(1200.0f, 2.0f, presenceDb);
            mPostTone_R.setPeaking(1200.0f, 2.0f, presenceDb);
            break;
        }

        // ========== RAT ==========
        case RAT: {
            // RAT has a simple LPF that acts as the filter control
            // PARAM_A = filter cutoff (dark to bright)
            float filterFreq = 800.0f + paramA * 7200.0f;  // 800Hz - 8kHz
            mRATFilter_L.setLowpass(filterFreq, 0.707f);
            mRATFilter_R.setLowpass(filterFreq, 0.707f);

            // Tone control adds/cuts presence
            float toneDb = (tone - 0.5f) * 6.0f;
            mPreTone_L.setPeaking(2000.0f, 1.0f, toneDb);
            mPreTone_R.setPeaking(2000.0f, 1.0f, toneDb);
            mPostTone_L.setPeaking(3500.0f, 1.0f, toneDb * 0.5f);
            mPostTone_R.setPeaking(3500.0f, 1.0f, toneDb * 0.5f);
            break;
        }

        // ========== DISTORTION+ ==========
        case DIST_PLUS: {
            // Simple tone stack
            float toneFreq = 1000.0f + tone * 4000.0f;
            mPreTone_L.setLowpass(toneFreq, 0.707f);
            mPreTone_R.setLowpass(toneFreq, 0.707f);
            mPostTone_L.setLowpass(8000.0f, 0.707f);
            mPostTone_R.setLowpass(8000.0f, 0.707f);
            break;
        }

        // ========== METAL ZONE ==========
        case METAL_ZONE: {
            // MT-2 has parametric EQ
            // PARAM_A = Low EQ, PARAM_B = High EQ, PARAM_C = Mid freq
            float lowGainDb = (paramA - 0.5f) * 24.0f;  // +/- 12dB
            float highGainDb = (paramB - 0.5f) * 24.0f;
            float midFreq = 200.0f + paramC * 2800.0f;  // 200Hz - 3kHz

            mMTLowShelf_L.setLowShelf(100.0f, 0.707f, lowGainDb);
            mMTLowShelf_R.setLowShelf(100.0f, 0.707f, lowGainDb);
            mMTHighShelf_L.setHighShelf(4000.0f, 0.707f, highGainDb);
            mMTHighShelf_R.setHighShelf(4000.0f, 0.707f, highGainDb);

            // Mid scoop (tone controls depth)
            float midScoopDb = -(1.0f - tone) * 6.0f;
            mMTMidPeak_L.setPeaking(midFreq, 1.5f, midScoopDb);
            mMTMidPeak_R.setPeaking(midFreq, 1.5f, midScoopDb);

            // Generic tone filters pass-through
            mPreTone_L.setHighpass(50.0f, 0.707f);
            mPreTone_R.setHighpass(50.0f, 0.707f);
            mPostTone_L.setLowpass(10000.0f, 0.707f);
            mPostTone_R.setLowpass(10000.0f, 0.707f);
            break;
        }

        // ========== BIG MUFF ==========
        case BIG_MUFF: {
            // Big Muff has a distinctive mid-scoop tone control
            // PARAM_B controls mid scoop depth
            float midScoopDepth = paramB * 0.7f;

            // Tone control blends between LPF and HPF
            float lpfFreq = 1000.0f + tone * 4000.0f;
            float hpfFreq = 200.0f + (1.0f - tone) * 300.0f;

            mMuffToneLPF_L.setLowpass(lpfFreq, 0.707f);
            mMuffToneLPF_R.setLowpass(lpfFreq, 0.707f);
            mMuffToneHPF_L.setHighpass(hpfFreq, 0.707f);
            mMuffToneHPF_R.setHighpass(hpfFreq, 0.707f);

            // Mid scoop notch around 1kHz
            float midCutDb = -midScoopDepth * 12.0f;  // Up to -8.4dB cut
            mMuffMidScoop_L.setPeaking(1000.0f, 1.5f, midCutDb);
            mMuffMidScoop_R.setPeaking(1000.0f, 1.5f, midCutDb);

            // Generic filters
            mPreTone_L.setHighpass(80.0f, 0.707f);
            mPreTone_R.setHighpass(80.0f, 0.707f);
            mPostTone_L.setLowpass(6000.0f, 0.707f);
            mPostTone_R.setLowpass(6000.0f, 0.707f);
            break;
        }

        // ========== FUZZ FACE (Ge/Si) ==========
        case FUZZ_FACE_GERM:
        case FUZZ_FACE_SI: {
            // Fuzz Face has a simple tone control
            float toneFreq = 200.0f + tone * 2000.0f;
            mPreTone_L.setHighpass(toneFreq, 0.707f);
            mPreTone_R.setHighpass(toneFreq, 0.707f);

            // Germanium is darker than silicon
            float postFreq = (algorithm == FUZZ_FACE_GERM) ? 4000.0f : 6000.0f;
            mPostTone_L.setLowpass(postFreq + tone * 4000.0f, 0.707f);
            mPostTone_R.setLowpass(postFreq + tone * 4000.0f, 0.707f);
            break;
        }

        // ========== OCTAVE FUZZ ==========
        case OCTAVE_FUZZ: {
            // Octave fuzz benefits from some high-frequency content
            float toneFreq = 500.0f + tone * 3500.0f;
            mPreTone_L.setHighpass(toneFreq * 0.5f, 0.707f);
            mPreTone_R.setHighpass(toneFreq * 0.5f, 0.707f);
            mPostTone_L.setLowpass(toneFreq * 2.0f, 0.707f);
            mPostTone_R.setLowpass(toneFreq * 2.0f, 0.707f);
            break;
        }

        // ========== HM-2 CHAINSAW ==========
        case HM2_CHAINSAW: {
            // The HM-2 "chainsaw" sound comes from extreme EQ
            // PARAM_A = Low boost, PARAM_B = High boost
            float lowBoostDb = paramA * 12.0f;    // 0-12 dB
            float highBoostDb = paramB * 12.0f;   // 0-12 dB

            // Low boost at 80Hz
            mHM2LowShelf_L.setLowShelf(80.0f, 0.707f, lowBoostDb);
            mHM2LowShelf_R.setLowShelf(80.0f, 0.707f, lowBoostDb);

            // High boost at 3kHz
            mHM2HighShelf_L.setHighShelf(3000.0f, 0.707f, highBoostDb);
            mHM2HighShelf_R.setHighShelf(3000.0f, 0.707f, highBoostDb);

            // Mid scoop at 800Hz (always present)
            mHM2MidScoop_L.setPeaking(800.0f, 2.0f, -6.0f);
            mHM2MidScoop_R.setPeaking(800.0f, 2.0f, -6.0f);

            // Presence peak at 5kHz
            float presenceDb = tone * 6.0f;
            mHM2Presence_L.setPeaking(5000.0f, 2.0f, presenceDb);
            mHM2Presence_R.setPeaking(5000.0f, 2.0f, presenceDb);

            // Generic filters
            mPreTone_L.setHighpass(40.0f, 0.707f);
            mPreTone_R.setHighpass(40.0f, 0.707f);
            mPostTone_L.setLowpass(12000.0f, 0.707f);
            mPostTone_R.setLowpass(12000.0f, 0.707f);
            break;
        }

        // ========== DOOM FUZZ ==========
        case DOOM_FUZZ: {
            // Doom fuzz is dark with massive low end
            float toneFreq = 500.0f + tone * 2500.0f;
            mPreTone_L.setLowpass(toneFreq, 0.707f);
            mPreTone_R.setLowpass(toneFreq, 0.707f);

            // Boost low end
            mPostTone_L.setLowShelf(100.0f, 0.707f, 4.0f);
            mPostTone_R.setLowShelf(100.0f, 0.707f, 4.0f);
            break;
        }

        // ========== LEGACY ALGORITHMS ==========
        case LEGACY_SOFT_CLIP:
        case LEGACY_HARD_CLIP:
        case LEGACY_TUBE_SIM:
        case LEGACY_FOLDBACK:
        case LEGACY_BITCRUSH:
        default: {
            // Use original generic tone stack for legacy compatibility
            float preGainDb = (tone - 0.5f) * 12.0f;
            float preFreq = 1000.0f + tone * 3000.0f;
            mPreTone_L.setHighShelf(preFreq, 0.707f, preGainDb);
            mPreTone_R.setHighShelf(preFreq, 0.707f, preGainDb);

            float postGainDb = (tone - 0.5f) * 6.0f;
            mPostTone_L.setPeaking(2500.0f, 1.0f, postGainDb);
            mPostTone_R.setPeaking(2500.0f, 1.0f, postGainDb);
            break;
        }
    }
}
