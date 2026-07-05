#include "DelayEffect.h"
#include "EffectDefaults.h"
#include "../dsp/DSPMath.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr int MAX_DELAY_SAMPLES = 96000 * 2;
constexpr float DENORMAL_THRESHOLD = 1e-20f;
}

DelayEffect::DelayEffect() {
    mSampleRate = DEFAULT_SAMPLE_RATE;
    bufferL.resize(MAX_DELAY_SAMPLES, 0.0f);
    bufferR.resize(MAX_DELAY_SAMPLES, 0.0f);
    writePos.store(0, std::memory_order_relaxed);
    delaySamplesSmoother.setSmoothingTime(50.0f, static_cast<float>(mSampleRate));
    updateDelaySamples();
    delaySamplesSmoother.reset(delaySamplesTarget.load(std::memory_order_relaxed));
}

void DelayEffect::setDelayTime(float dt) {
    delayTime.store(std::clamp(dt, 1.0f, 2000.0f), std::memory_order_relaxed);
    if (!sync.load(std::memory_order_relaxed)) updateDelaySamples();
}

void DelayEffect::setFeedback(float fb) {
    feedback.store(std::clamp(fb, 0.0f, 0.95f), std::memory_order_relaxed);
}

void DelayEffect::setWet(float w) {
    wet.store(std::clamp(w, 0.0f, 1.0f), std::memory_order_relaxed);
}

void DelayEffect::setBpm(float b) {
    bpm.store(std::clamp(b, 60.0f, 200.0f), std::memory_order_relaxed);
    if (sync.load(std::memory_order_relaxed)) updateDelaySamples();
}

void DelayEffect::setNoteDivision(float nd) {
    noteDivision.store(std::clamp(nd, 1.0f, 32.0f), std::memory_order_relaxed);
    if (sync.load(std::memory_order_relaxed)) updateDelaySamples();
}

void DelayEffect::setSync(bool s) {
    sync.store(s, std::memory_order_relaxed);
    updateDelaySamples();
}

void DelayEffect::process(float* input, float* output, int numFrames) {
    const float fb = feedback.load(std::memory_order_acquire);
    const float w = wet.load(std::memory_order_acquire);
    const float dry = 1.0f - w;
    const float targetDelay = delaySamplesTarget.load(std::memory_order_acquire);
    int wPos = writePos.load(std::memory_order_relaxed);

    for (int i = 0; i < numFrames * 2; i += 2) {
        float delaySamps = delaySamplesSmoother.process(targetDelay);
        delaySamps = std::clamp(delaySamps, 1.0f, static_cast<float>(MAX_DELAY_SAMPLES - 3));

        float readPosFloat = static_cast<float>(wPos) - delaySamps;
        if (readPosFloat < 0.0f) readPosFloat += static_cast<float>(MAX_DELAY_SAMPLES);

        int readPos1 = static_cast<int>(readPosFloat);
        float frac = readPosFloat - static_cast<float>(readPos1);

        int idx0 = DSPMath::wrapIndex(readPos1 - 1, MAX_DELAY_SAMPLES);
        int idx1 = DSPMath::wrapIndex(readPos1, MAX_DELAY_SAMPLES);
        int idx2 = DSPMath::wrapIndex(readPos1 + 1, MAX_DELAY_SAMPLES);
        int idx3 = DSPMath::wrapIndex(readPos1 + 2, MAX_DELAY_SAMPLES);

        float delayedL = DSPMath::cubicInterpolate(
            bufferL[idx0], bufferL[idx1], bufferL[idx2], bufferL[idx3], frac);
        float delayedR = DSPMath::cubicInterpolate(
            bufferR[idx0], bufferR[idx1], bufferR[idx2], bufferR[idx3], frac);

        if (std::abs(delayedL) < DENORMAL_THRESHOLD) delayedL = 0.0f;
        if (std::abs(delayedR) < DENORMAL_THRESHOLD) delayedR = 0.0f;

        output[i] = dry * input[i] + w * delayedL;
        output[i + 1] = dry * input[i + 1] + w * delayedR;

        float feedbackL = input[i] + fb * delayedL;
        float feedbackR = input[i + 1] + fb * delayedR;

        if (std::abs(feedbackL) < DENORMAL_THRESHOLD) feedbackL = 0.0f;
        if (std::abs(feedbackR) < DENORMAL_THRESHOLD) feedbackR = 0.0f;

        bufferL[wPos] = feedbackL;
        bufferR[wPos] = feedbackR;
        wPos = (wPos + 1) % MAX_DELAY_SAMPLES;

        if (!std::isfinite(output[i])) output[i] = 0.0f;
        if (!std::isfinite(output[i + 1])) output[i + 1] = 0.0f;
    }

    writePos.store(wPos, std::memory_order_relaxed);
}

void DelayEffect::setParam(int paramId, float value) {
    switch (paramId) {
        case 0: setDelayTime(value); break;
        case 1: setFeedback(value); break;
        case 2: setWet(value); break;
        case 3: setBpm(value); break;
        case 4: setNoteDivision(value); break;
        case 5: setSync(value > 0.5f); break;
        default: break;
    }
}

float DelayEffect::getParam(int paramId) {
    switch (paramId) {
        case 0: return delayTime.load(std::memory_order_relaxed);
        case 1: return feedback.load(std::memory_order_relaxed);
        case 2: return wet.load(std::memory_order_relaxed);
        case 3: return bpm.load(std::memory_order_relaxed);
        case 4: return noteDivision.load(std::memory_order_relaxed);
        case 5: return sync.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        default: return 0.0f;
    }
}

void DelayEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    std::fill(bufferL.begin(), bufferL.end(), 0.0f);
    std::fill(bufferR.begin(), bufferR.end(), 0.0f);
    writePos.store(0, std::memory_order_release);
    delaySamplesSmoother.setSmoothingTime(50.0f, static_cast<float>(sampleRate));
    updateDelaySamples();
    delaySamplesSmoother.reset(delaySamplesTarget.load(std::memory_order_relaxed));
}

void DelayEffect::reset() {
    std::fill(bufferL.begin(), bufferL.end(), 0.0f);
    std::fill(bufferR.begin(), bufferR.end(), 0.0f);
    writePos.store(0, std::memory_order_release);
    delaySamplesSmoother.reset(delaySamplesTarget.load(std::memory_order_relaxed));
}

void DelayEffect::updateDelaySamples() {
    float newDelaySamples;
    if (sync.load(std::memory_order_relaxed)) {
        float b = bpm.load(std::memory_order_relaxed);
        float nd = noteDivision.load(std::memory_order_relaxed);
        newDelaySamples = (60.0f / b) * static_cast<float>(mSampleRate) / nd;
    } else {
        float dt = delayTime.load(std::memory_order_relaxed);
        newDelaySamples = dt * static_cast<float>(mSampleRate) / 1000.0f;
    }
    newDelaySamples = std::clamp(newDelaySamples, 1.0f, static_cast<float>(MAX_DELAY_SAMPLES - 3));
    delaySamplesTarget.store(newDelaySamples, std::memory_order_release);
}
