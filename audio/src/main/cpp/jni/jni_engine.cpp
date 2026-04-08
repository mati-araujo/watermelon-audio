/**
 * @file jni_engine.cpp
 * @brief Global JNI state definitions and helper implementations.
 *
 * This file contains:
 * - Global state definitions (g_jniState, g_jniCache, g_floatArrayPool)
 * - Helper function implementations (ensureEngine, ensureInputNode)
 * - JNI_OnLoad / JNI_OnUnload
 *
 * Legacy NativeBridge_ functions removed in Phase E.3.
 * All JNI operations now use AudioNativeBridge (jni_audio_bridge.cpp).
 */

#include "jni_common.h"
#include "../core/AudioEngine.h"
#include "../core/AudioMode.h"
#include "../nodes/InputNode.h"
#include "../api/watermelon_audio.h"
#include "../api/watermelon_audio_internal.h"
#include <algorithm>

// ==================== Global State Definitions ====================

JniGlobalState g_jniState;
WmaEngine* g_wmaEngine = nullptr;  // Owns AudioEngine + BackendManager (Phase 0D)
JniCache g_jniCache;

// Phase 4.5: Float array pool for high-frequency operations
FloatArrayPool g_floatArrayPool;

// ==================== Helper Implementations ====================

bool ensureEngine() {
    std::lock_guard<std::mutex> lock(g_jniState.engineMutex);
    if (!g_jniState.engine) {
        LOGI("Creating AudioEngine via WmaEngine (Phase 0D)");
        g_wmaEngine = wma_engine_create();
        if (g_wmaEngine) {
            // Set raw pointer for fast JNI access (non-owning)
            g_jniState.engine = g_wmaEngine->engine.get();
            return true;
        } else {
            LOGE("Failed to create WmaEngine");
            return false;
        }
    }
    return true;
}

bool ensureInputNode() {
    if (!g_jniState.inputNode) {
        LOGI("Creating InputNode instance");
        try {
            g_jniState.inputNode = std::make_shared<InputNode>();
            if (g_jniState.inputNode) {
                // Prepare with 48kHz and generous block size for Oboe callbacks
                // Will be reconfigured if engine restarts at a different rate
                g_jniState.inputNode->prepare(48000, 4096);
                LOGI("InputNode prepared with sampleRate=48000, maxBlockSize=4096");
            }
            return g_jniState.inputNode != nullptr;
        } catch (const std::exception& e) {
            LOGE("Failed to create InputNode: %s", e.what());
            return false;
        }
    }
    return true;
}

// ==================== JniCache Implementation ====================

void JniCache::initialize(JNIEnv* env) {
    if (isInitialized) return;

    jclass localClass = env->FindClass(
        "com/watermellonstudios/audio/api/NativeEffectSnapshot"
    );
    if (localClass != nullptr) {
        nativeEffectSnapshotClass = (jclass)env->NewGlobalRef(localClass);
        env->DeleteLocalRef(localClass);
    } else {
        LOGW("JniCache: NativeEffectSnapshot class not found");
        env->ExceptionClear();
    }

    isInitialized = true;
    LOGI("JniCache initialized");
}

void JniCache::release(JNIEnv* env) {
    if (nativeEffectSnapshotClass != nullptr) {
        env->DeleteGlobalRef(nativeEffectSnapshotClass);
        nativeEffectSnapshotClass = nullptr;
    }
    isInitialized = false;
    LOGI("JniCache released");
}

// ==================== JNI_OnLoad / JNI_OnUnload ====================

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    JNIEnv* env;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    g_jniCache.initialize(env);
    LOGI("JNI_OnLoad: Native library loaded");

    return JNI_VERSION_1_6;
}

JNIEXPORT void JNI_OnUnload(JavaVM* vm, void* reserved) {
    JNIEnv* env;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_jniState.engineMutex);

    // Stop and release InputNode first
    if (g_jniState.inputNode) {
        g_jniState.inputNode->stopInputStream();
        g_jniState.inputNode.reset();
    }

    // Clear raw pointer before destroying owner
    g_jniState.engine = nullptr;

    // Destroy WmaEngine (which owns AudioEngine + BackendManager)
    if (g_wmaEngine) {
        wma_engine_destroy(g_wmaEngine);
        g_wmaEngine = nullptr;
    }

    g_jniCache.release(env);
    LOGI("JNI_OnUnload: Native library unloaded");
}

} // extern "C"
