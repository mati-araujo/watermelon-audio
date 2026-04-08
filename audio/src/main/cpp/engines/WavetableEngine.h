#pragma once

#include "SynthEngine.h"
#include <cmath>
#include <vector>
#include <array>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @class WavetableEngine
 * @brief Wavetable synthesis with spectral morphing
 *
 * Pre-computed wavetable frames with smooth interpolation between them.
 * Position parameter scans through the table, Morph applies spectral warping.
 *
 * Built-in wavetable: 32 frames transitioning from sine → saw → square → pulse,
 * with progressive harmonic content. Each frame is 2048 samples.
 *
 * Parameters:
 *   0 - Position: wavetable scan position (0=sine, 0.33=saw, 0.66=square, 1=pulse)
 *   1 - Morph: spectral warping intensity (0=clean, 1=heavily warped)
 *
 * RT-Safety: All wavetable data pre-allocated in prepare(). No allocations in process().
 */
class WavetableEngine : public SynthEngine {
public:
    static constexpr int PARAM_POSITION = 0;
    static constexpr int PARAM_MORPH = 1;

    static constexpr int TABLE_SIZE = 2048;
    static constexpr int NUM_FRAMES = 32;

    WavetableEngine() {
        mParams[PARAM_POSITION].store(0.0f, std::memory_order_relaxed);
        mParams[PARAM_MORPH].store(0.0f, std::memory_order_relaxed);
    }

    void prepare(int32_t sampleRate, int32_t maxBlockSize) override {
        SynthEngine::prepare(sampleRate, maxBlockSize);
        mInvSampleRate = 1.0f / static_cast<float>(sampleRate);
        mPhase = 0.0f;
        buildWavetable();
    }

    void reset() override {
        mPhase = 0.0f;
    }

    void process(float* buffer, int32_t numFrames,
                 float frequency, float amplitude) override {
        // Read parameters with smoothing (prevents zipper noise)
        const float position = smoothParam(PARAM_POSITION);
        const float morph = smoothParam(PARAM_MORPH);

        // Map position to frame index (fractional for interpolation)
        const float framePos = position * static_cast<float>(NUM_FRAMES - 1);
        const int frame0 = std::clamp(static_cast<int>(framePos), 0, NUM_FRAMES - 2);
        const int frame1 = frame0 + 1;
        const float frameFrac = framePos - static_cast<float>(frame0);

        const float phaseInc = frequency * mInvSampleRate;

        for (int32_t i = 0; i < numFrames; ++i) {
            // Read from two adjacent frames and interpolate
            float sample0 = readFrame(frame0, mPhase, morph);
            float sample1 = readFrame(frame1, mPhase, morph);

            // Linear crossfade between frames
            float sample = sample0 + frameFrac * (sample1 - sample0);

            sample *= amplitude;

            // Sanitize
            if (!std::isfinite(sample)) sample = 0.0f;

            buffer[i * 2]     = sample;
            buffer[i * 2 + 1] = sample;

            // Advance phase
            mPhase += phaseInc;
            if (mPhase >= 1.0f) mPhase -= std::floor(mPhase);
        }
    }

    const char* getName() const override { return "Wavetable"; }
    int getParameterCount() const override { return 2; }

    EngineParameterDef getParameterDef(int paramId) const override {
        switch (paramId) {
            case PARAM_POSITION:
                return {"Position", "POS", 0.0f, 1.0f, 0.0f};
            case PARAM_MORPH:
                return {"Morph", "MORPH", 0.0f, 1.0f, 0.0f};
            default:
                return {"Unknown", "?", 0.0f, 1.0f, 0.0f};
        }
    }

private:
    // Wavetable: NUM_FRAMES frames × TABLE_SIZE samples each
    std::vector<float> mTable; // Flat array: frame * TABLE_SIZE + sample
    float mPhase = 0.0f;
    float mInvSampleRate = 1.0f / 48000.0f;

    /**
     * Read a sample from a specific frame with morph warping.
     * @param frame Frame index (0 to NUM_FRAMES-1)
     * @param phase Normalized phase [0, 1)
     * @param morph Warp amount [0, 1]
     */
    float readFrame(int frame, float phase, float morph) const {
        // Apply morph as phase distortion (spectral warping)
        float warpedPhase = phase;
        if (morph > 0.001f) {
            // Phase distortion: bend the phase curve for harmonic reshaping
            // Creates formant-like shifts and spectral evolution
            float bend = morph * 0.8f;
            if (warpedPhase < 0.5f) {
                warpedPhase = 0.5f * std::pow(warpedPhase * 2.0f, 1.0f - bend);
            } else {
                warpedPhase = 1.0f - 0.5f * std::pow((1.0f - warpedPhase) * 2.0f, 1.0f - bend);
            }
        }

        // Interpolated table read
        float tablePos = warpedPhase * static_cast<float>(TABLE_SIZE);
        int idx0 = static_cast<int>(tablePos) & (TABLE_SIZE - 1);
        int idx1 = (idx0 + 1) & (TABLE_SIZE - 1);
        float frac = tablePos - std::floor(tablePos);

        int offset = frame * TABLE_SIZE;
        float s0 = mTable[offset + idx0];
        float s1 = mTable[offset + idx1];

        return s0 + frac * (s1 - s0);
    }

    /**
     * Build the wavetable with progressive harmonic content.
     * Frame 0 = pure sine, gradually adding harmonics toward saw/square/pulse.
     */
    void buildWavetable() {
        mTable.resize(NUM_FRAMES * TABLE_SIZE, 0.0f);

        const float twoPi = 2.0f * static_cast<float>(M_PI);

        for (int f = 0; f < NUM_FRAMES; ++f) {
            float t = static_cast<float>(f) / static_cast<float>(NUM_FRAMES - 1);

            // Determine waveform character based on position
            // 0.0-0.3: sine → saw (adding harmonics progressively)
            // 0.3-0.6: saw → square (even harmonics fade)
            // 0.6-1.0: square → pulse (duty cycle narrows)

            for (int s = 0; s < TABLE_SIZE; ++s) {
                float phase = static_cast<float>(s) / static_cast<float>(TABLE_SIZE);
                float sample = 0.0f;

                if (t < 0.33f) {
                    // Sine → Saw: additive harmonics
                    float sawAmount = t / 0.33f;
                    int maxHarmonics = 1 + static_cast<int>(sawAmount * 15.0f);

                    for (int h = 1; h <= maxHarmonics; ++h) {
                        float harmonicAmp = 1.0f / static_cast<float>(h);
                        // Crossfade from sine (only fundamental) to saw (all harmonics)
                        if (h == 1) {
                            sample += std::sin(phase * twoPi * static_cast<float>(h));
                        } else {
                            sample += sawAmount * harmonicAmp *
                                      std::sin(phase * twoPi * static_cast<float>(h));
                        }
                    }
                } else if (t < 0.66f) {
                    // Saw → Square: fade out even harmonics
                    float squareAmount = (t - 0.33f) / 0.33f;
                    int maxHarmonics = 16;

                    for (int h = 1; h <= maxHarmonics; ++h) {
                        float harmonicAmp = 1.0f / static_cast<float>(h);
                        bool isEven = (h % 2 == 0);
                        if (isEven) {
                            harmonicAmp *= (1.0f - squareAmount); // Fade even harmonics
                        }
                        sample += harmonicAmp * std::sin(phase * twoPi * static_cast<float>(h));
                    }
                } else {
                    // Square → Pulse: narrow duty cycle
                    float pulseAmount = (t - 0.66f) / 0.34f;
                    float dutyCycle = 0.5f - pulseAmount * 0.35f; // 0.5 → 0.15
                    sample = (phase < dutyCycle) ? 1.0f : -1.0f;

                    // Smooth transitions with a small crossfade
                    float edge = 0.01f;
                    if (std::abs(phase - dutyCycle) < edge) {
                        float blend = (phase - dutyCycle + edge) / (2.0f * edge);
                        sample = 1.0f - 2.0f * std::clamp(blend, 0.0f, 1.0f);
                    }
                }

                // Normalize
                mTable[f * TABLE_SIZE + s] = std::clamp(sample, -1.0f, 1.0f);
            }
        }
    }
};
