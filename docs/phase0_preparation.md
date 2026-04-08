# Phase 0 — Preparacion

**Desacoplar el modulo de audio sin mover codigo de lugar**

*Todos los cambios se hacen dentro de `audio/` en el repositorio NoisyPad.*
*NoisyPad debe compilar y funcionar identico al finalizar cada sub-fase.*

---

## Tabla de Contenidos

1. [Objetivo](#1-objetivo)
2. [Sub-fase 0A: Abstraer Logging y Platform](#2-sub-fase-0a-abstraer-logging-y-platform)
3. [Sub-fase 0B: Eliminar Herencia Oboe](#3-sub-fase-0b-eliminar-herencia-oboe)
4. [Sub-fase 0C: Crear C API](#4-sub-fase-0c-crear-c-api)
5. [Sub-fase 0D: Eliminar Singletons](#5-sub-fase-0d-eliminar-singletons)
6. [Sub-fase 0E: Validacion Integral](#6-sub-fase-0e-validacion-integral)

---

## 1. Objetivo

Preparar el codigo C++ y Kotlin del modulo `audio/` para su extraccion futura,
eliminando acoplamientos a Android, Oboe, y al singleton pattern, sin cambiar
la estructura de directorios ni la funcionalidad.

**Invariante:** Despues de cada sub-fase, `./gradlew assembleDebug` debe compilar
y la app debe funcionar sin regresiones audibles.

---

## 2. Sub-fase 0A: Abstraer Logging y Platform

### Pre-condiciones
- Ninguna (primera sub-fase)

### Contexto

El logging actual usa `__android_log_print` directamente via macros en `jni_common.h`.
El codigo de platform (denormal flushing, thread priority) usa inline assembly ARM
directamente en `AudioEngine.cpp`. Esto impide compilar para otras plataformas.

### Tareas

#### 0A.1 — Crear platform/Logger.h

Crear un sistema de logging abstracto que permita implementaciones por plataforma.

**Archivo:** `audio/src/main/cpp/platform/Logger.h`

```cpp
#pragma once

namespace wma {

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

// Function pointer type for log callback
using LogCallback = void(*)(LogLevel level, const char* tag, const char* message);

// Set custom logger (called once at init). If not set, uses platform default.
void setLogCallback(LogCallback callback);

// Log functions (safe to call from any thread, NOT RT-safe)
void logDebug(const char* tag, const char* fmt, ...);
void logInfo(const char* tag, const char* fmt, ...);
void logWarn(const char* tag, const char* fmt, ...);
void logError(const char* tag, const char* fmt, ...);

} // namespace wma
```

**Archivo:** `audio/src/main/cpp/platform/Logger.cpp`
- Default implementation: `__android_log_print` (actual behavior)
- Si se setea callback via `setLogCallback()`, usa eso en su lugar
- Thread-safe: callback pointer es `std::atomic<LogCallback>`

**Archivo:** Actualizar macros en `jni_common.h`
```cpp
// ANTES:
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, JNI_TAG, __VA_ARGS__)

// DESPUES:
#define LOGI(...) wma::logInfo(JNI_TAG, __VA_ARGS__)
```

#### 0A.2 — Crear platform/Platform.h

Abstraer operaciones platform-specific detras de funciones con implementaciones condicionales.

**Archivo:** `audio/src/main/cpp/platform/Platform.h`

```cpp
#pragma once
#include <cstdint>

namespace wma { namespace platform {

// Flush denormal numbers to zero (prevents 10-100x CPU slowdown)
// Implementation: ARM64 FPCR, ARMv7 FPSCR, x86 MXCSR, no-op on unknown
void flushDenormals();

// Set current thread to high priority for audio processing
// Implementation: Android pthread_setpriority, POSIX nice, Windows SetThreadPriority
void setThreadRealtimePriority();

// Query: does this platform support NEON SIMD?
bool hasNeonSupport();

// Query: does this platform support SSE SIMD?
bool hasSseSupport();

}} // namespace wma::platform
```

**Archivo:** `audio/src/main/cpp/platform/PlatformAndroid.cpp`
- Mueve la logica de denormal flushing de `AudioEngine.cpp:370-388` aqui
- Mueve thread priority logic aqui
- Compilado solo en Android builds (CMake condition)

#### 0A.3 — Actualizar AudioEngine.cpp

Reemplazar inline assembly y `__android_log_print` calls con las abstracciones.

```cpp
// ANTES (AudioEngine.cpp:370-388):
#if defined(__aarch64__)
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);
    asm volatile("msr fpcr, %0" : : "r"(fpcr));
#endif

// DESPUES:
wma::platform::flushDenormals();
```

#### 0A.4 — Actualizar Kotlin logging

Reemplazar `android.util.Log` en los 7 archivos del modulo audio con una
abstraccion que permita inyectar el logger.

**Archivos a modificar:**
- `internal/bridge/AudioNativeBridge.kt`
- `internal/engine/AudioEngineImpl.kt`
- `internal/effect/EffectManagerImpl.kt`
- `internal/sync/StateSynchronizer.kt`
- `internal/mode/ModeTransitionManagerImpl.kt`
- `internal/native/NativeLibraryLoader.kt`
- `internal/usb/UsbAudioManagerImpl.kt`

**Patron:** Usar el `AudioLogger` callback interface que ya existe en `callback/AudioLogger.kt`.
Asegurar que todos los archivos lo usen en vez de `android.util.Log` directo.

#### 0A.5 — Actualizar CMakeLists.txt

Agregar `platform/Logger.cpp` y `platform/PlatformAndroid.cpp` a la lista de compilacion.

### Verificacion

```bash
# V-0A.1: No hay __android_log_print directo en headers publicos del core
grep -r "__android_log_print" audio/src/main/cpp/core/ \
  audio/src/main/cpp/effects/ \
  audio/src/main/cpp/engines/ \
  audio/src/main/cpp/voice/ \
  audio/src/main/cpp/nodes/ \
  audio/src/main/cpp/looper/ \
  audio/src/main/cpp/utils/ \
  audio/src/main/cpp/backends/ && echo "FAIL: direct android logging found" || echo "PASS"

# V-0A.2: No hay inline assembly en AudioEngine.h/cpp
grep -n "asm volatile" audio/src/main/cpp/core/AudioEngine.cpp && echo "FAIL" || echo "PASS"

# V-0A.3: Platform files exist
test -f audio/src/main/cpp/platform/Logger.h && \
test -f audio/src/main/cpp/platform/Platform.h && \
echo "PASS" || echo "FAIL"

# V-0A.4: No hay android.util.Log en audio module Kotlin (excepto NativeLibraryLoader)
grep -rn "android.util.Log" audio/src/main/kotlin/ --include="*.kt" | \
  grep -v "NativeLibraryLoader" && echo "FAIL" || echo "PASS"

# V-0A.5: Build compiles
./gradlew :audio:assembleDebug
```

### Post-condiciones
- Platform abstractions en `audio/src/main/cpp/platform/`
- Cero `__android_log_print` directo fuera de `platform/` y `jni/`
- Cero inline assembly fuera de `platform/`
- Build green, funcionalidad identica

---

## 3. Sub-fase 0B: Eliminar Herencia Oboe

### Pre-condiciones
- 0A completada

### Contexto

`AudioEngine` hereda de `oboe::AudioStreamCallback` (legacy) ademas de
`noisypad::IAudioCallback` (nuevo). La herencia de Oboe:
- Fuerza `#include <oboe/Oboe.h>` en AudioEngine.h
- Hace que cualquier consumer de AudioEngine.h dependa de Oboe
- Es innecesaria porque `OboeBackend` ya maneja el callback internamente

### Tareas

#### 0B.1 — Analizar uso actual de oboe::AudioStreamCallback

Mapear todas las llamadas que pasan por la herencia de Oboe vs IAudioCallback.
Confirmar que `OboeBackend` ya tiene su propio `oboe::AudioStreamCallback`.

#### 0B.2 — Eliminar herencia oboe::AudioStreamCallback

```cpp
// ANTES (AudioEngine.h:91):
class AudioEngine : public oboe::AudioStreamCallback,
                    public noisypad::IAudioCallback {

// DESPUES:
class AudioEngine : public noisypad::IAudioCallback {
```

#### 0B.3 — Eliminar metodos override de Oboe en AudioEngine

Remover:
- `onAudioReady(oboe::AudioStream*, void*, int32_t)` — el override de Oboe
- `onErrorAfterClose(oboe::AudioStream*, oboe::Result)`
- `onErrorBeforeClose(oboe::AudioStream*, oboe::Result)`

Estos ya estan implementados en `OboeBackend`. `AudioEngine` solo necesita:
- `onAudioReady(float*, const float*, int32_t)` — de IAudioCallback
- `onBackendError(BackendError)` — de IAudioCallback

#### 0B.4 — Eliminar `#include <oboe/Oboe.h>` de AudioEngine.h

Reemplazar con forward declarations si es necesario, o eliminar por completo.
El include de Oboe solo debe estar en `backends/OboeBackend.h`.

#### 0B.5 — Verificar que BackendManager conecta correctamente

Confirmar que `BackendManager` setea `AudioEngine` como `IAudioCallback` en
`OboeBackend`, y que el flujo funciona sin la herencia directa.

### Verificacion

```bash
# V-0B.1: AudioEngine.h no incluye Oboe
grep "oboe/Oboe.h" audio/src/main/cpp/core/AudioEngine.h && echo "FAIL" || echo "PASS"

# V-0B.2: AudioEngine no hereda de oboe
grep "oboe::AudioStreamCallback" audio/src/main/cpp/core/AudioEngine.h && echo "FAIL" || echo "PASS"

# V-0B.3: Oboe solo se incluye en backends/
grep -rn "oboe/Oboe.h" audio/src/main/cpp/ --include="*.h" --include="*.cpp" | \
  grep -v "backends/" && echo "FAIL: Oboe leaked outside backends/" || echo "PASS"

# V-0B.4: Build compiles
./gradlew :audio:assembleDebug

# V-0B.5: Manual test — abrir app, tocar XY pad, verificar audio funciona
```

### Post-condiciones
- `AudioEngine` solo implementa `noisypad::IAudioCallback`
- `#include <oboe/Oboe.h>` solo aparece en `backends/`
- AudioEngine.h es compilable sin tener Oboe instalado
- Build green, audio funciona identico

---

## 4. Sub-fase 0C: Crear C API

### Pre-condiciones
- 0B completada (AudioEngine desacoplado de Oboe)

### Contexto

Actualmente el unico punto de entrada a C++ es JNI (`jni_audio_bridge.cpp`),
que usa tipos JNI (`JNIEnv*`, `jfloat`, `jintArray`). Esto impide usar el
engine desde iOS, desktop, o web.

La C API es el cambio mas importante de toda la extraccion: una vez que existe,
todo lo demas (JNI wrapper, iOS cinterop, FFI, Emscripten) se vuelve trivial.

### Tareas

#### 0C.1 — Disenar la C API

**Archivo:** `audio/src/main/cpp/api/watermelon_audio.h`

Principios de diseno:
- **C puro** — no C++, no templates, no exceptions
- **Opaque pointers** — `WmaEngine*` en vez de exponer internals
- **Error codes** — `WmaResult` enum, sin exceptions
- **Thread-safe** — documentar que funciones son RT-safe
- **Versionada** — `wma_get_version()` para compatibility checks

```c
#ifndef WATERMELON_AUDIO_H
#define WATERMELON_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ======================== VERSION ========================

#define WMA_VERSION_MAJOR 1
#define WMA_VERSION_MINOR 0
#define WMA_VERSION_PATCH 0

void wma_get_version(int* major, int* minor, int* patch);

// ======================== TYPES ========================

typedef struct WmaEngine WmaEngine;

typedef enum {
    WMA_OK = 0,
    WMA_ERROR_NOT_INITIALIZED = -1,
    WMA_ERROR_INVALID_INDEX = -2,
    WMA_ERROR_INVALID_PARAM = -3,
    WMA_ERROR_OUT_OF_RANGE = -4,
    WMA_ERROR_CHAIN_FULL = -5,
    WMA_ERROR_ALLOC_FAILED = -6,
    WMA_ERROR_STREAM = -7,
    WMA_ERROR_BUSY = -8,
    WMA_ERROR_INVALID_OP = -9,
    WMA_ERROR_INVALID_TYPE = -10,
    WMA_ERROR_TIMEOUT = -11,
    WMA_ERROR_UNKNOWN = -99
} WmaResult;

typedef enum {
    WMA_BACKEND_NONE = 0,
    WMA_BACKEND_OBOE = 1,
    WMA_BACKEND_LIBUSB = 2
} WmaBackendType;

typedef enum {
    WMA_ENGINE_CLASSIC = 0,
    WMA_ENGINE_KARPLUS_STRONG = 1,
    WMA_ENGINE_FM = 2,
    WMA_ENGINE_WAVETABLE = 3,
    WMA_ENGINE_GRANULAR = 4,
    WMA_ENGINE_SUPERSAW = 5,
    WMA_ENGINE_SOUNDFONT = 6
} WmaEngineType;

// Effect types: 0-19 (same as EffectTypes.h)
typedef int WmaEffectType;

// Log callback for custom logging
typedef void (*WmaLogCallback)(int level, const char* tag, const char* msg);

// Audio callback for custom backends
typedef struct {
    // Called from audio thread — MUST be RT-safe
    int (*on_audio_ready)(void* user_data, float* output, const float* input, int32_t num_frames);
    // Called on backend error — NOT RT-safe
    void (*on_error)(void* user_data, int error_code);
    void* user_data;
} WmaAudioCallback;

// ======================== LIFECYCLE ========================

// Create engine instance. Returns NULL on failure.
WmaEngine* wma_engine_create(void);

// Destroy engine instance. Safe to call with NULL.
void wma_engine_destroy(WmaEngine* engine);

// Start audio processing. fade_time_ms=0 for immediate start.
WmaResult wma_engine_start(WmaEngine* engine, int fade_time_ms);

// Stop audio processing.
WmaResult wma_engine_stop(WmaEngine* engine, int fade_time_ms);

// Pause (keeps stream alive). Resume with wma_engine_resume.
WmaResult wma_engine_pause(WmaEngine* engine, int fade_time_ms);
WmaResult wma_engine_resume(WmaEngine* engine, int fade_time_ms);

// ======================== REAL-TIME PARAMETERS ========================
// All functions below are lock-free and RT-safe.

WmaResult wma_set_xy(WmaEngine* engine, float x, float y);
WmaResult wma_set_frequency_amplitude(WmaEngine* engine, float freq_hz, float amplitude);
WmaResult wma_set_frequency_range(WmaEngine* engine, float min_hz, float max_hz);
WmaResult wma_set_master_volume(WmaEngine* engine, float volume);
WmaResult wma_set_bpm(WmaEngine* engine, float bpm);

// ======================== OSCILLATOR / ENGINE ========================

WmaResult wma_set_oscillator_type(WmaEngine* engine, int type);
WmaResult wma_set_engine_type(WmaEngine* engine, WmaEngineType type);
WmaResult wma_set_engine_param(WmaEngine* engine, int param_id, float value);
WmaEngineType wma_get_engine_type(WmaEngine* engine);

// ======================== EFFECTS ========================

WmaResult wma_effect_add(WmaEngine* engine, WmaEffectType type, int* out_index);
WmaResult wma_effect_remove(WmaEngine* engine, int index);
WmaResult wma_effect_set_param(WmaEngine* engine, int index, int param_id, float value);
float     wma_effect_get_param(WmaEngine* engine, int index, int param_id);
WmaResult wma_effect_set_bypass(WmaEngine* engine, int index, bool bypass);
WmaResult wma_effect_reorder(WmaEngine* engine, int from_index, int to_index);
int       wma_effect_chain_size(WmaEngine* engine);
WmaEffectType wma_effect_get_type(WmaEngine* engine, int index);

// Routing
WmaResult wma_set_routing_mode(WmaEngine* engine, int mode);
WmaResult wma_set_parallel_mix(WmaEngine* engine, float mix);
WmaResult wma_set_feedback_amount(WmaEngine* engine, float amount);

// ======================== SOUNDFONT ========================

WmaResult wma_soundfont_load_path(WmaEngine* engine, const char* path);
WmaResult wma_soundfont_unload(WmaEngine* engine);
WmaResult wma_soundfont_set_preset(WmaEngine* engine, int preset_index);
int       wma_soundfont_preset_count(WmaEngine* engine);
WmaResult wma_soundfont_note_on(WmaEngine* engine, int touch_id, int midi_note, float velocity);
WmaResult wma_soundfont_note_off(WmaEngine* engine, int touch_id);

// ======================== STATE ========================

uint64_t wma_get_state_version(WmaEngine* engine);
int      wma_get_engine_state(WmaEngine* engine);
bool     wma_is_paused(WmaEngine* engine);
bool     wma_has_error(WmaEngine* engine);

// Stream info: fills out_sample_rate, out_buffer_size, out_latency_ms
WmaResult wma_get_stream_info(WmaEngine* engine,
    int* out_sample_rate, int* out_buffer_size, float* out_latency_ms);

// Waveform sampling for visualization
int wma_get_waveform_samples(WmaEngine* engine, float* buffer, int max_samples);

// ======================== LOOPER (Phase 11 + 13) ========================

// Lifecycle
WmaResult wma_looper_prepare_track(WmaEngine* engine, int track, int length_frames, int sample_rate);
WmaResult wma_looper_start_recording(WmaEngine* engine, int track);
WmaResult wma_looper_stop_recording(WmaEngine* engine);
WmaResult wma_looper_start_overdub(WmaEngine* engine, int track);
WmaResult wma_looper_pause(WmaEngine* engine);
WmaResult wma_looper_resume(WmaEngine* engine);
WmaResult wma_looper_clear_track(WmaEngine* engine, int track);
WmaResult wma_looper_clear_all(WmaEngine* engine);

// Per-track control
WmaResult wma_looper_set_track_volume(WmaEngine* engine, int track, float volume);
WmaResult wma_looper_set_track_pan(WmaEngine* engine, int track, float pan);
WmaResult wma_looper_set_track_muted(WmaEngine* engine, int track, bool muted);
WmaResult wma_looper_set_track_speed(WmaEngine* engine, int track, float speed);
float     wma_looper_get_track_peak(WmaEngine* engine, int track);
float     wma_looper_get_track_progress(WmaEngine* engine, int track);

// Master volume (Phase 13A)
WmaResult wma_looper_set_master_volume(WmaEngine* engine, float volume);
float     wma_looper_get_master_volume(WmaEngine* engine);

// Loop regions (Phase 13D)
WmaResult wma_looper_set_track_loop_region(WmaEngine* engine, int track, int start_frame, int end_frame);
WmaResult wma_looper_reset_track_loop_region(WmaEngine* engine, int track);
int       wma_looper_get_track_loop_start(WmaEngine* engine, int track);
int       wma_looper_get_track_loop_end(WmaEngine* engine, int track);

// Import/Export
WmaResult wma_looper_import_track(WmaEngine* engine, int track, const char* wav_path);
WmaResult wma_looper_export_track(WmaEngine* engine, int track, const char* wav_path);
WmaResult wma_looper_export_mix(WmaEngine* engine, const char* wav_path);

// ======================== CONFIGURATION ========================

void wma_set_log_callback(WmaLogCallback callback);

// ======================== BACKEND ========================

WmaResult wma_set_backend(WmaEngine* engine, WmaBackendType type);
WmaBackendType wma_get_backend(WmaEngine* engine);

// USB-specific
WmaResult wma_usb_init(WmaEngine* engine, int file_descriptor, const char* usbfs_path);
WmaResult wma_usb_close(WmaEngine* engine);

#ifdef __cplusplus
}
#endif

#endif // WATERMELON_AUDIO_H
```

#### 0C.2 — Implementar la C API

**Archivo:** `audio/src/main/cpp/api/watermelon_audio.cpp`

Cada funcion de la C API es un thin wrapper sobre `AudioEngine`:

```cpp
#include "watermelon_audio.h"
#include "../core/AudioEngine.h"

struct WmaEngine {
    std::unique_ptr<AudioEngine> engine;
    // Backend, config, etc.
};

WmaEngine* wma_engine_create(void) {
    auto* ctx = new (std::nothrow) WmaEngine();
    if (!ctx) return nullptr;
    ctx->engine = std::make_unique<AudioEngine>();
    if (!ctx->engine || ctx->engine->hasInitializationFailed()) {
        delete ctx;
        return nullptr;
    }
    return ctx;
}

void wma_engine_destroy(WmaEngine* engine) {
    if (!engine) return;
    if (engine->engine) engine->engine->stop();
    delete engine;
}

WmaResult wma_set_xy(WmaEngine* engine, float x, float y) {
    if (!engine || !engine->engine) return WMA_ERROR_NOT_INITIALIZED;
    engine->engine->updateXY(x, y);
    return WMA_OK;
}

// ... (todas las funciones siguen este patron)
```

#### 0C.3 — Refactorizar JNI para usar C API

Modificar `jni_audio_bridge.cpp` para que use la C API en vez de acceder
a `g_jniState.engine` directamente.

```cpp
// ANTES:
JNIEXPORT void JNICALL nativeSetXY(JNIEnv*, jobject, jfloat x, jfloat y) {
    if (g_jniState.engine) {
        g_jniState.engine->updateXY(x, y);
    }
}

// DESPUES:
JNIEXPORT void JNICALL nativeSetXY(JNIEnv*, jobject, jfloat x, jfloat y) {
    wma_set_xy(g_engineHandle, x, y);
}
```

**Nota:** `g_engineHandle` es un `WmaEngine*` global que reemplaza a
`g_jniState.engine`. Es temporalmente global — Phase 0D lo eliminara.

#### 0C.4 — Agregar C API al CMakeLists.txt

Agregar `api/watermelon_audio.cpp` a la lista de compilacion.

#### 0C.5 — Funciones avanzadas de la C API

Agregar las funciones que falten para paridad completa con JNI:
- Modulator control
- Audio mode switching
- Looper control
- Voice system control
- Arp sequencer control
- XY mapping automation
- Dual touch

Documentar en `watermelon_audio.h` cuales funciones son RT-safe.

### Verificacion

```bash
# V-0C.1: C API header exists and is pure C
test -f audio/src/main/cpp/api/watermelon_audio.h && echo "PASS" || echo "FAIL"

# V-0C.2: Header compiles as C (not just C++)
# (would need a small test.c file)

# V-0C.3: JNI uses C API functions (spot check)
grep -c "wma_" audio/src/main/cpp/jni/jni_audio_bridge.cpp
# Should be > 50 (most JNI functions delegate to C API)

# V-0C.4: No direct g_jniState.engine-> access in jni_audio_bridge.cpp
# (except during transition — fully eliminated in 0D)
grep "g_jniState.engine->" audio/src/main/cpp/jni/jni_audio_bridge.cpp | wc -l
# Target: 0

# V-0C.5: Build compiles
./gradlew :audio:assembleDebug
```

### Post-condiciones
- `watermelon_audio.h` existe con C API pura
- JNI bridge delega a C API
- C API es compilable como C (no requiere C++ compiler para el header)
- Build green, funcionalidad identica

---

## 5. Sub-fase 0D: Eliminar Singletons

### Pre-condiciones
- 0C completada (C API funcional)

### Contexto

Tres singletons impiden instanciacion multiple y testabilidad:
1. `g_jniState` — global en `jni_common.h`
2. `BackendManager::getInstance()` — static local
3. `AudioNativeBridge.getInstance()` — Kotlin companion object

### Tareas

#### 0D.1 — BackendManager: de singleton a instanciable

```cpp
// ANTES:
class BackendManager {
    static BackendManager& getInstance();  // singleton
};

// AudioEngine.cpp:
auto& manager = BackendManager::getInstance();

// DESPUES:
class BackendManager {
public:
    BackendManager();  // constructible
};

// AudioEngine pasa a tener ownership:
class AudioEngine {
    std::unique_ptr<BackendManager> mBackendManager;
};
```

AudioEngine crea su propio `BackendManager` en el constructor.
Esto permite multiples AudioEngine con diferentes backends.

#### 0D.2 — g_jniState: de global a WmaEngine-owned

La C API ya encapsula esto via `WmaEngine*`. Ahora eliminamos el global:

```cpp
// ANTES (jni_common.h):
extern JniGlobalState g_jniState;

// DESPUES: g_jniState se elimina.
// JNI functions reciben WmaEngine* via un global handle:
static WmaEngine* g_activeEngine = nullptr;
// (single instance for JNI — Android limitation, not C++ limitation)
```

El `WmaEngine` struct pasa a contener todo el state que estaba en `JniGlobalState`:

```cpp
struct WmaEngine {
    std::unique_ptr<AudioEngine> engine;
    std::shared_ptr<InputNode> inputNode;
    std::atomic<int> currentMode{0};
    std::atomic<bool> modeTransitionInProgress{false};
    std::atomic<float> modeTransitionProgress{0.0f};
};
```

#### 0D.3 — USB Volume: de globals a backend-owned

```cpp
// ANTES (jni_usb.cpp):
std::atomic<float> g_usbOutputVolume{1.0f};
std::atomic<float> g_usbInputVolume{1.0f};

// DESPUES: dentro de LibusbBackend o WmaEngine
struct WmaEngine {
    // ...
    struct UsbVolumeState {
        std::atomic<float> outputVolume{1.0f};
        std::atomic<float> inputVolume{1.0f};
        std::atomic<bool> outputMuted{false};
        std::atomic<bool> inputMuted{false};
    } usbVolume;
};
```

#### 0D.4 — AudioNativeBridge: de singleton a injectable

```kotlin
// ANTES:
class AudioNativeBridge private constructor() {
    companion object {
        @Volatile private var INSTANCE: AudioNativeBridge? = null
        fun getInstance(): AudioNativeBridge = ...
    }
}

// DESPUES:
class AudioNativeBridge internal constructor() {
    // No companion, no singleton
    // Created by AudioEngineFactory
}

// Factory:
class AudioEngineFactory {
    fun create(config: AudioEngineConfig): AudioEngine {
        val bridge = AudioNativeBridge()
        return AudioEngineImpl(bridge, config)
    }
}
```

**Nota:** En Android, JNI sigue siendo single-instance por limitaciones del
native library loading. Pero la API Kotlin ya no fuerza singleton.

#### 0D.5 — Actualizar DI en NoisyPad

Actualizar `DomainModule.kt` para crear `AudioNativeBridge` via factory
en vez de `.getInstance()`.

### Verificacion

```bash
# V-0D.1: No BackendManager::getInstance() en el codebase
grep -rn "BackendManager::getInstance" audio/src/main/cpp/ && echo "FAIL" || echo "PASS"

# V-0D.2: g_jniState eliminado (o reducido a alias)
grep -rn "g_jniState" audio/src/main/cpp/jni/ | wc -l
# Target: 0 o minimo (solo en migration shim)

# V-0D.3: No globals de USB volume
grep -n "g_usbOutputVolume\|g_usbInputVolume" audio/src/main/cpp/ -r && echo "FAIL" || echo "PASS"

# V-0D.4: No .getInstance() en AudioNativeBridge usage
grep -rn "AudioNativeBridge.getInstance" audio/src/main/kotlin/ && echo "FAIL" || echo "PASS"

# V-0D.5: Build compiles
./gradlew assembleDebug
```

### Post-condiciones
- `BackendManager` es instanciable (no singleton)
- `WmaEngine` encapsula todo el state (no globals)
- `AudioNativeBridge` es instanciable via factory
- Build green, funcionalidad identica

---

## 6. Sub-fase 0E: Validacion Integral

### Pre-condiciones
- 0A, 0B, 0C, 0D completadas

### Contexto

Validacion exhaustiva de que todos los refactorings de Phase 0 no introdujeron
regresiones. Esta sub-fase es puro testing — no se escribe codigo nuevo.

### Tareas

#### 0E.1 — Build verification

```bash
./gradlew clean assembleDebug          # Build limpio
./gradlew assembleRelease              # Release con R8
./gradlew :audio:assembleDebug         # Modulo aislado
./gradlew lint                         # Sin warnings nuevos
```

#### 0E.2 — Audio functionality test (manual)

| Test Case | Esperado | Pass? |
|-----------|----------|-------|
| Abrir app, tocar XY pad | Audio sin glitches | |
| Cambiar engine type (Classic → FM → Wavetable) | Transicion suave | |
| Agregar 5 effects al chain | Todos suenan | |
| Cambiar routing mode (Serial → Parallel → Feedback) | Audio continuo | |
| Cargar SoundFont, tocar notas | Polifonia funciona | |
| Activar arpeggiator | Pattern suena | |
| Grabar looper track, reproducir | Audio correcto | |
| Looper: abrir Mixer Sheet, mover faders | Volumen cambia suavemente | |
| Looper: master volume fader | Afecta solo looper, no synth | |
| Looper: mute/solo desde mixer | Solo es exclusivo, mute funciona | |
| Looper: peak meters en mixer | Se actualizan en tiempo real | |
| Looper: definir loop region (long-press pill) | Audio loopea solo la region | |
| Looper: drag handles de region | Cambio en tiempo real sin clicks | |
| Looper: reset loop region | Vuelve a full buffer | |
| Looper: importar WAV + verificar resampling | Audio suena al pitch correcto | |
| Conectar USB audio device (si disponible) | Backend switch funciona | |
| Guitar mode (si hay input) | Input FX funciona | |
| Fade in/out al start/stop | Sin clicks | |
| Tocar 2 dedos (dual touch) | Ambos suenan | |

#### 0E.3 — Latency benchmark

Ejecutar `LatencyBenchmark` antes y despues del refactor.
Diferencia aceptable: <5% en latencia promedio.

#### 0E.4 — Memory check

- Verificar que no hay memory leaks nuevos (Android Profiler)
- Verificar que el consumo de memoria es similar al baseline

#### 0E.5 — Architectural verification

```bash
# C API es el unico punto de entrada para JNI
grep "g_jniState.engine->" audio/src/main/cpp/jni/jni_audio_bridge.cpp && echo "FAIL" || echo "PASS"

# AudioEngine.h no depende de Oboe
grep "oboe" audio/src/main/cpp/core/AudioEngine.h && echo "FAIL" || echo "PASS"

# No android-specific logging fuera de platform/
grep -r "__android_log_print" audio/src/main/cpp/ --include="*.h" --include="*.cpp" | \
  grep -v "platform/" | grep -v "jni/" && echo "FAIL" || echo "PASS"

# No singletons
grep -rn "getInstance\b" audio/src/main/cpp/ && echo "FAIL" || echo "PASS"
```

### Verificacion

Todos los tests de 0E.1-0E.5 deben pasar. Este es el gate para avanzar a Phase 1.

### Post-condiciones (Phase 0 completa)

- [x] `watermelon_audio.h` — C API pura (792 LOC, 181 funciones, 21 categorias), zero stubs
- [x] `watermelon_audio.cpp` — Implementacion completa (1324 LOC), thin wrappers sobre AudioEngine
- [x] `watermelon_audio_internal.h` — Definicion de WmaEngine compartida entre C API y JNI
- [x] AudioEngine desacoplado de Oboe (solo IAudioCallback). OboeCallbackAdapter en .cpp
- [x] AudioEngine.h no incluye `<oboe/Oboe.h>` (forward declaration only)
- [x] Logging via `platform/Logger.h` — 28 archivos migrados, zero `__android_log_print` fuera de platform/
- [x] Denormal flushing via `platform/Platform.h` — zero inline assembly fuera de platform/
- [x] BackendManager constructor publico, instanciable. `setGlobalInstance()` disponible.
- [x] WmaEngine owna BackendManager + AudioEngine + InputNode
- [x] g_jniState.engine es raw pointer (non-owning), ownership en g_wmaEngine
- [x] `ensureEngine()` crea via `wma_engine_create()` (C API)
- [x] Build green en 4 ABIs + app completa
- [x] processAudioBlock() RT-safe (zero oboe::, zero allocations, zero mutex en hot path)
- [ ] **PENDIENTE 0E:** Tests manuales de funcionalidad (requiere device fisico)
- [ ] **PENDIENTE 0E:** Latency benchmark antes/despues

### Deferido a Phase 1

- **USB volume globals** (`g_usbOutputVolume`, etc.) — quedan como globals porque OutputNode
  los accede via `extern "C"` desde el audio callback. Mover a WmaEngine requiere refactorizar
  cómo OutputNode obtiene el volumen USB. Se resuelve cuando AudioEngine se convierta en facade.
- **JNI migration a C API** — El JNI bridge aun llama `g_jniState.engine->` directamente
  (200+ call sites). La C API coexiste en paralelo. La migración incremental del JNI puede
  hacerse cuando sea conveniente, no es blocker para Phase 1.
- **BackendManager::getInstance()** sigue siendo usado por ~40 call sites. La instancia ahora
  es owned por WmaEngine via `setGlobalInstance()`, pero los callers no cambiaron. Se resuelve
  cuando AudioEngine reciba el BackendManager por constructor injection (Phase 1E).
