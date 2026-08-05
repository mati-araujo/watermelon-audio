#pragma once

#include "../dsp/DCBlocker.h"
#include "../dsp/SoftClipper.h"
#include "../dsp/Dithering.h"
#include "../dsp/SIMDUtils.h"
#include "../effects/LookaheadLimiter.h"
#include <atomic>
#include <vector>

/**
 * @brief Output stage protection chain, and the output meters.
 *
 * Owns DC blocker, lookahead limiter, soft clipper, ditherer and
 * the pre-allocated scratch buffer that was formerly mTempBuffer
 * inside AudioEngine.
 *
 * All DSP methods are RT-safe (no allocation, no locks).
 *
 * WHY THE METERS LIVE HERE. They used to live on OutputNode, whose process()
 * was never called from anywhere — so peak and RMS were permanently 0 while
 * audio played, and NoisyPad's guitar-mode level meter never moved. This class
 * is the one place the output paths converge: processOutput and
 * processOutputLightweight are each the LAST thing to touch the buffer before it
 * goes to the device. Metering at the tail of both means the reading is "what
 * the engine handed over", identically on every path — and a third path has to
 * come through here to exist at all.
 *
 * That invariant held only as long as each block goes through exactly ONE of
 * them, exactly once. It did not: MIX ran processOutput at the tail of the
 * instrument bus and then a second full pass over the summed buffer, so the same
 * stateful lookahead limiter advanced twice per block and the meter's second
 * reading landed on top of one it had already taken of the PRE-mix signal —
 * which made it settle at about half the true level. The second variant that
 * existed for that pass (processOutputNoClip) is gone with it; the master bus is
 * protected once, at the tail of the callback.
 */
class OutputStage {
public:
    OutputStage();

    /// Prepare DSP components for a new sample rate / block size.
    void prepare(int sampleRate, int maxBlockSize);

    /// Reset DSP state (call on stop / stream change).
    void reset();

    /// DC blocking (call before effect chain).
    void dcBlock(float* stereoData, int numFrames);

    /// Full output protection chain: limiter -> soft clip -> dither -> hard limit.
    void processOutput(float* stereoData, int numFrames);

    /// Lightweight output protection (no limiter, no dither — USB direct path).
    void processOutputLightweight(float* stereoData, int numFrames);

    // -- Scratch buffer access (pre-allocated) --
    float* getTempBuffer() { return mTempBuffer.data(); }
    size_t getTempBufferSize() const { return mTempBuffer.size(); }

    /// Resize scratch buffer (non-RT, call from constructor / init).
    void resizeTempBuffer(size_t size);

    /// Clear scratch buffer to zero (RT-safe, fixed size).
    void clearTempBuffer();

    // -- Output meters (for UI) --
    //
    // Written by the audio thread at the tail of every processOutput*, read by
    // whoever polls (UI, at its own pace). Plain atomics, no coordination: a
    // reader that catches a torn moment gets one stale block, which for a level
    // meter is invisible. `channel` is 0 = L, 1 = R; anything else reads 0.

    /// Peak level, linear, with slow decay so a transient stays visible.
    float getPeakLevel(int channel) const;

    /// RMS level, linear, exponentially smoothed over ~300 ms.
    float getRMSLevel(int channel) const;

private:
    /// Update the meters from the FINAL buffer. Called at the tail of every
    /// processOutput* — RT-safe (no allocation, no locks).
    void updateMeters(const float* stereoData, int numFrames);

    StereoDCBlocker   mDCBlocker;
    LookaheadLimiter  mLookaheadLimiter;
    SoftClipper       mSoftClipper;
    StereoDitherer    mDitherer;
    std::vector<float> mTempBuffer;

    std::atomic<float> mPeakL{0.0f};
    std::atomic<float> mPeakR{0.0f};
    std::atomic<float> mRmsL{0.0f};
    std::atomic<float> mRmsR{0.0f};

    // Per-SAMPLE smoothing factor for the RMS, set in prepare() from the sample
    // rate. It is raised to numFrames per block — see the note in the .cpp about
    // why applying it once per block (as the original OutputNode did) is wrong.
    float mRmsCoeffPerSample{0.0f};
};
