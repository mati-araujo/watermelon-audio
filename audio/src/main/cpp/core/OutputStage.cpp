#include "OutputStage.h"

OutputStage::OutputStage() = default;

void OutputStage::prepare(int sampleRate, int /*maxBlockSize*/) {
    mDCBlocker.reset();
    mDCBlocker.setCutoffFrequency(3.0f, static_cast<float>(sampleRate));
    mLookaheadLimiter.prepare(sampleRate);
    mDitherer.reset();
}

void OutputStage::reset() {
    mDCBlocker.reset();
    mDitherer.reset();
    // Stage 3 bugfix: the lookahead limiter is stateful (delay buffer +
    // gain envelope) and used to silently retain state across stream
    // restarts, causing distortion on first-playback. Cascade the reset.
    mLookaheadLimiter.reset();
}

void OutputStage::dcBlock(float* stereoData, int numFrames) {
    mDCBlocker.process(stereoData, numFrames);
}

void OutputStage::processOutput(float* stereoData, int numFrames) {
    mLookaheadLimiter.process(stereoData, stereoData, numFrames);
    mSoftClipper.processStereo(stereoData, numFrames);
    mDitherer.processStereo(stereoData, numFrames);
    simd::hardLimitStereo(stereoData, numFrames);
}

void OutputStage::processOutputLightweight(float* stereoData, int numFrames) {
    mSoftClipper.processStereo(stereoData, numFrames);
    simd::hardLimitStereo(stereoData, numFrames);
}

void OutputStage::processOutputNoClip(float* stereoData, int numFrames) {
    mLookaheadLimiter.process(stereoData, stereoData, numFrames);
    mSoftClipper.processStereo(stereoData, numFrames);
    simd::hardLimitStereo(stereoData, numFrames);
}

void OutputStage::resizeTempBuffer(size_t size) {
    mTempBuffer.resize(size);
}

void OutputStage::clearTempBuffer() {
    std::fill(mTempBuffer.begin(), mTempBuffer.end(), 0.0f);
}
