#pragma once

#include "SynthEngine.h"
#include <algorithm>  // std::clamp / std::min / std::max
#include <cmath>
#include <vector>
#include <array>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @class GranularEngine
 * @brief Granular synthesis engine for textural sound generation
 *
 * Generates a continuous source tone, then re-emits it as overlapping
 * micro-grains with controllable size, scatter, and density.
 *
 * Architecture:
 *   1. Internal oscillator fills a circular source buffer (~200ms)
 *   2. Grain emitter creates grains at rate = density
 *   3. Each grain reads from source buffer at position + random offset (scatter)
 *   4. Grains use Hann window envelope for smooth overlap
 *   5. Output = sum of all active grains
 *
 * Parameters:
 *   0 - Grain Size: duration per grain (0=~5ms tiny, 1=~100ms large)
 *   1 - Scatter: position randomization (0=sync, 1=chaos)
 *   2 - Density: grains per second (0=sparse ~2/s, 1=dense ~40/s)
 *
 * RT-Safety: All buffers pre-allocated. Max 32 simultaneous grains.
 */
class GranularEngine : public SynthEngine {
public:
    static constexpr int PARAM_GRAIN_SIZE = 0;
    static constexpr int PARAM_SCATTER = 1;
    static constexpr int PARAM_DENSITY = 2;

    static constexpr int MAX_GRAINS = 32;
    static constexpr int SOURCE_BUFFER_MS = 200;

    GranularEngine() {
        mParams[PARAM_GRAIN_SIZE].store(0.3f, std::memory_order_relaxed);
        mParams[PARAM_SCATTER].store(0.1f, std::memory_order_relaxed);
        mParams[PARAM_DENSITY].store(0.5f, std::memory_order_relaxed);
    }

    void prepare(int32_t sampleRate, int32_t maxBlockSize) override {
        SynthEngine::prepare(sampleRate, maxBlockSize);

        // Source buffer: circular, ~200ms of audio
        mSourceBufferSize = (sampleRate * SOURCE_BUFFER_MS) / 1000;
        mSourceBuffer.resize(mSourceBufferSize, 0.0f);
        mSourceWritePos = 0;

        // Reset grains
        for (auto& g : mGrains) {
            g.active = false;
        }
        mSamplesSinceLastGrain = 0;
        mSourcePhase = 0.0f;
        mRngState = 42;
    }

    void reset() override {
        std::fill(mSourceBuffer.begin(), mSourceBuffer.end(), 0.0f);
        mSourceWritePos = 0;
        for (auto& g : mGrains) {
            g.active = false;
        }
        mSamplesSinceLastGrain = 0;
        mSourcePhase = 0.0f;
    }

    void process(float* buffer, int32_t numFrames,
                 float frequency, float amplitude) override {
        // Read parameters with smoothing (prevents zipper noise)
        const float grainSizeNorm = smoothParam(PARAM_GRAIN_SIZE);
        const float scatter = smoothParam(PARAM_SCATTER);
        const float densityNorm = smoothParam(PARAM_DENSITY);

        // Map params
        // Grain size: 5ms to 100ms
        const int grainSizeSamples = static_cast<int>(
            (5.0f + grainSizeNorm * 95.0f) * static_cast<float>(mSampleRate) / 1000.0f
        );

        // Density: 2 to 40 grains/second
        const float grainsPerSec = 2.0f + densityNorm * 38.0f;
        const int samplesBetweenGrains = static_cast<int>(
            static_cast<float>(mSampleRate) / grainsPerSec
        );

        // Source oscillator increment (saw wave for rich harmonics)
        const float sourceInc = frequency / static_cast<float>(mSampleRate);

        for (int32_t i = 0; i < numFrames; ++i) {
            // 1. Generate source sample (sawtooth)
            float sourceSample = 2.0f * mSourcePhase - 1.0f;
            mSourcePhase += sourceInc;
            if (mSourcePhase >= 1.0f) mSourcePhase -= 1.0f;

            // Write to circular source buffer
            mSourceBuffer[mSourceWritePos] = sourceSample;
            mSourceWritePos = (mSourceWritePos + 1) % mSourceBufferSize;

            // 2. Emit new grains at density rate
            mSamplesSinceLastGrain++;
            if (mSamplesSinceLastGrain >= samplesBetweenGrains) {
                mSamplesSinceLastGrain = 0;
                emitGrain(grainSizeSamples, scatter);
            }

            // 3. Sum all active grains
            float grainSum = 0.0f;
            int activeCount = 0;

            for (auto& g : mGrains) {
                if (!g.active) continue;

                // Read from source buffer at grain position
                int readPos = (g.sourceStart + g.currentSample) % mSourceBufferSize;
                if (readPos < 0) readPos += mSourceBufferSize;
                float sample = mSourceBuffer[readPos];

                // Apply Hann window envelope
                float t = static_cast<float>(g.currentSample) / static_cast<float>(g.length);
                float envelope = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * t));

                grainSum += sample * envelope;
                activeCount++;

                // Advance grain
                g.currentSample++;
                if (g.currentSample >= g.length) {
                    g.active = false;
                }
            }

            // Normalize to prevent volume explosion with many grains
            if (activeCount > 1) {
                grainSum /= std::sqrt(static_cast<float>(activeCount));
            }

            float output = grainSum * amplitude;

            // Sanitize
            if (!std::isfinite(output)) output = 0.0f;

            buffer[i * 2]     = output;
            buffer[i * 2 + 1] = output;
        }
    }

    const char* getName() const override { return "Granular"; }
    int getParameterCount() const override { return 3; }

    EngineParameterDef getParameterDef(int paramId) const override {
        switch (paramId) {
            case PARAM_GRAIN_SIZE:
                return {"Grain Size", "SIZE", 0.0f, 1.0f, 0.3f};
            case PARAM_SCATTER:
                return {"Scatter", "SCAT", 0.0f, 1.0f, 0.1f};
            case PARAM_DENSITY:
                return {"Density", "DENS", 0.0f, 1.0f, 0.5f};
            default:
                return {"Unknown", "?", 0.0f, 1.0f, 0.0f};
        }
    }

private:
    struct Grain {
        bool active = false;
        int sourceStart = 0;    // Start position in source buffer
        int length = 0;         // Grain length in samples
        int currentSample = 0;  // Current playback position within grain
    };

    // Source buffer (circular)
    std::vector<float> mSourceBuffer;
    int mSourceBufferSize = 0;
    int mSourceWritePos = 0;
    float mSourcePhase = 0.0f;

    // Grain pool
    std::array<Grain, MAX_GRAINS> mGrains{};
    int mSamplesSinceLastGrain = 0;

    // RT-safe RNG
    uint32_t mRngState = 42;

    float fastRandom() {
        mRngState ^= mRngState << 13;
        mRngState ^= mRngState >> 17;
        mRngState ^= mRngState << 5;
        return static_cast<float>(static_cast<int32_t>(mRngState)) / static_cast<float>(INT32_MAX);
    }

    void emitGrain(int grainSize, float scatter) {
        // Find inactive grain slot
        for (auto& g : mGrains) {
            if (g.active) continue;

            g.active = true;
            g.length = grainSize;
            g.currentSample = 0;

            // Start position: current write pos minus grain size (read recent audio)
            // Scatter adds random offset into the source buffer
            int basePos = mSourceWritePos - grainSize;
            int scatterRange = static_cast<int>(scatter * static_cast<float>(mSourceBufferSize) * 0.5f);
            int offset = (scatterRange > 0)
                ? static_cast<int>(fastRandom() * static_cast<float>(scatterRange))
                : 0;

            g.sourceStart = (basePos - offset + mSourceBufferSize * 2) % mSourceBufferSize;
            return;
        }
        // All grain slots full — drop this grain (graceful degradation)
    }
};
