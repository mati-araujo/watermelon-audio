#pragma once

#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @file GrainEngine.h
 * @brief Granular synthesis engine for BeatGrain effect.
 *
 * Manages a circular buffer and multiple grain voices with
 * raised-cosine envelopes and pitch-shifting via playback speed.
 *
 * Thread safety: Call writeToBuffer/process/triggerGrain from audio thread only.
 * setSampleRate/setBufferLength from control thread only (before processing).
 */

static constexpr int GRAIN_MAX_VOICES = 8;

struct Grain {
    bool active = false;
    float readPosition = 0.0f;
    float readSpeed = 1.0f;
    float envelopePhase = 0.0f;
    float envelopeInc = 0.0f;
    int grainLength = 0;
};

class GrainEngine {
public:
    GrainEngine();
    ~GrainEngine() = default;

    void setSampleRate(int sampleRate);

    void writeToBuffer(float sampleL, float sampleR);
    void triggerGrain(float grainSizeMs, float positionSpread, float pitchShift);
    void process(float& outputL, float& outputR);
    void reset();

private:
    // Circular buffer (pre-allocated to max 4s)
    std::vector<float> mBufferL;
    std::vector<float> mBufferR;
    int mWritePos = 0;
    int mBufferSize = 0;

    // Voices
    Grain mVoices[GRAIN_MAX_VOICES];
    int mNextVoice = 0;

    // Envelope lookup table (raised cosine)
    static constexpr int ENVELOPE_TABLE_SIZE = 1024;
    float mEnvelopeTable[ENVELOPE_TABLE_SIZE];

    int mSampleRate = 48000;

    // PRNG
    uint32_t mRngState = 42;

    float readBufferInterpolated(const std::vector<float>& buffer, float position);
    float getEnvelope(float phase);
    float randomFloat(float min, float max);
    void precomputeEnvelope();
};
