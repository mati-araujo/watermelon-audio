#pragma once

#include "../dsp/DCBlocker.h"
#include "../dsp/SoftClipper.h"
#include "../dsp/Dithering.h"
#include "../dsp/SIMDUtils.h"
#include "../effects/LookaheadLimiter.h"
#include <vector>

/**
 * @brief Output stage protection chain.
 *
 * Owns DC blocker, lookahead limiter, soft clipper, ditherer and
 * the pre-allocated scratch buffer that was formerly mTempBuffer
 * inside AudioEngine.
 *
 * All DSP methods are RT-safe (no allocation, no locks).
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

    /// Partial output: limiter + soft clip + hard limit (no dither — mixer mix path).
    void processOutputNoClip(float* stereoData, int numFrames);

    // -- Scratch buffer access (pre-allocated) --
    float* getTempBuffer() { return mTempBuffer.data(); }
    size_t getTempBufferSize() const { return mTempBuffer.size(); }

    /// Resize scratch buffer (non-RT, call from constructor / init).
    void resizeTempBuffer(size_t size);

    /// Clear scratch buffer to zero (RT-safe, fixed size).
    void clearTempBuffer();

private:
    StereoDCBlocker   mDCBlocker;
    LookaheadLimiter  mLookaheadLimiter;
    SoftClipper       mSoftClipper;
    StereoDitherer    mDitherer;
    std::vector<float> mTempBuffer;
};
