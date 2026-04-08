#ifndef CABINETSIMULATOR_H
#define CABINETSIMULATOR_H

#include "Effect.h"
#include "BuiltInIRs.h"
#include <atomic>
#include <array>
#include <vector>
#include <complex>
#include <mutex>

/**
 * @file CabinetSimulator.h
 * @brief Cabinet impulse response convolution effect
 *
 * Features:
 * - FFT-based convolution with overlap-add processing
 * - 6 built-in cabinet IRs (Fender, Vox, Marshall, Mesa, Bass)
 * - Low/High cut filtering for tone shaping
 * - Wet/dry mix control
 *
 * Architecture:
 * Input → Convolution (FFT) → Low/High Cut → Mix → Output
 *           (Overlap-Add)     (Filtering)   (Dry/Wet)
 *
 * Performance:
 * - IR pre-transformed to frequency domain at load time
 * - Overlap-add for artifact-free processing
 * - Single-precision FFT for efficiency
 *
 * Thread-safe: All parameters use atomic operations.
 * IR loading uses mutex (only at parameter change, not in process).
 */
class CabinetSimulator : public Effect {
public:
    /**
     * @brief Parameter IDs (0-3)
     */
    enum Param {
        CABINET = 0,    ///< Cabinet selection [0-6]
        MIX = 1,        ///< Wet/dry mix [0-100]%
        LOW_CUT = 2,    ///< High-pass frequency [20-500] Hz
        HIGH_CUT = 3,   ///< Low-pass frequency [2000-20000] Hz
        PARAM_COUNT = 4
    };

    // FFT/IR constants
    static constexpr size_t IR_LENGTH = BuiltInIRs::IR_LENGTH;  // 512 samples
    static constexpr size_t FFT_SIZE = 1024;  // Next power of 2 for IR_LENGTH
    static constexpr size_t BLOCK_SIZE = FFT_SIZE - IR_LENGTH;  // 512 samples

    CabinetSimulator();
    ~CabinetSimulator() override = default;

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;

private:
    int mSampleRate{48000};

    // Parameters (atomic for RT safety)
    std::atomic<int> mCabinetType{static_cast<int>(BuiltInIRs::CabinetType::MARSHALL_4X12_STRAIGHT)};
    std::atomic<float> mMix{100.0f};
    std::atomic<float> mLowCut{80.0f};
    std::atomic<float> mHighCut{12000.0f};

    // FFT buffers (pre-allocated)
    std::array<std::complex<float>, FFT_SIZE> mIRFreqDomain;  // IR in frequency domain
    std::array<std::complex<float>, FFT_SIZE> mInputFreqDomain;  // Input FFT
    std::array<std::complex<float>, FFT_SIZE> mOutputFreqDomain;  // Output FFT
    std::array<float, FFT_SIZE> mFftBuffer;  // Working buffer
    std::array<float, FFT_SIZE> mIfftBuffer;  // IFFT output

    // Overlap-add buffers (stereo)
    std::array<float, IR_LENGTH> mOverlapL;  // Left channel overlap
    std::array<float, IR_LENGTH> mOverlapR;  // Right channel overlap
    std::array<float, BLOCK_SIZE> mInputBufferL;  // Left input accumulator
    std::array<float, BLOCK_SIZE> mInputBufferR;  // Right input accumulator
    size_t mInputPos = 0;  // Current position in input buffer

    // One-pole filter state (low/high cut)
    float mLowCutStateL = 0.0f;
    float mLowCutStateR = 0.0f;
    float mHighCutStateL = 0.0f;
    float mHighCutStateR = 0.0f;
    float mLowCutCoeff = 0.0f;
    float mHighCutCoeff = 0.0f;

    // IR loading mutex (only for loadIR, not process)
    std::mutex mIRMutex;
    std::atomic<bool> mIRReady{false};

    // Load IR into frequency domain
    void loadIR(BuiltInIRs::CabinetType type);

    // FFT operations (Cooley-Tukey radix-2)
    void fft(std::complex<float>* data, size_t n, bool inverse);
    void bitReverse(std::complex<float>* data, size_t n);

    // Process a single block through convolution
    void processBlock(const float* inputL, const float* inputR,
                      float* outputL, float* outputR);

    // Update filter coefficients
    void updateFilterCoefficients();

    // Apply one-pole filters
    float applyLowCut(float input, float& state);
    float applyHighCut(float input, float& state);
};

#endif // CABINETSIMULATOR_H
