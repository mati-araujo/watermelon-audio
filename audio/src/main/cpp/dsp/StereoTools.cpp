#include "StereoTools.h"
#include <algorithm>

// ============================================================================
// MidSideProcessor
// ============================================================================

std::pair<float, float> MidSideProcessor::encode(float left, float right) {
    float mid = (left + right) * 0.5f;
    float side = (left - right) * 0.5f;
    return {mid, side};
}

std::pair<float, float> MidSideProcessor::decode(float mid, float side) {
    float left = mid + side;
    float right = mid - side;
    return {left, right};
}

std::pair<float, float> MidSideProcessor::adjustWidth(float left, float right, float width) {
    // Clamp width to reasonable range
    width = std::clamp(width, 0.0f, 2.0f);

    // Encode to Mid/Side
    auto [mid, side] = encode(left, right);

    // Scale side signal by width
    side *= width;

    // Decode back to L/R
    return decode(mid, side);
}

// ============================================================================
// StereoDecorrelator
// ============================================================================

StereoDecorrelator::StereoDecorrelator(float sampleRate)
    : mSampleRate(sampleRate) {
    // Initialize allpass buffers with prime-number lengths for better decorrelation
    mAllpassBufferL.resize(mDelayLengthL, 0.0f);
    mAllpassBufferR.resize(mDelayLengthR, 0.0f);
}

void StereoDecorrelator::setSampleRate(float sampleRate) {
    if (sampleRate <= 0.0f) {
        return;
    }

    mSampleRate = sampleRate;

    // Scale delay lengths proportionally to sample rate
    // Base lengths are for 48kHz
    float scale = sampleRate / 48000.0f;
    mDelayLengthL = static_cast<int>(89.0f * scale);
    mDelayLengthR = static_cast<int>(97.0f * scale);

    // Resize buffers
    mAllpassBufferL.clear();
    mAllpassBufferL.resize(mDelayLengthL, 0.0f);
    mAllpassBufferR.clear();
    mAllpassBufferR.resize(mDelayLengthR, 0.0f);

    mAllpassPosL = 0;
    mAllpassPosR = 0;
}

void StereoDecorrelator::setWidth(float width) {
    mWidth = std::clamp(width, 0.0f, 1.0f);
}

std::pair<float, float> StereoDecorrelator::process(float monoInput) {
    // Apply different allpass filters to create decorrelated L/R from mono

    float left = processAllpass(monoInput, mAllpassBufferL, mAllpassPosL, mDelayLengthL);
    float right = processAllpass(monoInput, mAllpassBufferR, mAllpassPosR, mDelayLengthR);

    // Mix with original based on width
    float dry = 1.0f - mWidth;
    left = monoInput * dry + left * mWidth;
    right = monoInput * dry + right * mWidth;

    return {left, right};
}

std::pair<float, float> StereoDecorrelator::processStereo(float left, float right) {
    // Encode to Mid/Side
    auto [mid, side] = MidSideProcessor::encode(left, right);

    // Decorrelate the side signal
    auto [sideL, sideR] = process(side);

    // Reconstruct L/R
    float outLeft = mid + sideL;
    float outRight = mid - sideR;

    return {outLeft, outRight};
}

void StereoDecorrelator::reset() {
    std::fill(mAllpassBufferL.begin(), mAllpassBufferL.end(), 0.0f);
    std::fill(mAllpassBufferR.begin(), mAllpassBufferR.end(), 0.0f);
    mAllpassPosL = 0;
    mAllpassPosR = 0;
}

float StereoDecorrelator::processAllpass(float input, std::vector<float>& buffer,
                                         int& position, int delayLength) {
    if (delayLength == 0 || buffer.empty()) {
        return input;
    }

    // Read delayed sample
    float delayed = buffer[position];

    // Allpass formula: out = -g * input + delayed + g * out
    float output = -mAllpassGain * input + delayed;

    // Write to buffer: input + g * output
    buffer[position] = input + mAllpassGain * output;

    // Advance position
    position = (position + 1) % delayLength;

    return output;
}

// ============================================================================
// HaasEffect
// ============================================================================

HaasEffect::HaasEffect(float sampleRate, float delayMs)
    : mSampleRate(sampleRate), mDelayMs(delayMs) {
    updateDelay();
}

void HaasEffect::setSampleRate(float sampleRate) {
    if (sampleRate <= 0.0f) {
        return;
    }

    mSampleRate = sampleRate;
    updateDelay();
}

void HaasEffect::setDelayTime(float delayMs) {
    // Clamp to sweet spot range (5-35ms)
    mDelayMs = std::clamp(delayMs, 5.0f, 35.0f);
    updateDelay();
}

void HaasEffect::setDelayRight(bool delayRight) {
    mDelayRight = delayRight;
}

std::pair<float, float> HaasEffect::process(float left, float right) {
    if (mDelayBuffer.empty()) {
        return {left, right};
    }

    // Write to delay buffer
    mDelayBuffer[mWritePos] = mDelayRight ? right : left;

    // Calculate read position
    int readPos = (mWritePos - mDelaySamples + static_cast<int>(mDelayBuffer.size()))
                  % static_cast<int>(mDelayBuffer.size());

    // Read delayed sample
    float delayed = mDelayBuffer[readPos];

    // Advance write position
    mWritePos = (mWritePos + 1) % static_cast<int>(mDelayBuffer.size());

    // Output
    if (mDelayRight) {
        return {left, delayed};
    } else {
        return {delayed, right};
    }
}

void HaasEffect::reset() {
    std::fill(mDelayBuffer.begin(), mDelayBuffer.end(), 0.0f);
    mWritePos = 0;
}

void HaasEffect::updateDelay() {
    mDelaySamples = DSPMath::msToSamples(mDelayMs, mSampleRate);
    mDelaySamples = std::max(1, mDelaySamples);

    // Resize buffer with some headroom
    int bufferSize = mDelaySamples + 10;
    mDelayBuffer.clear();
    mDelayBuffer.resize(bufferSize, 0.0f);
    mWritePos = 0;
}

// ============================================================================
// StereoWidth
// ============================================================================

std::pair<float, float> StereoWidth::process(float left, float right, float width) {
    return MidSideProcessor::adjustWidth(left, right, width);
}
