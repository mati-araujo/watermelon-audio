#include "DelayEffect.h"
#include "EffectDefaults.h"
#include "../dsp/DSPMath.h"
#include <atomic>
#include <vector>
#include <cmath>
#include <algorithm>

// Maximum delay time: 2 seconds at max sample rate (96kHz)
const int MAX_DELAY_SAMPLES = 96000 * 2;

DelayEffect::DelayEffect() {
    // Initialize with default sample rate
    mSampleRate = DEFAULT_SAMPLE_RATE;
    bufferL.resize(MAX_DELAY_SAMPLES, 0.0f);
    bufferR.resize(MAX_DELAY_SAMPLES, 0.0f);
    writePos = 0;
    updateDelaySamples();
}

void DelayEffect::setDelayTime(float dt) {
    delayTime.store(std::clamp(dt, 1.0f, 2000.0f)); // ms
    updateDelaySamples();
}

void DelayEffect::setFeedback(float fb) {
    feedback.store(std::clamp(fb, 0.0f, 0.9f));
}

void DelayEffect::setWet(float w) {
    wet.store(std::clamp(w, 0.0f, 1.0f));
}

void DelayEffect::setBpm(float b) {
    bpm.store(std::clamp(b, 60.0f, 200.0f));
    if (sync) updateDelaySamples();
}

void DelayEffect::setNoteDivision(float nd) {
    noteDivision.store(std::clamp(nd, 1.0f, 32.0f));
    if (sync) updateDelaySamples();
}

void DelayEffect::setSync(bool s) {
    sync = s;
    updateDelaySamples();
}

void DelayEffect::process(float* input, float* output, int numFrames) {
    float fb = feedback.load(std::memory_order_acquire);
    float w = wet.load(std::memory_order_acquire);
    float dry = 1.0f - w;
    // FIXED: Leer delaySamples atómicamente (RT-safe)
    int delaySamps = delaySamples.load(std::memory_order_acquire);
    int wPos = writePos.load(std::memory_order_relaxed);

    constexpr float DENORMAL_THRESHOLD = 1e-20f;

    for (int i = 0; i < numFrames * 2; i += 2) {
        // PHASE 4 IMPROVEMENT: Interpolación cúbica para transiciones suaves de delay time
        // Cubic interpolation proporciona calidad profesional sin artifacts
        float readPosFloat = static_cast<float>(wPos) - static_cast<float>(delaySamps);
        if (readPosFloat < 0.0f) readPosFloat += static_cast<float>(MAX_DELAY_SAMPLES);

        // Cubic interpolation: 4 puntos para curva Hermite suave
        int readPos1 = static_cast<int>(readPosFloat);
        float frac = readPosFloat - static_cast<float>(readPos1);

        // Índices para interpolación cúbica: y0 (n-1), y1 (n), y2 (n+1), y3 (n+2)
        int idx0 = DSPMath::wrapIndex(readPos1 - 1, MAX_DELAY_SAMPLES);
        int idx1 = readPos1 % MAX_DELAY_SAMPLES;
        int idx2 = (readPos1 + 1) % MAX_DELAY_SAMPLES;
        int idx3 = (readPos1 + 2) % MAX_DELAY_SAMPLES;

        // Interpolar canal izquierdo con cubic Hermite
        float delayedL = DSPMath::cubicInterpolate(
            bufferL[idx0], bufferL[idx1], bufferL[idx2], bufferL[idx3], frac);
        // Interpolar canal derecho con cubic Hermite
        float delayedR = DSPMath::cubicInterpolate(
            bufferR[idx0], bufferR[idx1], bufferR[idx2], bufferR[idx3], frac);

        // Denormal protection en delayed signals
        if (std::abs(delayedL) < DENORMAL_THRESHOLD) delayedL = 0.0f;
        if (std::abs(delayedR) < DENORMAL_THRESHOLD) delayedR = 0.0f;

        output[i] = dry * input[i] + w * delayedL;
        output[i + 1] = dry * input[i + 1] + w * delayedR;

        // Write to buffer with feedback (con denormal protection)
        float feedbackL = input[i] + fb * delayedL;
        float feedbackR = input[i + 1] + fb * delayedR;

        if (std::abs(feedbackL) < DENORMAL_THRESHOLD) feedbackL = 0.0f;
        if (std::abs(feedbackR) < DENORMAL_THRESHOLD) feedbackR = 0.0f;

        bufferL[wPos] = feedbackL;
        bufferR[wPos] = feedbackR;

        wPos = (wPos + 1) % MAX_DELAY_SAMPLES;
    }

    // Store write position atomically
    writePos.store(wPos, std::memory_order_relaxed);

    // Final safety: protect against NaN/Inf in output
    for (int i = 0; i < numFrames * 2; ++i) {
        if (!std::isfinite(output[i])) output[i] = 0.0f;
    }
}

void DelayEffect::setParam(int paramId, float value) {
    switch(paramId) {
        case 0: setDelayTime(value); break;
        case 1: setFeedback(value); break;
        case 2: setWet(value); break;
        case 3: setBpm(value); break;
        case 4: setNoteDivision(value); break;
        case 5: setSync(value > 0.5f); break;
    }
}

float DelayEffect::getParam(int paramId) {
    switch(paramId) {
        case 0: return delayTime.load();
        case 1: return feedback.load();
        case 2: return wet.load();
        case 3: return bpm.load();
        case 4: return noteDivision.load();
        case 5: return sync ? 1.0f : 0.0f;
    }
    return 0.0f;
}

void DelayEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;

    // CRITICAL: Clear buffers to avoid glitches from old samples with wrong timing
    std::fill(bufferL.begin(), bufferL.end(), 0.0f);
    std::fill(bufferR.begin(), bufferR.end(), 0.0f);
    writePos.store(0, std::memory_order_release);

    // Recalculate delay samples with new sample rate
    updateDelaySamples();
}

void DelayEffect::reset() {
    // Zero-fill circular delay buffers WITHOUT resizing — RT-safe.
    // The feedback path accumulates seconds of audio that would
    // otherwise echo into the new signal after a mode transition.
    std::fill(bufferL.begin(), bufferL.end(), 0.0f);
    std::fill(bufferR.begin(), bufferR.end(), 0.0f);
    writePos.store(0, std::memory_order_release);
}

void DelayEffect::updateDelaySamples() {
    int newDelaySamples;
    if (sync) {
        float b = bpm.load();
        float nd = noteDivision.load();
        // IMPROVED: Use dynamic sample rate instead of constant
        newDelaySamples = static_cast<int>((60.0f / b) * mSampleRate / nd);
    } else {
        float dt = delayTime.load();
        // IMPROVED: Use dynamic sample rate instead of constant
        newDelaySamples = static_cast<int>(dt * mSampleRate / 1000.0f);
    }
    newDelaySamples = std::clamp(newDelaySamples, 1, MAX_DELAY_SAMPLES - 1);

    // FIXED: Store atómico para thread-safety
    delaySamples.store(newDelaySamples, std::memory_order_release);
}