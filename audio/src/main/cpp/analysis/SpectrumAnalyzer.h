#ifndef SPECTRUM_ANALYZER_H
#define SPECTRUM_ANALYZER_H

#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <atomic>

/**
 * @file SpectrumAnalyzer.h
 * @brief Real-time spectrum analyzer using FFT
 *
 * PHASE 5: Pro Features - Spectrum Analyzer
 *
 * Provides real-time frequency analysis of audio signals using
 * the Cooley-Tukey FFT algorithm. Optimized for ARM NEON when available.
 *
 * Features:
 * - Configurable FFT size (256, 512, 1024, 2048)
 * - Multiple window functions (Hann, Hamming, Blackman)
 * - Magnitude output in dB or linear scale
 * - Peak hold functionality
 * - Smoothed output for visualization
 */
class SpectrumAnalyzer {
public:
    /**
     * @brief Window function types
     */
    enum class WindowType {
        RECTANGULAR,    ///< No windowing (not recommended for music)
        HANN,           ///< Hann window (good all-purpose)
        HAMMING,        ///< Hamming window (lower sidelobes)
        BLACKMAN        ///< Blackman window (best sidelobe rejection)
    };

    /**
     * @brief Constructor
     * @param fftSize FFT size (power of 2, default 1024)
     */
    explicit SpectrumAnalyzer(int fftSize = 1024);

    /**
     * @brief Prepare the analyzer
     * @param sampleRate Sample rate in Hz
     * @param fftSize FFT size (256, 512, 1024, or 2048)
     */
    void prepare(float sampleRate, int fftSize = 1024);

    /**
     * @brief Set the window function type
     * @param type Window function to use
     */
    void setWindowType(WindowType type);

    /**
     * @brief Analyze a block of audio samples
     * @param input Input buffer (mono)
     * @param numSamples Number of samples to analyze
     *
     * The analyzer accumulates samples until it has enough for an FFT,
     * then processes them. Results are available via getMagnitudes().
     */
    void analyze(const float* input, int numSamples);

    /**
     * @brief Get the current magnitude spectrum
     * @return Reference to magnitude array (size = fftSize/2)
     *
     * Values are in dB (typically -100 to 0 dB range)
     */
    const std::vector<float>& getMagnitudes() const { return mMagnitudes; }

    /**
     * @brief Get the smoothed magnitude spectrum
     * @return Reference to smoothed magnitude array
     *
     * Smoothed values for better visualization (less jitter)
     */
    const std::vector<float>& getSmoothedMagnitudes() const { return mSmoothedMagnitudes; }

    /**
     * @brief Get peak magnitudes (with decay)
     * @return Reference to peak magnitude array
     */
    const std::vector<float>& getPeakMagnitudes() const { return mPeakMagnitudes; }

    /**
     * @brief Get frequency for a specific bin
     * @param binIndex FFT bin index (0 to fftSize/2-1)
     * @return Frequency in Hz for that bin
     */
    float getFrequencyForBin(int binIndex) const;

    /**
     * @brief Get bin index for a specific frequency
     * @param frequency Frequency in Hz
     * @return Nearest FFT bin index
     */
    int getBinForFrequency(float frequency) const;

    /**
     * @brief Get the number of frequency bins
     */
    int getNumBins() const { return mFFTSize / 2; }

    /**
     * @brief Get the FFT size
     */
    int getFFTSize() const { return mFFTSize; }

    /**
     * @brief Get the frequency resolution
     * @return Frequency resolution in Hz per bin
     */
    float getFrequencyResolution() const {
        return mSampleRate / static_cast<float>(mFFTSize);
    }

    /**
     * @brief Set smoothing factor for visualization
     * @param smoothing Smoothing factor (0.0 = no smoothing, 0.99 = very smooth)
     */
    void setSmoothing(float smoothing) {
        mSmoothing = std::clamp(smoothing, 0.0f, 0.99f);
    }

    /**
     * @brief Set peak decay rate
     * @param decayDb Decay rate in dB per second
     */
    void setPeakDecay(float decayDb) {
        mPeakDecayPerSample = decayDb / mSampleRate;
    }

    /**
     * @brief Check if new analysis data is available
     */
    bool isDataReady() const { return mDataReady.load(std::memory_order_relaxed); }

    /**
     * @brief Clear the ready flag (call after reading data)
     */
    void clearReady() { mDataReady.store(false, std::memory_order_relaxed); }

private:
    // FFT configuration
    int mFFTSize{1024};
    float mSampleRate{48000.0f};
    WindowType mWindowType{WindowType::HANN};

    // Buffers
    std::vector<float> mInputBuffer;        // Circular input buffer
    std::vector<float> mWindow;             // Window function
    std::vector<std::complex<float>> mFFTBuffer;  // FFT working buffer
    std::vector<std::complex<float>> mTwiddles;   // Precomputed twiddle factors

    // Output buffers
    std::vector<float> mMagnitudes;         // Raw magnitude (dB)
    std::vector<float> mSmoothedMagnitudes; // Smoothed for display
    std::vector<float> mPeakMagnitudes;     // Peak hold values

    // State
    int mInputWritePos{0};
    int mSamplesCollected{0};
    std::atomic<bool> mDataReady{false};

    // Visualization parameters
    float mSmoothing{0.7f};
    float mPeakDecayPerSample{0.001f};

    // Private methods

    /**
     * @brief Build the window function
     */
    void buildWindow();

    /**
     * @brief Precompute FFT twiddle factors
     */
    void precomputeTwiddles();

    /**
     * @brief Perform the FFT
     */
    void performFFT();

    /**
     * @brief Compute magnitudes from FFT output
     */
    void computeMagnitudes();

    /**
     * @brief In-place Cooley-Tukey FFT
     */
    void fft(std::complex<float>* data, int n);

    /**
     * @brief Bit-reverse an integer
     */
    static int bitReverse(int n, int bits);
};

#endif // SPECTRUM_ANALYZER_H
