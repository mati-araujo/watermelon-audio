#include "LFO.h"
#include <random>

LFO::LFO(float sampleRate, float initialRate)
    : mSampleRate(sampleRate) {
    setRate(initialRate);
    mRandomValue = randomFloat();
    mRandomPrev = randomFloat();
    mRandomNext = randomFloat();
}

void LFO::setSampleRate(float sampleRate) {
    if (sampleRate <= 0.0f) {
        return;
    }

    mSampleRate = sampleRate;
    updatePhaseIncrement();
}

void LFO::setRate(float rateHz) {
    // Clamp to reasonable LFO range (0.01 Hz to 50 Hz)
    rateHz = std::clamp(rateHz, 0.01f, 50.0f);
    mRateHz.store(rateHz, std::memory_order_release);
    updatePhaseIncrement();
}

void LFO::setWaveform(Waveform waveform) {
    mWaveform = waveform;
}

void LFO::reset() {
    mPhase = 0.0f;
    mRandomPhase = 0.0f;
    mRandomValue = randomFloat();
    mRandomSmoothPhase = 0.0f;
    mRandomPrev = randomFloat();
    mRandomNext = randomFloat();
}

void LFO::setPhaseOffset(float phaseOffset) {
    // Wrap to [0, 2π) using fmod (more efficient than while loops)
    phaseOffset = std::fmod(phaseOffset, DSPMath::TWO_PI);
    if (phaseOffset < 0.0f) {
        phaseOffset += DSPMath::TWO_PI;
    }

    mPhaseOffset = phaseOffset;
}

float LFO::process() {
    // Calculate effective phase with offset (optimized wrapping)
    float effectivePhase = mPhase + mPhaseOffset;
    if (effectivePhase >= DSPMath::TWO_PI) {
        effectivePhase = std::fmod(effectivePhase, DSPMath::TWO_PI);
    }

    // Generate waveform
    float output = generateWaveform(effectivePhase);

    // Advance phase (use current phase increment, updated only when setRate is called)
    mPhase += mPhaseIncrement;
    if (mPhase >= DSPMath::TWO_PI) {
        mPhase -= DSPMath::TWO_PI;
    }

    return output;
}

float LFO::processScaled(float min, float max) {
    float lfoValue = process();  // [-1, 1]
    float unipolar = (lfoValue + 1.0f) / 2.0f;  // [0, 1]
    return min + unipolar * (max - min);
}

float LFO::processUnipolar() {
    return (process() + 1.0f) / 2.0f;
}

void LFO::syncToPhase(float externalPhase) {
    // Wrap phase to [0, 2π) using fmod
    mPhase = std::fmod(externalPhase, DSPMath::TWO_PI);
    if (mPhase < 0.0f) {
        mPhase += DSPMath::TWO_PI;
    }
}

void LFO::updatePhaseIncrement() {
    float rate = mRateHz.load(std::memory_order_acquire);
    mPhaseIncrement = DSPMath::TWO_PI * rate / mSampleRate;
}

float LFO::generateWaveform(float phase) {
    switch (mWaveform) {
        case Waveform::SINE:
            return generateSine(phase);

        case Waveform::TRIANGLE:
            return generateTriangle(phase);

        case Waveform::SQUARE:
            return generateSquare(phase);

        case Waveform::SAWTOOTH:
            return generateSawtooth(phase);

        case Waveform::RANDOM:
            return generateRandom();

        case Waveform::RANDOM_SMOOTH:
            return generateRandomSmooth();

        default:
            return 0.0f;
    }
}

float LFO::generateSine(float phase) {
    // Use fast approximation for better performance
    return DSPMath::fastSin(phase);

    // Alternative: Use standard library for higher accuracy
    // return std::sin(phase);
}

float LFO::generateTriangle(float phase) {
    // Triangle wave: linear ramp up then down
    // Phase [0, π] → output [-1, 1]
    // Phase [π, 2π] → output [1, -1]

    if (phase < DSPMath::PI) {
        // Rising edge: -1 to 1
        return -1.0f + 2.0f * (phase / DSPMath::PI);
    } else {
        // Falling edge: 1 to -1
        return 1.0f - 2.0f * ((phase - DSPMath::PI) / DSPMath::PI);
    }
}

float LFO::generateSquare(float phase) {
    // Square wave: +1 for first half, -1 for second half
    return (phase < DSPMath::PI) ? 1.0f : -1.0f;
}

float LFO::generateSawtooth(float phase) {
    // Sawtooth: linear ramp from -1 to 1 over full period
    return -1.0f + 2.0f * (phase / DSPMath::TWO_PI);
}

float LFO::generateRandom() {
    // Sample & hold random: update value at each cycle
    mRandomPhase += mPhaseIncrement;
    if (mRandomPhase >= DSPMath::TWO_PI) {
        mRandomPhase -= DSPMath::TWO_PI;
        mRandomValue = randomFloat();
    }

    return mRandomValue;
}

float LFO::generateRandomSmooth() {
    // Interpolated random: smooth linear transition between random values
    mRandomSmoothPhase += mPhaseIncrement / DSPMath::TWO_PI;
    if (mRandomSmoothPhase >= 1.0f) {
        mRandomSmoothPhase -= 1.0f;
        mRandomPrev = mRandomNext;
        mRandomNext = randomFloat();
    }

    // Linear interpolation between previous and next random values
    return mRandomPrev + (mRandomNext - mRandomPrev) * mRandomSmoothPhase;
}

float LFO::randomFloat() {
    // Thread-local random generator for better performance
    static thread_local std::mt19937 generator(std::random_device{}());
    static thread_local std::uniform_real_distribution<float> distribution(-1.0f, 1.0f);

    return distribution(generator);
}
