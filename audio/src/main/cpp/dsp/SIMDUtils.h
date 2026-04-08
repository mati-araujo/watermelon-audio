#pragma once

/**
 * @file SIMDUtils.h
 * @brief SIMD-optimized audio processing utilities using ARM NEON
 *
 * These functions provide vectorized implementations of common audio DSP
 * operations, achieving 2-4x performance improvement over scalar code.
 *
 * All functions have scalar fallbacks for non-NEON platforms.
 *
 * Thread Safety: All functions are stateless and thread-safe.
 * RT-Safety: All functions are lock-free and suitable for audio callbacks.
 */

#include <cstdint>
#include <cmath>
#include <algorithm>

#if defined(USE_NEON) && (defined(__aarch64__) || defined(__arm__))
#include <arm_neon.h>
#define SIMD_NEON_AVAILABLE 1
// FMA instructions are only available on ARMv8 (64-bit)
#if defined(__aarch64__)
#define SIMD_FMA_AVAILABLE 1
#else
#define SIMD_FMA_AVAILABLE 0
#endif
#else
#define SIMD_NEON_AVAILABLE 0
#define SIMD_FMA_AVAILABLE 0
#endif

// Helper macros for portable FMA operations
// vfmaq_f32(a, b, c) computes a + b * c
#if SIMD_FMA_AVAILABLE
#define SIMD_FMAQ_F32(a, b, c) vfmaq_f32(a, b, c)
#define SIMD_FMA_F32(a, b, c) vfma_f32(a, b, c)
#else
// ARMv7 fallback: separate multiply and add
#define SIMD_FMAQ_F32(a, b, c) vaddq_f32(a, vmulq_f32(b, c))
#define SIMD_FMA_F32(a, b, c) vadd_f32(a, vmul_f32(b, c))
#endif

namespace simd {

// ========== GAIN AND MIXING OPERATIONS ==========

/**
 * Apply a linear gain ramp to a stereo buffer (interleaved L/R samples)
 * Smoothly interpolates gain from gainStart to gainEnd across the buffer.
 *
 * @param buffer Interleaved stereo buffer [L0,R0,L1,R1,...]
 * @param numFrames Number of stereo frames
 * @param gainStart Gain at start of buffer
 * @param gainEnd Gain at end of buffer
 */
inline void applyStereoGainRamp(float* buffer, int32_t numFrames,
                                 float gainStart, float gainEnd) {
    if (numFrames <= 0) return;

    const float gainInc = (gainEnd - gainStart) / static_cast<float>(numFrames);
    const int32_t totalSamples = numFrames * 2;

#if SIMD_NEON_AVAILABLE
    // Process 4 samples (2 stereo frames) at a time
    float gain = gainStart;
    int32_t i = 0;

    // NEON: Process 4 samples per iteration
    for (; i <= totalSamples - 4; i += 4) {
        // Create gain vector for 2 frames: [g0, g0, g0+inc, g0+inc]
        float g0 = gain;
        float g1 = gain + gainInc;
        float32x4_t vGain = {g0, g0, g1, g1};

        // Load 4 samples, multiply by gain, store
        float32x4_t vSamples = vld1q_f32(buffer + i);
        vSamples = vmulq_f32(vSamples, vGain);
        vst1q_f32(buffer + i, vSamples);

        gain += gainInc * 2.0f;  // Advance by 2 frames
    }

    // Scalar fallback for remaining samples
    for (; i < totalSamples; i += 2) {
        buffer[i] *= gain;
        buffer[i + 1] *= gain;
        gain += gainInc;
    }
#else
    // Scalar implementation
    float gain = gainStart;
    for (int32_t i = 0; i < totalSamples; i += 2) {
        buffer[i] *= gain;
        buffer[i + 1] *= gain;
        gain += gainInc;
    }
#endif
}

/**
 * Apply constant gain to a stereo buffer
 *
 * @param buffer Interleaved stereo buffer
 * @param numFrames Number of stereo frames
 * @param gain Gain to apply
 */
inline void applyStereoGain(float* buffer, int32_t numFrames, float gain) {
    const int32_t totalSamples = numFrames * 2;

#if SIMD_NEON_AVAILABLE
    float32x4_t vGain = vdupq_n_f32(gain);
    int32_t i = 0;

    for (; i <= totalSamples - 4; i += 4) {
        float32x4_t vSamples = vld1q_f32(buffer + i);
        vSamples = vmulq_f32(vSamples, vGain);
        vst1q_f32(buffer + i, vSamples);
    }

    for (; i < totalSamples; ++i) {
        buffer[i] *= gain;
    }
#else
    for (int32_t i = 0; i < totalSamples; ++i) {
        buffer[i] *= gain;
    }
#endif
}

/**
 * Mix two stereo buffers with individual gains: out = a * gainA + b * gainB
 *
 * @param out Output buffer (can be same as a or b for in-place)
 * @param a First input buffer
 * @param b Second input buffer
 * @param gainA Gain for buffer a
 * @param gainB Gain for buffer b
 * @param numFrames Number of stereo frames
 */
inline void mixStereoBuffers(float* out, const float* a, const float* b,
                              float gainA, float gainB, int32_t numFrames) {
    const int32_t totalSamples = numFrames * 2;

#if SIMD_NEON_AVAILABLE
    float32x4_t vGainA = vdupq_n_f32(gainA);
    float32x4_t vGainB = vdupq_n_f32(gainB);
    int32_t i = 0;

    for (; i <= totalSamples - 4; i += 4) {
        float32x4_t vA = vld1q_f32(a + i);
        float32x4_t vB = vld1q_f32(b + i);

        // result = a * gainA + b * gainB
        float32x4_t result = vmulq_f32(vA, vGainA);
        result = SIMD_FMAQ_F32(result, vB, vGainB);  // Portable FMA

        vst1q_f32(out + i, result);
    }

    for (; i < totalSamples; ++i) {
        out[i] = a[i] * gainA + b[i] * gainB;
    }
#else
    for (int32_t i = 0; i < totalSamples; ++i) {
        out[i] = a[i] * gainA + b[i] * gainB;
    }
#endif
}

/**
 * Add two stereo buffers: out = a + b (with optional -6dB headroom)
 *
 * @param out Output buffer
 * @param a First input buffer
 * @param b Second input buffer
 * @param numFrames Number of stereo frames
 * @param applyHeadroom If true, applies -6dB (0.5x) to prevent clipping
 */
inline void addStereoBuffers(float* out, const float* a, const float* b,
                              int32_t numFrames, bool applyHeadroom = true) {
    const float gain = applyHeadroom ? 0.5f : 1.0f;
    const int32_t totalSamples = numFrames * 2;

#if SIMD_NEON_AVAILABLE
    float32x4_t vGain = vdupq_n_f32(gain);
    int32_t i = 0;

    for (; i <= totalSamples - 4; i += 4) {
        float32x4_t vA = vld1q_f32(a + i);
        float32x4_t vB = vld1q_f32(b + i);
        float32x4_t sum = vaddq_f32(vA, vB);
        sum = vmulq_f32(sum, vGain);
        vst1q_f32(out + i, sum);
    }

    for (; i < totalSamples; ++i) {
        out[i] = (a[i] + b[i]) * gain;
    }
#else
    for (int32_t i = 0; i < totalSamples; ++i) {
        out[i] = (a[i] + b[i]) * gain;
    }
#endif
}

// ========== SOFT CLIPPING ==========

/**
 * Apply soft clipping to a stereo buffer using tanh approximation
 * Uses fast polynomial approximation: x * (27 + x²) / (27 + 9x²)
 *
 * This provides warm, musical saturation that prevents harsh digital clipping.
 *
 * @param buffer Interleaved stereo buffer (modified in-place)
 * @param numFrames Number of stereo frames
 */
inline void softClipStereo(float* buffer, int32_t numFrames) {
    const int32_t totalSamples = numFrames * 2;

#if SIMD_NEON_AVAILABLE
    const float32x4_t v27 = vdupq_n_f32(27.0f);
    const float32x4_t v9 = vdupq_n_f32(9.0f);
    const float32x4_t vMin = vdupq_n_f32(-1.0f);
    const float32x4_t vMax = vdupq_n_f32(1.0f);
    int32_t i = 0;

    for (; i <= totalSamples - 4; i += 4) {
        float32x4_t x = vld1q_f32(buffer + i);
        float32x4_t x2 = vmulq_f32(x, x);

        // numerator = x * (27 + x²)
        float32x4_t num = vmulq_f32(x, vaddq_f32(v27, x2));

        // denominator = 27 + 9x²
        float32x4_t den = SIMD_FMAQ_F32(v27, v9, x2);

        // Fast reciprocal with one Newton-Raphson iteration
        float32x4_t invDen = vrecpeq_f32(den);
        invDen = vmulq_f32(invDen, vrecpsq_f32(den, invDen));

        float32x4_t result = vmulq_f32(num, invDen);

        // Clamp to [-1, 1]
        result = vmaxq_f32(vMin, vminq_f32(vMax, result));

        vst1q_f32(buffer + i, result);
    }

    // Scalar fallback
    for (; i < totalSamples; ++i) {
        float x = buffer[i];
        float x2 = x * x;
        float result = x * (27.0f + x2) / (27.0f + 9.0f * x2);
        buffer[i] = std::clamp(result, -1.0f, 1.0f);
    }
#else
    for (int32_t i = 0; i < totalSamples; ++i) {
        float x = buffer[i];
        float x2 = x * x;
        float result = x * (27.0f + x2) / (27.0f + 9.0f * x2);
        buffer[i] = std::clamp(result, -1.0f, 1.0f);
    }
#endif
}

/**
 * Hard limit stereo buffer to [-1, 1] range
 *
 * @param buffer Interleaved stereo buffer (modified in-place)
 * @param numFrames Number of stereo frames
 */
inline void hardLimitStereo(float* buffer, int32_t numFrames) {
    const int32_t totalSamples = numFrames * 2;

#if SIMD_NEON_AVAILABLE
    const float32x4_t vMin = vdupq_n_f32(-1.0f);
    const float32x4_t vMax = vdupq_n_f32(1.0f);
    int32_t i = 0;

    for (; i <= totalSamples - 4; i += 4) {
        float32x4_t v = vld1q_f32(buffer + i);
        v = vmaxq_f32(vMin, vminq_f32(vMax, v));
        vst1q_f32(buffer + i, v);
    }

    for (; i < totalSamples; ++i) {
        buffer[i] = std::clamp(buffer[i], -1.0f, 1.0f);
    }
#else
    for (int32_t i = 0; i < totalSamples; ++i) {
        buffer[i] = std::clamp(buffer[i], -1.0f, 1.0f);
    }
#endif
}

// ========== DC BLOCKING ==========

/**
 * SIMD-optimized stereo DC blocker (first-order highpass filter)
 * Processes both L and R channels simultaneously using NEON
 *
 * Filter: y[n] = x[n] - x[n-1] + R * y[n-1]
 * where R = 1 - (2π * cutoff / sampleRate)
 */
class SIMDDCBlocker {
public:
    /**
     * Set the cutoff frequency for the DC blocker
     * @param cutoffHz Cutoff frequency in Hz (typically 3-20 Hz)
     * @param sampleRate Sample rate in Hz
     */
    void setCutoff(float cutoffHz, float sampleRate) {
        mR = 1.0f - (2.0f * 3.14159265358979f * cutoffHz / sampleRate);
        mR = std::clamp(mR, 0.9f, 0.9999f);  // Ensure stability
    }

    /**
     * Process a stereo buffer through the DC blocker
     * @param buffer Interleaved stereo buffer (modified in-place)
     * @param numFrames Number of stereo frames
     */
    void process(float* buffer, int32_t numFrames) {
#if SIMD_NEON_AVAILABLE
        // Process L and R channels together using 2-wide vectors
        float32x2_t vPrevIn = vld1_f32(mPrevInput);
        float32x2_t vPrevOut = vld1_f32(mPrevOutput);
        float32x2_t vR = vdup_n_f32(mR);

        for (int32_t i = 0; i < numFrames; ++i) {
            float32x2_t vIn = vld1_f32(buffer + i * 2);

            // y = x - prevX + R * prevY
            float32x2_t vDiff = vsub_f32(vIn, vPrevIn);
            float32x2_t vOut = SIMD_FMA_F32(vDiff, vR, vPrevOut);

            vst1_f32(buffer + i * 2, vOut);

            vPrevIn = vIn;
            vPrevOut = vOut;
        }

        vst1_f32(mPrevInput, vPrevIn);
        vst1_f32(mPrevOutput, vPrevOut);
#else
        // Scalar implementation
        for (int32_t i = 0; i < numFrames; ++i) {
            // Left channel
            float inL = buffer[i * 2];
            float outL = inL - mPrevInput[0] + mR * mPrevOutput[0];
            buffer[i * 2] = outL;
            mPrevInput[0] = inL;
            mPrevOutput[0] = outL;

            // Right channel
            float inR = buffer[i * 2 + 1];
            float outR = inR - mPrevInput[1] + mR * mPrevOutput[1];
            buffer[i * 2 + 1] = outR;
            mPrevInput[1] = inR;
            mPrevOutput[1] = outR;
        }
#endif
    }

    /**
     * Reset the filter state (call when audio restarts)
     */
    void reset() {
        mPrevInput[0] = mPrevInput[1] = 0.0f;
        mPrevOutput[0] = mPrevOutput[1] = 0.0f;
    }

private:
    float mPrevInput[2] = {0.0f, 0.0f};
    float mPrevOutput[2] = {0.0f, 0.0f};
    float mR = 0.995f;  // Default ~3Hz cutoff at 48kHz
};

// ========== BUFFER UTILITIES ==========

/**
 * Clear a buffer to zero
 * @param buffer Buffer to clear
 * @param numSamples Number of samples to clear
 */
inline void clearBuffer(float* buffer, int32_t numSamples) {
#if SIMD_NEON_AVAILABLE
    const float32x4_t vZero = vdupq_n_f32(0.0f);
    int32_t i = 0;

    for (; i <= numSamples - 4; i += 4) {
        vst1q_f32(buffer + i, vZero);
    }

    for (; i < numSamples; ++i) {
        buffer[i] = 0.0f;
    }
#else
    std::fill(buffer, buffer + numSamples, 0.0f);
#endif
}

/**
 * Copy buffer with gain
 * @param dst Destination buffer
 * @param src Source buffer
 * @param gain Gain to apply
 * @param numSamples Number of samples
 */
inline void copyWithGain(float* dst, const float* src, float gain, int32_t numSamples) {
#if SIMD_NEON_AVAILABLE
    float32x4_t vGain = vdupq_n_f32(gain);
    int32_t i = 0;

    for (; i <= numSamples - 4; i += 4) {
        float32x4_t v = vld1q_f32(src + i);
        v = vmulq_f32(v, vGain);
        vst1q_f32(dst + i, v);
    }

    for (; i < numSamples; ++i) {
        dst[i] = src[i] * gain;
    }
#else
    for (int32_t i = 0; i < numSamples; ++i) {
        dst[i] = src[i] * gain;
    }
#endif
}

// ========== ANALYSIS UTILITIES ==========

/**
 * Find peak absolute value in a buffer
 * @param buffer Input buffer
 * @param numSamples Number of samples
 * @return Peak absolute value
 */
inline float findPeak(const float* buffer, int32_t numSamples) {
#if SIMD_NEON_AVAILABLE
    float32x4_t vMax = vdupq_n_f32(0.0f);
    int32_t i = 0;

    for (; i <= numSamples - 4; i += 4) {
        float32x4_t v = vld1q_f32(buffer + i);
        v = vabsq_f32(v);
        vMax = vmaxq_f32(vMax, v);
    }

    // Reduce vector to scalar
    float32x2_t vMax2 = vpmax_f32(vget_low_f32(vMax), vget_high_f32(vMax));
    vMax2 = vpmax_f32(vMax2, vMax2);
    float peak = vget_lane_f32(vMax2, 0);

    // Handle remaining samples
    for (; i < numSamples; ++i) {
        peak = std::max(peak, std::abs(buffer[i]));
    }

    return peak;
#else
    float peak = 0.0f;
    for (int32_t i = 0; i < numSamples; ++i) {
        peak = std::max(peak, std::abs(buffer[i]));
    }
    return peak;
#endif
}

/**
 * Calculate RMS (Root Mean Square) level of a buffer
 * @param buffer Input buffer
 * @param numSamples Number of samples
 * @return RMS value
 */
inline float calculateRMS(const float* buffer, int32_t numSamples) {
    if (numSamples <= 0) return 0.0f;

#if SIMD_NEON_AVAILABLE
    float32x4_t vSum = vdupq_n_f32(0.0f);
    int32_t i = 0;

    for (; i <= numSamples - 4; i += 4) {
        float32x4_t v = vld1q_f32(buffer + i);
        vSum = SIMD_FMAQ_F32(vSum, v, v);  // sum += v * v
    }

    // Reduce vector to scalar
    float32x2_t vSum2 = vadd_f32(vget_low_f32(vSum), vget_high_f32(vSum));
    float sum = vget_lane_f32(vSum2, 0) + vget_lane_f32(vSum2, 1);

    // Handle remaining samples
    for (; i < numSamples; ++i) {
        sum += buffer[i] * buffer[i];
    }

    return std::sqrt(sum / numSamples);
#else
    float sum = 0.0f;
    for (int32_t i = 0; i < numSamples; ++i) {
        sum += buffer[i] * buffer[i];
    }
    return std::sqrt(sum / numSamples);
#endif
}

} // namespace simd
