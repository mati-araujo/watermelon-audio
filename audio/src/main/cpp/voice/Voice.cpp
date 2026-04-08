#include "Voice.h"
#include "../oscillators/Oscillators.h"
#include "../platform/Logger.h"
#include <cstring>
#include <algorithm>

#define LOG_TAG "Voice"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

namespace voice {

// ==================== CONSTRUCTOR ====================

Voice::Voice() {
    createOscillators();
}

// ==================== LIFECYCLE ====================

void Voice::prepare(int sampleRate, int maxBlockSize) {
    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;

    // Configure all oscillators
    for (auto& osc : mOscillators) {
        if (osc) {
            osc->setSampleRate(sampleRate);
        }
    }

    // Configure parameter smoothers (5ms for freq, 10ms for amp/pan/pressure)
    mFreqSmoother.setSmoothingTime(5.0f, static_cast<float>(sampleRate));
    mAmpSmoother.setSmoothingTime(10.0f, static_cast<float>(sampleRate));
    mPanSmoother.setSmoothingTime(10.0f, static_cast<float>(sampleRate));
    mPressureSmoother.setSmoothingTime(10.0f, static_cast<float>(sampleRate));

    // Pre-allocate oscillator buffer (stereo)
    mOscBuffer.resize(maxBlockSize * 2, 0.0f);

    // Initialize voice filter
    mFilter.prepare(static_cast<float>(sampleRate));
    mCutoffSmoother.setSmoothingTime(5.0f, static_cast<float>(sampleRate));

    // Calculate envelope rates
    calculateEnvelopeRates();
}

void Voice::reset() {
    mState.store(static_cast<int>(VoiceState::IDLE), std::memory_order_release);
    mEnvelopeLevel.store(0.0f, std::memory_order_release);
    mEnvelopePhase = 0.0f;
    mSourceId.store(-1, std::memory_order_release);
    mNoteId.store(-1, std::memory_order_release);
    mStartTime.store(0, std::memory_order_release);

    mFreqSmoother.reset(440.0f);
    mAmpSmoother.reset(0.0f);
    mPanSmoother.reset(0.5f);
}

// ==================== NOTE CONTROL ====================

void Voice::noteOn(const VoiceParams& params, uint64_t startTime) {
    // Store parameters
    mFrequency.store(params.frequency, std::memory_order_release);
    mAmplitude.store(params.amplitude, std::memory_order_release);
    mPan.store(params.pan, std::memory_order_release);
    mPressure.store(params.pressure, std::memory_order_release);
    mCurrentOscType.store(params.oscillatorType, std::memory_order_release);
    mSourceId.store(params.sourceId, std::memory_order_release);
    mNoteId.store(params.noteId, std::memory_order_release);
    mStartTime.store(startTime, std::memory_order_release);

    // Reset smoothers to current values for immediate response
    mFreqSmoother.reset(params.frequency);
    // Don't reset amp smoother - let it ramp up via envelope

    // Transition to ATTACK state
    mState.store(static_cast<int>(VoiceState::ATTACK), std::memory_order_release);
}

void Voice::noteOff() {
    VoiceState currentState = getState();

    // Only transition if currently playing (Attack, Decay, or Sustain)
    if (currentState == VoiceState::ATTACK || currentState == VoiceState::DECAY || currentState == VoiceState::SUSTAIN) {
        mState.store(static_cast<int>(VoiceState::RELEASE), std::memory_order_release);
    }
}

void Voice::steal() {
    VoiceState currentState = getState();

    // Only steal if not already idle
    if (currentState != VoiceState::IDLE) {
        mState.store(static_cast<int>(VoiceState::STEALING), std::memory_order_release);
    }
}

// ==================== REAL-TIME PARAMETER UPDATES ====================

void Voice::setFrequency(float freq) {
    mFrequency.store(freq, std::memory_order_release);
}

void Voice::setAmplitude(float amp) {
    mAmplitude.store(amp, std::memory_order_release);
}

void Voice::setPan(float pan) {
    mPan.store(pan, std::memory_order_release);
}

void Voice::setPressure(float pressure) {
    mPressure.store(pressure, std::memory_order_release);
}

void Voice::setOscillatorType(int type) {
    if (type >= 0 && type < static_cast<int>(mOscillators.size())) {
        mCurrentOscType.store(type, std::memory_order_release);
    }
}

void Voice::setEngine(SynthEngine* engine) {
    mEngine.store(engine, std::memory_order_release);
}

// ==================== RENDERING ====================

void Voice::render(float* buffer, int numFrames) {
    VoiceState state = getState();

    // If idle, output silence
    if (state == VoiceState::IDLE) {
        std::memset(buffer, 0, numFrames * 2 * sizeof(float));
        return;
    }

    // Get target parameters
    float targetFreq = mFrequency.load(std::memory_order_acquire);
    float targetAmp = mAmplitude.load(std::memory_order_acquire);
    float targetPan = mPan.load(std::memory_order_acquire);
    float targetPressure = mPressure.load(std::memory_order_acquire);

    float smoothedFreq = mFreqSmoother.process(targetFreq);

    // ========== ENGINE DISPATCH (Phase 6) ==========
    SynthEngine* engine = mEngine.load(std::memory_order_acquire);

    if (engine) {
        // Non-classic engine: use SynthEngine
        engine->process(mOscBuffer.data(), numFrames, smoothedFreq, 1.0f);
    } else {
        // Classic engine: use AudioSource oscillator
        int oscType = mCurrentOscType.load(std::memory_order_acquire);
        if (oscType < 0 || oscType >= static_cast<int>(mOscillators.size()) || !mOscillators[oscType]) {
            std::memset(buffer, 0, numFrames * 2 * sizeof(float));
            return;
        }
        mOscillators[oscType]->setParameters(smoothedFreq, 1.0f);
        mOscillators[oscType]->render(mOscBuffer.data(), numFrames);
    }

    // ========== PER-VOICE FILTER (Phase 6) ==========
    if (mFilterEnabled.load(std::memory_order_acquire)) {
        // Update filter params with smoothing
        float cutoff = mCutoffSmoother.process(mFilterCutoff.load(std::memory_order_relaxed));
        float resonance = mFilterResonance.load(std::memory_order_relaxed);
        int filterMode = mFilterMode.load(std::memory_order_relaxed);

        mFilter.setCutoff(cutoff);
        mFilter.setResonance(resonance);
        mFilter.setMode(static_cast<StateVariableFilter::Mode>(filterMode));
        mFilter.processBlock(mOscBuffer.data(), numFrames);
    }

    // FIX: Per-sample envelope interpolation to prevent staircase clicks
    float envLevelStart = mEnvelopeLevel.load(std::memory_order_acquire);
    updateEnvelope(numFrames);
    float envLevelEnd = mEnvelopeLevel.load(std::memory_order_acquire);

    // Apply envelope, amplitude, pressure, and panning
    float smoothedAmp = mAmpSmoother.process(targetAmp);
    float smoothedPan = mPanSmoother.process(targetPan);

    // Calculate stereo gains from pan (equal-power panning)
    float panRadians = smoothedPan * M_PI * 0.5f;
    float leftGain = cosf(panRadians);
    float rightGain = sinf(panRadians);

    // Combined gain components (without envelope, which is interpolated per-sample)
    float smoothedPressure = mPressureSmoother.process(targetPressure);
    float baseGain = smoothedAmp * smoothedPressure;

    // Apply gains to output buffer with per-sample envelope interpolation
    float envStep = (numFrames > 1) ? (envLevelEnd - envLevelStart) / static_cast<float>(numFrames) : 0.0f;
    float envLevel = envLevelStart;

    for (int i = 0; i < numFrames; ++i) {
        // Source is mono (duplicated stereo from oscillator)
        float monoSample = mOscBuffer[i * 2];  // Take left channel as mono
        float gain = envLevel * baseGain;

        buffer[i * 2] = monoSample * gain * leftGain;       // Left
        buffer[i * 2 + 1] = monoSample * gain * rightGain;  // Right

        envLevel += envStep;
    }

    // Check if we should transition to IDLE after release/stealing
    state = getState();
    if ((state == VoiceState::RELEASE || state == VoiceState::STEALING) && envLevelEnd <= 0.0001f) {
        mState.store(static_cast<int>(VoiceState::IDLE), std::memory_order_release);
        mEnvelopeLevel.store(0.0f, std::memory_order_release);
        mSourceId.store(-1, std::memory_order_release);
        mNoteId.store(-1, std::memory_order_release);
    }
}

// ==================== CONFIGURATION ====================

void Voice::setAttackTime(float ms) {
    mAttackTimeMs = ms;
    calculateEnvelopeRates();
}

void Voice::setDecayTime(float ms) {
    mDecayTimeMs = ms;
    calculateEnvelopeRates();
}

void Voice::setSustainLevel(float level) {
    mSustainLevelConfig = std::clamp(level, 0.0f, 1.0f);
    mSustainLevel.store(mSustainLevelConfig, std::memory_order_release);
}

void Voice::setReleaseTime(float ms) {
    mReleaseTimeMs = ms;
    calculateEnvelopeRates();
}

void Voice::setStealReleaseTime(float ms) {
    mStealReleaseTimeMs = ms;
    calculateEnvelopeRates();
}

// ==================== VOICE FILTER (Phase 6) ====================

void Voice::setFilterEnabled(bool enabled) {
    mFilterEnabled.store(enabled, std::memory_order_release);
}

void Voice::setFilterCutoff(float hz) {
    mFilterCutoff.store(std::clamp(hz, 20.0f, 20000.0f), std::memory_order_release);
}

void Voice::setFilterResonance(float q) {
    mFilterResonance.store(std::clamp(q, 0.0f, 1.0f), std::memory_order_release);
}

void Voice::setFilterMode(int mode) {
    if (mode >= 0 && mode <= 2) {
        mFilterMode.store(mode, std::memory_order_release);
    }
}

// ==================== INTERNAL METHODS ====================

void Voice::createOscillators() {
    // Create one oscillator of each type (same order as AudioEngine)
    mOscillators.reserve(5);
    mOscillators.push_back(std::make_unique<SineOscillator>());      // 0
    mOscillators.push_back(std::make_unique<SquareOscillator>());    // 1
    mOscillators.push_back(std::make_unique<SawtoothOscillator>());  // 2
    mOscillators.push_back(std::make_unique<TriangleOscillator>()); // 3
    mOscillators.push_back(std::make_unique<BandLimitedNoiseGenerator>()); // 4
}

void Voice::calculateEnvelopeRates() {
    if (mSampleRate <= 0) return;

    // Attack rate: reach 1.0 in attackTimeMs
    float attackSamples = (mAttackTimeMs / 1000.0f) * mSampleRate;
    mAttackRate.store(attackSamples > 0 ? 1.0f / attackSamples : 1.0f, std::memory_order_release);

    // Decay rate: fall from 1.0 to sustain level in decayTimeMs
    // Rate = (1.0 - sustainLevel) / decaySamples
    float decaySamples = (mDecayTimeMs / 1000.0f) * mSampleRate;
    float decayRange = 1.0f - mSustainLevelConfig;
    mDecayRate.store(decaySamples > 0 && decayRange > 0 ? decayRange / decaySamples : 1.0f, std::memory_order_release);

    // Sustain level
    mSustainLevel.store(mSustainLevelConfig, std::memory_order_release);

    // Release rate: reach 0.0 from sustain level in releaseTimeMs
    float releaseSamples = (mReleaseTimeMs / 1000.0f) * mSampleRate;
    mReleaseRate.store(releaseSamples > 0 ? mSustainLevelConfig / releaseSamples : 1.0f, std::memory_order_release);

    // Steal release rate: fast release
    float stealSamples = (mStealReleaseTimeMs / 1000.0f) * mSampleRate;
    mStealReleaseRate.store(stealSamples > 0 ? 1.0f / stealSamples : 1.0f, std::memory_order_release);
}

void Voice::updateEnvelope(int numFrames) {
    VoiceState state = getState();
    float envLevel = mEnvelopeLevel.load(std::memory_order_acquire);

    float attackRate = mAttackRate.load(std::memory_order_acquire);
    float decayRate = mDecayRate.load(std::memory_order_acquire);
    float sustainLevel = mSustainLevel.load(std::memory_order_acquire);
    float releaseRate = mReleaseRate.load(std::memory_order_acquire);
    float stealRate = mStealReleaseRate.load(std::memory_order_acquire);

    switch (state) {
        case VoiceState::ATTACK:
            // Ramp up: 0 → 1.0 (peak)
            envLevel += attackRate * numFrames;
            if (envLevel >= 1.0f) {
                envLevel = 1.0f;
                // Transition to DECAY (or skip to SUSTAIN if sustain == 1.0)
                if (sustainLevel >= 0.999f) {
                    mState.store(static_cast<int>(VoiceState::SUSTAIN), std::memory_order_release);
                } else {
                    mState.store(static_cast<int>(VoiceState::DECAY), std::memory_order_release);
                }
            }
            break;

        case VoiceState::DECAY:
            // Ramp down: 1.0 → sustainLevel
            envLevel -= decayRate * numFrames;
            if (envLevel <= sustainLevel) {
                envLevel = sustainLevel;
                mState.store(static_cast<int>(VoiceState::SUSTAIN), std::memory_order_release);
            }
            break;

        case VoiceState::SUSTAIN:
            // Hold at sustain level
            envLevel = sustainLevel;
            break;

        case VoiceState::RELEASE:
            // Ramp down: current → 0
            envLevel -= releaseRate * numFrames;
            if (envLevel <= 0.0f) {
                envLevel = 0.0f;
            }
            break;

        case VoiceState::STEALING:
            // Fast ramp down
            envLevel -= stealRate * numFrames;
            if (envLevel <= 0.0f) {
                envLevel = 0.0f;
            }
            break;

        case VoiceState::IDLE:
        default:
            envLevel = 0.0f;
            break;
    }

    mEnvelopeLevel.store(envLevel, std::memory_order_release);
}

void Voice::applyPanning(float* buffer, int numFrames) {
    // This is now integrated into render() for efficiency
    // Kept as placeholder for future enhancements
}

} // namespace voice
