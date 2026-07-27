/**
 * @file watermelon_audio.cpp
 * @brief Implementation of the Watermelon Audio C API.
 *
 * Each function is a thin wrapper around AudioEngine (and related classes).
 * The WmaEngine struct owns the AudioEngine instance.
 *
 * Version: 0.1.0 (Phase 0C — Audio Library Extraction)
 */

#include "watermelon_audio_internal.h"

#include "../core/AudioMode.h"
#include "../core/ModeConfigurations.h"
#include "../voice/VoiceTypes.h"
#include "../platform/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

/* ================================================================
 * Null-check macros
 * ================================================================ */

#define WMA_CHECK(e)       if (!(e) || !(e)->engine) return WMA_ERROR_NOT_INITIALIZED
#define WMA_CHECK_VOID(e)  if (!(e) || !(e)->engine) return
#define WMA_CHECK_VAL(e,v) if (!(e) || !(e)->engine) return (v)

/* ================================================================
 * Internal helpers
 * ================================================================ */

// Declared in watermelon_audio_internal.h so the JNI bridge shares this exact
// node instead of creating a second one — see the comment there.
bool wmaEnsureInputNode(WmaEngine* e) {
    if (!e) return false;
    std::lock_guard<std::mutex> lock(e->inputNodeMutex);
    if (!e->inputNode) {
        try {
            e->inputNode = std::make_shared<InputNode>();
            if (e->inputNode) {
                e->inputNode->prepare(48000, 4096);
            }
            return e->inputNode != nullptr;
        } catch (...) {
            return false;
        }
    }
    return true;
}

static bool ensureInputNode(WmaEngine* e) { return wmaEnsureInputNode(e); }

/* ================================================================
 * 1. Lifecycle
 * ================================================================ */

WmaEngine* wma_engine_create(void) {
    try {
        auto* e = new WmaEngine();

        // Create BackendManager owned by this engine (Phase 0D)
        e->backendManager = std::make_unique<watermelon_audio::BackendManager>();
        // Register as global instance so legacy code (JNI, AudioEngine) can find it
        watermelon_audio::BackendManager::setGlobalInstance(e->backendManager.get());

        e->engine = std::make_unique<AudioEngine>();
        return e;
    } catch (...) {
        return nullptr;
    }
}

void wma_engine_destroy(WmaEngine* engine) {
    if (!engine) return;
    if (engine->inputNode) {
        engine->inputNode->stopInputStream();
        engine->inputNode.reset();
    }
    if (engine->engine) {
        engine->engine->stop();
        engine->engine.reset();
    }
    // Clear global instance before destroying BackendManager
    if (engine->backendManager) {
        watermelon_audio::BackendManager::setGlobalInstance(nullptr);
        engine->backendManager.reset();
    }
    delete engine;
}

// The branch is on `>= 0`, not `> 0`: an explicit 0 is a real request (cut, no
// ramp) and has to reach startWithFade/stopWithFade, which is what the JNI has
// always done. Only WMA_FADE_DEFAULT falls through to the engine's own default.
// Branching on `> 0` made fade_time_ms = 0 mean "default" and lost the cut.

WmaResult wma_engine_start(WmaEngine* engine, int fade_time_ms) {
    WMA_CHECK(engine);
    bool ok;
    if (fade_time_ms >= 0) {
        ok = engine->engine->startWithFade(fade_time_ms);
    } else {
        ok = engine->engine->start();
    }
    return ok ? WMA_OK : WMA_ERROR_STREAM;
}

WmaResult wma_engine_stop(WmaEngine* engine, int fade_time_ms) {
    WMA_CHECK(engine);
    if (fade_time_ms >= 0) {
        engine->engine->stopWithFade(fade_time_ms);
    } else {
        engine->engine->stop();
    }
    return WMA_OK;
}

WmaResult wma_engine_pause(WmaEngine* engine, int fade_time_ms) {
    WMA_CHECK(engine);
    engine->engine->pauseWithFade(fade_time_ms);
    return WMA_OK;
}

WmaResult wma_engine_resume(WmaEngine* engine, int fade_time_ms) {
    WMA_CHECK(engine);
    engine->engine->resumeWithFade(fade_time_ms);
    return WMA_OK;
}

/* ================================================================
 * 2. State
 * ================================================================ */

int wma_get_engine_state(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getEngineState();
}

bool wma_is_paused(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->getIsPaused();
}

uint64_t wma_get_state_version(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getStateVersion();
}

bool wma_has_error(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->hasStreamError();
}

int wma_get_last_error_code(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getLastStreamErrorCode();
}

void wma_clear_error(WmaEngine* engine) {
    WMA_CHECK_VOID(engine);
    engine->engine->clearStreamError();
}

bool wma_has_init_failed(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->hasInitializationFailed();
}

bool wma_is_initialized(const WmaEngine* engine) {
    return engine != nullptr && engine->engine != nullptr;
}

bool wma_get_stream_info(const WmaEngine* engine,
                          int* sample_rate, int* buffer_size, float* latency_ms) {
    if (!engine || !engine->engine) return false;
    int32_t sr = 0, bs = 0;
    double lat = 0.0;
    bool ok = engine->engine->getStreamInfo(sr, bs, lat);
    if (ok) {
        if (sample_rate) *sample_rate = sr;
        if (buffer_size) *buffer_size = bs;
        if (latency_ms)  *latency_ms = static_cast<float>(lat);
    }
    return ok;
}

bool wma_is_using_reduced_buffers(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->isUsingReducedBuffers();
}

/* ================================================================
 * 3. Volume & Fade
 * ================================================================ */

void wma_set_master_volume(WmaEngine* engine, float volume) {
    WMA_CHECK_VOID(engine);
    volume = std::clamp(volume, 0.0f, 1.0f);
    engine->engine->setMasterVolume(volume);
}

float wma_get_master_volume(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 1.0f);
    return engine->engine->getMasterVolume();
}

float wma_get_fade_volume(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0.0f);
    return engine->engine->getCurrentFadeVolume();
}

float wma_get_target_fade_volume(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0.0f);
    return engine->engine->getTargetFadeVolume();
}

bool wma_is_fading(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->getIsFading();
}

float wma_get_fade_progress(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0.0f);
    return engine->engine->getFadeProgress();
}

/* ================================================================
 * 4. XY / Oscillator
 * ================================================================ */

void wma_set_xy(WmaEngine* engine, float x, float y) {
    WMA_CHECK_VOID(engine);
    x = std::clamp(x, 0.0f, 1.0f);
    y = std::clamp(y, 0.0f, 1.0f);
    engine->engine->updateXY(x, y);
}

void wma_set_frequency_amplitude(WmaEngine* engine, float frequency, float amplitude) {
    WMA_CHECK_VOID(engine);
    frequency = std::clamp(frequency, 20.0f, 20000.0f);
    amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    engine->engine->setFrequencyAndAmplitude(frequency, amplitude);
}

void wma_set_frequency_range(WmaEngine* engine, float min_hz, float max_hz) {
    WMA_CHECK_VOID(engine);
    if (!std::isfinite(min_hz) || !std::isfinite(max_hz) || max_hz <= min_hz) return;
    engine->engine->setFrequencyRange(min_hz, max_hz);
}

void wma_set_oscillator_type(WmaEngine* engine, int type_id) {
    WMA_CHECK_VOID(engine);
    if (type_id < 0 || type_id > 4) return;
    engine->engine->setOscillatorType(type_id);
}

/* ================================================================
 * 5. Engine (synth)
 * ================================================================ */

void wma_set_engine_type(WmaEngine* engine, int engine_type) {
    WMA_CHECK_VOID(engine);
    engine->engine->setEngineType(engine_type);
}

void wma_set_engine_param(WmaEngine* engine, int param_id, float value) {
    WMA_CHECK_VOID(engine);
    engine->engine->setEngineParameter(param_id, value);
}

int wma_get_engine_type(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getEngineType();
}

/* ================================================================
 * 6. SoundFont
 * ================================================================ */

bool wma_sf_load_path(WmaEngine* engine, const char* path) {
    WMA_CHECK_VAL(engine, false);
    if (!path) return false;
    return engine->engine->loadSoundFontFromPath(path);
}

bool wma_sf_load_fd(WmaEngine* engine, int fd, int64_t offset, int64_t length) {
    WMA_CHECK_VAL(engine, false);
    if (fd < 0 || length <= 0 || offset < 0) return false;
    return engine->engine->loadSoundFontFromFd(fd, offset, length);
}

bool wma_sf_load_data(WmaEngine* engine, const void* data, int size) {
    WMA_CHECK_VAL(engine, false);
    if (!data || size <= 0) return false;
    return engine->engine->loadSoundFont(data, size);
}

void wma_sf_unload(WmaEngine* engine) {
    WMA_CHECK_VOID(engine);
    engine->engine->unloadSoundFont();
}

void wma_sf_set_preset(WmaEngine* engine, int preset_index) {
    WMA_CHECK_VOID(engine);
    engine->engine->setSoundFontPreset(preset_index);
}

int wma_sf_get_preset_count(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getSoundFontPresetCount();
}

const char* wma_sf_get_preset_name(const WmaEngine* engine, int preset_index) {
    WMA_CHECK_VAL(engine, nullptr);
    return engine->engine->getSoundFontPresetName(preset_index);
}

bool wma_sf_is_loaded(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->isSoundFontLoaded();
}

bool wma_sf_get_preset_key_range(const WmaEngine* engine, int preset_index,
                                  int* out_min_key, int* out_max_key) {
    WMA_CHECK_VAL(engine, false);
    int minKey = 0, maxKey = 127;
    bool ok = engine->engine->getSoundFontPresetKeyRange(preset_index, minKey, maxKey);
    if (ok) {
        if (out_min_key) *out_min_key = minKey;
        if (out_max_key) *out_max_key = maxKey;
    }
    return ok;
}

bool wma_sf_get_preset_bank_program(const WmaEngine* engine, int preset_index,
                                     int* out_bank, int* out_program) {
    WMA_CHECK_VAL(engine, false);
    // -1 rather than 0: bank 0 / program 0 is a real preset (usually the piano),
    // so a caller reading the out params after a false return would take the
    // default for an answer. Mirrors what the JNI does before building its array.
    int bank = -1, program = -1;
    bool ok = engine->engine->getSoundFontPresetBankProgram(preset_index, bank, program);
    if (ok) {
        if (out_bank)    *out_bank = bank;
        if (out_program) *out_program = program;
    }
    return ok;
}

void wma_sf_note_on(WmaEngine* engine, int touch_id, int midi_note, float velocity) {
    WMA_CHECK_VOID(engine);
    engine->engine->sfNoteOn(touch_id, midi_note, velocity);
}

void wma_sf_note_off(WmaEngine* engine, int touch_id) {
    WMA_CHECK_VOID(engine);
    engine->engine->sfNoteOff(touch_id);
}

void wma_sf_note_off_all(WmaEngine* engine) {
    WMA_CHECK_VOID(engine);
    engine->engine->sfNoteOffAll();
}

void wma_sf_note_off_all_except(WmaEngine* engine, int keep_touch_id) {
    WMA_CHECK_VOID(engine);
    engine->engine->sfNoteOffAllExcept(keep_touch_id);
}

/* ================================================================
 * 7. Voice Filter
 * ================================================================ */

void wma_voice_filter_set_enabled(WmaEngine* engine, bool enabled) {
    WMA_CHECK_VOID(engine);
    engine->engine->setVoiceFilterEnabled(enabled);
}

void wma_voice_filter_set_cutoff(WmaEngine* engine, float hz) {
    WMA_CHECK_VOID(engine);
    engine->engine->setVoiceFilterCutoff(hz);
}

void wma_voice_filter_set_resonance(WmaEngine* engine, float q) {
    WMA_CHECK_VOID(engine);
    engine->engine->setVoiceFilterResonance(q);
}

void wma_voice_filter_set_mode(WmaEngine* engine, int mode) {
    WMA_CHECK_VOID(engine);
    engine->engine->setVoiceFilterMode(mode);
}

/* ================================================================
 * 8. Effects
 * ================================================================ */

int wma_effect_add(WmaEngine* engine, int type_id) {
    if (!engine || !engine->engine) return WMA_ERROR_NOT_INITIALIZED;
    if (type_id < 0 || type_id >= static_cast<int>(EFFECT_TYPE_COUNT)) {
        return WMA_ERROR_INVALID_EFFECT_TYPE;
    }
    try {
        bool success = engine->engine->addEffect(static_cast<EffectType>(type_id));
        if (!success) return WMA_ERROR_EFFECT_CHAIN_FULL;
        return static_cast<int>(engine->engine->getNumEffects()) - 1;
    } catch (const std::bad_alloc&) {
        return WMA_ERROR_MEMORY;
    } catch (...) {
        return WMA_ERROR_UNKNOWN;
    }
}

WmaResult wma_effect_remove(WmaEngine* engine, int index) {
    WMA_CHECK(engine);
    if (index < 0 || static_cast<size_t>(index) >= engine->engine->getNumEffects()) {
        return WMA_ERROR_INVALID_EFFECT_INDEX;
    }
    try {
        engine->engine->removeEffect(static_cast<size_t>(index));
        return WMA_OK;
    } catch (...) {
        return WMA_ERROR_UNKNOWN;
    }
}

WmaResult wma_effect_clear_all(WmaEngine* engine) {
    WMA_CHECK(engine);
    try {
        engine->engine->clearAllEffects();
        return WMA_OK;
    } catch (...) {
        return WMA_ERROR_UNKNOWN;
    }
}

WmaResult wma_effect_set_param(WmaEngine* engine, int index, int param_id, float value) {
    WMA_CHECK(engine);
    if (index < 0 || static_cast<size_t>(index) >= engine->engine->getNumEffects()) {
        return WMA_ERROR_INVALID_EFFECT_INDEX;
    }
    if (param_id < 0) return WMA_ERROR_INVALID_PARAMETER_ID;
    if (!std::isfinite(value)) return WMA_ERROR_PARAMETER_OUT_OF_RANGE;
    try {
        engine->engine->setParameter(static_cast<size_t>(index), param_id, value);
        return WMA_OK;
    } catch (...) {
        return WMA_ERROR_UNKNOWN;
    }
}

float wma_effect_get_param(const WmaEngine* engine, int index, int param_id) {
    WMA_CHECK_VAL(engine, 0.0f);
    if (index < 0 || static_cast<size_t>(index) >= engine->engine->getNumEffects()) {
        return 0.0f;
    }
    try {
        return engine->engine->getParameter(static_cast<size_t>(index), param_id);
    } catch (...) {
        return 0.0f;
    }
}

WmaResult wma_effect_set_params_batch(WmaEngine* engine, int index,
                                       const int* param_ids, const float* values,
                                       int count) {
    WMA_CHECK(engine);
    if (index < 0 || static_cast<size_t>(index) >= engine->engine->getNumEffects()) {
        return WMA_ERROR_INVALID_EFFECT_INDEX;
    }
    if (!param_ids || !values || count <= 0) return WMA_OK;
    try {
        // NOT a loop over setParameter: that bumps the state version once per
        // parameter, and the Kotlin synchronizer emits on every bump — a scene
        // load would be observed as N partial states. This is AUD-6, which the
        // JNI fixed years ago and this function quietly kept, because it was
        // written as a transcription of the individual setter rather than of
        // the batch one. setParametersBatch bumps exactly once, at the end.
        std::vector<int> effectIndices(static_cast<size_t>(count), index);
        engine->engine->setParametersBatch(effectIndices.data(), param_ids, values,
                                           static_cast<size_t>(count));
        return WMA_OK;
    } catch (...) {
        return WMA_ERROR_UNKNOWN;
    }
}

WmaResult wma_effect_set_params_multi(WmaEngine* engine,
                                       const int* effect_indices,
                                       const int* param_ids,
                                       const float* values,
                                       int count) {
    WMA_CHECK(engine);
    if (!effect_indices || !param_ids || !values || count <= 0) return WMA_OK;
    try {
        // No index guard here on purpose: setParametersBatch skips out-of-range
        // effects itself. Rejecting the whole call because one entry in a scene
        // points at an effect that is no longer in the chain would lose the
        // other N-1 updates.
        engine->engine->setParametersBatch(effect_indices, param_ids, values,
                                           static_cast<size_t>(count));
        return WMA_OK;
    } catch (...) {
        return WMA_ERROR_UNKNOWN;
    }
}

WmaResult wma_effect_set_bypass(WmaEngine* engine, int index, bool bypass) {
    WMA_CHECK(engine);
    if (index < 0 || static_cast<size_t>(index) >= engine->engine->getNumEffects()) {
        return WMA_ERROR_INVALID_EFFECT_INDEX;
    }
    try {
        engine->engine->setBypass(static_cast<size_t>(index), bypass);
        return WMA_OK;
    } catch (...) {
        return WMA_ERROR_UNKNOWN;
    }
}

bool wma_effect_is_bypassed(const WmaEngine* engine, int index) {
    WMA_CHECK_VAL(engine, false);
    if (index < 0 || static_cast<size_t>(index) >= engine->engine->getNumEffects()) {
        return false;
    }
    try {
        return engine->engine->isBypassed(static_cast<size_t>(index));
    } catch (...) {
        return false;
    }
}

WmaResult wma_effect_set_global_bypass(WmaEngine* engine, bool bypass) {
    WMA_CHECK(engine);
    try {
        engine->engine->setEffectsBypass(bypass);
        return WMA_OK;
    } catch (...) {
        return WMA_ERROR_UNKNOWN;
    }
}

bool wma_effect_is_global_bypassed(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, false);
    try {
        return engine->engine->isEffectsBypassed();
    } catch (...) {
        return false;
    }
}

WmaResult wma_effect_reorder(WmaEngine* engine, int from_index, int to_index) {
    WMA_CHECK(engine);
    size_t chainSize = engine->engine->getNumEffects();
    if (from_index < 0 || static_cast<size_t>(from_index) >= chainSize ||
        to_index < 0 || static_cast<size_t>(to_index) >= chainSize) {
        return WMA_ERROR_INVALID_EFFECT_INDEX;
    }
    try {
        engine->engine->reorderEffects(static_cast<size_t>(from_index),
                                       static_cast<size_t>(to_index));
        return WMA_OK;
    } catch (...) {
        return WMA_ERROR_UNKNOWN;
    }
}

int wma_effect_chain_size(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0);
    return static_cast<int>(engine->engine->getNumEffects());
}

int wma_effect_get_type(const WmaEngine* engine, int index) {
    WMA_CHECK_VAL(engine, -1);
    if (index < 0 || static_cast<size_t>(index) >= engine->engine->getNumEffects()) {
        return -1;
    }
    try {
        return static_cast<int>(engine->engine->getEffectType(static_cast<size_t>(index)));
    } catch (...) {
        return -1;
    }
}

void wma_set_bpm(WmaEngine* engine, float bpm) {
    WMA_CHECK_VOID(engine);
    engine->engine->setBpm(bpm);
}

float wma_get_bpm(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 120.0f);
    return engine->engine->getBpm();
}

/* ================================================================
 * 9. Routing
 * ================================================================ */

void wma_set_routing_mode(WmaEngine* engine, int mode) {
    WMA_CHECK_VOID(engine);
    if (mode < 0 || mode > 5) return;
    engine->engine->setRoutingMode(static_cast<RoutingMode>(mode));
}

int wma_get_routing_mode(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getRoutingMode();
}

void wma_set_parallel_mix(WmaEngine* engine, float mix) {
    WMA_CHECK_VOID(engine);
    engine->engine->setParallelMix(mix);
}

void wma_set_feedback_amount(WmaEngine* engine, float amount) {
    WMA_CHECK_VOID(engine);
    engine->engine->setFeedbackAmount(amount);
}

/* ================================================================
 * 10. Modulator
 * ================================================================ */

WmaResult wma_set_modulator_type(WmaEngine* engine, int type_id) {
    WMA_CHECK(engine);
    if (type_id < 0 || type_id > 7) return WMA_ERROR_INVALID_PARAMETER_ID;
    engine->engine->setModulatorType(type_id);
    return WMA_OK;
}

WmaResult wma_set_modulator_param(WmaEngine* engine, int param_id, float value) {
    WMA_CHECK(engine);
    if (param_id < 0) return WMA_ERROR_INVALID_PARAMETER_ID;
    if (!std::isfinite(value)) return WMA_ERROR_PARAMETER_OUT_OF_RANGE;
    engine->engine->setModulatorParameter(param_id, value);
    return WMA_OK;
}

/* ================================================================
 * 11. Audio Mode
 * ================================================================ */

namespace {

/**
 * Bring the InputNode up for a mode that needs microphone audio.
 *
 * The USB branch is not Android-specific by accident of where it was written —
 * it is backend-specific, and asking the BackendManager keeps it that way. On a
 * backend that delivers input through the render callback (LibusbBackend today)
 * there must be NO separate node-level stream: the data already arrives via
 * IAudioCallback::onAudioReady(inputData), and a second stream would fight it.
 * On iOS getCurrentType() is never LIBUSB, so this reads as "start the stream"
 * without a single #ifdef.
 */
void wmaAttachInputForMode(WmaEngine* engine) {
    if (!ensureInputNode(engine) || !engine->inputNode) {
        WMA_LOGW("wma_set_audio_mode: no input node — input will be silent");
        return;
    }

    const auto backendType = watermelon_audio::BackendManager::getInstance().getCurrentType();
    const bool backendDeliversInput =
        (backendType == watermelon_audio::BackendType::LIBUSB);

    if (backendDeliversInput) {
        if (engine->inputNode->isInputStreamRunning()) {
            engine->inputNode->stopInputStream();
        }
    } else if (!engine->inputNode->isInputStreamRunning()) {
        engine->inputNode->startInputStream();
    }

    engine->inputNode->setMonitoringEnabled(true);
    engine->engine->setInputNode(engine->inputNode);

    // Kept from the JNI: on a device this line is how you tell "the mic is not
    // working" apart from "the mode never attached it", and the smoke reads it
    // out of logcat.
    wma::logMessage(wma::LogLevel::INFO, "WMA_AUDIT",
        "SET_MODE_INPUT_ATTACHED: backendType=%d, backendDeliversInput=%d, "
        "monEnabled=%d, inputStreamRunning=%d",
        static_cast<int>(backendType),
        backendDeliversInput,
        engine->inputNode->isMonitoringEnabled(),
        engine->inputNode->isInputStreamRunning());
}

}  // namespace

void wma_set_audio_mode(WmaEngine* engine, int mode) {
    WMA_CHECK_VOID(engine);
    if (mode < 0 || mode > 2) {
        WMA_LOGE("wma_set_audio_mode: invalid mode %d", mode);
        return;
    }

    auto audioMode = static_cast<watermelon_audio::AudioMode>(mode);

    try {
        switch (audioMode) {
            case watermelon_audio::AudioMode::CHAOS_PAD:
                engine->engine->setOscillatorEnabled(true);
                engine->engine->setVocoderCarrierSource(false);
                engine->engine->setVocoderModulatorSource(engine->inputNode != nullptr);
                if (engine->inputNode) {
                    engine->inputNode->setMonitoringEnabled(false);
                }
                break;

            case watermelon_audio::AudioMode::INPUT_FX:
                // BEFORE flipping oscillatorEnabled, and this ordering is the
                // point: the audio thread services the request at the top of the
                // next onAudioReady(), zero-filling the chain's scratch and
                // feedback buffers and resetting every effect. Without it,
                // chaos_pad's reverb tail and delay feedback bleed into the first
                // blocks of microphone processing as a burst — and the longer the
                // user stayed in chaos_pad, the louder it is.
                engine->engine->requestResetEffectChain();
                engine->engine->setOscillatorEnabled(false);
                engine->engine->setVocoderCarrierSource(true);
                wmaAttachInputForMode(engine);
                break;

            case watermelon_audio::AudioMode::MIX:
                engine->engine->setOscillatorEnabled(true);
                engine->engine->setVocoderCarrierSource(true);
                wmaAttachInputForMode(engine);
                break;
        }

        engine->currentMode.store(static_cast<int>(audioMode), std::memory_order_release);
    } catch (const std::exception& e) {
        WMA_LOGE("wma_set_audio_mode: exception: %s", e.what());
    } catch (...) {
        WMA_LOGE("wma_set_audio_mode: unknown exception");
    }
}

const char* wma_get_mode_name(int mode) {
    return watermelon_audio::ModeUtils::getModeName(
        static_cast<watermelon_audio::AudioMode>(mode));
}

int wma_get_audio_mode(const WmaEngine* engine) {
    if (!engine) return 0;
    return engine->currentMode.load(std::memory_order_acquire);
}

bool wma_is_in_mode_transition(const WmaEngine* engine) {
    if (!engine) return false;
    return engine->modeTransitionInProgress.load(std::memory_order_relaxed);
}

float wma_get_mode_transition_progress(const WmaEngine* engine) {
    if (!engine) return 0.0f;
    return engine->modeTransitionProgress.load(std::memory_order_relaxed);
}

bool wma_mode_requires_input(int mode) {
    return watermelon_audio::ModeUtils::requiresInput(static_cast<watermelon_audio::AudioMode>(mode));
}

/* ================================================================
 * 12. Input
 * ================================================================ */

/*
 * Two ways for audio to get in, tried in order — and no platform #if anywhere,
 * because the fallthrough itself is the platform test.
 *
 *   1. InputNode's own capture backend. Oboe on Android: it opens a dedicated
 *      input stream and returns true, so Android stops right here and behaves
 *      exactly as it always has.
 *
 *   2. The audio backend carrying input through onAudioReady's inputData. That
 *      is the Apple path (CoreAudioBackend's AVAudioSinkNode) and the USB one.
 *      InputNode has no capture backend there, so step 1 returns false and the
 *      request falls through to here.
 *
 * AudioEngine already routes a non-null inputData to direct INPUT_FX or to
 * InputNode::feedExternalInput(), so both roads end in the same DSP.
 */
bool wma_input_start(WmaEngine* engine) {
    if (!engine) return false;
    if (!ensureInputNode(engine)) return false;

    if (engine->inputNode->startInputStream()) {
        return true;
    }

    if (!engine->backendManager) return false;

    // The caller explicitly asked for the microphone, so a stream reopen is
    // authorized: every backend decides on capture when it opens. The mode path
    // does NOT get this permission — see BackendManager::requestCapture.
    return engine->backendManager->requestCapture(
        watermelon_audio::BackendManager::CaptureRequester::INPUT_NODE,
        /*want=*/true,
        /*allowRestart=*/true);
}

void wma_input_stop(WmaEngine* engine) {
    if (!engine) return;

    if (engine->inputNode) {
        engine->inputNode->stopInputStream();
    }

    // Withdraw the request either way: on the backend-capture path there is no
    // node-level stream to stop, and leaving the request standing would make the
    // next reopen turn the microphone back on by itself.
    if (engine->backendManager) {
        engine->backendManager->requestCapture(
            watermelon_audio::BackendManager::CaptureRequester::INPUT_NODE,
            /*want=*/false,
            /*allowRestart=*/false);
    }
}

bool wma_input_is_running(const WmaEngine* engine) {
    if (!engine) return false;

    if (engine->inputNode && engine->inputNode->isInputStreamRunning()) {
        return true;
    }

    return engine->backendManager && engine->backendManager->isCaptureLive();
}

void wma_input_set_source(WmaEngine* engine, int source) {
    if (!engine || !engine->inputNode) return;
    if (source < 0 || source > 2) {
        WMA_LOGE("wma_input_set_source: invalid source %d", source);
        return;
    }
    // Switching source tears the stream down and brings it back up, so this is
    // the one input setter that can throw. The guard used to live in the JNI;
    // it belongs here instead, because a C++ exception unwinding into
    // Kotlin/Native is not a caught error, it is a dead process.
    try {
        engine->inputNode->setInputSource(static_cast<InputSource>(source));
    } catch (const std::exception& e) {
        WMA_LOGE("wma_input_set_source: exception: %s", e.what());
    } catch (...) {
        WMA_LOGE("wma_input_set_source: unknown exception");
    }
}

int wma_input_get_source(const WmaEngine* engine) {
    if (!engine || !engine->inputNode) return 0;
    return static_cast<int>(engine->inputNode->getInputSource());
}

void wma_input_set_gain(WmaEngine* engine, float gain_db) {
    if (!engine || !engine->inputNode) return;
    engine->inputNode->setInputGain(gain_db);
}

float wma_input_get_gain(const WmaEngine* engine) {
    if (!engine || !engine->inputNode) return 0.0f;
    return engine->inputNode->getInputGain();
}

void wma_input_set_noise_gate(WmaEngine* engine, bool enabled) {
    if (!engine || !engine->inputNode) return;
    engine->inputNode->setNoiseGateEnabled(enabled);
}

bool wma_input_is_noise_gate_enabled(const WmaEngine* engine) {
    if (!engine || !engine->inputNode) return false;
    return engine->inputNode->isNoiseGateEnabled();
}

void wma_input_set_noise_gate_threshold(WmaEngine* engine, float threshold_db) {
    if (!engine || !engine->inputNode) return;
    engine->inputNode->setNoiseGateThreshold(threshold_db);
}

bool wma_input_is_noise_gate_open(const WmaEngine* engine) {
    if (!engine || !engine->inputNode) return false;
    return engine->inputNode->isNoiseGateOpen();
}

float wma_input_get_level(const WmaEngine* engine, int channel) {
    // -100 dB, not 0: with no node there is no signal, and 0 dB is full scale.
    if (!engine || !engine->inputNode) return -100.0f;
    return engine->inputNode->getInputLevel(channel);
}

float wma_input_get_level_linear(const WmaEngine* engine, int channel) {
    if (!engine || !engine->inputNode) return 0.0f;
    return engine->inputNode->getInputLevelLinear(channel);
}

bool wma_input_is_clipping(const WmaEngine* engine) {
    if (!engine || !engine->inputNode) return false;
    return engine->inputNode->isClipping();
}

float wma_input_get_latency_ms(const WmaEngine* engine) {
    if (!engine || !engine->inputNode) return 0.0f;
    return engine->inputNode->getInputLatencyMs();
}

bool wma_input_get_metering_snapshot(const WmaEngine* engine, float* out_values) {
    if (!engine || !engine->inputNode || !out_values) return false;
    const auto& node = *engine->inputNode;
    // Order is contractual — see the header, and InputStateManager on the Kotlin
    // side, which indexes into this.
    out_values[0] = node.getInputLevel(0);
    out_values[1] = node.getInputLevel(1);
    out_values[2] = node.getInputLevelLinear(0);
    out_values[3] = node.getInputLevelLinear(1);
    out_values[4] = node.isClipping() ? 1.0f : 0.0f;
    out_values[5] = node.isNoiseGateOpen() ? 1.0f : 0.0f;
    out_values[6] = node.getInputLatencyMs();
    return true;
}

void wma_input_set_monitoring(WmaEngine* engine, bool enabled) {
    if (!engine || !engine->inputNode) return;
    engine->inputNode->setMonitoringEnabled(enabled);
}

bool wma_input_is_monitoring_enabled(const WmaEngine* engine) {
    if (!engine || !engine->inputNode) return false;
    return engine->inputNode->isMonitoringEnabled();
}

void wma_input_set_monitoring_volume(WmaEngine* engine, float volume) {
    if (!engine || !engine->inputNode) return;
    engine->inputNode->setMonitoringVolume(std::clamp(volume, 0.0f, 1.0f));
}

float wma_input_get_monitoring_volume(const WmaEngine* engine) {
    if (!engine || !engine->inputNode) return 0.0f;
    return engine->inputNode->getMonitoringVolume();
}

void wma_input_release(WmaEngine* engine) {
    if (!engine) return;
    // Under the mutex, like wmaEnsureInputNode: creating and destroying the node
    // race against each other, and the JNI's releaseInputNode() (which used to
    // take this lock itself) now delegates here.
    std::lock_guard<std::mutex> lock(engine->inputNodeMutex);
    if (engine->inputNode) {
        engine->inputNode->stopInputStream();
        engine->inputNode.reset();
    }
}

/* ================================================================
 * 13. Dual Touch
 * ================================================================ */

void wma_set_dual_touch_mode(WmaEngine* engine, bool enabled) {
    WMA_CHECK_VOID(engine);
    engine->engine->setDualTouchMode(enabled);
}

bool wma_get_dual_touch_mode(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->getDualTouchMode();
}

void wma_set_dual_touch(WmaEngine* engine,
                         float x1, float y1, float freq1, float amp1, float pressure1,
                         float x2, float y2, float freq2, float amp2, float pressure2,
                         float distance, float angle) {
    WMA_CHECK_VOID(engine);
    engine->engine->updateDualTouch(x1, y1, freq1, amp1, pressure1,
                                    x2, y2, freq2, amp2, pressure2,
                                    distance, angle);
}

void wma_set_dual_touch_mix_mode(WmaEngine* engine, int mode_id) {
    WMA_CHECK_VOID(engine);
    if (mode_id < 0 || mode_id > 5) return;
    engine->engine->setDualTouchMixMode(static_cast<DualTouchMixMode>(mode_id));
}

void wma_set_secondary_oscillator_type(WmaEngine* engine, int type_id) {
    WMA_CHECK_VOID(engine);
    engine->engine->setSecondaryOscillatorType(type_id);
}

/* ================================================================
 * 14. Voice System
 * ================================================================ */

void wma_voice_enable(WmaEngine* engine, bool enable) {
    WMA_CHECK_VOID(engine);
    engine->engine->enableVoiceSystem(enable);
}

bool wma_voice_is_enabled(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->isVoiceSystemEnabled();
}

void wma_voice_update_multi_touch(WmaEngine* engine,
                                   const float* touch_data, int count) {
    WMA_CHECK_VOID(engine);

    if (count <= 0 || !touch_data) {
        engine->engine->updateMultiTouch(nullptr, 0);
        return;
    }

    const int TOUCH_STRIDE = 6;
    int maxTouches = std::min(count, 4);

    /* Stack-allocate for small arrays (max 4 touches). */
    voice::TouchData touches[4];
    for (int i = 0; i < maxTouches; i++) {
        int offset = i * TOUCH_STRIDE;
        touches[i].x = touch_data[offset + 0];
        touches[i].y = touch_data[offset + 1];
        touches[i].frequency = touch_data[offset + 2];
        touches[i].amplitude = touch_data[offset + 3];
        touches[i].pressure = touch_data[offset + 4];
        touches[i].pointerId = static_cast<int>(touch_data[offset + 5]);
        touches[i].active = true;
    }

    engine->engine->updateMultiTouch(touches, maxTouches);
}

int wma_voice_get_active_count(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getActiveVoiceCount();
}

void wma_voice_set_max(WmaEngine* engine, int max_voices) {
    WMA_CHECK_VOID(engine);
    engine->engine->setMaxVoices(max_voices);
}

void wma_voice_set_stealing_strategy(WmaEngine* engine, int strategy) {
    WMA_CHECK_VOID(engine);
    engine->engine->setVoiceStealingStrategy(strategy);
}

void wma_voice_trigger_chord(WmaEngine* engine,
                              const float* frequencies, int count,
                              float amplitude, int oscillator_type) {
    WMA_CHECK_VOID(engine);
    if (!frequencies || count <= 0) return;
    engine->engine->triggerChordNotes(frequencies, count, amplitude, oscillator_type);
}

void wma_voice_update_chord(WmaEngine* engine,
                             const float* frequencies, int count,
                             float amplitude) {
    WMA_CHECK_VOID(engine);
    if (!frequencies || count <= 0) return;
    engine->engine->updateChordNotes(frequencies, count, amplitude);
}

void wma_voice_release_chord(WmaEngine* engine) {
    WMA_CHECK_VOID(engine);
    engine->engine->releaseChordNotes();
}

/* ================================================================
 * 15. Vocoder
 * ================================================================ */

void wma_vocoder_set_carrier_source(WmaEngine* engine, bool use_internal) {
    WMA_CHECK_VOID(engine);
    engine->engine->setVocoderCarrierSource(use_internal);
}

void wma_vocoder_set_carrier_freq(WmaEngine* engine, float frequency) {
    WMA_CHECK_VOID(engine);
    frequency = std::clamp(frequency, 20.0f, 2000.0f);
    engine->engine->setVocoderCarrierFrequency(frequency);
}

bool wma_vocoder_has_effect(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->hasVocoderEffect();
}

void wma_vocoder_set_modulator_source(WmaEngine* engine, bool use_external) {
    WMA_CHECK_VOID(engine);
    engine->engine->setVocoderModulatorSource(use_external);
}

/* ================================================================
 * 16. Backend
 * ================================================================ */

void wma_set_use_backend_manager(WmaEngine* engine, bool use) {
    WMA_CHECK_VOID(engine);
    engine->engine->setUseBackendManager(use);
}

bool wma_select_backend(int backend_id) {
    auto& manager = watermelon_audio::BackendManager::getInstance();
    return manager.selectBackend(static_cast<watermelon_audio::BackendType>(backend_id));
}

int wma_get_backend_type(void) {
    auto& manager = watermelon_audio::BackendManager::getInstance();
    return static_cast<int>(manager.getCurrentType());
}

bool wma_is_usb_available(void) {
    auto& manager = watermelon_audio::BackendManager::getInstance();
    return manager.isUsbBackendAvailable();
}

void wma_set_usb_streaming_mode(int mode_id) {
    auto& manager = watermelon_audio::BackendManager::getInstance();
    manager.setFullDuplexEnabled(mode_id == 2);
}

void wma_configure_usb_backend(int sample_rate, int channels, int bit_depth) {
    // `channels` and `bit_depth` are informational only. LibusbBackend picks
    // the actual stream format via AltsettingSelector. See the matching JNI
    // binding nativeConfigureUsbBackend in jni_audio_bridge.cpp for the full
    // story on why we no longer route bit_depth into setBufferSize().
    (void)channels;
    (void)bit_depth;
    auto& manager = watermelon_audio::BackendManager::getInstance();
    manager.setSampleRate(sample_rate);
}

bool wma_usb_init_device(int file_descriptor, const char* usbfs_path) {
    if (file_descriptor < 0 || !usbfs_path) return false;
    auto& manager = watermelon_audio::BackendManager::getInstance();
    return manager.initializeUsbBackend(file_descriptor, usbfs_path);
}

void wma_usb_close_device(void) {
    auto& manager = watermelon_audio::BackendManager::getInstance();
    manager.fallbackToOboe();
}

bool wma_usb_set_latency_profile(int profile) {
    // Persist on the BackendManager so it survives backend recreation and is
    // re-applied at start (Fase 1) — same semantics as the USB streaming mode.
    auto& manager = watermelon_audio::BackendManager::getInstance();
    const auto p = (profile == 1)
        ? watermelon_audio::usb::UsbLatencyProfile::LOW_LATENCY
        : watermelon_audio::usb::UsbLatencyProfile::SAFE;
    manager.setLatencyProfile(p);
    return true;
}

/* ================================================================
 * 17. XY Mapping / Automation
 * ================================================================ */

void wma_set_automation_param(WmaEngine* engine, int effect_index, int param_id, float xy_value) {
    WMA_CHECK_VOID(engine);
    size_t chainSize = engine->engine->getNumEffects();
    if (effect_index < 0 || static_cast<size_t>(effect_index) >= chainSize) return;
    engine->engine->setParameter(static_cast<size_t>(effect_index), param_id, xy_value);
}

void wma_set_mapping_config(WmaEngine* engine, int axis,
                             int effect_index, int param_id,
                             int curve, int polarity,
                             float map_min, float map_max, bool inverted) {
    WMA_CHECK_VOID(engine);
    if (axis < 0 || axis > 2) return;
    if (curve < 0 || curve > 3) return;
    if (polarity < 0 || polarity > 1) return;
    if (!std::isfinite(map_min) || !std::isfinite(map_max)) return;
    engine->engine->setMappingConfig(axis, effect_index, param_id,
                                     curve, polarity, map_min, map_max, inverted);
}

void wma_clear_mapping_config(WmaEngine* engine, int axis) {
    WMA_CHECK_VOID(engine);
    if (axis < 0 || axis > 2) return;
    engine->engine->clearMappingConfig(axis);
}

void wma_set_depth_value(WmaEngine* engine, float value) {
    WMA_CHECK_VOID(engine);
    engine->engine->setDepthValue(std::clamp(value, 0.0f, 1.0f));
}

void wma_apply_automation(WmaEngine* engine, int axis, float normalized_value) {
    WMA_CHECK_VOID(engine);
    if (axis < 0 || axis > 2) return;
    engine->engine->applyAutomation(axis, std::clamp(normalized_value, 0.0f, 1.0f));
}

/* ================================================================
 * 18. Arpeggiator
 * ================================================================ */

void wma_arp_set_enabled(WmaEngine* engine, bool enabled) {
    WMA_CHECK_VOID(engine);
    engine->engine->getArpSequencer().setEnabled(enabled);
}

bool wma_arp_is_enabled(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, false);
    /* const_cast needed because getArpSequencer() non-const is used;
       we use the const overload. */
    return engine->engine->getArpSequencer().isEnabled();
}

void wma_arp_set_pattern(WmaEngine* engine, int pattern_id) {
    WMA_CHECK_VOID(engine);
    engine->engine->getArpSequencer().setPattern(pattern_id);
}

void wma_arp_set_subdivision(WmaEngine* engine, float beats_per_step) {
    WMA_CHECK_VOID(engine);
    engine->engine->getArpSequencer().setSubdivision(beats_per_step);
}

void wma_arp_set_octave_range(WmaEngine* engine, int octaves) {
    WMA_CHECK_VOID(engine);
    engine->engine->getArpSequencer().setOctaveRange(octaves);
}

void wma_arp_set_gate_length(WmaEngine* engine, float gate) {
    WMA_CHECK_VOID(engine);
    engine->engine->getArpSequencer().setGateLength(gate);
}

void wma_arp_set_swing(WmaEngine* engine, float swing) {
    WMA_CHECK_VOID(engine);
    engine->engine->getArpSequencer().setSwing(swing);
}

void wma_arp_set_latch(WmaEngine* engine, bool latch) {
    WMA_CHECK_VOID(engine);
    engine->engine->getArpSequencer().setLatch(latch);
}

void wma_arp_set_velocity(WmaEngine* engine, float velocity) {
    WMA_CHECK_VOID(engine);
    engine->engine->getArpSequencer().setVelocity(velocity);
}

void wma_arp_set_velocity_variation(WmaEngine* engine, float variation) {
    WMA_CHECK_VOID(engine);
    engine->engine->getArpSequencer().setVelocityVariation(variation);
}

void wma_arp_set_probability(WmaEngine* engine, float probability) {
    WMA_CHECK_VOID(engine);
    engine->engine->getArpSequencer().setProbability(probability);
}

void wma_arp_set_scale_intervals(WmaEngine* engine, const int* intervals, int count) {
    WMA_CHECK_VOID(engine);
    if (!intervals || count <= 0) return;
    engine->engine->getArpSequencer().setScaleIntervals(intervals, count);
}

void wma_arp_set_touch_active(WmaEngine* engine, bool active) {
    WMA_CHECK_VOID(engine);
    engine->engine->getArpSequencer().setTouchActive(active);
}

void wma_arp_set_base_freq(WmaEngine* engine, float frequency) {
    WMA_CHECK_VOID(engine);
    engine->engine->getArpSequencer().setBaseFrequency(frequency);
}

int wma_arp_get_current_step(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getArpSequencer().getCurrentStep();
}

int wma_arp_get_total_steps(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getArpSequencer().getTotalSteps();
}

void wma_arp_set_ratchet(WmaEngine* engine, bool active) {
    WMA_CHECK_VOID(engine);
    engine->engine->getArpSequencer().setRatchet(active);
}

void wma_arp_regenerate(WmaEngine* engine) {
    WMA_CHECK_VOID(engine);
    engine->engine->getArpSequencer().regeneratePattern();
}

bool wma_arp_is_gate_open(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->getArpSequencer().isGateOpen();
}

/* ================================================================
 * 19. Looper
 * ================================================================ */

WmaResult wma_looper_prepare_track(WmaEngine* engine, int track_index,
                                    int length_frames, int sample_rate) {
    WMA_CHECK(engine);
    bool ok = engine->engine->getAudioLooper().prepareTrack(track_index, length_frames, sample_rate);
    return ok ? WMA_OK : WMA_ERROR_MEMORY;
}

void wma_looper_start_recording(WmaEngine* engine, int track_index) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().startRecording(track_index);
}

void wma_looper_stop_recording(WmaEngine* engine) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().stopRecording();
}

void wma_looper_start_overdub(WmaEngine* engine, int track_index) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().startOverdub(track_index);
}

void wma_looper_stop_all(WmaEngine* engine) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().stopAll();
}

void wma_looper_pause(WmaEngine* engine) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().pause();
}

void wma_looper_resume(WmaEngine* engine) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().resume();
}

void wma_looper_set_track_muted(WmaEngine* engine, int track_index, bool muted) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().setTrackMuted(track_index, muted);
}

void wma_looper_set_track_pan(WmaEngine* engine, int track_index, float pan) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().setTrackPan(track_index, pan);
}

void wma_looper_set_track_volume(WmaEngine* engine, int track_index, float volume) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().setTrackVolume(track_index, volume);
}

void wma_looper_set_track_speed(WmaEngine* engine, int track_index, float speed) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().setTrackSpeed(track_index, speed);
}

float wma_looper_get_track_speed(const WmaEngine* engine, int track_index) {
    WMA_CHECK_VAL(engine, 1.0f);
    return engine->engine->getAudioLooper().getTrackSpeed(track_index);
}

void wma_looper_clear_track(WmaEngine* engine, int track_index) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().clearTrack(track_index);
}

void wma_looper_clear_all(WmaEngine* engine) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().clearAll();
}

void wma_looper_set_enabled(WmaEngine* engine, bool enabled) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().setEnabled(enabled);
}

float wma_looper_get_progress(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0.0f);
    return engine->engine->getAudioLooper().getProgress();
}

float wma_looper_get_record_progress(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0.0f);
    return engine->engine->getAudioLooper().getRecordProgress();
}

float wma_looper_get_track_peak(const WmaEngine* engine, int track_index) {
    WMA_CHECK_VAL(engine, 0.0f);
    return engine->engine->getAudioLooper().getTrackPeakLevel(track_index);
}

bool wma_looper_is_track_active(const WmaEngine* engine, int track_index) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->getAudioLooper().isTrackActive(track_index);
}

bool wma_looper_is_playing(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->getAudioLooper().isPlaying();
}

bool wma_looper_is_recording(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->getAudioLooper().isRecording();
}

int wma_looper_get_master_loop_frames(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getAudioLooper().getMasterLoopFrames();
}

void wma_looper_set_free_length(WmaEngine* engine, bool free_length) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().setFreeLength(free_length);
}

void wma_looper_pause_track(WmaEngine* engine, int track_index) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().pauseTrack(track_index);
}

void wma_looper_resume_track(WmaEngine* engine, int track_index) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().resumeTrack(track_index);
}

bool wma_looper_is_track_playing(const WmaEngine* engine, int track_index) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->getAudioLooper().isTrackPlaying(track_index);
}

float wma_looper_get_track_progress(const WmaEngine* engine, int track_index) {
    WMA_CHECK_VAL(engine, 0.0f);
    return engine->engine->getAudioLooper().getTrackProgress(track_index);
}

int wma_looper_get_track_length_frames(const WmaEngine* engine, int track_index) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getAudioLooper().getTrackLengthFrames(track_index);
}

void wma_looper_reset_track_playhead(WmaEngine* engine, int track_index) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().resetTrackPlayHead(track_index);
}

bool wma_looper_save_undo(WmaEngine* engine, int track_index) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->getAudioLooper().saveUndoSnapshot(track_index);
}

bool wma_looper_restore_undo(WmaEngine* engine, int track_index) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->getAudioLooper().restoreUndo(track_index);
}

bool wma_looper_has_undo(const WmaEngine* engine, int track_index) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->getAudioLooper().hasUndo(track_index);
}

int wma_looper_get_track_waveform(const WmaEngine* engine, int track_index,
                                   float* buffer, int max_bins) {
    WMA_CHECK_VAL(engine, 0);
    if (!buffer || max_bins <= 0) return 0;
    return engine->engine->getAudioLooper().getTrackWaveform(track_index, buffer, max_bins);
}

void wma_looper_set_master_volume(WmaEngine* engine, float volume) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().setMasterVolume(volume);
}

float wma_looper_get_master_volume(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 1.0f);
    return engine->engine->getAudioLooper().getMasterVolume();
}

void wma_looper_set_track_loop_region(WmaEngine* engine, int track_index,
                                       int64_t start_frame, int64_t end_frame) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().setTrackLoopRegion(track_index, start_frame, end_frame);
}

void wma_looper_reset_track_loop_region(WmaEngine* engine, int track_index) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().resetTrackLoopRegion(track_index);
}

int wma_looper_get_track_loop_start(const WmaEngine* engine, int track_index) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getAudioLooper().getTrackLoopStart(track_index);
}

int wma_looper_get_track_loop_end(const WmaEngine* engine, int track_index) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getAudioLooper().getTrackLoopEnd(track_index);
}

void wma_looper_trigger_click(WmaEngine* engine, bool is_downbeat) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().triggerClick(is_downbeat);
}

/* ---------------- Track editing & analysis ---------------- */

void wma_looper_abort_recording(WmaEngine* engine) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().abortRecording();
}

void wma_looper_start_recording_with_pre_roll(WmaEngine* engine, int track_index,
                                               int pre_roll_ms) {
    WMA_CHECK_VOID(engine);
    auto& looper = engine->engine->getAudioLooper();

    // 1 s is what the pre-roll ring holds; asking for more would read past it.
    if (pre_roll_ms < 0) pre_roll_ms = 0;
    if (pre_roll_ms > 1000) pre_roll_ms = 1000;
    if (pre_roll_ms == 0) {
        looper.startRecording(track_index);
        return;
    }

    const int sampleRate = looper.getSampleRate();
    const int preRollFrames = (pre_roll_ms * sampleRate) / 1000;
    if (preRollFrames <= 0) {
        looper.startRecording(track_index);
        return;
    }

    // Allocation on the calling thread, which is the control thread — never the
    // audio thread. Same as the JNI did, and said so.
    std::vector<float> preRoll(static_cast<size_t>(preRollFrames) * 2, 0.0f);
    engine->engine->getPreRollRing().snapshot(preRoll.data(), preRollFrames);
    looper.startRecordingWithPreRoll(track_index, preRoll.data(), preRollFrames);
}

int wma_looper_prepare_track_bars(WmaEngine* engine, int track_index, int bars,
                                   int sample_rate) {
    WMA_CHECK_VAL(engine, -1);
    const int framesPerBar = engine->engine->getTransport().framesPerBar(1);
    if (framesPerBar <= 0 || bars <= 0) return -1;

    // `bars * framesPerBar` is int arithmetic in AudioLooper too, and prepareTrack
    // only rejects a NON-POSITIVE length — so a bar count large enough to wrap
    // into a small positive would allocate a tiny track and report the wrapped
    // length as success. Third width problem in this category; caught here rather
    // than propagated.
    const int64_t lengthFrames =
        static_cast<int64_t>(bars) * static_cast<int64_t>(framesPerBar);
    if (lengthFrames > INT32_MAX) return -1;

    const bool ok = engine->engine->getAudioLooper()
                        .prepareTrackBars(track_index, bars, framesPerBar, sample_rate);
    return ok ? static_cast<int>(lengthFrames) : -1;
}

bool wma_looper_trim_track(WmaEngine* engine, int track_index) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->getAudioLooper().trimTrack(track_index);
}

bool wma_looper_finalize_free_loop(WmaEngine* engine, int track_index,
                                    int loop_start, int loop_end, int tail_frames) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->getAudioLooper().finalizeFreeLoop(
        track_index, loop_start, loop_end, tail_frames);
}

bool wma_looper_find_content_bounds(const WmaEngine* engine, int track_index,
                                    float threshold_ratio,
                                    int* out_first, int* out_last) {
    WMA_CHECK_VAL(engine, false);
    if (!out_first || !out_last) return false;

    // AudioLooper packs the pair into an int64 for the JNI's benefit; unpack it
    // here so the C API can hand back two plain ints. The low half is masked as
    // unsigned on the way in, so it has to come back out the same way or a frame
    // index with the top bit set would arrive negative.
    const int64_t packed =
        engine->engine->getAudioLooper().findTrackContentBounds(track_index, threshold_ratio);
    *out_first = static_cast<int>(packed >> 32);
    *out_last = static_cast<int>(static_cast<uint32_t>(packed & 0xFFFFFFFF));
    return true;
}

int wma_looper_detect_onsets(const WmaEngine* engine, int track_index,
                              int* out_onsets, int max_onsets,
                              int hop_frames, float sensitivity) {
    WMA_CHECK_VAL(engine, 0);
    if (!out_onsets || max_onsets <= 0) return 0;
    const int written = engine->engine->getAudioLooper().detectTrackOnsets(
        track_index, out_onsets, max_onsets, hop_frames, sensitivity);
    return written < 0 ? 0 : written;
}

/* ---------------- Per-track playback modes ---------------- */

void wma_looper_set_track_play_count(WmaEngine* engine, int track_index, int plays) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().setTrackPlayCount(track_index, plays);
}

void wma_looper_set_track_percussion_mode(WmaEngine* engine, int track_index,
                                           bool percussion) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().setTrackPercussionMode(track_index, percussion);
}

bool wma_looper_is_track_percussion_mode(const WmaEngine* engine, int track_index) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->getAudioLooper().isTrackPercussionMode(track_index);
}

void wma_looper_set_tail_ms(WmaEngine* engine, int ms) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().setTailMs(ms);
}

int wma_looper_get_tail_ms(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getAudioLooper().getTailMs();
}

void wma_looper_set_capabilities(WmaEngine* engine, int64_t budget_bytes,
                                  int max_tracks, int max_free_seconds) {
    WMA_CHECK_VOID(engine);
    // Defaults reproduce the historical behaviour; each field is only overridden
    // when the caller passes a positive value. That "0 means leave it alone"
    // contract is the whole reason this takes three arguments instead of a struct.
    AudioLooper::LooperCapabilities caps;
    if (budget_bytes > 0) caps.memoryBudgetBytes = static_cast<size_t>(budget_bytes);
    if (max_tracks > 0) caps.maxActiveTracks = max_tracks;
    if (max_free_seconds > 0) caps.maxFreeSeconds = max_free_seconds;
    engine->engine->getAudioLooper().setCapabilities(caps);
}

/* ---------------- Armed recording ---------------- */

namespace {

/**
 * Arm @p track at @p trigger and report whether it took.
 *
 * AudioLooper::armRecording is void and no-ops on a bad index or a track with no
 * capacity, so the JNI versions of arm_at_next_bar / arm_in_frames returned a
 * positive trigger frame for a recording that was never armed — while their own
 * doc comment promised "-1 on failure". A caller showing a count-in would count
 * down to nothing. Reading the armed track back is how we keep the promise.
 */
int64_t armAndConfirm(WmaEngine* engine, int track, int64_t trigger) {
    // `track >= 0` is not redundant with the comparison below: getArmedTrack()
    // reports -1 for "nothing armed", so arming track -1 would confirm itself.
    // A test caught exactly that.
    if (track < 0) return -1;
    auto& looper = engine->engine->getAudioLooper();
    looper.armRecording(track, trigger);
    return looper.getArmedTrack() == track ? trigger : -1;
}

}  // namespace

int64_t wma_looper_arm_at_next_bar(WmaEngine* engine, int track_index) {
    WMA_CHECK_VAL(engine, -1);
    auto& transport = engine->engine->getTransport();
    const int64_t trigger = transport.nextBarBoundary(transport.getPlayFrame());
    return armAndConfirm(engine, track_index, trigger);
}

int64_t wma_looper_arm_in_frames(WmaEngine* engine, int track_index,
                                  int64_t offset_frames) {
    WMA_CHECK_VAL(engine, -1);
    if (offset_frames < 0) offset_frames = 0;
    const int64_t trigger = engine->engine->getTransport().getPlayFrame() + offset_frames;
    return armAndConfirm(engine, track_index, trigger);
}

int64_t wma_looper_arm_synced_to_loop(WmaEngine* engine, int track_index,
                                      int64_t latency_frames) {
    return wma_looper_arm_synced_to_loop_quantized(engine, track_index, latency_frames, 0);
}

int64_t wma_looper_arm_synced_to_loop_quantized(WmaEngine* engine, int track_index,
                                                 int64_t latency_frames,
                                                 int quantum_frames) {
    WMA_CHECK_VAL(engine, -1);
    if (latency_frames < 0) latency_frames = 0;
    // armSyncedToLoop takes latency as an int. The clamp above plus this cast
    // mirror what the JNI did; a latency that overflowed an int would be hours,
    // not a round trip.
    const int64_t playFrame = engine->engine->getTransport().getPlayFrame();
    return engine->engine->getAudioLooper().armSyncedToLoop(
        track_index, playFrame, static_cast<int>(latency_frames), quantum_frames);
}

void wma_looper_cancel_arm(WmaEngine* engine) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().cancelArm();
}

int wma_looper_get_armed_track(const WmaEngine* engine) {
    // -1, not 0: track 0 is a real track, so "none" needs its own value.
    WMA_CHECK_VAL(engine, -1);
    return engine->engine->getAudioLooper().getArmedTrack();
}

/* ---------------- Telemetry ---------------- */

int64_t wma_looper_get_armed_triggered(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getAudioLooper().getArmedTriggered();
}

int64_t wma_looper_get_frames_dropped(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getAudioLooper().getFramesDropped();
}

int64_t wma_looper_get_dropped_events(const WmaEngine* engine) {
    // The dispatcher, not the looper — this counter is about the event queue
    // overflowing, not about audio.
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getLooperEventDispatcher().getDroppedEvents();
}

void wma_looper_reset_telemetry(WmaEngine* engine) {
    WMA_CHECK_VOID(engine);
    engine->engine->getAudioLooper().resetTelemetry();
}

bool wma_looper_export_mix(WmaEngine* engine, const char* file_path) {
    WMA_CHECK_VAL(engine, false);
    if (!file_path) return false;
    return engine->engine->getAudioLooper().exportMix(file_path);
}

bool wma_looper_export_track(WmaEngine* engine, int track_index, const char* file_path) {
    WMA_CHECK_VAL(engine, false);
    if (!file_path) return false;
    return engine->engine->getAudioLooper().exportTrack(track_index, file_path);
}

bool wma_looper_import_track(WmaEngine* engine, int track_index,
                              const char* file_path, int sample_rate) {
    WMA_CHECK_VAL(engine, false);
    if (!file_path) return false;
    return engine->engine->getAudioLooper().importTrack(track_index, file_path, sample_rate);
}

/* ================================================================
 * 20. Transport (musical clock & metronome)
 * ================================================================ */

void wma_transport_set_beats_per_bar(WmaEngine* engine, int beats_per_bar) {
    WMA_CHECK_VOID(engine);
    engine->engine->getTransport().setBeatsPerBar(beats_per_bar);
}

int wma_transport_get_beats_per_bar(const WmaEngine* engine) {
    // 4 rather than 0: the default meter, which is also what the Transport
    // would report. A caller dividing by this must not get a zero.
    WMA_CHECK_VAL(engine, 4);
    return engine->engine->getTransport().getBeatsPerBar();
}

int wma_transport_frames_per_beat(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getTransport().framesPerBeat();
}

int wma_transport_frames_per_bar(const WmaEngine* engine, int bars) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getTransport().framesPerBar(bars);
}

void wma_transport_start_metronome(WmaEngine* engine, int beats,
                                    bool first_is_downbeat,
                                    bool every_beat_pattern) {
    WMA_CHECK_VOID(engine);
    engine->engine->getTransport().startMetronome(beats, first_is_downbeat,
                                                  every_beat_pattern);
}

void wma_transport_start_metronome_continuous(WmaEngine* engine,
                                               bool every_beat_pattern) {
    WMA_CHECK_VOID(engine);
    engine->engine->getTransport().startMetronomeContinuous(every_beat_pattern);
}

void wma_transport_stop_metronome(WmaEngine* engine) {
    WMA_CHECK_VOID(engine);
    engine->engine->getTransport().stopMetronome();
}

bool wma_transport_is_metronome_running(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->getTransport().isMetronomeRunning();
}

bool wma_transport_is_metronome_continuous(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, false);
    return engine->engine->getTransport().isMetronomeContinuous();
}

int wma_transport_get_remaining_beats(const WmaEngine* engine) {
    WMA_CHECK_VAL(engine, 0);
    return engine->engine->getTransport().getRemainingBeats();
}

/* ================================================================
 * 21. Diagnostics & Latency
 * ================================================================ */

int wma_get_recommended_buffer_size(const WmaEngine* engine, float target_latency_ms) {
    if (!(target_latency_ms > 0.0f)) return -1;  // also rejects NaN

    // currentSampleRate() rather than "getStreamInfo() or else 48000": it
    // resolves running stream -> preferred rate -> 48000 and never returns <= 0.
    // The hand-rolled version skipped the preferred rate, so a device configured
    // for 44.1 kHz that had not started yet got a size computed for 48 kHz.
    // That shortcut is exactly what AudioEngine.h warns about above
    // currentSampleRate(), and what put SoundFonts on the wrong rate in WA-2.0.
    const int sampleRate = engine && engine->engine
                               ? engine->engine->currentSampleRate()
                               : 48000;

    const double targetFrames =
        static_cast<double>(target_latency_ms) / 1000.0 * static_cast<double>(sampleRate);

    int bufferSize = 64;
    while (bufferSize < targetFrames && bufferSize < 2048) {
        bufferSize *= 2;
    }
    return bufferSize;
}

int wma_get_latency_report(const WmaEngine* engine, char* buffer, int buffer_size) {
    std::string report = "NoisyPad Latency Report\n";
    report += "========================\n\n";

    if (!engine || !engine->engine) {
        report += "Engine not initialized\n";
    } else {
        int32_t sampleRate = 0, bufferFrames = 0;
        double latencyMillis = 0.0;
        if (engine->engine->getStreamInfo(sampleRate, bufferFrames, latencyMillis)) {
            report += "Sample Rate: " + std::to_string(sampleRate) + " Hz\n";
            report += "Buffer Size: " + std::to_string(bufferFrames) + " frames\n";
            report += "Output Latency: " + std::to_string(latencyMillis) + " ms\n";
        } else {
            // The old report just omitted these three lines, which reads as
            // "zero latency" rather than "no stream to measure".
            report += "No stream running — nothing to measure.\n";
        }

        const float inputLatency = wma_input_get_latency_ms(engine);
        if (inputLatency > 0.0f) {
            report += "Input Latency: " + std::to_string(inputLatency) + " ms\n";
        }

        // The backend was available all along —BackendManager::getStreamInfo()
        // carries it and AudioEngine logs it at start— and the report threw it
        // away. On the USB path that left a latency report with no mention of
        // USB, which is the first thing you would want to know.
        auto& manager = watermelon_audio::BackendManager::getInstance();
        report += "Backend: ";
        report += watermelon_audio::backendTypeToString(manager.getCurrentType());
        report += "\n";
    }

    const int fullLength = static_cast<int>(report.size());
    if (buffer && buffer_size > 0) {
        const int copyLength = std::min(fullLength, buffer_size - 1);
        std::memcpy(buffer, report.data(), static_cast<size_t>(copyLength));
        buffer[copyLength] = '\0';
    }
    return fullLength;
}

/* ================================================================
 * 22. Waveform & Metering
 * ================================================================ */

int wma_get_waveform_samples(WmaEngine* engine, float* buffer, int max_size) {
    if (!engine || !engine->engine || !buffer || max_size <= 0) return 0;
    return engine->engine->getWaveformSamples(buffer, max_size);
}

float wma_get_output_peak(const WmaEngine* engine, int channel) {
    WMA_CHECK_VAL(engine, 0.0f);
    return engine->engine->getOutputPeakLevel(channel);
}

float wma_get_output_rms(const WmaEngine* engine, int channel) {
    WMA_CHECK_VAL(engine, 0.0f);
    return engine->engine->getOutputRMSLevel(channel);
}

float wma_get_output_peak_db(const WmaEngine* engine, int channel) {
    WMA_CHECK_VAL(engine, -100.0f);
    float linear = engine->engine->getOutputPeakLevel(channel);
    if (linear <= 0.0f) return -100.0f;
    return 20.0f * std::log10(linear);
}

float wma_get_output_rms_db(const WmaEngine* engine, int channel) {
    WMA_CHECK_VAL(engine, -100.0f);
    float linear = engine->engine->getOutputRMSLevel(channel);
    if (linear <= 0.0f) return -100.0f;
    return 20.0f * std::log10(linear);
}

void wma_get_output_levels(const WmaEngine* engine, float* out_levels) {
    if (!engine || !engine->engine || !out_levels) return;
    out_levels[0] = engine->engine->getOutputPeakLevel(0);
    out_levels[1] = engine->engine->getOutputPeakLevel(1);
    out_levels[2] = engine->engine->getOutputRMSLevel(0);
    out_levels[3] = engine->engine->getOutputRMSLevel(1);
}

/* ================================================================
 * 23. Configuration / Logging
 * ================================================================ */

/* Static storage for C callback — bridged to C++ LogCallback. */
static WmaLogCallback g_cLogCallback = nullptr;

static void wmaLogBridge(wma::LogLevel level, const char* tag, const char* msg) {
    WmaLogCallback cb = g_cLogCallback;
    if (cb) {
        cb(static_cast<WmaLogLevel>(static_cast<int>(level)), tag, msg);
    }
}

void wma_set_log_callback(WmaLogCallback callback) {
    g_cLogCallback = callback;
    wma::setLogCallback(callback ? wmaLogBridge : nullptr);
}

const char* wma_get_version(void) {
    return WMA_VERSION_STRING;
}
