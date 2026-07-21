#pragma once

#include "../core/graph/AudioNode.h"
#include "../dsp/LockFreeRingBuffer.h"
#include "../dsp/NoiseGate.h"
#include "../dsp/LevelMeter.h"
#include "../dsp/DCBlocker.h"
#include <oboe/Oboe.h>
#include <memory>
#include <vector>

/**
 * @enum InputSource
 * @brief Audio input source types
 */
enum class InputSource {
    MIC = 0,        ///< Built-in microphone
    LINE_IN = 1,    ///< Line input (3.5mm jack)
    USB_DAC = 2,    ///< USB audio device
    BLUETOOTH = 3   ///< Bluetooth audio input
};

/**
 * @class InputNode
 * @brief Audio input node for capturing audio from microphone, line-in, or USB
 *
 * This node captures audio from the device's input and makes it available
 * in the audio graph. It includes:
 * - Input gain control
 * - DC blocking
 * - Noise gate (optional)
 * - Level metering
 *
 * The input stream runs on a separate thread and communicates with the
 * output processing thread via a lock-free ring buffer.
 */
class InputNode : public AudioNode, public oboe::AudioStreamDataCallback {
public:
    InputNode();
    ~InputNode() override;

    // AudioNode interface
    NodeType getType() const override { return NodeType::INPUT; }
    const char* getName() const override { return "Input"; }

    void prepare(int sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(AudioBuffer& inputBuffer, int numFrames) override;

    // Oboe callback (input stream)
    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream* stream,
        void* audioData,
        int32_t numFrames) override;

    // Stream management
    bool startInputStream();
    void stopInputStream();
    bool isInputStreamRunning() const;

    // Configuration
    void setInputSource(InputSource source);
    InputSource getInputSource() const;

    void setInputGain(float gainDb);
    float getInputGain() const;

    void setNoiseGateEnabled(bool enabled);
    bool isNoiseGateEnabled() const;
    void setNoiseGateThreshold(float thresholdDb);

    // Monitoring
    float getInputLevel(int channel) const;       // -120 to 0 dB
    float getInputLevelLinear(int channel) const; // 0 to 1
    bool isClipping() const;
    bool isNoiseGateOpen() const;

    // Latency info
    int64_t getInputLatencyFrames() const;
    float getInputLatencyMs() const;
    void updateLatency();  // Call periodically to refresh latency

    // Sample rate (returns the actual sample rate of the input stream)
    int getStreamSampleRate() const { return mSampleRate; }

    // Monitoring (pass-through to output)
    void setMonitoringEnabled(bool enabled);
    bool isMonitoringEnabled() const;
    void setMonitoringVolume(float volume);  // 0.0 to 1.0
    float getMonitoringVolume() const;

    // Get monitoring samples for output mixing (thread-safe)
    // Returns number of frames actually read
    int getMonitoringSamples(float* outputBuffer, int numFrames);

    // FIX PHASE 7.2: Feed external audio input (from USB backend)
    // This allows USB audio input to be processed through the same pipeline
    // as native microphone input (INPUT_FX mode, vocoder, etc.)
    void feedExternalInput(const float* inputData, int numFrames);

private:
    bool createInputStream();
    void closeInputStream();
    oboe::InputPreset getInputPresetForSource(InputSource source) const;

private:
    // Input stream
    std::shared_ptr<oboe::AudioStream> mInputStream;
    std::atomic<bool> mInputStreamRunning{false};

    // Ring buffer for input data (1 second capacity)
    // Note: Buffer size affects memory, not latency. Latency is determined by
    // ISO transfer timing and DSP processing rate.
    static constexpr size_t RING_BUFFER_SECONDS = 1;
    LockFreeRingBuffer mRingBuffer;

    // Source configuration
    std::atomic<InputSource> mInputSource{InputSource::MIC};
    // BUGFIX: Default +12dB gain to compensate for low sensitivity of mobile microphones
    std::atomic<float> mInputGainDb{12.0f};
    std::atomic<float> mInputGainLinear{3.981f};  // 10^(12/20) ≈ 3.981

    // DSP processing
    StereoDCBlocker mDCBlocker;
    NoiseGate mNoiseGate;
    // Off by default: a -60 dB gate after +12 dB input gain cuts audible
    // guitar decay tails. Opt-in from the UI (InputTestScreen / Gain Staging).
    std::atomic<bool> mNoiseGateEnabled{false};
    LevelMeter mLevelMeter;

    // Temporary buffer for processing (pre-allocated in prepare())
    std::vector<float> mTempBuffer;

    // Temporary buffer for monitoring volume scaling (pre-allocated in prepare())
    std::vector<float> mMonitorTempBuffer;

    // Latency tracking
    std::atomic<int64_t> mInputLatencyFrames{0};

    // Monitoring
    std::atomic<bool> mMonitoringEnabled{false};
    std::atomic<float> mMonitoringVolume{0.7f};  // Default 70% to avoid feedback

    // Separate ring buffer for monitoring output (so input processing doesn't affect monitoring)
    LockFreeRingBuffer mMonitoringBuffer;
};
