# Watermelon Audio

Motor de sintesis en tiempo real con efectos DSP profesionales. C++20 + Oboe + Kotlin Multiplatform.

**Watermelon Studios**

---

## Arquitectura

```
audio/src/
  commonMain/kotlin/    74 files — pure Kotlin, zero Android deps
    api/                AudioEngine interface, IAudioNativeBridge, IEffectManager,
                        IInputBridge + AudioInput (camino de entrada, WA-5.5),
                        factories (AudioEngine, EffectManager, AudioInput,
                        StateSynchronizer)
    domain/             Effect types, oscillators, scales, modes, USB types, errors
    callback/           AudioLogger, AudioAnalyticsListener (dependency inversion)
    internal/           AudioEngineImpl, EffectManagerImpl, StateSynchronizer,
                        ScaleQuantizer, ChordGenerator, BridgeConcurrency,
                        util/Format, util/Time
                        expect: AudioBridgeProvider, NativeLibraryLoader,
                                currentDeviceCapabilities
    domain/device/      DeviceCapabilities (interfaz de hechos) + Snapshot
    domain/input/       InputSource + InputMetering (snapshot de 7 valores)
  androidMain/kotlin/   23 files — JNI bridge, USB, platform-specific
    internal/bridge/    AudioNativeBridge (3,209 LOC, 290 external funs)
    internal/usb/       USB audio driver (DataStore, BroadcastReceiver)
    internal/mode/      ModeTransitionManagerImpl, NativeModeStateWriter
  iosMain/kotlin/       5 files — IosAudioBridge (sobre cinterop), AudioBridgeProvider,
                        AudioSessionManager (AVAudioSession como Flow),
                        NativeLibraryLoader (no-op, link estatico),
                        DeviceCapabilitiesProvider (NSProcessInfo)
  commonTest/kotlin/    8 suites  ·  iosTest/kotlin/ 4 suites
  main/cpp/             C++20 engine
    api/                C API — watermelon_audio.h (251 functions, pure C)
    dsp/                watermelon-dsp sub-library (30 files, zero deps)
    effects/            watermelon-effects sub-library (59 files, 23 efectos + EffectRegistry)
    engines/            watermelon-engines sub-library (SynthEngine + 6 engines
                        header-only, SoundFontManager)
    voice/              watermelon-voice sub-library (10 files, VoiceManager, VoicePool)
    looper/             watermelon-looper sub-library (16 files, header-only salvo
                        LooperExporter.cpp)
    core/               AudioEngine facade + subsistemas (22 files)
    backends/           IAudioBackend, BackendManager, SplitBackend, DriftResampler,
                        OboeBackend + LibusbBackend (Android),
                        CoreAudioBackend.mm (iOS, output + captura full-duplex),
                        PlatformBackends.cpp (unico punto que nombra backends concretos)
    jni/                5 files — jni_audio_bridge.cpp (279 JNIEXPORT), jni_engine,
                        jni_usb, jni_benchmark, jni_common.h
    platform/           Logger.h/.cpp (logcat / os_log / stderr), Platform.h,
                        PlatformAndroid.cpp, PlatformApple.cpp, PlatformIsa.inc (ISA comun)
    ios/                CMakeLists.txt del build iOS (separado del que maneja AGP)

harness/src/            :harness — app de prueba multiplataforma (WA-5.5). NO se publica
  commonMain/kotlin/    HarnessApp — la UI entera (Compose Multiplatform)
  androidMain/kotlin/   MainActivity (shell) + AndroidManifest (RECORD_AUDIO)
  iosMain/kotlin/       MainViewController (shell)
harness/iosApp/         Proyecto de Xcode. Embebe el framework de :harness, NO el
                        XCFramework de WA-4.1 (usar los dos duplica el motor).
                        Info.plist: NSMicrophoneUsageDescription +
                        CADisableMinimumFrameDurationOnPhone (sin esta ultima
                        Compose aborta al arrancar)
```

> Los conteos de arriba son orientativos y **driftean**: al 2026-07-27 estaban mal
> commonMain (67→74), la C API (187→251, la movio WA-2.5/2.6), AudioNativeBridge
> (3352→3209 LOC, 289→290 funs), los JNIEXPORT (278→279) y los tests (749→762,
> 101→105). Re-medir es barato, asi que **medir antes de citar**:
>
> ```bash
> find audio/src/commonMain -name '*.kt' | wc -l          # archivos por source set
> grep -c 'external fun' audio/src/androidMain/kotlin/com/watermellonstudios/audio/internal/bridge/AudioNativeBridge.kt
> grep -c JNIEXPORT audio/src/main/cpp/jni/jni_audio_bridge.cpp
> python3 scripts/c-api-gap.py                            # C API + delegacion (§4b)
> ```

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

bash scripts/run-cpp-tests.sh              # Suite C++ de host (774 tests, googletest)
                                           # Kotlin: 112 iOS sim / 69 JVM

# Los mismos 774 bajo sanitizers. NO son opcionales: el CI tiene un job para
# cada uno y encontraron dos bugs reales que el resto del gate no ve.
# OJO: `detect_leaks=1` (lo que usa ci.yml) NO existe en macOS y rompe el
# discovery de gtest — en esta maquina va sin el.
#
# Desde el cambio a `DISCOVERY_MODE PRE_TEST` eso ya NO rompe el build: el
# listado de tests pasó de tiempo de build a tiempo de ctest, asi que el
# sintoma es `discover_tests failed to run command` con exit 8 de ctest, en
# vez de un `ninja: build stopped` que no menciona ningun test. Sigue siendo
# un error — pero ahora dice donde.
ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  SANITIZE=address,undefined bash scripts/run-cpp-tests.sh --timeout 180
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
  SANITIZE=thread bash scripts/run-cpp-tests.sh --timeout 180
# El TSan local (libc++) es MAS DEBIL que el del CI (libstdc++): una carrera
# real sobrevivio 15 corridas aca y fue roja a la primera alla. Para carreras
# el CI es la autoridad.
bash scripts/check-cpp-portability.sh      # Guardrail WA-0.4 (jni.h / android/)
bash scripts/build-harness.sh              # :harness: Android + framework iOS +
                                           # símbolos + shell de Xcode + ARRANQUE
                                           # de la app (lo único que agarra un
                                           # Info.plist incompleto: Compose aborta
                                           # el proceso desde PlistSanityCheck)
bash scripts/check-no-ui-in-library.sh     # Guardrail WA-5.5: la UI de :harness no
                                           # puede entrar al artefacto publicado.
                                           # Lo que de verdad mide es el classpath
                                           # resuelto de :audio.
bash scripts/build-ios.sh                  # libwatermelon_audio.a — ambos slices + link check
python3 scripts/c-api-gap.py               # Gap C API vs JNI + delegacion (WA-2.6).
                                           # Imprime; docs/kmp/c_api_coverage.md
                                           # se actualiza a mano con esa salida

./gradlew :audio:assembleWatermelonXCFramework   # XCFramework (device + simulador)

./gradlew :audio:compileKotlinIosArm64     # Compilar Kotlin para iOS (por target)
./gradlew :audio:compileIosMainKotlinMetadata  # El source set COMPARTIDO iosMain.
                                           # Los gates por target no lo cubren, y es
                                           # lo que compila un consumidor KMP con
                                           # targets iOS. Necesita
                                           # enableCInteropCommonization.
./gradlew :audio:iosSimulatorArm64Test     # Tests K/N en simulador (requiere Xcode
                                           # con first-launch hecho, ver docs/kmp)

./gradlew :harness:assembleDebug           # APK del harness de UI (WA-5.5)
./gradlew :harness:linkDebugFrameworkIosSimulatorArm64   # HarnessKit.framework
```

> `:harness` es la app de prueba multiplataforma (WA-5.5). **No se publica**, y eso es
> estructural: no aplica `maven-publish`, los workflows publican con `:audio:publishAll...`
> path-qualified, y `check-no-ui-in-library.sh` lo hace fallar si Compose aparece en el
> classpath de `:audio`. Su framework de iOS **no es** el XCFramework de WA-4.1 — las dos
> vías de consumo son alternativas y usar ambas duplicaría el motor.

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
