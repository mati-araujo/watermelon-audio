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

#include <cmath>
#include <algorithm>
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

WmaResult wma_engine_start(WmaEngine* engine, int fade_time_ms) {
    WMA_CHECK(engine);
    bool ok;
    if (fade_time_ms > 0) {
        ok = engine->engine->startWithFade(fade_time_ms);
    } else {
        ok = engine->engine->start();
    }
    return ok ? WMA_OK : WMA_ERROR_STREAM;
}

WmaResult wma_engine_stop(WmaEngine* engine, int fade_time_ms) {
    WMA_CHECK(engine);
    if (fade_time_ms > 0) {
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
        for (int i = 0; i < count; ++i) {
            if (std::isfinite(values[i])) {
                engine->engine->setParameter(static_cast<size_t>(index), param_ids[i], values[i]);
            }
        }
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

void wma_set_audio_mode(WmaEngine* engine, int mode) {
    WMA_CHECK_VOID(engine);
    if (mode < 0 || mode > 2) return;

    auto audioMode = static_cast<watermelon_audio::AudioMode>(mode);

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
            engine->engine->setOscillatorEnabled(false);
            engine->engine->setVocoderCarrierSource(true);
            ensureInputNode(engine);
            if (engine->inputNode) {
                if (!engine->inputNode->isInputStreamRunning()) {
                    engine->inputNode->startInputStream();
                }
                engine->inputNode->setMonitoringEnabled(true);
                engine->engine->setInputNode(engine->inputNode);
            }
            break;

        case watermelon_audio::AudioMode::MIX:
            engine->engine->setOscillatorEnabled(true);
            engine->engine->setVocoderCarrierSource(true);
            ensureInputNode(engine);
            if (engine->inputNode) {
                if (!engine->inputNode->isInputStreamRunning()) {
                    engine->inputNode->startInputStream();
                }
                engine->inputNode->setMonitoringEnabled(true);
                engine->engine->setInputNode(engine->inputNode);
            }
            break;
    }

    engine->currentMode.store(static_cast<int>(audioMode), std::memory_order_release);
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

bool wma_input_start(WmaEngine* engine) {
    if (!engine) return false;
    if (!ensureInputNode(engine)) return false;
    return engine->inputNode->startInputStream();
}

void wma_input_stop(WmaEngine* engine) {
    if (!engine || !engine->inputNode) return;
    engine->inputNode->stopInputStream();
}

bool wma_input_is_running(const WmaEngine* engine) {
    if (!engine || !engine->inputNode) return false;
    return engine->inputNode->isInputStreamRunning();
}

void wma_input_set_source(WmaEngine* engine, int source) {
    if (!engine || !engine->inputNode) return;
    if (source < 0 || source > 2) return;
    engine->inputNode->setInputSource(static_cast<InputSource>(source));
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

float wma_input_get_level(const WmaEngine* engine, int channel) {
    if (!engine || !engine->inputNode) return -100.0f;
    return engine->inputNode->getInputLevel(channel);
}

bool wma_input_is_clipping(const WmaEngine* engine) {
    if (!engine || !engine->inputNode) return false;
    return engine->inputNode->isClipping();
}

void wma_input_set_monitoring(WmaEngine* engine, bool enabled) {
    if (!engine || !engine->inputNode) return;
    engine->inputNode->setMonitoringEnabled(enabled);
}

void wma_input_release(WmaEngine* engine) {
    if (!engine) return;
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
                                       int start_frame, int end_frame) {
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
 * 20. Waveform & Metering
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
 * 21. Configuration / Logging
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
