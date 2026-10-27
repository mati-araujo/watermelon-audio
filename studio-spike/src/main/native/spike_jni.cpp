// REQ-055 S3 — shim JNI de la espiga: CoreAudioMacDiag.h hacia la JVM.
// Todo corre en threads de la JVM (control); nada de esto toca el thread de audio,
// que sólo escribe atomics del lado de libwatermelon_audio.
#include <jni.h>

#include "CoreAudioMacDiag.h"

#include <vector>

extern "C" {

JNIEXPORT jlongArray JNICALL
Java_com_watermellonstudios_studiospike_SpikeNative_renderStats(JNIEnv* env, jobject) {
    WmaMacRenderStats s{};
    if (!wma_macos_diag_render_stats(&s)) return nullptr;
    const jlong v[] = {
        (jlong)s.blocks, (jlong)s.overBudget, (jlong)s.discontinuities,
        (jlong)s.sampleTimeInvalid, (jlong)s.halOverloads, (jlong)s.maxRenderNs,
        (jlong)s.sumRenderNs, (jlong)s.lastFrames, (jlong)s.minFrames, (jlong)s.maxFrames,
    };
    const jsize n = (jsize)(sizeof(v) / sizeof(v[0]));
    jlongArray out = env->NewLongArray(n);
    if (out) env->SetLongArrayRegion(out, 0, n, v);
    return out;
}

JNIEXPORT jdoubleArray JNICALL
Java_com_watermellonstudios_studiospike_SpikeNative_deviceInfo(JNIEnv* env, jobject) {
    WmaMacDeviceInfo d{};
    if (!wma_macos_diag_device_info(&d)) return nullptr;
    const jdouble v[] = {
        (jdouble)d.deviceId, d.nominalSampleRate, (jdouble)d.bufferFramesRequested,
        (jdouble)d.bufferFramesActual, (jdouble)d.bufferFramesMin, (jdouble)d.bufferFramesMax,
        (jdouble)d.deviceLatencyFrames, (jdouble)d.safetyOffsetFrames,
        (jdouble)d.streamLatencyFrames, d.presentationLatencySec,
        (jdouble)d.bufferSetStatus, (jdouble)d.overloadListenerStatus,
    };
    const jsize n = (jsize)(sizeof(v) / sizeof(v[0]));
    jdoubleArray out = env->NewDoubleArray(n);
    if (out) env->SetDoubleArrayRegion(out, 0, n, v);
    return out;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_studiospike_SpikeNative_reset(JNIEnv*, jobject) {
    wma_macos_diag_reset();
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_studiospike_SpikeNative_markTouch(JNIEnv*, jobject) {
    wma_macos_diag_mark_touch();
}

JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_studiospike_SpikeNative_nowNs(JNIEnv*, jobject) {
    return (jlong)wma_macos_diag_now_ns();
}

// [touchNs, renderEntryNs, bufferHostNs, frames] x n; null si el reset todavia no se aplico.
JNIEXPORT jlongArray JNICALL
Java_com_watermellonstudios_studiospike_SpikeNative_touchSamples(JNIEnv* env, jobject) {
    std::vector<WmaMacTouchSample> buf(8192);
    const int n = wma_macos_diag_touch_samples(buf.data(), (int)buf.size());
    if (n < 0) return nullptr;
    std::vector<jlong> flat;
    flat.reserve((size_t)n * 4);
    for (int i = 0; i < n; ++i) {
        flat.push_back((jlong)buf[i].touchNs);
        flat.push_back((jlong)buf[i].renderEntryNs);
        flat.push_back((jlong)buf[i].bufferHostNs);
        flat.push_back((jlong)buf[i].frames);
    }
    jlongArray out = env->NewLongArray((jsize)flat.size());
    if (out && !flat.empty()) env->SetLongArrayRegion(out, 0, (jsize)flat.size(), flat.data());
    return out;
}

}  // extern "C"
