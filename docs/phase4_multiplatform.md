# Phase 4 — Multiplataforma

**Especificacion para extender a iOS, desktop, web y server**

*Esta fase es spec-only — define la arquitectura sin implementar.*
*Prerequisito: Phase 3 completada (repositorio independiente publicado)*

---

## Tabla de Contenidos

1. [Objetivo](#1-objetivo)
2. [Arquitectura Multiplataforma](#2-arquitectura-multiplataforma)
3. [Sub-fase 4A: iOS Backend (CoreAudio)](#3-sub-fase-4a-ios-backend-coreaudio)
4. [Sub-fase 4B: Desktop Backend (PortAudio)](#4-sub-fase-4b-desktop-backend-portaudio)
5. [Sub-fase 4C: Web Backend (Emscripten + WebAudio)](#5-sub-fase-4c-web-backend-emscripten--webaudio)
6. [Sub-fase 4D: Server Backend (Offline Rendering)](#6-sub-fase-4d-server-backend-offline-rendering)
7. [Evaluacion de Esfuerzo](#7-evaluacion-de-esfuerzo)
8. [Decision Framework](#8-decision-framework)

---

## 1. Objetivo

Definir la arquitectura y los requisitos tecnicos para extender
`watermelon-audio` a multiples plataformas, manteniendo:

- **Un solo codebase C++** para todo el DSP
- **Un solo codebase Kotlin (KMP)** para la API de alto nivel
- **Backends nativos por plataforma** para I/O de audio
- **Zero compromiso de latencia** — cada plataforma usa su API nativa optima

### Diagrama de plataformas

```
                        ┌────────────────────┐
                        │  audio-core (C++)  │
                        │  DSP, Effects,     │
                        │  Engines, Voice,   │
                        │  Graph, Looper     │
                        │  C API             │
                        └────────┬───────────┘
                                 │
         ┌───────────┬───────────┼───────────┬───────────┐
         │           │           │           │           │
    ┌────┴───┐  ┌────┴───┐  ┌───┴────┐ ┌────┴───┐  ┌───┴────┐
    │Android │  │  iOS   │  │Desktop │ │  Web   │  │Server  │
    │ Oboe   │  │CoreAu. │  │PortAu. │ │WebAu.  │  │File I/O│
    │ libusb │  │AVAudio │  │WASAPI  │ │AudioWl │  │        │
    └────┬───┘  └────┬───┘  └───┬────┘ └────┬───┘  └───┬────┘
         │           │          │            │          │
    ┌────┴───┐  ┌────┴───┐  ┌──┴────┐  ┌────┴───┐ ┌───┴────┐
    │KMP     │  │KMP     │  │KMP    │  │KMP/JS  │ │KMP/JVM │
    │android │  │ios     │  │jvm    │  │js      │ │jvm     │
    │Main    │  │Main    │  │Main   │  │Main    │ │Main    │
    └────────┘  └────────┘  └───────┘  └────────┘ └────────┘
```

---

## 2. Arquitectura Multiplataforma

### 2.1 — C++ Core: Sin cambios

El `audio-core/` C++ ya esta preparado (Phase 0-1):
- C API (`watermelon_audio.h`) — callable desde cualquier lenguaje
- Sin dependencias de plataforma en el core
- Backends via `IAudioBackend.h` — extensible

Para cada plataforma nueva, solo se necesita:
1. Implementar `IAudioBackend` con la API nativa
2. Compilar `audio-core` con CMake para la plataforma target
3. Implementar el `actual` de KMP o binding equivalente

### 2.2 — KMP: expect/actual por plataforma

```kotlin
// commonMain — shared across all platforms
expect class NativeAudioBridge
expect class AudioEngineFactory
expect object NativeLibraryLoader
expect object AudioLog

// androidMain — JNI calls
actual class NativeAudioBridge { /* JNI external fun */ }

// iosMain — cinterop with C API
actual class NativeAudioBridge { /* wma_* C function calls */ }

// jvmMain — JNI (same as Android, different backend)
actual class NativeAudioBridge { /* JNI external fun */ }

// jsMain — Kotlin/JS + WebAssembly
actual class NativeAudioBridge { /* WASM module calls */ }
```

### 2.3 — CMake Cross-Platform Build

```cmake
# audio-core/CMakeLists.txt (ya preparado)

if(ANDROID)
    # NDK build, NEON SIMD, Oboe dependency
    add_subdirectory(backends/oboe)
    add_subdirectory(backends/libusb)
elseif(APPLE)
    # Xcode build, NEON (Apple Silicon) or SSE (Intel)
    add_subdirectory(backends/coreaudio)
elseif(UNIX)
    # Linux, PortAudio
    add_subdirectory(backends/portaudio)
elseif(WIN32)
    # Windows, WASAPI or PortAudio
    add_subdirectory(backends/wasapi)
elseif(EMSCRIPTEN)
    # WebAssembly
    add_subdirectory(backends/webaudio)
endif()
```

---

## 3. Sub-fase 4A: iOS Backend (CoreAudio)

### Tecnologias

| Componente | Tecnologia |
|-----------|-----------|
| Audio I/O | AVAudioEngine / Audio Unit (C API) |
| Low-latency | kAudioUnitSubType_RemoteIO |
| MIDI | CoreMIDI |
| Bridge | Kotlin/Native cinterop → C API |
| SIMD | NEON (Apple Silicon) / SSE (Intel Mac) |
| Build | CMake via Xcode, CocoaPods o SPM |

### Requisitos del backend

```cpp
// backends/coreaudio/CoreAudioBackend.h
class CoreAudioBackend : public IAudioBackend {
    // Audio Unit render callback
    static OSStatus renderCallback(
        void* inRefCon,
        AudioUnitRenderActionFlags* ioActionFlags,
        const AudioTimeStamp* inTimeStamp,
        UInt32 inBusNumber,
        UInt32 inNumberFrames,
        AudioBufferList* ioData
    );

    // IAudioBackend implementation
    BackendResult start() override;
    void stop() override;
    void setCallback(IAudioCallback* callback) override;
    // ...
};
```

### KMP iOS actual

```kotlin
// iosMain/internal/bridge/NativeAudioBridge.kt
import kotlinx.cinterop.*
import watermelon.audio.*  // Generated from C API def file

actual class NativeAudioBridge {
    private var engine: CPointer<WmaEngine>? = null

    actual suspend fun startEngine(): Result<Unit> {
        val result = wma_engine_start(engine, 10)
        return if (result == WMA_OK) Result.success(Unit)
               else Result.failure(NativeBridgeException.fromCode(result.toInt(), "start"))
    }

    actual fun setXY(x: Float, y: Float) {
        wma_set_xy(engine, x, y)
    }

    // ...
}
```

### Distribucion iOS

- **CocoaPods:** `WatermalonAudio.podspec` con vendored_frameworks
- **Swift Package Manager:** Package.swift que compila C++ via CMake
- **XCFramework:** Pre-built binaries para arm64 + simulator

### Esfuerzo estimado: 2-3 semanas

| Tarea | Dias |
|-------|------|
| CoreAudioBackend implementation | 3-5 |
| CMake iOS cross-compilation | 2-3 |
| Kotlin/Native cinterop setup | 2-3 |
| iosMain actual implementations | 2-3 |
| Testing en device + simulator | 2-3 |
| CocoaPods/SPM packaging | 1-2 |

---

## 4. Sub-fase 4B: Desktop Backend (PortAudio)

### Tecnologias

| Componente | Tecnologia |
|-----------|-----------|
| Audio I/O | PortAudio (cross-platform) |
| Alternative | JUCE (mas features), RtAudio |
| MIDI | RtMidi |
| Bridge | JNI (Kotlin/JVM) |
| SIMD | SSE4.2 / AVX2 (x86_64), NEON (Apple Silicon) |
| Build | CMake standalone |

### Por que PortAudio

- Open source, permissive license (MIT)
- Cross-platform: Windows (WASAPI/ASIO), macOS (CoreAudio), Linux (ALSA/JACK)
- C API — facil de integrar con `IAudioBackend`
- Usado en Audacity, otros proyectos de audio

### Requisitos del backend

```cpp
// backends/portaudio/PortAudioBackend.h
class PortAudioBackend : public IAudioBackend {
    static int paCallback(
        const void* inputBuffer,
        void* outputBuffer,
        unsigned long framesPerBuffer,
        const PaStreamCallbackTimeInfo* timeInfo,
        PaStreamCallbackFlags statusFlags,
        void* userData
    );

    PaStream* mStream = nullptr;
    // ...
};
```

### Distribucion Desktop

- **Maven (Kotlin/JVM):** `audio-desktop` artifact con JNI + native libs
- **Native libs bundled:** .so (Linux), .dylib (macOS), .dll (Windows)
- **Standalone JAR:** Fat JAR con natives incluidos

### Esfuerzo estimado: 1-2 semanas

| Tarea | Dias |
|-------|------|
| PortAudioBackend implementation | 2-3 |
| CMake desktop build | 1-2 |
| JNI bridge (reusar Android pattern) | 1-2 |
| jvmMain actual implementations | 1-2 |
| Testing en Linux/macOS/Windows | 2-3 |
| Maven publishing | 1 |

---

## 5. Sub-fase 4C: Web Backend (Emscripten + WebAudio)

### Tecnologias

| Componente | Tecnologia |
|-----------|-----------|
| C++ → WASM | Emscripten |
| Audio I/O | Web Audio API (AudioWorklet) |
| Bridge | Kotlin/JS → WASM module |
| SIMD | WebAssembly SIMD (limited) |
| Build | CMake + Emscripten toolchain |

### Arquitectura Web

```
Browser
├── Kotlin/JS (UI, state management)
│   └── calls WASM module via JS interop
├── AudioWorklet (real-time audio thread)
│   └── calls WASM module directly
└── WASM Module (compiled C++ audio-core)
    └── watermelon_audio.h functions
```

### Requisitos del backend

```cpp
// backends/webaudio/WebAudioBackend.h
// NOTE: This backend is special — it doesn't use IAudioBackend directly.
// Instead, the AudioWorklet calls wma_* C API functions directly from WASM.

// Emscripten bindings:
EMSCRIPTEN_BINDINGS(watermelon_audio) {
    function("wma_engine_create", &wma_engine_create, allow_raw_pointers());
    function("wma_engine_start", &wma_engine_start, allow_raw_pointers());
    function("wma_set_xy", &wma_set_xy, allow_raw_pointers());
    // ...
}
```

### AudioWorklet processor

```javascript
// watermelon-audio-processor.js
class WatermalonAudioProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        this.engine = Module.wma_engine_create();
        Module.wma_engine_start(this.engine, 0);
    }

    process(inputs, outputs, parameters) {
        const output = outputs[0][0];
        Module.wma_process(this.engine, output, output.length);
        return true;
    }
}
```

### Limitaciones Web

- **Latencia:** ~128 samples minimum (AudioWorklet), ~2.7ms @ 48kHz
- **SIMD:** WebAssembly SIMD existe pero es mas limitado que NEON/SSE
- **Threads:** SharedArrayBuffer requerido para threading (CORS headers)
- **File access:** SoundFont loading via fetch + ArrayBuffer
- **No USB:** Web USB API existe pero limitada

### Esfuerzo estimado: 3-4 semanas

| Tarea | Dias |
|-------|------|
| Emscripten CMake toolchain | 2-3 |
| WASM compilation + optimizations | 3-5 |
| AudioWorklet integration | 3-5 |
| Kotlin/JS bridge | 3-5 |
| Web-specific adaptations (file I/O, etc.) | 2-3 |
| Testing en browsers | 2-3 |

---

## 6. Sub-fase 4D: Server Backend (Offline Rendering)

### Tecnologias

| Componente | Tecnologia |
|-----------|-----------|
| Audio I/O | File-based (WAV, FLAC) |
| Processing | Non-real-time (faster than RT) |
| Bridge | JNI (Kotlin/JVM) o C API directa |
| Use cases | Batch rendering, CI testing, audio generation APIs |

### Requisitos del backend

```cpp
// backends/file/FileBackend.h
class FileBackend : public IAudioBackend {
    // No real-time callback — processes in a loop
    void renderToFile(const char* outputPath, int durationMs, int sampleRate);

    // Implements IAudioBackend for compatibility
    BackendResult start() override;  // Starts render loop
    void stop() override;            // Stops render loop
};
```

### Use cases

1. **CI Testing:** Render audio, verify output (no hardware needed)
2. **Batch Processing:** Apply effects to audio files
3. **Audio API Server:** REST/gRPC endpoint que genera audio
4. **Automated Testing:** Regression tests comparing rendered output

### Esfuerzo estimado: 1 semana

| Tarea | Dias |
|-------|------|
| FileBackend implementation | 2-3 |
| WAV output (WavFile.h ya existe) | 1 |
| JVM integration | 1-2 |
| CI test pipeline | 1 |

---

## 7. Evaluacion de Esfuerzo

### Comparativa de plataformas

| Plataforma | Esfuerzo | Complejidad | Valor para NoisyPad | Recomendacion |
|-----------|----------|-------------|---------------------|---------------|
| **iOS** | 2-3 semanas | Media | ALTO — app iOS es el siguiente producto | Primera prioridad |
| **Desktop** | 1-2 semanas | Baja | MEDIO — herramienta para musicos | Segunda prioridad |
| **Server** | 1 semana | Baja | MEDIO — testing + APIs | Tercera prioridad (quick win) |
| **Web** | 3-4 semanas | Alta | BAJO — nicho, latencia no ideal | Ultima prioridad |

### Orden recomendado

1. **Server** — quick win, habilita CI testing sin hardware
2. **iOS** — mayor valor de negocio, complejidad manejable
3. **Desktop** — PortAudio es straightforward, expande mercado
4. **Web** — solo si hay demanda clara

---

## 8. Decision Framework

### Cuando agregar una plataforma nueva

Antes de empezar, verificar:

- [ ] **Demanda:** Hay usuarios o productos que la necesitan?
- [ ] **Audio API:** Existe una API nativa de baja latencia? (<10ms)
- [ ] **CMake support:** Se puede compilar C++ con CMake para esta plataforma?
- [ ] **KMP target:** Kotlin tiene target para esta plataforma? (o se necesita otro lenguaje?)
- [ ] **Testing:** Se puede testear en CI sin hardware?
- [ ] **Distribution:** Hay un package manager estandar? (Maven, CocoaPods, npm)

### Checklist para implementar un nuevo backend

1. [ ] Crear `backends/{name}/{Name}Backend.h` implementando `IAudioBackend`
2. [ ] Implementar los 12 virtual methods de `IAudioBackend`
3. [ ] Agregar al CMakeLists.txt con conditional compilation
4. [ ] Crear KMP `actual` implementations para la plataforma
5. [ ] Agregar `WmaBackendType` entry para el nuevo backend
6. [ ] Sample app que demuestre el backend
7. [ ] CI pipeline para la plataforma
8. [ ] Publishing del artifact
9. [ ] Documentacion

### Post-condiciones (Phase 4 — spec completa)

- [ ] Spec de arquitectura para cada plataforma documentada
- [ ] Decision framework para priorizar plataformas
- [ ] Estimaciones de esfuerzo basadas en el codebase real
- [ ] Checklists para implementar nuevos backends
- [ ] No se escribe codigo en esta fase — solo se planifica
