#include "OboeCaptureStream.h"

#include "../nodes/InputNode.h"
#include "../platform/Logger.h"

#define LOG_TAG "InputNode"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

namespace watermelon_audio {

namespace {

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

oboe::DataCallbackResult OboeCaptureStream::onAudioReady(oboe::AudioStream* stream,
                                                         void* audioData,
                                                         int32_t numFrames) {
    // RT path: one pointer hop plus getChannelCount(), which in Oboe is a
    // plain member read. No allocation, no locking, no indirect call setup.
    //
    // REQ-009 S3 (3.2b) — el xrun de ESTE stream. En AAudio es la lectura de
    // un contador que el propio stream lleva; en OpenSL ES el metodo base
    // devuelve `ErrorUnimplemented` y ahi el nodo se entera de que no sabe,
    // en vez de que le pasen un cero que significaria "todo bien".
    //
    // El adaptador NO decide nada con esto: lo pasa crudo. La regla de que
    // es una costura vive en `InputNode::noteCaptureXRuns()`, que la suite
    // de host SI compila y SI puede manejar — este archivo, no.
    const oboe::ResultWithValue<int32_t> xRuns = stream->getXRunCount();
    return mNode->processInputBlock(static_cast<float*>(audioData),
                                    numFrames,
                                    stream->getChannelCount(),
                                    xRuns ? xRuns.value()
                                          : InputNode::kCaptureXRunsUnknown)
               ? oboe::DataCallbackResult::Continue
               : oboe::DataCallbackResult::Stop;
}

bool OboeCaptureStream::openCaptureStream(InputSource source) {
    oboe::AudioStreamBuilder builder;

    // Use Shared mode for better device routing compatibility
    // This allows proper switching when USB DAC or other devices are connected
    builder.setDirection(oboe::Direction::Input)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Shared)  // Changed from Exclusive for device routing
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(oboe::ChannelCount::Stereo)
            // Don't force sample rate - let system choose best rate for the device
            ->setDataCallback(this)
            ->setInputPreset(inputPresetForSource(source));

    oboe::Result result = builder.openStream(mStream);

    if (result != oboe::Result::OK) {
        LOGE("Failed to open input stream: %s", oboe::convertToText(result));
        return false;
    }

    auto framesPerBurst = mStream->getFramesPerBurst();
    auto actualSampleRate = mStream->getSampleRate();
    auto deviceId = mStream->getDeviceId();
    auto sharingMode = mStream->getSharingMode();

    // DEBUG: Detailed stream info for routing diagnostics
    LOGI("=== INPUT STREAM OPENED ===");
    LOGI("  Sample rate: %d Hz", actualSampleRate);
    LOGI("  Channel count: %d", mStream->getChannelCount());
    LOGI("  Sharing mode: %s", sharingMode == oboe::SharingMode::Shared ? "Shared" : "Exclusive");
    LOGI("  Performance mode: %s",
         mStream->getPerformanceMode() == oboe::PerformanceMode::LowLatency ? "LowLatency" :
         mStream->getPerformanceMode() == oboe::PerformanceMode::PowerSaving ? "PowerSaving" : "None");
    LOGI("  Device ID: %d", deviceId);
    LOGI("  Frames per burst: %d", framesPerBurst);
    LOGI("  Buffer capacity: %d frames", mStream->getBufferCapacityInFrames());
    LOGI("===========================");

    return true;
}

bool OboeCaptureStream::startCaptureStream() {
    if (!mStream) return false;
    oboe::Result result = mStream->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start input stream: %s", oboe::convertToText(result));
        return false;
    }
    return true;
}

void OboeCaptureStream::stopAndCloseCaptureStream() {
    if (mStream) {
        mStream->stop();
        mStream->close();
        mStream.reset();
        LOGI("Input stream closed");
    }
}

int OboeCaptureStream::captureStreamSampleRate() const {
    return mStream ? mStream->getSampleRate() : 0;
}

int64_t OboeCaptureStream::captureStreamLatencyFrames() const {
    if (!mStream) return 0;

    // Try to get actual latency from Oboe
    // calculateLatencyMillis() returns the estimated latency in milliseconds
    oboe::ResultWithValue<double> latencyResult = mStream->calculateLatencyMillis();

    if (latencyResult.error() == oboe::Result::OK) {
        // Convert ms to frames
        double latencyMs = latencyResult.value();
        int64_t latencyFrames = static_cast<int64_t>(latencyMs * mStream->getSampleRate() / 1000.0);
        LOGI("Input latency measured: %.1fms (%lld frames)", latencyMs, (long long)latencyFrames);
        return latencyFrames;
    }
    // Fallback to estimate: framesPerBurst * 2 (double buffering)
    int64_t estimatedFrames = mStream->getFramesPerBurst() * 2;
    LOGI("Input latency estimated: %.1fms (%lld frames) - actual measurement unavailable",
         static_cast<float>(estimatedFrames) / mStream->getSampleRate() * 1000.0f,
         (long long)estimatedFrames);
    return estimatedFrames;
}

}  // namespace watermelon_audio
