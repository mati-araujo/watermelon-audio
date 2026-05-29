/**
 * @file jni_audio_bridge.cpp
 * @brief JNI bindings for the unified AudioNativeBridge.
 *
 * This file provides JNI functions for AudioNativeBridge which consolidates:
 * - NativeAudioBridge (effects operations)
 * - NativeBridge (lifecycle, oscillator, input, mode, etc.)
 *
 * The functions delegate to the same underlying engine but use the new
 * unified class naming convention:
 *   Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeXxx
 *
 * This allows gradual migration from the old bridges to the new unified bridge.
 */

#include "jni_common.h"
#include "../core/AudioEngine.h"
#include "../core/AudioMode.h"
#include "../core/ModeConfigurations.h"
#include "../nodes/InputNode.h"
#include "../backends/BackendManager.h"
#include "../backends/LibusbBackend.h"
#include "../looper/LooperEventDispatcher.h"
#include "../usb/UsbSnapshotCodec.h"
#include "../voice/VoiceTypes.h"
#include <cmath>
#include <algorithm>
#include <mutex>
#include <vector>

// External declarations from jni_usb.cpp for USB volume
extern "C" float getUsbOutputVolumeInternal();
extern "C" float getUsbInputVolumeInternal();

extern "C" {

// ==================== Lifecycle Functions ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStartEngine(
    JNIEnv* env, jobject thiz) {
    if (!ensureEngine()) {
        LOGE("AudioNativeBridge.startEngine: Failed to create engine");
        return;
    }
    g_jniState.engine->start();
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStopEngine(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine) {
        g_jniState.engine->stop();
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStartEngineWithFade(
    JNIEnv* env, jobject thiz, jint fadeTimeMs) {
    if (!ensureEngine()) {
        LOGE("AudioNativeBridge.startEngineWithFade: Failed to create engine");
        return;
    }
    g_jniState.engine->startWithFade(fadeTimeMs);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStopEngineWithFade(
    JNIEnv* env, jobject thiz, jint fadeTimeMs) {
    if (g_jniState.engine) {
        g_jniState.engine->stopWithFade(fadeTimeMs);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativePauseEngineWithFade(
    JNIEnv* env, jobject thiz, jint fadeTimeMs) {
    if (g_jniState.engine) {
        g_jniState.engine->pauseWithFade(fadeTimeMs);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeResumeEngineWithFade(
    JNIEnv* env, jobject thiz, jint fadeTimeMs) {
    if (g_jniState.engine) {
        g_jniState.engine->resumeWithFade(fadeTimeMs);
    }
}

// ==================== State Functions ====================

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetEngineState(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return 0;
    }
    return static_cast<jint>(g_jniState.engine->getEngineState());
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetIsPaused(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return JNI_FALSE;
    }
    return g_jniState.engine->getIsPaused() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetStateVersion(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return 0;
    }
    return static_cast<jlong>(g_jniState.engine->getStateVersion());
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeHasStreamError(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return JNI_FALSE;
    }
    return g_jniState.engine->hasStreamError() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetLastStreamErrorCode(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return 0;
    }
    return g_jniState.engine->getLastStreamErrorCode();
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeClearStreamError(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine) {
        g_jniState.engine->clearStreamError();
    }
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeHasInitializationFailed(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return JNI_FALSE;
    }
    return g_jniState.engine->hasInitializationFailed() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsEngineInitialized(
    JNIEnv* env, jobject thiz) {
    return (g_jniState.engine != nullptr) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetStreamInfo(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return nullptr;
    }
    int32_t sampleRate, bufferSize;
    double latencyMillis;
    bool hasStream = g_jniState.engine->getStreamInfo(sampleRate, bufferSize, latencyMillis);
    if (!hasStream) {
        return nullptr;
    }
    jfloatArray result = env->NewFloatArray(3);
    if (result) {
        float data[3] = {
            static_cast<float>(sampleRate),
            static_cast<float>(bufferSize),
            static_cast<float>(latencyMillis)
        };
        env->SetFloatArrayRegion(result, 0, 3, data);
    }
    return result;
}

// ==================== Volume Functions ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetMasterVolume(
    JNIEnv* env, jobject thiz, jfloat volume) {
    if (!ensureEngine()) {
        return;
    }
    volume = clampFloat(volume, 0.0f, 1.0f);
    g_jniState.engine->setMasterVolume(volume);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetCurrentFadeVolume(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return 0.0f;
    }
    return g_jniState.engine->getCurrentFadeVolume();
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetTargetFadeVolume(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return 0.0f;
    }
    return g_jniState.engine->getTargetFadeVolume();
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetIsFading(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return JNI_FALSE;
    }
    return g_jniState.engine->getIsFading() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetFadeProgress(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return 0.0f;
    }
    return g_jniState.engine->getFadeProgress();
}

// ==================== Real-time Functions ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetXY(
    JNIEnv* env, jobject thiz, jfloat x, jfloat y) {
    if (!ensureEngine()) {
        return;
    }
    x = clampFloat(x, 0.0f, 1.0f);
    y = clampFloat(y, 0.0f, 1.0f);
    g_jniState.engine->updateXY(x, y);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetFrequencyAndAmplitude(
    JNIEnv* env, jobject thiz, jfloat frequency, jfloat amplitude) {
    if (!ensureEngine()) {
        return;
    }
    frequency = clampFloat(frequency, 20.0f, 20000.0f);
    amplitude = clampFloat(amplitude, 0.0f, 1.0f);
    g_jniState.engine->setFrequencyAndAmplitude(frequency, amplitude);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetFrequencyRange(
    JNIEnv* env, jobject thiz, jfloat minHz, jfloat maxHz) {
    if (!ensureEngine()) {
        return;
    }
    if (!std::isfinite(minHz) || !std::isfinite(maxHz) || maxHz <= minHz) {
        return;
    }
    g_jniState.engine->setFrequencyRange(minHz, maxHz);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetOscillatorType(
    JNIEnv* env, jobject thiz, jint type) {
    if (!ensureEngine()) {
        return;
    }
    if (type < 0 || type > 4) {
        return;
    }
    g_jniState.engine->setOscillatorType(type);
}

// ========== SYNTH ENGINE SYSTEM (Phase 6) ==========

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetEngineType(
    JNIEnv* env, jobject thiz, jint engineType) {
    if (!ensureEngine()) {
        return;
    }
    g_jniState.engine->setEngineType(engineType);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetEngineParameter(
    JNIEnv* env, jobject thiz, jint paramId, jfloat value) {
    if (!ensureEngine()) {
        return;
    }
    g_jniState.engine->setEngineParameter(paramId, value);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetEngineType(
    JNIEnv* env, jobject thiz) {
    if (!ensureEngine()) {
        return 0;
    }
    return g_jniState.engine->getEngineType();
}

// ========== SOUNDFONT ENGINE (Phase 8) ==========

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLoadSoundFont(
    JNIEnv* env, jobject thiz, jbyteArray data) {
    if (!ensureEngine()) return JNI_FALSE;
    jsize size = env->GetArrayLength(data);
    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    if (!bytes) return JNI_FALSE;
    bool result = g_jniState.engine->loadSoundFont(bytes, size);
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLoadSoundFontFromPath(
    JNIEnv* env, jobject thiz, jstring path) {
    if (!ensureEngine()) return JNI_FALSE;
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (!pathStr) return JNI_FALSE;
    bool result = g_jniState.engine->loadSoundFontFromPath(pathStr);
    env->ReleaseStringUTFChars(path, pathStr);
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeUnloadSoundFont(
    JNIEnv* env, jobject thiz) {
    if (!ensureEngine()) return;
    g_jniState.engine->unloadSoundFont();
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetSoundFontPreset(
    JNIEnv* env, jobject thiz, jint presetIndex) {
    if (!ensureEngine()) return;
    g_jniState.engine->setSoundFontPreset(presetIndex);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetSoundFontPresetCount(
    JNIEnv* env, jobject thiz) {
    if (!ensureEngine()) return 0;
    return g_jniState.engine->getSoundFontPresetCount();
}

JNIEXPORT jstring JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetSoundFontPresetName(
    JNIEnv* env, jobject thiz, jint presetIndex) {
    if (!ensureEngine()) return nullptr;
    const char* name = g_jniState.engine->getSoundFontPresetName(presetIndex);
    if (!name) return nullptr;
    return env->NewStringUTF(name);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsSoundFontLoaded(
    JNIEnv* env, jobject thiz) {
    if (!ensureEngine()) return JNI_FALSE;
    return g_jniState.engine->isSoundFontLoaded() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jintArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetSoundFontPresetKeyRange(
    JNIEnv* env, jobject thiz, jint presetIndex) {
    if (!ensureEngine()) return nullptr;
    int minKey = 0, maxKey = 127;
    bool ok = g_jniState.engine->getSoundFontPresetKeyRange(presetIndex, minKey, maxKey);
    if (!ok) return nullptr;
    jintArray result = env->NewIntArray(2);
    if (!result) return nullptr;
    jint buf[2] = { minKey, maxKey };
    env->SetIntArrayRegion(result, 0, 2, buf);
    return result;
}

// ========== SOUNDFONT POLYPHONY (Phase 8E) ==========

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSfNoteOn(
    JNIEnv* env, jobject thiz, jint touchId, jint midiNote, jfloat velocity) {
    if (!ensureEngine()) return;
    g_jniState.engine->sfNoteOn(touchId, midiNote, velocity);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSfNoteOff(
    JNIEnv* env, jobject thiz, jint touchId) {
    if (!ensureEngine()) return;
    g_jniState.engine->sfNoteOff(touchId);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSfNoteOffAll(
    JNIEnv* env, jobject thiz) {
    if (!ensureEngine()) return;
    g_jniState.engine->sfNoteOffAll();
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSfNoteOffAllExcept(
    JNIEnv* env, jobject thiz, jint keepTouchId) {
    if (!ensureEngine()) return;
    g_jniState.engine->sfNoteOffAllExcept(keepTouchId);
}

// ========== VOICE FILTER (Phase 6) ==========

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetVoiceFilterEnabled(
    JNIEnv* env, jobject thiz, jboolean enabled) {
    if (!ensureEngine()) return;
    g_jniState.engine->setVoiceFilterEnabled(enabled);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetVoiceFilterCutoff(
    JNIEnv* env, jobject thiz, jfloat hz) {
    if (!ensureEngine()) return;
    g_jniState.engine->setVoiceFilterCutoff(hz);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetVoiceFilterResonance(
    JNIEnv* env, jobject thiz, jfloat q) {
    if (!ensureEngine()) return;
    g_jniState.engine->setVoiceFilterResonance(q);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetVoiceFilterMode(
    JNIEnv* env, jobject thiz, jint mode) {
    if (!ensureEngine()) return;
    g_jniState.engine->setVoiceFilterMode(mode);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetWaveformSamples(
    JNIEnv* env, jobject thiz, jfloatArray buffer, jint size) {
    if (!g_jniState.engine || buffer == nullptr || size <= 0) {
        return 0;
    }

    jsize bufferSize = env->GetArrayLength(buffer);
    int samplesToGet = std::min(static_cast<int>(bufferSize), static_cast<int>(size));

    ScopedFloatArrayRW samples(env, buffer);
    if (!samples.isValid()) {
        return 0;
    }

    return g_jniState.engine->getWaveformSamples(samples.get(), samplesToGet);
}

// ==================== Modulator Functions ====================

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetModulatorType(
    JNIEnv* env, jobject thiz, jint type) {
    if (!ensureEngine()) {
        return JniError::ENGINE_NOT_INITIALIZED;
    }
    if (type < 0 || type > 7) {
        LOGE("Invalid modulator type: %d (valid range: 0-7)", type);
        return JniError::INVALID_PARAMETER_ID;
    }
    g_jniState.engine->setModulatorType(type);
    return JniError::SUCCESS;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetModulatorParameter(
    JNIEnv* env, jobject thiz, jint paramId, jfloat value) {
    if (!ensureEngine()) {
        return JniError::ENGINE_NOT_INITIALIZED;
    }
    if (paramId < 0) {
        LOGE("Invalid modulator paramId: %d", paramId);
        return JniError::INVALID_PARAMETER_ID;
    }
    if (!isValidFloat(value)) {
        LOGE("Invalid modulator parameter value: %f (not finite)", value);
        return JniError::PARAMETER_OUT_OF_RANGE;
    }
    g_jniState.engine->setModulatorParameter(paramId, value);
    return JniError::SUCCESS;
}

// ==================== Effect Functions ====================
// These delegate to the same implementations as NativeAudioBridge

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeAddEffect(
    JNIEnv* env, jobject thiz, jint typeId) {
    if (!g_jniState.engine) {
        return JniError::ENGINE_NOT_INITIALIZED;
    }
    if (typeId < 0 || typeId >= static_cast<int>(EFFECT_TYPE_COUNT)) {
        return JniError::INVALID_EFFECT_TYPE;
    }
    try {
        bool success = g_jniState.engine->addEffect(static_cast<EffectType>(typeId));
        if (!success) {
            return JniError::EFFECT_CHAIN_FULL;
        }
        return static_cast<int>(g_jniState.engine->getNumEffects()) - 1;
    } catch (const std::bad_alloc&) {
        return JniError::MEMORY_ALLOCATION_FAILED;
    } catch (...) {
        return JniError::UNKNOWN_ERROR;
    }
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeRemoveEffect(
    JNIEnv* env, jobject thiz, jint index) {
    if (!g_jniState.engine) {
        return JniError::ENGINE_NOT_INITIALIZED;
    }
    if (index < 0 || static_cast<size_t>(index) >= g_jniState.engine->getNumEffects()) {
        return JniError::INVALID_EFFECT_INDEX;
    }
    try {
        g_jniState.engine->removeEffect(static_cast<size_t>(index));
        return JniError::SUCCESS;
    } catch (...) {
        return JniError::UNKNOWN_ERROR;
    }
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeClearAllEffects(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return JniError::ENGINE_NOT_INITIALIZED;
    }
    try {
        g_jniState.engine->clearAllEffects();
        return JniError::SUCCESS;
    } catch (...) {
        return JniError::UNKNOWN_ERROR;
    }
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetEffectParameter(
    JNIEnv* env, jobject thiz, jint index, jint paramId, jfloat value) {
    if (!g_jniState.engine) {
        return JniError::ENGINE_NOT_INITIALIZED;
    }
    if (index < 0 || static_cast<size_t>(index) >= g_jniState.engine->getNumEffects()) {
        return JniError::INVALID_EFFECT_INDEX;
    }
    if (paramId < 0) {
        return JniError::INVALID_PARAMETER_ID;
    }
    if (!std::isfinite(value)) {
        return JniError::PARAMETER_OUT_OF_RANGE;
    }
    try {
        g_jniState.engine->setParameter(static_cast<size_t>(index), paramId, value);
        return JniError::SUCCESS;
    } catch (...) {
        return JniError::UNKNOWN_ERROR;
    }
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetEffectParametersBatch(
    JNIEnv* env, jobject thiz, jint index, jintArray paramIds, jfloatArray values) {
    if (!g_jniState.engine) {
        return JniError::ENGINE_NOT_INITIALIZED;
    }
    if (index < 0 || static_cast<size_t>(index) >= g_jniState.engine->getNumEffects()) {
        return JniError::INVALID_EFFECT_INDEX;
    }

    jsize length = env->GetArrayLength(paramIds);
    if (length != env->GetArrayLength(values) || length == 0) {
        return JniError::SUCCESS;
    }

    ScopedIntArrayRW ids(env, paramIds);
    ScopedFloatArrayRW vals(env, values);

    if (!ids.isValid() || !vals.isValid()) {
        return JniError::MEMORY_ALLOCATION_FAILED;
    }

    try {
        // Single-effect batch: build a stack-allocated effectIndices array and
        // route through setParametersBatch so we get one state-version bump
        // for the whole batch instead of N.
        std::vector<int> effectIndices(static_cast<size_t>(length), index);
        g_jniState.engine->setParametersBatch(
            effectIndices.data(),
            ids.get(),
            vals.get(),
            static_cast<size_t>(length)
        );
        return JniError::SUCCESS;
    } catch (...) {
        return JniError::UNKNOWN_ERROR;
    }
}

// Phase 4.1: Multi-effect batch parameter update
JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetMultipleEffectParameters(
    JNIEnv* env, jobject thiz,
    jintArray effectIndices,
    jintArray paramIds,
    jfloatArray values) {

    if (!g_jniState.engine) {
        return JniError::ENGINE_NOT_INITIALIZED;
    }

    ScopedIntArrayRW indices(env, effectIndices);
    ScopedIntArrayRW params(env, paramIds);
    ScopedFloatArrayRW vals(env, values);

    if (!indices.isValid() || !params.isValid() || !vals.isValid()) {
        return JniError::MEMORY_ALLOCATION_FAILED;
    }

    jsize length = indices.size();
    if (length != params.size() || length != vals.size()) {
        LOGE("nativeSetMultipleEffectParameters: array size mismatch");
        return JniError::INVALID_OPERATION;
    }

    if (length == 0) {
        return JniError::SUCCESS;
    }

    try {
        // Single state-version bump at the end (AUD-6): scene loads previously
        // produced N version bumps for N parameters, causing the Kotlin
        // synchronizer to potentially observe partial states between updates.
        g_jniState.engine->setParametersBatch(
            indices.get(),
            params.get(),
            vals.get(),
            static_cast<size_t>(length)
        );
        LOGI("nativeSetMultipleEffectParameters: applied %d updates", static_cast<int>(length));
        return JniError::SUCCESS;
    } catch (const std::exception& e) {
        LOGE("nativeSetMultipleEffectParameters: exception: %s", e.what());
        return JniError::UNKNOWN_ERROR;
    }
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetEffectBypass(
    JNIEnv* env, jobject thiz, jint index, jboolean bypass) {
    if (!g_jniState.engine) {
        return JniError::ENGINE_NOT_INITIALIZED;
    }
    if (index < 0 || static_cast<size_t>(index) >= g_jniState.engine->getNumEffects()) {
        return JniError::INVALID_EFFECT_INDEX;
    }
    try {
        g_jniState.engine->setBypass(static_cast<size_t>(index), bypass);
        return JniError::SUCCESS;
    } catch (...) {
        return JniError::UNKNOWN_ERROR;
    }
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeReorderEffects(
    JNIEnv* env, jobject thiz, jint fromIndex, jint toIndex) {
    if (!g_jniState.engine) {
        return JniError::ENGINE_NOT_INITIALIZED;
    }
    size_t chainSize = g_jniState.engine->getNumEffects();
    if (fromIndex < 0 || static_cast<size_t>(fromIndex) >= chainSize ||
        toIndex < 0 || static_cast<size_t>(toIndex) >= chainSize) {
        return JniError::INVALID_EFFECT_INDEX;
    }
    try {
        g_jniState.engine->reorderEffects(static_cast<size_t>(fromIndex), static_cast<size_t>(toIndex));
        return JniError::SUCCESS;
    } catch (...) {
        return JniError::UNKNOWN_ERROR;
    }
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetEffectChainSize(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return 0;
    }
    return static_cast<jint>(g_jniState.engine->getNumEffects());
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetEffectType(
    JNIEnv* env, jobject thiz, jint index) {
    if (!g_jniState.engine || index < 0 ||
        static_cast<size_t>(index) >= g_jniState.engine->getNumEffects()) {
        return -1;
    }
    try {
        return static_cast<jint>(g_jniState.engine->getEffectType(static_cast<size_t>(index)));
    } catch (...) {
        return -1;
    }
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetEffectParameter(
    JNIEnv* env, jobject thiz, jint index, jint paramId) {
    if (!g_jniState.engine || index < 0 ||
        static_cast<size_t>(index) >= g_jniState.engine->getNumEffects()) {
        return 0.0f;
    }
    try {
        return g_jniState.engine->getParameter(static_cast<size_t>(index), paramId);
    } catch (...) {
        return 0.0f;
    }
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsEffectBypassed(
    JNIEnv* env, jobject thiz, jint index) {
    if (!g_jniState.engine || index < 0 ||
        static_cast<size_t>(index) >= g_jniState.engine->getNumEffects()) {
        return JNI_FALSE;
    }
    try {
        return g_jniState.engine->isBypassed(static_cast<size_t>(index)) ? JNI_TRUE : JNI_FALSE;
    } catch (...) {
        return JNI_FALSE;
    }
}

// ==================== Global BPM ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetBpm(
    JNIEnv* env, jobject thiz, jfloat bpm) {
    if (!g_jniState.engine) return;
    g_jniState.engine->setBpm(bpm);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetBpm(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 120.0f;
    return g_jniState.engine->getBpm();
}

// ==================== Effect Routing Mode ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetRoutingMode(
    JNIEnv* env, jobject thiz, jint mode) {
    if (!g_jniState.engine) return;
    if (mode < 0 || mode > 5) return;  // RoutingMode range validation
    g_jniState.engine->setRoutingMode(static_cast<RoutingMode>(mode));
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetRoutingMode(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 0;
    return g_jniState.engine->getRoutingMode();
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetParallelMix(
    JNIEnv* env, jobject thiz, jfloat mix) {
    if (!g_jniState.engine) return;
    g_jniState.engine->setParallelMix(mix);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetFeedbackAmount(
    JNIEnv* env, jobject thiz, jfloat amount) {
    if (!g_jniState.engine) return;
    g_jniState.engine->setFeedbackAmount(amount);
}

// ==================== Mode Functions ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetAudioMode(
    JNIEnv* env, jobject thiz, jint mode) {

    LOGI("AudioNativeBridge.setAudioMode: ENTER mode=%d", mode);

    if (!ensureEngine()) {
        LOGE("AudioNativeBridge.setAudioMode: engine not available");
        return;
    }

    if (mode < 0 || mode > 2) {
        LOGE("AudioNativeBridge.setAudioMode: invalid mode %d", mode);
        return;
    }

    try {
        auto audioMode = static_cast<watermelon_audio::AudioMode>(mode);

        switch (audioMode) {
            case watermelon_audio::AudioMode::CHAOS_PAD:
                LOGI("AudioNativeBridge.setAudioMode: configuring CHAOS_PAD");
                g_jniState.engine->setOscillatorEnabled(true);
                g_jniState.engine->setVocoderCarrierSource(false);
                g_jniState.engine->setVocoderModulatorSource(g_jniState.inputNode != nullptr);
                if (g_jniState.inputNode) {
                    g_jniState.inputNode->setMonitoringEnabled(false);
                }
                break;

            case watermelon_audio::AudioMode::INPUT_FX: {
                LOGI("AudioNativeBridge.setAudioMode: configuring INPUT_FX");
                // Request an effect chain state reset BEFORE flipping
                // oscillatorEnabled. On the next audio callback the
                // audio thread will zero-fill the chain's scratch and
                // feedback buffers and call reset() on every effect,
                // stopping stale reverb tails / delay feedback cooked
                // by chaos_pad from bleeding into the first blocks of
                // mic processing as a loud burst. Without this, the
                // longer the user stays in chaos_pad, the louder the
                // residual when entering INPUT_FX.
                g_jniState.engine->requestResetEffectChain();
                g_jniState.engine->setOscillatorEnabled(false);
                g_jniState.engine->setVocoderCarrierSource(true);

                // Ensure InputNode exists (may not have been created yet)
                ensureInputNode();

                auto& backendManager = watermelon_audio::BackendManager::getInstance();
                auto backendType = backendManager.getCurrentType();
                bool isUsbActive = (backendType == watermelon_audio::BackendType::LIBUSB);
                wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
                    "SET_MODE_INPUT_FX: inputNode=%p, backendType=%d, isUsbActive=%d",
                    static_cast<void*>(g_jniState.inputNode.get()),
                    static_cast<int>(backendType),
                    isUsbActive);

                if (g_jniState.inputNode) {
                    if (isUsbActive) {
                        LOGI("AudioNativeBridge.setAudioMode: USB active, stopping Oboe input");
                        if (g_jniState.inputNode->isInputStreamRunning()) {
                            g_jniState.inputNode->stopInputStream();
                        }
                        // USB INPUT_FX: data arrives via IAudioCallback::onAudioReady(inputData)
                        // No Oboe input stream needed — LibusbBackend provides input directly
                    } else {
                        if (!g_jniState.inputNode->isInputStreamRunning()) {
                            LOGI("AudioNativeBridge.setAudioMode: starting Oboe input stream");
                            g_jniState.inputNode->startInputStream();
                        }
                    }
                    g_jniState.inputNode->setMonitoringEnabled(true);
                    g_jniState.engine->setInputNode(g_jniState.inputNode);

                    wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
                        "SET_MODE_INPUT_FX_DONE: monEnabled=%d, inputStreamRunning=%d",
                        g_jniState.inputNode->isMonitoringEnabled(),
                        g_jniState.inputNode->isInputStreamRunning());
                } else {
                    wma::logMessage(wma::LogLevel::WARN, "WMA_AUDIT",
                        "SET_MODE_INPUT_FX: NO INPUT NODE! Input will be silent.");
                }
                break;
            }

            case watermelon_audio::AudioMode::MIX:
                LOGI("AudioNativeBridge.setAudioMode: configuring MIX");
                g_jniState.engine->setOscillatorEnabled(true);
                g_jniState.engine->setVocoderCarrierSource(true);
                // Ensure InputNode exists for MIX mode
                ensureInputNode();
                if (g_jniState.inputNode) {
                    auto& backendManager = watermelon_audio::BackendManager::getInstance();
                    bool isUsbActive = (backendManager.getCurrentType() == watermelon_audio::BackendType::LIBUSB);

                    if (isUsbActive) {
                        if (g_jniState.inputNode->isInputStreamRunning()) {
                            g_jniState.inputNode->stopInputStream();
                        }
                    } else {
                        if (!g_jniState.inputNode->isInputStreamRunning()) {
                            g_jniState.inputNode->startInputStream();
                        }
                    }
                    g_jniState.inputNode->setMonitoringEnabled(true);
                    g_jniState.engine->setInputNode(g_jniState.inputNode);
                }
                break;
        }

        g_jniState.currentMode.store(static_cast<int>(audioMode), std::memory_order_release);
        LOGI("AudioNativeBridge.setAudioMode: SUCCESS mode set to %d", mode);

    } catch (const std::exception& e) {
        LOGE("AudioNativeBridge.setAudioMode: exception: %s", e.what());
    }
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetAudioMode(
    JNIEnv* env, jobject thiz) {
    return static_cast<jint>(g_jniState.currentMode.load(std::memory_order_acquire));
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsInModeTransition(
    JNIEnv* env, jobject thiz) {
    return g_jniState.modeTransitionInProgress.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetModeTransitionProgress(
    JNIEnv* env, jobject thiz) {
    return g_jniState.modeTransitionProgress.load(std::memory_order_relaxed);
}

JNIEXPORT jstring JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetModeName(
    JNIEnv* env, jobject thiz, jint mode) {
    const char* name = watermelon_audio::ModeUtils::getModeName(static_cast<watermelon_audio::AudioMode>(mode));
    return env->NewStringUTF(name);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeModeRequiresInput(
    JNIEnv* env, jobject thiz, jint mode) {
    return watermelon_audio::ModeUtils::requiresInput(static_cast<watermelon_audio::AudioMode>(mode)) ? JNI_TRUE : JNI_FALSE;
}

// ==================== Input Functions ====================

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStartInputStream(
    JNIEnv* env, jobject thiz) {
    if (!ensureInputNode()) {
        return JNI_FALSE;
    }
    return g_jniState.inputNode->startInputStream() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStopInputStream(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.inputNode) {
        g_jniState.inputNode->stopInputStream();
    }
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsInputStreamRunning(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.inputNode) {
        return JNI_FALSE;
    }
    return g_jniState.inputNode->isInputStreamRunning() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetInputSource(
    JNIEnv* env, jobject thiz, jint source) {
    if (!g_jniState.inputNode) {
        return;
    }
    if (source < 0 || source > 2) {
        LOGE("AudioNativeBridge.setInputSource: invalid source %d", source);
        return;
    }
    try {
        g_jniState.inputNode->setInputSource(static_cast<InputSource>(source));
    } catch (const std::exception& e) {
        LOGE("AudioNativeBridge.setInputSource: exception: %s", e.what());
    }
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetInputSource(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.inputNode) {
        return 0;
    }
    return static_cast<jint>(g_jniState.inputNode->getInputSource());
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetInputGain(
    JNIEnv* env, jobject thiz, jfloat gainDb) {
    if (g_jniState.inputNode) {
        g_jniState.inputNode->setInputGain(gainDb);
    }
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetInputGain(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.inputNode) {
        return 0.0f;
    }
    return g_jniState.inputNode->getInputGain();
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetNoiseGateEnabled(
    JNIEnv* env, jobject thiz, jboolean enabled) {
    if (g_jniState.inputNode) {
        g_jniState.inputNode->setNoiseGateEnabled(enabled);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsNoiseGateEnabled(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.inputNode) {
        return JNI_FALSE;
    }
    return g_jniState.inputNode->isNoiseGateEnabled() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetNoiseGateThreshold(
    JNIEnv* env, jobject thiz, jfloat thresholdDb) {
    if (g_jniState.inputNode) {
        g_jniState.inputNode->setNoiseGateThreshold(thresholdDb);
    }
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetInputLevel(
    JNIEnv* env, jobject thiz, jint channel) {
    if (!g_jniState.inputNode) {
        return -100.0f;
    }
    return g_jniState.inputNode->getInputLevel(channel);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetInputLevelLinear(
    JNIEnv* env, jobject thiz, jint channel) {
    if (!g_jniState.inputNode) {
        return 0.0f;
    }
    return g_jniState.inputNode->getInputLevelLinear(channel);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsInputClipping(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.inputNode) {
        return JNI_FALSE;
    }
    return g_jniState.inputNode->isClipping() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsNoiseGateOpen(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.inputNode) {
        return JNI_FALSE;
    }
    return g_jniState.inputNode->isNoiseGateOpen() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetInputLatencyMs(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.inputNode) {
        return 0.0f;
    }
    return g_jniState.inputNode->getInputLatencyMs();
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeReleaseInputNode(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.inputNode) {
        g_jniState.inputNode->stopInputStream();
        g_jniState.inputNode.reset();
    }
}

// ==================== Monitoring Functions ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetMonitoringEnabled(
    JNIEnv* env, jobject thiz, jboolean enabled) {
    if (g_jniState.inputNode) {
        g_jniState.inputNode->setMonitoringEnabled(enabled);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsMonitoringEnabled(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.inputNode) {
        return JNI_FALSE;
    }
    return g_jniState.inputNode->isMonitoringEnabled() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetMonitoringVolume(
    JNIEnv* env, jobject thiz, jfloat volume) {
    if (g_jniState.inputNode) {
        volume = clampFloat(volume, 0.0f, 1.0f);
        g_jniState.inputNode->setMonitoringVolume(volume);
    }
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetMonitoringVolume(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.inputNode) {
        return 0.0f;
    }
    return g_jniState.inputNode->getMonitoringVolume();
}

// ==================== Dual Touch Functions ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetDualTouchMode(
    JNIEnv* env, jobject thiz, jboolean enabled) {
    if (g_jniState.engine) {
        g_jniState.engine->setDualTouchMode(enabled);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetDualTouch(
    JNIEnv* env, jobject thiz,
    jfloat x1, jfloat y1, jfloat freq1, jfloat amp1, jfloat pressure1,
    jfloat x2, jfloat y2, jfloat freq2, jfloat amp2, jfloat pressure2,
    jfloat distance, jfloat angle) {
    if (g_jniState.engine) {
        g_jniState.engine->updateDualTouch(
            x1, y1, freq1, amp1, pressure1,
            x2, y2, freq2, amp2, pressure2,
            distance, angle
        );
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetDualTouchMixMode(
    JNIEnv* env, jobject thiz, jint modeId) {
    if (g_jniState.engine && modeId >= 0 && modeId <= 5) {
        g_jniState.engine->setDualTouchMixMode(static_cast<DualTouchMixMode>(modeId));
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetSecondaryOscillatorType(
    JNIEnv* env, jobject thiz, jint typeId) {
    if (g_jniState.engine) {
        g_jniState.engine->setSecondaryOscillatorType(typeId);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetDualTouchMode(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return JNI_FALSE;
    }
    return g_jniState.engine->getDualTouchMode() ? JNI_TRUE : JNI_FALSE;
}

// ==================== Voice System Functions ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeEnableVoiceSystem(
    JNIEnv* env, jobject thiz, jboolean enable) {
    if (g_jniState.engine) {
        g_jniState.engine->enableVoiceSystem(enable);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsVoiceSystemEnabled(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return JNI_FALSE;
    }
    return g_jniState.engine->isVoiceSystemEnabled() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeUpdateMultiTouch(
    JNIEnv* env, jobject thiz, jint count, jfloatArray touchData) {
    if (!g_jniState.engine) {
        return;
    }

    if (count <= 0 || touchData == nullptr) {
        g_jniState.engine->updateMultiTouch(nullptr, 0);
        return;
    }

    ScopedFloatArrayRW data(env, touchData);
    if (!data.isValid()) {
        return;
    }

    // BUG FIX: Match Kotlin's floatsPerTouch = 6: [x, y, freq, amp, pressure, pointerId]
    const int TOUCH_STRIDE = 6;
    int maxTouches = std::min(static_cast<int>(count), 4);

    std::vector<voice::TouchData> touches(maxTouches);
    for (int i = 0; i < maxTouches; i++) {
        int offset = i * TOUCH_STRIDE;
        touches[i].x = data.get()[offset + 0];
        touches[i].y = data.get()[offset + 1];
        touches[i].frequency = data.get()[offset + 2];
        touches[i].amplitude = data.get()[offset + 3];
        touches[i].pressure = data.get()[offset + 4];
        touches[i].pointerId = static_cast<int>(data.get()[offset + 5]);  // Use actual pointer ID
        touches[i].active = true;  // Mark touch as active for voice activation
    }

    g_jniState.engine->updateMultiTouch(touches.data(), maxTouches);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetActiveVoiceCount(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return 0;
    }
    return g_jniState.engine->getActiveVoiceCount();
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetMaxVoices(
    JNIEnv* env, jobject thiz, jint maxVoices) {
    if (g_jniState.engine) {
        g_jniState.engine->setMaxVoices(maxVoices);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetVoiceStealingStrategy(
    JNIEnv* env, jobject thiz, jint strategy) {
    if (g_jniState.engine) {
        g_jniState.engine->setVoiceStealingStrategy(strategy);
    }
}

// ==================== Chord Functions (Phase 9C) ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTriggerChordNotes(
    JNIEnv* env, jobject thiz, jfloatArray frequencies, jfloat amplitude, jint oscillatorType) {
    if (!g_jniState.engine || frequencies == nullptr) return;

    jint count = env->GetArrayLength(frequencies);
    if (count <= 0) return;

    jfloat* freqs = env->GetFloatArrayElements(frequencies, nullptr);
    if (!freqs) return;

    g_jniState.engine->triggerChordNotes(freqs, count, amplitude, oscillatorType);
    env->ReleaseFloatArrayElements(frequencies, freqs, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeUpdateChordNotes(
    JNIEnv* env, jobject thiz, jfloatArray frequencies, jfloat amplitude) {
    if (!g_jniState.engine || frequencies == nullptr) return;

    jint count = env->GetArrayLength(frequencies);
    if (count <= 0) return;

    jfloat* freqs = env->GetFloatArrayElements(frequencies, nullptr);
    if (!freqs) return;

    g_jniState.engine->updateChordNotes(freqs, count, amplitude);
    env->ReleaseFloatArrayElements(frequencies, freqs, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeReleaseChordNotes(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine) {
        g_jniState.engine->releaseChordNotes();
    }
}

// ==================== Vocoder Functions ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetVocoderCarrierSource(
    JNIEnv* env, jobject thiz, jboolean useInternalCarrier) {
    if (g_jniState.engine) {
        g_jniState.engine->setVocoderCarrierSource(useInternalCarrier);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetVocoderCarrierFrequency(
    JNIEnv* env, jobject thiz, jfloat frequency) {
    if (g_jniState.engine) {
        frequency = clampFloat(frequency, 20.0f, 2000.0f);
        g_jniState.engine->setVocoderCarrierFrequency(frequency);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeHasVocoderEffect(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return JNI_FALSE;
    }
    return g_jniState.engine->hasVocoderEffect() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetVocoderModulatorSource(
    JNIEnv* env, jobject thiz, jboolean useExternalMod) {
    if (g_jniState.engine) {
        g_jniState.engine->setVocoderModulatorSource(useExternalMod);
    }
}

// ==================== USB Backend Functions ====================

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsUsbBackendAvailable(
    JNIEnv* env, jobject thiz) {
    auto& backendManager = watermelon_audio::BackendManager::getInstance();
    return backendManager.isUsbBackendAvailable() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetUseBackendManager(
    JNIEnv* env, jobject thiz, jboolean use) {
    if (g_jniState.engine) {
        g_jniState.engine->setUseBackendManager(use);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeCreateSplitBackend(
    JNIEnv* env, jobject thiz, jint inputBackendId, jint outputBackendId) {
    auto& backendManager = watermelon_audio::BackendManager::getInstance();
    auto inputType = static_cast<watermelon_audio::BackendType>(inputBackendId);
    auto outputType = static_cast<watermelon_audio::BackendType>(outputBackendId);
    return backendManager.createSplitBackend(inputType, outputType) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSelectBackend(
    JNIEnv* env, jobject thiz, jint backendId) {
    auto& backendManager = watermelon_audio::BackendManager::getInstance();
    auto backendType = static_cast<watermelon_audio::BackendType>(backendId);
    return backendManager.selectBackend(backendType) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetCurrentBackendType(
    JNIEnv* env, jobject thiz) {
    auto& backendManager = watermelon_audio::BackendManager::getInstance();
    return static_cast<jint>(backendManager.getCurrentType());
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetUsbStreamingMode(
    JNIEnv* env, jobject thiz, jint modeId) {
    // Streaming mode is controlled via full-duplex enable
    // modeId: 0=PLAYBACK_ONLY, 1=CAPTURE_ONLY, 2=FULL_DUPLEX
    auto& backendManager = watermelon_audio::BackendManager::getInstance();
    backendManager.setFullDuplexEnabled(modeId == 2);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeConfigureUsbBackend(
    JNIEnv* env, jobject thiz, jint sampleRate, jint channels, jint bitDepth) {
    // `channels` and `bitDepth` are informational only. LibusbBackend picks
    // the actual stream format via AltsettingSelector based on the parsed
    // device topology and the registered StreamPreference — the JNI layer
    // has no business overriding those.
    //
    // Historical code routed `bitDepth` into BackendManager::setBufferSize()
    // as a "buffer size hint", which was a latent time bomb: any caller
    // passing a realistic bitDepth (16/24/32) would set mRequestedBufferSize
    // to 16/24/32 frames, drop mDspOutputSamples to ~32-64, and starve the
    // output ring so badly that every iso transfer would silence-fill. Today
    // the bug is dormant only because NoisyPad's AudioEngineStateManager.
    // startWithUsbMode() — the only code path that calls configureUsbBackend
    // — is itself dead (no invocations). Remove the misrouted call before
    // someone wakes both up at once.
    (void)channels;
    (void)bitDepth;
    auto& backendManager = watermelon_audio::BackendManager::getInstance();
    backendManager.setSampleRate(sampleRate);
}

// ==================== Memory/Resource Functions ====================

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsUsingReducedBuffers(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return JNI_FALSE;
    }
    return g_jniState.engine->isUsingReducedBuffers() ? JNI_TRUE : JNI_FALSE;
}

// ==================== Automation Functions ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetAutomationParameter(
    JNIEnv* env, jobject thiz, jint effectIndex, jint paramId, jfloat xyValue) {
    if (!g_jniState.engine) {
        return;
    }

    size_t chainSize = g_jniState.engine->getNumEffects();
    if (effectIndex < 0 || static_cast<size_t>(effectIndex) >= chainSize) {
        return;
    }

    // XY value is already normalized 0-1, delegate to setParameter
    g_jniState.engine->setParameter(static_cast<size_t>(effectIndex), paramId, xyValue);
}

// ==================== XY Mapping Config Functions (Phase 4) ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetMappingConfig(
    JNIEnv* env, jobject thiz,
    jint axis, jint effectIndex, jint paramId,
    jint curve, jint polarity,
    jfloat mapMin, jfloat mapMax, jboolean inverted) {
    if (!g_jniState.engine) return;
    if (axis < 0 || axis > 2) return;
    if (curve < 0 || curve > 3) return;
    if (polarity < 0 || polarity > 1) return;
    if (!std::isfinite(mapMin) || !std::isfinite(mapMax)) return;

    g_jniState.engine->setMappingConfig(
        axis, effectIndex, paramId,
        curve, polarity,
        mapMin, mapMax, static_cast<bool>(inverted));
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeClearMappingConfig(
    JNIEnv* env, jobject thiz, jint axis) {
    if (!g_jniState.engine) return;
    if (axis < 0 || axis > 2) return;

    g_jniState.engine->clearMappingConfig(axis);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetDepthValue(
    JNIEnv* env, jobject thiz, jfloat value) {
    if (!g_jniState.engine) return;

    g_jniState.engine->setDepthValue(std::clamp(value, 0.0f, 1.0f));
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeApplyAutomation(
    JNIEnv* env, jobject thiz, jint axis, jfloat normalizedValue) {
    if (!g_jniState.engine) return;
    if (axis < 0 || axis > 2) return;

    g_jniState.engine->applyAutomation(axis, std::clamp(normalizedValue, 0.0f, 1.0f));
}

// ==================== USB Device Functions ====================

// USB device state structure (defined in jni_usb.cpp)
struct UsbDeviceState {
    int fileDescriptor = -1;
    std::string usbfsPath;
    bool isInitialized = false;
    bool isStreaming = false;
};
extern UsbDeviceState gUsbDeviceState;

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeInitializeUsbDevice(
    JNIEnv* env, jobject thiz, jint fileDescriptor, jstring usbfsPath) {
    if (fileDescriptor < 0) {
        LOGE("AudioNativeBridge.initializeUsbDevice: invalid file descriptor: %d", fileDescriptor);
        return JNI_FALSE;
    }

    ScopedUtfChars pathChars(env, usbfsPath);
    if (!pathChars.isValid()) {
        LOGE("AudioNativeBridge.initializeUsbDevice: failed to get usbfsPath string");
        return JNI_FALSE;
    }

    LOGI("AudioNativeBridge.initializeUsbDevice: fd=%d, path=%s", fileDescriptor, pathChars.c_str());

    gUsbDeviceState.fileDescriptor = fileDescriptor;
    gUsbDeviceState.usbfsPath = std::string(pathChars.c_str());

    auto& manager = watermelon_audio::BackendManager::getInstance();
    bool success = manager.initializeUsbBackend(fileDescriptor, pathChars.c_str());

    if (success) {
        gUsbDeviceState.isInitialized = true;
        LOGI("AudioNativeBridge.initializeUsbDevice: LibusbBackend initialized successfully");
    } else {
        gUsbDeviceState.isInitialized = false;
        LOGE("AudioNativeBridge.initializeUsbDevice: Failed to initialize LibusbBackend");
    }

    return static_cast<jboolean>(success);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeCloseUsbDevice(
    JNIEnv* env, jobject thiz) {
    LOGI("AudioNativeBridge.closeUsbDevice");

    if (gUsbDeviceState.isStreaming) {
        auto& manager = watermelon_audio::BackendManager::getInstance();
        auto* backend = manager.getLibusbBackend();
        if (backend) {
            backend->stop();
        }
        gUsbDeviceState.isStreaming = false;
    }

    auto& manager = watermelon_audio::BackendManager::getInstance();
    manager.fallbackToOboe();

    gUsbDeviceState.fileDescriptor = -1;
    gUsbDeviceState.usbfsPath.clear();
    gUsbDeviceState.isInitialized = false;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsUsbDeviceInitialized(
    JNIEnv* env, jobject thiz) {
    return static_cast<jboolean>(gUsbDeviceState.isInitialized);
}

JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeParseUsbDescriptors(
    JNIEnv* env, jobject thiz) {
    if (!gUsbDeviceState.isInitialized) {
        return nullptr;
    }

    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();

    jfloat caps[7] = {1.0f, 0.0f, 48000.0f, 3.0f, 2.0f, 0.0f, 2.0f};

    if (backend) {
        auto backendCaps = backend->getCapabilities();
        if (!backendCaps.supportedSampleRates.empty()) {
            caps[2] = static_cast<float>(backendCaps.supportedSampleRates.back());
        }
        caps[3] = 0.0f;
        for (int depth : backendCaps.supportedBitDepths) {
            if (depth == 16) caps[3] += 1.0f;
            if (depth == 24) caps[3] += 2.0f;
            if (depth == 32) caps[3] += 4.0f;
        }
        caps[4] = static_cast<float>(backendCaps.maxChannelsOutput);
        caps[5] = static_cast<float>(backendCaps.maxChannelsInput);

        auto* usbDevice = backend->getUsbAudioDevice();
        if (usbDevice) {
            caps[0] = static_cast<float>(usbDevice->playbackInterfaces.size());
            caps[1] = static_cast<float>(usbDevice->captureInterfaces.size());
        }
    }

    jfloatArray result = env->NewFloatArray(7);
    if (result) {
        env->SetFloatArrayRegion(result, 0, 7, caps);
    }
    return result;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStartUsbStreaming(
    JNIEnv* env, jobject thiz, jint sampleRate, jint channels, jint bitDepth) {
    if (!gUsbDeviceState.isInitialized) {
        LOGE("AudioNativeBridge.startUsbStreaming: device not initialized");
        return JNI_FALSE;
    }

    if (gUsbDeviceState.isStreaming) {
        return JNI_TRUE;
    }

    LOGI("AudioNativeBridge.startUsbStreaming: sampleRate=%d, channels=%d, bitDepth=%d", sampleRate, channels, bitDepth);

    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();

    if (!backend) {
        LOGE("AudioNativeBridge.startUsbStreaming: LibusbBackend not available");
        return JNI_FALSE;
    }

    backend->setSampleRate(sampleRate);
    backend->setBufferSize(256);

    auto result = backend->start();
    if (result != watermelon_audio::BackendResult::OK) {
        LOGE("AudioNativeBridge.startUsbStreaming: failed to start LibusbBackend");
        return JNI_FALSE;
    }

    gUsbDeviceState.isStreaming = true;
    LOGI("AudioNativeBridge.startUsbStreaming: USB streaming started successfully");
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStopUsbStreaming(
    JNIEnv* env, jobject thiz) {
    LOGI("AudioNativeBridge.stopUsbStreaming");

    if (!gUsbDeviceState.isStreaming) {
        return;
    }

    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();

    if (backend) {
        backend->stop();
    }

    gUsbDeviceState.isStreaming = false;
}

JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetUsbTransferStats(
    JNIEnv* env, jobject thiz) {
    if (!gUsbDeviceState.isInitialized) {
        return nullptr;
    }

    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();

    constexpr int STATS_SIZE = 19;
    jfloat statsArray[STATS_SIZE] = {0};

    if (backend) {
        auto* stats = backend->getTransferStats();
        if (stats) {
            statsArray[0] = static_cast<float>(stats->packetsSubmitted.load());
            statsArray[1] = static_cast<float>(stats->packetsCompleted.load());
            statsArray[2] = static_cast<float>(stats->packetsErrors.load());
            statsArray[3] = static_cast<float>(stats->underruns.load());
            statsArray[4] = static_cast<float>(stats->overruns.load());
            statsArray[5] = stats->currentLatencyMs.load();
            statsArray[6] = stats->avgLatencyMs.load();
            statsArray[7] = stats->currentLatencyMs.load() * 0.8f;
            statsArray[8] = stats->currentLatencyMs.load() * 1.5f;
            statsArray[9] = static_cast<float>(stats->ringBufferLevel.load());
            statsArray[10] = stats->ringBufferFillPct.load();
            statsArray[11] = 3840.0f;
            statsArray[12] = statsArray[1] * 192.0f;
            statsArray[13] = stats->currentSampleRateHz.load();
            statsArray[14] = stats->driftPpm.load();
            statsArray[15] = stats->feedbackEffectiveFramesPerPacket.load();
            statsArray[16] = static_cast<float>(stats->feedbackPacketsReceived.load());
            statsArray[17] = static_cast<float>(stats->feedbackPacketsInvalid.load());
            statsArray[18] = static_cast<float>(stats->activeClockSourceId.load());
        }
    }

    jfloatArray result = env->NewFloatArray(STATS_SIZE);
    if (result) {
        env->SetFloatArrayRegion(result, 0, STATS_SIZE, statsArray);
    }
    return result;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStartUsbStreamingWithMode(
    JNIEnv* env, jobject thiz, jint sampleRate, jint channels, jint bitDepth, jint streamingMode) {
    if (!gUsbDeviceState.isInitialized || gUsbDeviceState.isStreaming) {
        return gUsbDeviceState.isStreaming ? JNI_TRUE : JNI_FALSE;
    }

    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();

    if (!backend) {
        return JNI_FALSE;
    }

    watermelon_audio::UsbStreamingMode mode;
    switch (streamingMode) {
        case 0: mode = watermelon_audio::UsbStreamingMode::PLAYBACK_ONLY; break;
        case 1: mode = watermelon_audio::UsbStreamingMode::CAPTURE_ONLY; break;
        case 2: mode = watermelon_audio::UsbStreamingMode::FULL_DUPLEX; break;
        default: return JNI_FALSE;
    }

    backend->setStreamingMode(mode);
    backend->setSampleRate(sampleRate);

    auto result = backend->start();
    if (result != watermelon_audio::BackendResult::OK) {
        return JNI_FALSE;
    }

    gUsbDeviceState.isStreaming = true;
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeUsbDeviceSupportsFullDuplex(
    JNIEnv* env, jobject thiz) {
    if (!gUsbDeviceState.isInitialized) return JNI_FALSE;
    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();
    return (backend && backend->supportsFullDuplex()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeUsbDeviceHasCapture(
    JNIEnv* env, jobject thiz) {
    if (!gUsbDeviceState.isInitialized) return JNI_FALSE;
    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();
    return (backend && backend->hasCapture()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetUsbDeviceUacVersion(
    JNIEnv* env, jobject thiz) {
    if (!gUsbDeviceState.isInitialized) return 0;
    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();
    return backend ? static_cast<jint>(backend->getUacVersion()) : 0;
}

JNIEXPORT jbyteArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetUsbCapabilitySnapshot(
    JNIEnv* env, jobject thiz) {
    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();
    if (!backend) {
        LOGW("nativeGetUsbCapabilitySnapshot: no LibusbBackend in BackendManager");
        return nullptr;
    }
    if (!backend->isUsbDeviceReady()) {
        LOGW("nativeGetUsbCapabilitySnapshot: backend not ready");
        return nullptr;
    }

    const auto* device = backend->getUsbAudioDevice();
    if (!device) {
        LOGW("nativeGetUsbCapabilitySnapshot: backend has no parsed device");
        return nullptr;
    }

    auto encoded = watermelon_audio::usb::encodeSnapshot(*device);
    LOGI("nativeGetUsbCapabilitySnapshot: encoded %zu bytes (UAC%d, %zu pb / %zu cap)",
         encoded.size(), device->uacVersion,
         device->playbackInterfaces.size(), device->captureInterfaces.size());
    jbyteArray result = env->NewByteArray(static_cast<jsize>(encoded.size()));
    if (result) {
        env->SetByteArrayRegion(result, 0, static_cast<jsize>(encoded.size()),
                                 reinterpret_cast<const jbyte*>(encoded.data()));
    }
    return result;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetUsbStreamPreference(
    JNIEnv* env, jobject thiz, jint preferredSampleRate, jint minChannels,
    jboolean requireFeedback, jint profile) {
    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();
    if (!backend) {
        LOGW("nativeSetUsbStreamPreference: no LibusbBackend");
        return JNI_FALSE;
    }

    watermelon_audio::usb::StreamPreference pref;
    switch (profile) {
        case 1:
            pref = watermelon_audio::usb::StreamPreference::lowestLatency();
            break;
        case 2:
            pref = watermelon_audio::usb::StreamPreference::highestFidelity();
            break;
        default:
            pref = watermelon_audio::usb::StreamPreference::defaultPro();
            break;
    }
    pref.requiredSampleRate = static_cast<int>(preferredSampleRate);
    pref.minChannels = std::max(1, static_cast<int>(minChannels));
    pref.requireFeedback = (requireFeedback == JNI_TRUE);
    pref.skipRateCheck = false;

    backend->setStreamPreference(pref);
    LOGI("nativeSetUsbStreamPreference: rate=%d minCh=%d requireFeedback=%d profile=%d",
         pref.requiredSampleRate, pref.minChannels,
         pref.requireFeedback ? 1 : 0, profile);
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSelectUsbAltsetting(
    JNIEnv* env, jobject thiz, jint interfaceNumber, jint alternateSetting,
    jint formatIndex) {
    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();
    if (!backend) {
        LOGW("nativeSelectUsbAltsetting: no LibusbBackend");
        return JNI_FALSE;
    }
    const bool ok = backend->selectAltsetting(
        static_cast<int>(interfaceNumber),
        static_cast<int>(alternateSetting),
        static_cast<int>(formatIndex));
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSelectUsbClockSource(
    JNIEnv* env, jobject thiz, jint clockSourceId) {
    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();
    if (!backend) {
        LOGW("nativeSelectUsbClockSource: no LibusbBackend");
        return JNI_FALSE;
    }
    const bool ok = backend->selectClockSource(static_cast<int>(clockSourceId));
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsUsbDeviceDisconnected(
    JNIEnv* env, jobject thiz) {
    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();
    if (!backend) return JNI_TRUE;
    return !backend->isUsbDeviceReady() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jintArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetUsbHealthStatus(
    JNIEnv* env, jobject thiz) {
    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();
    if (!backend) return nullptr;

    const auto* stats = backend->getTransferStats();
    if (!stats) return nullptr;

    jintArray result = env->NewIntArray(3);
    if (result) {
        jint values[3] = {
            backend->isUsbDeviceReady() ? 0 : 1,
            static_cast<jint>(stats->packetsErrors.load()),
            0
        };
        env->SetIntArrayRegion(result, 0, 3, values);
    }
    return result;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeFallbackToOboeBackend(
    JNIEnv* env, jobject thiz) {
    LOGI("AudioNativeBridge.fallbackToOboeBackend: triggered from Kotlin");
    auto& manager = watermelon_audio::BackendManager::getInstance();
    manager.fallbackToOboe();
}

JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetAdaptiveBufferStats(
    JNIEnv* env, jobject thiz) {
    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();
    if (!backend) return nullptr;

    constexpr int SIZE = 10;
    jfloat stats[SIZE] = {0};

    jfloatArray result = env->NewFloatArray(SIZE);
    if (result) {
        env->SetFloatArrayRegion(result, 0, SIZE, stats);
    }
    return result;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetCurrentUsbBufferMs(
    JNIEnv* env, jobject thiz) {
    return 5; // Default ~5ms
}

// ==================== Output Level Metering Functions (Phase 1 - Gain Staging) ====================

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetMasterVolume(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) {
        return 1.0f;
    }
    return g_jniState.engine->getMasterVolume();
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetOutputPeakLevel(
    JNIEnv* env, jobject thiz, jint channel) {
    if (!g_jniState.engine) {
        return 0.0f;
    }
    return g_jniState.engine->getOutputPeakLevel(channel);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetOutputRmsLevel(
    JNIEnv* env, jobject thiz, jint channel) {
    if (!g_jniState.engine) {
        return 0.0f;
    }
    return g_jniState.engine->getOutputRMSLevel(channel);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetOutputPeakLevelDb(
    JNIEnv* env, jobject thiz, jint channel) {
    if (!g_jniState.engine) {
        return -100.0f;
    }
    float linear = g_jniState.engine->getOutputPeakLevel(channel);
    if (linear <= 0.0f) {
        return -100.0f;
    }
    return 20.0f * std::log10(linear);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetOutputRmsLevelDb(
    JNIEnv* env, jobject thiz, jint channel) {
    if (!g_jniState.engine) {
        return -100.0f;
    }
    float linear = g_jniState.engine->getOutputRMSLevel(channel);
    if (linear <= 0.0f) {
        return -100.0f;
    }
    return 20.0f * std::log10(linear);
}

JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetOutputLevels(
    JNIEnv* env, jobject thiz) {
    // Returns [peakL, peakR, rmsL, rmsR] for efficient single-call metering
    jfloatArray result = env->NewFloatArray(4);
    if (!result) {
        return nullptr;
    }

    jfloat levels[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (g_jniState.engine) {
        levels[0] = g_jniState.engine->getOutputPeakLevel(0);
        levels[1] = g_jniState.engine->getOutputPeakLevel(1);
        levels[2] = g_jniState.engine->getOutputRMSLevel(0);
        levels[3] = g_jniState.engine->getOutputRMSLevel(1);
    }

    env->SetFloatArrayRegion(result, 0, 4, levels);
    return result;
}

// ==================== USB Volume Functions (Phase 1 - Gain Staging) ====================

// USB Volume state - defined in jni_usb.cpp
extern std::atomic<float> g_usbOutputVolume;
extern std::atomic<float> g_usbInputVolume;
extern std::atomic<bool> g_usbOutputMuted;
extern std::atomic<bool> g_usbInputMuted;

JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetUsbVolumeCapabilities(
    JNIEnv* env, jobject thiz) {
    // Capabilities: [hasOutputVol, hasInputVol, minDb, maxDb, stepDb, ...reserved]
    constexpr int SIZE = 10;
    jfloat caps[SIZE] = {
        1.0f,    // hasOutputVolume
        1.0f,    // hasInputVolume
        -60.0f,  // minDb
        0.0f,    // maxDb
        1.0f,    // stepDb
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f  // reserved
    };

    jfloatArray result = env->NewFloatArray(SIZE);
    if (result) {
        env->SetFloatArrayRegion(result, 0, SIZE, caps);
    }
    return result;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetUsbOutputVolume(
    JNIEnv* env, jobject thiz, jfloat volume) {
    float clampedVolume = clampFloat(volume, 0.0f, 1.0f);
    LOGD("AudioNativeBridge.setUsbOutputVolume: %.2f", clampedVolume);
    g_usbOutputVolume.store(clampedVolume, std::memory_order_relaxed);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetUsbOutputVolume(
    JNIEnv* env, jobject thiz) {
    return getUsbOutputVolumeInternal();
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetUsbInputVolume(
    JNIEnv* env, jobject thiz, jfloat volume) {
    float clampedVolume = clampFloat(volume, 0.0f, 1.0f);
    LOGD("AudioNativeBridge.setUsbInputVolume: %.2f", clampedVolume);
    g_usbInputVolume.store(clampedVolume, std::memory_order_relaxed);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetUsbInputVolume(
    JNIEnv* env, jobject thiz) {
    return getUsbInputVolumeInternal();
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetUsbOutputMute(
    JNIEnv* env, jobject thiz, jboolean muted) {
    LOGD("AudioNativeBridge.setUsbOutputMute: %s", muted ? "true" : "false");
    g_usbOutputMuted.store(muted == JNI_TRUE, std::memory_order_relaxed);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsUsbOutputMuted(
    JNIEnv* env, jobject thiz) {
    return g_usbOutputMuted.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetUsbInputMute(
    JNIEnv* env, jobject thiz, jboolean muted) {
    LOGD("AudioNativeBridge.setUsbInputMute: %s", muted ? "true" : "false");
    g_usbInputMuted.store(muted == JNI_TRUE, std::memory_order_relaxed);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsUsbInputMuted(
    JNIEnv* env, jobject thiz) {
    return g_usbInputMuted.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
}

// ==================== Arpeggiator (Phase 7) ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpEnabled(
    JNIEnv* env, jobject thiz, jboolean enabled) {
    if (g_jniState.engine) {
        g_jniState.engine->getArpSequencer().setEnabled(enabled == JNI_TRUE);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsArpEnabled(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine) {
        return g_jniState.engine->getArpSequencer().isEnabled() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpPattern(
    JNIEnv* env, jobject thiz, jint patternId) {
    if (g_jniState.engine) {
        g_jniState.engine->getArpSequencer().setPattern(patternId);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpSubdivision(
    JNIEnv* env, jobject thiz, jfloat beatsPerStep) {
    if (g_jniState.engine) {
        g_jniState.engine->getArpSequencer().setSubdivision(beatsPerStep);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpOctaveRange(
    JNIEnv* env, jobject thiz, jint octaves) {
    if (g_jniState.engine) {
        g_jniState.engine->getArpSequencer().setOctaveRange(octaves);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpGateLength(
    JNIEnv* env, jobject thiz, jfloat gate) {
    if (g_jniState.engine) {
        g_jniState.engine->getArpSequencer().setGateLength(gate);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpSwing(
    JNIEnv* env, jobject thiz, jfloat swing) {
    if (g_jniState.engine) {
        g_jniState.engine->getArpSequencer().setSwing(swing);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpLatch(
    JNIEnv* env, jobject thiz, jboolean latch) {
    if (g_jniState.engine) {
        g_jniState.engine->getArpSequencer().setLatch(latch == JNI_TRUE);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpVelocity(
    JNIEnv* env, jobject thiz, jfloat velocity) {
    if (g_jniState.engine) {
        g_jniState.engine->getArpSequencer().setVelocity(velocity);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpVelocityVariation(
    JNIEnv* env, jobject thiz, jfloat variation) {
    if (g_jniState.engine) {
        g_jniState.engine->getArpSequencer().setVelocityVariation(variation);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpProbability(
    JNIEnv* env, jobject thiz, jfloat probability) {
    if (g_jniState.engine) {
        g_jniState.engine->getArpSequencer().setProbability(probability);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpScaleIntervals(
    JNIEnv* env, jobject thiz, jintArray intervals) {
    if (g_jniState.engine && intervals) {
        jint* data = env->GetIntArrayElements(intervals, nullptr);
        int count = env->GetArrayLength(intervals);
        g_jniState.engine->getArpSequencer().setScaleIntervals(data, count);
        env->ReleaseIntArrayElements(intervals, data, JNI_ABORT);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpTouchActive(
    JNIEnv* env, jobject thiz, jboolean active) {
    if (g_jniState.engine) {
        g_jniState.engine->getArpSequencer().setTouchActive(active == JNI_TRUE);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpBaseFrequency(
    JNIEnv* env, jobject thiz, jfloat frequency) {
    if (g_jniState.engine) {
        g_jniState.engine->getArpSequencer().setBaseFrequency(frequency);
    }
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetArpCurrentStep(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine) {
        return g_jniState.engine->getArpSequencer().getCurrentStep();
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetArpTotalSteps(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine) {
        return g_jniState.engine->getArpSequencer().getTotalSteps();
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpRatchet(
    JNIEnv* env, jobject thiz, jboolean active) {
    if (g_jniState.engine) {
        g_jniState.engine->getArpSequencer().setRatchet(active == JNI_TRUE);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeRegenerateArpPattern(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine) {
        g_jniState.engine->getArpSequencer().regeneratePattern();
    }
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsArpGateOpen(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine) {
        return g_jniState.engine->getArpSequencer().isGateOpen() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

// ========== AUDIO LOOPER (Phase 11) ==========

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperPrepareTrack(
    JNIEnv* env, jobject thiz, jint trackIndex, jint lengthFrames, jint sampleRate) {
    if (!g_jniState.engine) return JniError::ENGINE_NOT_INITIALIZED;
    bool ok = g_jniState.engine->getAudioLooper().prepareTrack(trackIndex, lengthFrames, sampleRate);
    return ok ? JniError::SUCCESS : JniError::MEMORY_ALLOCATION_FAILED;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperStartRecording(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().startRecording(trackIndex);
        // Looper state is polled independently at 30fps via LooperViewModel
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperStopRecording(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().stopRecording();
        // Looper state is polled independently at 30fps via LooperViewModel
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperAbortRecording(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().abortRecording();
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperStartOverdub(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().startOverdub(trackIndex);
        // Looper state is polled independently at 30fps via LooperViewModel
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperStopAll(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().stopAll();
        // Looper state is polled independently at 30fps via LooperViewModel
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperPause(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().pause();
        // Looper state is polled independently at 30fps via LooperViewModel
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperResume(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().resume();
        // Looper state is polled independently at 30fps via LooperViewModel
    }
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetRecordProgress(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 0.0f;
    return g_jniState.engine->getAudioLooper().getRecordProgress();
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetFreeLength(
    JNIEnv* env, jobject thiz, jboolean freeLength) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().setFreeLength(freeLength == JNI_TRUE);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetTrackMuted(
    JNIEnv* env, jobject thiz, jint trackIndex, jboolean muted) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().setTrackMuted(trackIndex, muted == JNI_TRUE);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetTrackPan(
    JNIEnv* env, jobject thiz, jint trackIndex, jfloat pan) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().setTrackPan(trackIndex, pan);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetTrackVolume(
    JNIEnv* env, jobject thiz, jint trackIndex, jfloat volume) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().setTrackVolume(trackIndex, volume);
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperClearTrack(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().clearTrack(trackIndex);
        // Looper state is polled independently at 30fps via LooperViewModel
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperClearAll(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().clearAll();
        // Looper state is polled independently at 30fps via LooperViewModel
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetEnabled(
    JNIEnv* env, jobject thiz, jboolean enabled) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().setEnabled(enabled == JNI_TRUE);
        // Looper state is polled independently at 30fps via LooperViewModel
    }
}

// Lock-free queries (metering)

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetProgress(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 0.0f;
    return g_jniState.engine->getAudioLooper().getProgress();
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetTrackPeakLevel(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (!g_jniState.engine) return 0.0f;
    return g_jniState.engine->getAudioLooper().getTrackPeakLevel(trackIndex);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperIsTrackActive(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (!g_jniState.engine) return JNI_FALSE;
    return g_jniState.engine->getAudioLooper().isTrackActive(trackIndex) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperIsPlaying(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return JNI_FALSE;
    return g_jniState.engine->getAudioLooper().isPlaying() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperIsRecording(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return JNI_FALSE;
    return g_jniState.engine->getAudioLooper().isRecording() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetMasterLoopFrames(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 0;
    return g_jniState.engine->getAudioLooper().getMasterLoopFrames();
}

// Per-track playback control
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperPauseTrack(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().pauseTrack(trackIndex);
        // Looper state is polled independently at 30fps via LooperViewModel
    }
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperResumeTrack(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().resumeTrack(trackIndex);
        // Looper state is polled independently at 30fps via LooperViewModel
    }
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperIsTrackPlaying(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (!g_jniState.engine) return JNI_FALSE;
    return g_jniState.engine->getAudioLooper().isTrackPlaying(trackIndex) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetTrackProgress(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (!g_jniState.engine) return 0.0f;
    return g_jniState.engine->getAudioLooper().getTrackProgress(trackIndex);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetTrackLengthFrames(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (!g_jniState.engine) return 0;
    return g_jniState.engine->getAudioLooper().getTrackLengthFrames(trackIndex);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperResetTrackPlayHead(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (g_jniState.engine) {
        g_jniState.engine->getAudioLooper().resetTrackPlayHead(trackIndex);
        // Looper state is polled independently at 30fps via LooperViewModel
    }
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSaveUndoSnapshot(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (!g_jniState.engine) return JNI_FALSE;
    return g_jniState.engine->getAudioLooper().saveUndoSnapshot(trackIndex) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperRestoreUndo(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (!g_jniState.engine) return JNI_FALSE;
    return g_jniState.engine->getAudioLooper().restoreUndo(trackIndex) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperHasUndo(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (!g_jniState.engine) return JNI_FALSE;
    return g_jniState.engine->getAudioLooper().hasUndo(trackIndex) ? JNI_TRUE : JNI_FALSE;
}

// Track waveform summary
JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetTrackWaveform(
    JNIEnv* env, jobject thiz, jint trackIndex, jfloatArray outBins, jint numBins) {
    if (!g_jniState.engine) return 0;
    jfloat* bins = env->GetFloatArrayElements(outBins, nullptr);
    int written = g_jniState.engine->getAudioLooper().getTrackWaveform(trackIndex, bins, numBins);
    env->ReleaseFloatArrayElements(outBins, bins, 0);
    return written;
}

// Track speed control
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetTrackSpeed(
    JNIEnv* env, jobject thiz, jint trackIndex, jfloat speed) {
    if (g_jniState.engine)
        g_jniState.engine->getAudioLooper().setTrackSpeed(trackIndex, speed);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetTrackSpeed(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (!g_jniState.engine) return 1.0f;
    return g_jniState.engine->getAudioLooper().getTrackSpeed(trackIndex);
}

// Master volume (lock-free)
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetMasterVolume(
    JNIEnv* env, jobject thiz, jfloat volume) {
    if (g_jniState.engine)
        g_jniState.engine->getAudioLooper().setMasterVolume(volume);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetMasterVolume(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 1.0f;
    return g_jniState.engine->getAudioLooper().getMasterVolume();
}

// Loop Region (lock-free)
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetTrackLoopRegion(
    JNIEnv* env, jobject thiz, jint trackIndex, jint startFrame, jint endFrame) {
    if (g_jniState.engine)
        g_jniState.engine->getAudioLooper().setTrackLoopRegion(trackIndex, startFrame, endFrame);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperResetTrackLoopRegion(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (g_jniState.engine)
        g_jniState.engine->getAudioLooper().resetTrackLoopRegion(trackIndex);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetTrackLoopStart(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (!g_jniState.engine) return 0;
    return g_jniState.engine->getAudioLooper().getTrackLoopStart(trackIndex);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetTrackLoopEnd(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (!g_jniState.engine) return 0;
    return g_jniState.engine->getAudioLooper().getTrackLoopEnd(trackIndex);
}

// Metronome click (lock-free trigger)
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperTriggerClick(
    JNIEnv* env, jobject thiz, jboolean isDownbeat) {
    if (g_jniState.engine)
        g_jniState.engine->getAudioLooper().triggerClick(isDownbeat == JNI_TRUE);
}

// Input metering (lock-free) — peak level of the input stream, useful as a
// pre-record signal indicator. Returns 0 if no input source is active.
// Returns max of L/R channels in linear [0..1] range.
JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetInputPeak(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.inputNode) return 0.0f;
    float l = g_jniState.inputNode->getInputLevelLinear(0);
    float r = g_jniState.inputNode->getInputLevelLinear(1);
    return l > r ? l : r;
}

// Pre-roll: start recording with `preRollMs` of prior post-FX audio seeded
// at the start of the track. Eliminates the human-reaction gap when arming.
// preRollMs is clamped to [0, 1000].
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperStartRecordingWithPreRoll(
    JNIEnv* env, jobject thiz, jint trackIndex, jint preRollMs) {
    if (!g_jniState.engine) return;
    if (preRollMs < 0) preRollMs = 0;
    if (preRollMs > 1000) preRollMs = 1000;

    auto& engine = *g_jniState.engine;
    auto& looper = engine.getAudioLooper();

    if (preRollMs == 0) {
        looper.startRecording(trackIndex);
        return;
    }

    const int sr = looper.getSampleRate();
    const int preRollFrames = (preRollMs * sr) / 1000;
    if (preRollFrames <= 0) {
        looper.startRecording(trackIndex);
        return;
    }

    // UI thread allocation — acceptable (not the audio thread).
    std::vector<float> preRoll(static_cast<size_t>(preRollFrames) * 2, 0.0f);
    engine.getPreRollRing().snapshot(preRoll.data(), preRollFrames);
    looper.startRecordingWithPreRoll(trackIndex, preRoll.data(), preRollFrames);
}

// Tail capture configuration (preserves sustain at loop seam).
// Affects tracks prepared AFTER this call. Default 250 ms.
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetTailMs(
    JNIEnv* env, jobject thiz, jint ms) {
    if (g_jniState.engine)
        g_jniState.engine->getAudioLooper().setTailMs(ms);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetTailMs(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 0;
    return g_jniState.engine->getAudioLooper().getTailMs();
}

// Armed recording: schedule recording to start at the next bar boundary.
// Returns the absolute trigger frame (>=0), or -1 on failure.
JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperArmAtNextBar(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    if (!g_jniState.engine) return -1;
    auto& transport = g_jniState.engine->getTransport();
    int64_t now = transport.getPlayFrame();
    int64_t triggerFrame = transport.nextBarBoundary(now);
    g_jniState.engine->getAudioLooper().armRecording(trackIndex, triggerFrame);
    return triggerFrame;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperCancelArm(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine)
        g_jniState.engine->getAudioLooper().cancelArm();
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetArmedTrack(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return -1;
    return g_jniState.engine->getAudioLooper().getArmedTrack();
}

// Loop quantization: prepare a track sized to N bars at current BPM/SR.
// Returns the loop length in frames (>=0) on success, or -1 on failure.
JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperPrepareTrackBars(
    JNIEnv* env, jobject thiz, jint trackIndex, jint bars, jint sampleRate) {
    if (!g_jniState.engine) return -1;
    auto& transport = g_jniState.engine->getTransport();
    int framesPerBar1 = transport.framesPerBar(1);
    if (framesPerBar1 <= 0) return -1;
    bool ok = g_jniState.engine->getAudioLooper()
                  .prepareTrackBars(trackIndex, bars, framesPerBar1, sampleRate);
    return ok ? bars * framesPerBar1 : -1;
}

// ========== TRANSPORT (BPM, beats, RT-safe metronome scheduler) ==========

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportSetBeatsPerBar(
    JNIEnv* env, jobject thiz, jint beatsPerBar) {
    if (g_jniState.engine)
        g_jniState.engine->getTransport().setBeatsPerBar(beatsPerBar);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportGetBeatsPerBar(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 4;
    return g_jniState.engine->getTransport().getBeatsPerBar();
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportFramesPerBeat(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 0;
    return g_jniState.engine->getTransport().framesPerBeat();
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportFramesPerBar(
    JNIEnv* env, jobject thiz, jint bars) {
    if (!g_jniState.engine) return 0;
    return g_jniState.engine->getTransport().framesPerBar(bars);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportStartMetronome(
    JNIEnv* env, jobject thiz, jint beats, jboolean firstIsDownbeat,
    jboolean everyBeatPattern) {
    if (g_jniState.engine)
        g_jniState.engine->getTransport().startMetronome(
            beats, firstIsDownbeat == JNI_TRUE, everyBeatPattern == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportStartMetronomeContinuous(
    JNIEnv* env, jobject thiz, jboolean everyBeatPattern) {
    if (g_jniState.engine)
        g_jniState.engine->getTransport().startMetronomeContinuous(everyBeatPattern == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportIsMetronomeContinuous(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return JNI_FALSE;
    return g_jniState.engine->getTransport().isMetronomeContinuous() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportStopMetronome(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine)
        g_jniState.engine->getTransport().stopMetronome();
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportIsMetronomeRunning(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return JNI_FALSE;
    return g_jniState.engine->getTransport().isMetronomeRunning() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportGetRemainingBeats(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 0;
    return g_jniState.engine->getTransport().getRemainingBeats();
}

// Export / Import (NOT RT-safe — call from IO thread)
JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperExportMix(
    JNIEnv* env, jobject thiz, jstring filePath) {
    if (!g_jniState.engine) return JNI_FALSE;
    const char* path = env->GetStringUTFChars(filePath, nullptr);
    bool ok = g_jniState.engine->getAudioLooper().exportMix(path);
    env->ReleaseStringUTFChars(filePath, path);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperExportTrack(
    JNIEnv* env, jobject thiz, jint trackIndex, jstring filePath) {
    if (!g_jniState.engine) return JNI_FALSE;
    const char* path = env->GetStringUTFChars(filePath, nullptr);
    bool ok = g_jniState.engine->getAudioLooper().exportTrack(trackIndex, path);
    env->ReleaseStringUTFChars(filePath, path);
    return ok ? JNI_TRUE : JNI_FALSE;
}

// Session capture: write the FULL track buffer (ignoring loop region) at the
// given bit depth (16/24 PCM, 32 = float). 32 = lossless round-trip.
JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperCaptureTrack(
    JNIEnv* env, jobject thiz, jint trackIndex, jstring filePath, jint bitDepth) {
    if (!g_jniState.engine) return JNI_FALSE;
    wav::BitDepth depth;
    switch (bitDepth) {
        case 24: depth = wav::BitDepth::PCM_24; break;
        case 32: depth = wav::BitDepth::FLOAT_32; break;
        default: depth = wav::BitDepth::PCM_16; break;
    }
    const char* path = env->GetStringUTFChars(filePath, nullptr);
    bool ok = g_jniState.engine->getAudioLooper().captureTrack(trackIndex, path, depth);
    env->ReleaseStringUTFChars(filePath, path);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperImportTrack(
    JNIEnv* env, jobject thiz, jint trackIndex, jstring filePath, jint sampleRate) {
    if (!g_jniState.engine) return JNI_FALSE;
    const char* path = env->GetStringUTFChars(filePath, nullptr);
    bool ok = g_jniState.engine->getAudioLooper().importTrack(trackIndex, path, sampleRate);
    env->ReleaseStringUTFChars(filePath, path);
    return ok ? JNI_TRUE : JNI_FALSE;
}

// Export with options. bitDepth: 16, 24, 32 (32 = float).
// repeatLoops: number of iterations (>=1). countInBeats: leading silence beats.
// applyLimiter: 1 to apply true-peak limiter, 0 for raw.
// projectName / artist / comment: optional metadata strings (null/"" to skip).
// bpm: embedded in comment if > 0; 0 = use Transport BPM if non-zero.
JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperExportMixV2(
    JNIEnv* env, jobject thiz,
    jstring filePath, jint bitDepth, jint repeatLoops,
    jint countInBeats, jboolean applyLimiter,
    jstring projectName, jstring artist, jstring comment, jint bpm) {
    if (!g_jniState.engine) return JNI_FALSE;
    AudioLooper::ExportOptions opts;
    switch (bitDepth) {
        case 24: opts.bitDepth = wav::BitDepth::PCM_24; break;
        case 32: opts.bitDepth = wav::BitDepth::FLOAT_32; break;
        default: opts.bitDepth = wav::BitDepth::PCM_16; break;
    }
    opts.repeatLoops = (repeatLoops > 0) ? repeatLoops : 1;
    opts.applyLimiter = (applyLimiter == JNI_TRUE);

    // Translate count-in beats to frames using the Transport.
    auto& transport = g_jniState.engine->getTransport();
    opts.countInFrames = (countInBeats > 0)
        ? countInBeats * transport.framesPerBeat()
        : 0;

    // Resolve metadata BPM: if caller passed 0, use Transport BPM.
    opts.metadata.bpm = (bpm > 0) ? bpm : static_cast<int>(transport.getBpm());

    auto pickStr = [env](jstring js, std::string& out) {
        if (!js) return;
        const char* c = env->GetStringUTFChars(js, nullptr);
        if (c) {
            out = c;
            env->ReleaseStringUTFChars(js, c);
        }
    };
    pickStr(projectName, opts.metadata.projectName);
    pickStr(artist, opts.metadata.artist);
    pickStr(comment, opts.metadata.comment);

    const char* path = env->GetStringUTFChars(filePath, nullptr);
    bool ok = g_jniState.engine->getAudioLooper().exportMix(path, opts);
    env->ReleaseStringUTFChars(filePath, path);
    return ok ? JNI_TRUE : JNI_FALSE;
}

// Export each active track as a separate WAV file in `directory`.
// Returns number of stems written, or -1 on failure.
JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperExportStems(
    JNIEnv* env, jobject thiz,
    jstring directory, jint bitDepth, jint repeatLoops,
    jint countInBeats, jboolean applyLimiter, jint bpm) {
    if (!g_jniState.engine) return -1;
    AudioLooper::ExportOptions opts;
    switch (bitDepth) {
        case 24: opts.bitDepth = wav::BitDepth::PCM_24; break;
        case 32: opts.bitDepth = wav::BitDepth::FLOAT_32; break;
        default: opts.bitDepth = wav::BitDepth::PCM_16; break;
    }
    opts.repeatLoops = (repeatLoops > 0) ? repeatLoops : 1;
    opts.applyLimiter = (applyLimiter == JNI_TRUE);
    auto& transport = g_jniState.engine->getTransport();
    opts.countInFrames = (countInBeats > 0)
        ? countInBeats * transport.framesPerBeat()
        : 0;
    opts.metadata.bpm = (bpm > 0) ? bpm : static_cast<int>(transport.getBpm());

    const char* dir = env->GetStringUTFChars(directory, nullptr);
    int n = g_jniState.engine->getAudioLooper().exportStems(dir, opts);
    env->ReleaseStringUTFChars(directory, dir);
    return n;
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetExportProgress(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 0.0f;
    return g_jniState.engine->getAudioLooper().getExportProgress();
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperCancelExport(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine)
        g_jniState.engine->getAudioLooper().cancelExport();
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperIsExportInProgress(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return JNI_FALSE;
    return g_jniState.engine->getAudioLooper().isExportInProgress() ? JNI_TRUE : JNI_FALSE;
}

// ========== TELEMETRY (lock-free counters for observability) ==========

JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetFramesDropped(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 0;
    return g_jniState.engine->getAudioLooper().getFramesDropped();
}
JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetExportsCompleted(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 0;
    return g_jniState.engine->getAudioLooper().getExportsCompleted();
}
JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetExportsFailed(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 0;
    return g_jniState.engine->getAudioLooper().getExportsFailed();
}
JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetStemsWritten(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 0;
    return g_jniState.engine->getAudioLooper().getStemsWritten();
}
JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetArmedTriggered(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 0;
    return g_jniState.engine->getAudioLooper().getArmedTriggered();
}
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperResetTelemetry(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine)
        g_jniState.engine->getAudioLooper().resetTelemetry();
}

// ============================================================================
// Looper state listener (push-based notifications C++ → Kotlin)
//
// Replaces the per-track polling pattern (~800 JNI calls/sec at 8 tracks ×
// 30fps × 3 fields) in NoisyPad's LooperViewModel. The audio thread pushes
// events onto LooperEventDispatcher's lock-free queue; the dispatcher's
// worker thread drains it and invokes the Kotlin listener through the sink
// installed below. All Kotlin callbacks therefore arrive on a single
// background thread — listener implementations must marshal to main thread.
// ============================================================================

namespace {

std::mutex                g_looperListenerMutex;
jobject                   g_looperListenerGlobalRef = nullptr;
jclass                    g_looperListenerClass = nullptr;
jmethodID                 g_looperOnTrackProgress = nullptr;
jmethodID                 g_looperOnTrackPlayingChanged = nullptr;
jmethodID                 g_looperOnTrackPeakChanged = nullptr;

/**
 * Attach the worker thread to the JVM if it isn't already, returning a
 * JNIEnv* valid on this thread. We use AttachCurrentThreadAsDaemon so the
 * JVM doesn't insist on a clean detach if the worker outlives the engine
 * (which it shouldn't, but daemon-attach is the safer default).
 */
JNIEnv* attachWorkerEnv() {
    if (!g_javaVm) return nullptr;
    JNIEnv* env = nullptr;
    jint rc = g_javaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (rc == JNI_OK) return env;
    if (rc == JNI_EDETACHED) {
        if (g_javaVm->AttachCurrentThreadAsDaemon(&env, nullptr) != JNI_OK) {
            return nullptr;
        }
        return env;
    }
    return nullptr;
}

void dispatchLooperEvent(const wm::LooperEvent& ev) {
    JNIEnv* env = attachWorkerEnv();
    if (!env) return;

    // Snapshot under the mutex to allow unregister to race safely.
    jobject  listener = nullptr;
    jmethodID method  = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_looperListenerMutex);
        if (!g_looperListenerGlobalRef) return;
        listener = g_looperListenerGlobalRef;
        switch (ev.type) {
            case wm::LooperEvent::Type::Progress:
                method = g_looperOnTrackProgress; break;
            case wm::LooperEvent::Type::PlayingChanged:
                method = g_looperOnTrackPlayingChanged; break;
            case wm::LooperEvent::Type::PeakChanged:
                method = g_looperOnTrackPeakChanged; break;
        }
    }
    if (!listener || !method) return;

    if (ev.type == wm::LooperEvent::Type::PlayingChanged) {
        env->CallVoidMethod(listener, method,
                            static_cast<jint>(ev.trackIndex),
                            ev.value > 0.5f ? JNI_TRUE : JNI_FALSE);
    } else {
        env->CallVoidMethod(listener, method,
                            static_cast<jint>(ev.trackIndex),
                            static_cast<jfloat>(ev.value));
    }
    // Swallow exceptions from the Kotlin side so one bad listener call
    // doesn't kill the worker thread.
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

}  // namespace

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperRegisterStateListener(
    JNIEnv* env, jobject thiz, jobject listener) {
    if (!g_jniState.engine) return JNI_FALSE;
    if (!listener) return JNI_FALSE;

    std::lock_guard<std::mutex> lk(g_looperListenerMutex);

    // Drop any previous registration first.
    if (g_looperListenerGlobalRef) {
        env->DeleteGlobalRef(g_looperListenerGlobalRef);
        g_looperListenerGlobalRef = nullptr;
    }
    if (g_looperListenerClass) {
        env->DeleteGlobalRef(g_looperListenerClass);
        g_looperListenerClass = nullptr;
    }

    g_looperListenerGlobalRef = env->NewGlobalRef(listener);
    if (!g_looperListenerGlobalRef) return JNI_FALSE;

    jclass localCls = env->GetObjectClass(listener);
    if (!localCls) {
        env->DeleteGlobalRef(g_looperListenerGlobalRef);
        g_looperListenerGlobalRef = nullptr;
        return JNI_FALSE;
    }
    g_looperListenerClass = static_cast<jclass>(env->NewGlobalRef(localCls));
    env->DeleteLocalRef(localCls);

    g_looperOnTrackProgress = env->GetMethodID(
        g_looperListenerClass, "onTrackProgress", "(IF)V");
    g_looperOnTrackPlayingChanged = env->GetMethodID(
        g_looperListenerClass, "onTrackPlayingChanged", "(IZ)V");
    g_looperOnTrackPeakChanged = env->GetMethodID(
        g_looperListenerClass, "onTrackPeakChanged", "(IF)V");

    if (!g_looperOnTrackProgress
        || !g_looperOnTrackPlayingChanged
        || !g_looperOnTrackPeakChanged) {
        LOGE("LooperStateListener: missing methods on %s",
             "LooperStateListener");
        env->ExceptionClear();
        env->DeleteGlobalRef(g_looperListenerGlobalRef);
        env->DeleteGlobalRef(g_looperListenerClass);
        g_looperListenerGlobalRef = nullptr;
        g_looperListenerClass = nullptr;
        return JNI_FALSE;
    }

    g_jniState.engine->getLooperEventDispatcher().setSink(&dispatchLooperEvent);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperUnregisterStateListener(
    JNIEnv* env, jobject thiz) {
    if (g_jniState.engine) {
        g_jniState.engine->getLooperEventDispatcher().setSink(nullptr);
    }
    std::lock_guard<std::mutex> lk(g_looperListenerMutex);
    if (g_looperListenerGlobalRef) {
        env->DeleteGlobalRef(g_looperListenerGlobalRef);
        g_looperListenerGlobalRef = nullptr;
    }
    if (g_looperListenerClass) {
        env->DeleteGlobalRef(g_looperListenerClass);
        g_looperListenerClass = nullptr;
    }
    g_looperOnTrackProgress = nullptr;
    g_looperOnTrackPlayingChanged = nullptr;
    g_looperOnTrackPeakChanged = nullptr;
}

JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetDroppedEvents(
    JNIEnv* env, jobject thiz) {
    if (!g_jniState.engine) return 0;
    return g_jniState.engine->getLooperEventDispatcher().getDroppedEvents();
}

} // extern "C"
