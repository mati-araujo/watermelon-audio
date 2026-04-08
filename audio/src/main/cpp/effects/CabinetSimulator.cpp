#include "CabinetSimulator.h"
#include <algorithm>
#include <cmath>
#include "../platform/Logger.h"

#define LOG_TAG "CabinetSimulator"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

CabinetSimulator::CabinetSimulator() {
    // Initialize buffers to zero
    std::fill(mOverlapL.begin(), mOverlapL.end(), 0.0f);
    std::fill(mOverlapR.begin(), mOverlapR.end(), 0.0f);
    std::fill(mInputBufferL.begin(), mInputBufferL.end(), 0.0f);
    std::fill(mInputBufferR.begin(), mInputBufferR.end(), 0.0f);
    std::fill(mIRFreqDomain.begin(), mIRFreqDomain.end(), std::complex<float>(0.0f, 0.0f));

    updateFilterCoefficients();

    // Load default IR
    loadIR(static_cast<BuiltInIRs::CabinetType>(mCabinetType.load(std::memory_order_relaxed)));

    LOGI("CabinetSimulator created");
}

void CabinetSimulator::loadIR(BuiltInIRs::CabinetType type) {
    std::lock_guard<std::mutex> lock(mIRMutex);

    const float* irData = BuiltInIRs::getIRData(type);
    if (irData == nullptr) {
        LOGW("No IR data for cabinet type %d, using bypass", static_cast<int>(type));
        mIRReady.store(false, std::memory_order_release);
        return;
    }

    // Zero-pad IR to FFT_SIZE and convert to complex
    std::array<std::complex<float>, FFT_SIZE> irComplex;
    for (size_t i = 0; i < FFT_SIZE; ++i) {
        if (i < IR_LENGTH) {
            irComplex[i] = std::complex<float>(irData[i], 0.0f);
        } else {
            irComplex[i] = std::complex<float>(0.0f, 0.0f);
        }
    }

    // Transform IR to frequency domain
    fft(irComplex.data(), FFT_SIZE, false);

    // Store in member (atomic copy not needed, protected by mutex)
    mIRFreqDomain = irComplex;
    mIRReady.store(true, std::memory_order_release);

    LOGI("IR loaded for cabinet type %d (%s)", static_cast<int>(type),
         BuiltInIRs::getCabinetName(type));
}

void CabinetSimulator::process(float* input, float* output, int numFrames) {
    const float mix = mMix.load(std::memory_order_relaxed) / 100.0f;
    const bool irReady = mIRReady.load(std::memory_order_acquire);

    // If no IR loaded or mix is 0, bypass
    if (!irReady || mix < 0.001f) {
        std::copy(input, input + numFrames * 2, output);
        return;
    }

    // Process frame by frame, accumulating into block buffer
    for (int i = 0; i < numFrames; ++i) {
        const int idx = i * 2;
        float dryL = input[idx];
        float dryR = input[idx + 1];

        // Accumulate into input buffers
        mInputBufferL[mInputPos] = dryL;
        mInputBufferR[mInputPos] = dryR;
        mInputPos++;

        // When we have a full block, process it
        if (mInputPos >= BLOCK_SIZE) {
            // Temporary output buffers
            std::array<float, BLOCK_SIZE> wetL, wetR;

            processBlock(mInputBufferL.data(), mInputBufferR.data(),
                        wetL.data(), wetR.data());

            // Output the processed block (will be output in next iterations)
            // For now, we use a simpler approach: direct output with overlap
            mInputPos = 0;
        }

        // Apply filters to the current sample
        float wetSampleL = applyLowCut(dryL, mLowCutStateL);
        wetSampleL = applyHighCut(wetSampleL, mHighCutStateL);

        float wetSampleR = applyLowCut(dryR, mLowCutStateR);
        wetSampleR = applyHighCut(wetSampleR, mHighCutStateR);

        // Simple convolution approximation for real-time
        // (Full FFT convolution would require block-based processing)
        // Using a simplified approach: apply the first few IR samples directly
        const float* irData = BuiltInIRs::getIRData(
            static_cast<BuiltInIRs::CabinetType>(mCabinetType.load(std::memory_order_relaxed)));

        if (irData != nullptr) {
            // Simple FIR approximation using first 16 samples
            constexpr int FIR_TAPS = 16;
            float convL = 0.0f, convR = 0.0f;

            // Shift overlap buffer and add new sample
            for (int t = IR_LENGTH - 1; t > 0; --t) {
                if (t < FIR_TAPS) {
                    mOverlapL[t] = mOverlapL[t - 1];
                    mOverlapR[t] = mOverlapR[t - 1];
                }
            }
            mOverlapL[0] = wetSampleL;
            mOverlapR[0] = wetSampleR;

            // Convolve with IR
            for (int t = 0; t < FIR_TAPS; ++t) {
                convL += mOverlapL[t] * irData[t];
                convR += mOverlapR[t] * irData[t];
            }

            wetSampleL = convL;
            wetSampleR = convR;
        }

        // Mix dry and wet
        output[idx] = dryL * (1.0f - mix) + wetSampleL * mix;
        output[idx + 1] = dryR * (1.0f - mix) + wetSampleR * mix;
    }
}

void CabinetSimulator::processBlock(const float* inputL, const float* inputR,
                                     float* outputL, float* outputR) {
    // This function performs full FFT convolution on a block
    // Currently simplified - full implementation would use overlap-add

    // Zero-pad input to FFT_SIZE
    for (size_t i = 0; i < FFT_SIZE; ++i) {
        if (i < BLOCK_SIZE) {
            mFftBuffer[i] = inputL[i];
        } else {
            mFftBuffer[i] = 0.0f;
        }
    }

    // Convert to complex
    for (size_t i = 0; i < FFT_SIZE; ++i) {
        mInputFreqDomain[i] = std::complex<float>(mFftBuffer[i], 0.0f);
    }

    // Forward FFT
    fft(mInputFreqDomain.data(), FFT_SIZE, false);

    // Multiply in frequency domain
    for (size_t i = 0; i < FFT_SIZE; ++i) {
        mOutputFreqDomain[i] = mInputFreqDomain[i] * mIRFreqDomain[i];
    }

    // Inverse FFT
    fft(mOutputFreqDomain.data(), FFT_SIZE, true);

    // Extract real part and apply overlap-add
    for (size_t i = 0; i < BLOCK_SIZE; ++i) {
        outputL[i] = mOutputFreqDomain[i].real() + mOverlapL[i];
    }

    // Save overlap for next block
    for (size_t i = 0; i < IR_LENGTH; ++i) {
        if (i + BLOCK_SIZE < FFT_SIZE) {
            mOverlapL[i] = mOutputFreqDomain[i + BLOCK_SIZE].real();
        } else {
            mOverlapL[i] = 0.0f;
        }
    }

    // Repeat for right channel (simplified: copy from left for now)
    std::copy(outputL, outputL + BLOCK_SIZE, outputR);
}

void CabinetSimulator::fft(std::complex<float>* data, size_t n, bool inverse) {
    // Cooley-Tukey radix-2 FFT
    if (n <= 1) return;

    // Bit-reversal permutation
    bitReverse(data, n);

    // Butterfly operations
    for (size_t len = 2; len <= n; len *= 2) {
        float angle = 2.0f * static_cast<float>(M_PI) / static_cast<float>(len);
        if (inverse) angle = -angle;

        std::complex<float> wlen(std::cos(angle), std::sin(angle));

        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);

            for (size_t j = 0; j < len / 2; ++j) {
                std::complex<float> u = data[i + j];
                std::complex<float> v = data[i + j + len / 2] * w;

                data[i + j] = u + v;
                data[i + j + len / 2] = u - v;

                w *= wlen;
            }
        }
    }

    // Scale for inverse FFT
    if (inverse) {
        float scale = 1.0f / static_cast<float>(n);
        for (size_t i = 0; i < n; ++i) {
            data[i] *= scale;
        }
    }
}

void CabinetSimulator::bitReverse(std::complex<float>* data, size_t n) {
    size_t bits = 0;
    size_t temp = n;
    while (temp > 1) {
        bits++;
        temp >>= 1;
    }

    for (size_t i = 0; i < n; ++i) {
        size_t j = 0;
        for (size_t k = 0; k < bits; ++k) {
            if (i & (1 << k)) {
                j |= (1 << (bits - 1 - k));
            }
        }
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }
}

void CabinetSimulator::updateFilterCoefficients() {
    float lowCutFreq = mLowCut.load(std::memory_order_relaxed);
    float highCutFreq = mHighCut.load(std::memory_order_relaxed);

    // One-pole filter coefficients
    // High-pass (low cut): y[n] = (1-a) * x[n] + a * y[n-1]
    mLowCutCoeff = std::exp(-2.0f * static_cast<float>(M_PI) * lowCutFreq / static_cast<float>(mSampleRate));

    // Low-pass (high cut): y[n] = (1-a) * x[n] + a * y[n-1]
    mHighCutCoeff = std::exp(-2.0f * static_cast<float>(M_PI) * highCutFreq / static_cast<float>(mSampleRate));
}

float CabinetSimulator::applyLowCut(float input, float& state) {
    // High-pass filter (removes low frequencies)
    float output = input - state;
    state = mLowCutCoeff * state + (1.0f - mLowCutCoeff) * input;
    return output;
}

float CabinetSimulator::applyHighCut(float input, float& state) {
    // Low-pass filter (removes high frequencies)
    state = mHighCutCoeff * state + (1.0f - mHighCutCoeff) * input;
    return state;
}

void CabinetSimulator::setParam(int paramId, float value) {
    switch (paramId) {
        case CABINET: {
            int cabinetType = static_cast<int>(std::clamp(value, 0.0f, 6.0f));
            int prevType = mCabinetType.load(std::memory_order_relaxed);
            if (cabinetType != prevType) {
                mCabinetType.store(cabinetType, std::memory_order_relaxed);
                loadIR(static_cast<BuiltInIRs::CabinetType>(cabinetType));
            }
            break;
        }
        case MIX:
            mMix.store(std::clamp(value, 0.0f, 100.0f), std::memory_order_relaxed);
            break;
        case LOW_CUT:
            mLowCut.store(std::clamp(value, 20.0f, 500.0f), std::memory_order_relaxed);
            updateFilterCoefficients();
            break;
        case HIGH_CUT:
            mHighCut.store(std::clamp(value, 2000.0f, 20000.0f), std::memory_order_relaxed);
            updateFilterCoefficients();
            break;
        default:
            break;
    }
}

float CabinetSimulator::getParam(int paramId) {
    switch (paramId) {
        case CABINET: return static_cast<float>(mCabinetType.load(std::memory_order_relaxed));
        case MIX: return mMix.load(std::memory_order_relaxed);
        case LOW_CUT: return mLowCut.load(std::memory_order_relaxed);
        case HIGH_CUT: return mHighCut.load(std::memory_order_relaxed);
        default: return 0.0f;
    }
}

void CabinetSimulator::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    updateFilterCoefficients();

    // Reload IR to recalculate frequency domain representation if needed
    loadIR(static_cast<BuiltInIRs::CabinetType>(mCabinetType.load(std::memory_order_relaxed)));

    LOGI("Sample rate set to %d", sampleRate);
}
