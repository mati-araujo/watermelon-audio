/**
 * AudioFormatConverter.h
 *
 * High-performance audio format conversion for USB Audio.
 *
 * Converts between:
 * - float (internal DSP format, range -1.0 to 1.0)
 * - PCM 16-bit signed (USB common format)
 * - PCM 24-bit signed (USB pro audio format)
 * - PCM 32-bit signed (USB high-resolution format)
 *
 * Design:
 * - Optimized for real-time audio (no allocations in hot path)
 * - SIMD-friendly structure for future vectorization
 * - Proper dithering support for bit-depth reduction
 * - Handles both interleaved and planar formats
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <random>

namespace watermelon_audio {
namespace usb {

// ============================================================================
// Audio Format Enums
// ============================================================================

enum class PcmFormat {
    PCM_S16_LE,     // Signed 16-bit little-endian
    PCM_S24_LE,     // Signed 24-bit little-endian (3 bytes)
    PCM_S24_3LE,    // Signed 24-bit little-endian, packed 3 bytes
    PCM_S24_4LE,    // Signed 24-bit in 4 bytes little-endian (MSB padded)
    PCM_S32_LE      // Signed 32-bit little-endian
};

// ============================================================================
// Format Info Utilities
// ============================================================================

struct PcmFormatInfo {
    int bytesPerSample;
    int bitsPerSample;
    bool isPacked;      // true for 24-bit in 3 bytes

    static PcmFormatInfo get(PcmFormat format) {
        switch (format) {
            case PcmFormat::PCM_S16_LE:
                return {2, 16, false};
            case PcmFormat::PCM_S24_LE:
            case PcmFormat::PCM_S24_3LE:
                return {3, 24, true};
            case PcmFormat::PCM_S24_4LE:
                return {4, 24, false};
            case PcmFormat::PCM_S32_LE:
                return {4, 32, false};
            default:
                return {2, 16, false};
        }
    }
};

/**
 * Map a wire bit depth (as reported by the UAC format descriptor) to the packed
 * PCM format the converter expects. Single source of truth: negotiation code in
 * LibusbBackend and any future caller must derive the format through here rather
 * than reimplementing the switch (H7 — this used to be duplicated inline).
 *
 * 24-bit maps to the packed 3-byte layout (PCM_S24_3LE) because that is how UAC
 * lays 24-bit samples on the wire; the 4-byte container variants are only used
 * internally by the converter.
 */
inline PcmFormat pcmFormatForBitDepth(int bitDepth) {
    switch (bitDepth) {
        case 16: return PcmFormat::PCM_S16_LE;
        case 24: return PcmFormat::PCM_S24_3LE;
        case 32: return PcmFormat::PCM_S32_LE;
        default: return PcmFormat::PCM_S16_LE;
    }
}

// ============================================================================
// Dithering
// ============================================================================

/**
 * TPDF (Triangular Probability Density Function) Dithering
 *
 * Generates triangular-distributed noise for smooth quantization.
 * Uses a simple, fast PRNG suitable for real-time audio.
 */
class TpdfDither {
public:
    TpdfDither() : mState(0x12345678) {}

    /**
     * Get dither value scaled for target bit depth.
     * Returns value in range [-1, 1] scaled appropriately.
     */
    inline float get(int targetBits) {
        // Generate two uniform random values
        float r1 = nextRandom();
        float r2 = nextRandom();

        // TPDF: sum of two uniform = triangular distribution
        float tpdf = r1 + r2 - 1.0f;

        // Scale for bit depth (dither should be ±0.5 LSB)
        float scale = 1.0f / static_cast<float>(1 << targetBits);
        return tpdf * scale;
    }

    void reset() { mState = 0x12345678; }

private:
    inline float nextRandom() {
        // Fast xorshift PRNG
        mState ^= mState << 13;
        mState ^= mState >> 17;
        mState ^= mState << 5;
        // Convert to [0, 1)
        return static_cast<float>(mState) * (1.0f / 4294967296.0f);
    }

    uint32_t mState;
};

// ============================================================================
// AudioFormatConverter Class
// ============================================================================

class AudioFormatConverter {
public:
    AudioFormatConverter();
    ~AudioFormatConverter() = default;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * Enable/disable dithering for bit-depth reduction.
     * Dithering improves perceived quality when going from float to 16-bit.
     */
    void setDitheringEnabled(bool enabled) { mDitheringEnabled = enabled; }
    bool isDitheringEnabled() const { return mDitheringEnabled; }

    /**
     * Set soft clipping threshold.
     * Values above this are soft-clipped to prevent harsh digital clipping.
     * Default: 0.95 (leaves some headroom)
     */
    void setSoftClipThreshold(float threshold) { mSoftClipThreshold = threshold; }

    // ========================================================================
    // Float to PCM Conversion (for USB output)
    // ========================================================================

    /**
     * Convert float samples to PCM format.
     *
     * @param input     Input float samples (range -1.0 to 1.0)
     * @param output    Output buffer for PCM data
     * @param numSamples Number of samples to convert
     * @param format    Target PCM format
     */
    void floatToPcm(
        const float* input,
        uint8_t* output,
        size_t numSamples,
        PcmFormat format
    );

    /**
     * Convert float samples to 16-bit PCM (optimized path).
     */
    void floatToS16(const float* input, int16_t* output, size_t numSamples);

    /**
     * Convert float samples to 24-bit PCM (3 bytes per sample).
     */
    void floatToS24_3LE(const float* input, uint8_t* output, size_t numSamples);

    /**
     * Convert float samples to 24-bit PCM in 4-byte container.
     */
    void floatToS24_4LE(const float* input, int32_t* output, size_t numSamples);

    /**
     * Convert float samples to 32-bit PCM.
     */
    void floatToS32(const float* input, int32_t* output, size_t numSamples);

    // ========================================================================
    // PCM to Float Conversion (for USB input)
    // ========================================================================

    /**
     * Convert PCM format to float samples.
     *
     * @param input     Input PCM data
     * @param output    Output float buffer
     * @param numSamples Number of samples to convert
     * @param format    Source PCM format
     */
    void pcmToFloat(
        const uint8_t* input,
        float* output,
        size_t numSamples,
        PcmFormat format
    );

    /**
     * Convert 16-bit PCM to float (optimized path).
     */
    void s16ToFloat(const int16_t* input, float* output, size_t numSamples);

    /**
     * Convert 24-bit PCM (3 bytes per sample) to float.
     */
    void s24_3LEToFloat(const uint8_t* input, float* output, size_t numSamples);

    /**
     * Convert 24-bit PCM in 4-byte container to float.
     */
    void s24_4LEToFloat(const int32_t* input, float* output, size_t numSamples);

    /**
     * Convert 32-bit PCM to float.
     */
    void s32ToFloat(const int32_t* input, float* output, size_t numSamples);

    // ========================================================================
    // Utility Functions
    // ========================================================================

    /**
     * Get the number of bytes needed for a given format and sample count.
     */
    static size_t getBytesForSamples(PcmFormat format, size_t numSamples);

    /**
     * Get bytes per sample for a format.
     */
    static int getBytesPerSample(PcmFormat format);

    /**
     * Convert between formats (PCM to PCM).
     * Useful for format negotiation.
     */
    void convertFormat(
        const uint8_t* input,
        uint8_t* output,
        size_t numSamples,
        PcmFormat inputFormat,
        PcmFormat outputFormat
    );

private:
    // Apply soft clipping to prevent harsh clipping
    inline float softClip(float sample) const {
        if (sample > mSoftClipThreshold) {
            float excess = sample - mSoftClipThreshold;
            return mSoftClipThreshold + std::tanh(excess * 2.0f) * (1.0f - mSoftClipThreshold);
        } else if (sample < -mSoftClipThreshold) {
            float excess = sample + mSoftClipThreshold;
            return -mSoftClipThreshold + std::tanh(excess * 2.0f) * (1.0f - mSoftClipThreshold);
        }
        return sample;
    }

    // Clamp to valid range
    inline float clamp(float value, float min, float max) const {
        return std::min(std::max(value, min), max);
    }

    bool mDitheringEnabled = true;
    float mSoftClipThreshold = 0.95f;
    TpdfDither mDither;
};

// ============================================================================
// Inline Implementations for Performance-Critical Functions
// ============================================================================

inline void AudioFormatConverter::floatToS16(
    const float* input,
    int16_t* output,
    size_t numSamples
) {
    constexpr float scale = 32767.0f;

    if (mDitheringEnabled) {
        for (size_t i = 0; i < numSamples; ++i) {
            float sample = softClip(input[i]);
            sample += mDither.get(16);
            sample = clamp(sample, -1.0f, 1.0f);
            output[i] = static_cast<int16_t>(sample * scale);
        }
    } else {
        for (size_t i = 0; i < numSamples; ++i) {
            float sample = clamp(softClip(input[i]), -1.0f, 1.0f);
            output[i] = static_cast<int16_t>(sample * scale);
        }
    }
}

inline void AudioFormatConverter::s16ToFloat(
    const int16_t* input,
    float* output,
    size_t numSamples
) {
    constexpr float scale = 1.0f / 32768.0f;

    for (size_t i = 0; i < numSamples; ++i) {
        output[i] = static_cast<float>(input[i]) * scale;
    }
}

inline void AudioFormatConverter::floatToS24_3LE(
    const float* input,
    uint8_t* output,
    size_t numSamples
) {
    constexpr float scale = 8388607.0f;  // 2^23 - 1

    for (size_t i = 0; i < numSamples; ++i) {
        float sample = clamp(softClip(input[i]), -1.0f, 1.0f);
        int32_t value = static_cast<int32_t>(sample * scale);

        // Store as little-endian 3 bytes
        uint8_t* p = output + (i * 3);
        p[0] = static_cast<uint8_t>(value & 0xFF);
        p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    }
}

inline void AudioFormatConverter::s24_3LEToFloat(
    const uint8_t* input,
    float* output,
    size_t numSamples
) {
    constexpr float scale = 1.0f / 8388608.0f;  // 1 / 2^23

    for (size_t i = 0; i < numSamples; ++i) {
        const uint8_t* p = input + (i * 3);

        // Sign-extend from 24-bit to 32-bit
        int32_t value = static_cast<int32_t>(p[0]) |
                       (static_cast<int32_t>(p[1]) << 8) |
                       (static_cast<int32_t>(p[2]) << 16);

        // Sign extension if MSB is set
        if (value & 0x800000) {
            value |= 0xFF000000;
        }

        output[i] = static_cast<float>(value) * scale;
    }
}

inline void AudioFormatConverter::floatToS24_4LE(
    const float* input,
    int32_t* output,
    size_t numSamples
) {
    constexpr float scale = 8388607.0f;  // 2^23 - 1

    for (size_t i = 0; i < numSamples; ++i) {
        float sample = clamp(softClip(input[i]), -1.0f, 1.0f);
        int32_t value = static_cast<int32_t>(sample * scale);
        // Store in upper 24 bits of 32-bit word
        output[i] = value << 8;
    }
}

inline void AudioFormatConverter::s24_4LEToFloat(
    const int32_t* input,
    float* output,
    size_t numSamples
) {
    constexpr float scale = 1.0f / 8388608.0f;  // 1 / 2^23

    for (size_t i = 0; i < numSamples; ++i) {
        // Value is in upper 24 bits
        int32_t value = input[i] >> 8;
        output[i] = static_cast<float>(value) * scale;
    }
}

inline void AudioFormatConverter::floatToS32(
    const float* input,
    int32_t* output,
    size_t numSamples
) {
    constexpr float scale = 2147483647.0f;  // 2^31 - 1

    for (size_t i = 0; i < numSamples; ++i) {
        float sample = clamp(softClip(input[i]), -1.0f, 1.0f);
        output[i] = static_cast<int32_t>(sample * scale);
    }
}

inline void AudioFormatConverter::s32ToFloat(
    const int32_t* input,
    float* output,
    size_t numSamples
) {
    constexpr float scale = 1.0f / 2147483648.0f;  // 1 / 2^31

    for (size_t i = 0; i < numSamples; ++i) {
        output[i] = static_cast<float>(input[i]) * scale;
    }
}

} // namespace usb
} // namespace watermelon_audio
