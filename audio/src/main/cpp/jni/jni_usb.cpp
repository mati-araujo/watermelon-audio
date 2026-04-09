/**
 * @file jni_usb.cpp
 * @brief USB audio shared state and AudioNativeBridge profiling bindings.
 *
 * This file contains:
 * - UsbDeviceState shared struct (used by jni_audio_bridge.cpp via extern)
 * - USB volume atomics and helper functions (used by audio callbacks)
 * - AudioNativeBridge USB profiling functions
 *
 * Legacy NativeBridge_ functions removed in Phase E.3.
 * All USB operations now use AudioNativeBridge (jni_audio_bridge.cpp).
 */

#include "jni_common.h"
#include "../core/AudioEngine.h"
#include "../backends/BackendManager.h"
#include "../backends/LibusbBackend.h"
#include <cmath>
#include <string>

// ==================== USB Device Shared State ====================
// Used by both this file and jni_audio_bridge.cpp (via extern)

struct UsbDeviceState {
    int fileDescriptor = -1;
    std::string usbfsPath;
    bool isInitialized = false;
    bool isStreaming = false;
};
UsbDeviceState gUsbDeviceState;

// ==================== USB Volume State (Phase 1 - Gain Staging) ====================
// USB volume controls (software fallback when hardware control not available)
// Range: 0.0 to 1.0 linear
// Defined globally to be accessible from jni_audio_bridge.cpp via extern
std::atomic<float> g_usbOutputVolume{1.0f};
std::atomic<float> g_usbInputVolume{1.0f};
std::atomic<bool> g_usbOutputMuted{false};
std::atomic<bool> g_usbInputMuted{false};

// Tracks whether hardware volume control is being used
std::atomic<bool> g_usingHardwareOutputVolume{false};
std::atomic<bool> g_usingHardwareInputVolume{false};

// Public getter for use from audio callback (OutputNode integration)
extern "C" float getUsbOutputVolumeInternal() {
    if (g_usbOutputMuted.load(std::memory_order_relaxed)) {
        return 0.0f;
    }
    return g_usbOutputVolume.load(std::memory_order_relaxed);
}

extern "C" float getUsbInputVolumeInternal() {
    if (g_usbInputMuted.load(std::memory_order_relaxed)) {
        return 0.0f;
    }
    return g_usbInputVolume.load(std::memory_order_relaxed);
}

// ==================== AudioNativeBridge USB Profiling Functions ====================

extern "C" {

JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetUsbProfilingStats(
        JNIEnv *env, jobject thiz) {
    if (!gUsbDeviceState.isInitialized) return nullptr;

    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();

    constexpr int STATS_SIZE = 18;
    jfloat statsArray[STATS_SIZE] = {0};

    if (backend) {
        auto profilingStats = backend->getProfilingStats();
        statsArray[0] = static_cast<float>(profilingStats.outputTransfers.avgLatencyUs);
        statsArray[1] = static_cast<float>(profilingStats.outputTransfers.minLatencyUs);
        statsArray[2] = static_cast<float>(profilingStats.outputTransfers.maxLatencyUs);
        statsArray[3] = static_cast<float>(profilingStats.outputTransfers.p95LatencyUs);
        statsArray[4] = static_cast<float>(profilingStats.outputTransfers.avgJitterUs);
        statsArray[5] = static_cast<float>(profilingStats.inputTransfers.avgLatencyUs);
        statsArray[6] = static_cast<float>(profilingStats.inputTransfers.minLatencyUs);
        statsArray[7] = static_cast<float>(profilingStats.inputTransfers.maxLatencyUs);
        statsArray[8] = static_cast<float>(profilingStats.inputTransfers.p95LatencyUs);
        statsArray[9] = static_cast<float>(profilingStats.inputTransfers.avgJitterUs);
        statsArray[10] = static_cast<float>(profilingStats.dspCallback.avgDurationUs);
        statsArray[11] = static_cast<float>(profilingStats.dspCallback.maxDurationUs);
        statsArray[12] = static_cast<float>(profilingStats.dspCallback.cpuLoadPercent);
        statsArray[13] = static_cast<float>(profilingStats.dspCallback.overrunCount);
        statsArray[14] = static_cast<float>(profilingStats.outputTransfers.ringBufferLatencyMs);
        statsArray[15] = static_cast<float>(profilingStats.outputTransfers.totalEstimatedLatencyMs);
        statsArray[16] = static_cast<float>(profilingStats.healthScore);
        statsArray[17] = static_cast<float>(profilingStats.outputTransfers.totalTransfers);
    }

    jfloatArray result = env->NewFloatArray(STATS_SIZE);
    if (result) {
        env->SetFloatArrayRegion(result, 0, STATS_SIZE, statsArray);
    }
    return result;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetUsbProfilingEnabled(
        JNIEnv *env, jobject thiz, jboolean enabled) {
    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();
    if (backend) {
        auto* profiler = backend->getLatencyProfiler();
        if (profiler) profiler->setEnabled(enabled == JNI_TRUE);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeResetUsbProfilingStats(
        JNIEnv *env, jobject thiz) {
    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();
    if (backend) {
        auto* profiler = backend->getLatencyProfiler();
        if (profiler) profiler->reset();
    }
}

} // extern "C"
