# Watermelon Audio

Motor de sintesis en tiempo real con efectos DSP profesionales. C++20 + Oboe + Kotlin Multiplatform.

**Watermelon Studios**

---

## Arquitectura

```
audio/src/
  commonMain/kotlin/    64 files — pure Kotlin, zero Android deps
    api/                AudioEngine interface, IAudioNativeBridge, IEffectManager,
                        factories (AudioEngine, EffectManager, StateSynchronizer)
    domain/             Effect types, oscillators, scales, modes, USB types, errors
    callback/           AudioLogger, AudioAnalyticsListener (dependency inversion)
    internal/           AudioEngineImpl, EffectManagerImpl, StateSynchronizer,
                        ScaleQuantizer, ChordGenerator, util/Format, util/Time
                        expect: AudioBridgeProvider, NativeLibraryLoader
  androidMain/kotlin/   21 files — JNI bridge, USB, platform-specific
    internal/bridge/    AudioNativeBridge (3,352 LOC, 289 external funs)
    internal/usb/       USB audio driver (DataStore, BroadcastReceiver)
    internal/mode/      ModeTransitionManagerImpl, NativeModeStateWriter
  iosMain/kotlin/       2 files — actual de NativeLibraryLoader (no-op, link estatico)
                        y AudioBridgeProvider (lanza NotImplementedError hasta WA-3.2)
  commonTest/kotlin/    5 suites
  main/cpp/             C++20 engine
    api/                C API — watermelon_audio.h (191 functions, pure C)
    dsp/                watermelon-dsp sub-library (30 files, zero deps)
    effects/            watermelon-effects sub-library (59 files, 23 efectos + EffectRegistry)
    engines/            watermelon-engines sub-library (SynthEngine + 6 engines
                        header-only, SoundFontManager)
    voice/              watermelon-voice sub-library (10 files, VoiceManager, VoicePool)
    looper/             watermelon-looper sub-library (16 files, header-only salvo
                        LooperExporter.cpp)
    core/               AudioEngine facade + subsistemas (22 files)
    backends/           IAudioBackend, BackendManager, SplitBackend, DriftResampler,
                        OboeBackend + LibusbBackend (Android), CoreAudioBackend.mm (iOS),
                        PlatformBackends.cpp (unico punto que nombra backends concretos)
    jni/                5 files — jni_audio_bridge.cpp (278 JNIEXPORT), jni_engine,
                        jni_usb, jni_benchmark, jni_common.h
    platform/           Logger.h/.cpp (logcat / os_log / stderr), Platform.h,
                        PlatformAndroid.cpp, PlatformApple.cpp, PlatformIsa.inc (ISA comun)
    ios/                CMakeLists.txt del build iOS (separado del que maneja AGP)
```

---

## Stack

| Componente | Version |
|------------|---------|
| Kotlin | 2.4.0   |
| AGP | 9.2.1   |
| Oboe | 1.10.0  |
| C++ | C++20   |
| CMake | 3.22.1  |
| Min SDK | 29      |
| Compile SDK | 36      |
| kotlinx-coroutines | 1.11.0  |
| TinySoundFont | 0.9     |
| iOS deployment target | 15.0    |

Targets KMP: `androidTarget`, `iosArm64`, `iosSimulatorArm64`.

---

## Reglas

### C++ Audio Thread
- Audio callback 100% **lock-free** — nunca mutex, new, malloc
- `std::atomic` para parametros UI↔Audio
- `incrementStateVersion()` despues de modificar estado observable
- Parametros con smoothing para evitar zipper noise
- Logging via `platform/Logger.h` — NOT RT-safe, solo fuera del hot path

### C++ portabilidad (iOS)
- Todo el motor cross-compila para iOS **salvo** `jni/`, `usb/`, `OboeBackend`,
  `LibusbBackend` y `PlatformAndroid.cpp`
- Prohibido `#include <jni.h>` / `<android/...>` fuera de esas capas —
  `scripts/check-cpp-portability.sh` lo hace fallar en CI (WA-0.4)
- Codigo especifico de plataforma: detras de `IAudioBackend` o de `wma::platform`
  (`platform/Platform.h`). Lo que depende solo del ISA va en `platform/PlatformIsa.inc`
- Un solo punto nombra backends concretos: `backends/PlatformBackends.cpp`
- El thread RT **jamas** entra a Kotlin (el GC de Kotlin/Native no es RT-safe):
  el estado sale por polling o colas lock-free

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

bash scripts/run-cpp-tests.sh              # Suite C++ de host (527 tests, googletest)
bash scripts/check-cpp-portability.sh      # Guardrail WA-0.4 (jni.h / android/)
bash scripts/build-ios.sh                  # libwatermelon_audio.a — ambos slices + link check
python3 scripts/c-api-gap.py               # Regenera docs/kmp/c_api_coverage.md

./gradlew :audio:compileKotlinIosArm64     # Compilar Kotlin para iOS
./gradlew :audio:iosSimulatorArm64Test     # Tests K/N en simulador (requiere Xcode
                                           # con first-launch hecho, ver docs/kmp)
```

> El build de iOS vive en `audio/src/main/cpp/ios/CMakeLists.txt`, **separado** del que
> maneja AGP: ese es Android-specific de punta a punta (Oboe, libusb, JNI,
> PlatformAndroid, flags de linker GNU que Apple ld rechaza). Separarlos deja el build
> que shippea en riesgo cero.

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
