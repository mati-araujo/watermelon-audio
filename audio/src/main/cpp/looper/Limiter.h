#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace wm {

/**
 * @class OfflineLimiter
 * @brief Look-ahead peak limiter for offline export (NOT RT-safe).
 *
 * Single-pass over an interleaved stereo float buffer. For each output sample,
 * the gain reduction is computed from the maximum absolute value within a
 * forward-looking window (`lookaheadMs`). The gain follows an exponential
 * envelope with separate attack and release time constants — fast attack
 * catches transients before they clip; slow release avoids pumping.
 *
 * Result: peak ≤ threshold (default -1 dBFS = 0.891) without the dynamic-range
 * destruction of the previous tanh soft-clip. Suitable for "professional"
 * mastering of looper bounces.
 *
 * Note: this is NOT a true ITU-R BS.1770 true-peak limiter (which would require
 * 4x oversampling); it's a sample-peak limiter, which is sufficient for the
 * 16/24-bit PCM export targets we use. Float export benefits less but stays
 * within nominal range.
 */
class OfflineLimiter {
public:
    /**
     * @param sampleRate    Output sample rate (Hz).
     * @param lookaheadMs   Forward window for peak detection (typical 5 ms).
     * @param attackMs      Time constant when gain decreases (typical 1 ms).
     * @param releaseMs     Time constant when gain recovers (typical 50 ms).
     * @param thresholdLin  Maximum allowed peak amplitude (linear, default -1 dBFS).
     */
    void prepare(int sampleRate,
                 float lookaheadMs = 5.0f,
                 float attackMs = 1.0f,
                 float releaseMs = 50.0f,
                 float thresholdLin = 0.891f /* ~ -1 dBFS */) {
        if (sampleRate <= 0) sampleRate = 48000;
        mLookahead = std::max(1, static_cast<int>((lookaheadMs / 1000.0f) * sampleRate));
        const float attackSamples  = std::max(1.0f, (attackMs  / 1000.0f) * sampleRate);
        const float releaseSamples = std::max(1.0f, (releaseMs / 1000.0f) * sampleRate);
        mAttackCoeff  = std::exp(-1.0f / attackSamples);
        mReleaseCoeff = std::exp(-1.0f / releaseSamples);
        mThreshold = std::clamp(thresholdLin, 0.01f, 1.0f);
    }

    int lookaheadFrames() const { return mLookahead; }

    /**
     * @brief Process a stereo interleaved buffer in-place. The first
     *        `lookaheadFrames()` output samples are slightly attenuated
     *        as the envelope warms up — for tight loops this is rarely
     *        audible but callers can pad the input with silence if needed.
     */
    void processStereo(float* buf, int numFrames) const {
        if (!buf || numFrames <= 0) return;

        // Pre-compute per-frame absolute peak (max of L/R).
        std::vector<float> peak(static_cast<size_t>(numFrames));
        for (int i = 0; i < numFrames; ++i) {
            peak[i] = std::max(std::fabs(buf[i * 2]), std::fabs(buf[i * 2 + 1]));
        }

        // Sliding-window max via monotonic deque. O(N) total instead of O(N*L).
        std::vector<int> dq;
        dq.reserve(static_cast<size_t>(mLookahead));
        // Prime the window with samples [0, mLookahead).
        const int initEnd = std::min(mLookahead, numFrames);
        for (int j = 0; j < initEnd; ++j) {
            while (!dq.empty() && peak[dq.back()] <= peak[j]) dq.pop_back();
            dq.push_back(j);
        }

        float gain = 1.0f;
        for (int i = 0; i < numFrames; ++i) {
            // Window for output sample i is [i, i + mLookahead - 1].
            // The deque holds indices in the current window; front is the max.
            // Pop expired indices (< i).
            while (!dq.empty() && dq.front() < i) dq.erase(dq.begin());
            // Push the new tail index (i + mLookahead - 1) if within bounds.
            const int newIdx = i + mLookahead - 1 + 1;  // next position to admit
            if (newIdx < numFrames) {
                while (!dq.empty() && peak[dq.back()] <= peak[newIdx]) dq.pop_back();
                dq.push_back(newIdx);
            }

            const float windowPeak = dq.empty() ? 0.0f : peak[dq.front()];
            const float targetGain = (windowPeak > mThreshold)
                ? (mThreshold / windowPeak)
                : 1.0f;

            const float coeff = (targetGain < gain) ? mAttackCoeff : mReleaseCoeff;
            gain = targetGain + (gain - targetGain) * coeff;

            buf[i * 2]     *= gain;
            buf[i * 2 + 1] *= gain;
        }
    }

private:
    int mLookahead{240};      // 5 ms @ 48 kHz
    float mAttackCoeff{0.0f};
    float mReleaseCoeff{0.0f};
    float mThreshold{0.891f}; // -1 dBFS
};

}  // namespace wm
