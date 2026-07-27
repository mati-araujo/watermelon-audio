/**
 * @file watermelon_audio.h
 * @brief Watermelon Audio — Pure C API for the NoisyPad audio engine.
 *
 * Platform-agnostic interface suitable for JNI, iOS cinterop, FFI, WASM, etc.
 * All functions are thread-safe unless marked otherwise.
 *
 * Version: 0.1.0 (Phase 0C — Audio Library Extraction)
 *
 * Copyright (c) 2026 Watermelon Studios. All rights reserved.
 */

#ifndef WATERMELON_AUDIO_H
#define WATERMELON_AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * Version
 * ================================================================ */

/* Defaults for standalone builds. Gradle-driven builds override these
   via CMake compile definitions (-DWMA_VERSION_MAJOR=... etc.). */
#ifndef WMA_VERSION_MAJOR
#define WMA_VERSION_MAJOR 0
#define WMA_VERSION_MINOR 1
#define WMA_VERSION_PATCH 0
#define WMA_VERSION_STRING "0.1.0"
#endif

/* ================================================================
 * Export / Visibility
 * ================================================================ */

#if defined(_WIN32) || defined(__CYGWIN__)
    #ifdef WMA_BUILDING_DLL
        #define WMA_API __declspec(dllexport)
    #else
        #define WMA_API
    #endif
#elif __GNUC__ >= 4 || defined(__clang__)
    #define WMA_API __attribute__((visibility("default")))
#else
    #define WMA_API
#endif

/* ================================================================
 * C linkage
 * ================================================================ */

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Opaque handle
 * ================================================================ */

/** Opaque engine handle. Created by wma_engine_create(). */
typedef struct WmaEngine WmaEngine;

/* ================================================================
 * Error codes (match JniError namespace values)
 * ================================================================ */

typedef enum WmaResult {
    WMA_OK                             =   0,
    WMA_ERROR_NOT_INITIALIZED          =  -1,
    WMA_ERROR_INVALID_EFFECT_INDEX     =  -2,
    WMA_ERROR_INVALID_PARAMETER_ID     =  -3,
    WMA_ERROR_PARAMETER_OUT_OF_RANGE   =  -4,
    WMA_ERROR_EFFECT_CHAIN_FULL        =  -5,
    WMA_ERROR_MEMORY                   =  -6,
    WMA_ERROR_STREAM                   =  -7,
    WMA_ERROR_MODE_TRANSITION          =  -8,
    WMA_ERROR_INVALID_OPERATION        =  -9,
    WMA_ERROR_INVALID_EFFECT_TYPE      = -10,
    WMA_ERROR_TIMEOUT                  = -11,
    WMA_ERROR_UNKNOWN                  = -99
} WmaResult;

/* ================================================================
 * Log levels (mirror wma::LogLevel)
 * ================================================================ */

typedef enum WmaLogLevel {
    WMA_LOG_DEBUG = 0,
    WMA_LOG_INFO  = 1,
    WMA_LOG_WARN  = 2,
    WMA_LOG_ERROR = 3
} WmaLogLevel;

/**
 * Log callback signature.
 * @param level  Severity
 * @param tag    Module tag (null-terminated)
 * @param msg    Formatted message (null-terminated)
 */
typedef void (*WmaLogCallback)(WmaLogLevel level, const char* tag, const char* msg);

/* ================================================================
 * 1. Lifecycle
 * ================================================================ */

/**
 * Fade duration meaning "whatever the engine picks by itself" — as opposed to
 * an explicit ramp length, which 0 also is (0 = cut, no ramp at all).
 *
 * Those are two different operations and the caller has to be able to say which
 * one it wants. Passing 0 to mean "no preference" silently collapsed them: the
 * JNI has always exposed both (`startEngine()` takes the engine default, a 10 ms
 * ramp; `startEngineWithFade(0)` cuts in instantly and cancels any fade already
 * running), while the iOS bridge mapped both onto fade_time_ms = 0 and got the
 * default for both. Same call, two platforms, two behaviours.
 */
#define WMA_FADE_DEFAULT (-1)

/**
 * Create a new audio engine instance.
 * @return Engine handle, or NULL on failure. Caller owns the handle.
 */
WMA_API WmaEngine* wma_engine_create(void);

/**
 * Destroy the engine and free all resources.
 * @param engine  Handle (may be NULL — no-op).
 */
WMA_API void wma_engine_destroy(WmaEngine* engine);

/**
 * Start the audio stream.
 * @param engine        Engine handle
 * @param fade_time_ms  Explicit fade-in duration in ms (0 = instant), or
 *                      WMA_FADE_DEFAULT for the engine's own default ramp.
 * @return WMA_OK on success
 */
WMA_API WmaResult wma_engine_start(WmaEngine* engine, int fade_time_ms);

/**
 * Stop the audio stream (blocking — waits for callbacks to finish).
 * @param engine        Engine handle
 * @param fade_time_ms  Explicit fade-out duration in ms (0 = instant), or
 *                      WMA_FADE_DEFAULT to stop without arming a ramp.
 * @return WMA_OK on success
 */
WMA_API WmaResult wma_engine_stop(WmaEngine* engine, int fade_time_ms);

/**
 * Pause audio (keeps stream alive, fades to silence).
 * @param engine        Engine handle
 * @param fade_time_ms  Fade-out duration in ms
 * @return WMA_OK on success
 */
WMA_API WmaResult wma_engine_pause(WmaEngine* engine, int fade_time_ms);

/**
 * Resume from pause (fades back in).
 * @param engine        Engine handle
 * @param fade_time_ms  Fade-in duration in ms
 * @return WMA_OK on success
 */
WMA_API WmaResult wma_engine_resume(WmaEngine* engine, int fade_time_ms);

/* ================================================================
 * 2. State
 * ================================================================ */

/** Get engine state: 0=Stopped, 1=Starting, 2=Running, 3=Stopping */
WMA_API int wma_get_engine_state(const WmaEngine* engine);

/** Check if engine is paused (stream alive but silent). */
WMA_API bool wma_is_paused(const WmaEngine* engine);

/** Monotonic state version counter (incremented on every mutation). */
WMA_API uint64_t wma_get_state_version(const WmaEngine* engine);

/** Check if there is an active stream error. */
WMA_API bool wma_has_error(const WmaEngine* engine);

/** Get last stream error code (0 if none). */
WMA_API int wma_get_last_error_code(const WmaEngine* engine);

/** Clear the stream error flag. */
WMA_API void wma_clear_error(WmaEngine* engine);

/** Check if initial memory allocation failed. */
WMA_API bool wma_has_init_failed(const WmaEngine* engine);

/** Check if the engine pointer is valid and constructed. */
WMA_API bool wma_is_initialized(const WmaEngine* engine);

/**
 * Get stream information.
 * @param[out] sample_rate  Stream sample rate in Hz
 * @param[out] buffer_size  Buffer size in frames
 * @param[out] latency_ms   Estimated latency in milliseconds
 * @return true if a stream is active
 */
WMA_API bool wma_get_stream_info(const WmaEngine* engine,
                                  int* sample_rate,
                                  int* buffer_size,
                                  float* latency_ms);

/** Check if engine is using reduced buffers (low memory mode). */
WMA_API bool wma_is_using_reduced_buffers(const WmaEngine* engine);

/* ================================================================
 * 3. Volume & Fade
 * ================================================================ */

/** Set master volume (0.0 – 1.0). RT-safe. */  /* RT-safe */
WMA_API void wma_set_master_volume(WmaEngine* engine, float volume);

/** Get master volume. */  /* RT-safe */
WMA_API float wma_get_master_volume(const WmaEngine* engine);

/** Get current fade volume multiplier. */
WMA_API float wma_get_fade_volume(const WmaEngine* engine);

/** Get fade target volume. */
WMA_API float wma_get_target_fade_volume(const WmaEngine* engine);

/** Check if a fade is in progress. */
WMA_API bool wma_is_fading(const WmaEngine* engine);

/** Get fade progress (0.0 – 1.0). */
WMA_API float wma_get_fade_progress(const WmaEngine* engine);

/* ================================================================
 * 4. XY / Oscillator
 * ================================================================ */

/** Update XY position (0.0–1.0 each). */  /* RT-safe */
WMA_API void wma_set_xy(WmaEngine* engine, float x, float y);

/** Set frequency and amplitude directly (bypasses XY mapping). */  /* RT-safe */
WMA_API void wma_set_frequency_amplitude(WmaEngine* engine, float frequency, float amplitude);

/** Set dynamic frequency range for XY mapping. */  /* RT-safe */
WMA_API void wma_set_frequency_range(WmaEngine* engine, float min_hz, float max_hz);

/** Set oscillator waveform type (0–4). */  /* RT-safe */
WMA_API void wma_set_oscillator_type(WmaEngine* engine, int type_id);

/* ================================================================
 * 5. Engine (synth)
 * ================================================================ */

/**
 * Set synthesis engine type.
 * 0=CLASSIC, 1=KARPLUS_STRONG, 2=FM, 3=WAVETABLE, 4=GRANULAR, 5=SUPERSAW, 6=SOUNDFONT
 */  /* RT-safe */
WMA_API void wma_set_engine_type(WmaEngine* engine, int engine_type);

/** Set a parameter on the current synth engine. */  /* RT-safe */
WMA_API void wma_set_engine_param(WmaEngine* engine, int param_id, float value);

/** Get the current engine type ID. */
WMA_API int wma_get_engine_type(const WmaEngine* engine);

/* ================================================================
 * 6. SoundFont
 * ================================================================ */

/**
 * Load a SoundFont from a file path (mmap, zero-copy). NOT RT-safe.
 * @return true on success
 */
WMA_API bool wma_sf_load_path(WmaEngine* engine, const char* path);

/**
 * Load a SoundFont from a sub-region [offset, offset+length) of an open file
 * descriptor (mmap, zero-copy). NOT RT-safe.
 *
 * For bundled assets exposed as an AssetFileDescriptor (fd + offset + length),
 * such as a Play Asset Delivery install-time pack. The fd stays owned by the
 * caller: this call is synchronous and never dup()s, close()s, or retains it —
 * the fd only needs to stay open for the duration of the call.
 *
 * @param fd      Open, readable file descriptor
 * @param offset  Byte offset of the SoundFont within the fd's file (>= 0)
 * @param length  Length of the SoundFont region, in bytes (> 0)
 * @return true on success; false for an invalid fd/region (never crashes)
 */
WMA_API bool wma_sf_load_fd(WmaEngine* engine, int fd, int64_t offset, int64_t length);

/**
 * Load a SoundFont from a memory buffer. NOT RT-safe.
 * @param data  Pointer to .sf2 bytes
 * @param size  Size in bytes
 * @return true on success
 */
WMA_API bool wma_sf_load_data(WmaEngine* engine, const void* data, int size);

/** Unload the current SoundFont. NOT RT-safe. */
WMA_API void wma_sf_unload(WmaEngine* engine);

/** Set active SoundFont preset by index. */
WMA_API void wma_sf_set_preset(WmaEngine* engine, int preset_index);

/** Get number of presets in the loaded SoundFont. */
WMA_API int wma_sf_get_preset_count(const WmaEngine* engine);

/**
 * Get preset name by index.
 * @return Pointer to internal string (valid until SoundFont is unloaded), or NULL.
 */
WMA_API const char* wma_sf_get_preset_name(const WmaEngine* engine, int preset_index);

/** Check if a SoundFont is loaded. */
WMA_API bool wma_sf_is_loaded(const WmaEngine* engine);

/**
 * Get the MIDI key range of a SoundFont preset.
 * @param[out] out_min_key  Lowest MIDI key
 * @param[out] out_max_key  Highest MIDI key
 * @return true if preset exists
 */
WMA_API bool wma_sf_get_preset_key_range(const WmaEngine* engine, int preset_index,
                                          int* out_min_key, int* out_max_key);

/**
 * Get the MIDI bank and program of a SoundFont preset.
 *
 * Bank/program is how a preset is addressed from outside the file — a saved
 * scene stores those two numbers rather than the preset index, which shifts
 * when a different SoundFont is loaded.
 *
 * @param[out] out_bank     MIDI bank number
 * @param[out] out_program  MIDI program number
 * @return true if the preset exists; the out params are untouched otherwise
 */
WMA_API bool wma_sf_get_preset_bank_program(const WmaEngine* engine, int preset_index,
                                             int* out_bank, int* out_program);

/** Start/update a SoundFont note. */  /* RT-safe */
WMA_API void wma_sf_note_on(WmaEngine* engine, int touch_id, int midi_note, float velocity);

/** Release a SoundFont note. */  /* RT-safe */
WMA_API void wma_sf_note_off(WmaEngine* engine, int touch_id);

/** Release all SoundFont notes. */  /* RT-safe */
WMA_API void wma_sf_note_off_all(WmaEngine* engine);

/**
 * Release every active SoundFont touch except @p keep_touch_id.
 * One lock-free call; the touch-state scan runs on the audio thread.
 * Use this instead of looping wma_sf_note_off() over all "other" slots.
 */
/* RT-safe */
WMA_API void wma_sf_note_off_all_except(WmaEngine* engine, int keep_touch_id);

/* ================================================================
 * 7. Voice Filter
 * ================================================================ */

WMA_API void wma_voice_filter_set_enabled(WmaEngine* engine, bool enabled);     /* RT-safe */
WMA_API void wma_voice_filter_set_cutoff(WmaEngine* engine, float hz);          /* RT-safe */
WMA_API void wma_voice_filter_set_resonance(WmaEngine* engine, float q);        /* RT-safe */
WMA_API void wma_voice_filter_set_mode(WmaEngine* engine, int mode);            /* RT-safe */

/* ================================================================
 * 8. Effects
 * ================================================================ */

/**
 * Add an effect to the chain.
 * @param type_id  Effect type (EffectType enum ordinal)
 * @return Index of the new effect on success, or negative WmaResult on error.
 */
WMA_API int wma_effect_add(WmaEngine* engine, int type_id);

/**
 * Remove an effect from the chain.
 * @return WMA_OK on success
 */
WMA_API WmaResult wma_effect_remove(WmaEngine* engine, int index);

/**
 * Remove ALL effects from the chain atomically.
 * Equivalent to calling wma_effect_remove() in a loop, but pays the audio-thread
 * grace period (~20ms) ONCE for the batch instead of per-effect. Scene-load
 * fast path.
 * @return WMA_OK on success
 */
WMA_API WmaResult wma_effect_clear_all(WmaEngine* engine);

/** Set one effect parameter. */
WMA_API WmaResult wma_effect_set_param(WmaEngine* engine, int index, int param_id, float value);

/** Get one effect parameter. */
WMA_API float wma_effect_get_param(const WmaEngine* engine, int index, int param_id);

/**
 * Batch-set parameters on a single effect, with ONE state-version bump.
 *
 * The single bump is the whole point, not an optimisation. Setting N parameters
 * one at a time bumps the version N times, and the Kotlin-side StateSynchronizer
 * emits on every bump — so a scene load is observed as a sequence of partial
 * states rather than one coherent one. That is AUD-6.
 *
 * @param param_ids  Array of parameter IDs
 * @param values     Array of parameter values
 * @param count      Number of elements
 * @return WMA_OK also when count is 0 or an array is NULL — nothing to do is
 *         not an error.
 */
WMA_API WmaResult wma_effect_set_params_batch(WmaEngine* engine, int index,
                                               const int* param_ids, const float* values,
                                               int count);

/**
 * Batch-set parameters across SEVERAL effects, with one state-version bump.
 *
 * The scene-load path: a scene touches many effects at once, and doing it with
 * one wma_effect_set_params_batch() call per effect brings the partial states
 * back, one per effect.
 *
 * @param effect_indices  Array of effect indices, parallel to the other two
 * @param param_ids       Array of parameter IDs
 * @param values          Array of parameter values
 * @param count           Number of elements in each array
 * @return WMA_OK also when count is 0 or an array is NULL. Out-of-range effect
 *         indices and non-finite values are skipped silently rather than
 *         failing the whole batch.
 */
WMA_API WmaResult wma_effect_set_params_multi(WmaEngine* engine,
                                               const int* effect_indices,
                                               const int* param_ids,
                                               const float* values,
                                               int count);

/** Set bypass state of an effect. */
WMA_API WmaResult wma_effect_set_bypass(WmaEngine* engine, int index, bool bypass);

/** Check if an effect is bypassed. */
WMA_API bool wma_effect_is_bypassed(const WmaEngine* engine, int index);

/** Set global bypass state for the full effect chain. */
WMA_API WmaResult wma_effect_set_global_bypass(WmaEngine* engine, bool bypass);

/** Check if the full effect chain is globally bypassed. */
WMA_API bool wma_effect_is_global_bypassed(const WmaEngine* engine);

/** Reorder an effect from one position to another. */
WMA_API WmaResult wma_effect_reorder(WmaEngine* engine, int from_index, int to_index);

/** Get number of effects in the chain. */
WMA_API int wma_effect_chain_size(const WmaEngine* engine);

/** Get effect type at index (-1 on error). */
WMA_API int wma_effect_get_type(const WmaEngine* engine, int index);

/** Set global BPM for tempo-synced effects (20–300). */
WMA_API void wma_set_bpm(WmaEngine* engine, float bpm);

/** Get global BPM. */
WMA_API float wma_get_bpm(const WmaEngine* engine);

/* ================================================================
 * 9. Routing
 * ================================================================ */

/** Set effect routing mode (0–5). */
WMA_API void wma_set_routing_mode(WmaEngine* engine, int mode);

/** Get current routing mode. */
WMA_API int wma_get_routing_mode(const WmaEngine* engine);

/** Set parallel mix amount (for parallel routing mode). */
WMA_API void wma_set_parallel_mix(WmaEngine* engine, float mix);

/** Set feedback amount (for feedback routing mode). */
WMA_API void wma_set_feedback_amount(WmaEngine* engine, float amount);

/* ================================================================
 * 10. Modulator
 * ================================================================ */

/**
 * Set modulator type (0=NONE, 1=BURST, 2=AM, 3=FM, 4=PWM, 5=ENV, 6=RING, 7=GATE).
 * @return WMA_OK on success
 */
WMA_API WmaResult wma_set_modulator_type(WmaEngine* engine, int type_id);

/** Set a parameter on the active modulator. */
WMA_API WmaResult wma_set_modulator_param(WmaEngine* engine, int param_id, float value);

/* ================================================================
 * 11. Audio Mode
 * ================================================================ */

/**
 * Set audio mode.
 * 0=CHAOS_PAD (oscillator), 1=INPUT_FX (microphone), 2=MIX (both).
 *
 * Does the whole transition: oscillator, vocoder routing, InputNode lifecycle,
 * the USB path, and the effect-chain reset that keeps chaos_pad's reverb tail
 * out of the first blocks of microphone audio.
 *
 * This used to be documented as "a simplified version" whose full behaviour
 * "should be handled by the platform layer". That is what it means for two
 * platforms to drift: the JNI grew the real transition and this one kept the
 * sketch, so iOS got a mode switch missing pieces nobody had written down as
 * missing. WA-2.6 moved the real one here.
 */
WMA_API void wma_set_audio_mode(WmaEngine* engine, int mode);

/** Get current audio mode. */
WMA_API int wma_get_audio_mode(const WmaEngine* engine);

/**
 * Human-readable name of a mode ("ChaosPad", "Input FX", "Mix").
 * @return Pointer to a static string; never NULL — an unknown id gives
 *         "Unknown" rather than nothing, which the JNI relies on because it
 *         feeds the result straight into NewStringUTF.
 */
WMA_API const char* wma_get_mode_name(int mode);

/**
 * Check if a mode transition is in progress.
 *
 * WARNING: always false today. Nothing writes the flag — the class that owns
 * the real transition state (core/ModeManager) is not wired into AudioEngine.
 * Kept because the JNI has always exposed it; see the note in §16.
 */
WMA_API bool wma_is_in_mode_transition(const WmaEngine* engine);

/** Get mode transition progress (0.0–1.0). Always 0 — see the warning above. */
WMA_API float wma_get_mode_transition_progress(const WmaEngine* engine);

/** Check if a given mode requires audio input. */
WMA_API bool wma_mode_requires_input(int mode);

/* ================================================================
 * 12. Input
 * ================================================================ */

/**
 * Start the input stream (microphone).
 *
 * **No bloquea.** En el camino donde el backend carga la entrada (Apple, USB) hay
 * que reabrir el stream, y eso corre en un thread propio: esta función vuelve
 * enseguida. El llamador típico es el main thread de una app con UI y reabrir un
 * stream puede tardar cientos de ms — o colgarse, como se midió en iOS adentro de
 * `[AVAudioSession setActive:]`.
 *
 * @return `false` **sólo si el pedido se rechazó de entrada** (sin motor, sin
 *         nodo de entrada, sin backend). `true` significa "la entrada está viva o
 *         se está abriendo", que son dos cosas distintas:
 *
 *         - wma_input_is_running()  — está entregando frames
 *         - wma_input_is_starting() — todavía se está abriendo
 *
 * Un consumidor que trate `true` como "ya hay señal" va a leer el medidor un
 * instante antes de tiempo. Uno que trate el `false` de wma_input_is_running()
 * como error va a mostrar "permiso denegado" mientras el stream todavía abre:
 * **hay que preguntar por wma_input_is_starting() antes de concluir que falló.**
 */
WMA_API bool wma_input_start(WmaEngine* engine);

/**
 * Whether a start requested by wma_input_start() is still opening the stream.
 *
 * Es la mitad que falta para distinguir "todavía no" de "no": mientras esto sea
 * true, que wma_input_is_running() diga false no es una negativa.
 */
WMA_API bool wma_input_is_starting(const WmaEngine* engine);

/** Stop the input stream. */
WMA_API void wma_input_stop(WmaEngine* engine);

/** Check if input stream is running. */
WMA_API bool wma_input_is_running(const WmaEngine* engine);

/** Set input source (0=DEFAULT, 1=MIC, 2=USB). */
WMA_API void wma_input_set_source(WmaEngine* engine, int source);

/** Get current input source. */
WMA_API int wma_input_get_source(const WmaEngine* engine);

/** Set input gain in dB. */
WMA_API void wma_input_set_gain(WmaEngine* engine, float gain_db);

/** Get input gain in dB. */
WMA_API float wma_input_get_gain(const WmaEngine* engine);

/** Enable/disable noise gate. */
WMA_API void wma_input_set_noise_gate(WmaEngine* engine, bool enabled);

/** Check if noise gate is enabled. */
WMA_API bool wma_input_is_noise_gate_enabled(const WmaEngine* engine);

/** Set the noise gate threshold in dB. */
WMA_API void wma_input_set_noise_gate_threshold(WmaEngine* engine, float threshold_db);

/** Check if the noise gate is currently open (letting signal through). */
WMA_API bool wma_input_is_noise_gate_open(const WmaEngine* engine);

/** Get input level for a channel (dB, -120 to 0). Returns -100 with no input. */
WMA_API float wma_input_get_level(const WmaEngine* engine, int channel);

/** Get input level for a channel, linear (0 to 1). */
WMA_API float wma_input_get_level_linear(const WmaEngine* engine, int channel);

/** Check if input is clipping. */
WMA_API bool wma_input_is_clipping(const WmaEngine* engine);

/** Get the input latency in milliseconds. */
WMA_API float wma_input_get_latency_ms(const WmaEngine* engine);

/** Number of floats wma_input_get_metering_snapshot() writes. */
#define WMA_INPUT_METERING_VALUES 7

/**
 * Read the whole input meter in one call.
 *
 * A UI meter polls these seven values every frame, and going through the
 * getters one by one costs a language-boundary crossing each (eight per tick at
 * 60 fps ≈ 480/s on the JNI side). This exists so that stays one crossing.
 *
 * @param[out] out_values  WMA_INPUT_METERING_VALUES floats, in this order:
 *                         [0] level dB ch0     [1] level dB ch1
 *                         [2] level linear ch0 [3] level linear ch1
 *                         [4] clipping (1/0)   [5] noise gate open (1/0)
 *                         [6] latency ms
 * @return false if there is no input node, or out_values is NULL. The buffer is
 *         left untouched in that case — the caller is meant to fall back to the
 *         individual getters rather than read zeros as real measurements.
 */
WMA_API bool wma_input_get_metering_snapshot(const WmaEngine* engine, float* out_values);

/** Enable/disable input monitoring. */
WMA_API void wma_input_set_monitoring(WmaEngine* engine, bool enabled);

/** Check if input monitoring is enabled. */
WMA_API bool wma_input_is_monitoring_enabled(const WmaEngine* engine);

/** Set the monitoring volume. Clamped to 0.0 – 1.0. */
WMA_API void wma_input_set_monitoring_volume(WmaEngine* engine, float volume);

/** Get the monitoring volume (0.0 – 1.0). */
WMA_API float wma_input_get_monitoring_volume(const WmaEngine* engine);

/** Release the input node entirely. */
WMA_API void wma_input_release(WmaEngine* engine);

/* ================================================================
 * 13. Dual Touch
 * ================================================================ */

/** Enable/disable dual touch mode. */
WMA_API void wma_set_dual_touch_mode(WmaEngine* engine, bool enabled);

/** Get dual touch mode state. */
WMA_API bool wma_get_dual_touch_mode(const WmaEngine* engine);

/**
 * Update dual touch parameters.
 * Each touch: x, y, freq, amp, pressure. Plus distance and angle.
 */
WMA_API void wma_set_dual_touch(WmaEngine* engine,
                                 float x1, float y1, float freq1, float amp1, float pressure1,
                                 float x2, float y2, float freq2, float amp2, float pressure2,
                                 float distance, float angle);

/** Set dual touch mix mode (0–5). */
WMA_API void wma_set_dual_touch_mix_mode(WmaEngine* engine, int mode_id);

/** Set secondary oscillator type for dual touch. */
WMA_API void wma_set_secondary_oscillator_type(WmaEngine* engine, int type_id);

/* ================================================================
 * 14. Voice System
 * ================================================================ */

/** Enable/disable polyphonic voice system. */
WMA_API void wma_voice_enable(WmaEngine* engine, bool enable);

/** Check if voice system is enabled. */
WMA_API bool wma_voice_is_enabled(const WmaEngine* engine);

/**
 * Update multi-touch voice data.
 * @param touch_data  Flat float array: [x, y, freq, amp, pressure, pointerId] per touch
 * @param count       Number of active touches
 */
WMA_API void wma_voice_update_multi_touch(WmaEngine* engine,
                                           const float* touch_data, int count);

/** Get number of active voices. */
WMA_API int wma_voice_get_active_count(const WmaEngine* engine);

/** Set maximum number of simultaneous voices. */
WMA_API void wma_voice_set_max(WmaEngine* engine, int max_voices);

/** Set voice stealing strategy. */
WMA_API void wma_voice_set_stealing_strategy(WmaEngine* engine, int strategy);

/**
 * Trigger chord notes.
 * @param frequencies      Array of frequencies in Hz
 * @param count            Number of notes
 * @param amplitude        Common amplitude (0.0–1.0)
 * @param oscillator_type  Oscillator type for chord voices
 */
WMA_API void wma_voice_trigger_chord(WmaEngine* engine,
                                      const float* frequencies, int count,
                                      float amplitude, int oscillator_type);

/**
 * Update chord note frequencies.
 * @param frequencies  Array of frequencies in Hz
 * @param count        Number of notes
 * @param amplitude    Common amplitude (0.0–1.0)
 */
WMA_API void wma_voice_update_chord(WmaEngine* engine,
                                     const float* frequencies, int count,
                                     float amplitude);

/** Release all chord notes. */
WMA_API void wma_voice_release_chord(WmaEngine* engine);

/* ================================================================
 * 15. Vocoder
 * ================================================================ */

/** Set vocoder carrier source (true=internal synth, false=input signal). */
WMA_API void wma_vocoder_set_carrier_source(WmaEngine* engine, bool use_internal);

/** Set vocoder carrier frequency in Hz (20–2000). */
WMA_API void wma_vocoder_set_carrier_freq(WmaEngine* engine, float frequency);

/** Check if a vocoder effect exists in the chain. */
WMA_API bool wma_vocoder_has_effect(const WmaEngine* engine);

/** Set vocoder modulator source (true=external mic, false=self-vocoding). */
WMA_API void wma_vocoder_set_modulator_source(WmaEngine* engine, bool use_external);

/* ================================================================
 * 16. Backend
 * ================================================================ */

/** Enable/disable BackendManager for audio lifecycle. */
WMA_API void wma_set_use_backend_manager(WmaEngine* engine, bool use);

/**
 * Select audio backend.
 * @param backend_id  Backend type enum value
 * @return true on success
 */
WMA_API bool wma_select_backend(int backend_id);

/** Get current backend type. */
WMA_API int wma_get_backend_type(void);

/** Check if USB backend is available. */
WMA_API bool wma_is_usb_available(void);

/** Set USB streaming mode (0=PLAYBACK_ONLY, 1=CAPTURE_ONLY, 2=FULL_DUPLEX). */
WMA_API void wma_set_usb_streaming_mode(int mode_id);

/** Configure USB backend parameters. */
WMA_API void wma_configure_usb_backend(int sample_rate, int channels, int bit_depth);

/**
 * Initialize a USB audio device.
 * @param file_descriptor  USB file descriptor
 * @param usbfs_path       USB filesystem path
 * @return true on success
 */
WMA_API bool wma_usb_init_device(int file_descriptor, const char* usbfs_path);

/** Close the USB audio device and fall back to Oboe. */
WMA_API void wma_usb_close_device(void);

/**
 * Select the USB latency profile (Fase 1).
 * Re-parametrizes the transfer pipeline (iso transfer duration, URBs in flight,
 * pacer jitter budget, DSP block size, ring capacity).
 *
 * Latched and applied on the next stream start (same semantics as the USB
 * streaming mode); a running stream keeps its current profile until restarted.
 *
 * @param profile  0 = SAFE (current behavior), 1 = LOW_LATENCY (~10–14 ms).
 * @return true if latched; false if no USB backend.
 */
WMA_API bool wma_usb_set_latency_profile(int profile);

/* ================================================================
 * 17. XY Mapping / Automation
 * ================================================================ */

/** Set an automation parameter on an effect (XY-driven). */
WMA_API void wma_set_automation_param(WmaEngine* engine, int effect_index, int param_id, float xy_value);

/**
 * Set XY mapping configuration for an axis.
 * @param axis        0=X, 1=Y, 2=DEPTH
 * @param curve       Mapping curve (0=LINEAR, 1=EXP, 2=LOG, 3=S_CURVE)
 * @param polarity    0=UNIPOLAR, 1=BIPOLAR
 * @param inverted    Invert mapping direction
 */
WMA_API void wma_set_mapping_config(WmaEngine* engine, int axis,
                                     int effect_index, int param_id,
                                     int curve, int polarity,
                                     float map_min, float map_max, bool inverted);

/** Clear mapping configuration for an axis. */
WMA_API void wma_clear_mapping_config(WmaEngine* engine, int axis);

/** Set depth control value (0.0–1.0). */
WMA_API void wma_set_depth_value(WmaEngine* engine, float value);

/** Apply automation for an axis with a normalized value. */
WMA_API void wma_apply_automation(WmaEngine* engine, int axis, float normalized_value);

/* ================================================================
 * 18. Arpeggiator
 * ================================================================ */

WMA_API void wma_arp_set_enabled(WmaEngine* engine, bool enabled);       /* RT-safe */
WMA_API bool wma_arp_is_enabled(const WmaEngine* engine);
WMA_API void wma_arp_set_pattern(WmaEngine* engine, int pattern_id);     /* RT-safe */
WMA_API void wma_arp_set_subdivision(WmaEngine* engine, float beats_per_step);  /* RT-safe */
WMA_API void wma_arp_set_octave_range(WmaEngine* engine, int octaves);   /* RT-safe */
WMA_API void wma_arp_set_gate_length(WmaEngine* engine, float gate);     /* RT-safe */
WMA_API void wma_arp_set_swing(WmaEngine* engine, float swing);          /* RT-safe */
WMA_API void wma_arp_set_latch(WmaEngine* engine, bool latch);           /* RT-safe */
WMA_API void wma_arp_set_velocity(WmaEngine* engine, float velocity);    /* RT-safe */
WMA_API void wma_arp_set_velocity_variation(WmaEngine* engine, float variation); /* RT-safe */
WMA_API void wma_arp_set_probability(WmaEngine* engine, float probability);      /* RT-safe */

/**
 * Set scale intervals for arpeggiator quantization.
 * @param intervals  Array of semitone offsets (e.g., [0,2,4,5,7,9,11])
 * @param count      Number of intervals
 */
WMA_API void wma_arp_set_scale_intervals(WmaEngine* engine, const int* intervals, int count);

WMA_API void wma_arp_set_touch_active(WmaEngine* engine, bool active);   /* RT-safe */
WMA_API void wma_arp_set_base_freq(WmaEngine* engine, float frequency);  /* RT-safe */
WMA_API int  wma_arp_get_current_step(const WmaEngine* engine);
WMA_API int  wma_arp_get_total_steps(const WmaEngine* engine);
WMA_API void wma_arp_set_ratchet(WmaEngine* engine, bool active);        /* RT-safe */
WMA_API void wma_arp_regenerate(WmaEngine* engine);
WMA_API bool wma_arp_is_gate_open(const WmaEngine* engine);

/* ================================================================
 * 19. Looper
 * ================================================================ */

/**
 * Prepare a looper track with given length.
 * @param track_index   Track index (0–7)
 * @param length_frames Duration in frames
 * @param sample_rate   Stream sample rate
 * @return WMA_OK on success, WMA_ERROR_MEMORY on allocation failure
 */
WMA_API WmaResult wma_looper_prepare_track(WmaEngine* engine, int track_index,
                                            int length_frames, int sample_rate);

WMA_API void wma_looper_start_recording(WmaEngine* engine, int track_index);
WMA_API void wma_looper_stop_recording(WmaEngine* engine);
WMA_API void wma_looper_start_overdub(WmaEngine* engine, int track_index);
WMA_API void wma_looper_stop_all(WmaEngine* engine);
WMA_API void wma_looper_pause(WmaEngine* engine);
WMA_API void wma_looper_resume(WmaEngine* engine);
WMA_API void wma_looper_set_track_muted(WmaEngine* engine, int track_index, bool muted);
WMA_API void wma_looper_set_track_pan(WmaEngine* engine, int track_index, float pan);
WMA_API void wma_looper_set_track_volume(WmaEngine* engine, int track_index, float volume);
WMA_API void wma_looper_set_track_speed(WmaEngine* engine, int track_index, float speed);
WMA_API float wma_looper_get_track_speed(const WmaEngine* engine, int track_index);
WMA_API void wma_looper_clear_track(WmaEngine* engine, int track_index);
WMA_API void wma_looper_clear_all(WmaEngine* engine);
WMA_API void wma_looper_set_enabled(WmaEngine* engine, bool enabled);
WMA_API float wma_looper_get_progress(const WmaEngine* engine);                 /* RT-safe */
WMA_API float wma_looper_get_record_progress(const WmaEngine* engine);          /* RT-safe */
WMA_API float wma_looper_get_track_peak(const WmaEngine* engine, int track_index); /* RT-safe */
WMA_API bool wma_looper_is_track_active(const WmaEngine* engine, int track_index);
WMA_API bool wma_looper_is_playing(const WmaEngine* engine);
WMA_API bool wma_looper_is_recording(const WmaEngine* engine);
WMA_API int  wma_looper_get_master_loop_frames(const WmaEngine* engine);
WMA_API void wma_looper_set_free_length(WmaEngine* engine, bool free_length);
WMA_API void wma_looper_pause_track(WmaEngine* engine, int track_index);
WMA_API void wma_looper_resume_track(WmaEngine* engine, int track_index);
WMA_API bool wma_looper_is_track_playing(const WmaEngine* engine, int track_index);
WMA_API float wma_looper_get_track_progress(const WmaEngine* engine, int track_index); /* RT-safe */
WMA_API int  wma_looper_get_track_length_frames(const WmaEngine* engine, int track_index);
WMA_API void wma_looper_reset_track_playhead(WmaEngine* engine, int track_index);
WMA_API bool wma_looper_save_undo(WmaEngine* engine, int track_index);
WMA_API bool wma_looper_restore_undo(WmaEngine* engine, int track_index);
WMA_API bool wma_looper_has_undo(const WmaEngine* engine, int track_index);

/**
 * Get waveform summary bins for a looper track.
 * @param track_index  Track index
 * @param buffer       Output float buffer
 * @param max_bins     Size of output buffer
 * @return Number of bins written
 */
WMA_API int wma_looper_get_track_waveform(const WmaEngine* engine, int track_index,
                                           float* buffer, int max_bins);

WMA_API void  wma_looper_set_master_volume(WmaEngine* engine, float volume);
WMA_API float wma_looper_get_master_volume(const WmaEngine* engine);

/**
 * Set loop region for a track (start/end in frames).
 *
 * 64-bit on purpose, and it has to stay that way: AudioLooper::setTrackLoopRegion
 * takes int64_t and saturates into int32 itself, because TrackBuffer still stores
 * frames as int32 while the API has to survive a high-tier device recording for
 * hours. This declaration used to say `int`, which made the C API the one narrow
 * link in a chain that is 64-bit from Kotlin (Long) all the way down — the
 * saturation could never run, because the value was already truncated by the
 * time it arrived.
 */
WMA_API void wma_looper_set_track_loop_region(WmaEngine* engine, int track_index,
                                               int64_t start_frame, int64_t end_frame);

/** Reset loop region to full track length. */
WMA_API void wma_looper_reset_track_loop_region(WmaEngine* engine, int track_index);

WMA_API int wma_looper_get_track_loop_start(const WmaEngine* engine, int track_index);
WMA_API int wma_looper_get_track_loop_end(const WmaEngine* engine, int track_index);

/** Trigger a metronome click (downbeat vs subdivision). */
WMA_API void wma_looper_trigger_click(WmaEngine* engine, bool is_downbeat);

/* ---------------- Export / import ----------------
 *
 * Bit depth is an int here —16, 24, or 32 for float— because wav::BitDepth is a
 * C++ enum and this header is C. Anything else is treated as 16, which is what
 * the JNI did. The mapping used to be written out three times up in the JNI; it
 * lives in one place now.
 */

/**
 * Options for a mix or stem export.
 *
 * The first struct in this header, and worth a word on why. The alternative was
 * a ten-argument function, duplicated across mix and stems, six of whose
 * arguments are same-typed ints — the shape where a caller transposes two and
 * nothing complains. A named field cannot be transposed silently, and cinterop
 * turns this into a named Kotlin type rather than ten positional arguments.
 *
 * Zero-initialising this struct is NOT the same as the defaults: pass it to
 * wma_looper_export_options_default() first, or set every field. The "<= 0 means
 * use the default" rules below exist so a caller can leave what it does not care
 * about alone, the same contract wma_looper_set_capabilities() uses.
 */
typedef struct WmaExportOptions {
    /** 16, 24, or 32 (float). Anything else is treated as 16. */
    int bit_depth;
    /** Iterations of the loop to write. <= 0 means 1. */
    int repeat_loops;
    /** Leading silence, in beats. Converted through the Transport. <= 0 = none. */
    int count_in_beats;
    /** True-peak limiter instead of a tanh soft-clip. */
    bool apply_limiter;
    /** BPM tag for the file. <= 0 means "use the Transport's current BPM". */
    int bpm;
    /** WAV metadata. NULL leaves the field empty. Ignored by stem export. */
    const char* project_name;
    const char* artist;
    const char* comment;
} WmaExportOptions;

/** The documented defaults: 16-bit, one iteration, no count-in, limiter on. */
WMA_API WmaExportOptions wma_looper_export_options_default(void);

/**
 * Export the mix to a WAV file, with options and metadata. NOT RT-safe.
 * Passing NULL for @p options uses the defaults.
 */
WMA_API bool wma_looper_export_mix_v2(WmaEngine* engine, const char* file_path,
                                      const WmaExportOptions* options);

/**
 * Export every active track as its own WAV file into @p directory. NOT RT-safe.
 * @return number of stems written, or -1 on failure.
 */
WMA_API int wma_looper_export_stems(WmaEngine* engine, const char* directory,
                                     const WmaExportOptions* options);

/**
 * Session capture: write a track's FULL buffer, ignoring the loop region, at the
 * given bit depth. 32 (float) is a lossless round-trip.
 */
WMA_API bool wma_looper_capture_track(WmaEngine* engine, int track_index,
                                       const char* file_path, int bit_depth);

/** Progress of the export in flight, 0..1. */
WMA_API float wma_looper_get_export_progress(const WmaEngine* engine);

/** Ask the export in flight to stop. */
WMA_API void wma_looper_cancel_export(WmaEngine* engine);

/** Sample rate to write exports at. */
WMA_API void wma_looper_set_export_sample_rate(WmaEngine* engine, int sample_rate);

WMA_API bool wma_looper_is_export_in_progress(const WmaEngine* engine);

/* Export telemetry. */
WMA_API int64_t wma_looper_get_exports_completed(const WmaEngine* engine);
WMA_API int64_t wma_looper_get_exports_failed(const WmaEngine* engine);
WMA_API int64_t wma_looper_get_stems_written(const WmaEngine* engine);

/* ---------------- Track editing & analysis ---------------- */

/** Abort the recording in progress, discarding the take. */
WMA_API void wma_looper_abort_recording(WmaEngine* engine);

/**
 * Start recording on @p track_index, seeded with the last `pre_roll_ms` of
 * post-FX output so the take does not begin at the user's reaction time.
 *
 * Clamped to 0..1000 ms — 1 s is what the pre-roll ring holds. 0 is a plain
 * start. Allocates on the calling thread; never call from the audio thread.
 */
WMA_API void wma_looper_start_recording_with_pre_roll(WmaEngine* engine,
                                                       int track_index,
                                                       int pre_roll_ms);

/**
 * Allocate a track quantized to @p bars musical bars at the Transport's current
 * tempo and meter. Like the arming calls, this composes Transport and looper so
 * the two cannot disagree about how long a bar is.
 *
 * @return the allocated length in frames, or -1 on failure (no tempo yet, a bad
 *         bar count, or the allocation itself failing).
 */
WMA_API int wma_looper_prepare_track_bars(WmaEngine* engine, int track_index,
                                           int bars, int sample_rate);

/** Trim a track's buffer to its recorded length. No-op while recording. */
WMA_API bool wma_looper_trim_track(WmaEngine* engine, int track_index);

/**
 * Bar-snap and seam-bake a free take's loop region. Pads with silence if
 * `loop_end` runs past the recording and bakes the wrap-mix when
 * `tail_frames > 0`. No-op while recording into or exporting this track.
 */
WMA_API bool wma_looper_finalize_free_loop(WmaEngine* engine, int track_index,
                                            int loop_start, int loop_end,
                                            int tail_frames);

/**
 * First and last audible frame of a track, for trimming the silence around a
 * free take. `out_last` is exclusive.
 *
 * Two out-params rather than the packed int64 the JNI returns: that encoding
 * exists because a JNI call cannot hand back two ints, and pushing it into the C
 * API would make every caller unpack a sign-sensitive bitfield for no reason.
 * The JNI still packs, on its own side.
 *
 * @return false if the track index is invalid; out-params are untouched then.
 */
WMA_API bool wma_looper_find_content_bounds(const WmaEngine* engine, int track_index,
                                             float threshold_ratio,
                                             int* out_first, int* out_last);

/**
 * Detect onsets in a track, for deriving a tempo from a free take.
 *
 * @param out_onsets   Frame index of each onset
 * @param max_onsets   Capacity of out_onsets; nothing is written past it
 * @return number of onsets written, never negative
 */
WMA_API int wma_looper_detect_onsets(const WmaEngine* engine, int track_index,
                                      int* out_onsets, int max_onsets,
                                      int hop_frames, float sensitivity);

/* ---------------- Per-track playback modes ---------------- */

/** How many times a track plays before stopping. 0 = loop forever. */
WMA_API void wma_looper_set_track_play_count(WmaEngine* engine, int track_index,
                                              int plays);

/** Percussion mode: retrigger from the top instead of pitch-shifting on speed. */
WMA_API void wma_looper_set_track_percussion_mode(WmaEngine* engine, int track_index,
                                                   bool percussion);
WMA_API bool wma_looper_is_track_percussion_mode(const WmaEngine* engine,
                                                  int track_index);

/** Wrap-mix tail window in ms, used when baking a free take's seam. */
WMA_API void wma_looper_set_tail_ms(WmaEngine* engine, int ms);
WMA_API int  wma_looper_get_tail_ms(const WmaEngine* engine);

/**
 * Configure runtime capabilities for the device tier.
 *
 * Each argument is "leave the current value alone" when <= 0, which is what lets
 * a caller set only the one it cares about. `budget_bytes` is 64-bit: a memory
 * budget is exactly the kind of number that outgrows an int.
 */
WMA_API void wma_looper_set_capabilities(WmaEngine* engine, int64_t budget_bytes,
                                          int max_tracks, int max_free_seconds);

/* ---------------- Armed recording ----------------
 *
 * These are not thin wrappers over AudioLooper: they read the Transport play
 * position and hand it to the looper in one go. That composition used to live in
 * the JNI, and it matters where it happens — reading the anchor here means the
 * trigger frame is sampled atomically on the calling thread, so UI-thread jitter
 * between "what time is it" and "arm at that time" cannot leak into the trigger.
 */

/**
 * Arm @p track_index to start recording at the next bar boundary.
 * @return the absolute trigger frame (>= 0), or -1 if nothing was armed.
 */
WMA_API int64_t wma_looper_arm_at_next_bar(WmaEngine* engine, int track_index);

/**
 * Arm @p track_index to start recording `offset_frames` after the current
 * Transport position. This is the latency-compensated entry point: pass
 * (count_in_frames + round_trip_latency_frames) so capture begins exactly that
 * far ahead, putting the player's first downbeat at loop frame 0.
 *
 * A negative offset is treated as 0.
 * @return the absolute trigger frame (>= 0), or -1 if nothing was armed.
 */
WMA_API int64_t wma_looper_arm_in_frames(WmaEngine* engine, int track_index,
                                          int64_t offset_frames);

/**
 * Sync-armed overdub: phase-lock a new layer to the loop already playing. Arms
 * at the reference track's next boundary plus `latency_frames`, and tags the
 * take so finalizing cancels the round-trip latency.
 *
 * @return the trigger frame, or -1 if no reference track is playing — the
 *         caller is expected to fall back to wma_looper_arm_in_frames().
 */
WMA_API int64_t wma_looper_arm_synced_to_loop(WmaEngine* engine, int track_index,
                                              int64_t latency_frames);

/**
 * Quantized sync-arm: start at the next multiple of `quantum_frames` within the
 * reference cycle instead of waiting out the rest of the loop, so a punch-in is
 * not stuck behind three spare bars. The rotated start offset is cancelled when
 * the take is finalized, so playback still phase-locks to the reference.
 *
 * `quantum_frames <= 0` behaves exactly like wma_looper_arm_synced_to_loop().
 */
WMA_API int64_t wma_looper_arm_synced_to_loop_quantized(WmaEngine* engine,
                                                         int track_index,
                                                         int64_t latency_frames,
                                                         int quantum_frames);

/** Cancel a pending armed recording. No-op if nothing is armed. */
WMA_API void wma_looper_cancel_arm(WmaEngine* engine);

/** Track index waiting on its trigger, or -1 if none. */
WMA_API int wma_looper_get_armed_track(const WmaEngine* engine);

/* ---------------- Telemetry (lock-free counters) ---------------- */

/** How many armed recordings have fired since the last reset. */
WMA_API int64_t wma_looper_get_armed_triggered(const WmaEngine* engine);

/** Frames the looper had to drop rather than block the audio thread. */
WMA_API int64_t wma_looper_get_frames_dropped(const WmaEngine* engine);

/** State-change events dropped because the dispatcher queue was full. */
WMA_API int64_t wma_looper_get_dropped_events(const WmaEngine* engine);

/** Zero every telemetry counter above. */
WMA_API void wma_looper_reset_telemetry(WmaEngine* engine);

/**
 * Export looper mix to a WAV file. NOT RT-safe.
 * @param file_path  Absolute path for the output WAV file
 * @return true on success
 */
WMA_API bool wma_looper_export_mix(WmaEngine* engine, const char* file_path);

/**
 * Export a single track to a WAV file. NOT RT-safe.
 * @return true on success
 */
WMA_API bool wma_looper_export_track(WmaEngine* engine, int track_index, const char* file_path);

/**
 * Import a WAV file into a looper track. NOT RT-safe.
 * @param sample_rate  Source file sample rate (for resampling if needed)
 * @return true on success
 */
WMA_API bool wma_looper_import_track(WmaEngine* engine, int track_index,
                                      const char* file_path, int sample_rate);

/* ================================================================
 * 20. Transport (musical clock & metronome)
 * ================================================================
 *
 * The Transport owns the musical clock: BPM, beats-per-bar and the sample
 * rate, plus an RT-safe metronome scheduler. The UI arms a schedule once and
 * the audio thread emits the clicks from the render callback, which is what
 * removes the "the click sometimes plays and sometimes doesn't" jank of a
 * UI-driven timer.
 *
 * Two neighbours of this section, deliberately left where they are:
 *   - BPM is set through wma_set_bpm() (§3). It fans out to the tempo-synced
 *     effects AND to the Transport, so there is no separate transport BPM
 *     setter — adding one would let the two drift apart.
 *   - wma_looper_trigger_click() (§19) fires a single click immediately,
 *     bypassing the scheduler. It keeps its wma_looper_ name because the click
 *     generator belongs to the looper and the name is already shipped.
 */

/** Set beats per bar (clamped to 1..16). */
WMA_API void wma_transport_set_beats_per_bar(WmaEngine* engine, int beats_per_bar);

/** Get beats per bar. Returns 4 (the default meter) with no engine. */
WMA_API int wma_transport_get_beats_per_bar(const WmaEngine* engine);

/** Frames per beat at the current BPM and sample rate. 0 with no engine. */
WMA_API int wma_transport_frames_per_beat(const WmaEngine* engine);

/**
 * Frames in `bars` complete bars at the current BPM / sample rate / meter.
 * Returns 0 for bars <= 0 and 0 with no engine. Used to quantize loop lengths.
 */
WMA_API int wma_transport_frames_per_bar(const WmaEngine* engine, int bars);

/**
 * Schedule `beats` clicks at beat intervals, starting on the next audio block.
 *
 * @param beats               Number of clicks (e.g. 4 for a one-bar pre-count).
 *                            <= 0 stops the metronome instead of starting it.
 * @param first_is_downbeat   First click is a downbeat (higher, louder).
 * @param every_beat_pattern  Clicks where index % beats_per_bar == 0 are
 *                            downbeats. Overrides first_is_downbeat.
 */
WMA_API void wma_transport_start_metronome(WmaEngine* engine, int beats,
                                            bool first_is_downbeat,
                                            bool every_beat_pattern);

/**
 * Run the metronome until wma_transport_stop_metronome() — the "click as a
 * reference while recording" mode. The scheduler does not count down.
 */
WMA_API void wma_transport_start_metronome_continuous(WmaEngine* engine,
                                                       bool every_beat_pattern);

/** Cancel any in-flight schedule. A click already sounding decays naturally. */
WMA_API void wma_transport_stop_metronome(WmaEngine* engine);

/** True while a schedule is armed, counted or continuous. */
WMA_API bool wma_transport_is_metronome_running(const WmaEngine* engine);

/** True while the metronome is in continuous mode. */
WMA_API bool wma_transport_is_metronome_continuous(const WmaEngine* engine);

/**
 * Clicks left in a counted schedule. 0 when idle.
 *
 * In CONTINUOUS mode this returns the scheduler's arming sentinel (1), not a
 * count of anything — a continuous schedule has no remaining beats. Gate on
 * wma_transport_is_metronome_continuous() before reading it.
 */
WMA_API int wma_transport_get_remaining_beats(const WmaEngine* engine);

/* ================================================================
 * 21. Diagnostics & Latency
 * ================================================================
 *
 * The portable half of the latency diagnostics. The rest of what the Android
 * bridge reports —AAudio vs OpenSL ES, exclusive vs shared, low-latency vs
 * normal— are properties of an `oboe::AudioStream` and have no counterpart
 * anywhere else, so they stay in the JNI, appended to what these return.
 *
 * There is deliberately no wma_get_latency_info() batch here. The numbers a
 * caller wants are already reachable as wma_get_stream_info() (§2) and
 * wma_input_get_latency_ms() (§12); a third function returning the same values
 * packed into an array would be one more thing to keep in agreement for no
 * gain. Metering has such a batch because a UI polls it every frame — nobody
 * polls latency.
 */

/**
 * Recommended buffer size in frames for a target output latency: the smallest
 * power of two whose duration is at or above `target_latency_ms`, clamped to
 * [64, 2048].
 *
 * Resolved against the sample rate actually in effect — the running stream if
 * there is one, otherwise the preferred rate, otherwise 48000. It never
 * silently assumes 48 kHz on a device configured for 44.1.
 *
 * @return frames, or -1 for a non-positive target.
 */
WMA_API int wma_get_recommended_buffer_size(const WmaEngine* engine,
                                             float target_latency_ms);

/**
 * Write a human-readable latency report into `buffer`.
 *
 * Always NUL-terminates when buffer_size > 0, and truncates rather than
 * overflowing.
 *
 * @return the length the full report would have, excluding the NUL — the
 *         snprintf convention. A value >= buffer_size means it was truncated.
 */
WMA_API int wma_get_latency_report(const WmaEngine* engine,
                                    char* buffer, int buffer_size);

/* ================================================================
 * 22. Waveform & Metering
 * ================================================================ */

/**
 * Get waveform visualization samples.
 * @param buffer    Output float buffer
 * @param max_size  Buffer capacity
 * @return Number of samples written
 */
WMA_API int wma_get_waveform_samples(WmaEngine* engine, float* buffer, int max_size);

/** Get output peak level for a channel (0=L, 1=R). Linear scale. */
WMA_API float wma_get_output_peak(const WmaEngine* engine, int channel);

/** Get output RMS level for a channel. Linear scale. */
WMA_API float wma_get_output_rms(const WmaEngine* engine, int channel);

/** Get output peak level in dB (returns -100 dB if silent). */
WMA_API float wma_get_output_peak_db(const WmaEngine* engine, int channel);

/** Get output RMS level in dB (returns -100 dB if silent). */
WMA_API float wma_get_output_rms_db(const WmaEngine* engine, int channel);

/**
 * Get all output levels in one call (efficient single-query metering).
 * @param[out] out_levels  Float array of at least 4: [peakL, peakR, rmsL, rmsR]
 */
WMA_API void wma_get_output_levels(const WmaEngine* engine, float* out_levels);

/* ================================================================
 * 23. Configuration / Logging
 * ================================================================ */

/**
 * Set a global log callback. Pass NULL to revert to platform default.
 * Thread-safe but NOT RT-safe.
 */
WMA_API void wma_set_log_callback(WmaLogCallback callback);

/**
 * Log capture — a pull-based ring of recent log lines (App V §3.2).
 *
 * Distinct from wma_set_log_callback(), which is push-based and delivers each
 * line as it happens. Capture exists so a UI can show the last N lines on
 * demand without keeping a live callback installed; the ring holds 4000 lines
 * and drops the oldest when full.
 */

/** Enable or disable log capture. Disabled capture costs one relaxed load. */
WMA_API void wma_log_capture_set_enabled(bool enabled);

/** Lines dropped because the ring was full. Not reset by a drain. */
WMA_API int wma_log_capture_dropped(void);

/**
 * An owned batch of drained log lines.
 *
 * Draining is destructive — the lines leave the ring — so the batch exists to
 * hand them over whole rather than into a caller buffer that might be too
 * small. Every batch must be released with wma_log_batch_free().
 */
typedef struct WmaLogBatch WmaLogBatch;

/**
 * Drain everything captured since the last call.
 * @return A batch the caller owns, or NULL if allocation failed. Never NULL
 *         merely because there was nothing to drain — that is an empty batch.
 */
WMA_API WmaLogBatch* wma_log_capture_drain(void);

/** Number of lines in the batch. 0 for NULL. */
WMA_API int wma_log_batch_count(const WmaLogBatch* batch);

/**
 * Line at `index`, valid until the batch is freed.
 * @return NULL if the batch is NULL or the index is out of range.
 */
WMA_API const char* wma_log_batch_line(const WmaLogBatch* batch, int index);

/** Release a batch. NULL is a no-op. */
WMA_API void wma_log_batch_free(WmaLogBatch* batch);

/** Get version string (e.g., "0.1.0"). */
WMA_API const char* wma_get_version(void);

/* ================================================================ */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WATERMELON_AUDIO_H */
