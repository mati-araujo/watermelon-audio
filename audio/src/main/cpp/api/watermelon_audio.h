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

#define WMA_VERSION_MAJOR 0
#define WMA_VERSION_MINOR 1
#define WMA_VERSION_PATCH 0
#define WMA_VERSION_STRING "0.1.0"

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
 * @param fade_time_ms  Fade-in duration in ms (0 = instant)
 * @return WMA_OK on success
 */
WMA_API WmaResult wma_engine_start(WmaEngine* engine, int fade_time_ms);

/**
 * Stop the audio stream (blocking — waits for callbacks to finish).
 * @param engine        Engine handle
 * @param fade_time_ms  Fade-out duration in ms (0 = instant)
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

/** Start/update a SoundFont note. */  /* RT-safe */
WMA_API void wma_sf_note_on(WmaEngine* engine, int touch_id, int midi_note, float velocity);

/** Release a SoundFont note. */  /* RT-safe */
WMA_API void wma_sf_note_off(WmaEngine* engine, int touch_id);

/** Release all SoundFont notes. */  /* RT-safe */
WMA_API void wma_sf_note_off_all(WmaEngine* engine);

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

/** Set one effect parameter. */
WMA_API WmaResult wma_effect_set_param(WmaEngine* engine, int index, int param_id, float value);

/** Get one effect parameter. */
WMA_API float wma_effect_get_param(const WmaEngine* engine, int index, int param_id);

/**
 * Batch-set parameters on a single effect.
 * @param param_ids  Array of parameter IDs
 * @param values     Array of parameter values
 * @param count      Number of elements
 */
WMA_API WmaResult wma_effect_set_params_batch(WmaEngine* engine, int index,
                                               const int* param_ids, const float* values,
                                               int count);

/** Set bypass state of an effect. */
WMA_API WmaResult wma_effect_set_bypass(WmaEngine* engine, int index, bool bypass);

/** Check if an effect is bypassed. */
WMA_API bool wma_effect_is_bypassed(const WmaEngine* engine, int index);

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
 * NOTE: This is a simplified version. Full mode transitions (InputNode
 * management, vocoder config, USB path) should be handled by the platform
 * layer — this function only stores the mode and configures the oscillator.
 */
WMA_API void wma_set_audio_mode(WmaEngine* engine, int mode);

/** Get current audio mode. */
WMA_API int wma_get_audio_mode(const WmaEngine* engine);

/** Check if a mode transition is in progress. */
WMA_API bool wma_is_in_mode_transition(const WmaEngine* engine);

/** Get mode transition progress (0.0–1.0). */
WMA_API float wma_get_mode_transition_progress(const WmaEngine* engine);

/** Check if a given mode requires audio input. */
WMA_API bool wma_mode_requires_input(int mode);

/* ================================================================
 * 12. Input
 * ================================================================ */

/**
 * Start the input stream (microphone).
 * @return true on success
 */
WMA_API bool wma_input_start(WmaEngine* engine);

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

/** Get input level for a channel (dB). */
WMA_API float wma_input_get_level(const WmaEngine* engine, int channel);

/** Check if input is clipping. */
WMA_API bool wma_input_is_clipping(const WmaEngine* engine);

/** Enable/disable input monitoring. */
WMA_API void wma_input_set_monitoring(WmaEngine* engine, bool enabled);

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

/** Set loop region for a track (start/end in frames). */
WMA_API void wma_looper_set_track_loop_region(WmaEngine* engine, int track_index,
                                               int start_frame, int end_frame);

/** Reset loop region to full track length. */
WMA_API void wma_looper_reset_track_loop_region(WmaEngine* engine, int track_index);

WMA_API int wma_looper_get_track_loop_start(const WmaEngine* engine, int track_index);
WMA_API int wma_looper_get_track_loop_end(const WmaEngine* engine, int track_index);

/** Trigger a metronome click (downbeat vs subdivision). */
WMA_API void wma_looper_trigger_click(WmaEngine* engine, bool is_downbeat);

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
 * 20. Waveform & Metering
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
 * 21. Configuration / Logging
 * ================================================================ */

/**
 * Set a global log callback. Pass NULL to revert to platform default.
 * Thread-safe but NOT RT-safe.
 */
WMA_API void wma_set_log_callback(WmaLogCallback callback);

/** Get version string (e.g., "0.1.0"). */
WMA_API const char* wma_get_version(void);

/* ================================================================ */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WATERMELON_AUDIO_H */
