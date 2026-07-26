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
JavaVM* g_javaVm = nullptr;        // Cached in JNI_OnLoad for worker-thread attachment

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
    if (g_jniState.inputNode) {
        return true;
    }
    // The node is OWNED by the WmaEngine, exactly like the AudioEngine above:
    // g_jniState.inputNode is a shared_ptr to the same instance, not a second one.
    //
    // Until this was unified the JNI built its own InputNode and attached it to
    // the same AudioEngine, so every wma_input_* function in the C API drove a
    // node the shipping Android path never touched. It never broke on Android
    // because only one of the two paths was ever exercised — and iOS was about to
    // become the first real user of the other one.
    if (!ensureEngine()) {
        LOGE("Cannot create InputNode: engine unavailable");
        return false;
    }
    if (!wmaEnsureInputNode(g_wmaEngine)) {
        LOGE("Failed to create InputNode");
        return false;
    }
    g_jniState.inputNode = g_wmaEngine->inputNode;
    LOGI("InputNode ready (shared with the C API), sampleRate=48000, maxBlockSize=4096");
    return g_jniState.inputNode != nullptr;
}

void releaseInputNode() {
    // Both handles have to drop together: leaving the engine's copy alive would
    // recreate the split this function exists to prevent.
    //
    // The stream stop and the engine-side drop are wma_input_release()'s job
    // (it takes inputNodeMutex). What stays here is the half the C API cannot
    // know about: the JNI's own mirror of the shared_ptr. Note the node itself
    // survives until this second handle drops — which is why the stop happens
    // first, inside the C API, rather than being left to the destructor.
    wma_input_release(g_wmaEngine);
    g_jniState.inputNode.reset();
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

    g_javaVm = vm;  // Workers attach via this in jni_audio_bridge.cpp (LooperStateListener sink).
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

    // Stop and release InputNode first (drops both handles — see releaseInputNode).
    // Must run before g_wmaEngine is cleared: it reaches through it.
    releaseInputNode();

    // Both pointers are cleared BEFORE the owner is destroyed, not after.
    // Since WA-2.6 the lifecycle entry points read g_wmaEngine rather than
    // g_jniState.engine, so it needs the same treatment the raw pointer already
    // had: leaving it published across wma_engine_destroy() would widen the
    // window in which a concurrent call can reach a half-destroyed engine.
    // Neither read is synchronised against this, so the ordering is all there is.
    WmaEngine* engine = g_wmaEngine;
    g_wmaEngine = nullptr;
    g_jniState.engine = nullptr;

    // Destroys AudioEngine + BackendManager.
    if (engine) {
        wma_engine_destroy(engine);
    }

    g_jniCache.release(env);
    LOGI("JNI_OnUnload: Native library unloaded");
}

} // extern "C"
