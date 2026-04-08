#ifndef PARAMETER_SMOOTHER_H
#define PARAMETER_SMOOTHER_H

#include <cmath>
#include <atomic>

/**
 * @file ParameterSmoother.h
 * @brief One-pole smoothing filter for audio parameters
 *
 * Prevents audible clicks and artifacts when parameters change abruptly.
 * Useful for cutoff frequency, resonance, and other continuous parameters.
 *
 * Thread Safety:
 * - process() can be called from the audio thread (lock-free)
 * - setCoefficient(), setSmoothingTime(), reset() can be called from UI thread
 * - Uses atomic operations with relaxed memory ordering for performance
 */

/**
 * @class ParameterSmoother
 * @brief Single-pole smoothing filter for parameter interpolation
 *
 * Uses exponential smoothing to gradually transition between parameter values.
 * The coefficient determines the smoothing amount (closer to 1.0 = more smoothing).
 *
 * Thread-safe: Uses atomic<float> for cross-thread access.
 */
class ParameterSmoother {
public:
    /**
     * @brief Constructor with optional smoothing coefficient
     * @param coeff Smoothing coefficient [0.0, 1.0). Default: 0.99
     *              Higher values = smoother but slower response
     *              Typical values: 0.95-0.999
     */
    explicit ParameterSmoother(float coeff = 0.99f)
        : mCurrent(0.0f), mCoefficient(clampCoefficient(coeff)) {
    }

    /**
     * @brief Process a target value and return smoothed output
     * @param target Target value to reach
     * @return Smoothed value
     *
     * RT-SAFE: Uses relaxed memory ordering for minimal overhead.
     * Can be called from audio thread without blocking.
     */
    inline float process(float target) {
        float current = mCurrent.load(std::memory_order_relaxed);
        float coeff = mCoefficient.load(std::memory_order_relaxed);
        float newCurrent = coeff * current + (1.0f - coeff) * target;
        mCurrent.store(newCurrent, std::memory_order_relaxed);
        return newCurrent;
    }

    /**
     * @brief Reset the smoother to a specific value (no smoothing)
     * @param value Value to reset to
     *
     * Thread-safe: Can be called from any thread.
     */
    inline void reset(float value) {
        mCurrent.store(value, std::memory_order_relaxed);
    }

    /**
     * @brief Set the smoothing coefficient
     * @param coeff New coefficient [0.0, 1.0)
     *
     * Thread-safe: Can be called from UI thread.
     */
    inline void setCoefficient(float coeff) {
        mCoefficient.store(clampCoefficient(coeff), std::memory_order_relaxed);
    }

    /**
     * @brief Set the smoothing time in milliseconds
     * @param timeMs Desired smoothing time in milliseconds
     * @param sampleRate Sample rate in Hz
     *
     * Calculates the coefficient to achieve approximately the desired
     * smoothing time (time to reach ~63% of the target value).
     *
     * Thread-safe: Can be called from UI thread.
     */
    inline void setSmoothingTime(float timeMs, float sampleRate) {
        // Calculate coefficient for desired time constant
        // tau = -1 / (sampleRate * ln(coefficient))
        // coefficient = exp(-1 / (tau * sampleRate))
        float tau = timeMs / 1000.0f;  // Convert to seconds
        float coeff = expf(-1.0f / (tau * sampleRate));
        mCoefficient.store(clampCoefficient(coeff), std::memory_order_relaxed);
    }

    /**
     * @brief Get the current smoothed value
     * @return Current value
     *
     * Thread-safe: Can be called from any thread.
     */
    inline float getCurrent() const {
        return mCurrent.load(std::memory_order_relaxed);
    }

private:
    /**
     * @brief Clamp coefficient to valid range [0.0, 0.999]
     */
    static inline float clampCoefficient(float coeff) {
        if (coeff < 0.0f) return 0.0f;
        if (coeff >= 1.0f) return 0.999f;
        return coeff;
    }

    std::atomic<float> mCurrent;      ///< Current smoothed value (atomic for RT-safety)
    std::atomic<float> mCoefficient;  ///< Smoothing coefficient (atomic for RT-safety)
};

#endif // PARAMETER_SMOOTHER_H
