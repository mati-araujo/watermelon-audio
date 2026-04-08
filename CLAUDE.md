# Watermelon Audio

Motor de sintesis en tiempo real con efectos DSP profesionales. C++20 + Oboe + Kotlin Multiplatform.

**Watermelon Studios**

---

## Arquitectura

```
audio/src/
  commonMain/kotlin/    52 files — pure Kotlin, zero Android deps
    api/                AudioEngine interface, IAudioNativeBridge, IEffectManager,
                        factories (AudioEngine, EffectManager, StateSynchronizer)
    domain/             Effect types, oscillators, scales, modes, USB types, errors
    callback/           AudioLogger, AudioAnalyticsListener (dependency inversion)
    internal/           AudioEngineImpl, EffectManagerImpl, StateSynchronizer,
                        ScaleQuantizer, ChordGenerator
                        expect: AudioBridgeProvider, NativeLibraryLoader
  androidMain/kotlin/   18 files — JNI bridge, USB, platform-specific
    internal/bridge/    AudioNativeBridge (2,619 LOC, 222 JNI external funs)
    internal/usb/       USB audio driver (DataStore, BroadcastReceiver)
    internal/mode/      ModeTransitionManagerImpl, NativeModeStateWriter
  main/cpp/             C++20 engine
    api/                C API — watermelon_audio.h (181 functions, pure C)
    dsp/                watermelon-dsp sub-library (30 files, zero deps)
    effects/            watermelon-effects sub-library (53 files, 20 effects + EffectRegistry)
    engines/            watermelon-engines sub-library (9 files, 7 synth engines)
    voice/              watermelon-voice sub-library (10 files, VoiceManager, VoicePool)
    looper/             watermelon-looper sub-library (3 files, header-only)
    core/               AudioEngine facade + 7 subsystems
    backends/           IAudioBackend, OboeBackend, LibusbBackend, BackendManager
    jni/                jni_audio_bridge.cpp (unified JNI)
    platform/           Logger.h, Platform.h (abstraction layer)
```

---

## Stack

| Componente | Version |
|------------|---------|
| Kotlin | 2.3.20 |
| AGP | 9.1.0 |
| Oboe | 1.10.0 |
| C++ | C++20 |
| CMake | 3.22.1 |
| Min SDK | 29 |
| Compile SDK | 36 |
| kotlinx-coroutines | 1.10.2 |
| TinySoundFont | 0.9 |

---

## Reglas

### C++ Audio Thread
- Audio callback 100% **lock-free** — nunca mutex, new, malloc
- `std::atomic` para parametros UI↔Audio
- `incrementStateVersion()` despues de modificar estado observable
- Parametros con smoothing para evitar zipper noise
- Logging via `platform/Logger.h` — NOT RT-safe, solo fuera del hot path

### JNI
- Todas las funciones nuevas en `jni/jni_audio_bridge.cpp` + `AudioNativeBridge.kt`
- Category mutexes: lifecycleMutex, effectsMutex, modeMutex, inputMutex
- Lock-free paths para real-time params (setXY, setFrequency)
- Return `Result<T>` para operaciones que pueden fallar

### Kotlin (commonMain)
- Zero imports de `android.*` o `java.*`
- Usar `AudioLogger` callback (NO `android.util.Log`)
- Usar `getAudioBridge()` (NOT `AudioNativeBridge.getInstance()`)
- `NativeLibraryLoader` via expect/actual

### Kotlin (androidMain)
- Dependencias Android permitidas: DataStore, Lifecycle, Core KTX, hardware.usb
- `android.util.Log` permitido (no necesita abstraccion aqui)

---

## Comandos

```bash
./gradlew :audio:assembleDebug                                     # Build debug (4 ABIs)
./gradlew :audio:assembleRelease                                   # Build release
./gradlew :audio:publishToMavenLocal                               # Publish local
./gradlew :audio:publishAllPublicationsToGitHubPackagesRepository   # Publish GitHub
```

---

## Agregar codigo nuevo

### Nuevo efecto de audio
1. C++: Crear en `audio/src/main/cpp/effects/`
2. Registrar en `EffectRegistry` (NO en switch — usar registro dinamico)
3. Agregar a `CMakeLists.txt`
4. Agregar `EffectType` entry en `EffectTypes.h` (C++) y `EffectType.kt` (Kotlin commonMain)
5. Parametros en `EffectParameter.kt` y `EffectConstants.kt` (commonMain)
6. C API: agregar `wma_effect_*` si necesario en `watermelon_audio.h/cpp`

### Nuevo synth engine
1. C++: Crear header-only en `audio/src/main/cpp/engines/` (heredar `SynthEngine`)
2. Registrar en `AudioEngine` (constructor, `getEngine()`, `prepare()`)
3. Registrar en `OscillatorNode` via `registerEngine()`

### Nuevo JNI function
1. C++: agregar en `jni/jni_audio_bridge.cpp`
2. Kotlin: agregar `private external fun` en `AudioNativeBridge.kt` (androidMain)
3. Wrapper con mutex apropiado y `Result<T>`
4. Si es parte del API publico: agregar a `IAudioNativeBridge` interface (commonMain)
5. Considerar: agregar a C API `watermelon_audio.h/cpp`

---

## Workflow con NoisyPad

Este repo es consumido por NoisyPad via GitHub Packages.

### Desarrollo local (rapido)
```kotlin
// En NoisyPad/settings.gradle.kts — descomentar:
// includeBuild("../watermelon-audio") { ... }
```
Cambios en este repo se ven inmediatamente en NoisyPad sin publicar.

### Publicar cambios
```bash
./gradlew :audio:publishToMavenLocal     # Para desarrollo local
./gradlew :audio:publishAllPublicationsToGitHubPackagesRepository  # Para CI/produccion
```

Ver `CONTRIBUTING.md` para workflow completo.

---

## Links

- **Consumer principal**: NoisyPad (`github.com/mati-araujo` — privado)
- **Extraction history**: `docs/00_master_plan.md`
- **Contributing guide**: `CONTRIBUTING.md`
