#ifndef BIQUADFILTER_H
#define BIQUADFILTER_H

#include <atomic>
#include <cmath>
#include <algorithm>
#include "DSPMath.h"

/**
 * @file BiquadFilter.h
 * @brief Second-order IIR filter (biquad)
 *
 * Implements various filter types using the standard biquad transfer function:
 *        b0 + b1*z^-1 + b2*z^-2
 * H(z) = ----------------------
 *        a0 + a1*z^-1 + a2*z^-2
 *
 * Normalized form (a0 = 1):
 * y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
 *
 * Thread-safe: Coefficients are atomic and can be updated from different thread.
 */
class BiquadFilter {
public:
    /**
     * @brief Filter type enumeration
     */
    enum class Type {
        LPF,        ///< Lowpass filter
        HPF,        ///< Highpass filter
        BPF,        ///< Bandpass filter (constant skirt gain)
        NOTCH,      ///< Notch filter (band-reject)
        PEAK,       ///< Peaking EQ
        LOW_SHELF,  ///< Low shelf
        HIGH_SHELF  ///< High shelf
    };

    /**
     * @brief Constructor
     * @param sampleRate Initial sample rate in Hz
     */
    explicit BiquadFilter(float sampleRate = 48000.0f);

    /**
     * @brief Copy constructor
     * Atomics are not copyable, so we load their values manually
     */
    BiquadFilter(const BiquadFilter& other)
        : b0(other.b0.load(std::memory_order_relaxed))
        , b1(other.b1.load(std::memory_order_relaxed))
        , b2(other.b2.load(std::memory_order_relaxed))
        , a1(other.a1.load(std::memory_order_relaxed))
        , a2(other.a2.load(std::memory_order_relaxed))
        , z1(other.z1)
        , z2(other.z2)
        , mSampleRate(other.mSampleRate)
        , mType(other.mType)
        , mFrequency(other.mFrequency)
        , mQ(other.mQ)
        , mGainDb(other.mGainDb) {}

    /**
     * @brief Move constructor
     */
    BiquadFilter(BiquadFilter&& other) noexcept
        : b0(other.b0.load(std::memory_order_relaxed))
        , b1(other.b1.load(std::memory_order_relaxed))
        , b2(other.b2.load(std::memory_order_relaxed))
        , a1(other.a1.load(std::memory_order_relaxed))
        , a2(other.a2.load(std::memory_order_relaxed))
        , z1(other.z1)
        , z2(other.z2)
        , mSampleRate(other.mSampleRate)
        , mType(other.mType)
        , mFrequency(other.mFrequency)
        , mQ(other.mQ)
        , mGainDb(other.mGainDb) {
        other.z1 = 0.0f;
        other.z2 = 0.0f;
    }

    /**
     * @brief Copy assignment operator
     */
    BiquadFilter& operator=(const BiquadFilter& other) {
        if (this != &other) {
            b0.store(other.b0.load(std::memory_order_relaxed), std::memory_order_relaxed);
            b1.store(other.b1.load(std::memory_order_relaxed), std::memory_order_relaxed);
            b2.store(other.b2.load(std::memory_order_relaxed), std::memory_order_relaxed);
            a1.store(other.a1.load(std::memory_order_relaxed), std::memory_order_relaxed);
            a2.store(other.a2.load(std::memory_order_relaxed), std::memory_order_relaxed);
            z1 = other.z1;
            z2 = other.z2;
            mSampleRate = other.mSampleRate;
            mType = other.mType;
            mFrequency = other.mFrequency;
            mQ = other.mQ;
            mGainDb = other.mGainDb;
        }
        return *this;
    }

    /**
     * @brief Move assignment operator
     */
    BiquadFilter& operator=(BiquadFilter&& other) noexcept {
        if (this != &other) {
            b0.store(other.b0.load(std::memory_order_relaxed), std::memory_order_relaxed);
            b1.store(other.b1.load(std::memory_order_relaxed), std::memory_order_relaxed);
            b2.store(other.b2.load(std::memory_order_relaxed), std::memory_order_relaxed);
            a1.store(other.a1.load(std::memory_order_relaxed), std::memory_order_relaxed);
            a2.store(other.a2.load(std::memory_order_relaxed), std::memory_order_relaxed);
            z1 = other.z1;
            z2 = other.z2;
            mSampleRate = other.mSampleRate;
            mType = other.mType;
            mFrequency = other.mFrequency;
            mQ = other.mQ;
            mGainDb = other.mGainDb;
            other.z1 = 0.0f;
            other.z2 = 0.0f;
        }
        return *this;
    }

    /**
     * @brief Set sample rate and recalculate coefficients
     * @param sampleRate Sample rate in Hz
     */
    void setSampleRate(float sampleRate);

    /**
     * @brief Configure as lowpass filter
     * @param frequency Cutoff frequency in Hz
     * @param Q Quality factor (resonance, typically 0.5 to 10)
     *
     * Q = 0.707 (1/√2) gives Butterworth response (maximally flat)
     * Higher Q = more resonance at cutoff
     */
    void setLowpass(float frequency, float Q = 0.707f);

    /**
     * @brief Configure as highpass filter
     * @param frequency Cutoff frequency in Hz
     * @param Q Quality factor (resonance, typically 0.5 to 10)
     */
    void setHighpass(float frequency, float Q = 0.707f);

    /**
     * @brief Configure as bandpass filter
     * @param frequency Center frequency in Hz
     * @param Q Quality factor (bandwidth control)
     *
     * Higher Q = narrower bandwidth
     * Bandwidth (Hz) ≈ frequency / Q
     */
    void setBandpass(float frequency, float Q = 1.0f);

    /**
     * @brief Configure as notch filter (band-reject)
     * @param frequency Center frequency in Hz (frequency to reject)
     * @param Q Quality factor (notch width)
     */
    void setNotch(float frequency, float Q = 1.0f);

    /**
     * @brief Configure as peaking EQ filter
     * @param frequency Center frequency in Hz
     * @param Q Quality factor (bandwidth)
     * @param gainDb Gain in decibels (positive = boost, negative = cut)
     */
    void setPeaking(float frequency, float Q, float gainDb);

    /**
     * @brief Configure as low shelf filter
     * @param frequency Transition frequency in Hz
     * @param Q Shelf slope (typically 0.5 to 1.0)
     * @param gainDb Gain in decibels
     */
    void setLowShelf(float frequency, float Q, float gainDb);

    /**
     * @brief Configure as high shelf filter
     * @param frequency Transition frequency in Hz
     * @param Q Shelf slope (typically 0.5 to 1.0)
     * @param gainDb Gain in decibels
     */
    void setHighShelf(float frequency, float Q, float gainDb);

    /**
     * @brief Process a single sample
     * @param input Input sample
     * @return Filtered output sample
     *
     * RT-safe: Uses atomic loads for coefficients
     */
    float process(float input);

    /**
     * @brief Process a block of samples
     * @param input Input buffer
     * @param output Output buffer
     * @param numSamples Number of samples to process
     *
     * RT-safe: Coefficients loaded once at start of block
     */
    void processBlock(const float* input, float* output, int numSamples);

    /**
     * @brief Reset filter state (clear delay lines)
     *
     * Call this when starting/stopping audio to avoid clicks
     */
    void reset();

    /**
     * @brief Get current filter type
     */
    Type getType() const { return mType; }

    /**
     * @brief Get frequency response at given frequency
     * @param frequency Frequency in Hz
     * @return Magnitude response (linear, not dB)
     *
     * Useful for visualization and debugging
     */
    float getFrequencyResponse(float frequency) const;

private:
    // Coefficients (atomic for thread-safety)
    std::atomic<float> b0{1.0f};
    std::atomic<float> b1{0.0f};
    std::atomic<float> b2{0.0f};
    std::atomic<float> a1{0.0f};
    std::atomic<float> a2{0.0f};

    // State variables (delay lines)
    float z1{0.0f};  ///< x[n-1] and y[n-1] combined state
    float z2{0.0f};  ///< x[n-2] and y[n-2] combined state

    // Configuration
    float mSampleRate{48000.0f};
    Type mType{Type::LPF};
    float mFrequency{1000.0f};
    float mQ{0.707f};
    float mGainDb{0.0f};

    /**
     * @brief Update coefficients based on current parameters
     *
     * Called internally whenever parameters change.
     * Implements formulas from "Audio EQ Cookbook" by Robert Bristow-Johnson
     */
    void updateCoefficients();

    /**
     * @brief Clamp frequency to valid range [10Hz, sampleRate/2]
     */
    float clampFrequency(float freq) const;
};

#endif // BIQUADFILTER_H
