/**
 * @file jni_benchmark.cpp
 * @brief JNI bindings for latency benchmarking and diagnostics.
 *
 * AudioNativeBridge functions for:
 * - Latency info: getDetailedLatencyInfo, getLatencyReport
 * - Optimization tests: runLatencyOptimizationTest
 * - Round-trip measurement: startRoundTripTest, getRoundTripResult
 * - AAudio availability check
 *
 * Legacy NativeBridge_ functions removed in Phase E.3.
 */

#include "jni_common.h"
#include "../core/AudioEngine.h"
#include "../nodes/InputNode.h"
#include <oboe/Oboe.h>
#include <string>
#include <thread>
#include <chrono>

// NOTE: the Oboe-path round-trip test (nativeStartRoundTripTest / …GetResult /
// …CancelRoundTripTest below) was never implemented — it only ever set a
// "waiting" state that never advanced, so runRoundTripTestFlow() always timed
// out after 5 s. The real round-trip measurement is the USB analog-loopback
// path (docs/usb-audio Fase 5 / RoundTripMeasurer). These three JNI symbols are
// kept as honest deprecated stubs so AudioNativeBridge/LatencyAnalyzer still
// link; startRoundTripTest() now returns false and the Flow ends immediately.

extern "C" {

// ==================== AudioNativeBridge Bindings ====================

JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetDetailedLatencyInfo(
        JNIEnv* env, jobject thiz) {
    jfloatArray result = env->NewFloatArray(8);
    if (result == nullptr) {
        return nullptr;
    }

    float values[8] = {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    if (g_jniState.engine) {
        int32_t sampleRate = 0;
        int32_t bufferSize = 0;
        double latencyMillis = 0.0;

        if (g_jniState.engine->getStreamInfo(sampleRate, bufferSize, latencyMillis)) {
            values[0] = static_cast<float>(latencyMillis);
            values[1] = -1.0f;
            values[2] = static_cast<float>(sampleRate);
            values[3] = static_cast<float>(bufferSize);

            auto* stream = g_jniState.engine->getOutputStream();
            if (stream) {
                values[4] = static_cast<float>(stream->getBufferCapacityInFrames());
                values[5] = (stream->getAudioApi() == oboe::AudioApi::AAudio) ? 1.0f : 0.0f;
                values[6] = (stream->getSharingMode() == oboe::SharingMode::Exclusive) ? 1.0f : 0.0f;
                values[7] = (stream->getPerformanceMode() == oboe::PerformanceMode::LowLatency) ? 1.0f : 0.0f;
            }
        }

        if (g_jniState.inputNode) {
            float inputLatency = g_jniState.inputNode->getInputLatencyMs();
            if (inputLatency > 0) {
                values[1] = inputLatency;
            }
        }
    }

    env->SetFloatArrayRegion(result, 0, 8, values);
    return result;
}

JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeRunLatencyOptimizationTest(
        JNIEnv* env, jobject thiz) {
    jfloatArray result = env->NewFloatArray(4);
    if (result == nullptr) {
        return nullptr;
    }

    float values[4] = {-1.0f, -1.0f, 0.0f, 0.0f};

    if (g_jniState.engine) {
        int32_t sampleRate = 0;
        int32_t bufferSize = 0;
        double currentLatency = 0.0;
        if (g_jniState.engine->getStreamInfo(sampleRate, bufferSize, currentLatency)) {
            values[1] = static_cast<float>(currentLatency);
        }
    }

    {
        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Output)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Exclusive)
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(2);

        std::shared_ptr<oboe::AudioStream> testStream;
        oboe::Result testResult = builder.openStream(testStream);

        if (testResult == oboe::Result::OK && testStream) {
            bool gotExclusive = (testStream->getSharingMode() == oboe::SharingMode::Exclusive);
            values[2] = gotExclusive ? 1.0f : 0.0f;

            if (gotExclusive) {
                testStream->requestStart();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                auto latency = testStream->calculateLatencyMillis();
                if (latency) {
                    values[0] = static_cast<float>(latency.value());
                }
                testStream->requestStop();
            }
            testStream->close();
        }
    }

    if (values[0] > 0 && values[1] > 0) {
        float improvement = values[1] - values[0];
        values[3] = (improvement / values[1]) * 100.0f;
    }

    env->SetFloatArrayRegion(result, 0, 4, values);
    return result;
}

// DEPRECATED stub. The Oboe-path round-trip test was never implemented; use the
// USB analog-loopback measurer (Fase 5). Returns false so the Kotlin Flow ends
// immediately instead of polling a state that never advances.
JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStartRoundTripTest(
        JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return JNI_FALSE;
}

// DEPRECATED stub. Always reports {latencyMs=-1, state=0 (IDLE)}.
JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetRoundTripResult(
        JNIEnv* env, jobject thiz) {
    (void)thiz;
    jfloatArray result = env->NewFloatArray(2);
    if (result == nullptr) {
        return nullptr;
    }
    float values[2] = {-1.0f, 0.0f};  // IDLE
    env->SetFloatArrayRegion(result, 0, 2, values);
    return result;
}

// DEPRECATED stub. No-op.
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeCancelRoundTripTest(
        JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetRecommendedBufferSize(
        JNIEnv* env, jobject thiz, jfloat targetLatencyMs) {
    if (targetLatencyMs <= 0) {
        return -1;
    }

    int sampleRate = 48000;
    if (g_jniState.engine) {
        int32_t sr = 0, bs = 0;
        double lat = 0;
        if (g_jniState.engine->getStreamInfo(sr, bs, lat)) {
            sampleRate = sr;
        }
    }

    int targetFrames = static_cast<int>((targetLatencyMs / 1000.0f) * sampleRate);
    int bufferSize = 64;
    while (bufferSize < targetFrames && bufferSize < 2048) {
        bufferSize *= 2;
    }
    return std::max(64, bufferSize);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsAAudioAvailable(
        JNIEnv* env, jobject thiz) {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output);

    std::shared_ptr<oboe::AudioStream> testStream;
    oboe::Result result = builder.openStream(testStream);

    if (result == oboe::Result::OK && testStream) {
        bool isAAudio = (testStream->getAudioApi() == oboe::AudioApi::AAudio);
        testStream->close();
        return isAAudio ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetLatencyReport(
        JNIEnv* env, jobject thiz) {
    std::string report = "NoisyPad Latency Report\n";
    report += "========================\n\n";

    if (g_jniState.engine) {
        int32_t sampleRate = 0, bufferSize = 0;
        double latencyMillis = 0;

        if (g_jniState.engine->getStreamInfo(sampleRate, bufferSize, latencyMillis)) {
            report += "Sample Rate: " + std::to_string(sampleRate) + " Hz\n";
            report += "Buffer Size: " + std::to_string(bufferSize) + " frames\n";
            report += "Output Latency: " + std::to_string(latencyMillis) + " ms\n";
        }

        auto* stream = g_jniState.engine->getOutputStream();
        if (stream) {
            report += "Audio API: ";
            report += (stream->getAudioApi() == oboe::AudioApi::AAudio) ? "AAudio" : "OpenSL ES";
            report += "\n";
            report += "Sharing Mode: ";
            report += (stream->getSharingMode() == oboe::SharingMode::Exclusive) ? "Exclusive" : "Shared";
            report += "\n";
            report += "Performance Mode: ";
            report += (stream->getPerformanceMode() == oboe::PerformanceMode::LowLatency) ? "LowLatency" : "Normal";
            report += "\n";
        }
    } else {
        report += "Engine not initialized\n";
    }

    return env->NewStringUTF(report.c_str());
}

} // extern "C"
