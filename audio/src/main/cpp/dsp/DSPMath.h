#ifndef DSPMATH_H
#define DSPMATH_H

#include <cmath>
#include <algorithm>

/**
 * @file DSPMath.h
 * @brief Utility functions for DSP calculations
 *
 * Provides common mathematical operations and conversions used in audio DSP:
 * - Decibel conversions
 * - Frequency mapping (linear/logarithmic)
 * - Interpolation
 * - Smoothing
 * - Clipping
 */

namespace DSPMath {

// ============================================================================
// CONSTANTS
// ============================================================================

constexpr float PI = 3.14159265358979323846f;
constexpr float TWO_PI = 2.0f * PI;
constexpr float HALF_PI = PI / 2.0f;

// Minimum value to prevent log(0) and divide by zero
constexpr float EPSILON = 1e-10f;

// ============================================================================
// DECIBEL CONVERSIONS
// ============================================================================

/**
 * @brief Convert linear gain to decibels
 * @param linear Linear gain value (0.0 to inf)
 * @return Gain in dB (-inf to +inf)
 */
inline float linearToDb(float linear) {
    if (linear <= EPSILON) {
        return -100.0f;  // Practical minimum instead of -inf
    }
    return 20.0f * std::log10(linear);
}

/**
 * @brief Convert decibels to linear gain
 * @param db Gain in dB
 * @return Linear gain value (0.0 to inf)
 */
inline float dbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

// ============================================================================
// FREQUENCY CONVERSIONS AND MAPPING
// ============================================================================

/**
 * @brief Convert frequency in Hz to radians per sample
 * @param frequencyHz Frequency in Hertz
 * @param sampleRate Sample rate in Hz
 * @return Angular frequency (radians per sample)
 */
inline float frequencyToRadians(float frequencyHz, float sampleRate) {
    return TWO_PI * frequencyHz / sampleRate;
}

/**
 * @brief Map normalized value (0-1) to frequency using logarithmic scale
 * @param normalized Value from 0.0 to 1.0
 * @param minFreq Minimum frequency in Hz
 * @param maxFreq Maximum frequency in Hz
 * @return Frequency in Hz
 *
 * Example: mapToFrequency(0.5, 20, 20000) ≈ 632 Hz (logarithmic midpoint)
 */
inline float mapToFrequency(float normalized, float minFreq, float maxFreq) {
    normalized = std::clamp(normalized, 0.0f, 1.0f);
    float logMin = std::log10(minFreq);
    float logMax = std::log10(maxFreq);
    float logValue = logMin + normalized * (logMax - logMin);
    return std::pow(10.0f, logValue);
}

/**
 * @brief Map frequency to normalized value (0-1) using logarithmic scale
 * @param frequency Frequency in Hz
 * @param minFreq Minimum frequency in Hz
 * @param maxFreq Maximum frequency in Hz
 * @return Normalized value from 0.0 to 1.0
 */
inline float frequencyToNormalized(float frequency, float minFreq, float maxFreq) {
    frequency = std::clamp(frequency, minFreq, maxFreq);
    float logMin = std::log10(minFreq);
    float logMax = std::log10(maxFreq);
    float logValue = std::log10(frequency);
    return (logValue - logMin) / (logMax - logMin);
}

// ============================================================================
// INTERPOLATION
// ============================================================================

/**
 * @brief Linear interpolation between two values
 * @param a First value
 * @param b Second value
 * @param t Interpolation factor (0.0 to 1.0)
 * @return Interpolated value
 *
 * When t=0, returns a. When t=1, returns b.
 */
inline float lerp(float a, float b, float t) {
    return a + t * (b - a);
}

/**
 * @brief Cubic interpolation (Hermite spline)
 * @param y0 Value at position -1
 * @param y1 Value at position 0
 * @param y2 Value at position 1
 * @param y3 Value at position 2
 * @param t Fractional position between y1 and y2 (0.0 to 1.0)
 * @return Interpolated value
 *
 * Higher quality than linear interpolation, used for smooth delay line reads.
 */
inline float cubicInterpolate(float y0, float y1, float y2, float y3, float t) {
    float a0 = y3 - y2 - y0 + y1;
    float a1 = y0 - y1 - a0;
    float a2 = y2 - y0;
    float a3 = y1;

    float t2 = t * t;
    return a0 * t * t2 + a1 * t2 + a2 * t + a3;
}

// ============================================================================
// SMOOTHING AND FILTERING
// ============================================================================

/**
 * @brief One-pole lowpass filter coefficient
 * @param timeConstantMs Time constant in milliseconds (time to reach ~63% of target)
 * @param sampleRate Sample rate in Hz
 * @return Filter coefficient (0.0 to 1.0)
 *
 * Usage:
 *   float smoothed = smoothed + coeff * (target - smoothed);
 */
inline float onePoleCoefficient(float timeConstantMs, float sampleRate) {
    if (timeConstantMs <= 0.0f) {
        return 1.0f;  // Instant change
    }
    float timeConstantSamples = timeConstantMs * sampleRate / 1000.0f;
    return 1.0f - std::exp(-1.0f / timeConstantSamples);
}

// ============================================================================
// CLIPPING AND SATURATION
// ============================================================================

/**
 * @brief Hard clip a value to range [-1.0, 1.0]
 * @param value Input value
 * @return Clipped value
 */
inline float hardClip(float value) {
    return std::clamp(value, -1.0f, 1.0f);
}

/**
 * @brief Soft clip using tanh saturation
 * @param value Input value
 * @return Saturated value (asymptotically approaches ±1.0)
 *
 * Smoother than hard clipping, introduces harmonic distortion.
 */
inline float softClip(float value) {
    return std::tanh(value);
}

/**
 * @brief Soft clip with adjustable drive
 * @param value Input value
 * @param drive Drive amount (1.0 = no saturation, higher = more saturation)
 * @return Saturated value
 */
inline float softClipWithDrive(float value, float drive) {
    return std::tanh(value * drive) / std::tanh(drive);
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

/**
 * @brief Check if a float is finite (not NaN or Infinity)
 * @param value Value to check
 * @return true if finite, false otherwise
 */
inline bool isFinite(float value) {
    return std::isfinite(value);
}

/**
 * @brief Normalize a value from one range to another
 * @param value Input value
 * @param inMin Input range minimum
 * @param inMax Input range maximum
 * @param outMin Output range minimum
 * @param outMax Output range maximum
 * @return Normalized value
 */
inline float normalize(float value, float inMin, float inMax, float outMin, float outMax) {
    value = std::clamp(value, inMin, inMax);
    float normalized = (value - inMin) / (inMax - inMin);
    return outMin + normalized * (outMax - outMin);
}

/**
 * @brief Wrap a value to range [0, max)
 * @param value Input value
 * @param max Maximum value (exclusive)
 * @return Wrapped value
 *
 * Used for circular buffer indexing with negative values.
 */
inline int wrapIndex(int value, int max) {
    if (value >= 0) {
        return value % max;
    } else {
        return (max + (value % max)) % max;
    }
}

/**
 * @brief Mix two values with crossfade
 * @param a First value
 * @param b Second value
 * @param mix Mix amount (0.0 = all a, 1.0 = all b)
 * @return Mixed value
 */
inline float crossfade(float a, float b, float mix) {
    mix = std::clamp(mix, 0.0f, 1.0f);
    return a * (1.0f - mix) + b * mix;
}

/**
 * @brief Equal-power crossfade (preserves perceived loudness)
 * @param a First value
 * @param b Second value
 * @param mix Mix amount (0.0 to 1.0)
 * @return Mixed value
 *
 * Uses sine/cosine curves to maintain constant power during crossfade.
 */
inline float equalPowerCrossfade(float a, float b, float mix) {
    mix = std::clamp(mix, 0.0f, 1.0f);
    float gainA = std::cos(mix * HALF_PI);
    float gainB = std::sin(mix * HALF_PI);
    return a * gainA + b * gainB;
}

/**
 * @brief Fast approximation of sin(x) using Taylor series
 * @param x Angle in radians
 * @return Approximate sine value
 *
 * Faster than std::sin but less accurate. Good for LFOs.
 * Error < 0.001 for x in [-PI, PI]
 */
inline float fastSin(float x) {
    // Wrap to [-PI, PI]
    while (x > PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;

    // Taylor series: sin(x) ≈ x - x³/6 + x⁵/120
    float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f));
}

/**
 * @brief Convert milliseconds to samples
 * @param timeMs Time in milliseconds
 * @param sampleRate Sample rate in Hz
 * @return Number of samples
 */
inline int msToSamples(float timeMs, float sampleRate) {
    return static_cast<int>(timeMs * sampleRate / 1000.0f);
}

/**
 * @brief Convert samples to milliseconds
 * @param samples Number of samples
 * @param sampleRate Sample rate in Hz
 * @return Time in milliseconds
 */
inline float samplesToMs(int samples, float sampleRate) {
    return (samples * 1000.0f) / sampleRate;
}

// ============================================================================
// PANNING
// ============================================================================

/**
 * @brief Equal-power panning gain for left channel
 * @param pan Pan position in range [-1, +1] (-1=full left, +1=full right)
 * @return Gain for left channel (0 to 1)
 */
inline float panGainL(float pan) {
    return std::cos((pan + 1.0f) * 0.25f * PI);
}

/**
 * @brief Equal-power panning gain for right channel
 * @param pan Pan position in range [-1, +1] (-1=full left, +1=full right)
 * @return Gain for right channel (0 to 1)
 */
inline float panGainR(float pan) {
    return std::sin((pan + 1.0f) * 0.25f * PI);
}

} // namespace DSPMath

#endif // DSPMATH_H
