# Watermelon Audio

Motor de sintesis en tiempo real con efectos DSP profesionales. C++20 + Oboe + Kotlin Multiplatform.

**Watermelon Studios**

---

## Arquitectura

```
audio/src/
  commonMain/kotlin/    83 files — pure Kotlin, zero Android deps
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
    internal/bridge/    AudioNativeBridge (3229 LOC, 291 external funs)
    internal/usb/       USB audio driver (DataStore, BroadcastReceiver)
    internal/mode/      ModeTransitionManagerImpl, NativeModeStateWriter
  iosMain/kotlin/       5 files — IosAudioBridge (sobre cinterop), AudioBridgeProvider,
                        AudioSessionManager (AVAudioSession como Flow),
                        NativeLibraryLoader (no-op, link estatico),
                        DeviceCapabilitiesProvider (NSProcessInfo)
  commonTest/kotlin/    8 suites  ·  iosTest/kotlin/ 4 suites
  main/cpp/             C++20 engine
    api/                C API — watermelon_audio.h (253 functions, pure C)
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
    jni/                5 files — jni_audio_bridge.cpp (280 JNIEXPORT), jni_engine,
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

> 🔴 **Estas reglas ya NO son solo prosa: las verifica `scripts/check-rt-safety.py`** (WD-1.1),
> que corre en `gate.sh` y en el CI. Antes de WD-1.1 el callback las violaba en **65 lugares**,
> y dos de los peores sobrevivian a `NDEBUG` porque llamaban a `wma::logMessage` directo en vez
> de pasar por los macros `LOGI/LOGW`. Lo que hay que saber para no reintroducirlas:
>
> - **NO loguees adentro del callback.** Ni "periodicamente", ni "solo en debug". Los bloques
>   `WMA_AUDIT` que habia eran un DEFECTO CONOCIDO, no un precedente. Si necesitas saber que
>   pasa adentro, agrega un `wma::RtCounter` (`platform/RtCounter.h`) — cuesta un `fetch_add`
>   relajado y se lee desde el thread de control.
> - **`reset()` es RT.** `EffectChain::reset()` y `Effect::reset()` los despacha `onAudioReady`
>   (ver `Effect.h`), aunque el nombre no lo sugiera.
> - **Los handlers de voces son RT.** `VoiceManager::handleNoteOn/Off/ParamChange` los despacha
>   la cola lock-free desde el thread de audio.
> - **La captura es un SEGUNDO thread RT.** `InputNode::processInputBlock` corre en el thread
>   del stream de entrada de Oboe, con su propio DSP. Lo que vale para el callback de salida
>   vale ahi.
> - **`try_lock` si, `lock()` no.** Y ojo con lo que parece un getter: `findVocoderIndex()`
>   tomaba `chainMutex` y lo llamaban cuatro setters desde el thread de audio (WD-1.6).
> - **El flush de denormales es POR THREAD.** `flushDenormalsRtSafe()` va al principio de cada
>   callback; `flushDenormals()` loguea y es solo para el arranque, en el thread de control.

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

### El gate — un solo comando antes de cada PR

```bash
bash scripts/gate.sh          # corre TODO lo que el CI iba a correr en los jobs
                              # `ios`, `build` y `cpp-tests-macos`, y si da verde
                              # deja la atestacion en .github/local-gate.json
```

El CI verifica esa atestacion y **saltea esos tres jobs en el PR** en vez de repetir
el trabajo: ~6 min local contra ~17,5 min de camino critico. En `push: master` el CI
paga su costo entero **siempre**, sin excepcion. Diseño y numeros: `docs/ci/local_first.md`.

> 🔴 **`.github/local-gate.json` lo escribe `gate.sh` y NADIE MAS.** Escribirlo o
> editarlo a mano —incluido "regenerarlo" para acallar un CI rojo— no es un atajo:
> es fabricar la prueba de que corrio algo que no corrio. **Esto aplica igual a los
> agentes.** Si el gate no pasa, se arregla el codigo. El unico costo de no tener
> atestacion es que el CI corre entero, que es exactamente lo que pasaba antes.

```bash
bash scripts/gate.sh --only ios          # un gate solo, para iterar. NO atesta
bash scripts/gate.sh --with-sanitizers   # + ASan/UBSan local (opt-in, no se atesta)
bash scripts/test-attestation.sh         # el verificador contra arboles mutados (~10 s)
```

**Lo que `gate.sh` NO corre, a proposito:** los tres jobs de ubuntu (`cpp-tests`,
`cpp-tests-asan`, `cpp-tests-tsan`). Nunca se atestan y siempre corren en el CI.
Suman 695 s de runner, van en paralelo y jamas estan en el camino critico, asi que
correrlos aca no ahorra nada — y el TSan local tarda **865 s contra 295 s** del CI y
encima es **mas debil**: una carrera sobrevivio 15 corridas locales y otra dio 0/60,
y las dos fueron rojas a la primera alla. **El TSan de Linux es la unica autoridad
sobre carreras.**

### Los comandos sueltos

Siguen haciendo falta para iterar, y varios cargan advertencias medidas que costaron
sesiones enteras. Los marcados **[gate]** ya los corre `scripts/gate.sh`.

```bash
./gradlew :audio:assembleDebug                                     # [gate] Build debug (4 ABIs)
./gradlew :audio:assembleRelease                                   # [gate] Build release
./gradlew :audio:publishToMavenLocal                               # Publish local
./gradlew :audio:publishAllPublicationsToGitHubPackagesRepository   # Publish GitHub

bash scripts/run-cpp-tests.sh              # [gate] Suite C++ de host (774 tests, googletest)
                                           # Kotlin: 112 iOS sim / 69 JVM

# audio/src/main/cpp/effects/tests/reset-baseline.txt — TRINQUETE del contrato
# de reset(), igual que scripts/rt-safety-baseline.txt: el test falla si aparece
# deuda nueva Y TAMBIEN si una entrada declarada ya no se reproduce.
# VACIO desde WD-3.2: tuvo 16 de 23 y se pagaron todas. Ademas Effect::reset()
# es VIRTUAL PURA, asi que un efecto nuevo no puede olvidarse — el compilador lo
# para, y uno sin estado escribe un `{}` explicito con su razon.
# Lo que es no-determinista POR DISEÑO no va aca: va a nonDeterministicByDesign()
# en test_golden_properties.cpp, y la exclusion esta MEDIDA por su propio test.

bash scripts/regen-golden.sh               # WD-2.2: RECAPTURAR los golden de DSP.
                                           # Es una tarea explicita y aparte a
                                           # proposito: recapturar no puede ser un
                                           # efecto colateral de correr los tests.
                                           # En modo regeneracion los tests quedan
                                           # SKIPPED, no PASSED — una corrida que
                                           # ESCRIBE no puede pasar por una que
                                           # VERIFICA (misma regla que local-gate.json).
                                           # Los .resp son texto: SU DIFF ES LA REVISION.
                                           # Si aparece un preset que el cambio no
                                           # tocaba, eso es el hallazgo.

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
bash scripts/check-cpp-portability.sh      # [gate] Guardrail WA-0.4 (jni.h / android/)
python3 scripts/check-rt-safety.py         # [gate] Guardrail WD-1.1. Camina el call-graph
                                           # del callback de audio y falla si aparece
                                           # logging, allocation, lock que bloquee o
                                           # shared_ptr. --graph imprime lo alcanzado;
                                           # --self-test verifica que el lint puede fallar
                                           # (corre ANTES del lint en gate.sh y en el CI:
                                           # si el parser se rompe queda en verde para
                                           # siempre). Excepciones: `// RT-SAFE-ALLOW: razon`
                                           # en el codigo para lo INOFENSIVO;
                                           # scripts/rt-safety-baseline.txt para la deuda
                                           # con dueno declarado — y ese archivo es un
                                           # TRINQUETE: falla tambien si una entrada suya
                                           # ya no se reproduce
bash scripts/build-harness.sh              # [gate] :harness: Android + framework iOS +
                                           # símbolos + shell de Xcode + ARRANQUE
                                           # de la app (lo único que agarra un
                                           # Info.plist incompleto: Compose aborta
                                           # el proceso desde PlistSanityCheck)
bash scripts/check-no-ui-in-library.sh     # [gate] Guardrail WA-5.5: la UI de :harness no
                                           # puede entrar al artefacto publicado.
                                           # Lo que de verdad mide es el classpath
                                           # resuelto de :audio.
bash scripts/build-ios.sh                  # [gate] libwatermelon_audio.a — ambos slices + link
                                           # check. gate.sh lo corre SUELTO y ANTES de
                                           # Gradle, y no es redundante: ver el comentario
                                           # en KmpNativeConventionPlugin.kt
python3 scripts/c-api-gap.py               # Gap C API vs JNI + delegacion (WA-2.6).
                                           # Imprime; docs/kmp/c_api_coverage.md
                                           # se actualiza a mano con esa salida

./gradlew :audio:assembleWatermelonXCFramework   # XCFramework (device + simulador).
                                           # YA NO es [gate] ni corre en el CI del PR: desde
                                           # el 2026-08-05 sus dos pasos de ci.yml llevan
                                           # `if: github.event_name != 'pull_request'`, asi
                                           # que se verifica en cada push a master y nada mas.
                                           # No tiene consumidores —NoisyPad usa la coordenada
                                           # Gradle KMP y :harness embebe su propio
                                           # HarnessKit.framework— y no se distribuye. Lo que
                                           # probaba lo cubre el harness: su framework lleva
                                           # los mismos simbolos wma_*. Paridad vigilada por
                                           # scripts/test-attestation.sh

./gradlew :audio:compileKotlinIosArm64     # Compilar Kotlin para iOS (por target)
./gradlew :audio:compileIosMainKotlinMetadata  # El source set COMPARTIDO iosMain.
                                           # Los gates por target no lo cubren, y es
                                           # lo que compila un consumidor KMP con
                                           # targets iOS. Necesita
                                           # enableCInteropCommonization.
./gradlew :audio:iosSimulatorArm64Test     # [gate] Tests K/N en simulador (requiere Xcode
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
