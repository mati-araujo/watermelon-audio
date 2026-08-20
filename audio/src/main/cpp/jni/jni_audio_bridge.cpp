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
#include "../api/watermelon_audio.h"
#include "../core/AudioEngine.h"
#include "../core/AudioMode.h"
#include "../nodes/InputNode.h"
#include "../backends/BackendManager.h"
#include "../backends/LibusbBackend.h"
#include "../looper/LooperEventDispatcher.h"
#include "../usb/UsbSnapshotCodec.h"
#include "../usb/RoundTripMeasurer.h"
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
//
// WA-2.6, category `lifecycle`: these entry points go through the C API rather
// than touching AudioEngine directly, so Android and iOS run the same code
// instead of two parallel transcriptions of it.
//
// The null checks are gone because every wma_* function already rejects a null
// handle and returns the same value this file used to return by hand — see the
// WMA_CHECK macros in api/watermelon_audio.cpp. The ensureEngine() calls stay:
// they are the opposite, a side effect (create the engine on first use) that
// the C API deliberately does not have.

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStartEngine(
    JNIEnv* env, jobject thiz) {
    if (!ensureEngine()) {
        LOGE("AudioNativeBridge.startEngine: Failed to create engine");
        return;
    }
    wma_engine_start(g_wmaEngine, WMA_FADE_DEFAULT);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStopEngine(
    JNIEnv* env, jobject thiz) {
    wma_engine_stop(g_wmaEngine, WMA_FADE_DEFAULT);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStartEngineWithFade(
    JNIEnv* env, jobject thiz, jint fadeTimeMs) {
    if (!ensureEngine()) {
        LOGE("AudioNativeBridge.startEngineWithFade: Failed to create engine");
        return;
    }
    // Kotlin already coerces to >= 0, so this never means WMA_FADE_DEFAULT.
    wma_engine_start(g_wmaEngine, fadeTimeMs);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStopEngineWithFade(
    JNIEnv* env, jobject thiz, jint fadeTimeMs) {
    wma_engine_stop(g_wmaEngine, fadeTimeMs);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativePauseEngineWithFade(
    JNIEnv* env, jobject thiz, jint fadeTimeMs) {
    wma_engine_pause(g_wmaEngine, fadeTimeMs);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeResumeEngineWithFade(
    JNIEnv* env, jobject thiz, jint fadeTimeMs) {
    wma_engine_resume(g_wmaEngine, fadeTimeMs);
}

// ==================== State Functions ====================

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetEngineState(
    JNIEnv* env, jobject thiz) {
    return static_cast<jint>(wma_get_engine_state(g_wmaEngine));
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetIsPaused(
    JNIEnv* env, jobject thiz) {
    return wma_is_paused(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetStateVersion(
    JNIEnv* env, jobject thiz) {
    return static_cast<jlong>(wma_get_state_version(g_wmaEngine));
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeHasStreamError(
    JNIEnv* env, jobject thiz) {
    return wma_has_error(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetLastStreamErrorCode(
    JNIEnv* env, jobject thiz) {
    return wma_get_last_error_code(g_wmaEngine);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeClearStreamError(
    JNIEnv* env, jobject thiz) {
    wma_clear_error(g_wmaEngine);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeHasInitializationFailed(
    JNIEnv* env, jobject thiz) {
    return wma_has_init_failed(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsEngineInitialized(
    JNIEnv* env, jobject thiz) {
    return wma_is_initialized(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsUsingReducedBuffers(
    JNIEnv* env, jobject thiz) {
    return wma_is_using_reduced_buffers(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetStreamInfo(
    JNIEnv* env, jobject thiz) {
    int sampleRate = 0, bufferSize = 0;
    float latencyMillis = 0.0f;
    if (!wma_get_stream_info(g_wmaEngine, &sampleRate, &bufferSize, &latencyMillis)) {
        return nullptr;
    }
    jfloatArray result = env->NewFloatArray(3);
    if (result) {
        float data[3] = {
            static_cast<float>(sampleRate),
            static_cast<float>(bufferSize),
            latencyMillis
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
    // wma_set_master_volume clamps to [0, 1] itself.
    wma_set_master_volume(g_wmaEngine, volume);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetMasterVolume(
    JNIEnv* env, jobject thiz) {
    return wma_get_master_volume(g_wmaEngine);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetSynthVolume(
    JNIEnv* env, jobject thiz, jfloat volume) {
    if (!ensureEngine()) {
        return;
    }
    // wma_set_synth_volume clamps to [0, 1] itself.
    wma_set_synth_volume(g_wmaEngine, volume);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetSynthVolume(
    JNIEnv* env, jobject thiz) {
    return wma_get_synth_volume(g_wmaEngine);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetCurrentFadeVolume(
    JNIEnv* env, jobject thiz) {
    return wma_get_fade_volume(g_wmaEngine);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetTargetFadeVolume(
    JNIEnv* env, jobject thiz) {
    return wma_get_target_fade_volume(g_wmaEngine);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetIsFading(
    JNIEnv* env, jobject thiz) {
    return wma_is_fading(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetFadeProgress(
    JNIEnv* env, jobject thiz) {
    return wma_get_fade_progress(g_wmaEngine);
}

// ==================== Real-time Functions ====================

// WA-2.6, category `oscillator/synth` — sections 4 (XY / Oscillator),
// 5 (Engine synth) and 6 (SoundFont) of watermelon_audio.h.
//
// The clamps and range checks moved into the C API; ensureEngine() stays,
// because creating the engine on first use is a JNI-only side effect. What
// else stays is the Java-object handling: pinning arrays and strings, and
// building the jintArray results, which a C API with out-params cannot do.

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetXY(
    JNIEnv* env, jobject thiz, jfloat x, jfloat y) {
    if (!ensureEngine()) {
        return;
    }
    wma_set_xy(g_wmaEngine, x, y);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetFrequencyAndAmplitude(
    JNIEnv* env, jobject thiz, jfloat frequency, jfloat amplitude) {
    if (!ensureEngine()) {
        return;
    }
    wma_set_frequency_amplitude(g_wmaEngine, frequency, amplitude);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetFrequencyRange(
    JNIEnv* env, jobject thiz, jfloat minHz, jfloat maxHz) {
    if (!ensureEngine()) {
        return;
    }
    wma_set_frequency_range(g_wmaEngine, minHz, maxHz);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetOscillatorType(
    JNIEnv* env, jobject thiz, jint type) {
    if (!ensureEngine()) {
        return;
    }
    wma_set_oscillator_type(g_wmaEngine, type);
}

// ========== SYNTH ENGINE SYSTEM (Phase 6) ==========

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetEngineType(
    JNIEnv* env, jobject thiz, jint engineType) {
    if (!ensureEngine()) {
        return;
    }
    wma_set_engine_type(g_wmaEngine, engineType);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetEngineParameter(
    JNIEnv* env, jobject thiz, jint paramId, jfloat value) {
    if (!ensureEngine()) {
        return;
    }
    wma_set_engine_param(g_wmaEngine, paramId, value);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetEngineType(
    JNIEnv* env, jobject thiz) {
    if (!ensureEngine()) {
        return 0;
    }
    return wma_get_engine_type(g_wmaEngine);
}

// ========== SOUNDFONT ENGINE (Phase 8) ==========

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLoadSoundFont(
    JNIEnv* env, jobject thiz, jbyteArray data) {
    if (!ensureEngine()) return JNI_FALSE;
    jsize size = env->GetArrayLength(data);
    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    if (!bytes) return JNI_FALSE;
    bool result = wma_sf_load_data(g_wmaEngine, bytes, size);
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLoadSoundFontFromPath(
    JNIEnv* env, jobject thiz, jstring path) {
    if (!ensureEngine()) return JNI_FALSE;
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (!pathStr) return JNI_FALSE;
    bool result = wma_sf_load_path(g_wmaEngine, pathStr);
    env->ReleaseStringUTFChars(path, pathStr);
    return result ? JNI_TRUE : JNI_FALSE;
}

// fd OWNERSHIP: the fd stays owned by the Kotlin caller. This call is
// synchronous — native mmaps the [offset, offset+length) region, lets tsf
// parse it, and unmaps before returning. It never dup()s, close()s, or
// retains the fd, so the caller (which typically holds an AssetFileDescriptor)
// must keep the fd open for the duration of the call and close it afterwards.
JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLoadSoundFontFromFd(
    JNIEnv* env, jobject thiz, jint fd, jlong offset, jlong length) {
    if (!ensureEngine()) return JNI_FALSE;
    // wma_sf_load_fd rejects a negative fd or a non-positive length up front,
    // which this path did not. Both ways end in false, so it is a cheaper no,
    // not a different answer.
    bool result = wma_sf_load_fd(g_wmaEngine,
                                 static_cast<int>(fd),
                                 static_cast<int64_t>(offset),
                                 static_cast<int64_t>(length));
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeUnloadSoundFont(
    JNIEnv* env, jobject thiz) {
    if (!ensureEngine()) return;
    wma_sf_unload(g_wmaEngine);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetSoundFontPreset(
    JNIEnv* env, jobject thiz, jint presetIndex) {
    if (!ensureEngine()) return;
    wma_sf_set_preset(g_wmaEngine, presetIndex);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetSoundFontPresetCount(
    JNIEnv* env, jobject thiz) {
    if (!ensureEngine()) return 0;
    return wma_sf_get_preset_count(g_wmaEngine);
}

JNIEXPORT jstring JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetSoundFontPresetName(
    JNIEnv* env, jobject thiz, jint presetIndex) {
    if (!ensureEngine()) return nullptr;
    const char* name = wma_sf_get_preset_name(g_wmaEngine, presetIndex);
    if (!name) return nullptr;
    return env->NewStringUTF(name);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsSoundFontLoaded(
    JNIEnv* env, jobject thiz) {
    if (!ensureEngine()) return JNI_FALSE;
    return wma_sf_is_loaded(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jintArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetSoundFontPresetKeyRange(
    JNIEnv* env, jobject thiz, jint presetIndex) {
    if (!ensureEngine()) return nullptr;
    int minKey = 0, maxKey = 127;
    if (!wma_sf_get_preset_key_range(g_wmaEngine, presetIndex, &minKey, &maxKey)) {
        return nullptr;
    }
    jintArray result = env->NewIntArray(2);
    if (!result) return nullptr;
    jint buf[2] = { minKey, maxKey };
    env->SetIntArrayRegion(result, 0, 2, buf);
    return result;
}

JNIEXPORT jintArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetSoundFontPresetBankProgram(
    JNIEnv* env, jobject thiz, jint presetIndex) {
    if (!ensureEngine()) return nullptr;
    int bank = -1, program = -1;
    if (!wma_sf_get_preset_bank_program(g_wmaEngine, presetIndex, &bank, &program)) {
        return nullptr;
    }
    jintArray result = env->NewIntArray(2);
    if (!result) return nullptr;
    jint buf[2] = { bank, program };
    env->SetIntArrayRegion(result, 0, 2, buf);
    return result;
}

// ========== SOUNDFONT POLYPHONY (Phase 8E) ==========

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSfNoteOn(
    JNIEnv* env, jobject thiz, jint touchId, jint midiNote, jfloat velocity) {
    if (!ensureEngine()) return;
    wma_sf_note_on(g_wmaEngine, touchId, midiNote, velocity);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSfNoteOff(
    JNIEnv* env, jobject thiz, jint touchId) {
    if (!ensureEngine()) return;
    wma_sf_note_off(g_wmaEngine, touchId);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSfNoteOffAll(
    JNIEnv* env, jobject thiz) {
    if (!ensureEngine()) return;
    wma_sf_note_off_all(g_wmaEngine);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSfNoteOffAllExcept(
    JNIEnv* env, jobject thiz, jint keepTouchId) {
    if (!ensureEngine()) return;
    wma_sf_note_off_all_except(g_wmaEngine, keepTouchId);
}

// ========== VOICE FILTER (Phase 6) ==========

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetVoiceFilterEnabled(
    JNIEnv* env, jobject thiz, jboolean enabled) {
    if (!ensureEngine()) return;
    wma_voice_filter_set_enabled(g_wmaEngine, enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetVoiceFilterCutoff(
    JNIEnv* env, jobject thiz, jfloat hz) {
    if (!ensureEngine()) return;
    wma_voice_filter_set_cutoff(g_wmaEngine, hz);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetVoiceFilterResonance(
    JNIEnv* env, jobject thiz, jfloat q) {
    if (!ensureEngine()) return;
    wma_voice_filter_set_resonance(g_wmaEngine, q);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetVoiceFilterMode(
    JNIEnv* env, jobject thiz, jint mode) {
    if (!ensureEngine()) return;
    wma_voice_filter_set_mode(g_wmaEngine, mode);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetWaveformSamples(
    JNIEnv* env, jobject thiz, jfloatArray buffer, jint size) {
    if (buffer == nullptr || size <= 0) {
        return 0;
    }

    // The array length still has to be honoured here: wma_* takes a bare
    // pointer and cannot know how big the Java array is, so asking for more
    // than it holds would write past the end.
    ScopedFloatArrayRW samples(env, buffer);
    if (!samples.isValid()) {
        return 0;
    }
    const int samplesToGet = std::min(static_cast<int>(samples.size()), static_cast<int>(size));

    return wma_get_waveform_samples(g_wmaEngine, samples.get(), samplesToGet);
}

// ==================== Modulator Functions ====================

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetModulatorType(
    JNIEnv* env, jobject thiz, jint type) {
    if (!ensureEngine()) {
        return JniError::ENGINE_NOT_INITIALIZED;
    }
    return wma_set_modulator_type(g_wmaEngine, type);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetModulatorParameter(
    JNIEnv* env, jobject thiz, jint paramId, jfloat value) {
    if (!ensureEngine()) {
        return JniError::ENGINE_NOT_INITIALIZED;
    }
    return wma_set_modulator_param(g_wmaEngine, paramId, value);
}

// ==================== Effect Functions ====================
//
// WA-2.6, category `effects` — section 8 of watermelon_audio.h.
//
// The C API already returned this file's exact error codes (WmaResult mirrors
// the JniError namespace value for value), so these are the most mechanical
// wrappers of the three categories migrated so far: the index guards, the
// EFFECT_TYPE_COUNT check and the try/catch blocks all live there now.
//
// What stays here is the JNI-only work: pinning the Java arrays
// (ScopedIntArrayRW / ScopedFloatArrayRW) and checking their lengths agree,
// which is not something a C API taking plain pointers can do for us.

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeAddEffect(
    JNIEnv* env, jobject thiz, jint typeId) {
    // Returns the new effect's index on success, a negative WmaResult on error.
    return wma_effect_add(g_wmaEngine, typeId);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeRemoveEffect(
    JNIEnv* env, jobject thiz, jint index) {
    return wma_effect_remove(g_wmaEngine, index);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeClearAllEffects(
    JNIEnv* env, jobject thiz) {
    return wma_effect_clear_all(g_wmaEngine);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetEffectParameter(
    JNIEnv* env, jobject thiz, jint index, jint paramId, jfloat value) {
    return wma_effect_set_param(g_wmaEngine, index, paramId, value);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetEffectParametersBatch(
    JNIEnv* env, jobject thiz, jint index, jintArray paramIds, jfloatArray values) {
    // Kept ahead of the array work so a missing engine still outranks an empty
    // batch, which is the precedence this entry point has always had. It is the
    // one null check in this block, and it is about error ordering, not safety —
    // wma_effect_set_params_batch rejects a null handle by itself.
    if (!wma_is_initialized(g_wmaEngine)) {
        return JniError::ENGINE_NOT_INITIALIZED;
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

    // One state-version bump for the whole batch, not N — see AUD-6 in the
    // header. The C API used to loop over the individual setter here, so iOS
    // still had the bug this path exists to avoid.
    return wma_effect_set_params_batch(g_wmaEngine, index, ids.get(), vals.get(),
                                       static_cast<int>(length));
}

// Phase 4.1: Multi-effect batch parameter update
JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetMultipleEffectParameters(
    JNIEnv* env, jobject thiz,
    jintArray effectIndices,
    jintArray paramIds,
    jfloatArray values) {

    // Same reason as the single-effect batch above: a missing engine outranks a
    // size mismatch, which is the order this has always reported.
    if (!wma_is_initialized(g_wmaEngine)) {
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

    jint result = wma_effect_set_params_multi(g_wmaEngine, indices.get(), params.get(),
                                              vals.get(), static_cast<int>(length));
    if (result == JniError::SUCCESS && length > 0) {
        LOGI("nativeSetMultipleEffectParameters: applied %d updates", static_cast<int>(length));
    }
    return result;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetEffectBypass(
    JNIEnv* env, jobject thiz, jint index, jboolean bypass) {
    return wma_effect_set_bypass(g_wmaEngine, index, bypass == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeReorderEffects(
    JNIEnv* env, jobject thiz, jint fromIndex, jint toIndex) {
    return wma_effect_reorder(g_wmaEngine, fromIndex, toIndex);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetEffectChainSize(
    JNIEnv* env, jobject thiz) {
    return static_cast<jint>(wma_effect_chain_size(g_wmaEngine));
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetEffectType(
    JNIEnv* env, jobject thiz, jint index) {
    return static_cast<jint>(wma_effect_get_type(g_wmaEngine, index));
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetEffectParameter(
    JNIEnv* env, jobject thiz, jint index, jint paramId) {
    return wma_effect_get_param(g_wmaEngine, index, paramId);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsEffectBypassed(
    JNIEnv* env, jobject thiz, jint index) {
    return wma_effect_is_bypassed(g_wmaEngine, index) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetEffectsBypass(
    JNIEnv* env, jobject thiz, jboolean bypass) {
    return wma_effect_set_global_bypass(g_wmaEngine, bypass == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsEffectsBypassed(
    JNIEnv* env, jobject thiz) {
    return wma_effect_is_global_bypassed(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

// ==================== Global BPM ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetBpm(
    JNIEnv* env, jobject thiz, jfloat bpm) {
    // Fans out to the tempo-synced effects AND the Transport — see §20.
    wma_set_bpm(g_wmaEngine, bpm);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetBpm(
    JNIEnv* env, jobject thiz) {
    // The no-engine default of 120 BPM lives in the C API.
    return wma_get_bpm(g_wmaEngine);
}

// ==================== Effect Routing Mode ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetRoutingMode(
    JNIEnv* env, jobject thiz, jint mode) {
    // The 0..5 RoutingMode range check lives in wma_set_routing_mode now.
    wma_set_routing_mode(g_wmaEngine, mode);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetRoutingMode(
    JNIEnv* env, jobject thiz) {
    // The no-engine default of 0 (SERIAL) lives in the C API.
    return wma_get_routing_mode(g_wmaEngine);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetParallelMix(
    JNIEnv* env, jobject thiz, jfloat mix) {
    wma_set_parallel_mix(g_wmaEngine, mix);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetFeedbackAmount(
    JNIEnv* env, jobject thiz, jfloat amount) {
    wma_set_feedback_amount(g_wmaEngine, amount);
}

// ==================== Mode Functions ====================

// WA-2.6, category `mode` — section 11 of watermelon_audio.h.
//
// THIS IS THE CATEGORY THAT KILLS THE DUPLICATED MODE STATE. JniGlobalState used
// to carry its own currentMode / modeTransitionInProgress /
// modeTransitionProgress alongside WmaEngine's, as independent copies; the JNI
// wrote and read one set, the C API the other. Both are gone from jni_common.h
// now — there is one copy, in WmaEngine, and these functions read it.
//
// The transition logic moved too, and it moved UP: wma_set_audio_mode used to be
// documented as "a simplified version" of what lives here, missing the effect
// chain reset and the USB path. Now it is the real one and this is a wrapper.

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetAudioMode(
    JNIEnv* env, jobject thiz, jint mode) {
    LOGI("AudioNativeBridge.setAudioMode: ENTER mode=%d", mode);

    if (!ensureEngine()) {
        LOGE("AudioNativeBridge.setAudioMode: engine not available");
        return;
    }

    // wma_set_audio_mode creates the InputNode itself for the modes that need
    // one, but it has no way to know about the JNI's mirror of that shared_ptr.
    // Creating it here first — through the JNI helper, which syncs the mirror —
    // leaves both handles on the same node, and the C API's own ensureInputNode
    // then finds it already there.
    if (wma_mode_requires_input(mode)) {
        ensureInputNode();
    }

    wma_set_audio_mode(g_wmaEngine, mode);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetAudioMode(
    JNIEnv* env, jobject thiz) {
    return static_cast<jint>(wma_get_audio_mode(g_wmaEngine));
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsInModeTransition(
    JNIEnv* env, jobject thiz) {
    // Always false. Nothing writes the flag on either side — see the warning on
    // wma_is_in_mode_transition. Migrating it does not change that; it just puts
    // the dead state in one place instead of two.
    return wma_is_in_mode_transition(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetModeTransitionProgress(
    JNIEnv* env, jobject thiz) {
    return wma_get_mode_transition_progress(g_wmaEngine);
}

JNIEXPORT jstring JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetModeName(
    JNIEnv* env, jobject thiz, jint mode) {
    return env->NewStringUTF(wma_get_mode_name(mode));
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeModeRequiresInput(
    JNIEnv* env, jobject thiz, jint mode) {
    return wma_mode_requires_input(mode) ? JNI_TRUE : JNI_FALSE;
}

// ==================== Input Functions ====================
//
// WA-2.6, category `input/monitor`. Same shape as the lifecycle block: the null
// checks and the default returns live in api/watermelon_audio.cpp now.
//
// Two things do NOT move, and both are about the JNI's own mirror of the node:
//
//   - ensureInputNode() (jni_engine.cpp) still runs before wma_input_start().
//     The C API creates the node on demand too, but it has no way to know about
//     g_jniState.inputNode, so calling it alone would leave the mirror empty.
//
//   - releaseInputNode() still wraps wma_input_release(), because the same
//     mirror has to drop with it.
//
// THIS CHANGES BEHAVIOUR ON ANDROID, in two spots, both because the C API does
// more than the JNI used to — see the notes on the individual functions.

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStartInputStream(
    JNIEnv* env, jobject thiz) {
    // ANDROID BEHAVIOUR CHANGE: when InputNode::startInputStream() fails, this
    // used to just report false. wma_input_start() then asks BackendManager for
    // capture on the existing output stream, which may reopen it. Only the
    // failure path is affected — a successful mic open returns before that — and
    // requestCapture() falls back to reopening without capture, so the worst
    // case is a stream restart instead of a silent false. Needs the device smoke.
    if (!ensureInputNode()) {
        return JNI_FALSE;
    }
    return wma_input_start(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStopInputStream(
    JNIEnv* env, jobject thiz) {
    // Also withdraws the capture request, which the JNI never did. Verified not
    // to trigger a reopen: BackendManager::requestCapture returns early when the
    // request is being dropped rather than raised.
    wma_input_stop(g_wmaEngine);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsInputStreamRunning(
    JNIEnv* env, jobject thiz) {
    // ANDROID BEHAVIOUR CHANGE: wma_input_is_running() also reports true when
    // the backend itself carries capture (full-duplex) without a separate node
    // stream. On Android that is the USB/split path, which used to read as "not
    // running" here even while input was flowing. Needs the device smoke.
    return wma_input_is_running(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsInputStarting(
    JNIEnv* env, jobject thiz) {
    // La mitad que falta para distinguir "todavia no" de "no": mientras esto sea
    // true, que nativeIsInputStreamRunning() diga false no es una negativa. En el
    // camino Oboe directo de Android es siempre false —ese start es sincronico—;
    // en el camino USB/split, no.
    return wma_input_is_starting(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetInputSource(
    JNIEnv* env, jobject thiz, jint source) {
    // The range check and the try/catch moved into wma_input_set_source.
    wma_input_set_source(g_wmaEngine, source);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetInputSource(
    JNIEnv* env, jobject thiz) {
    return static_cast<jint>(wma_input_get_source(g_wmaEngine));
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetInputGain(
    JNIEnv* env, jobject thiz, jfloat gainDb) {
    wma_input_set_gain(g_wmaEngine, gainDb);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetInputGain(
    JNIEnv* env, jobject thiz) {
    return wma_input_get_gain(g_wmaEngine);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetNoiseGateEnabled(
    JNIEnv* env, jobject thiz, jboolean enabled) {
    wma_input_set_noise_gate(g_wmaEngine, enabled);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsNoiseGateEnabled(
    JNIEnv* env, jobject thiz) {
    return wma_input_is_noise_gate_enabled(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetNoiseGateThreshold(
    JNIEnv* env, jobject thiz, jfloat thresholdDb) {
    wma_input_set_noise_gate_threshold(g_wmaEngine, thresholdDb);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetInputLevel(
    JNIEnv* env, jobject thiz, jint channel) {
    return wma_input_get_level(g_wmaEngine, channel);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetInputLevelLinear(
    JNIEnv* env, jobject thiz, jint channel) {
    return wma_input_get_level_linear(g_wmaEngine, channel);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsInputClipping(
    JNIEnv* env, jobject thiz) {
    return wma_input_is_clipping(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsNoiseGateOpen(
    JNIEnv* env, jobject thiz) {
    return wma_input_is_noise_gate_open(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetInputLatencyMs(
    JNIEnv* env, jobject thiz) {
    return wma_input_get_latency_ms(g_wmaEngine);
}

// Batched input metering: the 7 values a UI meter polls per frame in a single
// JNI crossing (was 7 separate getters + a running-state check = 8 crossings per
// tick at 60 fps ≈ 480/s). The layout is the C API's contract now — see
// wma_input_get_metering_snapshot — and MUST stay in sync with the Kotlin
// consumer (InputStateManager).
//
// Returns null when there is no input node so the caller can fall back to the
// individual getters (older library / no active stream).
JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetInputMeteringSnapshot(
    JNIEnv* env, jobject thiz) {
    float values[WMA_INPUT_METERING_VALUES];
    if (!wma_input_get_metering_snapshot(g_wmaEngine, values)) {
        return nullptr;
    }
    jfloatArray result = env->NewFloatArray(WMA_INPUT_METERING_VALUES);
    if (result == nullptr) {
        return nullptr;
    }
    env->SetFloatArrayRegion(result, 0, WMA_INPUT_METERING_VALUES, values);
    return result;
}

// ==================== Tuner analysis (REQ-001 S1) ====================
//
// Same batched shape as the input metering above, and for a stronger reason:
// the C API publishes these eight values under a seqlock, so they all come from
// the SAME tick. Reading them one by one would let the UI pair this tick's cents
// with the next tick's phase angle, which is what makes a strobe disc jump.
//
// Returns null when there is no analysis seam or nothing has been published yet
// — null is NOT the same as all-zeros, and flattening it would hand the Kotlin
// side a plausible reading nobody measured.

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStartTuner(
    JNIEnv* env, jobject thiz) {
    return wma_tuner_start(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeStopTuner(
    JNIEnv* env, jobject thiz) {
    wma_tuner_stop(g_wmaEngine);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsTunerRunning(
    JNIEnv* env, jobject thiz) {
    return wma_tuner_is_running(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetTunerTarget(
    JNIEnv* env, jobject thiz, jfloat hz) {
    return wma_tuner_set_target(g_wmaEngine, hz) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetTunerTarget(
    JNIEnv* env, jobject thiz) {
    return wma_tuner_get_target(g_wmaEngine);
}

JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetTunerSnapshot(
    JNIEnv* env, jobject thiz) {
    float values[WMA_TUNER_SNAPSHOT_VALUES];
    if (!wma_tuner_get_snapshot(g_wmaEngine, values)) {
        return nullptr;
    }
    jfloatArray result = env->NewFloatArray(WMA_TUNER_SNAPSHOT_VALUES);
    if (result == nullptr) {
        return nullptr;
    }
    env->SetFloatArrayRegion(result, 0, WMA_TUNER_SNAPSHOT_VALUES, values);
    return result;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeReleaseInputNode(
    JNIEnv* env, jobject thiz) {
    // Not wma_input_release() directly: the JNI's mirror handle has to drop too.
    releaseInputNode();
}

// ==================== Monitoring Functions ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetMonitoringEnabled(
    JNIEnv* env, jobject thiz, jboolean enabled) {
    wma_input_set_monitoring(g_wmaEngine, enabled);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsMonitoringEnabled(
    JNIEnv* env, jobject thiz) {
    return wma_input_is_monitoring_enabled(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetMonitoringVolume(
    JNIEnv* env, jobject thiz, jfloat volume) {
    // wma_input_set_monitoring_volume clamps to [0, 1] itself.
    wma_input_set_monitoring_volume(g_wmaEngine, volume);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetMonitoringVolume(
    JNIEnv* env, jobject thiz) {
    return wma_input_get_monitoring_volume(g_wmaEngine);
}

// ==================== Dual Touch Functions ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetDualTouchMode(
    JNIEnv* env, jobject thiz, jboolean enabled) {
    wma_set_dual_touch_mode(g_wmaEngine, enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetDualTouch(
    JNIEnv* env, jobject thiz,
    jfloat x1, jfloat y1, jfloat freq1, jfloat amp1, jfloat pressure1,
    jfloat x2, jfloat y2, jfloat freq2, jfloat amp2, jfloat pressure2,
    jfloat distance, jfloat angle) {
    wma_set_dual_touch(g_wmaEngine,
                       x1, y1, freq1, amp1, pressure1,
                       x2, y2, freq2, amp2, pressure2,
                       distance, angle);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetDualTouchMixMode(
    JNIEnv* env, jobject thiz, jint modeId) {
    // The 0–5 range check lives in wma_set_dual_touch_mix_mode now.
    wma_set_dual_touch_mix_mode(g_wmaEngine, modeId);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetSecondaryOscillatorType(
    JNIEnv* env, jobject thiz, jint typeId) {
    // Section 4, sitting in the dual-touch block rather than with the other
    // oscillator setters — which is exactly why it nearly got left behind.
    wma_set_secondary_oscillator_type(g_wmaEngine, typeId);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetDualTouchMode(
    JNIEnv* env, jobject thiz) {
    return wma_get_dual_touch_mode(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

// ==================== Voice System Functions ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeEnableVoiceSystem(
    JNIEnv* env, jobject thiz, jboolean enable) {
    wma_voice_enable(g_wmaEngine, enable == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsVoiceSystemEnabled(
    JNIEnv* env, jobject thiz) {
    return wma_voice_is_enabled(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeUpdateMultiTouch(
    JNIEnv* env, jobject thiz, jint count, jfloatArray touchData) {
    if (count <= 0 || touchData == nullptr) {
        wma_voice_update_multi_touch(g_wmaEngine, nullptr, 0);
        return;
    }

    ScopedFloatArrayRW data(env, touchData);
    if (!data.isValid()) {
        return;
    }

    // OUT-OF-BOUNDS READ, fixed here.
    //
    // `count` arrives as its own parameter, independent of how long the array
    // actually is, and nothing used to cross-check the two. The unpack reads
    // count * 6 floats (capped at 4 touches = 24), so a caller passing count=4
    // with a two-touch array read 12 floats past the end of the heap buffer —
    // garbage touch data at best.
    //
    // The check belongs here rather than in the C API: wma_* takes a bare
    // pointer and cannot know the length. Same reason the effect batch keeps
    // its array-length check on this side.
    const int TOUCH_STRIDE = 6;
    const int touchesInArray = data.size() / TOUCH_STRIDE;
    const int maxTouches = std::min({static_cast<int>(count), touchesInArray, 4});
    if (maxTouches <= 0) {
        wma_voice_update_multi_touch(g_wmaEngine, nullptr, 0);
        return;
    }

    // Layout matches Kotlin's floatsPerTouch = 6:
    // [x, y, freq, amp, pressure, pointerId]. The unpacking itself lives in
    // wma_voice_update_multi_touch, so iOS gets the same stride by construction.
    wma_voice_update_multi_touch(g_wmaEngine, data.get(), maxTouches);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetActiveVoiceCount(
    JNIEnv* env, jobject thiz) {
    return wma_voice_get_active_count(g_wmaEngine);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetMaxVoices(
    JNIEnv* env, jobject thiz, jint maxVoices) {
    wma_voice_set_max(g_wmaEngine, maxVoices);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetVoiceStealingStrategy(
    JNIEnv* env, jobject thiz, jint strategy) {
    wma_voice_set_stealing_strategy(g_wmaEngine, strategy);
}

// ==================== Chord Functions (Phase 9C) ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTriggerChordNotes(
    JNIEnv* env, jobject thiz, jfloatArray frequencies, jfloat amplitude, jint oscillatorType) {
    // Unlike multi-touch, the count here IS the array length, so there is
    // nothing to cross-check.
    if (frequencies == nullptr) return;

    ScopedFloatArrayRW freqs(env, frequencies);
    if (!freqs.isValid()) return;

    wma_voice_trigger_chord(g_wmaEngine, freqs.get(), freqs.size(),
                            amplitude, oscillatorType);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeUpdateChordNotes(
    JNIEnv* env, jobject thiz, jfloatArray frequencies, jfloat amplitude) {
    if (frequencies == nullptr) return;

    ScopedFloatArrayRW freqs(env, frequencies);
    if (!freqs.isValid()) return;

    wma_voice_update_chord(g_wmaEngine, freqs.get(), freqs.size(), amplitude);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeReleaseChordNotes(
    JNIEnv* env, jobject thiz) {
    wma_voice_release_chord(g_wmaEngine);
}

// ==================== Vocoder Functions ====================

// WA-2.6, category `oscillator/synth` — section 15 (Vocoder). The 20–2000 Hz
// clamp on the carrier moved into wma_vocoder_set_carrier_freq.

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetVocoderCarrierSource(
    JNIEnv* env, jobject thiz, jboolean useInternalCarrier) {
    wma_vocoder_set_carrier_source(g_wmaEngine, useInternalCarrier == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetVocoderCarrierFrequency(
    JNIEnv* env, jobject thiz, jfloat frequency) {
    wma_vocoder_set_carrier_freq(g_wmaEngine, frequency);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeHasVocoderEffect(
    JNIEnv* env, jobject thiz) {
    return wma_vocoder_has_effect(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetVocoderModulatorSource(
    JNIEnv* env, jobject thiz, jboolean useExternalMod) {
    wma_vocoder_set_modulator_source(g_wmaEngine, useExternalMod == JNI_TRUE);
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
    wma_set_use_backend_manager(g_wmaEngine, use);
}

// Android-only, and not for want of a portable body: createSplitBackend() itself
// compiles for iOS, but resolveBackendForSplit() only resolves two endpoints —
// OBOE (the system backend) and LIBUSB. On iOS createUsbAudioBackend() returns
// null by D4, so the LIBUSB endpoint is always null and the only remaining call
// is system+system, which the `input == output` guard rejects. Every path
// returns false. Splitting input from output is a USB feature; there is no
// behavior here to lift into the C API.
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
    return wma_select_backend(backendId) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetCurrentBackendType(
    JNIEnv* env, jobject thiz) {
    return static_cast<jint>(wma_get_backend_type());
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
//
// nativeIsUsingReducedBuffers moved up to the State block with the rest of
// section 2 of the C API (WA-2.6, category `lifecycle`).

// ==================== Automation Functions ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetAutomationParameter(
    JNIEnv* env, jobject thiz, jint effectIndex, jint paramId, jfloat xyValue) {
    wma_set_automation_param(g_wmaEngine, effectIndex, paramId, xyValue);
}

// ==================== XY Mapping Config Functions (Phase 4) ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetMappingConfig(
    JNIEnv* env, jobject thiz,
    jint axis, jint effectIndex, jint paramId,
    jint curve, jint polarity,
    jfloat mapMin, jfloat mapMax, jboolean inverted) {
    // The axis/curve/polarity range checks and the isfinite() guard on the
    // mapping bounds all live in wma_set_mapping_config now.
    wma_set_mapping_config(g_wmaEngine, axis, effectIndex, paramId,
                           curve, polarity,
                           mapMin, mapMax, static_cast<bool>(inverted));
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeClearMappingConfig(
    JNIEnv* env, jobject thiz, jint axis) {
    // The 0..2 axis check lives in wma_clear_mapping_config now.
    wma_clear_mapping_config(g_wmaEngine, axis);
}

// nativeSetDepthValue was removed on 2026-07-27 together with wma_set_depth_value:
// it was a dead store in all four layers. Depth is mapping axis 2 —
// nativeApplyAutomation(2, value).

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeApplyAutomation(
    JNIEnv* env, jobject thiz, jint axis, jfloat normalizedValue) {
    // The 0..2 axis check and the clamp live in wma_apply_automation now.
    wma_apply_automation(g_wmaEngine, axis, normalizedValue);
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
    // DSP block size comes from the active latency profile (Fase 1): the
    // tuning's dspBlockFrames is stored in mRequestedBufferSize by
    // setLatencyTuning/setLatencyProfile. Defaults to 256 (SAFE) when no
    // profile was set, so removing the old hardcoded setBufferSize(256) is
    // behavior-preserving for existing callers while letting LOW_LATENCY use
    // its 96-frame block.

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
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetUsbLatencyProfile(
    JNIEnv* env, jobject thiz, jint profile) {
    // Persist on the BackendManager (not the LibusbBackend directly): it
    // survives backend recreation and is re-applied via applyConfigToBackend,
    // exactly like the USB streaming mode (setFullDuplexEnabled). Consumed by
    // setupTransferManager() on the next start().
    auto& manager = watermelon_audio::BackendManager::getInstance();
    const auto p = (profile == 1)
        ? watermelon_audio::usb::UsbLatencyProfile::LOW_LATENCY
        : watermelon_audio::usb::UsbLatencyProfile::SAFE;
    manager.setLatencyProfile(p);
    wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
        "USB_SET_LATENCY_PROFILE: profile=%d (0=SAFE,1=LOW_LATENCY)",
        static_cast<int>(p));
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetUsbLatencyTuning(
    JNIEnv* env, jobject thiz, jint targetTransferMs, jint numTransfers,
    jint jitterBudgetMs, jint dspBlockFrames, jint ringCapacityMs) {
    auto& manager = watermelon_audio::BackendManager::getInstance();
    auto* backend = manager.getLibusbBackend();
    if (!backend) {
        LOGW("nativeSetUsbLatencyTuning: no LibusbBackend");
        return JNI_FALSE;
    }
    // Latched; consumed on the next start() (see nativeSetUsbLatencyProfile).
    watermelon_audio::usb::UsbLatencyTuning tuning;
    tuning.targetTransferMs = static_cast<int>(targetTransferMs);
    tuning.numTransfers     = static_cast<int>(numTransfers);
    tuning.jitterBudgetMs   = static_cast<int>(jitterBudgetMs);
    tuning.dspBlockFrames   = static_cast<int>(dspBlockFrames);
    tuning.ringCapacityMs   = static_cast<int>(ringCapacityMs);
    backend->setLatencyTuning(tuning);
    LOGI("nativeSetUsbLatencyTuning: targetTransferMs=%d numTransfers=%d "
         "jitterBudgetMs=%d dspBlockFrames=%d ringCapacityMs=%d",
         tuning.targetTransferMs, tuning.numTransfers, tuning.jitterBudgetMs,
         tuning.dspBlockFrames, tuning.ringCapacityMs);
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

// Android-only for the same reason as createSplitBackend: the body compiles for
// iOS, but fallbackToOboe() *is* the USB-disconnect recovery path — it clears
// mUsbBackendAvailable, selects the system backend and tears down the USB and
// split backends. With no USB backend on iOS (D4) there is nothing to fall back
// *from*, and nothing calls it. Migrating it would export a no-op.
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeFallbackToOboeBackend(
    JNIEnv* env, jobject thiz) {
    LOGI("AudioNativeBridge.fallbackToOboeBackend: triggered from Kotlin");
    auto& manager = watermelon_audio::BackendManager::getInstance();
    manager.fallbackToOboe();
}

// Returns null unconditionally, deliberately — and that IS the fix, not a gutting.
//
// This has handed back ten zeros since the initial extraction (d66ac4d): it
// declared `jfloat stats[10] = {0}` and returned it without ever filling it.
// The zeros were not harmless, because the only caller reads them through elvis
// fallbacks that a *non-null* array silently defeats — an all-zero array is not
// absent data, it is data that says zero (UsbAudioManagerImpl.getTransferStats):
//
//     val currentBufferMs = adaptiveStats?.getOrNull(0)?.toInt()  // 0f, never null
//         ?: nativeBridge.getCurrentUsbBufferMs()                 // so: never consulted
//     val healthScore     = adaptiveStats?.getOrNull(6) ?: 100f   // 0f, not 100f
//     val bufferAdjustments = adaptiveStats?.getOrNull(8)?.toInt() ?: 0
//
// So bufferMs came out 0 and healthScore 0, and NoisyPad gates its entire
// Buffer/Health/Adjustments card on `if (stats.bufferMs > 0)` (UsbAudioScreen.kt,
// "Adaptive buffer stats (if available)"). Net effect: that card has never
// rendered, on any USB streaming session, since the extraction. Returning null is
// precisely what the caller's own defaults were written for — 5 ms, 100 %, 0.
//
// NOT implemented from AdaptiveBufferController on purpose. That controller is the
// DEPRECATED legacy ring-capacity path (see "Adaptive Buffer Reconfiguration" in
// UsbTransferManager.cpp); its underrun/overrun/transfer counters are still fed,
// but updateFromProfiler() has no caller anywhere, so healthScore would be frozen
// at its 100.0 construction default regardless. Reporting real telemetry means
// repointing this at the jitter-budget numbers — App plan Etapa D — which is a
// product decision, not a migration. Inventing figures here that no one can
// validate without USB hardware would just be a nicer-looking lie.
JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetAdaptiveBufferStats(
    JNIEnv* env, jobject thiz) {
    return nullptr;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetCurrentUsbBufferMs(
    JNIEnv* env, jobject thiz) {
    return 5; // Default ~5ms
}

// ==================== Output Level Metering Functions (Phase 1 - Gain Staging) ====================

// nativeGetMasterVolume moved up to the Volume block with the rest of
// section 3 of the C API (WA-2.6, category `lifecycle`).

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetOutputPeakLevel(
    JNIEnv* env, jobject thiz, jint channel) {
    return wma_get_output_peak(g_wmaEngine, channel);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetOutputRmsLevel(
    JNIEnv* env, jobject thiz, jint channel) {
    return wma_get_output_rms(g_wmaEngine, channel);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetOutputPeakLevelDb(
    JNIEnv* env, jobject thiz, jint channel) {
    // The -100 dB floor for a silent or absent signal lives in the C API.
    return wma_get_output_peak_db(g_wmaEngine, channel);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetOutputRmsLevelDb(
    JNIEnv* env, jobject thiz, jint channel) {
    return wma_get_output_rms_db(g_wmaEngine, channel);
}

JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetOutputLevels(
    JNIEnv* env, jobject thiz) {
    // Returns [peakL, peakR, rmsL, rmsR] for efficient single-call metering.
    // Pre-zeroed because wma_get_output_levels leaves the buffer untouched
    // when there is no engine, and this entry point has always answered with
    // an array of zeros rather than null in that case.
    jfloat levels[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    wma_get_output_levels(g_wmaEngine, levels);

    jfloatArray result = env->NewFloatArray(4);
    if (!result) {
        return nullptr;
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
//
// WA-2.6, category `oscillator/synth` — section 18. This block was already a
// 1:1 transcription of the C API, guards and default returns included, so the
// migration is the plainest of the four categories done so far: every function
// below is exactly the wma_* call, and only setScaleIntervals keeps any JNI
// work of its own.

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpEnabled(
    JNIEnv* env, jobject thiz, jboolean enabled) {
    wma_arp_set_enabled(g_wmaEngine, enabled == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsArpEnabled(
    JNIEnv* env, jobject thiz) {
    return wma_arp_is_enabled(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpPattern(
    JNIEnv* env, jobject thiz, jint patternId) {
    wma_arp_set_pattern(g_wmaEngine, patternId);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpSubdivision(
    JNIEnv* env, jobject thiz, jfloat beatsPerStep) {
    wma_arp_set_subdivision(g_wmaEngine, beatsPerStep);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpOctaveRange(
    JNIEnv* env, jobject thiz, jint octaves) {
    wma_arp_set_octave_range(g_wmaEngine, octaves);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpGateLength(
    JNIEnv* env, jobject thiz, jfloat gate) {
    wma_arp_set_gate_length(g_wmaEngine, gate);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpSwing(
    JNIEnv* env, jobject thiz, jfloat swing) {
    wma_arp_set_swing(g_wmaEngine, swing);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpLatch(
    JNIEnv* env, jobject thiz, jboolean latch) {
    wma_arp_set_latch(g_wmaEngine, latch == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpVelocity(
    JNIEnv* env, jobject thiz, jfloat velocity) {
    wma_arp_set_velocity(g_wmaEngine, velocity);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpVelocityVariation(
    JNIEnv* env, jobject thiz, jfloat variation) {
    wma_arp_set_velocity_variation(g_wmaEngine, variation);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpProbability(
    JNIEnv* env, jobject thiz, jfloat probability) {
    wma_arp_set_probability(g_wmaEngine, probability);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpScaleIntervals(
    JNIEnv* env, jobject thiz, jintArray intervals) {
    // The one arp function with Java-object handling, so the pinning stays here.
    if (!intervals) {
        return;
    }
    jint* data = env->GetIntArrayElements(intervals, nullptr);
    if (!data) {
        return;
    }
    wma_arp_set_scale_intervals(g_wmaEngine, data, env->GetArrayLength(intervals));
    env->ReleaseIntArrayElements(intervals, data, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpTouchActive(
    JNIEnv* env, jobject thiz, jboolean active) {
    wma_arp_set_touch_active(g_wmaEngine, active == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpBaseFrequency(
    JNIEnv* env, jobject thiz, jfloat frequency) {
    wma_arp_set_base_freq(g_wmaEngine, frequency);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetArpCurrentStep(
    JNIEnv* env, jobject thiz) {
    return wma_arp_get_current_step(g_wmaEngine);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetArpTotalSteps(
    JNIEnv* env, jobject thiz) {
    return wma_arp_get_total_steps(g_wmaEngine);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetArpRatchet(
    JNIEnv* env, jobject thiz, jboolean active) {
    wma_arp_set_ratchet(g_wmaEngine, active == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeRegenerateArpPattern(
    JNIEnv* env, jobject thiz) {
    wma_arp_regenerate(g_wmaEngine);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeIsArpGateOpen(
    JNIEnv* env, jobject thiz) {
    return wma_arp_is_gate_open(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

// ========== AUDIO LOOPER (Phase 11) ==========

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperPrepareTrack(
    JNIEnv* env, jobject thiz, jint trackIndex, jint lengthFrames, jint sampleRate) {
    return wma_looper_prepare_track(g_wmaEngine, trackIndex, lengthFrames, sampleRate);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperStartRecording(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    wma_looper_start_recording(g_wmaEngine, trackIndex);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperStopRecording(
    JNIEnv* env, jobject thiz) {
    wma_looper_stop_recording(g_wmaEngine);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperAbortRecording(
    JNIEnv* env, jobject thiz) {
    wma_looper_abort_recording(g_wmaEngine);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperStartOverdub(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    wma_looper_start_overdub(g_wmaEngine, trackIndex);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperStopAll(
    JNIEnv* env, jobject thiz) {
    wma_looper_stop_all(g_wmaEngine);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperPause(
    JNIEnv* env, jobject thiz) {
    wma_looper_pause(g_wmaEngine);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperResume(
    JNIEnv* env, jobject thiz) {
    wma_looper_resume(g_wmaEngine);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetRecordProgress(
    JNIEnv* env, jobject thiz) {
    return wma_looper_get_record_progress(g_wmaEngine);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetFreeLength(
    JNIEnv* env, jobject thiz, jboolean freeLength) {
    wma_looper_set_free_length(g_wmaEngine, freeLength == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetTrackMuted(
    JNIEnv* env, jobject thiz, jint trackIndex, jboolean muted) {
    wma_looper_set_track_muted(g_wmaEngine, trackIndex, muted == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetTrackPan(
    JNIEnv* env, jobject thiz, jint trackIndex, jfloat pan) {
    wma_looper_set_track_pan(g_wmaEngine, trackIndex, pan);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetTrackVolume(
    JNIEnv* env, jobject thiz, jint trackIndex, jfloat volume) {
    wma_looper_set_track_volume(g_wmaEngine, trackIndex, volume);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperClearTrack(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    wma_looper_clear_track(g_wmaEngine, trackIndex);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperClearAll(
    JNIEnv* env, jobject thiz) {
    wma_looper_clear_all(g_wmaEngine);
}

// Trim a track's buffer to its recorded length (frees unused capacity). UI/IO
// thread; safe no-op while recording/exporting. Returns true if trimmed.
JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperTrimTrack(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    return wma_looper_trim_track(g_wmaEngine, trackIndex) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetEnabled(
    JNIEnv* env, jobject thiz, jboolean enabled) {
    wma_looper_set_enabled(g_wmaEngine, enabled == JNI_TRUE);
}

// Lock-free queries (metering)

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetProgress(
    JNIEnv* env, jobject thiz) {
    return wma_looper_get_progress(g_wmaEngine);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetTrackPeakLevel(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    return wma_looper_get_track_peak(g_wmaEngine, trackIndex);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperIsTrackActive(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    return wma_looper_is_track_active(g_wmaEngine, trackIndex) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperIsPlaying(
    JNIEnv* env, jobject thiz) {
    return wma_looper_is_playing(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperIsRecording(
    JNIEnv* env, jobject thiz) {
    return wma_looper_is_recording(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetMasterLoopFrames(
    JNIEnv* env, jobject thiz) {
    return wma_looper_get_master_loop_frames(g_wmaEngine);
}

// Per-track playback control
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperPauseTrack(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    wma_looper_pause_track(g_wmaEngine, trackIndex);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperResumeTrack(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    wma_looper_resume_track(g_wmaEngine, trackIndex);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperIsTrackPlaying(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    return wma_looper_is_track_playing(g_wmaEngine, trackIndex) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetTrackProgress(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    return wma_looper_get_track_progress(g_wmaEngine, trackIndex);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetTrackLengthFrames(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    return wma_looper_get_track_length_frames(g_wmaEngine, trackIndex);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperResetTrackPlayHead(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    wma_looper_reset_track_playhead(g_wmaEngine, trackIndex);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSaveUndoSnapshot(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    return wma_looper_save_undo(g_wmaEngine, trackIndex) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperRestoreUndo(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    return wma_looper_restore_undo(g_wmaEngine, trackIndex) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperHasUndo(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    return wma_looper_has_undo(g_wmaEngine, trackIndex) ? JNI_TRUE : JNI_FALSE;
}

// Track waveform summary
JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetTrackWaveform(
    JNIEnv* env, jobject thiz, jint trackIndex, jfloatArray outBins, jint numBins) {
    if (outBins == nullptr || numBins <= 0) {
        return 0;
    }

    // Same reason as nativeGetWaveformSamples: wma_* takes a bare pointer and
    // cannot know how big the Java array is, so a numBins larger than the array
    // would write past the end. Its only caller allocates FloatArray(numBins)
    // and passes the same number, so nothing is broken today — this keeps the
    // two from having to stay in sync by convention.
    ScopedFloatArrayRW bins(env, outBins);
    if (!bins.isValid()) {
        return 0;
    }
    const int binsToFill = std::min(static_cast<int>(bins.size()), static_cast<int>(numBins));
    return wma_looper_get_track_waveform(g_wmaEngine, trackIndex, bins.get(), binsToFill);
}

// Track speed control
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetTrackSpeed(
    JNIEnv* env, jobject thiz, jint trackIndex, jfloat speed) {
    wma_looper_set_track_speed(g_wmaEngine, trackIndex, speed);
}

// Runtime capabilities (F3.2). budgetBytes uses jlong (64-bit) so a high tier
// can exceed 2 GB; 0 keeps the current default. maxTracks is clamped to the
// hardware ceiling (16); maxFreeSeconds is stored for the caller to read back.
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetCapabilities(
    JNIEnv* env, jobject thiz, jlong budgetBytes, jint maxTracks, jint maxFreeSeconds) {
    wma_looper_set_capabilities(g_wmaEngine, budgetBytes, maxTracks, maxFreeSeconds);
}

// Loop N times then auto-stop + emit onTrackCompleted (F3.4). plays <= 0 = infinite.
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetTrackPlayCount(
    JNIEnv* env, jobject thiz, jint trackIndex, jint plays) {
    wma_looper_set_track_play_count(g_wmaEngine, trackIndex, plays);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetTrackSpeed(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    return wma_looper_get_track_speed(g_wmaEngine, trackIndex);
}

// Per-track loop-seam profile: true = percussion (hard declick cut, no tail
// bleed), false = sustained (long crossfade + tail). Live & RT-safe.
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetTrackPercussionMode(
    JNIEnv* env, jobject thiz, jint trackIndex, jboolean percussion) {
    wma_looper_set_track_percussion_mode(g_wmaEngine, trackIndex, percussion == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperIsTrackPercussionMode(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    return wma_looper_is_track_percussion_mode(g_wmaEngine, trackIndex) ? JNI_TRUE : JNI_FALSE;
}

// Master volume (lock-free)
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetMasterVolume(
    JNIEnv* env, jobject thiz, jfloat volume) {
    wma_looper_set_master_volume(g_wmaEngine, volume);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetMasterVolume(
    JNIEnv* env, jobject thiz) {
    return wma_looper_get_master_volume(g_wmaEngine);
}

// Loop Region (lock-free)
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetTrackLoopRegion(
    JNIEnv* env, jobject thiz, jint trackIndex, jlong startFrame, jlong endFrame) {
    wma_looper_set_track_loop_region(g_wmaEngine, trackIndex, startFrame, endFrame);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperResetTrackLoopRegion(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    wma_looper_reset_track_loop_region(g_wmaEngine, trackIndex);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetTrackLoopStart(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    return wma_looper_get_track_loop_start(g_wmaEngine, trackIndex);
}

// Onset bounds for trimming a free take's leading/trailing silence.
// Returns (firstFrame << 32) | (lastFrame & 0xFFFFFFFF); last is exclusive.
JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperFindContentBounds(
    JNIEnv* env, jobject thiz, jint trackIndex, jfloat thresholdRatio) {
    // The packed (first << 32) | last encoding stays HERE: it exists because a
    // JNI call cannot hand back two ints, and it is this side's problem. The C
    // API deals in two out-params.
    int first = 0, last = 0;
    if (!wma_looper_find_content_bounds(g_wmaEngine, trackIndex, thresholdRatio,
                                       &first, &last)) {
        return 0;
    }
    return (static_cast<jlong>(first) << 32)
         | static_cast<jlong>(static_cast<uint32_t>(last));
}

// Onset detection for tempo derivation (free auto-loop, phase B). Returns an
// int[] of onset frame positions (ascending), capped at maxOnsets.
JNIEXPORT jintArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperDetectOnsets(
    JNIEnv* env, jobject thiz, jint trackIndex, jint maxOnsets,
    jint hopFrames, jfloat sensitivity) {
    // Sizing the Java array to the ACTUAL count stays here too — same family as
    // the waveform clamp: what knows about Java array lengths lives up top.
    if (maxOnsets <= 0) return env->NewIntArray(0);
    std::vector<jint> onsets(static_cast<size_t>(maxOnsets), 0);
    static_assert(sizeof(jint) == sizeof(int), "onset buffer is reinterpreted as int*");
    const int n = wma_looper_detect_onsets(g_wmaEngine, trackIndex,
                                          reinterpret_cast<int*>(onsets.data()),
                                          maxOnsets, hopFrames, sensitivity);
    jintArray result = env->NewIntArray(n);
    if (result && n > 0) env->SetIntArrayRegion(result, 0, n, onsets.data());
    return result;
}

// Bar-snap + seam-bake a free take's loop (Free-loop auto-sync, phases A+C).
JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperFinalizeFreeLoop(
    JNIEnv* env, jobject thiz, jint trackIndex, jint loopStart, jint loopEnd, jint tailFrames) {
    return wma_looper_finalize_free_loop(g_wmaEngine, trackIndex, loopStart, loopEnd,
                                        tailFrames) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetTrackLoopEnd(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    return wma_looper_get_track_loop_end(g_wmaEngine, trackIndex);
}

// Metronome click (lock-free trigger)
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperTriggerClick(
    JNIEnv* env, jobject thiz, jboolean isDownbeat) {
    wma_looper_trigger_click(g_wmaEngine, isDownbeat == JNI_TRUE);
}

// Input metering (lock-free) — peak level of the input stream, useful as a
// pre-record signal indicator. Returns 0 if no input source is active.
// Returns max of L/R channels in linear [0..1] range.
JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetInputPeak(
    JNIEnv* env, jobject thiz) {
    // Two existing C API calls plus the max, rather than a third way to ask
    // the same question. The 'peak' here is a decision (loudest of L/R), not
    // plumbing, and it is one line.
    const float l = wma_input_get_level_linear(g_wmaEngine, 0);
    const float r = wma_input_get_level_linear(g_wmaEngine, 1);
    return l > r ? l : r;
}

// Pre-roll: start recording with `preRollMs` of prior post-FX audio seeded
// at the start of the track. Eliminates the human-reaction gap when arming.
// preRollMs is clamped to [0, 1000].
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperStartRecordingWithPreRoll(
    JNIEnv* env, jobject thiz, jint trackIndex, jint preRollMs) {
    wma_looper_start_recording_with_pre_roll(g_wmaEngine, trackIndex, preRollMs);
}

// Tail capture configuration (preserves sustain at loop seam).
// Affects tracks prepared AFTER this call. Default 250 ms.
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetTailMs(
    JNIEnv* env, jobject thiz, jint ms) {
    wma_looper_set_tail_ms(g_wmaEngine, ms);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetTailMs(
    JNIEnv* env, jobject thiz) {
    return wma_looper_get_tail_ms(g_wmaEngine);
}

// Armed recording: schedule recording to start at the next bar boundary.
// Returns the absolute trigger frame (>=0), or -1 on failure.
JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperArmAtNextBar(
    JNIEnv* env, jobject thiz, jint trackIndex) {
    return wma_looper_arm_at_next_bar(g_wmaEngine, trackIndex);
}

// Armed recording with an explicit frame offset from the current Transport play
// position. Used for latency-compensated record start: the caller passes
// (countInFrames + roundTripLatencyFrames) so capture begins exactly that many
// frames after "now", placing the user's first downbeat (which lands late in the
// buffer by the round-trip latency) at loop frame 0. The anchor (play frame) is
// read on this thread atomically — no UI-thread jitter leaks into the trigger.
// Returns the absolute trigger frame (>=0), or -1 on failure.
JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperArmInFrames(
    JNIEnv* env, jobject thiz, jint trackIndex, jlong offsetFrames) {
    return wma_looper_arm_in_frames(g_wmaEngine, trackIndex, offsetFrames);
}

// Sync-armed overdub: phase-lock a new layer to the existing loop. Arms `track`
// to start at the loop reference's next boundary + `latencyFrames`, and tags the
// take so finalize phase-locks it to the reference (cancels round-trip latency).
// Returns the trigger frame, or -1 if no reference track is playing (caller falls
// back to a plain latency-armed start).
JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperArmSyncedToLoop(
    JNIEnv* env, jobject thiz, jint trackIndex, jlong latencyFrames) {
    return wma_looper_arm_synced_to_loop(g_wmaEngine, trackIndex, latencyFrames);
}

// Quantized variant: capture starts at the next multiple of `quantumFrames`
// inside the reference cycle (e.g. the next bar) instead of the next loop wrap,
// so a punch-in doesn't wait out the rest of the loop. The rotated start offset
// is cancelled at finalize, so playback still phase-locks to the reference.
// quantumFrames <= 0 behaves exactly like nativeLooperArmSyncedToLoop.
JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperArmSyncedToLoopQuantized(
    JNIEnv* env, jobject thiz, jint trackIndex, jlong latencyFrames, jint quantumFrames) {
    return wma_looper_arm_synced_to_loop_quantized(g_wmaEngine, trackIndex,
                                                  latencyFrames, quantumFrames);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperCancelArm(
    JNIEnv* env, jobject thiz) {
    wma_looper_cancel_arm(g_wmaEngine);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetArmedTrack(
    JNIEnv* env, jobject thiz) {
    return wma_looper_get_armed_track(g_wmaEngine);
}

// Loop quantization: prepare a track sized to N bars at current BPM/SR.
// Returns the loop length in frames (>=0) on success, or -1 on failure.
JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperPrepareTrackBars(
    JNIEnv* env, jobject thiz, jint trackIndex, jint bars, jint sampleRate) {
    return wma_looper_prepare_track_bars(g_wmaEngine, trackIndex, bars, sampleRate);
}

// ========== TRANSPORT (BPM, beats, RT-safe metronome scheduler) ==========

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportSetBeatsPerBar(
    JNIEnv* env, jobject thiz, jint beatsPerBar) {
    wma_transport_set_beats_per_bar(g_wmaEngine, beatsPerBar);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportGetBeatsPerBar(
    JNIEnv* env, jobject thiz) {
    // The no-engine default of 4 lives in the C API.
    return wma_transport_get_beats_per_bar(g_wmaEngine);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportFramesPerBeat(
    JNIEnv* env, jobject thiz) {
    return wma_transport_frames_per_beat(g_wmaEngine);
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportFramesPerBar(
    JNIEnv* env, jobject thiz, jint bars) {
    return wma_transport_frames_per_bar(g_wmaEngine, bars);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportStartMetronome(
    JNIEnv* env, jobject thiz, jint beats, jboolean firstIsDownbeat,
    jboolean everyBeatPattern) {
    wma_transport_start_metronome(g_wmaEngine, beats,
                                  firstIsDownbeat == JNI_TRUE,
                                  everyBeatPattern == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportStartMetronomeContinuous(
    JNIEnv* env, jobject thiz, jboolean everyBeatPattern) {
    wma_transport_start_metronome_continuous(g_wmaEngine,
                                             everyBeatPattern == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportIsMetronomeContinuous(
    JNIEnv* env, jobject thiz) {
    return wma_transport_is_metronome_continuous(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportStopMetronome(
    JNIEnv* env, jobject thiz) {
    wma_transport_stop_metronome(g_wmaEngine);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportIsMetronomeRunning(
    JNIEnv* env, jobject thiz) {
    return wma_transport_is_metronome_running(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeTransportGetRemainingBeats(
    JNIEnv* env, jobject thiz) {
    return wma_transport_get_remaining_beats(g_wmaEngine);
}

// Export / Import (NOT RT-safe — call from IO thread)
JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperExportMix(
    JNIEnv* env, jobject thiz, jstring filePath) {
    ScopedUtfChars path(env, filePath);
    return wma_looper_export_mix(g_wmaEngine, path.c_str()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperExportTrack(
    JNIEnv* env, jobject thiz, jint trackIndex, jstring filePath) {
    ScopedUtfChars path(env, filePath);
    return wma_looper_export_track(g_wmaEngine, trackIndex, path.c_str()) ? JNI_TRUE : JNI_FALSE;
}

// Session capture: write the FULL track buffer (ignoring loop region) at the
// given bit depth (16/24 PCM, 32 = float). 32 = lossless round-trip.
JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperCaptureTrack(
    JNIEnv* env, jobject thiz, jint trackIndex, jstring filePath, jint bitDepth) {
    // The 16/24/32 -> wav::BitDepth mapping used to be written out here, and in
    // the two export functions below. It lives in the C API now.
    ScopedUtfChars path(env, filePath);
    return wma_looper_capture_track(g_wmaEngine, trackIndex, path.c_str(), bitDepth)
        ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperImportTrack(
    JNIEnv* env, jobject thiz, jint trackIndex, jstring filePath, jint sampleRate) {
    ScopedUtfChars path(env, filePath);
    return wma_looper_import_track(g_wmaEngine, trackIndex, path.c_str(), sampleRate)
        ? JNI_TRUE : JNI_FALSE;
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
    WmaExportOptions opts = wma_looper_export_options_default();
    opts.bit_depth = bitDepth;
    opts.repeat_loops = repeatLoops;
    opts.count_in_beats = countInBeats;
    opts.apply_limiter = (applyLimiter == JNI_TRUE);
    opts.bpm = bpm;

    // The jstrings have to outlive the call, so the scoped holders live here and
    // the struct only borrows. A nullptr means "leave the metadata field empty",
    // which is the same thing the old pickStr lambda did by not assigning.
    ScopedUtfChars project(env, projectName);
    ScopedUtfChars artistChars(env, artist);
    ScopedUtfChars commentChars(env, comment);
    opts.project_name = project.c_str();
    opts.artist = artistChars.c_str();
    opts.comment = commentChars.c_str();

    ScopedUtfChars path(env, filePath);
    return wma_looper_export_mix_v2(g_wmaEngine, path.c_str(), &opts)
        ? JNI_TRUE : JNI_FALSE;
}

// Export each active track as a separate WAV file in `directory`.
// Returns number of stems written, or -1 on failure.
JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperExportStems(
    JNIEnv* env, jobject thiz,
    jstring directory, jint bitDepth, jint repeatLoops,
    jint countInBeats, jboolean applyLimiter, jint bpm) {
    WmaExportOptions opts = wma_looper_export_options_default();
    opts.bit_depth = bitDepth;
    opts.repeat_loops = repeatLoops;
    opts.count_in_beats = countInBeats;
    opts.apply_limiter = (applyLimiter == JNI_TRUE);
    opts.bpm = bpm;

    ScopedUtfChars dir(env, directory);
    return wma_looper_export_stems(g_wmaEngine, dir.c_str(), &opts);
}

JNIEXPORT jfloat JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetExportProgress(
    JNIEnv* env, jobject thiz) {
    return wma_looper_get_export_progress(g_wmaEngine);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperCancelExport(
    JNIEnv* env, jobject thiz) {
    wma_looper_cancel_export(g_wmaEngine);
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperSetExportSampleRate(
    JNIEnv* env, jobject thiz, jint sampleRate) {
    wma_looper_set_export_sample_rate(g_wmaEngine, sampleRate);
}

JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperIsExportInProgress(
    JNIEnv* env, jobject thiz) {
    return wma_looper_is_export_in_progress(g_wmaEngine) ? JNI_TRUE : JNI_FALSE;
}

// ========== TELEMETRY (lock-free counters for observability) ==========

JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetFramesDropped(
    JNIEnv* env, jobject thiz) {
    return wma_looper_get_frames_dropped(g_wmaEngine);
}
JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetExportsCompleted(
    JNIEnv* env, jobject thiz) {
    return wma_looper_get_exports_completed(g_wmaEngine);
}
JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetExportsFailed(
    JNIEnv* env, jobject thiz) {
    return wma_looper_get_exports_failed(g_wmaEngine);
}
JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetStemsWritten(
    JNIEnv* env, jobject thiz) {
    return wma_looper_get_stems_written(g_wmaEngine);
}
JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetArmedTriggered(
    JNIEnv* env, jobject thiz) {
    return wma_looper_get_armed_triggered(g_wmaEngine);
}
JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperResetTelemetry(
    JNIEnv* env, jobject thiz) {
    wma_looper_reset_telemetry(g_wmaEngine);
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
jmethodID                 g_looperOnTrackRecordProgress = nullptr;  // optional (QW-5)
jmethodID                 g_looperOnTrackCompleted = nullptr;       // optional (F3.4)

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
            case wm::LooperEvent::Type::RecordProgress:
                method = g_looperOnTrackRecordProgress; break;
            case wm::LooperEvent::Type::TrackCompleted:
                method = g_looperOnTrackCompleted; break;
        }
    }
    if (!listener || !method) return;

    if (ev.type == wm::LooperEvent::Type::PlayingChanged) {
        env->CallVoidMethod(listener, method,
                            static_cast<jint>(ev.trackIndex),
                            ev.value > 0.5f ? JNI_TRUE : JNI_FALSE);
    } else if (ev.type == wm::LooperEvent::Type::TrackCompleted) {
        // onTrackCompleted(trackIndex) — no value payload.
        env->CallVoidMethod(listener, method, static_cast<jint>(ev.trackIndex));
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

// ============================================================================
// The two looper entry points that do NOT delegate to the C API, and why.
//
// WA-2.6 closed the looper at 77/79. These two are the remainder, and they stay
// here on purpose rather than as unfinished work:
//
// Everything below is JNI machinery with no portable question inside it — global
// refs, GetObjectClass, GetMethodID with Java type signatures like "(IF)V", and
// the optional-method probing that lets an older LooperStateListener register
// without the callbacks added in QW-5 and F3.4. None of that means anything off
// the JVM.
//
// The one portable piece is `getLooperEventDispatcher().setSink(...)`, and the
// sink it installs is a C++ function that calls into Kotlin through JNIEnv. iOS
// will eventually want its own event callback — a `wma_looper_set_event_callback`
// taking a plain C function pointer and a user_data — but that is a NEW surface
// to design, not this migration: there is no existing behaviour to lift, because
// the existing behaviour is "call these Java methods". Left as a follow-up rather
// than half-invented here.
//
// Note also that these are the reason wma_looper_get_dropped_events() reads the
// dispatcher rather than the looper: the counter belongs to this queue.
// ============================================================================

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
    // Optional (QW-5): older listeners predate this callback. It has a Kotlin
    // default body, so a conforming implementation still exposes it, but we
    // must not fail registration if it's absent — clear any pending lookup
    // exception and dispatch RecordProgress only when present.
    g_looperOnTrackRecordProgress = env->GetMethodID(
        g_looperListenerClass, "onTrackRecordProgress", "(IF)V");
    if (!g_looperOnTrackRecordProgress && env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    // Optional (F3.4): older listeners predate onTrackCompleted (Kotlin default body).
    g_looperOnTrackCompleted = env->GetMethodID(
        g_looperListenerClass, "onTrackCompleted", "(I)V");
    if (!g_looperOnTrackCompleted && env->ExceptionCheck()) {
        env->ExceptionClear();
    }

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
    g_looperOnTrackRecordProgress = nullptr;
    g_looperOnTrackCompleted = nullptr;
}

JNIEXPORT jlong JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeLooperGetDroppedEvents(
    JNIEnv* env, jobject thiz) {
    return wma_looper_get_dropped_events(g_wmaEngine);
}

// ==================== USB Round-Trip Loopback Test (Fase 5) ====================
//
// A single global measurer installed on the running LibusbBackend via
// swapCallback. lifecycleMutex serializes start/cancel (which mutate the backend
// callback) and the terminal-phase auto-restore in poll(). The original callback
// (the engine) is stashed in g_rtPrevCallback and restored on
// COMPLETE/ERROR/cancel — a guaranteed round-trip so the stream never keeps the
// measurer as its callback.

namespace {
watermelon_audio::usb::RoundTripMeasurer g_rtMeasurer;
watermelon_audio::IAudioCallback* g_rtPrevCallback = nullptr;
bool g_rtInstalled = false;
std::mutex g_rtLifecycleMutex;

// Restore the backend's original callback. Caller holds g_rtLifecycleMutex.
void rtRestoreCallbackLocked() {
    if (!g_rtInstalled) return;
    auto* backend = watermelon_audio::BackendManager::getInstance().getLibusbBackend();
    if (backend) backend->swapCallback(g_rtPrevCallback);
    g_rtPrevCallback = nullptr;
    g_rtInstalled = false;
}
}  // namespace

// config floats: [0]=burstCount [1]=burstIntervalMs [2]=amplitude [3]=searchWindowMs
JNIEXPORT jboolean JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeUsbRoundTripStart(
        JNIEnv* env, jobject thiz, jfloatArray config) {
    std::lock_guard<std::mutex> lock(g_rtLifecycleMutex);
    if (g_rtInstalled) {
        LOGE("Round-trip: test already active");
        return JNI_FALSE;
    }
    auto* backend = watermelon_audio::BackendManager::getInstance().getLibusbBackend();
    if (!backend || !backend->isRunning()) {
        LOGE("Round-trip: USB backend not running");
        return JNI_FALSE;
    }
    const auto info = backend->getStreamInfo();
    if (!info.isFullDuplex) {
        LOGE("Round-trip: requires FULL_DUPLEX");
        return JNI_FALSE;  // Kotlin pre-check surfaces REQUIRES_FULL_DUPLEX
    }

    watermelon_audio::usb::RoundTripMeasurer::StartParams params;
    params.sampleRate = info.sampleRate > 0 ? info.sampleRate : 48000;
    params.outChannels = info.channelCount > 0 ? info.channelCount : 2;
    params.inChannels = params.outChannels;  // engine-facing layout is symmetric
    params.jitterBudgetMs = backend->getJitterBudgetMs();
    params.profile = backend->getLatencyProfileOrdinal();

    if (config) {
        const jsize n = env->GetArrayLength(config);
        jfloat buf[4] = {10.0f, 300.0f, 0.25f, 250.0f};
        env->GetFloatArrayRegion(config, 0, std::min<jsize>(n, 4), buf);
        params.config.burstCount = std::max(1, static_cast<int>(buf[0]));
        params.config.burstIntervalMs = std::max(50, static_cast<int>(buf[1]));
        params.config.amplitude = std::clamp(buf[2], 0.01f, 1.0f);
        params.config.searchWindowMs = std::max(50, static_cast<int>(buf[3]));
    }

    if (!g_rtMeasurer.start(params)) {
        LOGE("Round-trip: measurer.start() failed");
        return JNI_FALSE;
    }
    g_rtPrevCallback = backend->swapCallback(&g_rtMeasurer);
    g_rtInstalled = true;
    LOGI("Round-trip: installed measurer over live stream");
    return JNI_TRUE;
}

// poll floats [13]: [0]=state [1]=progressPct [2]=currentBurst [3]=medianMs
// [4]=madMs [5]=confidence [6]=softwareOutMs [7]=softwareInMs [8]=validBursts
// [9]=errorCode [10]=minMs [11]=maxMs [12]=sampleRate
JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeUsbRoundTripPoll(
        JNIEnv* env, jobject thiz) {
    std::lock_guard<std::mutex> lock(g_rtLifecycleMutex);
    const auto snap = g_rtMeasurer.poll();

    // Feed the software-latency (L7) average while actively measuring.
    if (g_rtInstalled &&
        snap.phase == watermelon_audio::usb::RoundTripMeasurer::Phase::MEASURING) {
        if (auto* backend =
                watermelon_audio::BackendManager::getInstance().getLibusbBackend()) {
            g_rtMeasurer.noteSoftwareLatency(backend->getOutputLatencyMs(),
                                             backend->getInputLatencyMs());
        }
    }

    jfloat v[13] = {0};
    v[0] = static_cast<float>(snap.phase);
    v[1] = snap.progressPct;
    v[2] = static_cast<float>(snap.currentBurst);
    v[3] = snap.result.medianMs;
    v[4] = snap.result.madMs;
    v[5] = snap.result.confidence;
    v[6] = snap.result.softwareOutputMs;
    v[7] = snap.result.softwareInputMs;
    v[8] = static_cast<float>(snap.result.validBursts);
    v[9] = static_cast<float>(snap.result.error);
    v[10] = snap.result.minMs;
    v[11] = snap.result.maxMs;
    v[12] = static_cast<float>(snap.result.sampleRate);

    // Guaranteed restore the moment the test reaches a terminal phase.
    using Phase = watermelon_audio::usb::RoundTripMeasurer::Phase;
    if (snap.phase == Phase::COMPLETE || snap.phase == Phase::ERROR) {
        rtRestoreCallbackLocked();
    }

    jfloatArray result = env->NewFloatArray(13);
    if (result) env->SetFloatArrayRegion(result, 0, 13, v);
    return result;
}

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeUsbRoundTripCancel(
        JNIEnv* env, jobject thiz) {
    std::lock_guard<std::mutex> lock(g_rtLifecycleMutex);
    rtRestoreCallbackLocked();  // restore FIRST, then tear the measurer down
    g_rtMeasurer.cancel();
    LOGI("Round-trip: cancelled");
}

// ==================== USB RT environment (App V §4, steps 2 & 5) ==========
//
// One poll for the USB Lab RT-env + jitter-budget steps. floats:
// [0]=dspSchedResult [1]=eventLoopSchedResult [2]=adpfState
// [3]=jitterBudgetMs [4]=convergedFloorMs [5]=latencyProfileOrdinal
// (ThreadUtils::SchedResult ordinals; adpfState 0/1/2.) All -1/0 if no USB.
JNIEXPORT jfloatArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetUsbRtEnv(
        JNIEnv* env, jobject thiz) {
    (void)thiz;
    jfloat v[6] = {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    auto* backend = watermelon_audio::BackendManager::getInstance().getLibusbBackend();
    if (backend) {
        v[0] = static_cast<float>(backend->getDspSchedResult());
        v[1] = static_cast<float>(backend->getEventLoopSchedResult());
        v[2] = static_cast<float>(backend->getAdpfState());
        v[3] = static_cast<float>(backend->getJitterBudgetMs());
        v[4] = static_cast<float>(backend->getConvergedFloorMs());
        v[5] = static_cast<float>(backend->getLatencyProfileOrdinal());
    }
    jfloatArray result = env->NewFloatArray(6);
    if (result) env->SetFloatArrayRegion(result, 0, 6, v);
    return result;
}

// ==================== Native Log Capture (App V §3.2) ====================

JNIEXPORT void JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeSetLogCaptureEnabled(
        JNIEnv* env, jobject thiz, jboolean enabled) {
    (void)env; (void)thiz;
    wma_log_capture_set_enabled(enabled == JNI_TRUE);
}

// Drain the captured lines since the last call (each "L/TAG: message").
JNIEXPORT jobjectArray JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeDrainCapturedLogs(
        JNIEnv* env, jobject thiz) {
    (void)thiz;
    jclass stringClass = env->FindClass("java/lang/String");
    if (!stringClass) return nullptr;

    // Look the class up *before* draining: the drain is destructive, so failing
    // after it would throw the lines away. The old code drained first.
    WmaLogBatch* batch = wma_log_capture_drain();
    const jsize count = static_cast<jsize>(wma_log_batch_count(batch));

    jobjectArray arr = env->NewObjectArray(count, stringClass, nullptr);
    if (arr) {
        for (jsize i = 0; i < count; ++i) {
            jstring s = env->NewStringUTF(wma_log_batch_line(batch, i));
            env->SetObjectArrayElement(arr, i, s);
            env->DeleteLocalRef(s);  // avoid overflowing the local ref table on big drains
        }
    }
    wma_log_batch_free(batch);
    return arr;
}

JNIEXPORT jint JNICALL
Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeGetLogCaptureDropped(
        JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return static_cast<jint>(wma_log_capture_dropped());
}

} // extern "C"
