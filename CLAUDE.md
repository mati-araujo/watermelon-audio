# Watermelon Audio

Motor de sintesis en tiempo real con efectos DSP profesionales. C++20 + Oboe + Kotlin
Multiplatform. **Watermelon Studios.** Lo consume NoisyPad via GitHub Packages.

Este archivo es el **nucleo**. Cada trampa local vive en el `CLAUDE.md` del subdirectorio que
toca (se carga al trabajar ahi); la historia y los porques largos, en `docs/dev/`. Antes de
editar en un directorio con `CLAUDE.md`, leelo.

## Mapa

```
audio/src/
  commonMain/kotlin/   Kotlin puro, cero deps de Android: api/ (AudioEngine, IAudioNativeBridge,
                       IEffectManager, IInputBridge, factories), domain/, callback/, internal/
  androidMain/kotlin/  JNI bridge (AudioNativeBridge), USB, mode
  iosMain/kotlin/      IosAudioBridge sobre cinterop, AudioSessionManager, NativeLibraryLoader no-op
  commonTest/ iosTest/
  androidUnitTest/     el ARNES JNI: ejecuta JNIEXPORT reales en la JVM del host
  main/cpp/            motor C++20
    api/               C API pura (watermelon_audio.h)
    dsp/ effects/ engines/ voice/ looper/ analysis/ (afinador)   sub-librerias
    core/              fachada AudioEngine + subsistemas
    backends/          IAudioBackend, Oboe/Libusb (Android), CoreAudio (iOS), PlatformBackends.cpp
    jni/               capa JNI   ·   platform/  Logger, Platform, PlatformIsa.inc
    ios/               CMakeLists del build iOS (separado del de AGP)
    tests/hostjni/     la libreria JNI para el HOST, con FakeAudioBackend
harness/               app de prueba multiplataforma (Compose MP). NO se publica
```

Detalle por archivo y su historia: `docs/dev/arquitectura.md`.

## Conteos medidos

Esta tabla **la escribe `scripts/check-doc-counts.py`, no la mano** (REQ-021); corre en
`gate.sh` y en el CI, asi que un numero stale es **rojo**. Se re-mide con
`python3 scripts/check-doc-counts.py --update` y **su diff es la revision**. Editarla a mano
fabrica la prueba de una medicion que no se hizo. No vigila lo que exige *correr* algo (suite de
host, cobertura del arnes JNI): eso lo imprime el comando que lo produce. **Medir antes de
citar** cualquier otro numero; por que, en `docs/dev/conteos-que-envejecen.md`.

<!-- BEGIN conteos-medidos — los escribe scripts/check-doc-counts.py, NO la mano -->
| métrica | valor | qué mide |
|---|---|---|
| `kt-commonMain` | 99 | archivos .kt en commonMain |
| `kt-androidMain` | 22 | archivos .kt en androidMain |
| `kt-iosMain` | 6 | archivos .kt en iosMain |
| `bridge-loc` | 3715 | LOC de AudioNativeBridge.kt |
| `bridge-external` | 319 | `external fun` en AudioNativeBridge |
| `jniexport-bridge` | 308 | JNIEXPORT en jni_audio_bridge.cpp |
| `jniexport-total` | 321 | JNIEXPORT en todo jni/*.cpp |
| `wma-api` | 285 | declaraciones WMA_API en la C API |
| `analysis-files` | 19 | fuentes .h/.cpp en cpp/analysis/ (sin tests/) |
| `callers-baseline` | 1 callback-externo / 15 deuda / 1 entrada / 64 sonda-de-tests | reparto del baseline de llamadores |
| `ver-kotlin` | 2.4.20 | version de Kotlin |
| `ver-agp` | 9.4.1 | version de AGP |
<!-- END conteos-medidos -->

## Stack

| Componente | Version |
|------------|---------|
| Kotlin | ver «Conteos medidos» |
| AGP | ver «Conteos medidos» |
| Oboe | 1.11.0  |
| C++ | C++20   |
| CMake | 3.22.1  |
| Min SDK | 29      |
| Compile SDK | 37      |
| kotlinx-coroutines | 1.11.0  |
| TinySoundFont | 0.9, **fork** (ver `thirdparty/VENDORED.md`) |
| iOS deployment target | 15.0    |
| googletest (suite de host) | ver `audio/src/main/cpp/thirdparty/googletest.cmake` |
| Compose Multiplatform (sólo `:harness`) | ver `gradle/libs.versions.toml` |
| Xcode del CI | ver la clave `xcode` de `.github/toolchain-pins.json` |

Targets KMP: `androidTarget`, `iosArm64`, `iosSimulatorArm64`. Las filas "ver" apuntan a la
**unica fuente** del pin: `scripts/check-dep-pins.py` falla si un pin aparece en un segundo
lugar. Lo vendorizado y como buscar sus CVE: `audio/src/main/cpp/thirdparty/VENDORED.md`.

## Innegociables

- **Callback de audio 100 % lock-free**: nunca mutex, new, malloc ni log. Lo verifica
  `check-rt-safety.py`. Reglas y trampas: `audio/src/main/cpp/CLAUDE.md`.
- **El thread RT jamas entra a Kotlin**; el estado sale por polling o colas lock-free.
- **commonMain**: cero `android.*`/`java.*`. **Portabilidad C++**: `jni.h`/`android/` sólo en
  `jni/`, `usb/`, `OboeBackend`, `LibusbBackend`, `PlatformAndroid.cpp`.
- **Tests**: nunca sincronizar con una duracion y afirmar despues (REQ-002); ver
  `audio/src/main/cpp/CLAUDE.md`.
- **JNI**: tres preguntas (simbolo, firma, ejecucion), cada una con su gate; ver
  `audio/src/main/cpp/jni/CLAUDE.md`.
- 🔴 **`.github/local-gate.json` lo escribe `gate.sh` y NADIE MAS**, agentes incluidos.
  Escribirlo o "regenerarlo" a mano para acallar un CI rojo es fabricar la prueba. Si el gate no
  pasa, se arregla el codigo.
- **Un baseline es un trinquete y su diff es la revision.** Si tu cambio paga una deuda, saca la
  entrada; si aparece una nueva, se arregla el codigo. Cobertura del walker de RT y homonimos de
  mechanism-callers: **se renombra el metodo**, no se redeclara (`scripts/CLAUDE.md`).

## Commits y merges

**Master sólo acepta merge commits** (ADR-0011; squash y rebase deshabilitados desde el
2026-08-31). Por eso **cada commit de etapa llega a `master` y release-please lo lee**: su tipo es
una entrada del CHANGELOG publico.

- Commit de etapa **interno** → `chore:`, `build:`, `ci:`, `test:`, `docs:` o `refactor:` (ocultos).
- `feat:`, `fix:`, `perf:`, `revert:` sólo para lo que un consumidor entenderia en el CHANGELOG.
  Pregunta: *"¿esto le cambia algo a NoisyPad?"*
- El titulo del PR va con prefijo convencional (nombra el merge commit).

Medicion que sostiene la regla: `docs/dev/commits-y-changelog.md`.

## Comandos

```bash
bash scripts/gate.sh                     # TODO lo de los jobs ios, build y cpp-tests-macos; si da
                                         # verde atesta en .github/local-gate.json y el CI del PR
                                         # saltea esos jobs. En push a master el CI corre entero.
bash scripts/gate.sh --only ios          # un gate solo, para iterar. NO atesta
bash scripts/gate.sh --with-sanitizers   # + ASan/UBSan local (opt-in, no atesta)

./gradlew :audio:assembleDebug           # [gate] (4 ABIs)   ·  :audio:assembleRelease [gate]
./gradlew :audio:testDebugUnitTest       # [gate] commonTest JVM + arnes JNI; imprime su cobertura
./gradlew :audio:iosSimulatorArm64Test   # [gate] (Xcode con first-launch hecho)
./gradlew :audio:compileKotlinIosArm64   # Kotlin para iOS, por target
./gradlew :audio:compileIosMainKotlinMetadata   # el source set COMPARTIDO iosMain: NI gate.sh NI
                                         # el CI lo compilan, y es lo que compila un consumidor KMP
                                         # con targets iOS. Correlo si tocas iosMain. Necesita
                                         # kotlin.mpp.enableCInteropCommonization (gradle.properties)
bash scripts/run-cpp-tests.sh            # [gate] suite C++ de host; ctest imprime el total
bash scripts/build-ios.sh                # [gate] libwatermelon_audio.a, ambos slices + link
bash scripts/build-harness.sh            # [gate] :harness Android + iOS + arranque de la app
./gradlew :audio:publishToMavenLocal     # NoisyPad local (o includeBuild en su settings)
./gradlew :audio:publishAllPublicationsToGitHubPackagesRepository   # GitHub (CI/produccion)
bash scripts/test-attestation.sh         # el verificador de la atestacion (~10 s)

# Sanitizers: SIEMPRE con CTEST_JOBS=4 en esta maquina. Si da timeout, bajar CTEST_JOBS,
# nunca subir el techo. detect_leaks=1 no existe en macOS.
CTEST_JOBS=4 ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  SANITIZE=address,undefined bash scripts/run-cpp-tests.sh --timeout 180
CTEST_JOBS=4 TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
  SANITIZE=thread bash scripts/run-cpp-tests.sh --timeout 180
```

`gate.sh` **no** corre los tres jobs de ubuntu (`cpp-tests`, `-asan`, `-tsan`): nunca se
atestan, porque el TSan local (libc++) es mas debil que el del CI (libstdc++). **El TSan de
Linux es la unica autoridad sobre carreras.** Reparto y numeros: `docs/ci/local_first.md`.

Que vigila cada lint del gate y sus trinquetes: `scripts/CLAUDE.md`; los scripts
de medicion del afinador y del corpus, en `audio/src/main/cpp/analysis/CLAUDE.md`.

## Agregar codigo nuevo

- Efecto → `audio/src/main/cpp/effects/CLAUDE.md`
- Synth engine → `audio/src/main/cpp/engines/CLAUDE.md`
- Funcion JNI → `audio/src/main/cpp/jni/CLAUDE.md`

## Links

- Consumidor principal: NoisyPad (`github.com/mati-araujo`, privado). Workflow: `CONTRIBUTING.md`
- Historia de la extraccion: `docs/00_master_plan.md`
