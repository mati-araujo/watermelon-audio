#include "FDN.h"
#include <algorithm>

FDN::FDN()
    : mDelays{
        DelayLine(150.0f), DelayLine(150.0f), DelayLine(150.0f), DelayLine(150.0f),
        DelayLine(150.0f), DelayLine(150.0f), DelayLine(150.0f), DelayLine(150.0f)
    } {
    // Initialize LFOs with different rates for decorrelation
    for (int i = 0; i < FDN_CHANNELS; ++i) {
        mModLfo[i].setWaveform(LFO::Waveform::SINE);
        mModLfo[i].setRate(0.3f + 0.1f * static_cast<float>(i));
        mModLfo[i].setPhaseOffset(static_cast<float>(i) / FDN_CHANNELS);
    }
    // Start the smoother at the current size, not at ParameterSmoother's 0.0f
    // default — otherwise the first block glides up from a degenerate 1-sample
    // delay and the network screams on the very first note.
    mSizeSmooth.reset(mSize.load(std::memory_order_relaxed));
    mSizeSmooth.setSmoothingTime(SIZE_SMOOTHING_MS,
                                 static_cast<float>(mSampleRate.load(std::memory_order_relaxed)));
    updateFeedbackGains();
}

void FDN::setSampleRate(int sampleRate) {
    mSampleRate.store(sampleRate, std::memory_order_relaxed);
    float sr = static_cast<float>(sampleRate);

    for (int i = 0; i < FDN_CHANNELS; ++i) {
        mDelays[i] = DelayLine(150.0f, sr);
        mModLfo[i].setSampleRate(sampleRate);
    }

    // Coefficient is sample-rate dependent; re-derive it and snap to the
    // current target (the delay lines were just rebuilt, so there is no tail
    // left to protect and gliding from a stale value would be audible).
    mSizeSmooth.setSmoothingTime(SIZE_SMOOTHING_MS, sr);
    mSizeSmooth.reset(mSize.load(std::memory_order_relaxed));

    updateDamping();
    updateFeedbackGains();
}

void FDN::setDecayTime(float seconds) {
    mDecayTime.store(std::max(seconds, 0.1f), std::memory_order_relaxed);
    updateFeedbackGains();
}

void FDN::setSize(float size) {
    mSize.store(std::clamp(size, 0.1f, 1.0f), std::memory_order_relaxed);
    updateFeedbackGains();
}

void FDN::setDamping(float hfDamping, float lfDamping) {
    mHfDamping.store(std::clamp(hfDamping, 0.0f, 1.0f), std::memory_order_relaxed);
    mLfDamping.store(std::clamp(lfDamping, 0.0f, 1.0f), std::memory_order_relaxed);
    updateDamping();
}

void FDN::setModulation(float depth) {
    mModDepth.store(std::clamp(depth, 0.0f, 1.0f), std::memory_order_relaxed);
}

void FDN::process(float inputL, float inputR, float& outputL, float& outputR) {
    // Load parameters atomically. Size is glided toward its target so the delay
    // taps sweep instead of jumping; the resulting slow pitch drift during the
    // move is the same behaviour the mod LFO already imposes on these taps.
    float size = mSizeSmooth.process(mSize.load(std::memory_order_relaxed));
    float modDepth = mModDepth.load(std::memory_order_relaxed);
    int sampleRate = mSampleRate.load(std::memory_order_relaxed);
    float sr = static_cast<float>(sampleRate);

    // Read feedback gains from active buffer (atomic index)
    int activeBuffer = mActiveGainBuffer.load(std::memory_order_acquire);
    const float* feedbackGain = (activeBuffer == 0) ? mFeedbackGainA : mFeedbackGainB;

    // Distribute input to channels (0-3 L, 4-7 R)
    float input[FDN_CHANNELS];
    for (int i = 0; i < 4; ++i) input[i] = inputL * 0.5f;
    for (int i = 4; i < 8; ++i) input[i] = inputR * 0.5f;

    float out[FDN_CHANNELS];

    for (int i = 0; i < FDN_CHANNELS; ++i) {
        // Delay time modulation
        float modSamples = mModLfo[i].process() * modDepth * 10.0f;
        float delaySamples = BASE_DELAYS_MS[i] * size * sr / 1000.0f + modSamples;
        delaySamples = std::max(delaySamples, 1.0f);

        // Read from delay
        out[i] = mDelays[i].readInterpolated(delaySamples);

        // Damping filters (BiquadFilter uses atomic coefficients internally)
        out[i] = mLpf[i].process(out[i]);
        out[i] = mHpf[i].process(out[i]);

        // Feedback gain (from RT60)
        out[i] *= feedbackGain[i];

        // Denormal protection
        if (std::abs(out[i]) < DENORMAL_THRESHOLD) out[i] = 0.0f;
    }

    // Hadamard mixing
    applyHadamardMix(out);

    // Write back to delays with new input
    for (int i = 0; i < FDN_CHANNELS; ++i) {
        mDelays[i].write(input[i] + out[i]);
    }

    // Output: sum channels with panning
    outputL = 0.0f;
    outputR = 0.0f;
    for (int i = 0; i < FDN_CHANNELS; ++i) {
        float pan = static_cast<float>(i) / (FDN_CHANNELS - 1);
        outputL += out[i] * (1.0f - pan);
        outputR += out[i] * pan;
    }
}

void FDN::reset() {
    for (int i = 0; i < FDN_CHANNELS; ++i) {
        mDelays[i].clear();

        // WD-3.2 — lo que faltaba, y es el mismo error una capa mas abajo:
        // limpiar las lineas de delay es limpiar la COLA, pero cada canal tiene
        // ademas dos biquads de damping (con su z1/z2) y un LFO de modulacion
        // (con su fase). Sin esto, tres reverbs distintos —hall, plate y
        // shimmer— seguian arrastrando estado a traves del reset aunque los
        // tres hubieran arreglado el suyo: la deuda estaba en el primitivo que
        // comparten, no en ellos.
        mLpf[i].reset();
        mHpf[i].reset();
        mModLfo[i].reset();
    }
    // No tail survives a reset, so there is nothing to glide for: snap to the
    // target and let the next note start at the size the user actually set.
    mSizeSmooth.reset(mSize.load(std::memory_order_relaxed));
}

void FDN::updateFeedbackGains() {
    float decayTime = mDecayTime.load(std::memory_order_relaxed);
    float size = mSize.load(std::memory_order_relaxed);

    // Write to INACTIVE buffer, then swap
    int activeBuffer = mActiveGainBuffer.load(std::memory_order_acquire);
    float* targetGain = (activeBuffer == 0) ? mFeedbackGainB : mFeedbackGainA;

    // g = 10^(-3 * delayTime / RT60)
    for (int i = 0; i < FDN_CHANNELS; ++i) {
        float delaySeconds = BASE_DELAYS_MS[i] * size / 1000.0f;
        targetGain[i] = std::pow(10.0f, -3.0f * delaySeconds / decayTime);
    }

    // Atomic swap to new buffer
    mActiveGainBuffer.store(1 - activeBuffer, std::memory_order_release);
}

void FDN::updateDamping() {
    float hfDamping = mHfDamping.load(std::memory_order_relaxed);
    float lfDamping = mLfDamping.load(std::memory_order_relaxed);
    float sr = static_cast<float>(mSampleRate.load(std::memory_order_relaxed));

    for (int i = 0; i < FDN_CHANNELS; ++i) {
        // HF damping: LPF cutoff from 20kHz down to 1kHz
        float lpfCutoff = 20000.0f * std::pow(1000.0f / 20000.0f, hfDamping);
        lpfCutoff = std::clamp(lpfCutoff, 200.0f, sr * 0.45f);
        mLpf[i].setLowpass(lpfCutoff, 0.707f);

        // LF damping: HPF cutoff from 20Hz up to 500Hz
        float hpfCutoff = 20.0f * std::pow(500.0f / 20.0f, lfDamping);
        hpfCutoff = std::clamp(hpfCutoff, 20.0f, sr * 0.45f);
        mHpf[i].setHighpass(hpfCutoff, 0.707f);
    }
}

void FDN::applyHadamardMix(float* s) {
    // Hadamard 8x8 in-place via butterfly operations
    float a, b;

    // Stage 1: pairs
    for (int i = 0; i < 8; i += 2) {
        a = s[i]; b = s[i + 1];
        s[i] = a + b; s[i + 1] = a - b;
    }
    // Stage 2: quads
    for (int i = 0; i < 8; i += 4) {
        a = s[i]; b = s[i + 2];
        s[i] = a + b; s[i + 2] = a - b;
        a = s[i + 1]; b = s[i + 3];
        s[i + 1] = a + b; s[i + 3] = a - b;
    }
    // Stage 3: octets
    for (int i = 0; i < 4; ++i) {
        a = s[i]; b = s[i + 4];
        s[i] = a + b; s[i + 4] = a - b;
    }
    // Normalize: 1/sqrt(8)
    constexpr float norm = 0.35355339f;
    for (int i = 0; i < 8; ++i) s[i] *= norm;
}
