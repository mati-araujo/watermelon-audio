#include "Oversampler.h"

Oversampler::Oversampler(int maxBlockSize)
    : mMaxBlockSize(maxBlockSize)
    , mUpsampleFilter1(48000.0f)
    , mUpsampleFilter2(48000.0f)
    , mDownsampleFilter1(48000.0f)
    , mDownsampleFilter2(48000.0f) {

    // Pre-allocate buffers for maximum oversampling (4x)
    mWorkBuffer.resize(maxBlockSize * 4, 0.0f);
    mTempBuffer.resize(maxBlockSize * 4, 0.0f);
}

void Oversampler::prepare(float sampleRate, int blockSize) {
    mSampleRate = sampleRate;
    mMaxBlockSize = std::max(mMaxBlockSize, blockSize);

    // Resize buffers if needed
    size_t requiredSize = static_cast<size_t>(mMaxBlockSize * 4);
    if (mWorkBuffer.size() < requiredSize) {
        mWorkBuffer.resize(requiredSize, 0.0f);
        mTempBuffer.resize(requiredSize, 0.0f);
    }

    updateFilters();
    reset();
}

void Oversampler::setFactor(Factor factor) {
    int newFactor = static_cast<int>(factor);
    int oldFactor = mFactor.exchange(newFactor, std::memory_order_acq_rel);

    if (newFactor != oldFactor) {
        updateFilters();
        reset();
    }
}

void Oversampler::upsample(const float* input, float* output, int numSamples) {
    int factor = mFactor.load(std::memory_order_relaxed);

    if (factor == 1) {
        // No oversampling - just copy
        std::copy(input, input + numSamples, output);
        return;
    }

    int upsampledSize = numSamples * factor;

    // Zero-stuffing: insert zeros between samples
    // Original sample at position i goes to position i*factor
    // All other positions are zero
    std::fill(output, output + upsampledSize, 0.0f);
    for (int i = 0; i < numSamples; ++i) {
        output[i * factor] = input[i] * static_cast<float>(factor);
    }

    // Apply interpolation lowpass filter (removes imaging artifacts)
    // Use cascaded filters for steeper rolloff
    for (int i = 0; i < upsampledSize; ++i) {
        output[i] = mUpsampleFilter1.process(output[i]);
    }
    for (int i = 0; i < upsampledSize; ++i) {
        output[i] = mUpsampleFilter2.process(output[i]);
    }
}

void Oversampler::downsample(const float* input, float* output, int numSamples) {
    int factor = mFactor.load(std::memory_order_relaxed);

    if (factor == 1) {
        // No oversampling - just copy
        std::copy(input, input + numSamples, output);
        return;
    }

    int upsampledSize = numSamples * factor;

    // Apply anti-aliasing lowpass filter before decimation
    // Copy to temp buffer to avoid modifying input
    std::copy(input, input + upsampledSize, mTempBuffer.data());

    // Cascaded filtering for steep rolloff
    for (int i = 0; i < upsampledSize; ++i) {
        mTempBuffer[i] = mDownsampleFilter1.process(mTempBuffer[i]);
    }
    for (int i = 0; i < upsampledSize; ++i) {
        mTempBuffer[i] = mDownsampleFilter2.process(mTempBuffer[i]);
    }

    // Decimation: take every factor-th sample
    for (int i = 0; i < numSamples; ++i) {
        output[i] = mTempBuffer[i * factor];
    }
}

void Oversampler::reset() {
    mUpsampleFilter1.reset();
    mUpsampleFilter2.reset();
    mDownsampleFilter1.reset();
    mDownsampleFilter2.reset();

    std::fill(mWorkBuffer.begin(), mWorkBuffer.end(), 0.0f);
    std::fill(mTempBuffer.begin(), mTempBuffer.end(), 0.0f);
}

void Oversampler::updateFilters() {
    int factor = mFactor.load(std::memory_order_relaxed);

    if (factor <= 1) {
        return; // No filtering needed
    }

    // Upsampled rate
    float upsampledRate = mSampleRate * static_cast<float>(factor);

    // Set all filters to operate at upsampled rate
    mUpsampleFilter1.setSampleRate(upsampledRate);
    mUpsampleFilter2.setSampleRate(upsampledRate);
    mDownsampleFilter1.setSampleRate(upsampledRate);
    mDownsampleFilter2.setSampleRate(upsampledRate);

    // Cutoff frequency: original Nyquist frequency (half of base sample rate)
    // With some margin to avoid ringing
    float cutoff = mSampleRate * 0.45f;  // Slightly below Nyquist

    // Q value for Butterworth-like response
    // Two cascaded filters give 4th-order response
    float Q = 0.7071f;  // 1/sqrt(2) for Butterworth

    // Configure all filters as lowpass
    mUpsampleFilter1.setLowpass(cutoff, Q);
    mUpsampleFilter2.setLowpass(cutoff, Q);
    mDownsampleFilter1.setLowpass(cutoff, Q);
    mDownsampleFilter2.setLowpass(cutoff, Q);
}
