#ifndef VOCODEREFFECT_H
#define VOCODEREFFECT_H

#include "Effect.h"
#include "EffectTypes.h"
#include "../dsp/VocoderBank.h"
#include "../dsp/BiquadFilter.h"
#include "../dsp/DSPMath.h"
#include <atomic>
#include <array>
#include <vector>
#include <cmath>

/**
 * @file VocoderEffect.h
 * @brief Spectral vocoder effect with mic modulator and synth carrier
 *
 * A vocoder synthesizes the timbre of a carrier (synth oscillator)
 * controlled by the spectral envelope of a modulator (microphone/voice).
 *
 * Signal Flow:
 *   Modulator (MIC) → Analysis filterbank → Envelope followers
 *                                                    ↓
 *   Carrier (SYNTH) → Synthesis filterbank → Apply envelopes → Output
 *
 * This creates the classic "robot voice" or "talking synth" effect.
 *
 * Thread-safe: Parameters use atomic operations for lock-free updates.
 */
class VocoderEffect : public Effect {
public:
    /**
     * @brief Parameter IDs
     */
    enum Param {
        BAND_COUNT = 0,      ///< Number of frequency bands (4-32)
        FORMANT_SHIFT = 1,   ///< Formant shift in semitones (-24 to +24)
        ATTACK = 2,          ///< Envelope attack time (0.1-100 ms)
        RELEASE = 3,         ///< Envelope release time (1-500 ms)
        MIX = 4,             ///< Wet/dry mix (0.0-1.0)
        CARRIER_LEVEL = 5,   ///< Internal carrier level (0.0-1.0)
        MOD_SOURCE = 6,      ///< Modulator source (0=internal/self, 1=external mic)
        CARRIER_SOURCE = 7,  ///< Carrier source (0=input signal, 1=internal oscillator)
        CARRIER_FREQ = 8     ///< Internal carrier frequency in Hz (50-500)
    };

    /**
     * @brief Constructor
     */
    VocoderEffect();

    /**
     * @brief Destructor
     */
    ~VocoderEffect() override = default;

    /**
     * @brief Process audio through the vocoder
     * @param input Input buffer (stereo interleaved) - used as carrier or modulator
     * @param output Output buffer (stereo interleaved)
     * @param numFrames Number of frames to process
     *
     * The input is treated as the carrier signal (synth oscillator).
     * For external modulator (mic), use setModulatorBuffer() before processing.
     */
    void process(float* input, float* output, int numFrames) override;

    /**
     * @brief Set a parameter value
     * @param paramId Parameter ID from Param enum
     * @param value Parameter value
     */
    void setParam(int paramId, float value) override;

    /**
     * @brief Get a parameter value
     * @param paramId Parameter ID from Param enum
     * @return Current parameter value
     */
    float getParam(int paramId) override;

    /**
     * @brief Set sample rate
     * @param sampleRate Sample rate in Hz
     */
    void setSampleRate(int sampleRate) override;

    /**
     * @brief Set external modulator buffer (from microphone)
     * @param modulator Mono modulator signal
     * @param numSamples Number of samples
     *
     * Call this before process() when using external modulator (MOD_SOURCE=1).
     * The buffer is copied internally for thread safety.
     */
    void setModulatorBuffer(const float* modulator, int numSamples);

    /**
     * @brief Check if external modulator is available
     * @return true if modulator buffer has valid data
     */
    bool hasExternalModulator() const {
        return mHasExternalMod.load(std::memory_order_relaxed);
    }

private:
    int mSampleRate{48000};

    // Parameters (atomic for thread-safety)
    std::atomic<int> mBandCount{16};
    std::atomic<float> mFormantShift{0.0f};
    std::atomic<float> mAttackMs{5.0f};
    std::atomic<float> mReleaseMs{50.0f};
    std::atomic<float> mMix{0.5f};
    std::atomic<float> mCarrierLevel{1.0f};
    std::atomic<int> mModSource{1};        // Default to mic (1)
    std::atomic<int> mCarrierSource{0};    // Default to input (0), 1=internal oscillator
    std::atomic<float> mCarrierFreqParam{110.0f};  // Internal oscillator frequency

    // Core vocoder processing
    VocoderBank mVocoderBank;
    std::array<float, VocoderBank::MAX_BANDS> mBandEnvelopes{};

    // Internal carrier oscillator (sawtooth for rich harmonics)
    float mCarrierPhase{0.0f};
    std::atomic<float> mCarrierFrequency{110.0f};

    // External modulator buffer (from mic)
    std::vector<float> mModulatorBuffer;
    std::atomic<bool> mHasExternalMod{false};
    std::atomic<int> mModulatorSamples{0};

    // Working buffers (pre-allocated)
    std::vector<float> mCarrierMono;
    std::vector<float> mModulatorMono;
    std::vector<float> mOutputMono;
    std::vector<float> mInternalCarrier;  // For internal oscillator carrier

    // Pre/post filtering
    BiquadFilter mInputHPF_L;   ///< Remove DC from left input
    BiquadFilter mInputHPF_R;   ///< Remove DC from right input
    BiquadFilter mOutputLPF_L;  ///< Smooth output left
    BiquadFilter mOutputLPF_R;  ///< Smooth output right

    /**
     * @brief Generate internal carrier signal (sawtooth)
     * @param buffer Output buffer
     * @param numSamples Number of samples
     */
    void generateCarrier(float* buffer, int numSamples);

    /**
     * @brief Convert stereo to mono (average L+R)
     * @param stereo Input stereo buffer
     * @param mono Output mono buffer
     * @param numFrames Number of frames
     */
    void stereoToMono(const float* stereo, float* mono, int numFrames);

    /**
     * @brief Convert mono to stereo (duplicate)
     * @param mono Input mono buffer
     * @param stereo Output stereo buffer
     * @param numFrames Number of frames
     */
    void monoToStereo(const float* mono, float* stereo, int numFrames);
};

#endif // VOCODEREFFECT_H
