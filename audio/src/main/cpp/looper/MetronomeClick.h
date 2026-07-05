#pragma once

#include <atomic>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace wm {

/**
 * @class MetronomeClick
 * @brief Self-contained metronome / count-in click generator.
 *
 * Extracted verbatim from AudioLooper (plan §3.3 — move, not redesign). Renders
 * a short sine "tick" with a linear fade-out tail. Fully lock-free:
 * - render() runs on the audio thread (RT-safe: no allocs, no locks).
 * - trigger() may be called from the audio or control thread (atomic stores).
 * - setSampleRate() runs on the control thread.
 *
 * It is intentionally independent of looper enable/master-volume state so a
 * count-in can sound before any track has enabled playback or recording.
 */
class MetronomeClick {
public:
    // Click envelope is sample-rate aware; recompute on setSampleRate().
    static constexpr float CLICK_DURATION_MS = 10.0f;
    static constexpr float CLICK_FADE_MS = 2.5f;        // ~25% of duration

    MetronomeClick() { recompute(); }

    /** Control thread: update sample rate and recompute envelope durations. */
    void setSampleRate(int sampleRate) {
        if (sampleRate <= 0) return;
        mSampleRate.store(sampleRate, std::memory_order_release);
        recompute();
    }

    /** Fire a click. Downbeats are higher/louder than off-beats. */
    void trigger(bool isDownbeat) {
        mClickFreq.store(isDownbeat ? 1200.0f : 900.0f, std::memory_order_relaxed);
        mClickGain.store(isDownbeat ? 0.35f : 0.25f, std::memory_order_relaxed);
        mClickPhase.store(0, std::memory_order_relaxed);
        mClickRemaining.store(mClickDurationFrames.load(std::memory_order_relaxed),
                              std::memory_order_release);
    }

    /**
     * @brief Audio thread: add the in-flight click into the stereo output. RT-safe.
     *        No-op when no click is active.
     */
    void render(float* audioData, int numFrames) {
        if (mClickRemaining.load(std::memory_order_relaxed) <= 0) return;

        int remaining = mClickRemaining.load(std::memory_order_relaxed);
        int phase = mClickPhase.load(std::memory_order_relaxed);
        float freq = mClickFreq.load(std::memory_order_relaxed);
        float gain = mClickGain.load(std::memory_order_relaxed);
        const float invSr = mInvSampleRate.load(std::memory_order_relaxed);
        const int fadeFrames = mClickFadeFrames.load(std::memory_order_relaxed);
        for (int i = 0; i < numFrames && remaining > 0; ++i) {
            float env = (remaining < fadeFrames && fadeFrames > 0)
                ? static_cast<float>(remaining) / static_cast<float>(fadeFrames)
                : 1.0f;
            float sample = std::sin(2.0f * static_cast<float>(M_PI) * freq
                * static_cast<float>(phase) * invSr) * gain * env;
            audioData[i * 2] += sample;
            audioData[i * 2 + 1] += sample;
            phase++;
            remaining--;
        }
        mClickPhase.store(phase, std::memory_order_relaxed);
        mClickRemaining.store(remaining, std::memory_order_relaxed);
    }

private:
    void recompute() {
        const int sr = mSampleRate.load(std::memory_order_relaxed);
        if (sr <= 0) return;
        mInvSampleRate.store(1.0f / static_cast<float>(sr), std::memory_order_relaxed);
        mClickDurationFrames.store(
            static_cast<int>(CLICK_DURATION_MS * 0.001f * static_cast<float>(sr)),
            std::memory_order_relaxed);
        mClickFadeFrames.store(
            static_cast<int>(CLICK_FADE_MS * 0.001f * static_cast<float>(sr)),
            std::memory_order_relaxed);
    }

    std::atomic<int>   mSampleRate{48000};
    std::atomic<float> mInvSampleRate{1.0f / 48000.0f};
    std::atomic<int>   mClickDurationFrames{480};
    std::atomic<int>   mClickFadeFrames{120};
    std::atomic<int>   mClickRemaining{0};
    std::atomic<int>   mClickPhase{0};
    std::atomic<float> mClickFreq{1000.0f};
    std::atomic<float> mClickGain{0.3f};
};

}  // namespace wm
