# Watermelon Audio

Motor de sintesis en tiempo real con efectos DSP profesionales. C++20 + Oboe + Kotlin Multiplatform.

**Watermelon Studios**

---

## Arquitectura

```
audio/src/
  commonMain/kotlin/    94 files — pure Kotlin, zero Android deps
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
  androidMain/kotlin/   21 files — JNI bridge, USB, platform-specific
    internal/bridge/    AudioNativeBridge (3357 LOC, 308 external funs)
    internal/usb/       USB audio driver (DataStore, BroadcastReceiver)
    internal/mode/      ModeTransitionManagerImpl, NativeModeStateWriter
  iosMain/kotlin/       6 files — IosAudioBridge (sobre cinterop), AudioBridgeProvider,
                        AudioSessionManager (AVAudioSession como Flow),
                        NativeLibraryLoader (no-op, link estatico),
                        DeviceCapabilitiesProvider (NSProcessInfo)
  commonTest/kotlin/    8 suites  ·  iosTest/kotlin/ 4 suites
  androidUnitTest/      4 files — el ARNES JNI (REQ-016). Corre en la JVM del host y
                        EJECUTA funciones JNIEXPORT reales contra un JNIEnv real,
                        entrando por AudioNativeBridge
  main/cpp/             C++20 engine
    api/                C API — watermelon_audio.h (274 declaraciones WMA_API, pure C)
    dsp/                watermelon-dsp sub-library (30 files, zero deps)
    effects/            watermelon-effects sub-library (59 files, 23 efectos + EffectRegistry)
    engines/            watermelon-engines sub-library (SynthEngine + 6 engines
                        header-only, SoundFontManager)
    voice/              watermelon-voice sub-library (10 files, VoiceManager, VoicePool)
    looper/             watermelon-looper sub-library (16 files, header-only salvo
                        LooperExporter.cpp)
    analysis/           watermelon-analysis (14 files) — el afinador de REQ-001:
                        ring lock-free + thread de analisis + snapshot atomico,
                        PhaseSlopeEstimator (S2), StrobeTracker (S6),
                        InharmonicityEstimator (S7), FastModeTracker (S5),
                        IntonationMode (S9). REQ-014 le sumo la compuerta de
                        ausencia de señal, el arbitraje por signo y el contador
                        acumulado de discontinuidades (snapshot: 17 valores)
    core/               AudioEngine facade + subsistemas (22 files)
    backends/           IAudioBackend, BackendManager, SplitBackend, DriftResampler,
                        OboeBackend + LibusbBackend (Android),
                        CoreAudioBackend.mm (iOS, output + captura full-duplex),
                        PlatformBackends.cpp (unico punto que nombra backends concretos)
    jni/                5 files — jni_audio_bridge.cpp (295 JNIEXPORT), jni_engine,
                        jni_usb, jni_benchmark, jni_common.h
    platform/           Logger.h/.cpp (logcat / os_log / stderr), Platform.h,
                        PlatformAndroid.cpp, PlatformApple.cpp, PlatformIsa.inc (ISA comun)
    ios/                CMakeLists.txt del build iOS (separado del que maneja AGP)
    tests/hostjni/      REQ-016: libwatermelon_audio.{so,dylib} PARA EL HOST — el
                        motor + la capa JNI compilados con el jni.h del JDK, para
                        que un test de JVM pueda cargarlos. Lleva FakeAudioBackend
                        adentro: valida la frontera JNI/Kotlin, NO audio en device

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

> Los conteos de arriba son orientativos y **driftean**. Re-medidos el 2026-08-27 tras cerrar
> REQ-013 y REQ-017: commonMain 93→**94**, androidMain **21**, iosMain **6**,
> AudioNativeBridge 3332→**3357** LOC y 306→**308** funs, JNIEXPORT 295→**297**,
> C API 272→**274**, suite de host 1131→**1154** tests. Y la tabla de **Stack** también estaba
> vieja: AGP decía 9.2.1 contra **9.3.2** y Kotlin 2.4.0 contra **2.4.10** — o sea que el drift
> no es sólo de los conteos, alcanza a cualquier número escrito a mano en este archivo.
>
> (Tandas anteriores: 2026-08-25 al cerrar REQ-014 venía de commonMain 91, AudioNativeBridge
> 3304 LOC / 303 funs, JNIEXPORT 292, C API 269 y 1011 tests; 2026-08-20 al cerrar REQ-001 S10,
> de commonMain 83, AudioNativeBridge 3229 / 291, JNIEXPORT 280, C API 253 y 883 tests.) El
> afinador entero (REQ-001) agregó `cpp/analysis/` con 14 archivos.
>
> **QUINTA tanda, el 2026-08-27 al cerrar REQ-016**: la suite de host es de **1169** tests, no
> 1154 — o sea que el número de arriba envejeció **el mismo día** en que se lo re-midió. Y los
> tests de Kotlin en la JVM eran **153**, no los 69 que decía la sección de comandos; con el
> arnés JNI son **169**. Las `JNIEXPORT` de `jni/*.cpp` son **310** en total (297 del bridge + 8
> de benchmark + 3 de usb + 2 `JNI_OnLoad`/`JNI_OnUnload`), de las cuales **308** son entradas
> `Java_*` que Kotlin declara.
>
> 🔴 **Que este bloque haya quedado stale CUATRO veces seguidas es el dato, no el accidente**, y
> la cuarta agregó un eje nuevo: las VERSIONES del stack, que nadie sospechaba. Un
> conteo escrito a mano envejece en silencio: nadie lo lee como "esto puede estar viejo", se lee
> como un hecho. Le pasó también a la spec viva del afinador —decía "snapshot de 14 valores"
> cuando ya eran 16— y a un KDoc que decía "ocho floats" con quince, contra el que un consumidor
> real diseñó tres pedidos. Re-medir es barato, asi que **medir antes de citar**:
>
> ```bash
> find audio/src/commonMain -name '*.kt' | wc -l          # archivos por source set
> wc -l audio/src/androidMain/kotlin/com/watermellonstudios/audio/internal/bridge/AudioNativeBridge.kt
> grep -c 'external fun' audio/src/androidMain/kotlin/com/watermellonstudios/audio/internal/bridge/AudioNativeBridge.kt
> grep -c JNIEXPORT audio/src/main/cpp/jni/jni_audio_bridge.cpp
> grep -c WMA_API audio/src/main/cpp/api/watermelon_audio.h
> grep -nE '^agp|^kotlin ' gradle/libs.versions.toml     # la tabla de Stack tambien driftea
> python3 scripts/c-api-gap.py                            # C API + delegacion (§4b)
> ```
>
> **REQ-016 construyó el primer pedazo de esa salida**, para su propia rebanada: el conteo de
> cobertura del arnés JNI (`13 de 310`) sale MEDIDO en cada corrida — el numerador se anota al
> cruzar la frontera, el denominador se cuenta del árbol, y sacar un test **baja** el número
> (probado por su propio self-test). No cubre este bloque; muestra la forma.
>
> 🔴 **Cuatro veces stale es la evidencia de que "medir antes de citar" NO alcanza**: es una regla
> que sólo vive en prosa, y este repo ya sabe cómo terminan (WD-1.1: el callback violaba sus
> reglas escritas en 65 lugares). La salida coherente con el resto del repo es un guardrail que
> re-mida y falle contra este archivo, como `rt-safety` o `mechanism-callers`. No existe todavía.

---

## Stack

| Componente | Version |
|------------|---------|
| Kotlin | 2.4.10  |
| AGP | 9.3.2   |
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
> - **Un metodo nuevo con un nombre comun puede APAGAR parte del lint.** El walker sigue solo
>   las llamadas que resuelven a UNA definicion, asi que un segundo `run`/`analyze`/`read` en
>   CUALQUIER parte del arbol vuelve ambigua una llamada y le saca cobertura — con el lint en
>   verde. Paso dos veces el 2026-08-19. Por eso `scripts/rt-coverage-baseline.txt` declara
>   que funciones se alcanzan y el lint falla si el conjunto cambia: si te sale en rojo una
>   funcion que no tocaste, la salida es **renombrar tu metodo**, no redeclarar la cobertura.

### Tests — como se espera (REQ-002)

- **Nunca sincronices con una duracion y afirmes despues.** Da verde en una maquina
  ociosa y rojo en un runner con siete jobs: asi se cayo `master` tres veces el
  2026-08-20, y la release 2.3.0 quedo esperando.
- `wma_test::waitUntil(pred, techo)` para esperar a que algo **ocurra**.
  `wma_test::sleepFixed` para las esperas de **ausencia**, que no se pueden esperar por
  condicion. Las dos en `cpp/tests/support/TestWait.h`.
- Un `sleep_for` crudo tiene que decir que es (`// WAIT-OK: razon`) o el gate falla.
  El polling dentro de un bucle con deadline se reconoce solo.
- **Un receptor registrado en el motor tiene que vivir MAS que el motor.** Declararlo
  local en el cuerpo de un test es un use-after-free con abort, no un detalle de estilo
  — medido 5/5. La regla la declara el KDoc de `wma_looper_set_event_callback`, y la
  violaban cuatro tests.
- 🔴 **Sacar una espera ciega no es el arreglo; poner una condicion en su lugar lo es.**
  Al quitar un `sleep` sin reemplazarlo por una condicion, un test de este repo dejo de
  detectar el mutante que antes mataba 20 contra 0 — y pasaba en las dos escalas.

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

> 🔴 **El camino JNI tiene DOS preguntas, y una sola no alcanza** (REQ-016).
>
> - *¿el simbolo existe?* — `scripts/check-jni-symbols.py` (MINI-001), que compara **solo
>   NOMBRES**. Da verde con las 310 funciones jamas ejecutadas, y **no ve un desajuste de
>   FIRMA**: un `Int` declarado donde el C++ espera `jlong` compila de los dos lados,
>   linkea, pasa ese gate y corrompe memoria en el device.
> - *¿alguien lo ejecuta?* — el arnes de `androidUnitTest`, que entra por
>   `AudioNativeBridge` y cruza la frontera de verdad. Hasta REQ-016 la respuesta era
>   **nadie**: 310 `JNIEXPORT` y ningun test las corria.
>
> 🔴 **Y el arnes cubre 13 de 310**, sobre un backend FALSO. El conteo lo imprime el propio
> arnes en cada corrida, con los dos numeros MEDIDOS —jamas escritos a mano— justamente
> para que "13/310" no se lea nunca como "el JNI esta probado". No reemplaza al smoke en
> device; nada de lo que corre en el host lo hace.

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
`cpp-tests-asan`, `cpp-tests-tsan`). Nunca se atestan y siempre corren en el CI,
porque el TSan local (libc++) es **mas debil** que el del CI (libstdc++): una carrera
sobrevivio 15 corridas locales y otra dio 0/60, y las dos fueron rojas a la primera
alla. **El TSan de Linux es la unica autoridad sobre carreras**, y por eso ninguna
atestacion local puede reemplazarlo — seria cambiar un chequeo fuerte por uno flojo y
registrarlo como prueba.

> 🔴 **Ojo con la razon que este parrafo daba antes.** Decia tambien que esos tres
> "jamas estan en el camino critico". Eso valia mientras `ios` costaba ~1000 s; con
> `ios` atestandose en 9 s, los tres de ubuntu **SON** el camino critico entero de un
> PR atestado. La conclusion no cambio, pero ahora se apoya en una sola pata. Detalle
> en `docs/ci/local_first.md` §2.

### Los comandos sueltos

Siguen haciendo falta para iterar, y varios cargan advertencias medidas que costaron
sesiones enteras. Los marcados **[gate]** ya los corre `scripts/gate.sh`.

```bash
./gradlew :audio:assembleDebug                                     # [gate] Build debug (4 ABIs)
./gradlew :audio:assembleRelease                                   # [gate] Build release
./gradlew :audio:publishToMavenLocal                               # Publish local
./gradlew :audio:publishAllPublicationsToGitHubPackagesRepository   # Publish GitHub

bash scripts/run-cpp-tests.sh              # [gate] Suite C++ de host (1169 tests, googletest).
                                           # ctest corre en PARALELO desde el 18/08: 149,7 s -> 20,4 s.
                                           # `CTEST_JOBS=n` lo baja si hace falta
                                           # Kotlin: 112 iOS sim / 162 JVM (los dos numeros
                                           # RE-MEDIDOS el 27/08: decian 1154 y 69)

./gradlew :audio:testDebugUnitTest         # [gate] commonTest en la JVM + el ARNES JNI (REQ-016).
                                           # Construye la libreria de host sola (dependsOn +
                                           # inputs.files de buildHostJniLib) y le pone el
                                           # java.library.path. Imprime el conteo de cobertura:
                                           #   [REQ-016] arnes JNI ...: 13 de 310 ... hueco: 297
                                           # 🔴 `dependsOn` SOLO ORDENA. La libreria esta declarada
                                           # como INPUT del test porque sin eso, romper el simbolo
                                           # del lado C++ dejaba la task UP-TO-DATE y el arnes en
                                           # VERDE sin ejecutar nada — medido, y es la misma trampa
                                           # que ya tenia documentada cinteropWatermelonAudio.

bash scripts/build-host-jni.sh             # La libreria de host suelta, para iterar.
                                           # WMA_JAVA_HOME/JAVA_HOME manda el JDK: tiene que ser
                                           # EL MISMO que corre los tests. NO usa find_package(JNI)
                                           # —busca AWT y falla contra un JDK headless, medido en
                                           # ubuntu— y compila jni/ con -Wall -Wextra -Werror, que
                                           # es el unico gate de warnings que esa capa tiene.

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

# Los mismos 1131 bajo sanitizers. NO son opcionales: el CI tiene un job para
# cada uno y encontraron dos bugs reales que el resto del gate no ve.
# OJO: `detect_leaks=1` (lo que usa ci.yml) NO existe en macOS y rompe el
# discovery de gtest — en esta maquina va sin el.
#
# Desde el cambio a `DISCOVERY_MODE PRE_TEST` eso ya NO rompe el build: el
# listado de tests pasó de tiempo de build a tiempo de ctest, asi que el
# sintoma es `discover_tests failed to run command` con exit 8 de ctest, en
# vez de un `ninja: build stopped` que no menciona ningun test. Sigue siendo
# un error — pero ahora dice donde.
CTEST_JOBS=4 ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  SANITIZE=address,undefined bash scripts/run-cpp-tests.sh --timeout 180
CTEST_JOBS=4 TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
  SANITIZE=thread bash scripts/run-cpp-tests.sh --timeout 180
# El TSan local (libc++) es MAS DEBIL que el del CI (libstdc++): una carrera
# real sobrevivio 15 corridas aca y fue roja a la primera alla. Para carreras
# el CI es la autoridad.
#
# 🔴 LOS SANITIZERS LOCALES VAN CON `CTEST_JOBS=4`, y no es opcional.
# Con ctest en paralelo a full (18/08) TSan bajaba a 192 s pero DABA TIMEOUT en
# `RateInvariance.EveryEffectStaysFiniteAndBoundedAtEveryRate` y en
# `NyquistLimits.NoNewDivergenceAppearsBelowFortyKilohertz`: con 10 procesos de
# TSan compitiendo por el ancho de banda de memoria, esos barridos pasan de
# ~105 s a mas de 180. Con `CTEST_JOBS=4` quedan en 114 s y 86 s (63 % y 48 %
# del techo) y la corrida entera tarda 361 s — todavia 3,7x mejor que los
# 1344 s en serie.
#
# La salida correcta cuando esto da timeout es BAJAR `CTEST_JOBS`, nunca subir
# el techo: bajo paralelismo "cuanto tarda este test" deja de ser una propiedad
# del test y pasa a incluir la contencion, y subir el techo taparia el
# crecimiento real. El presupuesto que importa es el de los sanitizers.
#
# NO es el default del script a proposito: el CI no pasa `--timeout` y ahi el
# `-j` completo mide 45 % mejor. Poner 4 por defecto pesimizaria el CI para
# resolver una restriccion de esta maquina.
python3 scripts/check-test-waits.py        # [gate] Guardrail REQ-002. Toda espera cruda en
                                           # un test tiene que estar CLASIFICADA. No busca
                                           # "esperas sospechosas" por su forma —eso se evade
                                           # sin querer— sino que cada `sleep_for` diga cual de
                                           # las cuatro cosas es:
                                           #   polling    bucle con deadline. Lo reconoce SOLO.
                                           #   estimulo   la duracion ES el experimento
                                           #              (jitter, intervalo entre callbacks,
                                           #              forzar un orden). `// WAIT-OK: razon`
                                           #   presencia  espera y afirma -> wma_test::waitUntil
                                           #   ausencia   espera a que NO pase ->
                                           #              wma_test::sleepFixed
                                           # --self-test verifica que puede fallar, y corre
                                           # ANTES del lint (misma razon que check-rt-safety).
                                           # 🔴 Si es PRESENCIA, agrandar el sleep NO lo
                                           # arregla: alcanza en tu maquina y se queda corto en
                                           # el runner. Eso fue REQ-002.

bash scripts/check-time-dependence.sh      # Corre la suite con las esperas CIEGAS colapsadas y
                                           # dice que test cambia de veredicto. Lo corre el CI
                                           # en el job `cpp-tests` (ubuntu, que nunca se
                                           # saltea), NO gate.sh: cuesta una corrida entera.
                                           # 🔴 NO carga la maquina, y no es un descuido:
                                           # esta MEDIDO que la contencion no reproduce esta
                                           # clase — 40 quemadores sobre 10 nucleos dan 0/10,
                                           # `taskpolicy -c background` + carga da 1/10 (y ese
                                           # uno es un timeout). Un `sleep_for(120ms)` es
                                           # tiempo ABSOLUTO: ahogar la maquina no achica la
                                           # ventana, y encima el render tarda mas, o sea que
                                           # le da MAS margen. Colapsar la espera: 10/10 en 2 s.
                                           # Cubre UNA clase (espera ciega insuficiente). NO ve
                                           # la otra que REQ-002 encontro —una espera POR
                                           # CONDICION cuya condicion se vuelve inalcanzable— ni
                                           # las esperas de AUSENCIA, cuyo modo de falla es un
                                           # falso VERDE. El script lo imprime el mismo.

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
                                           # ya no se reproduce.
                                           # SEGUNDO TRINQUETE, sobre la COBERTURA:
                                           # scripts/rt-coverage-baseline.txt declara QUE
                                           # funciones alcanza el walker, y el lint falla
                                           # si el conjunto cambia en cualquier direccion.
                                           # Existe porque la cobertura se encoge SOLA: el
                                           # walker sigue solo lo que resuelve a UNA
                                           # definicion, asi que un metodo nuevo con un
                                           # nombre comun (`run`, `analyze`, `read`...) en
                                           # CUALQUIER parte del arbol vuelve ambigua una
                                           # llamada y el lint queda verde revisando menos.
                                           # Se redeclara con --update-coverage, y SU DIFF
                                           # ES LA REVISION
python3 scripts/check-mechanism-callers.py # [gate] Guardrail REQ-013. Contesta "?quien LLAMA
                                           # a esto?": falla si una funcion de produccion tiene
                                           # sus UNICOS llamadores en tests. REQ-012 entrego un
                                           # mecanismo verificado con TSan y mutacion que nadie
                                           # llamaba en produccion, y la suite entera estaba
                                           # verde. --self-test corre ANTES (misma razon que
                                           # check-rt-safety).
                                           # scripts/mechanism-callers-baseline.txt es un
                                           # TRINQUETE bidireccional, y cada entrada lleva su
                                           # CATEGORIA y su RAZON (una entrada sin razon falla).
                                           # Al 26/08: 44 sonda-de-tests, 30 deuda (que son ~13
                                           # mecanismos), 1 entrada, 1 callback externo.
                                           # 🔴 NO es un detector de codigo muerto: si no la
                                           # llama NADIE, no se reporta. Y NO ve el hueco del
                                           # JNI (REQ-016) — ahi la pregunta es quien EJECUTA.
                                           # Falso negativo MEDIDO: 117 nombres simples tienen
                                           # homonimos en produccion y quedan tapados por la
                                           # fusion conservadora (`reset` tiene 96 definiciones).
                                           # La salida es RENOMBRAR, igual que en rt-coverage.

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
bash scripts/fetch-corpus.sh                # REQ-001 S10: baja el corpus grabado y
                                           # VERIFICA su checksum contra
                                           # analysis/tests/corpus-manifest.txt.
                                           # Hoy el manifiesto esta VACIO a proposito:
                                           # el corpus no existe todavia. Los tests de
                                           # robustez que dependen de el salen SKIPPED
                                           # y NUNCA passed — una corrida que no
                                           # verifico no se puede leer como cobertura,
                                           # que es la misma regla de regen-golden.sh.

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
