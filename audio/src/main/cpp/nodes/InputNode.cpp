#include "InputNode.h"
#include "../platform/Logger.h"

// Oboe is Android-only. The input *capture* path (opening a mic/line stream)
// lives entirely behind this guard; the rest of InputNode — DSP, gain, noise
// gate, level metering, the ring buffers — is portable and compiles everywhere.
// On iOS the stream methods are inert (no capture yet); a CoreAudio input
// adapter would slot in at this same seam. See AudioEngine.cpp for the same
// WMA_HAS_OBOE pattern.
#if defined(__ANDROID__)
#define WMA_HAS_OBOE 1
#include <oboe/Oboe.h>
#else
#define WMA_HAS_OBOE 0
#endif

#include <cmath>
#include <algorithm>
#include <cassert>
#include <cstring>

#define LOG_TAG "InputNode"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

#if WMA_HAS_OBOE
// ========== OBOE ADAPTER (WA-2.0) ==========
// Concentrates every Oboe dependency of InputNode: the callback inheritance and
// the stream handle. InputNode only sees it through an opaque pointer, which is
// what keeps <oboe/Oboe.h> out of InputNode.h and lets the core be built for
// platforms that have no Oboe. A CoreAudio adapter would sit at this same seam.
class InputOboeAdapter : public oboe::AudioStreamDataCallback {
public:
    explicit InputOboeAdapter(InputNode* node) : mNode(node) {}

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream,
                                          void* audioData,
                                          int32_t numFrames) override {
        // RT path: one pointer hop plus getChannelCount(), which in Oboe is a
        // plain member read. No allocation, no locking, no indirect call setup.
        return mNode->processInputBlock(static_cast<float*>(audioData),
                                        numFrames,
                                        stream->getChannelCount())
                   ? oboe::DataCallbackResult::Continue
                   : oboe::DataCallbackResult::Stop;
    }

    // Owned here rather than in InputNode so the header stays Oboe-free.
    std::shared_ptr<oboe::AudioStream> stream;

private:
    InputNode* mNode;
};

void InputNode::BackendAdapterDeleter::operator()(void* p) const {
    delete static_cast<InputOboeAdapter*>(p);
}

namespace {

// The nested deleter type is private, so the adapter is reached by casting the
// raw void* rather than by a helper that would have to name that type.
InputOboeAdapter* asAdapter(void* p) {
    return static_cast<InputOboeAdapter*>(p);
}

oboe::InputPreset inputPresetForSource(InputSource source) {
    switch (source) {
        case InputSource::MIC:
            // VoicePerformance is optimized for low-latency music/voice
            return oboe::InputPreset::VoicePerformance;
        case InputSource::LINE_IN:
            // Unprocessed gives raw audio without AGC/noise suppression
            return oboe::InputPreset::Unprocessed;
        case InputSource::USB_DAC:
            return oboe::InputPreset::Unprocessed;
        case InputSource::BLUETOOTH:
            return oboe::InputPreset::Generic;
        default:
            return oboe::InputPreset::Generic;
    }
}

}  // namespace

#else  // !WMA_HAS_OBOE

// Without Oboe nothing populates mBackendAdapter, but the deleter is declared in
// the header and must still link.
void InputNode::BackendAdapterDeleter::operator()(void*) const {}

#endif  // WMA_HAS_OBOE

InputNode::InputNode()
    : mRingBuffer(48000 * 2 * RING_BUFFER_SECONDS)  // Initial size: 1 second stereo at 48kHz
    , mMonitoringBuffer(48000 * 2 * RING_BUFFER_SECONDS)  // Same size for monitoring
{
    mNumInputChannels = 0;  // No inputs from other nodes (this is a source)
    mNumOutputChannels = 2;
    mTempBuffer.resize(8192);
}

InputNode::~InputNode() {
    stopInputStream();
}

void InputNode::prepare(int sampleRate, int maxBlockSize) {
    AudioNode::prepare(sampleRate, maxBlockSize);

    // Prepare DSP components
    mDCBlocker.prepare(sampleRate);
    mNoiseGate.prepare(sampleRate);
    mLevelMeter.prepare(sampleRate);

    // Resize ring buffers for 1 second of stereo audio
    mRingBuffer.resize(sampleRate * 2 * RING_BUFFER_SECONDS);
    mMonitoringBuffer.resize(sampleRate * 2 * RING_BUFFER_SECONDS);

    // Resize temp buffers
    mTempBuffer.resize(maxBlockSize * 2);
    mMonitorTempBuffer.resize(maxBlockSize * 2);

    LOGI("InputNode prepared: sampleRate=%d, maxBlockSize=%d", sampleRate, maxBlockSize);
}

void InputNode::reset() {
    AudioNode::reset();
    mRingBuffer.clear();
    mMonitoringBuffer.clear();
    mLevelMeter.reset();
    mNoiseGate.reset();
    mDCBlocker.reset();
}

bool InputNode::createInputStream() {
#if !WMA_HAS_OBOE
    // No capture backend on this platform yet (iOS input is future work). The
    // node still exists and processes; it just never has a live input stream.
    return false;
#else
    // The adapter is created on first use and then kept for the lifetime of the
    // node: Oboe holds a raw pointer to it while a stream is open, so it must
    // outlive every stream it is registered with.
    if (!mBackendAdapter) {
        mBackendAdapter.reset(new InputOboeAdapter(this));
    }
    auto* adapter = asAdapter(mBackendAdapter.get());

    oboe::AudioStreamBuilder builder;

    // Use Shared mode for better device routing compatibility
    // This allows proper switching when USB DAC or other devices are connected
    builder.setDirection(oboe::Direction::Input)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Shared)  // Changed from Exclusive for device routing
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(oboe::ChannelCount::Stereo)
            // Don't force sample rate - let system choose best rate for the device
            ->setDataCallback(adapter)
            ->setInputPreset(inputPresetForSource(mInputSource.load()));

    oboe::Result result = builder.openStream(adapter->stream);

    if (result != oboe::Result::OK) {
        LOGE("Failed to open input stream: %s", oboe::convertToText(result));
        return false;
    }

    // Update latency with actual measurement
    updateLatency();

    auto& stream = adapter->stream;
    auto framesPerBurst = stream->getFramesPerBurst();
    auto actualSampleRate = stream->getSampleRate();
    auto deviceId = stream->getDeviceId();
    auto sharingMode = stream->getSharingMode();

    // DEBUG: Detailed stream info for routing diagnostics
    LOGI("=== INPUT STREAM OPENED ===");
    LOGI("  Sample rate: %d Hz", actualSampleRate);
    LOGI("  Channel count: %d", stream->getChannelCount());
    LOGI("  Sharing mode: %s", sharingMode == oboe::SharingMode::Shared ? "Shared" : "Exclusive");
    LOGI("  Performance mode: %s",
         stream->getPerformanceMode() == oboe::PerformanceMode::LowLatency ? "LowLatency" :
         stream->getPerformanceMode() == oboe::PerformanceMode::PowerSaving ? "PowerSaving" : "None");
    LOGI("  Device ID: %d", deviceId);
    LOGI("  Frames per burst: %d", framesPerBurst);
    LOGI("  Buffer capacity: %d frames", stream->getBufferCapacityInFrames());
    LOGI("  Latency: %.1f ms", getInputLatencyMs());
    LOGI("===========================");

    // Store actual sample rate for reference
    mSampleRate = actualSampleRate;

    return true;
#endif  // WMA_HAS_OBOE
}

void InputNode::closeInputStream() {
#if WMA_HAS_OBOE
    auto* adapter = asAdapter(mBackendAdapter.get());
    if (adapter && adapter->stream) {
        adapter->stream->stop();
        adapter->stream->close();
        adapter->stream.reset();
        LOGI("Input stream closed");
    }
#endif
}

bool InputNode::startInputStream() {
#if !WMA_HAS_OBOE
    return false;  // No capture backend on this platform (see createInputStream).
#else
    if (mInputStreamRunning.load()) {
        LOGI("Input stream already running");
        return true;
    }

    auto* adapter = asAdapter(mBackendAdapter.get());
    if (!adapter || !adapter->stream) {
        if (!createInputStream()) {
            return false;
        }
        adapter = asAdapter(mBackendAdapter.get());
    }

    oboe::Result result = adapter->stream->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start input stream: %s", oboe::convertToText(result));
        return false;
    }

    mInputStreamRunning.store(true);
    LOGI("Input stream started");
    return true;
#endif  // WMA_HAS_OBOE
}

void InputNode::stopInputStream() {
    mInputStreamRunning.store(false);
    closeInputStream();
    mRingBuffer.clear();
    LOGI("Input stream stopped");
}

bool InputNode::isInputStreamRunning() const {
    return mInputStreamRunning.load();
}

bool InputNode::processInputBlock(float* audioData, int numFrames, int channelCount) {
    if (!mInputStreamRunning.load()) {
        return false;
    }

    float* processBuffer = audioData;

    // Handle mono input by duplicating to stereo
    // mTempBuffer is pre-allocated in prepare() to maxBlockSize * 2.
    // [[maybe_unused]]: sólo la lee el assert de abajo, que desaparece con NDEBUG.
    [[maybe_unused]] const size_t requiredSize = static_cast<size_t>(numFrames * 2);
    assert(mTempBuffer.size() >= requiredSize &&
           "Temp buffer too small — prepare() should allocate maxBlockSize * 2");

    if (channelCount == 1) {
        // Convert mono to stereo in temp buffer
        for (int i = numFrames - 1; i >= 0; --i) {
            mTempBuffer[i * 2] = audioData[i];
            mTempBuffer[i * 2 + 1] = audioData[i];
        }
        processBuffer = mTempBuffer.data();
    } else if (channelCount == 2) {
        // For stereo, copy to temp buffer so we can process without modifying original
        std::copy(audioData, audioData + numFrames * 2, mTempBuffer.begin());
        processBuffer = mTempBuffer.data();
    }

    // DEBUG: Check raw input level BEFORE any processing
    static int rawInputLogCount = 0;
    if (++rawInputLogCount >= 100) {
        float maxRawSample = 0.0f;
        for (int i = 0; i < numFrames * 2 && i < 100; ++i) {
            if (std::abs(processBuffer[i]) > maxRawSample) {
                maxRawSample = std::abs(processBuffer[i]);
            }
        }
        LOGI("RAW INPUT: maxSample=%.6f, numFrames=%d, channels=%d",
             maxRawSample, numFrames, channelCount);
        rawInputLogCount = 0;
    }

    // Apply input gain
    float gain = mInputGainLinear.load(std::memory_order_relaxed);
    if (std::abs(gain - 1.0f) > 0.001f) {
        for (int i = 0; i < numFrames * 2; ++i) {
            processBuffer[i] *= gain;
        }
    }

    // DC Blocking - process directly in callback for immediate metering
    mDCBlocker.process(processBuffer, numFrames);

    // Noise Gate (if enabled)
    if (mNoiseGateEnabled.load(std::memory_order_relaxed)) {
        mNoiseGate.process(processBuffer, numFrames);
    }

    // Level metering - updates atomic values that can be read by UI thread
    mLevelMeter.process(processBuffer, numFrames);

    // DEBUG: Check processed level AFTER DSP
    static int processedLogCount = 0;
    if (++processedLogCount >= 100) {
        float maxProcessedSample = 0.0f;
        for (int i = 0; i < numFrames * 2 && i < 100; ++i) {
            if (std::abs(processBuffer[i]) > maxProcessedSample) {
                maxProcessedSample = std::abs(processBuffer[i]);
            }
        }
        LOGI("PROCESSED: maxSample=%.6f, noiseGate=%d, gateOpen=%d",
             maxProcessedSample,
             mNoiseGateEnabled.load(),
             mNoiseGate.isOpen());
        processedLogCount = 0;
    }

    // Write to ring buffer for potential future audio graph integration
    // Only write if there's space to avoid overflow spam
    size_t available = mRingBuffer.availableToWrite();
    size_t needed = static_cast<size_t>(numFrames * 2);
    if (available >= needed) {
        mRingBuffer.write(processBuffer, numFrames * 2);
    }

    // Write to monitoring buffer if monitoring is enabled
    if (mMonitoringEnabled.load(std::memory_order_relaxed)) {
        float monitorVolume = mMonitoringVolume.load(std::memory_order_relaxed);

        // DEBUG: Log monitoring write periodically (every ~1 second)
        static int monitorWriteCount = 0;
        if (++monitorWriteCount >= 100) {  // ~100 callbacks = ~1 second
            LOGI("MONITOR WRITE: volume=%.2f, frames=%d, bufferAvailable=%zu",
                 monitorVolume, numFrames, mMonitoringBuffer.availableToWrite());
            monitorWriteCount = 0;
        }

        // Apply monitoring volume before writing
        if (std::abs(monitorVolume - 1.0f) > 0.001f) {
            // Use pre-allocated monitoring buffer (avoids RT allocations)
            assert(mMonitorTempBuffer.size() >= needed &&
                   "Monitor temp buffer too small — prepare() should allocate maxBlockSize * 2");

            for (size_t i = 0; i < needed; ++i) {
                mMonitorTempBuffer[i] = processBuffer[i] * monitorVolume;
            }

            size_t monitorAvailable = mMonitoringBuffer.availableToWrite();
            if (monitorAvailable >= needed) {
                mMonitoringBuffer.write(mMonitorTempBuffer.data(), needed);
            } else {
                // DEBUG: Buffer full
                static int overflowCount = 0;
                if (++overflowCount >= 100) {
                    LOGW("MONITOR OVERFLOW: available=%zu, needed=%zu", monitorAvailable, needed);
                    overflowCount = 0;
                }
            }
        } else {
            // Volume is 1.0, write directly
            size_t monitorAvailable = mMonitoringBuffer.availableToWrite();
            if (monitorAvailable >= needed) {
                mMonitoringBuffer.write(processBuffer, needed);
            } else {
                // DEBUG: Buffer full
                static int overflowCount = 0;
                if (++overflowCount >= 100) {
                    LOGW("MONITOR OVERFLOW: available=%zu, needed=%zu", monitorAvailable, needed);
                    overflowCount = 0;
                }
            }
        }
    }

    return true;
}

void InputNode::process(AudioBuffer& inputBuffer, int numFrames) {
    if (!isActive()) {
        mBuffer.clear();
        return;
    }

    // Read from ring buffer to our internal interleaved temp buffer
    bool success = mRingBuffer.read(mTempBuffer.data(), numFrames * 2);

    if (!success) {
        // Underrun: ring buffer already filled with silence
        mBuffer.clear();
        return;
    }

    float* data = mTempBuffer.data();

    // DC Blocking
    mDCBlocker.process(data, numFrames);

    // Noise Gate (if enabled)
    if (mNoiseGateEnabled.load(std::memory_order_relaxed)) {
        mNoiseGate.process(data, numFrames);
    }

    // Level metering
    mLevelMeter.process(data, numFrames);

    // Copy to output buffer (de-interleave)
    mBuffer.copyFromInterleaved(data, numFrames);
}

void InputNode::setInputSource(InputSource source) {
    if (mInputSource.load() == source) return;

    bool wasRunning = mInputStreamRunning.load();

    if (wasRunning) {
        stopInputStream();
    }

    mInputSource.store(source);

    if (wasRunning) {
        startInputStream();
    }

    LOGI("Input source changed to: %d", static_cast<int>(source));
}

InputSource InputNode::getInputSource() const {
    return mInputSource.load();
}

void InputNode::setInputGain(float gainDb) {
    mInputGainDb.store(gainDb, std::memory_order_relaxed);
    mInputGainLinear.store(std::pow(10.0f, gainDb / 20.0f), std::memory_order_relaxed);
}

float InputNode::getInputGain() const {
    return mInputGainDb.load(std::memory_order_relaxed);
}

void InputNode::setNoiseGateEnabled(bool enabled) {
    mNoiseGateEnabled.store(enabled, std::memory_order_relaxed);
}

bool InputNode::isNoiseGateEnabled() const {
    return mNoiseGateEnabled.load(std::memory_order_relaxed);
}

void InputNode::setNoiseGateThreshold(float thresholdDb) {
    mNoiseGate.setThreshold(thresholdDb);
}

float InputNode::getInputLevel(int channel) const {
    return mLevelMeter.getPeakDb(channel);
}

float InputNode::getInputLevelLinear(int channel) const {
    return (channel == 0) ? mLevelMeter.getPeakL() : mLevelMeter.getPeakR();
}

bool InputNode::isClipping() const {
    return mLevelMeter.isClipping();
}

bool InputNode::isNoiseGateOpen() const {
    return mNoiseGate.isOpen();
}

int64_t InputNode::getInputLatencyFrames() const {
    return mInputLatencyFrames.load();
}

float InputNode::getInputLatencyMs() const {
    if (mSampleRate <= 0) return 0.0f;
    return static_cast<float>(mInputLatencyFrames.load()) / mSampleRate * 1000.0f;
}

void InputNode::updateLatency() {
#if !WMA_HAS_OBOE
    mInputLatencyFrames.store(0);
    return;
#else
    auto* adapter = asAdapter(mBackendAdapter.get());
    if (!adapter || !adapter->stream) {
        mInputLatencyFrames.store(0);
        return;
    }
    auto& stream = adapter->stream;

    // Try to get actual latency from Oboe
    // calculateLatencyMillis() returns the estimated latency in milliseconds
    oboe::ResultWithValue<double> latencyResult = stream->calculateLatencyMillis();

    if (latencyResult.error() == oboe::Result::OK) {
        // Convert ms to frames
        double latencyMs = latencyResult.value();
        int64_t latencyFrames = static_cast<int64_t>(latencyMs * stream->getSampleRate() / 1000.0);
        mInputLatencyFrames.store(latencyFrames);
        LOGI("Input latency measured: %.1fms (%lld frames)", latencyMs, (long long)latencyFrames);
    } else {
        // Fallback to estimate: framesPerBurst * 2 (double buffering)
        auto framesPerBurst = stream->getFramesPerBurst();
        int64_t estimatedFrames = framesPerBurst * 2;
        mInputLatencyFrames.store(estimatedFrames);
        LOGI("Input latency estimated: %.1fms (%lld frames) - actual measurement unavailable",
             static_cast<float>(estimatedFrames) / stream->getSampleRate() * 1000.0f,
             (long long)estimatedFrames);
    }
#endif  // WMA_HAS_OBOE
}

// Monitoring functions
void InputNode::setMonitoringEnabled(bool enabled) {
    bool wasEnabled = mMonitoringEnabled.load();
    mMonitoringEnabled.store(enabled, std::memory_order_relaxed);

    if (enabled && !wasEnabled) {
        // Clear monitoring buffer when enabling to avoid stale data
        mMonitoringBuffer.clear();
        LOGI("Monitoring enabled");
    } else if (!enabled && wasEnabled) {
        LOGI("Monitoring disabled");
    }
}

bool InputNode::isMonitoringEnabled() const {
    return mMonitoringEnabled.load(std::memory_order_relaxed);
}

void InputNode::setMonitoringVolume(float volume) {
    // Clamp to 0-1 range
    float clampedVolume = std::max(0.0f, std::min(1.0f, volume));
    mMonitoringVolume.store(clampedVolume, std::memory_order_relaxed);
}

float InputNode::getMonitoringVolume() const {
    return mMonitoringVolume.load(std::memory_order_relaxed);
}

int InputNode::getMonitoringSamples(float* outputBuffer, int numFrames) {
    if (!mMonitoringEnabled.load(std::memory_order_relaxed)) {
        // Monitoring disabled, fill with silence
        std::memset(outputBuffer, 0, numFrames * 2 * sizeof(float));
        return 0;
    }

    size_t samplesToRead = static_cast<size_t>(numFrames * 2);
    size_t available = mMonitoringBuffer.availableToRead();

    // DEBUG: Log read attempts periodically
    static int readAttemptCount = 0;
    if (++readAttemptCount >= 100) {
        LOGI("MONITOR READ: requested=%d frames (%zu samples), available=%zu samples",
             numFrames, samplesToRead, available);
        readAttemptCount = 0;
    }

    // If not enough data, read what's available instead of returning silence
    size_t actualSamplesToRead = samplesToRead;
    int actualFrames = numFrames;

    if (available < samplesToRead) {
        // Read what we have, fill rest with silence
        if (available >= 2) {  // At least 1 stereo frame
            actualSamplesToRead = (available / 2) * 2;  // Round down to stereo frames
            actualFrames = static_cast<int>(actualSamplesToRead / 2);

            // DEBUG: Log partial read
            static int partialCount = 0;
            if (++partialCount >= 50) {
                LOGW("MONITOR PARTIAL: reading %zu of %zu samples", actualSamplesToRead, samplesToRead);
                partialCount = 0;
            }
        } else {
            // Really nothing available
            std::memset(outputBuffer, 0, numFrames * 2 * sizeof(float));
            return 0;
        }
    }

    // Read from monitoring buffer
    bool success = mMonitoringBuffer.read(outputBuffer, actualSamplesToRead);
    if (!success) {
        std::memset(outputBuffer, 0, numFrames * 2 * sizeof(float));
        LOGE("MONITOR READ FAILED");
        return 0;
    }

    // Fill remaining with silence if we did a partial read
    if (actualSamplesToRead < samplesToRead) {
        std::memset(outputBuffer + actualSamplesToRead, 0,
                   (samplesToRead - actualSamplesToRead) * sizeof(float));
    }

    // DEBUG: Log sample values occasionally
    static int sampleValueCount = 0;
    if (++sampleValueCount >= 100) {
        float maxSample = 0.0f;
        for (size_t i = 0; i < actualSamplesToRead && i < 100; ++i) {
            if (std::abs(outputBuffer[i]) > maxSample) {
                maxSample = std::abs(outputBuffer[i]);
            }
        }
        LOGI("MONITOR SAMPLES: actualFrames=%d, maxSample=%.4f", actualFrames, maxSample);
        sampleValueCount = 0;
    }

    return actualFrames;
}

// FIX PHASE 7.2: Feed external audio input (from USB backend)
void InputNode::feedExternalInput(const float* inputData, int numFrames) {
    if (inputData == nullptr || numFrames <= 0) {
        return;
    }

    const size_t numSamples = static_cast<size_t>(numFrames * 2);  // Stereo

    // Use the temp buffer for processing (pre-allocated in prepare())
    assert(mTempBuffer.size() >= numSamples &&
           "Temp buffer too small — prepare() should allocate maxBlockSize * 2");
    if (mTempBuffer.size() < numSamples) {
        LOGW("feedExternalInput: temp buffer too small, clamping frames");
        numFrames = static_cast<int>(mTempBuffer.size()) / 2;
    }

    // Copy input data to temp buffer for processing
    std::copy(inputData, inputData + numSamples, mTempBuffer.data());
    float* processBuffer = mTempBuffer.data();

    // Apply input gain
    float gainLinear = mInputGainLinear.load(std::memory_order_relaxed);
    if (std::abs(gainLinear - 1.0f) > 0.001f) {
        for (size_t i = 0; i < numSamples; ++i) {
            processBuffer[i] *= gainLinear;
        }
    }

    // DC Blocking
    mDCBlocker.process(processBuffer, numFrames);

    // Noise Gate (if enabled)
    if (mNoiseGateEnabled.load(std::memory_order_relaxed)) {
        mNoiseGate.process(processBuffer, numFrames);
    }

    // Level metering
    mLevelMeter.process(processBuffer, numFrames);

    // Write to ring buffer
    size_t available = mRingBuffer.availableToWrite();
    if (available >= numSamples) {
        mRingBuffer.write(processBuffer, numSamples);
    }

    // Write to monitoring buffer if monitoring is enabled
    bool monitoringEnabled = mMonitoringEnabled.load(std::memory_order_relaxed);
    if (monitoringEnabled) {
        float monitorVolume = mMonitoringVolume.load(std::memory_order_relaxed);

        if (std::abs(monitorVolume - 1.0f) > 0.001f) {
            // Use pre-allocated monitoring buffer (avoids RT allocations)
            assert(mMonitorTempBuffer.size() >= numSamples &&
                   "Monitor temp buffer too small — prepare() should allocate maxBlockSize * 2");

            for (size_t i = 0; i < numSamples; ++i) {
                mMonitorTempBuffer[i] = processBuffer[i] * monitorVolume;
            }

            size_t monitorAvailable = mMonitoringBuffer.availableToWrite();
            if (monitorAvailable >= numSamples) {
                mMonitoringBuffer.write(mMonitorTempBuffer.data(), numSamples);
            } else {
                // DIAGNOSTIC: Log when data is dropped due to full buffer
                static int dropCount = 0;
                if (++dropCount >= 100) {
                    LOGW("USB FEED DROP: monitorBuffer full! available=%zu, needed=%zu",
                         monitorAvailable, numSamples);
                    dropCount = 0;
                }
            }
        } else {
            size_t monitorAvailable = mMonitoringBuffer.availableToWrite();
            if (monitorAvailable >= numSamples) {
                mMonitoringBuffer.write(processBuffer, numSamples);
            } else {
                // DIAGNOSTIC: Log when data is dropped due to full buffer
                static int dropCount2 = 0;
                if (++dropCount2 >= 100) {
                    LOGW("USB FEED DROP: monitorBuffer full! available=%zu, needed=%zu",
                         monitorAvailable, numSamples);
                    dropCount2 = 0;
                }
            }
        }
    }

    // Debug logging (periodic) - Enhanced for MIX mode diagnostics
    static int feedLogCount = 0;
    if (++feedLogCount >= 500) {
        float maxSample = 0.0f;
        for (size_t i = 0; i < numSamples && i < 100; ++i) {
            if (std::abs(processBuffer[i]) > maxSample) {
                maxSample = std::abs(processBuffer[i]);
            }
        }
        LOGI("USB FEED: %d frames, maxSample=%.4f, ringAvail=%zu, monitorEnabled=%d, monitorAvail=%zu",
             numFrames, maxSample, mRingBuffer.availableToRead(),
             monitoringEnabled, mMonitoringBuffer.availableToRead());
        feedLogCount = 0;
    }
}
