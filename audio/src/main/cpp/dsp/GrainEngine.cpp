#include "GrainEngine.h"
#include <cstring>

GrainEngine::GrainEngine() {
    precomputeEnvelope();
}

void GrainEngine::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;

    // Pre-allocate to max 4 seconds stereo
    mBufferSize = sampleRate * 4;
    mBufferL.resize(mBufferSize, 0.0f);
    mBufferR.resize(mBufferSize, 0.0f);

    reset();
}

void GrainEngine::writeToBuffer(float sampleL, float sampleR) {
    if (mBufferSize <= 0) return;
    mBufferL[mWritePos] = sampleL;
    mBufferR[mWritePos] = sampleR;
    mWritePos = (mWritePos + 1) % mBufferSize;
}

void GrainEngine::triggerGrain(float grainSizeMs, float positionSpread, float pitchShift) {
    if (mBufferSize <= 0) return;

    Grain& grain = mVoices[mNextVoice];
    mNextVoice = (mNextVoice + 1) % GRAIN_MAX_VOICES;

    grain.active = true;
    grain.envelopePhase = 0.0f;
    grain.grainLength = std::clamp(
        static_cast<int>(grainSizeMs * mSampleRate / 1000.0f),
        1, mBufferSize / 2
    );
    grain.envelopeInc = 1.0f / static_cast<float>(grain.grainLength);

    // Read position: behind write position + random spread
    float maxOffset = static_cast<float>(mBufferSize) * std::clamp(positionSpread, 0.0f, 1.0f);
    float randomOffset = randomFloat(0.0f, maxOffset);
    float baseOffset = grainSizeMs * mSampleRate / 1000.0f * 2.0f;
    grain.readPosition = static_cast<float>(mWritePos) - baseOffset - randomOffset;
    while (grain.readPosition < 0.0f) grain.readPosition += static_cast<float>(mBufferSize);

    // Pitch shift via playback speed
    grain.readSpeed = std::pow(2.0f, std::clamp(pitchShift, -12.0f, 12.0f) / 12.0f);
}

void GrainEngine::process(float& outputL, float& outputR) {
    outputL = 0.0f;
    outputR = 0.0f;

    if (mBufferSize <= 0) return;

    for (int v = 0; v < GRAIN_MAX_VOICES; ++v) {
        Grain& g = mVoices[v];
        if (!g.active) continue;

        float env = getEnvelope(g.envelopePhase);

        float sampleL = readBufferInterpolated(mBufferL, g.readPosition) * env;
        float sampleR = readBufferInterpolated(mBufferR, g.readPosition) * env;

        outputL += sampleL;
        outputR += sampleR;

        // Advance
        g.readPosition += g.readSpeed;
        float bufSize = static_cast<float>(mBufferSize);
        if (g.readPosition >= bufSize) g.readPosition -= bufSize;
        if (g.readPosition < 0.0f) g.readPosition += bufSize;

        g.envelopePhase += g.envelopeInc;
        if (g.envelopePhase >= 1.0f) {
            g.active = false;
        }
    }

    // Denormal protection
    if (std::abs(outputL) < 1e-20f) outputL = 0.0f;
    if (std::abs(outputR) < 1e-20f) outputR = 0.0f;

    // NaN/Inf protection
    if (!std::isfinite(outputL)) outputL = 0.0f;
    if (!std::isfinite(outputR)) outputR = 0.0f;
}

void GrainEngine::reset() {
    mWritePos = 0;
    if (mBufferSize > 0) {
        std::fill(mBufferL.begin(), mBufferL.end(), 0.0f);
        std::fill(mBufferR.begin(), mBufferR.end(), 0.0f);
    }
    for (int v = 0; v < GRAIN_MAX_VOICES; ++v) {
        mVoices[v].active = false;
    }
    mNextVoice = 0;
}

float GrainEngine::readBufferInterpolated(const std::vector<float>& buffer, float position) {
    int bufSize = static_cast<int>(buffer.size());
    if (bufSize <= 0) return 0.0f;

    // Clamp position
    position = std::fmod(position, static_cast<float>(bufSize));
    if (position < 0.0f) position += static_cast<float>(bufSize);

    int index0 = std::clamp(static_cast<int>(position), 0, bufSize - 1);
    int index1 = (index0 + 1) % bufSize;
    float frac = position - static_cast<float>(index0);

    return buffer[index0] * (1.0f - frac) + buffer[index1] * frac;
}

float GrainEngine::getEnvelope(float phase) {
    phase = std::clamp(phase, 0.0f, 1.0f);
    float idx = phase * static_cast<float>(ENVELOPE_TABLE_SIZE - 1);
    int i0 = std::clamp(static_cast<int>(idx), 0, ENVELOPE_TABLE_SIZE - 2);
    float frac = idx - static_cast<float>(i0);
    return mEnvelopeTable[i0] * (1.0f - frac) + mEnvelopeTable[i0 + 1] * frac;
}

float GrainEngine::randomFloat(float min, float max) {
    // Xorshift32
    mRngState ^= mRngState << 13;
    mRngState ^= mRngState >> 17;
    mRngState ^= mRngState << 5;
    float normalized = static_cast<float>(mRngState) / static_cast<float>(UINT32_MAX);
    return min + normalized * (max - min);
}

void GrainEngine::precomputeEnvelope() {
    for (int i = 0; i < ENVELOPE_TABLE_SIZE; ++i) {
        float phase = static_cast<float>(i) / static_cast<float>(ENVELOPE_TABLE_SIZE - 1);
        mEnvelopeTable[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * phase));
    }
}
