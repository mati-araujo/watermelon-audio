#include "OutputStage.h"

#include <cmath>

namespace {
/// Per-sample peak decay. Raised to the block size each block, so the decay is
/// a function of TIME and not of how the host happens to chop up the stream.
constexpr float kPeakDecayPerSample = 0.9995f;

/// RMS integration time. 300 ms is the usual "smooth enough to read" figure.
constexpr float kRmsTimeSeconds = 0.3f;
}  // namespace

OutputStage::OutputStage() = default;

void OutputStage::prepare(int sampleRate, int /*maxBlockSize*/) {
    mDCBlocker.reset();
    mDCBlocker.setCutoffFrequency(3.0f, static_cast<float>(sampleRate));
    mLookaheadLimiter.prepare(sampleRate);
    mDitherer.reset();

    mRmsCoeffPerSample =
        std::exp(-1.0f / (kRmsTimeSeconds * static_cast<float>(sampleRate)));

    mPeakL.store(0.0f, std::memory_order_release);
    mPeakR.store(0.0f, std::memory_order_release);
    mRmsL.store(0.0f, std::memory_order_release);
    mRmsR.store(0.0f, std::memory_order_release);
}

void OutputStage::reset() {
    mDCBlocker.reset();
    mDitherer.reset();

    // The limiter holds 5 ms of audio in its lookahead line. Leaving it across a
    // context change hands the first block of the new context a tail of the old
    // one — audible as a blip of the pad on the first block of INPUT_FX.
    mLookaheadLimiter.reset();

    // The meters reset with the stream. Leaving a stale peak across a stop/start
    // would show the UI a level that belongs to audio nobody is playing.
    mPeakL.store(0.0f, std::memory_order_release);
    mPeakR.store(0.0f, std::memory_order_release);
    mRmsL.store(0.0f, std::memory_order_release);
    mRmsR.store(0.0f, std::memory_order_release);
}

void OutputStage::dcBlock(float* stereoData, int numFrames) {
    mDCBlocker.process(stereoData, numFrames);
}

void OutputStage::processOutput(float* stereoData, int numFrames) {
    mLookaheadLimiter.process(stereoData, stereoData, numFrames);
    mSoftClipper.processStereo(stereoData, numFrames);
    mDitherer.processStereo(stereoData, numFrames);
    simd::hardLimitStereo(stereoData, numFrames);
    updateMeters(stereoData, numFrames);
}

void OutputStage::processOutputLightweight(float* stereoData, int numFrames) {
    mSoftClipper.processStereo(stereoData, numFrames);
    simd::hardLimitStereo(stereoData, numFrames);
    updateMeters(stereoData, numFrames);
}

// ---------------------------------------------------------------------------
// Meters
// ---------------------------------------------------------------------------

void OutputStage::updateMeters(const float* stereoData, int numFrames) {
    if (numFrames <= 0) return;

    float peakL = 0.0f;
    float peakR = 0.0f;
    float sumSquaredL = 0.0f;
    float sumSquaredR = 0.0f;

    for (int i = 0; i < numFrames; ++i) {
        const float sampleL = stereoData[i * 2];
        const float sampleR = stereoData[i * 2 + 1];

        const float absL = std::abs(sampleL);
        const float absR = std::abs(sampleR);
        if (absL > peakL) peakL = absL;
        if (absR > peakR) peakR = absR;

        sumSquaredL += sampleL * sampleL;
        sumSquaredR += sampleR * sampleR;
    }

    // Peak: decaying maximum, so a transient stays readable for a moment
    // instead of flashing for one block.
    const float peakDecay =
        std::pow(kPeakDecayPerSample, static_cast<float>(numFrames));

    float heldL = mPeakL.load(std::memory_order_acquire) * peakDecay;
    float heldR = mPeakR.load(std::memory_order_acquire) * peakDecay;
    if (peakL > heldL) heldL = peakL;
    if (peakR > heldR) heldR = peakR;
    mPeakL.store(heldL, std::memory_order_release);
    mPeakR.store(heldR, std::memory_order_release);

    // RMS: exponential average over ~300 ms.
    //
    // OJO — acá había un bug, y viajó intacto porque el medidor entero estaba
    // muerto y nadie podía verlo. mRmsCoeffPerSample es un coeficiente POR
    // MUESTRA, y OutputNode lo aplicaba UNA VEZ POR BLOQUE. A 48 kHz eso da
    // 0.9999306 por bloque: con bloques de 256 frames la constante de tiempo
    // real era ~77 SEGUNDOS en vez de los 300 ms que promete el comentario, o
    // sea un RMS clavado cerca de cero. Elevarlo a numFrames —igual que hace el
    // peak justo arriba— lo vuelve una constante de tiempo, no una constante
    // por bloque, y deja de depender de cómo el host parta el stream.
    const float rmsCoeff =
        std::pow(mRmsCoeffPerSample, static_cast<float>(numFrames));

    const float blockRmsL = std::sqrt(sumSquaredL / static_cast<float>(numFrames));
    const float blockRmsR = std::sqrt(sumSquaredR / static_cast<float>(numFrames));

    const float smoothedL =
        mRmsL.load(std::memory_order_acquire) * rmsCoeff + blockRmsL * (1.0f - rmsCoeff);
    const float smoothedR =
        mRmsR.load(std::memory_order_acquire) * rmsCoeff + blockRmsR * (1.0f - rmsCoeff);

    mRmsL.store(smoothedL, std::memory_order_release);
    mRmsR.store(smoothedR, std::memory_order_release);
}

float OutputStage::getPeakLevel(int channel) const {
    if (channel == 0) return mPeakL.load(std::memory_order_acquire);
    if (channel == 1) return mPeakR.load(std::memory_order_acquire);
    return 0.0f;
}

float OutputStage::getRMSLevel(int channel) const {
    if (channel == 0) return mRmsL.load(std::memory_order_acquire);
    if (channel == 1) return mRmsR.load(std::memory_order_acquire);
    return 0.0f;
}

void OutputStage::resizeTempBuffer(size_t size) {
    mTempBuffer.resize(size);
}

void OutputStage::clearTempBuffer() {
    std::fill(mTempBuffer.begin(), mTempBuffer.end(), 0.0f);
}
