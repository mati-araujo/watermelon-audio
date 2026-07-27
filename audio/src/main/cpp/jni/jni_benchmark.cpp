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
 *
 * WA-2.6, category `benchmark`: this file only PARTLY delegates to the C API,
 * and that is the honest outcome rather than a half-finished migration. What
 * stays here, and why:
 *
 *   - runLatencyOptimizationTest() and isAAudioAvailable() open an
 *     oboe::AudioStreamBuilder to probe the device. There is no portable
 *     question being asked — "did I get AAudio in exclusive mode" is an Oboe
 *     concept — so there is nothing to lift.
 *   - The tail of getDetailedLatencyInfo() and of getLatencyReport(): the
 *     audio API, sharing mode and performance mode of an oboe::AudioStream.
 *     Same reason. Their portable halves DID move.
 *   - startRoundTripTest / getRoundTripResult / cancelRoundTripTest are
 *     deprecated stubs that never touch the engine. Migrating a stub that
 *     returns a constant would add a C API function with no behaviour.
 *   - getAdaptiveBufferStats() (in jni_audio_bridge.cpp) reads the
 *     LibusbBackend directly — Android-only by D4, like the rest of USB.
 *
 * See docs/kmp/c_api_coverage.md §4b.
 */

#include "jni_common.h"
#include "../api/watermelon_audio.h"
#include "../core/AudioEngine.h"
#include "../nodes/InputNode.h"
#include <oboe/Oboe.h>
#include <algorithm>
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

// The portable head ([0..3]) comes from the C API; the Oboe tail ([4..7])
// describes an oboe::AudioStream and has no counterpart off Android, so it is
// filled here. Note the tail is all zeros whenever BackendManager owns the
// stream (getOutputStream() returns nullptr there by design) — that is the USB
// path today and every path on iOS.
JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetDetailedLatencyInfo(
        JNIEnv* env, jobject thiz) {
    jfloatArray result = env->NewFloatArray(8);
    if (result == nullptr) {
        return nullptr;
    }

    float values[8] = {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    int sampleRate = 0, bufferSize = 0;
    float latencyMillis = 0.0f;
    if (wma_get_stream_info(g_wmaEngine, &sampleRate, &bufferSize, &latencyMillis)) {
        values[0] = latencyMillis;
        values[2] = static_cast<float>(sampleRate);
        values[3] = static_cast<float>(bufferSize);
    }

    // Outside the getStreamInfo() branch on purpose: the input path can be up
    // with no output stream to report, and the old code already read it
    // unconditionally. Its -1 default comes from the initializer.
    const float inputLatency = wma_input_get_latency_ms(g_wmaEngine);
    if (inputLatency > 0) {
        values[1] = inputLatency;
    }

    if (g_jniState.engine) {
        auto* stream = g_jniState.engine->getOutputStream();
        if (stream) {
            values[4] = static_cast<float>(stream->getBufferCapacityInFrames());
            values[5] = (stream->getAudioApi() == oboe::AudioApi::AAudio) ? 1.0f : 0.0f;
            values[6] = (stream->getSharingMode() == oboe::SharingMode::Exclusive) ? 1.0f : 0.0f;
            values[7] = (stream->getPerformanceMode() == oboe::PerformanceMode::LowLatency) ? 1.0f : 0.0f;
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
    // Behaviour change, on purpose: the sample rate now resolves through
    // currentSampleRate() (running stream -> preferred -> 48000) instead of
    // falling straight to 48000 when no stream is up. See the C API.
    return wma_get_recommended_buffer_size(g_wmaEngine, targetLatencyMs);
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

// Portable body from the C API, Oboe-specific tail appended here — the same
// split as nativeGetDetailedLatencyInfo. The C API version also names the
// backend, which this report never did.
JNIEXPORT jstring JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetLatencyReport(
        JNIEnv* env, jobject thiz) {
    // Ask for the length first rather than guessing at a buffer: the report
    // grows with the backend name and the optional input-latency line.
    const int needed = wma_get_latency_report(g_wmaEngine, nullptr, 0);
    std::string report;
    if (needed > 0) {
        report.resize(static_cast<size_t>(needed) + 1);
        const int written = wma_get_latency_report(g_wmaEngine, report.data(),
                                                   static_cast<int>(report.size()));
        report.resize(static_cast<size_t>(std::min(written, needed)));
    }

    if (g_jniState.engine) {
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
    }

    return env->NewStringUTF(report.c_str());
}

} // extern "C"
