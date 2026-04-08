#ifndef STEREOTOOLS_H
#define STEREOTOOLS_H

#include <vector>
#include <cmath>
#include <utility>
#include "DSPMath.h"

/**
 * @file StereoTools.h
 * @brief Utilities for stereo field manipulation
 *
 * Provides tools for:
 * - Stereo width control
 * - Mid/Side encoding and decoding
 * - Decorrelation (making mono signal sound stereo)
 * - Haas effect (psychoacoustic stereo widening)
 *
 * Used in reverbs, chorus, and stereo enhancement effects
 */

/**
 * @brief Mid/Side encoder and decoder
 *
 * Mid/Side encoding allows independent processing of:
 * - Mid: (L + R) / 2 (center/mono content)
 * - Side: (L - R) / 2 (stereo/width content)
 */
class MidSideProcessor {
public:
    /**
     * @brief Encode L/R to Mid/Side
     * @param left Left channel input
     * @param right Right channel input
     * @return Pair of (mid, side)
     */
    static std::pair<float, float> encode(float left, float right);

    /**
     * @brief Decode Mid/Side to L/R
     * @param mid Mid signal
     * @param side Side signal
     * @return Pair of (left, right)
     */
    static std::pair<float, float> decode(float mid, float side);

    /**
     * @brief Adjust stereo width using Mid/Side
     * @param left Left channel input
     * @param right Right channel input
     * @param width Width factor (0.0 = mono, 1.0 = normal, 2.0 = extra wide)
     * @return Pair of (left, right) with adjusted width
     *
     * Width < 1.0 = narrower stereo
     * Width = 1.0 = no change
     * Width > 1.0 = wider stereo (be careful with > 1.5)
     */
    static std::pair<float, float> adjustWidth(float left, float right, float width);
};

/**
 * @brief Stereo decorrelator using allpass filters
 *
 * Creates decorrelated stereo from mono source by applying different
 * allpass filters to left and right channels. Preserves mono compatibility.
 *
 * Used in:
 * - Reverb (stereo from mono reverb tail)
 * - Chorus (stereo widening)
 * - Ambience effects
 */
class StereoDecorrelator {
public:
    /**
     * @brief Constructor
     * @param sampleRate Sample rate in Hz
     */
    explicit StereoDecorrelator(float sampleRate = 48000.0f);

    /**
     * @brief Set sample rate and reinitialize buffers
     * @param sampleRate Sample rate in Hz
     */
    void setSampleRate(float sampleRate);

    /**
     * @brief Set decorrelation amount
     * @param width Stereo width (0.0 = mono, 1.0 = full decorrelation)
     */
    void setWidth(float width);

    /**
     * @brief Get current width setting
     */
    float getWidth() const { return mWidth; }

    /**
     * @brief Process mono input to decorrelated stereo
     * @param monoInput Mono input sample
     * @return Pair of (left, right) decorrelated outputs
     *
     * RT-safe: O(1) operation
     */
    std::pair<float, float> process(float monoInput);

    /**
     * @brief Process stereo input with decorrelation
     * @param left Left input
     * @param right Right input
     * @return Pair of (left, right) with enhanced stereo image
     *
     * Decorrelates the stereo difference signal
     */
    std::pair<float, float> processStereo(float left, float right);

    /**
     * @brief Reset filter state
     */
    void reset();

private:
    float mSampleRate{48000.0f};
    float mWidth{1.0f};
    float mAllpassGain{0.7f};

    // Allpass delay lines for L and R (different lengths for decorrelation)
    std::vector<float> mAllpassBufferL;
    std::vector<float> mAllpassBufferR;
    int mAllpassPosL{0};
    int mAllpassPosR{0};
    int mDelayLengthL{89};   // Prime numbers for decorrelation
    int mDelayLengthR{97};

    /**
     * @brief Process allpass filter
     */
    float processAllpass(float input, std::vector<float>& buffer,
                        int& position, int delayLength);
};

/**
 * @brief Haas effect processor (precedence effect)
 *
 * Creates stereo width by delaying one channel by 5-35ms.
 * Based on psychoacoustic Haas effect (precedence effect):
 * - Delays < 5ms: Perceived as coloration
 * - Delays 5-35ms: Perceived as widening (optimal)
 * - Delays > 35ms: Perceived as echo
 *
 * Note: Can cause comb filtering when summed to mono!
 * Use decorrelator for mono-compatible widening.
 */
class HaasEffect {
public:
    /**
     * @brief Constructor
     * @param sampleRate Sample rate in Hz
     * @param delayMs Delay time in milliseconds (5-35ms recommended)
     */
    explicit HaasEffect(float sampleRate = 48000.0f, float delayMs = 15.0f);

    /**
     * @brief Set sample rate
     */
    void setSampleRate(float sampleRate);

    /**
     * @brief Set delay time
     * @param delayMs Delay in milliseconds (5-35ms)
     */
    void setDelayTime(float delayMs);

    /**
     * @brief Set which channel to delay
     * @param delayRight true = delay right, false = delay left
     */
    void setDelayRight(bool delayRight);

    /**
     * @brief Process stereo signal with Haas effect
     * @param left Left input
     * @param right Right input
     * @return Pair of (left, right) with Haas widening
     */
    std::pair<float, float> process(float left, float right);

    /**
     * @brief Reset delay buffer
     */
    void reset();

private:
    float mSampleRate{48000.0f};
    float mDelayMs{15.0f};
    bool mDelayRight{true};

    std::vector<float> mDelayBuffer;
    int mWritePos{0};
    int mDelaySamples{0};

    void updateDelay();
};

/**
 * @brief Simple stereo width control
 *
 * Adjusts stereo width without complex processing.
 * Mono-compatible when width < 1.0
 */
class StereoWidth {
public:
    /**
     * @brief Process stereo with width adjustment
     * @param left Left input
     * @param right Right input
     * @param width Width factor (0.0 = mono, 1.0 = unchanged, 2.0 = wide)
     * @return Pair of (left, right)
     *
     * Uses Mid/Side technique internally
     */
    static std::pair<float, float> process(float left, float right, float width);
};

#endif // STEREOTOOLS_H
