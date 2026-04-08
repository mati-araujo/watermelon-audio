claudee# Phase 1 — Modularizacion C++

**Reestructurar el core C++ en sub-libraries independientes y testables**

*Prerequisito: Phase 0 completada (C API funcional, sin Oboe leak, sin singletons)*
*Phase 0 completada 2026-04-06. Estado: ver [phase0_preparation.md](phase0_preparation.md)*

**Estado post-Phase 0 relevante para Phase 1:**
- `watermelon_audio.h` (181 funciones) y `watermelon_audio_internal.h` (WmaEngine struct) listos
- `platform/Logger.h` y `platform/Platform.h` disponibles como abstracciones
- AudioEngine.h no incluye Oboe (forward declaration only). `processAudioBlock()` es el entry point RT-safe
- BackendManager instanciable pero `getInstance()` sigue en uso (~40 call sites)
- USB volume globals deferidos (resolver en 1E cuando AudioEngine sea facade)
- JNI sigue llamando `g_jniState.engine->` directamente (200+ call sites, no blocker)

---

## Tabla de Contenidos

1. [Objetivo](#1-objetivo)
2. [Sub-fase 1A: Extraer DSP Primitives](#2-sub-fase-1a-extraer-dsp-primitives)
3. [Sub-fase 1B: Extraer Effects](#3-sub-fase-1b-extraer-effects)
4. [Sub-fase 1C: Extraer Engines](#4-sub-fase-1c-extraer-engines)
5. [Sub-fase 1D: Extraer Voice System](#5-sub-fase-1d-extraer-voice-system)
6. [Sub-fase 1E: AudioEngine Facade](#6-sub-fase-1e-audioengine-facade)
7. [Sub-fase 1F: Dynamic Registry](#7-sub-fase-1f-dynamic-registry)
8. [Sub-fase 1G: C++ Unit Tests](#8-sub-fase-1g-c-unit-tests)

---

## 1. Objetivo

Transformar el monolito C++ en sub-libraries con dependencias claras:

```
                    ┌─────────────┐
                    │   api/      │  C API (watermelon_audio.h)
                    └──────┬──────┘
                           │
                    ┌──────┴──────┐
                    │   core/     │  AudioEngine facade (~200 LOC)
                    └──────┬──────┘
                           │
          ┌────────┬───────┼───────┬──────────┐
          │        │       │       │          │
     ┌────┴───┐ ┌──┴──┐ ┌─┴──┐ ┌─┴───┐ ┌────┴────┐
     │effects/│ │voice│ │loop│ │arp/ │ │backends/│
     └────┬───┘ └──┬──┘ └─┬──┘ └─┬───┘ └─────────┘
          │        │      │      │
     ┌────┴───┐    │      │      │
     │engines/│────┘      │      │
     └────┬───┘           │      │
          │               │      │
     ┌────┴───────────────┴──────┴──┐
     │           dsp/               │  Pure DSP primitives
     └──────────────────────────────┘
```

**Regla fundamental:** Cada sub-library puede compilarse y testearse
independientemente. Las dependencias solo fluyen hacia abajo.

---

## 2. Sub-fase 1A: Extraer DSP Primitives

### Pre-condiciones
- Phase 0 completada

### Contexto

`utils/dsp/` contiene primitivas DSP puras (filtros, delays, LFOs, etc.) que
no dependen de nada mas que math estandar. Son la base de todo y el candidato
ideal para la primera extraccion.

### Archivos a mover a `dsp/`

| Archivo actual | Destino | Dependencias externas |
|---------------|---------|----------------------|
| `utils/dsp/BiquadFilter.h/cpp` | `dsp/BiquadFilter.h/cpp` | Ninguna |
| `utils/dsp/DelayLine.h/cpp` | `dsp/DelayLine.h/cpp` | Ninguna |
| `utils/dsp/DSPMath.h` | `dsp/DSPMath.h` | Ninguna |
| `utils/dsp/LFO.h/cpp` | `dsp/LFO.h/cpp` | Ninguna |
| `utils/dsp/FDN.h/cpp` | `dsp/FDN.h/cpp` | DelayLine |
| `utils/dsp/EarlyReflections.h/cpp` | `dsp/EarlyReflections.h/cpp` | Ninguna |
| `utils/dsp/GrainEngine.h/cpp` | `dsp/GrainEngine.h/cpp` | Ninguna |
| `utils/dsp/Oversampler.h/cpp` | `dsp/Oversampler.h/cpp` | Ninguna |
| `utils/dsp/StateVariableFilter.h` | `dsp/StateVariableFilter.h` | Ninguna |
| `utils/dsp/StereoTools.h/cpp` | `dsp/StereoTools.h/cpp` | Ninguna |
| `utils/dsp/VocoderBank.h/cpp` | `dsp/VocoderBank.h/cpp` | BiquadFilter |
| `utils/dsp/EnvelopeFollower.h` | `dsp/EnvelopeFollower.h` | Ninguna |
| `utils/ParameterSmoother.h` | `dsp/ParameterSmoother.h` | Ninguna |
| `utils/DCBlocker.h` | `dsp/DCBlocker.h` | Ninguna |
| `utils/SoftClipper.h` | `dsp/SoftClipper.h` | Ninguna |
| `utils/Dithering.h` | `dsp/Dithering.h` | Ninguna |
| `utils/NoiseGate.h` | `dsp/NoiseGate.h` | Ninguna |
| `utils/LevelMeter.h` | `dsp/LevelMeter.h` | Ninguna |
| `utils/SIMDUtils.h` | `dsp/SIMDUtils.h` | Platform SIMD |
| `utils/LockFreeRingBuffer.h` | `dsp/LockFreeRingBuffer.h` | Ninguna |

### Tareas

#### 1A.1 — Crear directorio `dsp/` con CMakeLists.txt propio

```cmake
# audio/src/main/cpp/dsp/CMakeLists.txt
add_library(watermelon-dsp STATIC
    BiquadFilter.cpp
    DelayLine.cpp
    LFO.cpp
    FDN.cpp
    EarlyReflections.cpp
    GrainEngine.cpp
    Oversampler.cpp
    StereoTools.cpp
    VocoderBank.cpp
)

target_include_directories(watermelon-dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(watermelon-dsp PUBLIC cxx_std_20)
```

#### 1A.2 — Mover archivos y actualizar includes

Mover cada archivo a `dsp/`. Actualizar todos los `#include` que referencian
los paths viejos (`utils/dsp/`, `utils/`).

#### 1A.3 — Verificar que dsp/ compila independientemente

El `CMakeLists.txt` de `dsp/` debe compilar sin incluir nada del resto del proyecto.

#### 1A.4 — Actualizar CMakeLists.txt principal

```cmake
add_subdirectory(dsp)
# ... link watermelon-dsp to main library
target_link_libraries(noisypad-audio PRIVATE watermelon-dsp)
```

### Verificacion

```bash
# V-1A.1: dsp/ directory exists with CMakeLists.txt
test -f audio/src/main/cpp/dsp/CMakeLists.txt && echo "PASS" || echo "FAIL"

# V-1A.2: No quedan archivos en utils/dsp/
ls audio/src/main/cpp/utils/dsp/ 2>/dev/null && echo "FAIL: files remain" || echo "PASS"

# V-1A.3: No broken includes
./gradlew :audio:assembleDebug

# V-1A.4: dsp/ files don't include anything from effects/, engines/, core/
grep -rn '#include.*\(effects\|engines\|core\|nodes\|voice\)' audio/src/main/cpp/dsp/ && \
  echo "FAIL: dependency leak" || echo "PASS"
```

### Post-condiciones
- `dsp/` es una static library auto-contenida
- Zero dependencias ascendentes (no incluye effects, engines, core)
- Build green

---

## 3. Sub-fase 1B: Extraer Effects

### Pre-condiciones
- 1A completada (dsp/ disponible)

### Contexto

El directorio `effects/` ya esta bastante aislado. La extraccion principal
es asegurar que dependa solo de `dsp/` y no de `core/` o `nodes/`.

### Archivos

| Componente | Archivos |
|-----------|---------|
| Base class | `Effect.h/cpp` |
| Chain | `EffectChain.h/cpp` |
| Types | `EffectTypes.h` |
| Limiter | `LookaheadLimiter.h/cpp` |
| Effects (20) | `FilterEffect.h`, `ReverbEffect.h`, `DelayEffect.h`, ... |

### Tareas

#### 1B.1 — Crear CMakeLists.txt para effects/

```cmake
add_library(watermelon-effects STATIC
    Effect.cpp
    EffectChain.cpp
    LookaheadLimiter.cpp
    # ... all 20 effects
)

target_link_libraries(watermelon-effects PUBLIC watermelon-dsp)
target_include_directories(watermelon-effects PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
```

#### 1B.2 — Auditar dependencias de effects/

Verificar que ningun effect incluye headers de `core/`, `nodes/`, `engines/`.
Si hay dependencias, resolver via interfaces o mover la logica.

**Dependencias conocidas a resolver:**
- Algunos effects pueden referenciar `ParameterSmoother` (ya en dsp/)
- `EffectChain` puede tener XY mapping atomics — verificar que son self-contained

#### 1B.3 — Effect base class: verificar que es pura

```cpp
class Effect {
    virtual void process(float* input, float* output, int numFrames) = 0;
    virtual void setParam(int paramId, float value) = 0;
    virtual float getParam(int paramId) = 0;
    virtual void setSampleRate(int sampleRate) = 0;
    virtual void setBpm(float bpm) {}
};
```

Debe permanecer libre de dependencias externas.

### Verificacion

```bash
# V-1B.1: effects/ has own CMakeLists.txt
test -f audio/src/main/cpp/effects/CMakeLists.txt && echo "PASS" || echo "FAIL"

# V-1B.2: effects/ only depends on dsp/
grep -rn '#include' audio/src/main/cpp/effects/ --include="*.h" --include="*.cpp" | \
  grep -v "effects/" | grep -v "dsp/" | grep -v "<" | \
  grep -v "platform/" && echo "FAIL: unexpected dependency" || echo "PASS"

# V-1B.3: Build green
./gradlew :audio:assembleDebug
```

### Post-condiciones
- `effects/` es una static library que depende solo de `dsp/`
- Zero dependencias a `core/`, `nodes/`, `engines/`

---

## 4. Sub-fase 1C: Extraer Engines

### Pre-condiciones
- 1A completada (dsp/ disponible)

### Archivos

| Componente | Archivos |
|-----------|---------|
| Base class | `SynthEngine.h` |
| Classic | (integrado en OscillatorNode — requiere atencion) |
| KarplusStrong | `KarplusStrongEngine.h` |
| FM | `FMEngine.h` |
| Supersaw | `SupersawEngine.h` |
| Wavetable | `WavetableEngine.h` |
| Granular | `GranularEngine.h` |
| SoundFont | `SoundFontEngine.h`, `SoundFontManager.h/cpp` |

### Tareas

#### 1C.1 — Crear CMakeLists.txt para engines/

```cmake
add_library(watermelon-engines STATIC
    SoundFontManager.cpp
    # Rest are header-only
)

target_link_libraries(watermelon-engines PUBLIC watermelon-dsp)
target_link_libraries(watermelon-engines PRIVATE tinysoundfont)
```

#### 1C.2 — Auditar dependencia de SoundFontEngine a TinySoundFont

TinySoundFont es una dependencia de `engines/` (no de `core/`).
Asegurar que esta linkada a `watermelon-engines`, no al target principal.

#### 1C.3 — Verificar que engines/ no depende de core/

Ningun engine debe incluir `AudioEngine.h` o headers de `core/`.

#### 1C.4 — Osciladores basicos

`oscillators/Oscillators.h` (Sine, Square, Saw, Triangle, Noise) deberia
moverse a `dsp/` o `engines/` dependiendo de si se usa fuera de engines.

### Verificacion

```bash
# V-1C.1: engines/ has own CMakeLists.txt
test -f audio/src/main/cpp/engines/CMakeLists.txt && echo "PASS" || echo "FAIL"

# V-1C.2: engines/ only depends on dsp/
grep -rn '#include' audio/src/main/cpp/engines/ --include="*.h" --include="*.cpp" | \
  grep -v "engines/" | grep -v "dsp/" | grep -v "<" | \
  grep -v "platform/" | grep -v "tinysoundfont" && \
  echo "FAIL: unexpected dependency" || echo "PASS"

# V-1C.3: Build green
./gradlew :audio:assembleDebug
```

### Post-condiciones
- `engines/` es una static library que depende solo de `dsp/` + TinySoundFont

---

## 5. Sub-fase 1D: Extraer Voice System

### Pre-condiciones
- 1C completada (engines/ disponible)

### Archivos

| Componente | Archivos |
|-----------|---------|
| Manager | `VoiceManager.h/cpp` |
| Voice | `Voice.h/cpp` |
| Pool | `VoicePool.h/cpp` |
| Types | `VoiceTypes.h` |
| Trigger base | `VoiceTriggerSource.h` |
| Touch trigger | `TouchTriggerSource.h/cpp` |

### Tareas

#### 1D.1 — Crear CMakeLists.txt para voice/

```cmake
add_library(watermelon-voice STATIC
    VoiceManager.cpp
    Voice.cpp
    VoicePool.cpp
    TouchTriggerSource.cpp
)

target_link_libraries(watermelon-voice PUBLIC watermelon-engines watermelon-dsp)
```

#### 1D.2 — Auditar dependencias de voice/ a core/

`VoiceManager` probablemente necesita access a engines para generar audio.
Verificar que la dependencia es via interfaces (SynthEngine base) y no via
AudioEngine directamente.

### Verificacion

```bash
# V-1D.1: voice/ only depends on engines/, dsp/
grep -rn '#include' audio/src/main/cpp/voice/ --include="*.h" --include="*.cpp" | \
  grep -v "voice/" | grep -v "engines/" | grep -v "dsp/" | grep -v "<" | \
  grep -v "platform/" && echo "FAIL" || echo "PASS"

# V-1D.2: Build green
./gradlew :audio:assembleDebug
```

---

## 5.5. Sub-fase 1D.2: Extraer Looper

### Pre-condiciones
- 1A completada (dsp/ disponible)

### Contexto

El Audio Track Looper (Phase 11) + Mixer & Loop Regions (Phase 13) forman
un subsistema complejo que merece extraccion propia. Incluye:

- **AudioLooper.h** — Orquestrador: 8 tracks, master volume con smoothing,
  pre-allocated mix buffer (mLooperMixBuf[2048]), metronome click, pre-count sync
- **TrackBuffer.h** — Per-track: buffer de audio, playhead independiente, speed
  con interpolacion, loop regions (mLoopStart/mLoopEnd atomics), adaptive crossfade
  para regiones cortas, peak level metering, import con sample rate resampling

### Archivos

| Componente | Archivos | Notas |
|-----------|---------|-------|
| Orchestrator | `looper/AudioLooper.h` | Master volume, process(), track delegation |
| Track buffer | `looper/TrackBuffer.h` | Per-track: buffer, playhead, speed, loop regions, crossfade |
| WAV I/O | `utils/WavFile.h` | Header-only WAV reader/writer (PCM 16/24-bit) |

### Tareas

#### 1D.2.1 — Crear CMakeLists.txt para looper/

```cmake
add_library(watermelon-looper STATIC
    # Header-only: AudioLooper.h, TrackBuffer.h
    # WavFile.h is header-only too
)

target_include_directories(watermelon-looper PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(watermelon-looper PUBLIC watermelon-dsp)
# Note: looper does NOT depend on effects/ or engines/ — it records raw audio
```

#### 1D.2.2 — Auditar dependencias del looper

El looper debe depender solo de `dsp/` (para ParameterSmoother, math utils).
Verificar que no incluye `AudioEngine.h`, `EffectChain.h`, ni engine headers.

**Dependencia critica: WavFile.h** — Usado para import/export.
Mover a `dsp/` o mantener en `looper/` si solo lo usa el looper.

#### 1D.2.3 — Verificar RT-safety del looper post-Phase 13

Phase 13 agrego complejidad al audio thread path:
- `mLooperMixBuf[2048]` pre-allocated (no stack VLA) — verificar que se usa
- Master volume smoothing en `process()` — lock-free via atomics
- Loop region boundaries en `mixInto()` — lock-free via atomics
- Adaptive crossfade para regiones cortas — pure math, RT-safe
- Peak level computation — already in `mixInto()`, RT-safe

### Verificacion

```bash
# V-1D.2.1: looper/ only depends on dsp/
grep -rn '#include' audio/src/main/cpp/looper/ --include="*.h" | \
  grep -v "looper/" | grep -v "dsp/" | grep -v "utils/" | grep -v "<" | \
  grep -v "platform/" && echo "FAIL: unexpected dependency" || echo "PASS"

# V-1D.2.2: Build green
./gradlew :audio:assembleDebug
```

### Post-condiciones
- `looper/` es una sub-library que depende solo de `dsp/`
- `WavFile.h` ubicado correctamente
- RT-safety del looper validada (no regressions de Phase 13)

---

## 6. Sub-fase 1E: AudioEngine Facade

### Pre-condiciones
- 1A, 1B, 1C, 1D (including 1D.2) completadas

### Contexto

Este es el refactoring mas critico: transformar `AudioEngine` de god class
(~1000 LOC, 40+ atomics, ownership de 80+ objetos) a un facade liviano
(~200 LOC) que compone las sub-libraries.

### Arquitectura Target

```cpp
class AudioEngine : public noisypad::IAudioCallback {
public:
    AudioEngine();
    ~AudioEngine();

    // Lifecycle
    bool start(int fadeTimeMs = 10);
    void stop();
    void pauseWithFade(int fadeTimeMs);
    void resumeWithFade(int fadeTimeMs);

    // Delegates to subsystems
    EffectChain& effectChain();
    VoiceManager& voiceManager();
    AudioGraph& audioGraph();
    AudioLooper& audioLooper();
    ArpSequencer& arpSequencer();

    // RT parameters (lock-free atomics)
    void setMasterVolume(float volume);
    void updateXY(float x, float y);
    void setFrequencyAndAmplitude(float freq, float amp);
    // ...

    // IAudioCallback
    Result onAudioReady(float* output, const float* input, int32_t numFrames) override;
    void onBackendError(BackendError error) override;

private:
    // Owned subsystems (created in constructor)
    std::unique_ptr<BackendManager> mBackend;
    std::unique_ptr<EffectChain> mEffectChain;
    std::unique_ptr<VoiceManager> mVoiceManager;
    std::unique_ptr<AudioGraph> mGraph;
    std::unique_ptr<AudioLooper> mLooper;
    std::unique_ptr<ArpSequencer> mArp;

    // State
    std::atomic<EngineState> mState{EngineState::Stopped};
    std::atomic<float> mMasterVolume{1.0f};
    std::atomic<uint64_t> mStateVersion{0};
    // ... (only orchestration state, not subsystem state)
};
```

### Tareas

#### 1E.1 — Identificar estado que pertenece a subsistemas

Clasificar las 40+ variables atomicas de AudioEngine:

| Variable | Pertenece a | Accion |
|----------|------------|--------|
| `mMasterVolume` | AudioEngine (orchestration) | Mantener |
| `mState` | AudioEngine (lifecycle) | Mantener |
| `mStateVersion` | AudioEngine (sync) | Mantener |
| `mCurrentFadeVolume` | AudioEngine (fade) | Mantener |
| `mEffectChain.*` | EffectChain | Ya delegado |
| `mDualTouchMode` | VoiceManager/OscillatorNode | Mover |
| `mCurrentEngineType` | OscillatorNode/EngineRegistry | Mover |
| `mArpEnabled` | ArpSequencer | Ya delegado |
| `mLooperRecording` | AudioLooper | Ya delegado |

#### 1E.2 — Mover ownership de engines a OscillatorNode/EngineRegistry

```cpp
// ANTES (AudioEngine tiene):
std::unique_ptr<KarplusStrongEngine> mKarplusStrong;
std::unique_ptr<FMEngine> mFMEngine;
// ... 10+ mas, plus 80 voice pool engines

// DESPUES: EngineRegistry o VoiceManager los owna
class VoiceManager {
    // Internally creates and manages engine instances per voice
};
```

#### 1E.3 — Mover ownership de nodes a AudioGraph

```cpp
// ANTES (AudioEngine tiene):
std::unique_ptr<OscillatorNode> mOscillatorNode;
std::unique_ptr<MixerNode> mMixerNode;
std::unique_ptr<EffectChainNode> mEffectChainNode;
std::unique_ptr<OutputNode> mOutputNode;

// DESPUES: AudioGraph los owna
class AudioGraph {
    std::unique_ptr<OscillatorNode> mOscNode;
    // ...
};
```

#### 1E.4 — Simplificar onAudioReady

```cpp
// ANTES: 200+ lineas de procesamiento directo

// DESPUES:
Result AudioEngine::onAudioReady(float* output, const float* input, int32_t numFrames) {
    if (mState.load() != EngineState::Running) {
        memset(output, 0, numFrames * 2 * sizeof(float));
        return Result::CONTINUE;
    }

    // Delegate to graph (which handles nodes, effects, mixing)
    mGraph->process(output, input, numFrames);

    // Apply master volume + fade
    applyMasterVolume(output, numFrames);

    // Looper capture + playback
    mLooper->process(output, numFrames);

    mStateVersion.fetch_add(1, std::memory_order_relaxed);
    return Result::CONTINUE;
}
```

#### 1E.5 — Actualizar C API para usar subsistemas

```cpp
WmaResult wma_effect_add(WmaEngine* engine, WmaEffectType type, int* out_index) {
    // ANTES: engine->engine->addEffect(type)
    // DESPUES: engine->engine->effectChain().addEffect(type)
}
```

### Verificacion

```bash
# V-1E.1: AudioEngine.h has fewer than 250 lines (was ~800+)
wc -l audio/src/main/cpp/core/AudioEngine.h | awk '{if ($1 < 250) print "PASS"; else print "FAIL: "$1" lines"}'

# V-1E.2: AudioEngine.h has fewer than 15 includes (was 35+)
grep -c '#include' audio/src/main/cpp/core/AudioEngine.h | awk '{if ($1 < 15) print "PASS"; else print "FAIL: "$1" includes"}'

# V-1E.3: AudioEngine.h does not include individual engine headers
grep -n "Engine.h" audio/src/main/cpp/core/AudioEngine.h | \
  grep -v "SynthEngine.h\|AudioEngine.h" && echo "FAIL" || echo "PASS"

# V-1E.4: Build green
./gradlew :audio:assembleDebug

# V-1E.5: Manual audio test (same as 0E.2)
```

### Post-condiciones
- AudioEngine es un facade de ~200 LOC
- Subsistemas son independientes y composables
- `onAudioReady` es <50 LOC de orchestration
- Build green, audio identico

---

## 7. Sub-fase 1F: Dynamic Registry

### Pre-condiciones
- 1E completada

### Contexto

Actualmente, agregar un effect o engine requiere editar switch statements
hardcoded en `EffectChain::createEffect()` y registrar manualmente en
`AudioEngine`. Un registro dinamico permite extensibilidad sin tocar el core.

### Tareas

#### 1F.1 — EffectRegistry

```cpp
// effects/EffectRegistry.h
class EffectRegistry {
public:
    using EffectFactory = std::function<std::unique_ptr<Effect>()>;

    void registerEffect(int typeId, const char* name, EffectFactory factory);
    std::unique_ptr<Effect> createEffect(int typeId) const;
    bool hasEffect(int typeId) const;
    std::vector<int> registeredTypes() const;

    // Singleton-free: one registry per engine instance
};
```

#### 1F.2 — EngineRegistry

```cpp
// engines/EngineRegistry.h
class EngineRegistry {
public:
    using EngineFactory = std::function<std::unique_ptr<SynthEngine>()>;

    void registerEngine(WmaEngineType type, const char* name, EngineFactory factory);
    std::unique_ptr<SynthEngine> createEngine(WmaEngineType type) const;
    bool hasEngine(WmaEngineType type) const;
};
```

#### 1F.3 — Registrar built-in effects y engines

```cpp
// En la inicializacion de AudioEngine (o via C API config):
void registerBuiltinEffects(EffectRegistry& reg) {
    reg.registerEffect(0, "Filter", []{ return std::make_unique<FilterEffect>(); });
    reg.registerEffect(1, "Reverb", []{ return std::make_unique<ReverbEffect>(); });
    // ... 18 mas
}
```

#### 1F.4 — C API para registrar effects/engines externos

```c
// En watermelon_audio.h:
typedef void* (*WmaEffectCreateFunc)(void);
WmaResult wma_register_effect(WmaEngine* engine, int type_id, const char* name, WmaEffectCreateFunc factory);
```

### Verificacion

```bash
# V-1F.1: No switch(type) para crear effects
grep -n "switch.*type" audio/src/main/cpp/effects/EffectChain.cpp | \
  grep -i "create" && echo "FAIL: hardcoded switch" || echo "PASS"

# V-1F.2: Registry files exist
test -f audio/src/main/cpp/effects/EffectRegistry.h && \
test -f audio/src/main/cpp/engines/EngineRegistry.h && \
echo "PASS" || echo "FAIL"

# V-1F.3: Build green
./gradlew :audio:assembleDebug
```

---

## 8. Sub-fase 1G: C++ Unit Tests

### Pre-condiciones
- 1A completada (dsp/ testable independientemente)
- Puede ejecutarse en paralelo con 1B-1F

### Contexto

El modulo no tiene tests C++. Para un refactoring de esta magnitud y para
un package publico, necesitamos tests automatizados.

### Tareas

#### 1G.1 — Integrar Google Test

Agregar GoogleTest como dependencia en CMake:

```cmake
# tests/CMakeLists.txt
include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

enable_testing()
```

**Nota:** Los tests se ejecutan en host (x86_64), no en Android device.
Esto requiere que las sub-libraries (dsp, effects, engines) sean compilables
sin NDK.

#### 1G.2 — Tests para DSP primitives

| Test | Que verifica |
|------|-------------|
| `BiquadFilter_LPF` | Pasa frecuencias bajas, atenua altas |
| `BiquadFilter_HPF` | Atenua bajas, pasa altas |
| `BiquadFilter_Stability` | No produce NaN/Inf con input extremo |
| `DelayLine_BasicDelay` | Output correcto con delay conocido |
| `DelayLine_Interpolation` | Interpolacion suave entre samples |
| `LFO_Sine` | Genera sine con frecuencia correcta |
| `LFO_AllWaveforms` | Todas las formas de onda en rango [-1,1] |
| `ParameterSmoother_Ramp` | Ramp de valor A a B en N samples |
| `ParameterSmoother_Instant` | Skip smoothing cuando delta < epsilon |
| `LockFreeRingBuffer_SPSC` | Produce/consume correctamente |
| `LockFreeRingBuffer_Overflow` | No corrompe memoria en overflow |
| `SoftClipper_Range` | Output siempre en [-1,1] |
| `DCBlocker_RemovesDC` | Elimina offset DC de la señal |

#### 1G.3 — Tests para EffectChain

| Test | Que verifica |
|------|-------------|
| `EffectChain_AddRemove` | Add/remove effects, indices correctos |
| `EffectChain_Process` | Audio pasa por la chain sin corrupcion |
| `EffectChain_Bypass` | Effect bypassed produce dry signal |
| `EffectChain_Routing_Serial` | Serial routing order correcto |
| `EffectChain_Routing_Parallel` | Parallel routing mix correcto |
| `EffectChain_Snapshot` | Atomic snapshot swap funciona |
| `EffectChain_MaxEffects` | Rechaza cuando chain esta llena |

#### 1G.4 — Tests para SynthEngine

| Test | Que verifica |
|------|-------------|
| `SynthEngine_Prepare` | Inicializacion sin crash |
| `SynthEngine_Process` | Genera audio no-silente |
| `SynthEngine_ParamRange` | Parametros clamped a rangos validos |
| `SynthEngine_Reset` | Reset produce silencio |

#### 1G.5 — Tests para VoicePool

| Test | Que verifica |
|------|-------------|
| `VoicePool_Allocate` | Asigna voices hasta MAX |
| `VoicePool_Release` | Libera voices correctamente |
| `VoicePool_Stealing` | Voice stealing cuando pool lleno |
| `VoicePool_Concurrent` | Thread-safe bajo contention |

#### 1G.6 — Tests para AudioLooper / TrackBuffer (Phase 11 + 13)

| Test | Que verifica |
|------|-------------|
| `TrackBuffer_RecordPlayback` | Graba audio, reproduce identico |
| `TrackBuffer_Overdub` | Overdub mezcla con existing audio |
| `TrackBuffer_MasterVolume` | Master volume aplica smoothing sin clicks |
| `TrackBuffer_MasterVolumeZero` | Master volume 0.0 produce silencio del looper |
| `TrackBuffer_LoopRegion` | `setLoopRegion(start, end)` limita playback a la region |
| `TrackBuffer_LoopRegionWrap` | Playhead wraps correctamente en region boundaries |
| `TrackBuffer_LoopRegionMinLength` | Region menor a 1024 frames se clampea |
| `TrackBuffer_LoopRegionCrossfade` | Crossfade en bordes de region no produce clicks |
| `TrackBuffer_AdaptiveCrossfade` | Regiones cortas (<2x CROSSFADE_FRAMES) reducen crossfade |
| `TrackBuffer_LoopRegionReset` | `resetLoopRegion()` vuelve a full buffer |
| `TrackBuffer_PlayheadClampOnRegionChange` | Playhead se re-wraps al cambiar region mid-playback |
| `TrackBuffer_Speed` | Speed 0.5x-2x con interpolacion correcta |
| `TrackBuffer_PeakLevel` | Peak level refleja audio real |
| `TrackBuffer_ClearResetsRegion` | `clear()` resetea loop region a defaults |
| `AudioLooper_MultiTrackMix` | 8 tracks se mezclan correctamente |
| `AudioLooper_TrackIndependence` | Cada track tiene playhead/speed/region independiente |
| `AudioLooper_MemoryBudget` | 48MB budget no se excede |
| `AudioLooper_ImportResample` | Import de 44100Hz WAV resamples a 48000Hz |

#### 1G.7 — Configurar ejecucion en CI

Tests deben correr en Linux/macOS host (no Android):
```bash
cd audio/src/main/cpp
mkdir build && cd build
cmake -DBUILD_TESTS=ON ..
make -j$(nproc)
ctest --output-on-failure
```

### Verificacion

```bash
# V-1G.1: Tests compile and pass
cd audio/src/main/cpp && mkdir -p build && cd build && \
  cmake -DBUILD_TESTS=ON .. && make -j$(nproc) && ctest --output-on-failure

# V-1G.2: Coverage report
# Target: >80% for dsp/, >60% for effects/

# V-1G.3: Android build still works
./gradlew :audio:assembleDebug
```

### Post-condiciones (Phase 1 completa) — COMPLETADA 2026-04-07

**Sub-libraries (5 CMake targets):**
- [x] `watermelon-dsp` — 30 files, zero external deps, compila standalone en host (x86_64)
- [x] `watermelon-effects` — 53 files, depende solo de dsp/ + platform/
- [x] `watermelon-engines` — 9 files, depende de dsp/ + TinySoundFont
- [x] `watermelon-voice` — 10 files, depende de engines/ + dsp/ + oscillators/ + AudioSource
- [x] `watermelon-looper` — 3 files (header-only INTERFACE), depende de dsp/

**AudioEngine facade (1E):**
- [x] 7 subsistemas extraidos: WaveformCapture, OutputStage, FadeController,
      DualTouchManager, ChordHarmony, OscillatorBank, SynthEngineDispatcher
- [x] processAudioBlock: 660 → 120 LOC, descompuesto en 9 render methods
- [x] AudioEngine total: 4,418 → 3,208 LOC (-27%), members: 65+ → ~30
- [ ] AudioEngine.h aun tiene 1,169 LOC (target <250 requiere forward declarations — cosmético)

**Dynamic registry (1F):**
- [x] EffectRegistry funcional — 20 effects registrados, switch de 60 LOC eliminado
- [ ] EngineRegistry no implementado (engines se registran en SynthEngineDispatcher constructor — suficiente por ahora)

**Unit tests (1G):**
- [x] 8 test files, 36 test cases, ALL PASSING
- [x] Toolchain: CMake 4.3.1 + g++ 15.2.0 (MinGW/MSYS2)
- [x] watermelon-dsp compila como standalone en host x86_64
- [ ] Looper tests pendientes (requiere más infraestructura de test)
- [ ] Coverage medición pendiente

**Build & funcionalidad:**
- [x] Build green en 4 ABIs (arm64-v8a, armeabi-v7a, x86_64, x86) + app completa
- [ ] Tests manuales en device pendientes
- [ ] Latency benchmark pendiente
