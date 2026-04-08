#include "SpectrumAnalyzer.h"
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

SpectrumAnalyzer::SpectrumAnalyzer(int fftSize) {
    prepare(48000.0f, fftSize);
}

void SpectrumAnalyzer::prepare(float sampleRate, int fftSize) {
    // Ensure FFT size is power of 2
    mFFTSize = 256;
    while (mFFTSize < fftSize && mFFTSize < 4096) {
        mFFTSize *= 2;
    }

    mSampleRate = sampleRate;

    // Allocate buffers
    mInputBuffer.resize(mFFTSize, 0.0f);
    mWindow.resize(mFFTSize, 0.0f);
    mFFTBuffer.resize(mFFTSize);
    mTwiddles.resize(mFFTSize / 2);

    int numBins = mFFTSize / 2;
    mMagnitudes.resize(numBins, -100.0f);
    mSmoothedMagnitudes.resize(numBins, -100.0f);
    mPeakMagnitudes.resize(numBins, -100.0f);

    mInputWritePos = 0;
    mSamplesCollected = 0;

    // Build window and twiddles
    buildWindow();
    precomputeTwiddles();

    // Set default peak decay (20 dB/second)
    setPeakDecay(20.0f);
}

void SpectrumAnalyzer::setWindowType(WindowType type) {
    mWindowType = type;
    buildWindow();
}

void SpectrumAnalyzer::buildWindow() {
    const float N = static_cast<float>(mFFTSize);

    for (int i = 0; i < mFFTSize; ++i) {
        float n = static_cast<float>(i);

        switch (mWindowType) {
            case WindowType::RECTANGULAR:
                mWindow[i] = 1.0f;
                break;

            case WindowType::HANN:
                // Hann window: 0.5 * (1 - cos(2*pi*n/N))
                mWindow[i] = 0.5f * (1.0f - std::cos(2.0f * M_PI * n / N));
                break;

            case WindowType::HAMMING:
                // Hamming window: 0.54 - 0.46 * cos(2*pi*n/N)
                mWindow[i] = 0.54f - 0.46f * std::cos(2.0f * M_PI * n / N);
                break;

            case WindowType::BLACKMAN:
                // Blackman window: 0.42 - 0.5*cos(2*pi*n/N) + 0.08*cos(4*pi*n/N)
                mWindow[i] = 0.42f - 0.5f * std::cos(2.0f * M_PI * n / N)
                           + 0.08f * std::cos(4.0f * M_PI * n / N);
                break;
        }
    }
}

void SpectrumAnalyzer::precomputeTwiddles() {
    // Precompute twiddle factors: W_N^k = e^(-2*pi*i*k/N)
    for (int i = 0; i < mFFTSize / 2; ++i) {
        float angle = -2.0f * M_PI * i / mFFTSize;
        mTwiddles[i] = std::complex<float>(std::cos(angle), std::sin(angle));
    }
}

void SpectrumAnalyzer::analyze(const float* input, int numSamples) {
    // Accumulate samples into input buffer
    for (int i = 0; i < numSamples; ++i) {
        mInputBuffer[mInputWritePos] = input[i];
        mInputWritePos = (mInputWritePos + 1) % mFFTSize;
        mSamplesCollected++;

        // When we have enough samples, perform FFT
        if (mSamplesCollected >= mFFTSize) {
            performFFT();
            computeMagnitudes();
            mSamplesCollected = 0;
            mDataReady.store(true, std::memory_order_relaxed);
        }
    }
}

void SpectrumAnalyzer::performFFT() {
    // Copy input to FFT buffer with windowing
    // Handle circular buffer wraparound
    int readPos = mInputWritePos;  // Start from oldest sample

    for (int i = 0; i < mFFTSize; ++i) {
        float sample = mInputBuffer[readPos] * mWindow[i];
        mFFTBuffer[i] = std::complex<float>(sample, 0.0f);
        readPos = (readPos + 1) % mFFTSize;
    }

    // Perform in-place FFT
    fft(mFFTBuffer.data(), mFFTSize);
}

void SpectrumAnalyzer::fft(std::complex<float>* data, int n) {
    // Bit-reversal permutation
    int bits = 0;
    int temp = n;
    while (temp > 1) {
        temp >>= 1;
        bits++;
    }

    for (int i = 0; i < n; ++i) {
        int j = bitReverse(i, bits);
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }

    // Cooley-Tukey iterative FFT
    for (int len = 2; len <= n; len *= 2) {
        int halfLen = len / 2;
        int step = n / len;

        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < halfLen; ++j) {
                std::complex<float> twiddle = mTwiddles[j * step];
                std::complex<float> u = data[i + j];
                std::complex<float> v = data[i + j + halfLen] * twiddle;

                data[i + j] = u + v;
                data[i + j + halfLen] = u - v;
            }
        }
    }
}

int SpectrumAnalyzer::bitReverse(int n, int bits) {
    int reversed = 0;
    for (int i = 0; i < bits; ++i) {
        reversed = (reversed << 1) | (n & 1);
        n >>= 1;
    }
    return reversed;
}

void SpectrumAnalyzer::computeMagnitudes() {
    int numBins = mFFTSize / 2;
    float normFactor = 2.0f / mFFTSize;  // Normalize FFT output

    for (int i = 0; i < numBins; ++i) {
        // Compute magnitude
        float real = mFFTBuffer[i].real();
        float imag = mFFTBuffer[i].imag();
        float mag = std::sqrt(real * real + imag * imag) * normFactor;

        // Convert to dB
        float magDb = 20.0f * std::log10(std::max(mag, 1e-10f));

        // Clamp to reasonable range
        magDb = std::clamp(magDb, -100.0f, 0.0f);

        // Store raw magnitude
        mMagnitudes[i] = magDb;

        // Apply smoothing for visualization
        mSmoothedMagnitudes[i] = mSmoothing * mSmoothedMagnitudes[i]
                                + (1.0f - mSmoothing) * magDb;

        // Update peak hold
        if (magDb > mPeakMagnitudes[i]) {
            mPeakMagnitudes[i] = magDb;
        } else {
            // Decay peak
            mPeakMagnitudes[i] -= mPeakDecayPerSample * mFFTSize;
            mPeakMagnitudes[i] = std::max(mPeakMagnitudes[i], magDb);
        }
    }
}

float SpectrumAnalyzer::getFrequencyForBin(int binIndex) const {
    if (binIndex < 0 || binIndex >= mFFTSize / 2) {
        return 0.0f;
    }
    return static_cast<float>(binIndex) * mSampleRate / static_cast<float>(mFFTSize);
}

int SpectrumAnalyzer::getBinForFrequency(float frequency) const {
    int bin = static_cast<int>(frequency * mFFTSize / mSampleRate + 0.5f);
    return std::clamp(bin, 0, mFFTSize / 2 - 1);
}
