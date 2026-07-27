# Requerimiento: KMP/iOS Readiness — watermelon-audio

**Proyecto:** watermelon-audio (v1.8.1). Coordenada **KMP**: `com.watermellonstudios:audio`
— `:audio-android` es el módulo Android suelto, **no** el que debe usar un consumidor KMP
**Documento hermano:** `NoisyPad/docs/kmp/kmp_requirements.md` (consumidor)
**Estado:** EN CURSO — **Fases 0 y 3 esencialmente cerradas**: Kotlin/Native ejecuta el motor C++ en el simulador (75 tests iOS, 0 fallas). Queda `DeviceCapabilities`, el input path de iOS, el XCFramework, y la validación en device (G2)
**Fecha:** 2026-07-05 · **Última actualización:** 2026-07-25 (cerrados: WA-0.1/0.2/0.3/0.4,
WA-1.4, WA-1.6, WA-2.0, **WA-2.1 completo**, WA-2.2, WA-2.3, WA-2.4 output, WA-2.7,
**WA-3.1, WA-3.2, WA-3.4**, WA-3.3 parcial, WA-T.1/T.3; `InputNode` unificado JNI↔C API)
**Objetivo estratégico:** que la librería de audio compile y funcione en iOS con el mismo motor C++ y la misma API Kotlin (`commonMain`) que hoy consume NoisyPad Android, habilitando la versión iOS de NoisyPad.

---

## 1. Objetivo

1. Publicar watermelon-audio como **librería KMP completa**: metadata común + AAR Android + klibs iOS con el motor C++ empaquetado como XCFramework.
2. Reutilizar **sin cambios** el DSP C++ (dsp/effects/engines/voice/looper), el sistema de voces, el looper y TinySoundFont — ya son C++20 puro y están modularizados en sub-librerías CMake.
3. Implementar un **backend de audio Core Audio** para iOS detrás de la interfaz `IAudioBackend` existente, con las mismas garantías RT-safe (lock-free, zero-alloc en el callback).
4. Convertir la **C API existente** (`api/watermelon_audio.h`) en la única superficie de puente hacia Kotlin (cinterop en iOS, y a mediano plazo también debajo del JNI Android), eliminando el doble mantenimiento.
5. Mantener el flujo Android actual (Oboe, USB/libusb, JNI) **intacto y sin regresiones**.

**No-alcance:** USB audio en iOS (limitación de plataforma, ver WA-5.1), AUv3 (backlog estratégico), MIDI hardware iOS.

---

## 2. Diagnóstico (estado actual)

Auditoría 2026-07-05, **re-auditada 2026-07-22** contra el código real. Puntos de
partida excepcionales — el trabajo de extracción de 2026-04 dejó la librería medio
camino andado:

> **Corrección de la auditoría original (2026-07-22).** La versión inicial de este
> documento daba por sentado que `commonMain` era Kotlin puro y que compilaría para
> iOS sin cambios. **Era falso.** Al agregar los targets (WA-0.2) aparecieron 34
> errores de compilación en 6 archivos: `String.format` (×14), `@Volatile` de JVM
> (×8), `System.currentTimeMillis/nanoTime` (×7), `android.util.Log` (×2, violando
> la regla de CLAUDE.md) y `java.*` — incluido **reflection JVM en el path de
> `setXY`**, es decir una vez por frame de gesto. El compilador de Android nunca los
> marcó porque `commonMain` sólo se compilaba contra el classpath de la JVM.
> Lección para el resto del programa: **la pureza de `commonMain` no se asume, se
> compila.** Ver §5 WA-0.2 para el detalle y §16 para el estado.

**Ya resuelto (no tocar, capitalizar):**

- El módulo `audio/` **ya es KMP**: `commonMain` (62 archivos: `api/`, `domain/`, `callback/`, `internal/` con `AudioEngineImpl`, `EffectManagerImpl`, `StateSynchronizer`) + `androidMain` (21 archivos) + `iosMain` (2 archivos, WA-0.2). Ya existe el patrón `expect/actual` (`AudioBridgeProvider`, `NativeLibraryLoader`) — y son los **únicos dos** `expect` del módulo, lo que mantiene la superficie de portabilidad mínima.
- La interfaz `IAudioNativeBridge` (commonMain) ya define el contrato completo del puente — iOS solo necesita otra implementación.
- El C++ está **modularizado en sub-librerías CMake sin dependencias Android**: `watermelon-dsp`, `watermelon-effects`, `watermelon-engines`, `watermelon-voice`, `watermelon-looper` (INTERFACE). Third-party portable: TinySoundFont (MIT), stb_vorbis (PD).
- Abstracciones de plataforma ya existen: `platform/Logger.h` (callback configurable, sin `__android_log_print` en DSP) y `platform/Platform.h` (denormals, prioridad de thread, SIMD caps) con `PlatformAndroid.cpp` como única implementación.
- Ya existe una **C API pura**: `api/watermelon_audio.h/.cpp` con **189** funciones.
- Backends detrás de interfaz: `IAudioBackend` + `BackendManager` (hot-swap) — el diseño ya contempla múltiples backends.
- Tests C++ (googletest) **host-compilables**: dsp/effects/looper/voice/engine/usb + **core** (nuevo con WA-2.0, 22 tests de `AudioEngine`) suites, scripts `run-cpp-tests.{ps1,sh}`, integrados a `check`. **527 tests en total.**

**Trabajo pendiente (el objeto de este requerimiento):**

| Área | Estado | Problema |
|---|---|---|
| ~~Audio I/O~~ | **RESUELTO (WA-2.4):** `CoreAudioBackend` (output) compila y se enchufa; falta validar sonido en device (WA-4.3) e input path iOS |
| Puente nativo | JNI: `jni_audio_bridge.cpp` 3.583 LOC, **278 JNIEXPORT** | iOS no tiene JNI; además el JNI llama al engine directo, no vía C API |
| C API | **189 funciones** | **Gap ~89 funciones** vs JNI (cobertura incompleta: looper avanzado, mixer, regions, transiciones de modo, análisis) |
| ~~Platform C++~~ | **RESUELTO (WA-2.2):** `PlatformApple.cpp` existe; el `.a` de iOS linkea sin gaps de proyecto |
| Kotlin androidMain | `Mp4AacTranscoder` (MediaCodec), `UsbAudioManagerImpl`, `TrustedUsbDevicesRepository` (DataStore), `DeviceCapabilities` (Context) | Sin contrapartes iOS ni interfaces comunes en algunos casos |
| Build | CMake vía AGP `externalNativeBuild` + `ios/CMakeLists.txt` (separado) | **RESUELTO parcial (WA-2.1):** hay toolchain iOS y `libwatermelon_audio.a` por slice; **falta** el empaquetado XCFramework (WA-4.1) y que Gradle invoque el build C++ de iOS |
| ~~Targets Gradle~~ | ~~Solo `androidTarget`~~ | **RESUELTO (WA-0.2):** `iosArm64` + `iosSimulatorArm64` compilan |
| ~~Pureza de commonMain~~ | ~~Asumida~~ | **RESUELTO (WA-0.2):** 34 deps JVM/Android eliminadas; ver recuadro arriba |
| Tests Kotlin | 5 suites en `commonTest` (34 tests) | Cubren ChordGenerator, ScaleQuantizer, EffectManagerBatch, Format, Time. **Falta** lo que WA-1.5 pedía: `StateSynchronizer`, `AudioEngineImpl`, mapeo de errores |
| Herramientas locales | Xcode 26.6 sin *first launch* | `iosSimulatorArm64Test` **se cuelga** esperando privilegios de admin; requiere `sudo xcodebuild -runFirstLaunch` (ver §11) |

---

## 3. Decisiones estratégicas

| # | Decisión | Recomendación | Justificación |
|---|---|---|---|
| D1 | **Mecanismo de puente iOS** | **Kotlin/Native cinterop contra la C API** (`watermelon_audio.h`). Nada de puente Swift manual ni Objective-C++ intermedio | La C API ya existe; cinterop genera bindings directos con overhead mínimo (crítico para `setXY` de alta frecuencia). Kotlin `commonMain` queda intacto. |
| D2 | **Backend de audio iOS** | `CoreAudioBackend` implementando `IAudioBackend`. Iteración 1: **AVAudioEngine + AVAudioSourceNode/SinkNode** (más simple, RT-safe si el render block entra directo a C++). Escalar a **AURemoteIO (AUAudioUnit)** solo si la latencia medida no alcanza | AVAudioSourceNode da callbacks RT reales con mucho menos código; el diseño `IAudioBackend` permite swap posterior sin tocar el motor. |
| D3 | **JNI sobre C API** | Refactor incremental: `jni_audio_bridge.cpp` pasa a llamar funciones `wma_*` de la C API en lugar del engine directo | **Refactor de más alto valor del requerimiento:** garantiza por construcción que Android e iOS ejecutan el mismo código de orquestación; toda función nueva nace multiplataforma. Se hace por categorías, sin big-bang. |
| D4 | **USB audio** | Fuera de alcance iOS v1. `LibusbBackend`, `usb/` C++ y `UsbAudioManagerImpl` Kotlin se compilan **solo para Android** | iOS no permite acceso USB genérico sin DriverKit (iPadOS) y entitlements; no es critical path de NoisyPad iOS. La API Kotlin USB queda en androidMain o marcada android-only. |
| D5 | **Distribución** | XCFramework (device + simulator) generado en el build, **embebido en el klib** vía cinterop/link estático; publicación a GitHub Packages como artefacto KMP único (misma coordenada, Release Please sigue versionando) | NoisyPad consume una sola dependencia KMP; sin CocoaPods/SPM que mantener. SPM puede evaluarse después si terceros consumen la lib. |
| D6 | **Threading hacia Kotlin** | El thread RT **jamás entra a Kotlin**. Estado sale por polling (patrón actual de `StateSynchronizer`) o colas lock-free leídas desde un dispatcher Kotlin | Kotlin/Native GC no es RT-safe; el diseño actual ya respeta esto — se formaliza como regla. |
| D7 | **Logging iOS** | `Logger.cpp` con sink `os_log` por defecto en Apple + mismo callback configurable | Simetría con Android (logcat). |
| D8 | **Versión Kotlin** | Este repo está en **2.4.0** (2026-07-22). Verificar el lockstep con NoisyPad antes de publicar metadata KMP y mantenerlo en adelante | La metadata KMP es sensible a la versión del compilador. Kotlin 2.4 aporta dos piezas que este requerimiento usa: `kotlin.concurrent.Volatile` y `kotlin.time.Clock` multiplataforma, que evitaron tener que meter expect/actual o kotlinx-datetime para el port de WA-0.2. |

---

## 4. Arquitectura objetivo

Marcas: ✅ existe · 🚧 pendiente.

```
audio/src/
├── commonMain/kotlin/            (api/, domain/, callback/, internal/ — ahora sí portables)
│   ├── internal/util/Format.kt               ✅ WA-0.2: fmt(decimals), toHex4()
│   │                                            (reemplazo de String.format)
│   ├── internal/util/Time.kt                 ✅ WA-0.2: epochMillis(), monotonicNanos(),
│   │                                            formatEpochMillisUtc()
│   └── internal/bridge/BridgeConcurrency.kt  🚧 WA-1.4: mutexes por categoría extraídos
│                                                de AudioNativeBridge, compartidos con iOS
├── androidMain/kotlin/           (INTACTO: JNI bridge, USB, MediaCodec, DataStore)
├── iosMain/kotlin/
│   ├── internal/native/NativeLibraryLoader.kt ✅ WA-0.2: actual no-op (link estático)
│   ├── internal/bridge/AudioBridgeProvider.kt ✅ WA-0.2: actual que lanza
│   │                                             NotImplementedError hasta WA-3.2
│   ├── internal/bridge/IosAudioBridge.kt      🚧 WA-3.2: IAudioNativeBridge sobre cinterop
│   ├── internal/audio/AudioSessionManager.kt  🚧 WA-3.4: AVAudioSession (categoría, buffer,
│   │                                             interrupciones)
│   └── internal/util/DeviceCapabilities.kt    🚧 WA-1.2/3.3: actual ProcessInfo/UIDevice
├── nativeInterop/cinterop/watermelon_audio.def 🚧 WA-3.1
└── main/cpp/                                    (SIN TOCAR TODAVÍA — todo 🚧)
    ├── api/watermelon_audio.h/.cpp   🚧 WA-2.5: completar cobertura 1:1 con JNI
    ├── jni/                          🚧 WA-2.6: refactor a wrapper delgado de la C API
    ├── backends/
    │   ├── OboeBackend.*             ✅ Android-only, sin cambios
    │   ├── LibusbBackend.*           ✅ Android-only, sin cambios
    │   └── CoreAudioBackend.mm/.cpp  🚧 WA-2.4 (NUEVO, iOS)
    ├── platform/
    │   ├── PlatformAndroid.cpp       ✅ sin cambios
    │   └── PlatformApple.cpp         🚧 WA-2.2 (NUEVO: denormals FPCR arm64 factorizado,
    │                                    pthread time-constraint, os_log sink)
    └── dsp/ effects/ engines/ voice/ looper/ thirdparty/   (SIN CAMBIOS — se recompilan)
```

---

## 5. Requerimientos — Fase 0: Análisis y fundaciones

Prioridades: P0 = bloqueante, P1 = importante, P2 = diferible. Esfuerzo: S (< 1 día), M (días), L (semanas).

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf | Estado |
|---|---|---|---|---|---|---|
| WA-0.1 | **Gap analysis C API vs JNI** | Inventario función por función: **278** JNIEXPORT vs **187** `wma_*`. Clasificar el gap por categoría y marcar cuáles son Android-only (USB) y no se portan | Tabla de cobertura en `docs/kmp/c_api_coverage.md` con estado por función | P0 | M | ✅ **HECHO** 2026-07-22 — ver `docs/kmp/c_api_coverage.md`, reproducible con `scripts/c-api-gap.py` |
| WA-0.2 | Targets iOS en Gradle | Ampliar el convention plugin `watermelon.kmp.native`: `iosArm64` + `iosSimulatorArm64`; verificar el lockstep de Kotlin con NoisyPad (D8). ~~Los 52 archivos de commonMain deben compilar para iOS sin cambios~~ → **requirió 34 fixes de portabilidad**, ver nota abajo | `:audio:compileKotlinIosArm64` verde | P0 | M | ✅ **HECHO** 2026-07-22 |
| WA-0.3 | CI macOS | Job en GitHub Actions (runner macOS): compila targets iOS + tests C++ con clang de Xcode (WA-T.1) | Job verde en PR de prueba | P0 | M | ✅ **HECHO Y VERIFICADO** 2026-07-22 — job `ios` verde en PR #45 (5m36s), con `iosSimulatorArm64Test` ejecutando de verdad y 490/490 tests C++ bajo Apple clang |
| WA-0.4 | Guardrail de portabilidad C++ | Check de CI que falle si aparece `#include <jni.h>`/`<android/...>` fuera de `jni/`, `backends/Oboe*`, `backends/Libusb*`, `usb/` y `platform/PlatformAndroid.cpp`. **Ampliar a Kotlin:** el equivalente para `commonMain` es que WA-0.3 compile los targets iOS en cada PR — es lo que habría atajado los 34 errores de WA-0.2 | CI rojo ante include prohibido (probado con PR sintético) | P1 | S | ✅ **HECHO** 2026-07-25 — `scripts/check-cpp-portability.sh`, paso en el job `cpp-tests` (ver nota) |

### Nota de cierre — WA-0.2 (2026-07-22)

Targets agregados en `KmpNativeConventionPlugin.kt`. `commonMain` **no era portable**;
los 34 errores se resolvieron así:

| Causa | Usos | Resolución |
|---|---|---|
| `String.format` | 14 | `internal/util/Format.kt` — `Double/Float.fmt(decimals)`, `Int.toHex4()` |
| `@Volatile` (JVM) | 8 | `import kotlin.concurrent.Volatile` (multiplataforma desde Kotlin 2.1) |
| `System.currentTimeMillis()` / `nanoTime()` | 7 | `internal/util/Time.kt` — `epochMillis()` / `monotonicNanos()` sobre `kotlin.time` |
| `android.util.Log` en `ScaleQuantizer` | 2 | `AudioLogger` inyectable (default `NoOpAudioLogger`) |
| `java.text.SimpleDateFormat` | 1 | `formatEpochMillisUtc()` (algoritmo civil-from-days) |
| Reflection JVM (`getMethod("getIntervals")`) | 1 | Overload tipado sobre `ScaleMode` |

**Dos hallazgos que exceden la portabilidad:**

1. La reflection eliminada estaba en `ScaleQuantizer.quantizeFrequency`, invocada
   desde `AudioEngineImpl.setXY` — o sea **una vez por frame de gesto**.
   `ScaleMode.intervals` ya era una propiedad tipada: la reflection no compraba nada
   y costaba un `getMethod` + `invoke` por frame. Es una mejora de rendimiento en el
   path de control, no sólo un fix de compilación.
2. Los `android.util.Log.d("XY_TRACE", …)` de `ScaleQuantizer` eran incondicionales.
   Ahora el sink es `NoOpAudioLogger` por default.
   ⚠️ **Cambio de comportamiento en Android:** ese trace ya no se emite salvo que se
   asigne `ScaleQuantizer.logger`.

**Otros cambios de comportamiento a comunicar a NoisyPad:**

- `UsbTestReport` imprime `Generated:` en **UTC** (antes: zona local del device).
  El string lleva sufijo ` UTC` para que no sea ambiguo.
- `EffectChainSnapshot.timestamp` pasa de `System.nanoTime()` a `monotonicNanos()`.
  Sigue siendo monótono y en nanosegundos; sólo cambia el origen, que ya era
  arbitrario (sólo las diferencias tenían sentido).

**Estado del bridge iOS:** `getAudioBridge()` en `iosMain` lanza `NotImplementedError`
con un mensaje explícito hasta WA-3.2. Es deliberado: habilita publicar metadata KMP
(gate G1) sin que un consumidor pueda creer que hay audio en iOS.

**Verificación:** `compileKotlinIosArm64` + `compileKotlinIosSimulatorArm64` verdes;
34/34 tests de `commonTest` pasan en JVM; `linkDebugTestIosSimulatorArm64` produce el
binario — o sea `commonTest` también es portable. **No** se corrió en simulador (ver §11).

### Nota de cierre — WA-0.3 (2026-07-22)

Job `ios` agregado a `.github/workflows/ci.yml` (runner `macos-latest`, obligatoriamente
arm64 porque los tests `iosSimulatorArm64` no ejecutan en x86). Cubre, en un solo job
para no pagar dos veces el minutaje de macOS (×10 vs Linux):

1. **Suite C++ con Apple clang** (cierra WA-T.1) — corre primero, antes del setup de
   JDK/SDK/Gradle, para fallar barato ante una rotura de portabilidad.
2. **Compilación de ambos targets iOS** — el guardrail que evita que `commonMain`
   vuelva a acumular dependencias JVM (métrica 6 de §15).
3. **`iosSimulatorArm64Test`** — el único lugar donde el Kotlin compartido se
   *ejecuta* bajo Kotlin/Native, no sólo se compila.

**Hallazgo — `scripts/run-cpp-tests.sh` estaba roto en macOS.** El script fallaba
antes de compilar una sola línea:

```
scripts/run-cpp-tests.sh: line 45: GEN_ARGS[@]: unbound variable
```

Causa: macOS ships **bash 3.2.57** (Apple no lo actualiza por la licencia GPLv3 de
bash 4+), y en bash 3.2 con `set -u` expandir un array **vacío** aborta el script.
`GEN_ARGS` queda vacío si no hay `ninja`, y `SAN_ARGS` queda vacío en **toda** corrida
sin sanitizers — o sea el script nunca habría corrido en un runner macOS. Corregido con
`"${arr[@]+"${arr[@]}"}"`, que expande a cero palabras en bash 3.2 y en bash 4/5 por
igual (los jobs de ubuntu no cambian de comportamiento). Es exactamente la clase de
bug que WA-T.1 existía para cazar, y apareció en el primer intento.

**Hallazgo — AGP descarga el NDK aunque el job sea sólo iOS.** Compilar
`compileKotlinIosArm64` con un SDK sin NDK dispara una auto-instalación silenciosa de
~1 GB. El job lo instala explícitamente vía `sdkmanager` para que la descarga sea
visible en el log y quede fijada a la versión del catálogo. Además, el módulo aplica
`com.android.library`, así que **el Android SDK es necesario incluso para tareas
iOS-only**: sin él Gradle ni configura el proyecto.

**Cachés:** `~/.konan` (toolchain de Kotlin/Native, ~1 GB por corrida sin cache),
más el cache de `gradle/actions/setup-gradle`.

**Resultado de la primera corrida (PR #45): el job encontró dos bugs reales**, ambos
invisibles para g++ porque son diagnósticos exclusivos de clang:

1. **`platform/Logger.h` no compilaba con clang** (`'format' attribute argument not
   supported: gnu_printf`). El guard era `#if defined(__GNUC__) || defined(__clang__)`,
   pero clang define `__GNUC__` también, así que caía en la rama de `gnu_printf` — un
   archetype que clang rechaza. **Efecto lateral:** el NDK de Android también es clang,
   o sea que ahí el atributo venía siendo *ignorado en silencio* y no había ningún
   chequeo de formato en el build Android. Ahora sí lo hay (y no agregó ni un warning).
2. **Campos muertos en `FDN.h`** (`-Wunused-private-field`): `mSmoothSize` y
   `mSmoothDecayTime`, declarados pero nunca usados. Ver nota abajo.

> [!NOTE]
> **Lección de método.** El CI solo habría mostrado el #2: ninja se detiene en el primer
> error y paró en FDN. El problema de `Logger.h` — el más importante, y en un archivo que
> este mismo documento daba por listo para iOS — apareció sólo porque se compiló local
> con `ninja -k 0`. Al encarar WA-2.1 conviene compilar local y en paralelo, no iterar
> contra el CI.

> [!WARNING]
> **Deuda destapada por FDN, fuera del alcance de WA-0.3.** El smoothing que
> `mSmoothSize`/`mSmoothDecayTime` pretendían **sí hace falta**: `FDN::process()` corre
> en el audio thread y lee `mSize` por sample para calcular los delay taps, así que mover
> el control de size lo cambia de golpe. `setSize`/`setDecayTime` ya usan ganancias
> double-buffered para el feedback, pero `mSize` no pasa por ese mecanismo. Contradice la
> regla de CLAUDE.md ("parámetros con smoothing para evitar zipper noise"). Cablearlo es
> un cambio de DSP audible y va en su propio ticket.

### Nota de cierre — WA-0.1 (2026-07-22)

Entregable: **`docs/kmp/c_api_coverage.md`**, reproducible con
`python3 scripts/c-api-gap.py` (el script lee el JNI y el header directamente, así que
los números siguen al código y se re-corren al cerrar cada categoría de WA-2.5).

| Métrica | Valor |
|---|---|
| JNIEXPORT | 278 |
| Funciones `wma_*` | 187 |
| Cubiertas (match exacto de tokens) | 136 |
| **Gap total** | **142** |
| — USB, no se porta (D4) | 32 |
| — **Gap portable** | **110** |
| — de esos, con near-match a revisar | 31 |
| — **neto a implementar en WA-2.5** | **~79** |

**La estimación original (~89) queda confirmada:** el gap portable real está entre 79 y
110 según cuántos near-match resulten ser la misma función con otro nombre, y ~89 cae
cómodo en ese rango. WA-2.5 no hay que re-dimensionarlo.

**El hallazgo que sí cambia la planificación: el looper es el 35% del gap portable**
(39 de 110), muy por encima de cualquier otra categoría (le sigue input/monitor con 14).
Es el bloque de trabajo dominante de WA-2.5 y conviene decidir temprano si NoisyPad iOS
v1 necesita el looper completo o alcanza con un subconjunto.

Limitación honesta del método: el matching es por conjunto de tokens y no puede decidir
los near-match — `SetNoiseGateEnabled ~ wma_input_set_noise_gate` es plausible, pero
`LooperGetArmedTrack ~ wma_looper_get_track_peak` es un falso positivo evidente. Se
revisan a mano al encarar cada categoría.

---

## 6. Requerimientos — Fase 1: Quick wins (sin tocar el critical path)

Mejoras de valor inmediato para el mantenimiento Android actual, que además despejan el camino iOS.

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf | Estado |
|---|---|---|---|---|---|---|
| WA-1.1 | Logging unificado en Kotlin | `AudioNativeBridge.kt` usa `android.util.Log` directo; migrar a la interfaz `AudioLogger` ya existente en `commonMain/callback/`. **Quedan 10 archivos en androidMain** (`AudioNativeBridge`, `NativeLibraryLoader`, `ModeTransitionManagerImpl`, `NativeModeStateWriter`, `DeviceCapabilities`, USB ×3, `LatencyBenchmarkRunner`, `Mp4AacTranscoder`) — nótese que en androidMain `android.util.Log` está permitido por CLAUDE.md, así que esto es prolijidad, no bloqueo | Cero `android.util.Log` fuera de un actual Android de `AudioLogger` | P1 | S | Parcial — `ScaleQuantizer` (commonMain) migrado en WA-0.2 |
| WA-1.2 | `DeviceCapabilities` común | Definir interfaz/expect en commonMain (RAM, low-latency hint, API level abstracto); actual Android actual queda como está; deja el hueco para el actual iOS | `AudioEngineFactory` consume la abstracción | P1 | S | ✅ **HECHO** 2026-07-25 — `domain/device/DeviceCapabilities.kt` + `expect fun currentDeviceCapabilities()`, actuals Android e iOS, `AudioEngineConfig.tunedFor()`. 12 tests nuevos. Ver nota |
| WA-1.3 | API USB segregada | Asegurar que los tipos/factories USB (`IUsbAudioManager`, `UsbAudioManagerFactory`) no sean requeridos para usar el resto de la API (interface segregation). Mover a androidMain lo que no necesite estar en common, o documentar como android-only.<br>**Acoplamiento real medido (2026-07-22): sólo 3 puntos.** (a) `AudioBackendType` vive en `domain/usb/UsbAudioTypes.kt` pero **no es un tipo USB** — lo consumen `AudioEngine` y `AudioEngineImpl`; debería mudarse a `domain/`. (b) `IAudioNativeBridge.isUsbBackendAvailable()`. (c) `IAudioNativeBridge.setUsbLatencyProfile()`. Nada más de commonMain depende de `domain/usb/` | Un consumidor sin USB compila para iOS sin stubs USB | P1 | M | Pendiente — **decisión 2026-07-22:** en WA-0.2 se optó por portar `domain/usb/` en su lugar (sigue en commonMain) para no mezclar un cambio de API pública dentro de WA-0.2 |
| WA-1.4 | Extraer `BridgeConcurrency` | Los mutexes por categoría (lifecycle/effects/mode/input) y el mapeo error-code→excepción de `AudioNativeBridge` (**3.352 LOC**) se extraen a commonMain para reutilizarlos en `IosAudioBridge` sin duplicar | AudioNativeBridge delega en la clase común; tests Android verdes | P1 | M | ✅ **HECHO** 2026-07-25 — `internal/bridge/BridgeConcurrency.kt` + 8 tests en commonTest. Los 26 call sites migrados. Ver nota |
| WA-1.5 | Tests Kotlin de commonMain | ~~hoy: cero tests Kotlin~~ → **hoy hay 5 suites / 34 tests** (`ChordGenerator`, `ScaleQuantizerFlow`, `EffectManagerBatch`, + `Format` y `Time` de WA-0.2). **Falta lo central:** `StateSynchronizer`, `AudioEngineImpl`, mapeo de errores | Suite commonTest corriendo en JVM en CI | P1 | M | Parcial |
| WA-1.6 | Factorizar denormals ARM64 | El código FPCR de `PlatformAndroid.cpp` para arm64 es idéntico al que necesitará Apple Silicon → extraer a `PlatformArm64.inc` compartido | PlatformAndroid compila igual; código listo para PlatformApple | P2 | S | ✅ **HECHO** 2026-07-25 — `platform/PlatformIsa.inc`. **La duplicación era más ancha que el FPCR:** el bloque x86_64 (MXCSR) y **las dos funciones de SIMD caps** también eran byte-for-byte idénticas. De ahí el nombre más amplio que el `PlatformArm64.inc` del ticket. En los `.cpp` queda sólo `setAudioThreadPriority()`, que es lo único que difiere de verdad |

### Nota de cierre — WA-1.2 (2026-07-25)

**Cierra también WA-3.3, y con eso la Fase 3 entera.**

- `commonMain/domain/device/DeviceCapabilities.kt` — interfaz de **hechos**
  (`platform`, `apiLevel`, `totalRamMb`, `cpuCoreCount`, `supportsLowLatencyAudio`,
  `isLowEndDevice`) + `DeviceCapabilitiesSnapshot` para construir una a mano.
- `commonMain/api/DeviceCapabilitiesProvider.kt` — `expect fun currentDeviceCapabilities()`,
  cacheado por proceso.
- Actual Android: `/proc/meminfo` + topología de CPU + `Build.VERSION.SDK_INT`.
  Actual iOS: `NSProcessInfo` (**no** `UIDevice`: da todo lo que hace falta sin arrastrar
  UIKit a una librería de audio ni exigir main thread).
- `AudioEngineConfig.tunedFor(caps, base)` — la **política**, separada de la detección.
- `AudioEngineFactory.create()` la consume: sin argumentos, el default sale ajustado al
  dispositivo.

**El `object DeviceCapabilities` de androidMain quedó intacto**, como pedía el ticket. La
sobrecarga `deviceCapabilities(context)` de androidMain delega en él para el criterio de
gama baja, así que no hay dos definiciones de "gama baja" en el mismo módulo. El alias de
import (`as AndroidDeviceCapabilities`) está porque el object legacy y la interfaz nueva se
llaman igual.

**Hechos y política separados a propósito.** La interfaz no tiene "cuántos efectos usar":
el umbral de gama baja es una heurística por plataforma que va a cambiar, y no debería
arrastrar consigo la detección. Por eso el recorte vive en `tunedFor()`, donde el consumidor
lo puede ignorar.

**El recorte es un techo, no un valor fijo.** `maxEffects` baja a 6 en gama baja, pero
`minOf(base.maxEffects, 6)`: quien pidió 3 sigue teniendo 3, no 6.

> [!WARNING]
> **Cambio de comportamiento en Android.** `AudioEngineFactory.create()` **sin argumentos**
> ahora devuelve `maxEffects = 6` en un dispositivo de gama baja, donde antes devolvía 12
> siempre. La política existía desde antes en el `object DeviceCapabilities` de androidMain,
> pero **nunca había estado conectada al factory** — WA-1.2 la activa. Con una config
> explícita no se toca nada: lo que se pasa es lo que se usa. Conviene mirarlo en el smoke
> manual de NoisyPad Android junto con lo de WA-1.4.

**Dos gotchas de plataforma que se resolvieron en el camino:**

- **`Runtime.availableProcessors()` cuenta CPUs *online*, no las que el dispositivo tiene.**
  Mapea a `_SC_NPROCESSORS_ONLN`, así que un governor que apagó cores por temperatura hace
  que un octa-core reporte 4 — y como la foto se cachea por proceso, esa lectura transitoria
  quedaría congelada y marcaría el dispositivo como gama baja **para siempre**. El actual
  lee `/sys/devices/system/cpu/possible` (topología de boot) y usa el conteo online sólo
  como piso.
- **El umbral de iOS no es el de Android, y no por descuido.** 3 GB en iOS vs 2 GB en
  Android: el piso de iOS es más alto (deployment target 15.0 → el dispositivo más flojo es
  un 6s con 2 GB), y los núcleos de Apple no son comparables uno a uno con los de un
  big.LITTLE barato. En iOS el que discrimina es la RAM; el umbral de núcleos (≤2) sólo
  atrapa a los A9/A10.

**Verificado local:** 50/50 en JVM (eran 42), 87/87 en el simulador (eran 75) — 8 tests
comunes de política + 4 de `iosTest` sobre el actual iOS. `assembleDebug` y
`assembleRelease` verdes, ambos slices de iOS compilan.

**Alcance de esa verificación:** en el simulador `NSProcessInfo` reporta la RAM y los
núcleos del **Mac anfitrión**, no los de un iPhone, así que el path de gama baja de iOS
está cubierto sólo con `DeviceCapabilitiesSnapshot` armado a mano. El parseo de
`/proc/meminfo` y de `/sys/devices/system/cpu/possible` **no corre en el host de tests**
(macOS no los tiene): ambos devuelven un valor de "no sé" y el gate real fue revisión.
Se confirma en el smoke de device.

---

### Nota de cierre — WA-1.4 (2026-07-25)

`commonMain/internal/bridge/BridgeConcurrency.kt` + 8 tests en `commonTest` (corren en JVM
**y** en el simulador, así que la primitiva queda verificada en las dos plataformas que la
van a usar). Los **26 call sites** de `AudioNativeBridge` migrados.

**El mapeo error-code→excepción que pedía el ticket ya estaba compartido:**
`NativeBridgeException.fromCode()` vive en `commonMain/domain/error/` desde antes. Esa
mitad del ticket no existía como trabajo. Lo que sí estaba duplicado —y ahora no— es el
*envelope*: `withContext(Dispatchers.Default)` + mutex de categoría + `try/catch` → `Result`.

> [!WARNING]
> **El refactor destapó un bug real de producción en Android.** Los **22 bloques
> `catch (e: Exception)`** de `AudioNativeBridge` **se tragaban `CancellationException`**.
> En Kotlin, `CancellationException` **es** una `Exception`: cada uno de esos bloques
> convertía una cancelación en `Result.failure`, así que el scope padre nunca se enteraba
> de que había sido cancelado y la concurrencia estructurada dejaba de funcionar. No había
> una sola mención de `CancellationException` en las 3.352 líneas.
>
> `BridgeConcurrency.guarded()` la re-lanza explícitamente antes de cualquier otro manejo.
> El test `cancellationPropagatesInsteadOfBecomingAFailure` lo fija. Sin WA-1.4 este bug se
> habría replicado tal cual a `IosAudioBridge`.

**Dos primitivas, no una.** De los 26 sites, 4 son **lecturas** que devuelven valores crudos
(`Int`, `Boolean`, `EffectType?`, un `Map`), no `Result<T>`. Meterlas en `guarded()`
obligaría a inventarles un `Result` que nadie pidió y a elegir un valor "de fallo" que no
existe. Para ellas está `serialized()`, que sólo hace dispatcher + mutex y **deja propagar**
las excepciones — que es lo correcto para una lectura y lo que ya hacían.

**`LogcatAudioLogger`** (androidMain) existe para que centralizar el manejo de errores no se
lleve puesta la diagnosticabilidad: el default de `BridgeConcurrency` es `NoOpAudioLogger`
—una librería no decide por el consumidor a dónde van los logs— pero en Android esos errores
venían saliendo por logcat y ahí se los busca. Avanza WA-1.1 de paso.

**Verificado:** 42/42 en JVM, 49/49 en el simulador, `assembleDebug` y `assembleRelease`
verdes. Ojo con el alcance de esa verificación: **los métodos de `AudioNativeBridge` no
tienen tests** (necesitan JNI y device), así que para ellos el gate real fue el compilador
más la revisión del diff. La migración es mecánicamente uniforme, pero conviene un smoke
manual en NoisyPad Android antes de publicar.

---

## 7. Requerimientos — Fase 2: C++ multiplataforma

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WA-2.1 | Build CMake iOS | Toolchain/presets iOS (device arm64 + simulator arm64): compilar `watermelon-dsp/effects/engines/voice/looper` ~~+ core + nodes + api~~ como **librería estática** por slice. Excluir del build iOS: `jni/`, `usb/`, `OboeBackend`, `LibusbBackend`, `PlatformAndroid.cpp`. Definir `USE_NEON=1` en arm64 Apple `libwatermelon_audio.a` (motor completo) para ambos slices compila con Xcode clang, C++20, y **linkea** | P0 | L | ✅ **HECHO** 2026-07-25 — la sonda se promovió al target shipped; ver notas de cierre |
| **WA-2.0** | **Desacoplar el core de Oboe** | `core/AudioEngine.cpp` (30 usos de `oboe::`, con `OboeCallbackAdapter` propio y apertura directa de streams) y `nodes/InputNode` (**hereda** `oboe::AudioStreamDataCallback`, 32 usos) deben pasar **exclusivamente** por `IAudioBackend`. Incluye `BackendManager` y `api/watermelon_audio.cpp`, que instancian los backends Android directo | `core/`, `nodes/` y `api/` compilan para iOS; Android sin regresiones (suite C++ + smoke) | **P0** | **L** | ✅ **HECHO** 2026-07-22 — `core/`, `nodes/`, `api/` y `backends/` compilan para iOS |
| WA-2.2 | `PlatformApple.cpp` | Implementar `wma::platform`: denormals (FPCR, reutiliza WA-1.6), `setAudioThreadPriority()` (pthread `THREAD_TIME_CONSTRAINT_POLICY` — solo como refuerzo: el thread de Core Audio ya viene priorizado), SIMD caps (NEON fijo en arm64) | `engine_tests` linkea y pasa en macOS con PlatformApple | P0 | M | ✅ **HECHO** 2026-07-23 — `platform/PlatformApple.cpp`. FPCR arm64 idéntico a Android (WA-1.6 aún sin factorizar a `.inc`) |
| WA-2.3 | Logger Apple | Sink `os_log` por defecto en builds Apple; callback configurable idéntico a Android | Log visible en Console.app desde sample app | P1 | S | ✅ **HECHO** 2026-07-25 — `platform/Logger.cpp`, subsistema `com.watermellonstudios.audio`, categoría `engine`. Ver nota |
| WA-2.4 | **`CoreAudioBackend`** | Implementar `IAudioBackend` para iOS (D2): AVAudioEngine + AVAudioSourceNode (output) y AVAudioSinkNode/inputNode (input full-duplex para guitar/input FX). Reglas: el render block invoca directo el mix C++ (sin ObjC dispatch, sin allocs, sin locks); negociación de sample rate/buffer contra el hardware; manejo de formato (Float32 nativo de Core Audio vs pipeline interno) | Sine + cadena de efectos + looper suenan en device real; callback verificado sin allocs (Instruments) | P0 | L | 🟡 **OUTPUT ✅ + INPUT ✅ (código)** — output 2026-07-23, captura full-duplex 2026-07-25 (etapas 1 y 2): `AVAudioSinkNode` sobre el **mismo** `AVAudioEngine`, ring SPSC hacia el render block, `supportsFullDuplex()` → true, y el armado resuelto en `BackendManager` con dos solicitantes ORed. **Falta sólo** la validación en device (sonido + Instruments) que es WA-4.3 |
| WA-2.5 | **Completar la C API** | Cerrar el gap de WA-0.1: agregar a `watermelon_audio.h` las funciones faltantes (excluyendo USB). **Dimensionado por WA-0.1: 110 portables, de las cuales ~79 netas tras descartar near-matches — y 39 son del looper.** Ver `docs/kmp/c_api_coverage.md` para el detalle por categoría. Reglas de ABI: handles opacos, códigos de error enteros (sin excepciones cruzando la frontera), sin tipos C++ en firmas, documentación de thread-safety por función (RT-safe vs coordinación) | Cobertura 1:1 con el JNI no-USB según tabla WA-0.1 | P0 | L |
| WA-2.6 | **JNI → wrapper de la C API** (alto valor) | Refactor incremental por categorías (lifecycle → effects → looper → mode → análisis): cada `Java_..._nativeXxx` pasa a llamar `wma_xxx` en vez del engine directo. El JNI queda como capa de traducción de tipos JNI↔C de ~1 línea por función | Paridad Android/iOS por construcción; tests C++ y smoke Android verdes tras cada categoría | P1 | L |
| WA-2.7 | Selección de backend por plataforma | `BackendManager` compila con Oboe+Libusb en Android y CoreAudio en iOS (compile-time, `#if` mínimos en un solo archivo de registro de backends) | Sin `#ifdef` dispersos; un único punto de registro | P1 | S | ✅ **HECHO** 2026-07-22 — `backends/PlatformBackends.{h,cpp}`; **una sola guarda en todo `backends/`**, cero en `BackendManager` |

### Nota de cierre — WA-2.1 parcial (2026-07-22)

**Entregado y verde:**

- `audio/src/main/cpp/ios/CMakeLists.txt` — build iOS **separado** del CMakeLists que
  maneja AGP. Deliberado: el de Android es Android-specific de punta a punta (Oboe,
  libusb, JNI, `PlatformAndroid`, y flags de linker GNU como `-Wl,-z,max-page-size` y
  `--gc-sections` que Apple ld rechaza). Separarlo deja el build que shippea en riesgo cero.
- `scripts/build-ios.sh` — ambos slices + merge con `libtool` en un `.a` por slice.
- Paso en el job `ios` del CI.

Resultado: **`libwatermelon-audio-ios.a`, arm64, 7.9 MB, 6.435 símbolos**, para
`iphoneos` e `iphonesimulator`.

**Un solo fix de portabilidad hizo falta** en las 5 sub-librerías: `mmap64`/`off64_t` no
existen en Darwin. El comentario del código justificaba bien la variante de 64 bits (en
las ABIs de 32 bits de Android `off_t` truncaría un offset grande), así que se preservó
la intención con un alias condicional (`WMA_MMAP`/`WmaMapOffset`) en vez de degradar a
`mmap` en todas las plataformas. **Confirma la métrica 1**: el DSP es portable de verdad.

**Lo que NO entró, y por qué — el hallazgo que abre WA-2.0:**

El criterio original pedía `libwatermelon_audio.a`, o sea core + nodes + api. No es
alcanzable: el requerimiento asumía que **`IAudioBackend` ya abstraía todo el I/O**, y no
es así. `AudioEngine` tiene un camino directo a Oboe *además* del backend.

| Archivo | Acoplamiento |
|---|---|
| `core/AudioEngine.cpp` | 30 usos de `oboe::`; `OboeCallbackAdapter` propio; abre streams directo |
| `nodes/InputNode.h/.cpp` | **hereda** de `oboe::AudioStreamDataCallback` — 32 usos |
| `backends/BackendManager.cpp` | instancia `OboeBackend`/`LibusbBackend` directo |
| `api/watermelon_audio.cpp` | idem |

Por eso el §2 ("el C++ está modularizado sin dependencias Android") vale para las
sub-librerías pero **no para el core**. Se levanta **WA-2.0** como prerequisito real de
WA-2.4: sin él no hay dónde enchufar `CoreAudioBackend`.

### Nota de cierre — WA-2.1 completo (2026-07-25)

La sonda `WMA_IOS_PROBE_CORE` **se promovió al target shipped**. WA-2.2 y WA-2.4 habían
cerrado los dos gaps que la mantenían aparte, así que la opción ya no separaba nada: era
deuda de andamio. `ios/CMakeLists.txt` ahora define `watermelon-core` (core, nodes, api,
backends, platform, analysis) linkeando las cinco sub-librerías, y `build-ios.sh` produce
el `libwatermelon_audio.a` que el criterio original pedía.

**Resultado: 18 MB, arm64, 14.771 símbolos**, para `iphoneos` e `iphonesimulator`.

**Cambio de método en la verificación.** Hasta ahora la linkeabilidad se comprobaba con
`nm -u` (undefined menos definido). Se reemplazó por un **link check real**: `build-ios.sh`
compila un `main()` trivial y lo linkea contra el archivo con `-Wl,-force_load`, que
arrastra **todos** los miembros del `.a` y obliga a resolver cada símbolo que cualquiera de
ellos referencie. Es la diferencia entre una heurística —`nm -u` no puede distinguir un
símbolo que aporta el sistema de uno que falta— y la verdad del linker, que es exactamente
lo que va a hacer cinterop en WA-3.1. De paso valida la lista de frameworks.

> [!NOTE]
> **Sigue faltando la captura de audio en iOS, y no es un gap de link.**
> `InputNode::createInputStream()`/`startInputStream()` devuelven `false` en cualquier
> plataforma sin Oboe: el nodo existe y procesa, pero nunca tiene stream de entrada. El
> archivo linkea igual. Full-duplex / guitar FX en iOS necesita un adapter de captura
> CoreAudio en la costura `WMA_HAS_OBOE`.

### Nota de cierre — Input path de iOS, etapa 1 (2026-07-25)

**Decisión de arquitectura (no re-litigar): la captura NO va en un adapter propio de
`InputNode`, va full-duplex en `CoreAudioBackend`.**

Se evaluaron tres caminos. El que decía el plan original —un `InputCoreAudioAdapter.mm`
con su propio `AVAudioEngine` en la costura `!WMA_HAS_OBOE`— se descartó al descubrir que
**el lado consumidor ya existe y ya es agnóstico de plataforma**:
`AudioEngine::onAudioReady(output, inputData, frames)` rutea `inputData` a INPUT_FX directo
(guitar FX) o a `InputNode::feedExternalInput()` (vocoder / MIX). Los comentarios dicen
"USB" pero el mecanismo no tiene nada de USB — es "el backend entregó input". Sólo faltaba
que `CoreAudioBackend` pasara ese puntero.

Un segundo `AVAudioEngine` habría costado un buffer extra de latencia y CPU para reimplementar
un camino que ya estaba escrito y probado.

**Qué se hizo:**

- `AVAudioSinkNode` colgado de `engine.inputNode`, en el **mismo** `AVAudioEngine` que el
  output: un solo dominio de reloj, así que el ring no tiene que absorber drift (sólo el
  desfasaje de un buffer entre los dos callbacks) y no hace falta `DriftResampler`.
- Ring SPSC lock-free (`LockFreeRingBuffer`) del thread de captura al de render. Un scratch
  pre-alocado **por thread** — los dos bloques corren en threads distintos de CoreAudio y no
  pueden compartir memoria.
- Normalización a estéreo intercalado en el bloque de captura, cubriendo las tres formas en
  que el OS puede entregar el ABL (planar mono —el mic del iPhone—, planar estéreo,
  intercalado). `supportsFullDuplex()` → true, `getInputLatencyMs()` real.

**Tres decisiones de robustez que valen la pena:**

- **El `@try` alrededor del setup de captura no es relleno.** `engine.inputNode` y `connect:`
  reportan "no hay entrada usable" **lanzando**, no devolviendo error. Sin eso, un usuario que
  negó el micrófono se llevaba el proceso entero en vez de perder la captura.
- **La captura nunca es fatal para el output.** Sin permiso de micrófono, o en un simulador
  sin dispositivo de entrada, el formato vuelve en cero: se loguea y se sigue output-only.
  Por eso `isCaptureActive()` existe aparte de `supportsFullDuplex()` — la capacidad y el
  hecho son cosas distintas.
- **`playAndRecord` sólo si se pidió captura.** Pedirla siempre significaría prompt de
  micrófono para toda app y ruteo al receiver en vez del parlante. Ojo: hay solapamiento con
  `AudioSessionManager` (WA-3.4, Kotlin), que también configura la sesión — las opciones se
  espejan a propósito, y el backend nunca **degrada** una sesión de grabación a `playback`.

**Verificado:** ambos slices compilan y linkean (link check con `-force_load`), 517/517 tests
C++ de host, guardrail WA-0.4 verde, `assembleDebug` de Android sin regresiones.
**Nada de esto prueba que entre audio**: el bloque de captura no lo ejercita ninguna suite
—necesita device— así que el gate fue el compilador, el link check y revisión. Sonido real e
Instruments sobre el render block van en WA-4.3.

---

### Nota de cierre — Input path de iOS, etapa 2 (2026-07-25)

Cierra el hueco que dejó la etapa 1: el armado de la captura.

**El hallazgo que definió el diseño: el gap no era de iOS.** `OboeBackend` **también** lee su
flag de full-duplex recién en `start()` (`OboeBackend.cpp:63`). O sea que
`setFullDuplexEnabled()` en caliente nunca abrió un stream de captura en **ninguna**
plataforma — en Android no se nota porque el micrófono entra por el stream Oboe propio de
`InputNode`. Por eso un restart genérico en `BackendManager` habría **regresionado Android**:
un corte de audio en cada cambio a INPUT_FX donde hoy no hay ninguno.

**Dos solicitantes, ORed, en `BackendManager`.** `requestCapture(who, want, allowRestart)` con
`CaptureRequester::{MODE, INPUT_NODE}`. Antes había **un solo bool** para dos solicitantes
independientes: salir de INPUT_FX mataba una captura que la app había arrancado a propósito.
Es la misma forma de bug que ya mordió dos veces acá (`InputNode` duplicado, estado de modo
duplicado), así que esta vez está fijada con tests en lugar de confiada.

**El permiso de reabrir es asimétrico, y es la decisión central.** Un cambio de modo **nunca**
reabre el stream (no puede meter un corte en la reproducción); un `wma_input_start()` explícito
**sí** (el que llamó pidió el micrófono, y un hueco breve es el precio). Eso deja el path del
modo de Android byte-idéntico.

**El fallback en la C API no tiene un solo `#if`** — el fallthrough *es* el test de plataforma:

```
wma_input_start()
  1. inputNode->startInputStream()   → Oboe en Android: true, y corta acá
  2. backendManager->requestCapture(INPUT_NODE, true, allowRestart=true)
                                     → Apple: el backend carga el input por onAudioReady
```

Si el reopen con captura falla, se **suelta la solicitud y se reabre sin ella**: quedarse sin
audio es peor que quedarse sin micrófono.

> [!NOTE]
> **Verificado con tests de verdad, no con revisión.** 10 tests nuevos en
> `core/tests/test_capture_requests.cpp` (527 total, eran 517). El `FakeAudioBackend` se
> corrigió para modelar el backend real —la captura se decide al **abrir**, no cuando se
> pide— porque con el fake anterior, que honraba la solicitud en el acto, el caso que
> necesita reopen no ocurría nunca y la suite no habría probado nada.
>
> **Y se verificó que los tests muerden**, con dos mutaciones deliberadas:
> volver al bool único → **3 rojos** (los que fijan el OR); dejar que el modo reinicie →
> **1 rojo** (`AModeChangeNeverReopensARunningStream`). Restaurado, 527/527.

**Lo que estos tests NO cubren:** que entre audio de verdad. `CoreAudioBackend` no se compila
en la suite de host —el fake ocupa su lugar—, así que lo verificado es la **lógica de
solicitudes**, no la captura. Sonido real sigue siendo WA-4.3.

---

### Nota de cierre — WA-2.3 (2026-07-25)

`Logger.cpp` gana una rama `os_log` (subsistema `com.watermellonstudios.audio`, categoría
`engine`), simétrica con la de logcat. Se ve en Console.app y con
`log stream --predicate 'subsystem == "com.watermellonstudios.audio"'`.

Dos decisiones que conviene tener presentes:

1. **Está gateada por `TARGET_OS_IPHONE`, no por `__APPLE__` a secas.** Un build Apple que
   no sea iOS es, en este repo, la suite googletest de host — y `ctest --output-on-failure`
   muestra *stderr*, no el unified log. Mandar esas corridas a `os_log` haría el job macOS
   **más difícil** de depurar, no más fácil. Una app macOS, si algún día existe, puede
   instalar su propio callback.
2. **Ambos strings van con `%{public}s`.** `os_log` redacta los strings dinámicos como
   `<private>` por defecto; sin eso, cada línea del motor saldría inútil.

Sigue valiendo la regla de siempre: el logger **no es RT-safe** y no va en el hot path.

---

## 8. Requerimientos — Fase 3: Kotlin iosMain

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WA-3.1 | cinterop | `watermelon_audio.def` sobre `watermelon_audio.h`; link estático de los `.a` por target; verificación de que los 191 símbolos resuelven | Kotlin/Native llama la C API desde un test de simulador | P0 | M | ✅ **HECHO** 2026-07-25 — ver nota de cierre |
| WA-3.2 | `IosAudioBridge` | Implementación de `IAudioNativeBridge` en iosMain sobre cinterop: mismos contratos `Result<T>`, mismos mutexes por categoría (reutiliza `BridgeConcurrency` de WA-1.4), mapeo error-code→excepción idéntico. Los métodos RT (`setXY`, `setFrequencyAndAmplitude`) llaman la función C directa sin suspend ni locks | Suite commonTest de bridge (WA-1.5) pasa contra el bridge iOS en simulador | P0 | L | ✅ **HECHO** 2026-07-25 — 87 de 88 miembros sobre la C API; 13 tests en `iosTest`. Ver nota |
| WA-3.3 | actuals iOS | `NativeLibraryLoader` (no-op, link estático), `AudioBridgeProvider` (retorna `IosAudioBridge`), `DeviceCapabilities` (ProcessInfo/UIDevice) | `AudioEngineFactory.create()` funciona en iOS | P0 | S | ✅ **HECHO** 2026-07-25 — `NativeLibraryLoader` ✅, `AudioBridgeProvider` ✅, `DeviceCapabilities` ✅ (WA-1.2). **Cierra la Fase 3** |
| WA-3.4 | `AudioSessionManager` | Helper iosMain para AVAudioSession: categoría `playAndRecord`, `preferredIOBufferDuration`/`preferredSampleRate`, notificaciones de interrupción y route change expuestas como Flow para que el consumidor (NoisyPad) las mapee a start/stop | Interrupción por llamada entrante pausa y reanuda el engine en sample app | P0 | M | ✅ **HECHO** 2026-07-25 — `internal/audio/AudioSessionManager.kt` + 13 tests. La validación con llamada entrante real queda para WA-4.3 (device) |
| WA-3.5 | Transcoder abstracto | `Mp4AacTranscoder` (MediaCodec) → interfaz `IAudioTranscoder` en commonMain; actual Android existente; actual iOS con `AVAssetWriter` (diferible: el export WAV no lo necesita) | Interfaz común; iOS actual puede llegar después | P2 | M |
| WA-3.6 | Regla RT documentada | Documentar y hacer cumplir D6: ningún callback del thread RT entra a Kotlin; estado via polling/colas. Incluir en el README de contribución | Doc + revisión de que ningún path actual lo viola | P1 | S | Parcial — la regla ya está en `CLAUDE.md` §portabilidad; falta la revisión de paths |

### Nota de cierre — WA-3.1 (2026-07-25)

**Kotlin/Native llama al motor C++ en el simulador.** `iosSimulatorArm64Test`: **41 tests,
0 fallas**, de los cuales 7 son el nuevo `CinteropSmokeTest` (WA-T.3).

Piezas:

- `audio/src/nativeInterop/cinterop/watermelon_audio.def` — `staticLibraries` embebe
  `libwatermelon_audio.a` **dentro del klib**, que es lo que pedía D5: NoisyPad consume una
  sola coordenada KMP y no hay CocoaPods ni SPM que mantener al lado. `headerFilter` evita
  que se generen bindings para stdint/stdbool/stddef.
- `KmpNativeConventionPlugin` — bloque `cinterops` por target y la task `buildIosNativeLib`,
  que invoca `scripts/build-ios.sh` con inputs/outputs declarados (sin eso el `.a` se
  recompilaría en cada build). `-libraryPath` se pasa **desde Gradle y no desde el `.def`**
  porque es lo único que cambia entre slices, y un `.def` no puede ramificar.
- Artefactos publicados: `audio-ios{arm64,simulatorarm64}-<v>-cinterop-watermelonAudio.klib`,
  4,7 MB cada uno — el archivo embebido, verificado con `publishToMavenLocal`.

**El smoke prueba marshalling, no DSP.** El comportamiento del motor ya lo cubren los 517
tests C++; lo que no cubría nada es que los símbolos resuelvan desde Kotlin y que cada
familia de tipos cruce intacta. Por eso los casos están elegidos por categoría —puntero
opaco, `const char*`, `int`, `float`, `bool`— y no por feature. **No arranca el motor a
propósito:** `wma_engine_start()` abre un stream de CoreAudio y volvería el test flaky por
una razón ajena al cinterop. El sonido real es WA-4.3, en device.

> [!NOTE]
> **Un falso positivo que quedó convertido en test.** La primera versión del round-trip de
> float pedía `set_param(cutoff, 0.25)` y recibía `20.0`. Parecía un bug de marshalling y
> era el motor haciendo lo correcto: `FilterEffect.cpp:21` clampea el cutoff a
> [20, 20000] Hz, y el param 0 de FILTER son **Hz, no un valor normalizado**. El float
> había cruzado perfecto. Quedó como `outOfRangeParameterIsClampedByTheEngineNotSilentlyAccepted`,
> que ahora documenta que la C API aplica el dominio del motor y no es un passthrough de
> bytes.

**El publish se movió a `macos-latest`** (`release-please.yml`), que era la consecuencia
anticipada: cinterop necesita el SDK de iOS para parsear el header y el `.a` que sólo
produce Xcode. Se descartó la matriz host-specific porque parte la publicación en dos jobs
que pueden divergir de versión — el modo de falla que ya mordió con 1.8.0.

**Prerrequisito de entorno resuelto:** el bloqueo de §11 (`iosSimulatorArm64Test` se colgaba
esperando privilegios de admin) **ya no existe** — `xcodebuild -checkFirstLaunchStatus`
devuelve 0 y hay simuladores disponibles. Los tests K/N corren local.


### Nota de cierre — WA-3.2 / WA-3.3 parcial (2026-07-25)

**`getAudioBridge()` ya no lanza `NotImplementedError`.** `iosMain` tiene
`IosAudioBridge`, un `IAudioNativeBridge` completo sobre los bindings de WA-3.1, y
13 tests propios en `iosTest` corriendo en el simulador (**62 tests iOS en total,
0 fallas**).

**El gap de la C API no bloqueaba nada, y eso no era obvio.** El riesgo razonable
era que las ~110 funciones faltantes de WA-0.1 impidieran implementar el bridge. Se
mapearon los **88 miembros** de `IAudioNativeBridge` (+ `IEffectStateProvider` /
`IEffectStateWriter`) contra las 191 `wma_*`: **87 tienen contraparte**. La interfaz
excluye deliberadamente looper, USB, arpegiador, SoundFont y benchmark — que es
exactamente donde vive el gap (39 del looper, 14 de input/monitor, 9 de metrónomo).

**El único hueco real: `createSplitBackend`.** No hay una sola mención de "split" en
`watermelon_audio.h`. `SplitBackend` compone un backend de entrada con otro de
salida — en Android, USB-in + Oboe-out. En iOS no existe ninguna de las dos mitades
(no hay USB por D4, y `InputNode` todavía no tiene adapter de captura CoreAudio), así
que devuelve `false`. Junto con `isUsbBackendAvailable()` (`false`) y
`setUsbLatencyProfile()` (`Result.failure`), la regla es la misma: **fallar
explícito antes que fingir**. Un consumidor que cree tener un split y no lo tiene es
peor que uno que sabe que no pudo.

**Dos decisiones de diseño que conviene tener a mano:**

1. **`setXY(coalesce)` ignora el flag en iOS.** El coalescer de Android existe para
   amortizar el costo de cruzar JNI en cada frame de gesto; una llamada de cinterop
   no tiene ese costo, así que bufferear sólo agregaría latencia de control. Si
   WA-4.3 mide lo contrario, el lugar del coalescer es ese método.
2. **`setMultipleEffectParameters` itera, pero bajo un solo lock.** La C API no tiene
   una operación que abarque varios efectos; lo que le da semántica de lote es que la
   cadena no cambia entre updates, no que sea una sola llamada C.

**Detalle de cinterop que sorprendió:** `WmaResult` **no** se mapea a un `enum class`
sino a `typealias WmaResult = Int`, y el handle opaco vive en
`cnames.structs.WmaEngine`, no en el paquete del `.def`. Se descubrió leyendo el klib
con `klib dump-metadata`, que es la forma confiable de saber qué generó cinterop en
vez de suponerlo.


### Nota de cierre — WA-3.4 (2026-07-25)

`AudioSessionManager` en iosMain: `configure()` / `activate()` / `deactivate()`, los
valores **concedidos** (`actualSampleRate`, `actualIOBufferDuration`, latencias) y un
`Flow<AudioSessionEvent>` con interrupciones y cambios de ruta. 13 tests en el
simulador — **75 tests iOS en total, 0 fallas**.

**No actúa sobre el motor, sólo informa.** Quién decide pausar o reanudar es NoisyPad,
que es el único que sabe si había un loop grabando o si conviene avisar en pantalla. Un
manager que pausa por su cuenta le saca esa decisión al consumidor.

**Lo que pedís no es lo que obtenés.** `preferredSampleRate` y
`preferredIOBufferDuration` son preferencias; iOS puede ignorarlas según hardware, ruta
y qué estén haciendo otras apps. Por eso hay que leer los `actual*` **después** de
activar: son los valores con los que el motor va a trabajar de verdad, y son los que
van a `prepare()`.

> [!WARNING]
> **Dos bugs propios que encontró el proceso, no el diseño.**
>
> 1. **`runCatching` sobre `AVAudioSession` daba éxito siempre.** Los setters de
>    `AVAudioSession` **no lanzan**: devuelven `false` y llenan un `NSError`.
>    Envolverlos en `runCatching` reportaba éxito incluso cuando el sistema rechazaba
>    la configuración — justo el caso que interesa detectar. Reemplazado por un helper
>    que chequea el `Boolean` y conserva el `localizedDescription`.
> 2. **El `Flow` podía perder eventos en silencio.** `trySend` desde un callback de
>    `NSNotificationCenter` no puede suspender, así que sin capacidad explícita un
>    evento se descarta sin rastro. Ahora hay `.buffer(16, DROP_OLDEST)`. El comentario
>    ya decía `DROP_OLDEST` antes de que el código lo hiciera — el doc iba adelante del
>    código, que es su propia clase de bug.

**Los tests están partidos en dos a propósito.** El parseo del `userInfo` (tipos y
opciones son enteros mágicos del ABI de iOS) se prueba llamando a los parsers directo:
determinista y rápido. El cableado del `Flow` tiene **un solo** test, que reintenta con
timeout. La primera versión probaba todo por el `Flow` bajo `runTest` y los cinco tests
colgaban hasta el timeout —5 minutos— porque el tiempo virtual de `runTest` no se lleva
con la entrega **síncrona** de `NSNotificationCenter`: la notificación se posteaba antes
de que el observer estuviera registrado y se perdía.

**Falta para cerrar el criterio original:** la interrupción por llamada entrante real
pausando y reanudando el motor. Eso necesita hardware y va con WA-4.3.

---

## 9. Requerimientos — Fase 4: Empaquetado y publicación

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WA-4.1 | XCFramework en el pipeline | Task Gradle que ensambla el XCFramework (device+simulator) y lo integra al klib/publicación; cache para no recompilar C++ sin cambios | `./gradlew :audio:assembleWatermelonXCFramework` reproducible en CI | P0 | M | ✅ **HECHO** 2026-07-25 — `XCFramework("Watermelon")` en el convention plugin, framework estático, wired al job `ios` de CI con verificación de símbolos. Ver nota |
| WA-4.2 | Publicación KMP | Publicar a GitHub Packages el artefacto KMP completo (metadata común + AAR Android + klibs iOS). Release Please sigue gobernando la versión. Validar consumo desde un proyecto de prueba iOS y desde NoisyPad Android (sin cambios para el consumidor Android actual) | NoisyPad resuelve la misma coordenada para ambos targets — **desbloquea gate G1 de NoisyPad** | P0 | M |
| WA-4.3 | Sample app iOS | Mini app (puede vivir en el repo) que haga smoke de la librería: start engine, sine, un efecto, looper record/play, medición de latencia round-trip | Smoke manual documentado; latencia medida y registrada | P1 | M | 🟡 **Partido en dos, primera mitad APROBADA 2026-07-25** (sin empezar). Ver decisión abajo |

### Decisión — WA-4.3 se parte en dos (aprobada 2026-07-25, sin empezar)

**Primera mitad: sample app en el simulador. No necesita hardware. Aprobada, pendiente.**
**Segunda mitad: validación en device. Sigue bloqueada por el iPhone.**

**Qué NO cierra el simulador**, y por qué no es cuestión de intentarlo igual:

| Criterio de WA-4.3 | En simulador |
|---|---|
| Instruments sobre el render block (cero allocs, cero locks) | **No sirve** — corre el CoreAudio de macOS sobre el hardware del Mac, no el I/O unit del iPhone. El comportamiento RT no es representativo |
| Latencia round-trip medida | **No sirve** — mediría la placa de audio del Mac |
| Interrupción por llamada entrante (cierra el criterio de WA-3.4) | **Imposible** — no hay llamadas |

**Qué sí, y por qué vale la pena igual:** el simulador usa el **micrófono del Mac**, así que
es lo único disponible hoy que ejercita **el input path que se escribió a ciegas**. Las
etapas 1 y 2 se cerraron declarando que "nada de esto prueba que entre audio"; esto lo
prueba. En concreto: el `AVAudioSinkNode`, la normalización del ABL, el ring SPSC, el
camino del `@try` cuando se niega el permiso, y el reopen de `wma_input_start()`.

De paso responde la pregunta que WA-4.1 dejó abierta: si el XCFramework es **realmente
consumible** desde Swift, no sólo si se construye.

**Forma acordada:**

- Proyecto Xcode mínimo en `samples/ios/`, SwiftUI.
- Consume el **XCFramework** (`import Watermelon`), **no** la C API directa: así ejercita el
  stack completo tal como lo va a hacer NoisyPad —Swift → Kotlin → cinterop → C API → C++—
  que es justo donde está el riesgo no verificado. Una app Swift pelada contra
  `libwatermelon_audio.a` sería menos código y no probaría ni la capa Kotlin ni el
  XCFramework.
- Controles: start/stop, sine, agregar un efecto, looper record/play, toggle de input con
  medidor de nivel.
- `NSMicrophoneUsageDescription` en el `Info.plist`. **Negar el permiso a propósito es un
  caso a probar**, no un accidente: es el único disparador del `@try` de `CoreAudioBackend`.

### Nota de cierre — WA-4.1 (2026-07-25)

`XCFramework("Watermelon")` en el convention plugin, con `binaries.framework` por slice.
`./gradlew :audio:assembleWatermelonXCFramework` — el nombre exacto del criterio.

**El nombre no es libre.** El del XCFramework y el `baseName` del framework interno tienen
que coincidir: KGP no soporta renombrar y avisa que el resultado puede no ser consumible. El
criterio fija la task en `assembleWatermelonXCFramework`, así que el módulo Swift es
`import Watermelon`, no `WatermelonAudio`.

**Estático a propósito.** El motor C++ ya viaja como archivo estático dentro del klib
(`staticLibraries` en el `.def`), así que un framework dinámico agregaría un dylib que la app
tiene que embeber y firmar, más un salto de dyld en el arranque, sin ganar nada.

**Para qué sirve, si D5 dice que NoisyPad consume el klib:** es la vía de salida para un
consumidor iOS que **no** es KMP — un proyecto Xcode que quiere `import Watermelon` desde
Swift. Las dos formas de consumo son **alternativas, no complementarias**: usar las dos en la
misma app duplicaría el motor.

**Verificado que el artefacto es real, no que la task diga OK:**

| Chequeo | Resultado |
|---|---|
| Slices | `ios-arm64` + `ios-arm64-simulator`, ambos en el `Info.plist` |
| Tipo de binario | `current ar archive` — estático, como se pidió |
| C API adentro | **187 símbolos `wma_*`** por slice |
| Motor C++ adentro | 58 símbolos de `CoreAudioBackend` |
| Módulo Swift | `Headers/Watermelon.h` + `Modules/` |
| Cache | segunda corrida: 12 de 13 tasks UP-TO-DATE, 1 s |

Ese chequeo de símbolos quedó **en el job `ios` de CI**, no sólo en esta sesión: un
XCFramework que se construye pero no trae el motor adentro pasaría un `assemble` sin chistar.
Es además lo único que prueba que el archivo estático embebido en el klib **resuelve al
linkear un binario** — el link check de `build-ios.sh` valida el archivo, no el framework.

> [!WARNING]
> **Bug latente encontrado y arreglado de paso: `publish.yml` seguía en `ubuntu-latest`.**
> La nota de WA-3.1 avisó que el publish tenía que moverse a macOS en cuanto entrara cinterop,
> y `release-please.yml` se movió — pero `publish.yml`, que dispara con cualquier tag `v*` y
> con `workflow_dispatch`, quedó en Linux. Habría fallado en el próximo tag. Movido a
> `macos-latest` con el mismo setup (cmake/ninja de sistema, cache de `~/.konan`).

**Declarar los binarios de framework no toca los jobs de Linux:** verificado con
`--dry-run` sobre `assembleRelease + publishAllPublications...` → **0** tasks de framework
arrastradas. Además las tasks `link*Framework*` tienen un `onlyIf` de macOS, que es el
cinturón sobre los tirantes.

---

## 10. Requerimientos — Fase 5: Diferidos / backlog

| ID | Requerimiento | Detalle | Prio |
|---|---|---|---|
| WA-5.1 | USB en iPadOS | Investigación DriverKit/entitlements para clase de audio USB — solo si el negocio lo pide | P3 |
| WA-5.2 | Latency benchmark iOS | Port de `LatencyBenchmarkRunner` (hoy androidMain) sobre la infraestructura de WA-4.3 | P2 |
| WA-5.3 | AUv3 | Empaquetar el motor como Audio Unit v3 (extensión) — habilitaría NoisyPad como plugin en GarageBand/Logic/AUM. Análisis de arquitectura propio | P3 |
| WA-5.4 | CoreMIDI | Si el roadmap de NoisyPad incorpora MIDI-in en iOS | P3 |
| WA-5.5 | **Harness de UI multiplataforma + design system** | App de prueba en este repo que corra en Android e iOS contra la librería, con un design system propio y los componentes genéricos que valga la pena traer de NoisyPad. **APROBADA y EN CURSO** — controles 1 y 2 hechos; análisis y decisiones abajo | P1 |

### Análisis — WA-5.5, harness de UI y design system (propuesta 2026-07-27)

**El problema que resuelve, que es real y está creciendo.** Hoy la validación de esta
librería depende de dos cosas que no controlamos desde acá:

1. **La lista del smoke de Android son 9 ítems** y no se puede correr sin **NoisyPad** más un
   dispositivo. Nada de eso vive en este repo, así que cada categoría de WA-2.6 agrega deuda
   que nadie puede saldar desde acá.
2. **El input path de iOS se escribió a ciegas** y sigue sin que nada pruebe que captura audio
   de verdad. Es el riesgo no validado más grande del programa.

**WA-4.3 primera mitad ya apuntaba a esto** (sample app SwiftUI sobre el XCFramework), pero
cubre **sólo iOS** y sólo el camino Swift → framework. Un harness en Compose Multiplatform
cubriría **las dos plataformas** y ejercitaría `commonMain`, que es la superficie que de
verdad consume un cliente. **WA-5.5 subsume WA-4.3 primera mitad** si se aprueba.

**Lo que cuesta, medido y no estimado a ojo.** Hoy el repo es **un solo módulo** (`:audio`),
sin `samples/`, y el catálogo de versiones **no tiene una sola dependencia de UI** — ni
Compose, ni Activity, ni Material. La librería es deliberadamente libre de UI y `commonMain`
tiene cero imports de `android.*`. Entonces el harness implica:

- un módulo Gradle nuevo, **que no se publica** (ver el riesgo de abajo);
- **Compose Multiplatform como eje de dependencias nuevo** para este repo (plugin + runtime +
  el catálogo);
- del lado iOS, un shell de Xcode sobre el `UIViewController` de Compose — el XCFramework ya
  existe por WA-4.1, así que esa parte está;
- del lado Android, un módulo de app con su Activity.

> [!CAUTION]
> **El riesgo de diseño principal: que la UI se filtre al artefacto publicado.** Este repo
> publica `com.watermellonstudios:audio`, y su valor es justamente que no arrastra UI. El
> harness tiene que ser **estructuralmente incapaz** de entrar ahí: módulo aparte, sin
> `publishing`, y sin que `:audio` lo declare como dependencia en ninguna dirección.
>
> Y el design system merece una **decisión explícita, no llegar de rebote**: si va a ser
> compartido con NoisyPad tiene que ser un artefacto publicable propio (o su propio repo), con
> lo que eso implica de versionado y de que *este* repo pase a shippear Compose. Que aparezca
> como efecto secundario de construir un harness de test es la forma equivocada de tomar esa
> decisión.

**Cuándo — recomendación.** Ordenado así, y el porqué del orden importa más que el orden:

1. **Primero la cola de 15** que cierra WA-2.5/2.6. Es corta y deja la C API **completa**, así
   que el harness se construye contra una superficie quieta en vez de una que se mueve abajo.
2. **Después el harness, con UI mínima y fea.** Su valor es *validar*, y validar no puede
   esperar a que exista un design system. Acá se drena el smoke de 9 ítems y se prueba por fin
   si el input path de iOS captura.
3. **El design system al final**, con el harness como primer consumidor real y NoisyPad como
   fuente de cosecha.

**El paso 3 va último a propósito.** Construir el design system primero es invertir en
componentes antes de saber cuáles el harness necesita, y un design system sin un consumidor
real es especulación con buena letra. Al revés, el harness te dice exactamente qué componentes
se repiten y cuáles eran de un solo uso — que es la única información que hace que la cosecha
desde NoisyPad no sea copiar por copiar.

**Si la prioridad es el design system como entregable de producto** (porque NoisyPad iOS lo va
a necesitar igual), el orden se puede invertir — pero eso es una decisión de producto y
conviene tomarla como tal, no dejarla implícita en "hagamos las cosas bien".

**Lo que no se puede decidir desde este repo:** qué componentes de NoisyPad valen la pena.
NoisyPad es un repo privado aparte y no está montado acá, así que la cosecha necesita una
sesión con acceso a los dos. Antes de eso conviene saber si su UI es Compose (probable) y
cuánto de ella es genérica de verdad y no específica del pad.

---

### Propuesta concreta — WA-5.5 (2026-07-27, sin aprobar)

El paso 1 de la recomendación de arriba **ya está hecho**: WA-2.5/2.6 cerró, así que la
superficie contra la que se construiría el harness está quieta.

#### A. Cómo se estructura para que la UI no pueda entrar al artefacto publicado

Cinco capas, y la primera ya existe sin que nadie la haya diseñado para esto:

1. **El pipeline de publicación es path-qualified y no puede alcanzar otro módulo.** Los dos
   workflows dicen literalmente `./gradlew :audio:publishAllPublicationsToGitHubPackagesRepository`
   (`publish.yml:90`, `release-please.yml:116`), nunca la variante de raíz. **Un módulo nuevo
   no es publicable por el pipeline actual aunque aplique `maven-publish`.**
2. **El harness no aplica `maven-publish`.** KMP crea publicaciones sólo cuando el plugin
   está aplicado; sin él no hay nada que publicar ni siquiera con un publish de raíz.
3. **La dependencia va en una sola dirección**: `:harness → :audio`, jamás al revés.
4. **El gate mecánico, que es la pieza que falta.** Este repo ya convirtió un "no hagas eso"
   en una regla estructural una vez —WA-0.4 y `check-cpp-portability.sh`, que barre 324
   archivos buscando `jni.h`/`android/` y voltea el build—. La misma receta:
   `scripts/check-no-ui-in-library.sh`, **como noveno comando del gate**, afirmando dos cosas:
   - el conjunto de proyectos con publicaciones es exactamente `{:audio}`;
   - el classpath resuelto de `:audio`, **para cada target**, tiene cero coordenadas
     `org.jetbrains.compose`, `androidx.compose` y `androidx.activity`.

   La segunda es la que agarra el modo de falla realista. Nadie va a publicar el harness por
   accidente; lo que pasa de verdad es que alguien agrega una dependencia de Compose a
   `:audio` "para un helper de preview". Un check de publicaciones no ve eso; el de classpath sí.
5. **El plugin de Compose se declara sólo en el build file del harness**, nunca en la raíz ni
   en `watermelon.kmp.native`. Como `:audio` toma su configuración de ese convention plugin,
   si el plugin nunca nombra Compose, `:audio` no puede heredarlo.

> [!NOTE]
> El catálogo de versiones **sí** va a tener entradas de Compose, y eso está bien: el catálogo
> declara versiones disponibles, no dependencias efectivas. Confundir las dos cosas lleva a
> gimnasia inútil. Lo que importa es el classpath resuelto, que es lo que el gate mide.

#### B. Qué se necesita del lado de Xcode

> [!CAUTION]
> **La propuesta original se equivocaba acá, y en la dirección cara.** Decía que "el
> XCFramework ya existe por WA-4.1, así que esa parte está". **Un harness Compose
> Multiplatform no consume el XCFramework.** El comentario del propio convention plugin lo
> dice de las dos vías de consumo: *son alternativas, no complementarias — usar las dos en la
> misma app duplicaría el motor*. El harness consume `:audio` como dependencia KMP (vía klib),
> y **produce su propio framework**, que es lo que Xcode embebe. El XCFramework de WA-4.1
> sigue siendo lo que es: la salida para un consumidor Swift que **no** es KMP.

Con eso claro, la lista es corta:

1. **`binaries.framework` en el módulo del harness** (p.ej. `baseName = "HarnessKit"`),
   exponiendo el `UIViewController` de Compose. Adentro viajan Compose + el klib de `:audio` +
   `libwatermelon_audio.a`, que ya va embebido en el klib por `staticLibraries` del `.def`.
2. **Un proyecto Xcode** en `harness/iosApp/`, deployment target **15.0** para no quedar por
   debajo del de la librería.
3. **Build phase que corra la task de Gradle** del framework antes de linkear. La cadena hacia
   el `.a` **ya está resuelta**: el convention plugin engancha `cinteropWatermelonAudio*` a
   `buildIosNativeLib`, así que `scripts/build-ios.sh` corre solo por depender de `:audio`.
4. **`NSMicrophoneUsageDescription` en el `Info.plist`.** Sin esa clave iOS mata la app en el
   primer intento de captura — y la captura es *exactamente* la pregunta que el harness existe
   para responder. Es la forma más probable de perder una tarde.
5. **Firma:** un team personal gratis alcanza para el simulador, que es la mitad que WA-4.3
   apuntaba y donde ya se puede contestar lo de la captura — **el simulador de iOS usa el
   micrófono del Mac**. El device (G2) necesita team real y queda para después.
6. **Decidir `isStatic` del framework del harness.** No es libre: el motor ya viaja estático
   adentro del klib.
7. **`AVAudioSession` no hay que escribirla**: `AudioSessionManager` (WA-3.4) ya elige
   `PlayAndRecord` cuando hace falta. Lo que falta es que el harness la maneje y muestre lo
   que reporta.

#### C. Los controles mínimos — 7, y qué drena cada uno

| # | Control | Ítems del smoke que drena |
|---|---|---|
| 1 | **Transporte**: start/stop/pause/resume con fade + lectura del `AudioState` | 1 (BridgeConcurrency), 4 |
| 2 | **Pad XY + selector de oscilador + slider de depth** | 4, **11 (`setDepthValue` muerto)** |
| 3 | **Rack de efectos**: add/remove/reorder, bypass, params, selector de routing mode | 4, 2 (agregar >6 efectos para ver el tope), y todo lo que se migró hoy |
| 4 | **Monitor de entrada**: enable, source, **medidor de nivel en vivo + indicador de clipping** | 3, **y la pregunta abierta más grande del programa** |
| 5 | **Tira de looper**: prepare(bars), arm at next bar **mostrando el valor devuelto**, record, play, export a archivo | 5, 7, 8, 9 |
| 6 | **Metrónomo/clock**: BPM, click, count-in | 5 (junto con el 5) |
| 7 | **Panel de diagnóstico**: device caps, backend actual **+ selector**, recommended buffer size, latency report, y **la vista de logs nativos** | 2, 6, **10 (`selectBackend` que miente)** |

**El control 4 es el que justifica el proyecto entero.** Es lo único que contesta si el input
path de iOS captura de verdad, y no hay forma de contestarlo con tests de host: el stub de
`InputNode` de `core/tests/` no tiene comportamiento, y por eso el medidor tiene que ser de
nivel *audible*, no un booleano de "arrancó".

**El control 5 tiene que mostrar el valor devuelto por el arm, no sólo armar.** El bug de la
tanda 2 era justamente que devolvía un trigger frame para una grabación que nunca arrancaba;
un botón que sólo dice "armado" no lo habría visto nunca.

> [!NOTE]
> **La vista de logs del control 7 se volvió construible hoy.** `wma_log_capture_*` no existía
> a la mañana: la captura de logs era el único pedazo de la cola sin contraparte en C. Falta un
> paso, y conviene decirlo en vez de darlo por hecho: **el bridge de Kotlin todavía no la
> expone** — `IAudioNativeBridge`/`IosAudioBridge` no tienen `drainCapturedLogs`. Es trabajo
> chico y con caller (el harness), que es justo la condición que faltaba para agregarlo.

#### D. Lo que esta propuesta NO decide

El design system. Sigue yendo al final, con el harness como primer consumidor, y la cosecha
desde NoisyPad sigue necesitando una sesión con los dos repos montados. **Una UI mínima y fea
es un requisito de esta etapa, no una concesión**: si el harness espera al design system, la
pregunta de la captura se sigue sin contestar mientras tanto.

---

## 11. Testing

| ID | Requerimiento | Detalle | Prio | Estado |
|---|---|---|---|---|
| WA-T.1 | Tests C++ en macOS | Los googletest existentes (dsp/effects/looper/voice/engine) ya compilan en host: agregar job macOS con clang de Xcode — detecta problemas de portabilidad (MSVC/MinGW vs clang-apple) antes de tocar iOS | P0 (parte de WA-0.3) | ✅ **HECHO** — paso en el job `ios`; ya encontró un bug (script roto en bash 3.2, ver §5) |
| WA-T.2 | commonTest Kotlin | Cobertura de la lógica común (WA-1.5) corriendo en JVM y luego contra `IosAudioBridge` en simulador (WA-3.2) | P1 | Parcial — 34 tests verdes en JVM; el binario K/N de test linkea |
| WA-T.3 | Smoke cinterop | Test de simulador sobre la C API: handle, version, cadena de efectos, params, bypass, setXY | P0 | ✅ **HECHO** 2026-07-25 — `iosTest/CinteropSmokeTest.kt`, 7 tests. El round-trip vía `AudioEngineFactory.create()` que pedía el criterio original llega con WA-3.2, cuando exista el bridge |
| WA-T.4 | Verificación RT | Sesión de Instruments (Time Profiler + Allocations) sobre el render callback de `CoreAudioBackend`: cero allocs, cero locks, sin prioridad invertida | P0 (criterio de WA-2.4) | Pendiente |

### Prerrequisito de entorno local — RESUELTO (2026-07-25)

~~`./gradlew :audio:iosSimulatorArm64Test` **se cuelga indefinidamente** en la máquina de
desarrollo: Xcode 26.6 no tiene completado su *first launch*.~~

Ya no aplica: `xcodebuild -checkFirstLaunchStatus` devuelve 0 y hay simuladores
disponibles. **Los tests de Kotlin/Native corren local**, que es como se verificó WA-3.1.
Ya no hace falta conformarse con `:audio:linkDebugTestIosSimulatorArm64`.

---

## 12. Guía RT-safety para iOS (normativa)

1. El render block de `AVAudioSourceNode` corre en el thread RT del sistema: **mismas reglas que Oboe** — sin allocs, sin locks, sin I/O, sin ObjC/Swift messaging. El block debe ser un trampolín directo a la función C++ de mezcla.
2. Las estructuras existentes (`std::atomic`, `LockFreeRingBuffer`, `LockFreeEventQueue`, `ParameterSmoother`) aplican sin cambios.
3. **Kotlin/Native nunca en el thread RT** (D6): el GC de K/N no es determinista. Estado hacia Kotlin: polling desde dispatcher (patrón `StateSynchronizer` actual) o drenaje de colas lock-free desde un loop de coroutine.
4. Diferencia a vigilar: Core Audio entrega buffers Float32 no-interleaved por defecto; decidir en WA-2.4 si el pipeline interno consume así o se convierte en el borde (una sola conversión, pre-asignada).

---

## 13. Riesgos y mitigaciones

| Riesgo | Impacto | Mitigación |
|---|---|---|
| **El core está acoplado a Oboe** (WA-2.0, descubierto 2026-07-22) | **Alto — bloquea toda la ruta crítica hacia G2.** Se creía que `IAudioBackend` ya abstraía el I/O; `AudioEngine` tiene además un camino directo a Oboe, e `InputNode` hereda de una clase Oboe | Refactor incremental detrás de `IAudioBackend`, con la suite C++ (495 tests) y el job `ios` como red. Riesgo de regresión en Android: es el critical path que hoy shippea — conviene hacerlo por partes y medir latencia antes/después |
| Latencia iOS insuficiente con AVAudioEngine | UX del instrumento | Medir temprano (WA-4.3); plan B explícito: AURemoteIO detrás del mismo `IAudioBackend` (D2) |
| Overhead cinterop en llamadas de alta frecuencia (`setXY` por frame de gesto) | Jitter de control | Las llamadas C desde K/N son baratas; medir en WA-4.3; plan B: batching de eventos XY (la infraestructura de colas ya existe) |
| ~~Gap real de la C API mayor al estimado (~89)~~ | ~~Cronograma F2~~ | **MITIGADO (WA-0.1, 2026-07-22):** gap portable = 110, ~79 neto. La estimación se confirmó. Riesgo residual: el looper concentra 39 de esas 110 |
| Divergencia silenciosa JNI vs C API durante la transición | Bugs solo-iOS o solo-Android | WA-2.6 (JNI sobre C API) elimina la clase de bug por construcción; hasta entonces, la tabla WA-0.1 es el contrato |
| Kotlin/Native + libs estáticas grandes: tiempos de link | DX | Cachear XCFramework (WA-4.1); compilar targets iOS solo en CI/macOS |
| libusb (LGPL) | Licenciamiento iOS | No aplica: libusb queda excluido del build iOS (D4) |
| Deriva de versión Kotlin con NoisyPad | Metadata KMP incompatible | D8: lockstep de versión; check en CI |

---

## 14. Secuencia y gates con NoisyPad

```
WA-0 (gap + targets + CI) ──► WA-1 (quick wins) ──► WA-2 (C++: CMake iOS, PlatformApple,
        │                                                  CoreAudioBackend, C API completa)
        │                                                        │
        └──► WA-4.2 parcial (publicar metadata KMP) ─────────────┼──► gate G1 NoisyPad (Fase 2)
                                                                 ▼
                                                   WA-3 (cinterop + IosAudioBridge)
                                                                 ▼
                                                   WA-4 (XCFramework + publicación)
                                                                 ▼
                                                   gate G2 NoisyPad (Fase 5: app iOS con sonido)
```

- **G1 (temprano):** NoisyPad solo necesita que los tipos de `domain/` estén disponibles como metadata KMP para convertir su `core-domain` — eso se logra con WA-0.2 + una publicación intermedia (WA-4.2 parcial), sin esperar el backend de audio.
  **Estado 2026-07-25: la publicación KMP YA EXISTE — G1 es sólo validar el consumo.**
  Ver la verificación abajo.

#### Verificación de la publicación KMP (2026-07-25)

Reproducida con `./gradlew :audio:publishToMavenLocal` y contrastada con los logs del run
de release 1.8.1 en CI. El módulo raíz `com.watermellonstudios:audio:1.8.1` declara **las
tres plataformas**, cada una apuntando a su módulo:

| Variante | platform | target | `available-at` |
|---|---|---|---|
| `metadataApiElements` | common | — | (inline) |
| `releaseApiElements-published` | androidJvm | — | `audio-android` |
| `iosArm64ApiElements-published` | native | `ios_arm64` | `audio-iosarm64` |
| `iosSimulatorArm64ApiElements-published` | native | `ios_simulator_arm64` | `audio-iossimulatorarm64` |

Los artefactos existen y están completos: `.aar` para Android y **`.klib` para cada slice
de iOS**, cada uno con su `-sources.jar` y `-metadata.jar`.

> [!IMPORTANT]
> **La coordenada que debe usar NoisyPad es `com.watermellonstudios:audio`, no
> `:audio-android`.** El sufijo `-android` es el módulo Android suelto; Gradle llega a él
> solo, vía el `available-at` del módulo raíz, cuando el consumidor declara el raíz. Un
> consumidor KMP que pida `:audio-android` se queda clavado en Android y no resuelve el
> source set iOS — y el síntoma sería "la metadata KMP no funciona", cuando en realidad
> funciona. Este documento lo venía escribiendo mal en su encabezado.

Queda entonces para G1, y sólo eso: declarar el raíz en NoisyPad, confirmar que resuelve
para ambos targets, y verificar el lockstep de Kotlin (D8 — este repo está en 2.4.0).
Recordar que en iOS `getAudioBridge()` sigue lanzando `NotImplementedError` hasta WA-3.2:
hay tipos, no hay audio. Es deliberado y hay que comunicarlo para que no se lea como bug.
- **G2 (ruta crítica):** el sonido en iOS depende de WA-2.4 + WA-3 + WA-4. Es la ruta crítica de todo el programa KMP; conviene arrancar WA-2.4 (CoreAudioBackend) apenas cierre WA-2.1.

---

## 15. Métricas de éxito

1. **Reutilización C++:** 100% de dsp/effects/engines/voice/looper compilando para iOS sin modificaciones de código (solo build).
2. ~~**Reutilización Kotlin:** los 52 archivos de commonMain sin cambios~~ → **métrica corregida (2026-07-22).** La original era inalcanzable porque partía de un supuesto falso. Reformulada: **6 archivos de commonMain tocados** (34 fixes de portabilidad, ningún cambio de lógica salvo la reflection eliminada) + **2 helpers nuevos** (`Format.kt`, `Time.kt`); `iosMain` < 10 archivos.
3. **Paridad de API:** cobertura C API = 100% del JNI no-USB (tabla WA-0.1 en verde).
4. **Latencia iOS:** round-trip medido y documentado; objetivo indicativo ≤ 20 ms output-only con buffer 128–256 frames (validar contra la experiencia Android actual).
5. **Regresión Android:** suite C++ + smoke NoisyPad Android sin desviaciones tras WA-2.6.
6. **Portabilidad sostenida (nuevo):** los targets iOS compilan en cada PR (WA-0.3). Sin esto, `commonMain` vuelve a acumular dependencias JVM en silencio — que es exactamente cómo se llegó a los 34 errores de WA-0.2.

---

## 16. Estado del programa

### Nota de cierre — WA-2.5/2.6, categoría `lifecycle` (2026-07-25)

**Primera categoría de la fusión WA-2.5/2.6, cerrada.** Las 22 funciones JNI de
lifecycle / state / volume pasan por la C API; el JNI ya no entra a `AudioEngine` en ese
bloque. Delegación medida: **22/278** (`python3 scripts/c-api-gap.py`).

**El hallazgo que cambia cómo se dimensionan las categorías que siguen:** las 8
"faltantes" de `lifecycle` en `c_api_coverage.md` **no eran un gap**. Las 8 ya existían
con otro nombre y el matcher por tokens no las unía (`nativeHasInitializationFailed` ↔
`wma_has_init_failed`, `nativeStartEngineWithFade` ↔ `wma_engine_start`, …). El gap real
era 0 — y aun así el JNI transcribía las 22 a mano. **El número de gap no dimensiona
WA-2.6.** Por eso el script ahora mide delegación aparte, mirando adentro del cuerpo de
cada función JNI, y `c_api_coverage.md` tiene un §4b con esa métrica.

**El bug que sí apareció, y es una divergencia Android/iOS viva:** `AudioEngine::start`
declara `int fadeTimeMs = 10`, así que el default del motor **no es 0**. El JNI siempre
distinguió dos operaciones — `nativeStartEngine()` toma ese default, y
`nativeStartEngineWithFade(0)` corta de una y cancela cualquier fade en curso. La C API
las colapsaba: ramificaba en `fade_time_ms > 0`, así que un 0 explícito caía en el
default. `IosAudioBridge` mapeaba **las dos** a `wma_engine_start(engine, 0)`. Misma
llamada, dos plataformas, dos comportamientos.

Se arregló con **`WMA_FADE_DEFAULT (-1)`** en `watermelon_audio.h`: `fade_time_ms >= 0`
es una rampa explícita (0 = corte), y sólo el sentinel cae al default del motor. El
bridge de iOS lo toma del header por cinterop, no copiado a mano.

**Verificación — hay tests de host de verdad, no sólo el compilador.**
`api/watermelon_audio.cpp` entró al target `core_tests` (antes estaba excluido: *"minus
api/, which the tests do not exercise"*) y se sumó `test_c_api_lifecycle.cpp`, 13 tests:
la distinción `WMA_FADE_DEFAULT` vs 0 explícito, el contrato de handle nulo —que es lo
que justifica haber borrado los 20 `if (!g_jniState.engine) return <default>;` del JNI— y
el clamp de master volume. Suite: **540 tests, 0 fallas** (eran 527).

Los dos tests discriminantes se corrieron **contra la implementación vieja** antes de
darlos por buenos: fallan con el síntoma exacto (fade volume 0.1333 = 64/480 frames, la
rampa de 10 ms que nadie pidió). Los otros 11 pasan en ambas versiones: fijan contrato,
no regresión.

**Lo que sigue sin tests:** las 22 funciones JNI en sí. Necesitan device. El gate fue el
compilador más el hecho de que ahora el cuerpo es una línea sobre código sí cubierto.

### Nota de cierre — WA-2.5/2.6, categoría `input/monitor` (2026-07-26)

**Segunda categoría cerrada.** Las 21 funciones JNI de input y monitoring pasan por la
C API. Delegación: **43/278**. Gap portable neto: 79 → **72**.

A diferencia de `lifecycle`, acá **sí había C API que escribir**: 8 funciones nuevas —
`wma_input_set_noise_gate_threshold`, `wma_input_is_noise_gate_open`,
`wma_input_get_level_linear`, `wma_input_get_latency_ms`,
`wma_input_is_monitoring_enabled`, `wma_input_set/get_monitoring_volume` y
`wma_input_get_metering_snapshot` (el batcheado de 7 valores, con
`WMA_INPUT_METERING_VALUES` para que el layout no se copie a mano en ninguno de los dos
lados).

> [!WARNING]
> **Esto cambia comportamiento en Android, en dos lugares.** No es un descuido: la C API
> hacía **más** que el JNI en estas tres funciones, porque se escribió con el camino de
> captura del `BackendManager` (WA-2.4, etapa 2) adentro. Unificar significa que Android
> pasa a recorrerlo también.
>
> 1. **`nativeStartInputStream`** — cuando `InputNode::startInputStream()` falla, antes
>    devolvía `false` y listo. Ahora `wma_input_start()` le pide captura al
>    `BackendManager`, lo que puede reabrir el stream de salida en full-duplex. **Sólo
>    afecta el camino de falla**; un mic que abre bien retorna antes. Y si el reopen
>    falla, `requestCapture` reabre sin captura, así que el peor caso es un reinicio de
>    stream en vez de un `false` silencioso.
> 2. **`nativeIsInputStreamRunning`** — ahora también da `true` cuando el backend lleva
>    la captura sin un stream de nodo aparte. En Android eso es el camino USB/split, que
>    hasta hoy leía "no está corriendo" mientras el input fluía.
>
> **Va a la lista del smoke manual en NoisyPad Android**, junto con lo de WA-1.4 y WA-1.2.
>
> Un tercer cambio se verificó que **no** tiene efecto: `nativeStopInputStream` ahora
> retira el pedido de captura. `BackendManager::requestCapture` retorna temprano cuando el
> pedido se baja en vez de subirse (`if (!effective) return false;`), así que no reabre.

**Dos guards se mudaron al lugar correcto:** el rango de `setInputSource` y su `try/catch`
ahora viven en `wma_input_set_source`. El `catch` importa más del lado C que del lado JNI:
una excepción de C++ desarmando la pila hacia Kotlin/Native no es un error atrapado, es el
proceso muerto.

**Verificación — `test_c_api_input.cpp`, 12 tests.** Con un límite explícito: `core/tests`
sustituye `InputNode.cpp` por un stub sin comportamiento, así que no se afirma nada sobre
niveles ni gating. Lo que sí se cubre es el contrato de "todavía no hay nodo" (lo que el
JNI resolvía a mano y ya no), que cada función llegue al método que dice, y la forma del
snapshot. La sonda útil es el nivel: sin nodo da −100 dB y el stub da −120, así que los
dos estados se distinguen en vez de leerse los dos como silencio.

Se probaron mutando el código: sacar el guard de rango y hacer que el snapshot devuelva
ceros en vez de `false` **hace fallar exactamente 2 tests**. El clamp del monitoring volume
**no** se testea y está dicho en el archivo: `InputNode` ya clampea, en producción y en el
stub, así que una assertion ahí pasaría con o sin clamp en la C API — sería teatro.

**Lo que esto NO habilita todavía:** iOS sigue sin poder usar el input desde Kotlin.
`IAudioNativeBridge` (commonMain) no expone la superficie de input — hoy es sólo de
`AudioNativeBridge` en Android. La C API ya la tiene entera; falta subirla a la interfaz
común. Es un ticket aparte, no parte de WA-2.5/2.6.

### Nota de cierre — WA-2.5/2.6, categoría `effects` (2026-07-26)

**Tercera categoría cerrada.** Las 14 funciones JNI de la sección 8 pasan por la C API.
Delegación: **57/278**. Gap portable neto: 72 → **71**.

La migración en sí fue la más mecánica de las tres: la sección 8 ya replicaba los códigos
de `JniError` valor por valor, los guards de índice y los `try/catch`. Lo único que quedó
del lado JNI es lo que un API de punteros no puede hacer: pinear los arrays de Java
(`ScopedIntArrayRW`) y chequear que los largos coincidan.

> [!WARNING]
> **Apareció AUD-6 reintroducido en el camino de la C API — el bug que el JNI arregló hace
> años y que iOS tenía igual.**
>
> `AudioEngine::setParameter` bumpea la versión de estado en **cada** llamada, y el
> `StateSynchronizer` de Kotlin emite en cada bump. Por eso el JNI rutea los batch por
> `setParametersBatch`, que bumpea **una sola vez al final**: sin eso, un scene load se
> observa como N estados parciales en vez de uno coherente.
>
> `wma_effect_set_params_batch` estaba escrita como transcripción del setter **individual**,
> no del batch: un loop de `setParameter`, N bumps. Cualquier consumidor de la C API —hoy
> iOS— tenía el bug.
>
> Arreglado: ahora rutea por `setParametersBatch`. Y se agregó
> **`wma_effect_set_params_multi`**, la contraparte de `nativeSetMultipleEffectParameters`,
> que no existía en la C API — sin ella un scene load multi-efecto habría necesitado una
> llamada por efecto, trayendo los estados parciales de vuelta por la otra puerta.

**Verificación — `test_c_api_effects.cpp`, 15 tests.** Acá, a diferencia de `input`, el
`EffectChain` es **real** en el host, así que se testea comportamiento: add/remove/reorder
cambian la cadena de verdad, y los códigos de error salen del motor.

El test que importa es `TheBatchSetterBumpsTheStateVersionExactlyOnce`: cuenta bumps de
`wma_get_state_version`. Es la **única** forma de ver este bug — los dos caminos dejan cada
parámetro con el valor correcto, sólo difieren en cuántas veces avisan. Revertido el fix,
falla ese test y sólo ese.

**Dos tests estaban mal escritos y el motor los corrigió:** afirmaban
`get_param(...) == 0.25f` después de un batch, pero el parámetro 0 de un `FILTER` es una
frecuencia de corte y el efecto clampea 0.25 a 20 Hz. Reescritos para comparar **contra lo
que produce el setter individual** con la misma entrada — que además es el invariante que
de verdad importa (batch ≡ individual salvo en la cuenta de bumps) y es inmune al clamp.

**Quedaron afuera a propósito**, para su propia sección: los 5 `SoundFontPreset*`
(sección 6) y `nativeHasVocoderEffect` (sección 15). Caen en el bucket "Effects" del script
por las keywords `preset` y `effect`.

### Nota de cierre — WA-2.5/2.6, categoría `oscillator/synth` (2026-07-26)

**Cuarta categoría cerrada, y la más grande hasta ahora: 40 funciones.** Cubre las
secciones 4 (XY/Oscilador), 5 (Engine synth), 6 (SoundFont), 15 (Vocoder) y 18
(Arpeggiador) — todo lo que hace sonido antes de la cadena de efectos.
Delegación: **98/278**.

**Se buscó el bug de divergencia a propósito y no está.** El patrón venía 3 de 3;
ahora es 3 de 4. El arpegiador es 19/19 idéntico entre JNI y C API, guards y valores
por defecto incluidos; vocoder 4/4; secciones 4 y 5 iguales. La única función que
faltaba de verdad era **`wma_sf_get_preset_bank_program`**.

La única asimetría es a favor de la C API y es benigna: `wma_sf_load_fd` valida
`fd`/`offset`/`length` antes de llamar, cosa que `nativeLoadSoundFontFromFd` no hacía.
Los dos caminos terminan en `false`, así que es un no más barato, no una respuesta
distinta.

**Casi se escapa una función.** `nativeSetSecondaryOscillatorType` es sección 4 pero
vive en el bloque de dual-touch del JNI, lejos del resto de los osciladores. La
encontró la métrica de delegación al quedar en 20/21, no la lectura del diff. Es el
argumento más concreto a favor de esa métrica.

**Verificación — `test_c_api_synth.cpp`, 13 tests (580 en total).** Cubre el
round-trip del tipo de motor, el flag del arpegiador, los guards de argumentos de
SoundFont y el contrato de handle nulo de las 40.

**Dos límites quedaron escritos en el archivo en vez de tapados:**

- **El range check del tipo de oscilador y la validación de frequency range no se
  pueden testear**: los dos se movieron del JNI a la C API, y ninguno tiene getter,
  así que un valor rechazado es indistinguible de uno aceptado desde afuera. Agregar
  getters sólo para testear sería inventar API; dejar un test que no puede fallar
  sería peor.
- **El guard de `fd` de `wma_sf_load_fd` tampoco.** Se comprobó mutando: borrarlo
  **no** hace fallar ningún test, porque el dispatcher rechaza esos valores igual.
  El test quedó, renombrado a `BadLoaderArgumentsFailAndLoadNothing`, porque el
  contrato sí vale — pero dice explícitamente que no prueba dónde se aplica. Los
  checks de null sí son suyos: sin ellos un path nulo llega a tsf y eso es un crash,
  no un `false`.

Lo que sí muerde, comprobado por mutación: `wma_sf_get_preset_bank_program` debe
dejar los out-params intactos cuando falla. Escribirlos igual hace fallar el test —
y importa, porque banco 0 / programa 0 es un preset real (suele ser el piano), así
que un caller que lea después de un `false` se lleva un valor plausible y falso.

**Se corrigió el matcher de `c-api-gap.py`.** Las 10 funciones de SoundFont figuraban
como gap permanente: el JNI dice `LoadSoundFontFromPath`, la C API `wma_sf_load_path`,
y existen desde siempre. Ahora el script pliega abreviaturas (`SoundFont` → `sf`) y
descarta `from`. **El neto bajó de ~71 a ~61 sin escribir una línea de motor** —
9 de las 10 ahora machean, y se auditó que no entraran falsos positivos.

### Nota de cierre — WA-2.5/2.6, categoría `voice` (2026-07-26)

**Quinta categoría cerrada: 21 funciones**, secciones 7 (Voice Filter), 13 (Dual Touch)
y 14 (Voice System), más los cuatro `SfNote*` de la 6 que se me habían pasado en la
categoría anterior (el grep buscaba `SoundFont`, no `SfNote`). Delegación: **119/278**.

**Cero funciones nuevas de C API.** Es la primera categoría que no necesitó ninguna: las
21 ya existían, varias con nombres que el matcher no puede aparear
(`nativeTriggerChordNotes` ↔ `wma_voice_trigger_chord`). Gap nominal 6, trabajo real 0.

> [!WARNING]
> **Se encontró y arregló una lectura fuera de rango en `nativeUpdateMultiTouch`.**
>
> `count` llega como parámetro propio, independiente del largo real del array, y **nada
> cruzaba los dos**. El desempaquetado lee `count * 6` floats (tope 4 toques = 24), así
> que un caller que pasara `count=4` con un array de dos toques leía **12 floats pasados
> del final** del buffer del heap.
>
> El arreglo va en el JNI, no en la C API: `wma_*` recibe un puntero pelado y no puede
> conocer el largo. Es el mismo reparto que ya tenía el batch de efectos.
>
> No lo encontró el compilador ni la migración mecánica — apareció al leer la función
> para migrarla.

**Otro hallazgo, no arreglado a propósito: `VoiceManager::setMaxVoices` es un no-op.**
Clampea el argumento, loguea *"requires recreation of VoicePool to take effect"* y vuelve
sin tocar nada. O sea que `wma_voice_set_max` y `nativeSetMaxVoices` son un setter público
que no hace nada — y encima bumpea la state version, notificando un cambio que no ocurrió.
Arreglarlo es cirugía de asignación de voces (recrear el pool con el thread de audio
leyéndolo) y no entra en un PR de migración. **Queda un test de caracterización que
documenta el estado actual y falla si alguien lo implementa**, más un ticket propio.
Conviene chequear si NoisyPad lo llama creyendo que limita la polifonía.

**Verificación — `test_c_api_voice.cpp`, 16 tests (596 en total).** Acá `VoiceManager` es
real en el host, así que se testea comportamiento de verdad: un toque = una voz, el tope
de 4, y que apagar el sistema libere lo que sonaba.

**El motor corrigió tres tests míos**, y las tres correcciones enseñaron algo:

1. `updateMultiTouch` **no asigna voces**: sólo entrega los toques al trigger source. La
   asignación ocurre en `processSourceEvents()`, en el thread de audio, así que el conteo
   no se mueve hasta renderizar un bloque. Sin eso el test leía 0 para siempre y parecía
   un API roto.
2. **Una voz soltada sigue contando como activa** mientras corre su cola de release, que
   es correcto: todavía suena. El test pasó a afirmar que *eventualmente* se libera, con
   una cota holgada, en vez de fijar el largo de la envolvente.
3. El tope de voces sólo se puede observar si se fija **antes** de que suene algo — y de
   ahí salió el hallazgo del no-op.

Los helpers `startAt()` y `render()` subieron de la suite de lifecycle a
`support/CApiFixture.h`: es la tercera suite que los necesita.

### Análisis estático del smoke de Android (2026-07-26)

**No hay device ni AVD en la máquina** (`adb devices` vacío, `emulator -list-avds` sin
resultados), así que el smoke manual sigue pendiente. Lo que sí se hizo es leer los call
sites reales de NoisyPad (`../NoisyPad`, branch `feature/f4-e4-catalogo`) contra los dos
cambios de comportamiento de `input/monitor`. Cambia la evaluación de riesgo.

> [!CAUTION]
> **`startInputStream` tiene una regresión plausible en el caso de permiso denegado, y es
> el caso de falla más común.**
>
> El call site es `InputStateManager.startInputStream()`
> (`feature-usb/.../InputStateManager.kt:130`): usa el booleano para decidir entre
> `InputEvent.StreamStarted` + arrancar el polling, o `InputEvent.StreamError`.
>
> Antes: si `InputNode::startInputStream()` fallaba —típicamente porque el usuario negó
> `RECORD_AUDIO`— volvía `false` de una y NoisyPad mostraba el error. Sin costo.
>
> Ahora: se le pide captura al `BackendManager`, que **reabre el stream de salida**. Sin
> permiso esa reapertura tampoco consigue captura, así que `requestCapture` cae a reabrir
> *sin* captura. Resultado: **un corte audible en la reproducción, y después el mismo
> error**. Estrictamente peor que antes en ese camino.
>
> No se mitigó porque no hay forma barata de distinguir "falló por permiso" de "falló por
> otra cosa" —`startInputStream()` devuelve un `bool` pelado— y las alternativas
> reintroducen divergencia de plataforma, que es lo que WA-2.6 viene a sacar. **Es una
> decisión de producto sobre un camino de falla: hay que mirarla en el smoke antes de
> publicar.** Reproducción: negar el permiso de micrófono, poner audio a sonar, y darle a
> "Start input" en la pantalla de input test.

**El otro cambio, en cambio, parece un arreglo.** `isInputStreamRunning` ahora también
reporta `true` cuando el backend lleva la captura sin stream de nodo aparte — el camino
USB/split. Sus call sites son `pollLevels()` (early return) y `refreshState()`
(`InputStateManager.kt:235` y `:270`), que además arranca el polling. Con el cambio, la
entrada por USB pasa a reportar "corriendo" y a mostrar niveles, cosa que antes no hacía
aunque el audio estuviera fluyendo — los medidores del `InputNode` sí se actualizan por
ese camino, porque `onAudioReady` rutea `inputData` a `feedExternalInput()`. **Igual hay
que verificarlo con la placa USB conectada.**

**`setMaxVoices` no lo llama nadie.** Grep sobre NoisyPad: cero hits. Con eso la decisión
del ticket deja de ser abierta — corresponde **retirarlo**, no implementarlo.

**`stopEngineWithFadeSync(fadeTimeMs = 300)`** (`MainActivity.kt:379`) es el único call
site del fade y pasa un valor positivo, así que `WMA_FADE_DEFAULT` no lo toca: 300 ≥ 0
sigue yendo a `stopWithFade(300)` igual que antes.

### Nota de cierre — el smoke encontró un bloqueo de G1 (2026-07-26)

El smoke manual no se pudo hacer (no hay device ni AVD), pero **compilar NoisyPad contra
la branch sí, y encontró algo que ninguno de los 7 gates veía**.

> [!CAUTION]
> **`:audio:compileIosMainKotlinMetadata` estaba roto en esta branch: 109 referencias sin
> resolver en `IosAudioBridge.kt`.** Bloqueaba G1.
>
> Los bindings de cinterop llegaban a las compilaciones **por target** (`iosArm64`,
> `iosSimulatorArm64`) pero no al source set **compartido** `iosMain`, que es donde vive
> `IosAudioBridge.kt`. O sea: compilaba perfecto por target —que es lo único que corrían
> los gates— y fallaba al compilar la metadata común.
>
> **Se rompió con WA-3.2** (`IosAudioBridge.kt` no existe en `master`) y estuvo invisible
> desde entonces, porque ningún gate corre esa tarea y nadie había consumido la librería
> desde un módulo KMP con targets iOS.
>
> Arreglo: `kotlin.mpp.enableCInteropCommonization=true` en `gradle.properties`. 109
> errores → 0. `compileIosMainKotlinMetadata` entra al set de gates.

**Cómo apareció, que es la parte reutilizable:** el `core-domain` de NoisyPad es KMP con
targets iOS y consume la coordenada raíz `com.watermellonstudios:audio`. Consumir desde ahí
dispara la compilación de metadata. Ningún gate nuestro lo hacía.

**Ojo con el `includeBuild` de NoisyPad:** el bloque comentado en su `settings.gradle.kts`
sustituye **sólo** `audio-android`. Pero `core-domain` usa `libs.audio` (la raíz KMP) y el
resto usa `libs.audio.android`. Descomentarlo tal cual deja un build **mixto** —
`core-domain` contra el artefacto publicado y todo lo demás contra el código local. Hay que
agregar la segunda sustitución.

**Resultado del smoke automatizable, con el arreglo puesto:** `:core-domain:assemble` y
`:core-data:assemble` de NoisyPad compilan contra la branch, 0 errores. Cubre la superficie
Kotlin de la librería en sus dos coordenadas.

**Lo que NO cubre y sigue pendiente:** el `:app:assembleDebug` completo no se pudo correr —
la branch `feature/f4-e4-catalogo` de NoisyPad tiene su propio breakage pre-existente en
`core-ui` (están migrando de `androidMain` a `commonMain` y `rememberHaptics` quedó a mitad
de camino). Se verificó que es de ellos: los mismos 31 errores aparecen **sin** el
`includeBuild`. Y sobre todo: **sigue sin correrse una sola línea del JNI migrado en un
device.**

**El árbol de NoisyPad quedó intacto** — `settings.gradle.kts` restaurado y verificado por
hash.

### Nota de cierre — WA-2.5/2.6, categoría `mode` (2026-07-26)

**Sexta categoría cerrada: 8 funciones de la sección 11.** Delegación: **125/278**. Y es la
que más cosas destapó de todas.

> [!IMPORTANT]
> **Murió la duplicación de estado de modo**, el hallazgo de auditoría que quedó abierto
> desde que se unificó el `InputNode`. `JniGlobalState` tenía sus propios `currentMode`,
> `modeTransitionInProgress` y `modeTransitionProgress` al lado de los de `WmaEngine`, como
> copias independientes: el JNI escribía y leía las suyas, la C API las suyas, y nada las
> mantenía en sincronía. Ya no existen — hay una sola copia, en `WmaEngine`.

> [!CAUTION]
> **La C API hacía MENOS que el JNI, y lo decía en su propio doc comment:** *"This is a
> simplified version. Full mode transitions (InputNode management, vocoder config, USB
> path) should be handled by the platform layer"*. O sea que la divergencia estaba
> **documentada como diseño** — pero el JNI creció la transición de verdad y esta se quedó
> con el boceto. Faltaban dos cosas, y la primera es audible:
>
> 1. **`requestResetEffectChain()` al entrar a INPUT_FX.** Va *antes* de apagar el
>    oscilador; el thread de audio lo atiende arriba del siguiente `onAudioReady()` y
>    vacía los buffers de scratch y feedback de la cadena. Sin eso, la cola de reverb y el
>    feedback del delay que dejó chaos_pad entran arriba de los primeros bloques del
>    micrófono como un estallido — **y cuanto más tiempo estuvo el usuario en el pad, más
>    fuerte**. iOS no lo tenía.
> 2. **La rama USB.** En un backend que entrega la entrada por el render callback
>    (`LibusbBackend`) no puede haber además un stream a nivel de nodo. La C API siempre
>    arrancaba el stream.
>
> Las dos se portaron a `wma_set_audio_mode`, que ahora es la implementación real y el JNI
> su wrapper. La rama USB quedó **sin `#ifdef`**: pregunta el tipo de backend al
> `BackendManager`, y en iOS ese nunca es `LIBUSB`, así que se lee como "arrancá el
> stream" por construcción.

También se agregó `wma_get_mode_name`, la única función que faltaba de verdad.

**Verificación — `test_c_api_mode.cpp`, 12 tests (610 en total).** El modo sí es observable
en el host: `isOscillatorEnabled()` distingue "tocando el pad" de "procesando el micrófono",
y se testea que INPUT_FX lo apague, que MIX lo devuelva, que sólo los modos con micrófono
creen el `InputNode`, y que salir de INPUT_FX corte el monitoring.

**Un test que casi queda mintiendo.** Escribí `EnteringInputFxResetsTheEffectChain` y al
mutar —borrar la llamada a `requestResetEffectChain`— **no falló**. Lo medí: la salida
después del switch da `0.000000` exacto, porque en el host la cola de reverb **nunca llega
a la salida** cuando el oscilador se apaga; el harness no modela ese camino. Quedó
renombrado a `SwitchingToInputFxLeavesNoResidueOnTheOutput`, que es lo que de verdad
afirma, con el límite escrito adentro: **el reset en sí va al smoke de device**, no está
testeado acá. Pinchar el test antes de creerle sigue siendo lo que separa cobertura de
decorado.

**Tercer hallazgo, con ticket propio: los flags de transición están muertos.**
`isInModeTransition` y `getModeTransitionProgress` devuelven siempre `false` y `0` — nadie
escribe ese estado. Y existe `core/ModeManager`, que tiene la maquinaria real con
crossfade, **desconectado de `AudioEngine`**: cero referencias. Migrarlos no lo cambió;
movió estado muerto de dos copias a una. Queda test de caracterización que falla si alguien
lo arregla.

### Nota de cierre — WA-2.5/2.6, categoría `análisis` (2026-07-26)

**Séptima categoría cerrada: 10 funciones.** Metering de salida (5), waveform, modulador (2)
y automatización (2). Delegación: **135/278**. Las tres filas del script quedan completas:
`Analysis` 13/13, `Modulation` 3/3.

**La migración fue mecánica y sin sorpresas** — cero funciones nuevas de C API, cero
divergencias entre las dos superficies. Pero escribir los tests destapó otra cosa.

> [!CAUTION]
> **Los medidores de salida no funcionan, en ninguna de las dos plataformas, y se ve en
> pantalla.**
>
> `AudioEngine::getOutputPeakLevel` / `getOutputRMSLevel` leen de `mOutputNode`, y
> **`OutputNode::process()` no lo llama nadie**: el nodo se aloca, se `prepare()`a y se
> queda ahí. Tampoco está en el `AudioGraph`. Así que peak y RMS son 0 permanente mientras
> suena audio.
>
> **Verificado del lado de NoisyPad:** `GuitarModeViewModel.startMetering()` postea
> `getOutputLevels()` en un loop y muestra el resultado. **Ese medidor nunca se movió.** El
> de input al lado sí anda, porque `InputNode` sí se maneja — probablemente por eso nadie
> lo notó.
>
> Lo encontró un test que escribí esperando que el medidor siguiera al audio. Falló, y en
> vez de bajarle la vara lo seguí hasta el origen. Queda test de caracterización que
> **falla el día que alguien lo arregle**, más ticket propio.

**Verificación — `test_c_api_analysis.cpp`, 14 tests (624 en total).** Lo que sí se cubre y
sigue valiendo: el piso de **−100 dB** para el silencio (0 dB sería fondo de escala, la
lectura opuesta), que los getters en dB sean el log de los lineales, que el batch de 4
coincida con los individuales —donde se escondería una transposición—, los rangos del
modulador (tipo 0–7, paramId, no-finitos) y los ejes de automatización.

**Un detalle del waveform que se preservó:** `nativeGetWaveformSamples` recorta el pedido
al largo real del array de Java. Eso **no se puede mover a la C API** —recibe un puntero
pelado— así que queda del lado JNI, igual que el cross-check del batch de efectos y el del
multi-touch. Es el mismo patrón por tercera vez: lo que sabe de largos de array se queda
arriba.

### Nota de cierre — WA-2.5/2.6, categoría `metronome` (2026-07-27)

**Octava categoría cerrada: 13 entry points, 10 funciones nuevas de C API.**
Delegación: **148/278**. Es la primera categoría que **no tenía sección** en
`watermelon_audio.h`, la primera cuyo gap quedó **corto** en vez de largo, y la
primera cuyo comportamiento se puede escuchar entero en la suite de host.

**La sección no existía.** La numeración del header saltaba de 19 (Looper) a 20
(Waveform & Metering): las 10 `nativeTransport*` no tenían contraparte ninguna.
Se creó la **§20 Transport** y las dos de abajo corrieron un número (Waveform
20→21, Configuration 21→22). Dos vecinas se dejaron donde estaban a propósito:
`wma_set_bpm` sigue en la §3 —fanea a los efectos tempo-sync *y* al Transport, y
un setter aparte los dejaría divergir— y `wma_looper_trigger_click` conserva su
nombre `looper_` en la §19 porque el generador de click es del looper y el
nombre ya está shippeado.

**13 entry points, no 11.** La fila del script dice 11: `nativeTransportFramesPerBar`
cae en "Mixer / Regions" por `bar` y `nativeLooperTriggerClick` en "Looper" —por
eso esa fila arranca en 1/79 sin que se haya tocado el looper. Tercera vez que la
tabla desparrama una categoría; la unidad sigue siendo la sección.

> [!CAUTION]
> **El metrónomo adelanta un bloque de audio, en las dos plataformas, y el gap no
> tenía nada que ver.** `Transport::tick()` disparaba el click con `next <= 0`,
> donde `next` es —después de restar `numFrames`— los frames que faltan *una vez
> consumido este bloque*. `next == 0` significa que el beat cae en la primera
> muestra del bloque **siguiente**: todavía no pasó. Al dispararlo igual, el click
> salía un bloque antes y, como la cuenta regresiva se reinicia desde ese instante
> adelantado, **el tren entero de clicks queda corrido un bloque para siempre**.
>
> Sólo muerde cuando `framesPerBeat` es múltiplo exacto del tamaño del callback, y
> eso no es exótico: 120 BPM a 48 kHz son 24000 frames, y **24000 / 192 = 125**
> — 192 es un burst de Oboe de los comunes. Con 256 no pasa (93.75) y el bug queda
> latente, que es por qué nadie lo vio.
>
> Arreglado a `next < 0`. El propio doc comment de la clase ya decía cuál era la
> intención: *"subsequent clicks fire in the callback block where the next beat
> falls"*. El `<= 0` no cumplía su propio contrato.
>
> **Lo encontró un test, no el compilador ni la migración.** Es un bug de
> producción preexistente, del mismo tipo que el de `voice`: no es divergencia
> entre las dos superficies, es una función que estaba mal desde antes y aparece
> porque para migrarla hay que leerla entera y escribirle un test que la mida.

**Verificación — `test_c_api_transport.cpp`, 26 tests (650 en total).** Ésta es la
primera categoría **audible de punta a punta en el host**: `Transport::tick()` corre
dentro de `AudioEngine::onAudioReady` y el click que agenda lo renderiza
`AudioLooper::process` unas líneas después, sobre el mismo buffer. Así que "¿sonó
el metrónomo?" se responde mirando las muestras. Las dos anteriores no podían:
`mode` tuvo que conformarse con "la salida no trae residuo" y `análisis` descubrió
que los medidores leen un nodo que nadie corre.

Lo que eso habilita: el intervalo entre clicks se mide en bloques y se compara con
`framesPerBeat` (que es lo que destapó el off-by-one), el downbeat se distingue del
off-beat por amplitud (0.35 contra 0.25), y el patrón `every_beat` se verifica
contra el compás. Detalle del harness que hace exactos los índices: el limiter de
lookahead de `OutputStage` atrasa todo 5 ms fijos, así que con bloques de 1000
frames un click de 10 ms ocupa `[240, 720)` de su bloque y nunca cruza al siguiente.

**Un test de caracterización, no de aprobación.** `RemainingBeatsIsASentinelInContinuousMode`:
el modo continuo arma el scheduler guardando un `1` en el mismo contador que la
cuenta regresiva decrementa, y nunca lo baja. `wma_transport_get_remaining_beats`
devuelve `1` para siempre, que no es una cantidad de nada. Queda documentado en el
header —hay que preguntar por `is_metronome_continuous()` primero— y pinchado por
un test que falla si alguien cambia el mecanismo.

**Mutación: 10 mutantes, 9 detectados.** Los tres tests de intervalo ya habían
fallado contra el código sin arreglar, así que ésos vinieron con los dientes
probados. De los otros: matar `stopMetronome`, apagar el modo continuo, igualar
las ganancias de downbeat y off-beat, romper el patrón `every_beat`, sacarle al
Transport el sample rate negociado, hacer que `startMetronome(0)` arme en vez de
parar, cambiar el default de 4 sin engine, y sacarle el compás a `framesPerBar`
— los ocho hacen fallar tests. **El décimo no se detectó y estaba bien así:**
sacar el guard de `if (!continuous) beatsLeft--` no cambia nada porque el *store*
de vuelta también está guardado, así que el decremento local se descarta. Es un
mutante equivalente, no un agujero de cobertura.

> [!WARNING]
> **El harness de mutación tendió una trampa que casi pasa por verde.** Restauraba
> el archivo con `mv fichero.bak fichero`, y eso le devuelve la mtime del momento
> del backup —**anterior** a la compilación del mutante—. ninja veía el fuente más
> viejo que el `.o`, decía "no work to do", y el binario se quedaba con la última
> mutación adentro. El fuente estaba limpio y `git diff` no mostraba nada: lo
> único que lo delató fue correr **la suite entera** después y ver fallar un test
> que ya había pasado. Moraleja para la próxima: al mutar hay que `touch` el
> archivo al restaurarlo, y correr el gate completo después de mutar, no sólo el
> filtro.

**Lo que NO se cubre, y se dice acá en vez de fingirlo:** el fast path de USB
(`LibusbBackend`) tiene su **propia** llamada a `mTransport.tick()`, y el backend
falso reporta OBOE, así que la suite sólo recorre el camino principal. Los dos
call sites se diffearon a mano y hacen lo mismo, pero una divergencia entre ellos
no haría fallar estos tests.

**Para el smoke de device**, encima de lo que ya había: el fix del off-by-one
cambia el timing del metrónomo en Android. Escuchar un count-in de 4 contra una
grabación armada al compás — antes de esto el click iba ~4 ms adelantado del grid
al que el looper alinea la grabación (`nextBarBoundary` sobre `mPlayFrameCounter`),
y ahora deberían coincidir.

**Nota de alcance:** `IAudioNativeBridge` (commonMain) hoy sólo expone
`setBpm`/`getBpm`; toda la superficie `transport*` es del wrapper de Android. Esta
categoría deja las `wma_transport_*` listas para que iOS las tenga, pero **subirlas
a la interfaz común es otra decisión** — arrastra al looper, que va último.

### Nota de cierre — WA-2.5/2.6, categoría `benchmark` (2026-07-27)

**Novena categoría cerrada, y la primera que migra a medias a propósito.** 3 entry
points delegan, **5 se quedan en el JNI con el porqué escrito**, y 2 funciones nuevas
de C API contra un gap nominal de 4. Sección **21 (Diagnostics & Latency)**, nueva;
Waveform y Configuration corren un número otra vez.

**Lo que no porta, y no se forzó.** `runLatencyOptimizationTest` e `isAAudioAvailable`
abren un `oboe::AudioStreamBuilder` para preguntar algo que **sólo existe en Android**
("¿me dieron AAudio en modo exclusivo?"). Las colas de `getDetailedLatencyInfo` ([4..7])
y del reporte describen un `oboe::AudioStream`. Tres son stubs deprecados que no tocan
el motor. Y `getAdaptiveBufferStats` lee el `LibusbBackend` (D4). Migrar cualquiera de
esas habría sido inventar una función de C API sin comportamiento del otro lado. El
encabezado de `jni_benchmark.cpp` las lista una por una.

**Las dos que sí:** `wma_get_recommended_buffer_size` y `wma_get_latency_report`.
**No se agregó un `wma_get_latency_info` batch** aunque el JNI arma un array de 8: sus
números ya son `wma_get_stream_info` (§2) + `wma_input_get_latency_ms` (§12), las dos
existentes y ya testeadas. El metering tiene batch porque una UI lo polea por frame;
nadie polea la latencia.

> [!CAUTION]
> **La métrica de delegación tiene un punto ciego y esta categoría lo destapó.**
> `scripts/c-api-gap.py` lee **un solo archivo**, `jni/jni_audio_bridge.cpp`. Se
> migraron 3 funciones y **el número no se movió ni uno**, porque viven en
> `jni_benchmark.cpp`. Son 13 entry points invisibles entre `jni_benchmark.cpp` (8),
> `jni_usb.cpp` (3) y `jni_engine.cpp`.
>
> Medido a mano sobre los cuatro archivos: **152/290**, contra el **148/278** que
> imprime el script. Al cerrar una categoría, **preguntarse primero en qué archivo
> vive** antes de creerle al delta.

**Dos cambios de comportamiento, los dos deliberados y los dos con test que falla
contra la versión vieja:**

1. **`wma_get_recommended_buffer_size` resuelve el sample rate por
   `currentSampleRate()`** en vez de "`getStreamInfo()` o si no 48000". El atajo viejo
   se saltaba la tasa preferida, así que un equipo configurado a 44.1 kHz que todavía
   no había arrancado stream recibía un tamaño calculado para 48. Es **exactamente** el
   anti-patrón contra el que advierte el comentario de `currentSampleRate()` en
   `AudioEngine.h`, y el que puso los SoundFonts a la tasa equivocada en WA-2.0.
2. **Dejó de truncar el requerimiento a `int` antes de comparar.** Truncar redondea el
   requerimiento **para abajo**, así que un target que necesitaba 128.6 frames se
   respondía con 128 — un buffer **más corto que la latencia pedida**, que es la única
   dirección en la que esta función no puede redondear.

**Verificación — `test_c_api_diagnostics.cpp`, 17 tests (667 en total).** Además de los
dos cambios de arriba, el contrato del buffer del reporte: convención snprintf, truncado
con NUL, y un centinela detrás del buffer. **6 mutantes, 6 detectados.**

> [!WARNING]
> **Segunda trampa del harness de mutación, después de la mtime.** El mutante que copia
> `fullLength` sin clampear —el overflow clásico— dio **"no detectado"**. Era mentira:
> el test **crashea el proceso** (rc=133), y mi harness sólo grepeaba las líneas `FAILED`
> de gtest, que un proceso muerto nunca imprime. **Un crash es la detección más fuerte
> que hay y estaba puntuando como la más débil.** Arreglado a mirar el exit code. Vale
> la pena la moraleja general: cuando una herramienta de verificación dice "no detectado",
> el primer sospechoso es la herramienta.

**Lo que NO se cubre:** todo lo que necesita un `oboe::AudioStream` real. Los 4 floats
de la cola de `getDetailedLatencyInfo` y las 3 líneas de Oboe del reporte salen de
`getOutputStream()`, que devuelve `nullptr` bajo BackendManager —el backend falso
incluido—, así que **ningún test de host los toca**. El split se verificó leyendo los
dos call sites.

**Un detalle del reporte que cambió para mejor:** ahora **nombra el backend**. La
información estaba disponible desde siempre (`BackendManager::getStreamInfo()` la trae
y `AudioEngine` la loguea al arrancar) y el reporte la tiraba, así que un reporte de
latencia tomado en el camino USB no decía nada de USB. También distingue "no hay stream
que medir" de omitir las tres líneas en silencio, que se leía como latencia cero.

### Nota de cierre — WA-2.5/2.6, categoría `looper`, las 4 tandas (2026-07-27)

**El looper está cerrado: 77/79.** Delegación **224/278** por el script, **228/290** real.
Los 2 que no delegan no lo hacen a propósito, y está escrito en el archivo (abajo).
**Pero WA-2.5/2.6 NO está cerrada** — al contar el final apareció un agujero en la propia
lista de categorías, ver la nota que sigue a ésta. Se hace por tandas
porque son 79 funciones: cada tanda es un commit con su gate completo, no un WIP.

**Tanda 1 — las 40 que ya tenían contraparte** (commit `6cbda84`). Cero funciones nuevas.
Los 40 pares se diffearon lado a lado antes de tocar nada: transcripciones fieles, cero
divergencia. `prepareTrack` cruza `JniError` y `WmaResult`, que son **idénticas entrada por
entrada**, así que la delegación preserva el valor que lee `NativeErrorCode`.

> [!CAUTION]
> **Una regresión que la migración mecánica iba a introducir, vista al leer el diff.**
> `wma_looper_set_track_loop_region` declaraba `int start_frame, int end_frame`, pero la
> cadena es 64-bit de punta a punta: Kotlin `Long` → `jlong` → `AudioLooper::setTrackLoopRegion`,
> que toma `int64_t` y **satura a int32 él mismo**, con un comentario diciendo que el ancho
> es deliberado porque `TrackBuffer` guarda frames como int32. La C API era el único eslabón
> angosto, así que la saturación **nunca podía correr**: el valor llegaba ya truncado.
> Pasar los `jlong` a la firma vieja habría hecho que el JNI perdiera el ensanchado.
> Ensanchada a `int64_t`. **El compilador no dijo una palabra** — es una conversión
> implícita perfectamente legal.

También `nativeLooperGetTrackWaveform` pasó a clampear al largo real del array de Java,
como su hermano `nativeGetWaveformSamples`, que lo hace y explica por qué. **No era
explotable** —su único caller aloca `FloatArray(numBins)` y pasa el mismo `numBins`— pero
dependía de que los dos números se mantuvieran en sync por convención.

**Tanda 2 — arming y telemetría** (commit `fd49bfb`), 10 funciones nuevas. **No son
wrappers delgados**: las cuatro de arming leen la posición del Transport y la pasan al
looper en una sola operación, y el comentario del JNI decía por qué —*"the anchor is read
on this thread atomically — no UI-thread jitter leaks into the trigger"*—. Esa composición
vivía arriba del todo, donde iOS no la tenía.

> [!CAUTION]
> **`armAtNextBar` y `armInFrames` mentían sobre el fallo.** `AudioLooper::armRecording` es
> `void` y no-opea si el track no tiene capacidad o el índice está fuera de rango, así que
> las dos devolvían **un trigger frame positivo con nada armado** — mientras su propio doc
> comment prometía *"-1 on failure"*. Una UI mostrando la cuenta regresiva contaba hasta un
> frame en el que no iba a pasar nada. Ahora leen el armed track de vuelta y cumplen el
> contrato. **Cambia comportamiento en Android: va al smoke.**
>
> Y el guard `track >= 0` de esa confirmación no es redundante: `getArmedTrack()` devuelve
> `-1` para "nada armado", así que armar el track `-1` **se confirmaba a sí mismo**. Lo
> agarró un test, no la lectura.

Dos decisiones de alcance: `getInputPeak` **no necesitó función nueva** (`wma_input_get_level_linear`
ya existía, el `max(L,R)` queda como composición en el JNI, igual que en `benchmark`), y
`get_dropped_events` sale del **`LooperEventDispatcher`**, no del `AudioLooper` — dos objetos
detrás de una misma sección, y `resetTelemetry()` **no** lo limpia. Hay test que lo pincha
para que nadie unifique los dos resets y pierda la distinción.

**Verificación — `test_c_api_looper.cpp`, 24 tests (691 en total).** El foco **no** es
`AudioLooper`: `looper/tests/` ya lo cubre mejor y re-afirmarlo acá sería duplicar. Lo que
no estaba cubierto es la **frontera de la C API**: el barrido de null-handle pincha los 38
defaults que se movieron del JNI (los de `1.0f` son los que un default zero-initialized
rompe sin que se note), el `int64` del loop region, y un **ida y vuelta audible** que graba
salida real del motor y la reproduce — para probar que la C API llega al *mismo*
`AudioLooper` que renderiza el callback, que es el modo de falla del `InputNode` duplicado.
**7 mutantes entre las dos tandas, 7 detectados.**

**Tres tests que fallaron por la razón equivocada y quedaron anotados en el archivo:**
el loop region **no existe en un track sin largo grabado** (`prepareTrack` reserva capacidad,
`mLengthFrames` queda en 0 y `setLoopRegion` retorna temprano); **no se puede afirmar "el
sinte se calló" mientras el loop suena**, así que va `pause`/`resume` primero; y el **mute
está smootheado** (~0.28 de ganancia después del primer bloque de 256), así que medir al
toque lee la rampa y no el mute.

**Tanda 3 — edición y análisis** (commit `e67fbc2`), 13 funciones nuevas.

> [!CAUTION]
> **Tercer problema de ancho de la categoría, en `prepareTrackBars`.** `bars * framesPerBar`
> es aritmética `int` tanto en el JNI como en `AudioLooper`, y `prepareTrack` sólo rechaza un
> largo **no positivo** — así que un `bars` que envuelve a un positivo **chico** alocaba un
> track diminuto y devolvía el número envuelto como si fuera el largo. Ahora se calcula en
> `int64` y se rechaza arriba de `INT32_MAX`.
>
> Van tres: `set_track_loop_region` (tanda 1), éste, y el `jlong` de `SetCapabilities` que
> resultó benigno. **El patrón vale como regla:** donde el JNI recibe un `jlong` o hace
> aritmética de frames, revisar el ancho en las cuatro capas.

Dos encodings de plataforma que **se quedan arriba** a propósito, siguiendo el patrón de la
tanda 1: `findTrackContentBounds` empaqueta `(first << 32) | last` porque una llamada JNI no
puede devolver dos ints —la C API devuelve dos out-params y el packing queda del lado JNI—, y
dimensionar el `jintArray` de `detectOnsets` al conteo real se queda arriba, misma familia
que el clamp del waveform. **El mask unsigned del low half no es decorativo:** sin él, un
frame con el bit alto prendido vuelve negativo.

`startRecordingWithPreRoll` y `prepareTrackBars` son **composiciones** (PreRollRing + looper,
Transport + looper), no wrappers — la clase de lógica que iOS no tenía. `setCapabilities`
baja con su contrato de "0 = dejar como está", que para `maxActiveTracks` es **load-bearing**
(`setCapabilities` clampea a `[1,16]`, así que pasar 0 dejaría el device en **un** track) y
para el budget es **redundante** (`AudioLooper` ya defaultea un 0). Las dos cosas quedan
dichas en los tests, para que nadie lea el bloque entero como uniformemente necesario.

> [!WARNING]
> **La mutación corrigió tres cosas en esta tanda, y una era un test que pasaba por la razón
> equivocada.** El del overflow usaba `bars=100000`, cuyo producto envuelve a 1.010.065.408:
> positivo, pero pide ~8 GB, así que fallaba la **allocation** y no el guard. Con
> `bars=44740` envuelve a **72.704** —chico, positivo y alocable— y ahí sí aísla el guard.
> Las otras dos: el clamp de negativos de `detect_onsets` **no se puede cubrir**
> (`TrackBuffer::detectOnsets` nunca devuelve negativo, todas sus salidas tempranas son
> `return 0`), y faltaba el test del techo de tracks. Las tres quedaron escritas en el
> archivo. **7 mutantes detectados** una vez corregidas.

Y dos tests que fallaron por razón equivocada antes de eso: `trimTrack` devuelve **false**
cuando **no** hay capacidad sobrante (el helper grababa exactamente lo reservado, así que no
había nada que trimear), y `clearTrack` **libera el buffer**, así que un track limpiado no se
puede volver a grabar sin prepararlo de nuevo.

**Tanda 4 — export/import** (commit `413f25b`), 10 funciones nuevas.

**Primer struct del header: `WmaExportOptions`.** La alternativa era una función de **diez**
argumentos, duplicada entre mix y stems, seis de ellos ints del mismo tipo — la forma exacta
en la que un caller transpone dos y nadie se queja. Un campo con nombre no se transpone en
silencio, y cinterop lo mapea a un tipo Kotlin con nombre en vez de diez posicionales. Viene
con `wma_looper_export_options_default()`, y **hay un test que verifica que esa copia a mano
coincide con los member initializers de `wm::ExportOptions`** — es el precio de que un struct
de C no pueda heredarlos. El mapeo `16/24/32 → wav::BitDepth` estaba escrito **tres veces**
en el JNI (capture, mix, stems); ahora está en uno.

> [!CAUTION]
> **Una excepción de C++ cruzaba la frontera de la C API.** `LooperExporter` dimensiona su
> mix buffer desde el largo pedido (`std::vector<float>(totalFrames * 2)`) **sin techo**, así
> que un count-in absurdo tira `std::length_error` en vez de devolver `false`. El JNI **no
> puede propagar** una excepción de C++ —sale como `abort`, no como excepción de Java— y
> cinterop no tiene noción de ellas. Las **seis** funciones de I/O de archivo quedan
> envueltas, con la misma convención que ya usaba `wma_effect_add()`.
>
> Lo encontró un test que primero decía sólo `SUCCEED()` y murió con
> `C++ exception with description "vector"`. **Un test que no afirma nada igual puede
> encontrar algo, pero sólo si uno mira por qué murió** en vez de bajarle la vara.

**Cuarto problema de ancho:** `countInBeats * framesPerBeat()` era aritmética `int` y a 24000
frames por beat desborda arriba de ~89k beats. En `int64` y clampeado.

> [!NOTE]
> **Las dos que no delegan, y por qué.** `RegisterStateListener` y `UnregisterStateListener`
> son maquinaria JNI de punta a punta: global refs, `GetObjectClass`, `GetMethodID` con firmas
> Java como `"(IF)V"`, y el probing de métodos opcionales que deja registrar un listener viejo
> sin los callbacks de QW-5 y F3.4. Nada de eso significa algo fuera de la JVM.
>
> La única pieza portable es `setSink()`, y el sink llama a Kotlin por `JNIEnv`. **iOS va a
> querer su propio callback de eventos** —un `wma_looper_set_event_callback` con un puntero a
> función de C y `user_data`— pero eso es una **superficie nueva para diseñar, no una
> migración**: no hay comportamiento existente que levantar, porque el comportamiento
> existente es "llamá a estos métodos Java". Queda como follow-up, escrito en el archivo, en
> vez de medio inventado. Es también la razón por la que
> `wma_looper_get_dropped_events()` lee el dispatcher y no el looper.

**Verificación — 57 tests en el archivo (724 en total).** El export es lo más verificable del
looper porque **escribe archivos**: se chequea que el motor pueda **importar de vuelta su
propio export**, que el count-in crezca el archivo en los frames que dice el Transport, que
24 y 32 bits den archivos más grandes que 16, que un `bit_depth` inválido caiga a 16, y que
el BPM 0 se resuelva a la tasa del Transport —leyendo el `BPM=140` que queda en el chunk
ICMT, que es la única forma de observarlo sin un lector de metadata—.

**Total de la categoría: 22 mutantes, 19 detectados.** Los 3 restantes eran equivalentes o
guards redundantes, y los dos redundantes quedaron anotados: `repeat_loops <= 0 → 1` ya lo
hace `LooperExporter` con `std::max(1, ...)` en sus dos use sites, y el budget de
`setCapabilities` ya lo defaultea `AudioLooper`. Los tests se quedan porque pinchan el
contrato observable independientemente de qué capa lo sostenga — pero decir **cuál** capa lo
sostiene evita que el próximo lector lea el bloque entero como uniformemente necesario.

### Hallazgo al contar el final — WA-2.5/2.6 tiene una cola de 15 (2026-07-27)

> [!IMPORTANT]
> **Las 10 categorías están cerradas, pero la lista de categorías tenía un agujero.** Al
> contar los 62 entry points que no delegan salieron **46 deliberados** (39 USB por D4, 5 de
> Oboe/stubs en `benchmark`, 2 del state listener) y **16 sin clasificar**. Uno es
> `nativeGetAdaptiveBufferStats`, que ya está documentado como USB. **Los otros 15 son
> superficies que la secuencia de categorías nunca nombró:**
>
> | Superficie | Entry points | ¿Existe la `wma_*`? |
> |---|---|---|
> | Routing | `GetRoutingMode`, `SetRoutingMode`, `SetParallelMix`, `SetDepthValue`, `SetFeedbackAmount` | **Sí** — §9 y §17 |
> | Backend | `SelectBackend`, `GetCurrentBackendType`, `SetUseBackendManager` | **Sí** — §16 |
> | Backend Android-only | `CreateSplitBackend`, `FallbackToOboeBackend` | revisar: probablemente no portan |
> | XY Mapping | `SetMappingConfig`, `ClearMappingConfig` | **Sí** — §17 |
> | Captura de logs | `DrainCapturedLogs`, `GetLogCaptureDropped`, `SetLogCaptureEnabled` | **No** — decidir si porta |
>
> **12 de los 15 ya tienen contraparte**, así que es migración mecánica del tipo más barato
> —la tanda 1 del looper— más 3 de log capture para decidir y 2 de backend para revisar.
>
> **Es el mismo error, una capa más arriba.** Todo el método insistía en que las filas del
> script no son las categorías… y la lista de categorías se armó igual mirando esas filas.
> Estos 15 vivían en la fila "Otros" (17/27) y nadie los enumeró hasta contar el final.
> **Moraleja para la próxima descomposición: enumerar el complemento, no la lista.** La
> pregunta que lo destapa no es "¿cerré todas las categorías?" sino "¿qué queda afuera, y
> está afuera a propósito?".


### Nota de cierre — la cola de 15, y con eso WA-2.5/2.6 (2026-07-27)

**Cerrada. Delegación 237/278 por el script, 240/289 real.** Los 49 que no delegan se
reparten en cuatro baldes, **todos deliberados y todos con el porqué escrito en el código**:
40 USB (D4), 5 de `benchmark` (Oboe/stubs), 2 listeners del looper y 2 de backend. **Cero
sin clasificar** — que era exactamente lo que faltaba para poder decir que está cerrada.

**Primero se enumeró el complemento, y esta vez cerró.** Antes de tocar nada se listaron
los 54 entry points que no delegaban dentro del alcance del script y los 11 que viven fuera
de él (`jni_benchmark.cpp`, `jni_usb.cpp`): 62 en total, que descomponen exacto en 40 + 5 +
2 + 15. **No había un ítem 16 escondido.** También apareció que el "290 real" del conteo
anterior era uno de más: el `grep` crudo contaba una línea que no es un entry point. Son
**289**.

**Migración mecánica: 10, no 12.** La tabla de la nota anterior decía "12 de los 15 ya
tienen contraparte" contando como tales los 2 de backend que estaban marcados "a revisar".
Los que tenían `wma_*` eran 10: routing 4 (§9), XY mapping 2 + `SetDepthValue` (§17, no §9
— las categorías desparraman una vez más) y backend 3 (§16). Los 10 se diffearon en las dos
direcciones: **transcripciones fieles, cero divergencia.** Es la primera categoría de la
serie sin un solo bug de divergencia — y también la primera cuyo trabajo real fue escribir
los tests, no arreglar el código.

**El ancho de los tipos no tenía nada que buscar acá, y eso también se verifica.** Las cuatro
capas son `Int`/`Float`/`Boolean` → `jint`/`jfloat`/`jboolean` → `int`/`float`/`bool` →
`int`/`float`/`bool`. Ni un `jlong`, ni aritmética de frames. Los cuatro casos del looper
salieron todos de esas dos señales; su ausencia es la razón por la que no hay un quinto.

> [!CAUTION]
> **`setDepthValue` es un dead store en las cuatro capas.** Kotlin hace `coerceIn(0f,1f)`,
> el JNI clampea, la C API clampea, `EffectChain::setDepthValue` guarda en
> `std::atomic<float> mDepthValue`… y **nadie lee `mDepthValue`**: un grep sobre el motor
> entero encuentra la declaración, el único store, y nada más. No tiene getter y no llega al
> render path. Mientras tanto su doc comment en Kotlin promete *"Set depth axis value.
> Lock-free real-time path."*
>
> El eje depth **sí** funciona, por otro lado: es el eje 2 del mapping, manejado por
> `applyAutomation`. `setDepthValue` es un escalar huérfano al lado.
>
> **Se migró fiel en vez de arreglarlo, a propósito.** Cablear `mDepthValue` al audio
> inventaría comportamiento que ningún caller escuchó nunca, y eso es una decisión de
> producto, no una migración. **Va a la lista del smoke**: si un slider de depth en NoisyPad
> llama sólo a `setDepthValue`, no hace nada — y no es una regresión de esta serie, es así
> desde siempre.

> [!CAUTION]
> **`wma_select_backend` miente por omisión, y iOS es donde se nota.**
> `BackendManager::selectBackend(LIBUSB)` sin backend USB **no falla**: reescribe el tipo a
> OBOE, apunta al backend de sistema y devuelve `true`. En iOS `createUsbAudioBackend()`
> siempre devuelve null (D4), así que un caller que pide USB en iOS **recibe éxito** y sólo
> `wma_get_backend_type()` revela que no lo consiguió. El valor de retorno no significa
> "conseguiste lo que pediste"; la query es la fuente de verdad. Está pinchado con un test
> que lo dice en el nombre.
>
> Lo encontró un test escrito al revés —afirmaba que seleccionar un backend inexistente
> falla— y el fallo importó más que el test. **Van cuatro categorías donde el test que
> falló por la razón equivocada valía más que el que pasó.**

**Los 3 de captura de logs eran superficie nueva, no migración.** El buffer
(`LogCaptureBuffer`, ring de 4000 líneas) ya era portable y ya tenía tests propios; lo que
no existía era la forma en C. `setEnabled` y `droppedCount` son directos. `drain` no:
**es destructivo**, así que la forma obvia en C —buffer del caller + capacidad— convierte un
buffer chico en líneas descartadas en silencio que ya no están en el ring. Se resolvió con
`WmaLogBatch`, un handle opaco que entrega las líneas enteras y que el caller libera; es
además la forma que el JNI necesita, porque `NewObjectArray` pide el conteo por adelantado.
**No se expuso `clear()`**, aunque existe en el buffer: el JNI nunca lo expuso, y agregar
superficie sin caller es cómo un header termina lleno de funciones que nadie sabe explicar.

> [!NOTE]
> **Una excepción más que cruzaba la frontera, misma familia que la tanda 4 del looper.**
> `drain()` devuelve un `std::vector<std::string>` por valor: un `bad_alloc` ahí salía del
> JNI como `abort`, no como excepción de Java. Queda envuelta en `wma_log_capture_drain()`.
> Y de paso el `FindClass` del JNI ahora pasa **antes** del drain: fallar después de drenar
> tiraba las líneas a la basura.

**Verificación — 26 tests nuevos (749 en total)**, en `test_c_api_routing.cpp` y
`test_c_api_logcapture.cpp`. Esta superficie tenía **cobertura cero** antes: una sola llamada
a `wma_select_backend` dentro de `CApiFixture` era todo. De los cuatro guards que se movieron
del JNI a la C API, **dos son load-bearing y dos son decoración**, y el archivo dice cuál es
cuál: el rango 0..5 de routing lo es (`EffectChain::setRoutingMode` guarda lo que le den) y el
`isfinite` de las cotas del mapping también (nada lo re-chequea aguas abajo, y una cota NaN se
vuelve un parámetro NaN); el guard de eje y los de curve/polarity son redundantes con el
`default:` de `getMappingForAxis` y de `applyMappingCurve`.

**11 mutantes, 9 detectados.** Los 2 restantes son justamente los que el archivo predice: el
clamp de `setDepthValue` (no se puede observar un dead store) y el guard de eje (redundante).
**La mutación no verificó sólo los tests: verificó las etiquetas.**

**Y otros dos tests fallaron primero por la razón equivocada, los dos por el harness.** El de
mapping usaba el rango [0.25, 0.75] suponiendo que un parámetro de efecto es normalizado: el
parámetro 0 de un filtro es cutoff en Hz y `setCutoff` clampea a [20, 20000], así que los tres
valores mapeados llegaban como 20. **Un rango de mapping tiene que vivir dentro del rango del
parámetro destino** — conviene saberlo antes de cablear un pad XY a algo. El otro es el de
backend de arriba.

**Ojo con leer la tabla del script como progreso por categoría.** Migrar la cola movió filas
que nadie tocó: `Mode transitions` pasó de 10/12 a 12/12 y `Benchmark / diagnostics` de 2/6 a
5/6, porque `SetRoutingMode` lleva `mode` en el nombre y `DrainCapturedLogs` entra por `log`.
Es el mismo desparramo de keywords de siempre, ahora visible en el signo contrario.


### Nota de cierre — WA-5.5, el módulo del harness y su gate (2026-07-27)

**Existe `:harness` y compila en las dos plataformas.** APK de Android y
`HarnessKit.framework` de iOS (245 MB debug, **250 símbolos `wma_*` adentro**, con
`MainViewController` exportado). Es el esqueleto: transporte y lectura de estado, que es lo
mínimo que prueba que la cadena entera está viva —Compose → `commonMain` → bridge → C API →
C++— en Android e iOS. Los otros seis controles van encima de este mismo andamio.

**El harness vive entero en `commonMain`**, y eso salió gratis: `AudioEngineFactory.create()`
no pide `Context` ni nada de plataforma. Los shells son dos archivos de ~10 líneas
(`MainActivity`, `MainViewController`). Es también la razón por la que el harness ejercita
**la misma superficie que consume un cliente KMP**, en vez de una parecida.

**Se confirmó lo del XCFramework, y por el mejor camino: funcionando.** El link del framework
del harness disparó `:audio:buildIosNativeLib` solo, porque el convention plugin ya engancha
`cinteropWatermelonAudio*` a esa task. **No se tocó nada del lado de WA-4.1** — el harness
produce su propio framework y el XCFramework sigue siendo la salida para un consumidor Swift
no-KMP, exactamente como decía la propuesta.

**Dos cosas que la propuesta no había anticipado:**

1. **AGP y KGP ya están en el classpath del build** por `includeBuild("build-logic")`, así que
   el harness los aplica **sin versión** — Gradle rechaza que se les vuelva a declarar una.
   Compose sí lleva versión, porque **no** está en ese classpath: eso es justamente lo que lo
   mantiene fuera del alcance de `:audio`.
2. **Compose Multiplatform 1.11.1 arrastra `lifecycle-runtime-compose 2.11.0`, que exige
   compilar contra API 37**, y el repo está en 36. **`:harness` tiene su propio `compileSdk`
   (37) en vez de mover el de la librería.** Subir el de `:audio` es una decisión de la
   librería, a tomar por sus motivos — no un efecto colateral de agregar una app de prueba.
   Que el harness no arrastre la configuración de lo que se publica **es el punto entero** de
   tenerlo aparte.

#### El gate — `scripts/check-no-ui-in-library.sh`, noveno comando

Tres assertions: (1) toda task de publicación pertenece a `:audio`; (2) **el classpath
resuelto de `:audio` no tiene una sola coordenada de Compose**; (3) `:audio` no depende de
`:harness`.

**La 2 es la única que agarra el modo de falla realista.** Nadie va a publicar el harness por
accidente — los workflows dicen `:audio:publishAll...`, path-qualified, y el harness ni
siquiera aplica `maven-publish`. Lo que pasa de verdad es que alguien le agrega Compose a
`:audio` "para un helper de preview", y las otras dos no ven eso.

**El catálogo de versiones sí tiene entradas de Compose y no es una violación.** El catálogo
declara versiones *disponibles*, no dependencias efectivas. Confundirlas lleva a gimnasia
inútil; lo que decide es el classpath, que es lo que el gate mide.

> [!TIP]
> **Se mutó el gate tres veces, y las tres fallas enseñaron algo distinto.**
> - **`maven-publish` en `:harness`** → falla el check 1. Limpio.
> - **Compose en `:audio`** → falla el check 2, nombrando la coordenada culpable. Es el
>   mutante que justifica el script.
> - **`:audio → :harness`** → falla… **por la razón equivocada**. Como `:harness` depende de
>   `:audio`, esa arista es un **ciclo**, y Gradle muere en la configuración antes de que el
>   check 3 llegue a correr: lo agarra el check 1. **El check 3 hoy no puede dispararse**, y
>   eso está escrito en el script en vez de dejar creer que él sostiene esa invariante — hoy
>   la sostiene el grafo de tareas. Se queda porque deja de ser cierto en cuanto exista un
>   segundo módulo de UI que no dependa de `:audio` (un design system, por ejemplo).
>
> **Y el tercer mutante destapó dos bugs míos en el script, no en el build.** El primero:
> mandaba `stderr` a `/dev/null`, así que reportaba "no se encontró ninguna task de
> publicación" mientras Gradle decía, textual, *"Circular dependency between the following
> tasks"* — el gate acertaba y el mensaje mentía. El segundo, al arreglar el primero: guardaba
> el error en una variable global seteada dentro de `$( … | grep | sort )`, o sea **en un
> subshell**, así que nunca volvía al padre y seguía imprimiendo "(sin detalle)". Va a archivo.
> **Van tres veces en dos sesiones que el mutante apunta al harness y no a lo que se probaba.**

**El gate pasa a 10 comandos:** los 8 de siempre, más `check-no-ui-in-library.sh` y
`build-harness.sh`. El segundo no es sólo "que compile" — **verifica que el framework traiga
el motor adentro**, que es una afirmación distinta de que linkeó. Es el mismo modo de falla
que `ci.yml` ya cubría para el XCFramework de WA-4.1, con el mismo piso de 100 símbolos. Y sin
este comando el harness se pudre en silencio: nada más lo compila.

> [!TIP]
> **`build-harness.sh` también se mutó, y ahí las mutaciones enseñaron más que el resultado.**
>
> **Los dos primeros mutantes no se detectaron, y la culpa era del método.** Se habían mutado
> las **salidas** —vaciar el binario del framework, renombrar el símbolo dentro del Mach-O— y
> Gradle detecta que el output cambió y **re-linkea antes de que el check corra**. La mutación
> se deshacía sola. A las salidas de un build no se las muta: se mutan las **entradas**.
>
> **Y con las entradas apareció que una de las dos assertions no afirmaba lo que decía.**
> Renombrar `fun MainViewController` a otra cosa **seguía pasando**, porque Kotlin/Native
> nombra la clase ObjC por el **archivo** (`MainViewController.kt` →
> `HarnessKitMainViewControllerKt`), no por la función: el check estaba afirmando "el archivo
> existe", no "el punto de entrada existe". Ahora afirma contra el **header generado**, con
> tipo de retorno incluido (`+ (UIViewController *)MainViewController`), que es además
> exactamente lo que compila Swift. Con eso, el mutante falla y encima lista qué declara el
> header de verdad.
>
> **El check de símbolos sí puede dispararse, y por la razón real.** Comentar
> `staticLibraries` en el `.def` produce un framework que **linkea perfecto con 0 símbolos
> `wma_*`**: el linker no dice nada. Ese es exactamente el fallo silencioso para el que existe
> el check — a diferencia del check 3 del otro script, éste no es un backstop teórico.
>
> **Un tercer bug de bash, de la misma familia que el del subshell:** `nm … | grep -q` bajo
> `set -o pipefail` da distinto de cero **aunque encuentre** el símbolo, porque `grep -q`
> corta al primer match y `nm` se come un SIGPIPE. Falla del lado seguro, pero falla igual.
> `grep -c` lee toda la entrada y no tiene el problema.

#### El shell de Xcode — hecho, y **la app corre en el simulador**

`harness/iosApp/` con `.pbxproj` escrito a mano (no hay xcodegen en la máquina). Compila,
instala, arranca y **Compose dibuja el estado real del motor**. Es la primera vez en todo el
programa que la librería se ve corriendo en iOS fuera de un test.

Tres cosas que la propuesta no había previsto, y las tres se resolvieron del mismo lado —el de
Xcode— para no tocar la configuración de la librería:

1. **`EXCLUDED_ARCHS[sdk=iphonesimulator*] = x86_64`.** Xcode pide `ios_x64` y el repo declara
   sólo `iosSimulatorArm64`. La otra salida que ofrece el error es agregarle el target a
   Gradle: eso le cambiaría el set de targets a **lo que se publica**, por un harness.
2. **Sin firma para el simulador** (`CODE_SIGNING_ALLOWED = NO`), que es donde vive la primera
   mitad de WA-4.3. El device (G2) necesita team real.
3. **`NSMicrophoneUsageDescription`** desde el primer commit, como estaba planeado.

> [!CAUTION]
> **`CADisableMinimumFrameDurationOnPhone` no es opcional, y esto costó la tarde.** Compose
> Multiplatform corre `PlistSanityCheck` al arrancar y **aborta el proceso** si esa clave falta
> o es `false`. La app moría con **SIGABRT antes de dibujar un pixel**, y el stack no menciona
> el motor de audio por ningún lado.
>
> **Ningún gate lo agarraba.** Gradle compilaba, el framework tenía sus 250 símbolos,
> `xcodebuild` daba `BUILD SUCCEEDED`. Los ocho comandos en verde con la app muerta. Lo único
> que lo encuentra es **lanzarla y preguntar si sigue viva** — y el mensaje real de la
> excepción hubo que sacarlo del binario a mano, porque las strings de Kotlin son UTF-16 y
> `strings` no las ve.
>
> Por eso `build-harness.sh` ahora **lanza la app y verifica que sobreviva 3 segundos**. Está
> mutado: sacar la clave da `xcodebuild: OK` seguido de `FAIL — arrancó y MURIÓ`.

> [!WARNING]
> **`grep -q` en un pipeline bajo `set -o pipefail` volvió a morder, en el mismo archivo.**
> Ya estaba arreglado quince líneas más arriba para `nm`, y el chequeo de arranque salió
> escrito igual (`simctl spawn … launchctl list | grep -q`): reportaba que la app había muerto
> **con la app corriendo**. Ahora usa `ps -p "$pid"`, que además pregunta exactamente lo que
> importa —¿sigue vivo *este* proceso?— en vez de si algún listado menciona el bundle.
> **Regla para el repo: `grep -q` no va en un pipeline bajo pipefail.** Van cuatro apariciones
> de esta familia de bugs de bash en dos sesiones, todas en gates, ninguna en el build.

**No se pudo tocar "start".** El panel del simulador no levanta: el MCP reporta que Xcode no
está seleccionado y, efectivamente, **falta el symlink `/var/db/xcode_select_link`** —
`xcode-select -p` responde bien igual porque cae a un default. El fix necesita `sudo`
(`sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`). Sin eso, manejar la UI
necesita una persona.

**Vale la pena saber qué queda sin verificar, porque es preciso:** `CinteropSmokeTest`
**deliberadamente no arranca el motor** —lo dice en su propio doc comment: `wma_engine_start()`
abre un stream de CoreAudio y volvería flaky el test—. Así que *nada* ha ejecutado todavía
`start()` en iOS. Eso es el control 1, y ahora hay dónde apretarlo.

#### Control 1 de 7 — monitor de entrada, y lo que hizo falta antes

> [!IMPORTANT]
> **El control no se podía escribir: el camino de entrada no llegaba a `commonMain`.**
> `AudioEngine` no tiene un solo método de input y `IAudioNativeBridge` tampoco tenía ninguno.
> La superficie existía **entera** en la C API (§12, 21 funciones) y **entera** en
> `AudioNativeBridge` (Android, 21/21 delegando desde WA-2.6) — y el medio estaba vacío. iOS no
> tenía forma de tocar la entrada desde Kotlin **aunque `CoreAudioBackend` capture desde la
> etapa 2 del input path**. Estaba anotado como "ticket aparte"; resultó ser el camino crítico.

Lo que se agregó, de abajo hacia arriba:

1. **`IInputBridge`** — §12 como contrato propio, y `IAudioNativeBridge` pasa a extenderlo.
   Es el mismo patrón que ya tenían `IEffectStateProvider` / `IEffectStateWriter`, y no es
   cosmético: con los 21 métodos adentro de una interfaz de más de cien, escribir un fake para
   testear la lógica de entrada obliga a implementar los cien. **Esa fricción es por la que la
   lógica se queda sin test.** Partida, el fake son 21 métodos y el archivo de tests existe.
2. **`IosAudioBridge`** implementa los 21 sobre cinterop — **el primer usuario real de
   `wma_input_*` desde Kotlin en iOS**. Android sólo necesitó `override` en 21 firmas, porque
   los nombres se eligieron para eso.
3. **`InputSource` / `InputMetering`** en el dominio. El snapshot de 7 valores es un tipo y no
   siete getters porque leídos de a uno **no son coherentes entre sí**: el thread de audio
   corre entre lectura y lectura, así que un medidor podría mostrar el pico de un bloque y el
   flag de clipping de otro. Aparte de eso, un cruce de frontera por frame en vez de siete.
4. **`AudioInput` + `AudioInputFactory`** — la puerta pública, espejando `IEffectManager` /
   `EffectManagerFactory`.

> [!CAUTION]
> **"No hay medición" y "medición en cero" no son lo mismo, y la mitad de los tests nuevos
> existen para que esa distinción no se pierda.** La C API deja el buffer intacto cuando no hay
> nodo de entrada —con un comentario que dice por qué: para que nadie lea ceros como si alguien
> los hubiera medido—. Si esa intención se aplana en cualquiera de las capas de arriba, el
> síntoma es **un medidor plano y convincente**: exactamente el modo de falla más caro para un
> harness cuyo trabajo es contestar "¿esto captura?". Por eso `metering()` devuelve `null`, el
> flujo **no emite** mientras no haya nada que medir, y la barra dibuja el "no sé" con otro
> color que el silencio.

**`setNoiseGateThresholdDb` es función y no propiedad** porque **no se puede leer**: no hay
getter en ninguna capa, ni en la C API. Un `var` tendría que inventar el valor de vuelta o
cachear el último escrito y mentir en cuanto algo más lo cambie.

**Verificación — 14 tests nuevos (101 en el simulador, 64 en JVM).** 4 mutantes, 4 detectados:
rellenar el snapshot corto con ceros, leer los flags como `== 1f` en vez de `!= 0f` (un backend
que escriba `2.0` apagaría el indicador de clipping justo cuando importa), devolver `SILENT` en
vez de `null`, y sacar el clamp del volumen de monitoreo.

**El medidor es de nivel y no un booleano a propósito.** "El stream arrancó" y "está entrando
señal" son dos afirmaciones distintas, y la primera se cumple perfectamente con silencio: un
indicador de encendido habría dado verde durante todo el desarrollo del input path sin probar
nada. Lo que prueba algo es una barra que se mueve cuando hablás. Y va escalada **en dB**,
porque en lineal todo lo que uno mide hablándole a un teléfono queda apretado contra el cero —
indistinguible de "no captura", que es justo lo que hay que distinguir.

**Sigue sin apretarse.** El control está en pantalla en el simulador; tocar "capturar" necesita
el panel (el fix con `sudo` de arriba) o una persona. **La barra moviéndose es la respuesta que
falta**, y ahora hay dónde leerla.

#### Control 2 de 7 — pad XY y oscilador

El único control que ejercita el **camino de tiempo real**. `setXY` corre una vez por frame de
gesto y es la única llamada del programa con una nota explícita sobre su costo por plataforma:
Android tiene un coalescer que junta updates para amortizar JNI, iOS no lo tiene porque
cinterop no cobra lo mismo, y el comentario en `IosAudioBridge` dice textual que si una
medición muestra lo contrario, el lugar del coalescer es ahí. **Este pad es cómo se hace esa
medición.**

**Sin slider de depth, y no por olvido:** `setDepthValue` no existe en `commonMain` en ninguna
forma. Ponerlo habría requerido subir al bridge común una función que ya sabemos que es un dead
store en las cuatro capas — dejar escrito un control muerto en la API multiplataforma. El ítem
11 del smoke se mira desde NoisyPad en Android, que es donde el caller existe.


### El harness apretó "capturar" y encontró dos bugs de producción (2026-07-27)

> [!CAUTION]
> **En el primer tap. Y ninguno de los dos lo veía ningún test.**
>
> El resultado visible fue `start() devolvió false` y —lo más raro—
> **`lifecycle: RUNNING` con `stream: —`**: el motor decía estar corriendo sin un stream
> abierto. Los logs del motor lo explican entero:
>
> ```
> WMA_AUDIT: [START] calling manager.start()...
> BackendManager: No backend selected
> AudioEngine: Failed to start via BackendManager: Not initialized
> WMA_AUDIT: [START] manager.start() FAILED: Not initialized
> AudioEngine: Invalid state transition: 2 -> 0
> ```

**Bug 1 — en iOS nadie selecciona un backend, así que el motor NUNCA puede abrir un stream.**

`mUseBackendManager` es `false` en Android (camino Oboe directo, el que shippea) y **`true` en
todo lo demás**, con un comentario que dice por qué: fuera de Android ese camino directo no
existe y `BackendManager` es la única forma de abrir un stream. Pero `selectBackend()` **no lo
llama nadie en el arranque**: sus dos únicos callers son `wma_select_backend()` —la entrada de
la C API— y `AudioEngineImpl.setAudioBackend()`, que sólo corre si el consumidor lo pide
explícitamente. Con `mActiveBackend` en null, `manager.start()` falla siempre.

**Por qué nunca se vio:** en Android el flag es `false` y el camino directo no pasa por acá. Y
en la suite de host **`CApiFixture` llama `wma_select_backend(1)` a mano** — o sea que los
tests seleccionan el backend y producción en iOS no. El fixture estaba compensando, sin querer,
exactamente el paso que falta.

**Bug 2 — un start fallido deja el motor mintiendo que está RUNNING, para siempre.**

`AudioEngine::start()` transiciona a `Running` **antes** de llamar a `manager.start()`. Cuando
eso falla intenta volver a `Stopped`, y la tabla de transiciones lo rechaza: desde `Running`
la única salida válida es `Stopping`. Resultado: la transición se descarta, el motor queda en
`Running`, y `isRunning` devuelve `true` sobre un motor que no tiene stream. **La UI mostró
exactamente eso**, y es lo que hizo obvio que había un segundo problema debajo del primero.

Es la misma familia que los hallazgos de WA-2.6: un valor que se reporta bien mientras la
realidad es otra. Acá el arreglo correcto es no transicionar a `Running` hasta que el backend
haya arrancado de verdad — que el estado siga a la realidad, no al revés.

**Qué NO es:** no es el permiso de micrófono. No apareció diálogo porque nunca se llegó a
pedirlo: el stream de salida ni siquiera abrió. La pregunta original —**¿el input path de iOS
captura?**— sigue sin contestar, y ahora se sabe qué la estaba tapando.

> [!TIP]
> **Esto es exactamente para lo que se construyó el harness, y lo encontró en el primer tap.**
> Los diez comandos del gate estaban en verde: 749 tests C++, 101 de simulador, 64 JVM, ambos
> slices linkeando, el framework con sus 250 símbolos. Ninguno de esos ejercita
> `wma_engine_start()` en iOS — `CinteropSmokeTest` **deliberadamente** no lo hace, y lo dice en
> su doc comment. Verde completo y el motor no podía abrir un stream.
>
> También vale cómo se encontró: el filtro de logs por proceso no mostraba nada del motor,
> porque `Logger.cpp` en Apple escribe al **subsystem** `com.watermellonstudios.audio`. Con el
> predicado correcto (`--info --debug --predicate 'subsystem == "…"'`) los logs contaban la
> historia completa. Vale anotarlo: en iOS, para ver el motor hay que filtrar por subsystem,
> no por proceso.

#### Arreglados (2026-07-27) — y arreglarlos destapó tres más

**Bug 1** — `AudioEngine::start()` selecciona el backend de sistema cuando nadie eligió. El
guard es `== NONE`, así que una elección explícita sigue mandando. **No va dentro de
`BackendManager::start()`**: ese método tiene tomado `mMutex` y `selectBackend()` toma el mismo
mutex no recursivo — juntos, deadlock.

**Bug 2** — el rollback pasa por `Stopping`, que es la única salida válida desde `Running`, y
además cancela el fade. `cancel()` **no alcanza**: mata el worker del stop-fade pero no toca
`mFadeRemainingFrames`, que es lo que lee `isFading()`; el reset va con un fade de largo cero,
el mismo idiom que ya usaba el motor.

**6 tests de caracterización** en `test_c_api_start_without_select.cpp`, que **no usan
`startAt()`** — usarlo los haría pasar por el `wma_select_backend(1)` del fixture, que es justo
lo que tapaba el bug. **4 mutantes, 3 detectados**; el cuarto destapó que uno de mis tests
mentía en el nombre (ver el archivo: el default no se puede distinguir de incondicional en el
host, y por qué el guard igual es load-bearing).

> [!CAUTION]
> **Bug 3 — un cambio en C++ no llegaba a iOS, y el gate daba OK igual.**
> Se encontró verificando el fix: el `.a` recién compilado **tenía** el símbolo nuevo y el
> framework, 24 minutos más viejo, **no**. El convention plugin enganchaba cinterop al `.a` con
> `dependsOn`, que **sólo ordena**: no declara que el contenido del `.a` importe. Sin
> `inputs.files`, cinterop quedaba UP-TO-DATE, el klib seguía con el archivo viejo embebido
> (`staticLibraries` del `.def`) y el framework no se re-linkeaba. **Toda verificación de C++
> en iOS —tests de simulador incluidos— podía estar mirando binarios viejos.** Arreglado
> declarando el `.a` como input.

> [!CAUTION]
> **Bug 4 — el armado del grafo de salida no estaba bajo `@try`, y una NSException mataba el
> proceso.** La rama de captura tenía guarda desde que se escribió el input path; la de salida
> nunca, porque **nada había llegado hasta ahí en iOS**. `connect:` tiró `-10868` y el proceso
> murió con SIGABRT en pleno tap. Ahora devuelve `ERROR_STREAM_FAILED`, que es lo que el
> harness puede mostrar.

> [!CAUTION]
> **Bug 5 — el formato del grafo era interleaveado y AVAudioEngine lo rechaza.** Los nodos de
> AVAudioEngine se conectan en float **deinterleaveado**. "Qué rama del ABL toma el OS" estaba
> anotado como pregunta de WA-4.3 para contestar en device; la contestó el simulador y la
> respuesta es que **no toma ninguna**: falla al conectar, antes de renderizar un bloque. El
> render block ya tenía la rama planar escrita y con scratch pre-alocado. **Lo que esto NO
> verifica es cómo suena** — eso necesita oídos y, para latencia, un device.

> [!TIP]
> **Con los cinco, el motor abre un stream real en iOS por primera vez:**
> `StreamInfo(sampleRate=48000, bufferSizeInFrames=256, channelCount=2, isLowLatency=true)`,
> 480 frames por buffer y 10.10 ms de latencia de salida reportada. Y el monitor de entrada
> **mide**: `L -120.0 dB · R -120.0 dB`, o sea silencio medido — distinto de "sin medición",
> que es exactamente la distinción para la que se diseñó.

**Lo que queda abierto, con el log que lo dice:** el stream de salida abre con
`Capture: off`, y cuando `wma_input_start` pide captura el backend responde *"Full duplex
requested while running without a capture stream — takes effect on the next start()"*. O sea
que **difiere en vez de reabrir**, aunque `wma_input_start` llame a `requestCapture` con
`allowRestart=true`. El render lo confirma: `inputData=0x0` en cada callback. **Ese es el
último eslabón entre "el medidor mide" y "el medidor se mueve", y es la próxima sesión.**


### El sexto bug — por qué difería en vez de reabrir (2026-07-27)

> [!CAUTION]
> **`CoreAudioBackend::start()` tiraba los valores negociados apenas los medía.**
>
> `openEngineLocked()` publica el stream info real —tasa, frames, latencia, y si la captura
> quedó viva— y termina con `mStreamInfoValid = true`. Cinco líneas después, ya de vuelta en
> `start()`, había un `mStreamInfoValid.store(false)`. El cache moría en el mismo start que
> lo llenaba.
>
> Con el cache inválido, `getStreamInfo()` cae a su rama de "todavía no abrí", que devuelve
> **el pedido**. Y `isFullDuplex` viaja en ese mismo struct, valiendo ahí `mFullDuplexRequested`.
> De ahí sale el comportamiento entero:
>
> ```
> requestCapture(INPUT_NODE, want=true, allowRestart=true)
>   → mActiveBackend->setFullDuplexEnabled(true)   // el pedido queda en true
>   → live = getStreamInfo().isFullDuplex          // lee EL PEDIDO → true
>   → if (live == effective) return live;          // "ya hay captura" → sale
> ```
>
> El `allowRestart=true` nunca se llegaba a mirar: la función se iba tres líneas antes,
> convencida de que la captura ya estaba viva sobre un stream que no tenía ninguna. No era
> que el permiso de reabrir no se ejerciera — era que nadie llegaba a pedirlo.

**Cómo se confirmó antes de tocar nada.** Los dos números que el motor imprime en el mismo
start, con 0.3 ms de diferencia:

```
CoreAudioBackend:   Frames/buffer:    480          ← lo que openEngineLocked() midió
CoreAudioBackend:   Output latency:   10.10 ms
AudioEngine:        Output latency:   0.0 ms       ← lo que getStreamInfo() devolvió
```

Y la UI mostrando `bufferSizeInFrames=256, latencyMillis=0.0` — que es, literalmente, la rama
del pedido: `mRequestedBufferSize` y un `outputLatencyMs = 0.0f` hardcodeado. Después del
arreglo la UI muestra `480` y `10.100000381469727`, iguales al log.

**El arreglo** es invalidar **antes** de abrir, no después: el cache viejo muere cuando empieza
el open nuevo, y lo que `openEngineLocked()` publica sobrevive. Con eso el reopen **se dispara
por primera vez** — `BackendManager: Reopening the stream to add a capture path`, una línea que
no había aparecido nunca.

#### Y detrás apareció un bloqueo que no es del motor

El reopen llega hasta `[AVAudioSession setActive:YES]` con `playAndRecord` y **se cuelga ahí**,
en el thread principal:

```
wma_input_start → requestCapture → BackendManager::start → CoreAudioBackend::openEngineLocked
  → -[AVAudioSession privateSetActive:withOptions:error:core:]
    → AQMEIO_HAL::HandleDefaultDeviceChange → SelectDevice → DeviceCreateIOProcID
      → HALC_ProxyIOContext::_TellServerAboutStreamUsage → HALC_ProxyObject::SetPropertyData
```

1581 de 1584 muestras en el mismo frame. **No es del reopen, y hay experimento que lo prueba:**
forzando `wantCapture = true` en el *primer* open —sin stop/start de por medio, sin cambio de
categoría sobre una sesión viva— pasa lo mismo, y CoreAudio lo dice con todas las letras antes
de matar el proceso:

```
HALC_ProxyObject::SetPropertyData ('guse','inpt'): got an error from the server, 0x10004003
HALC_ProxyObject::SetPropertyData ('guse','outp'): got an error from the server, 0x10004003
Initialize: RPC timeout. Apparently deadlocked. Aborting now.
```

Dos RPC al servidor de audio del host que expiran a los 30 s exactos. Descartados en el camino,
cada uno con su prueba: **no es carrera con el teardown** (500 ms de espera entre `stop()` y
`start()` no cambian nada, y el stack queda idéntico); **no es el permiso de micrófono**
(concedido con `simctl privacy grant microphone`, mismo cuelgue); **no es la sesión activa al
cambiar de categoría** (`closeEngineLocked()` ya hacía `setActive:NO`); y **no es que falte
`Simulator.app`** (con la GUI arriba, igual). `tccd` no registra **nada** en todo el episodio:
el pedido de micrófono no llega siquiera a macOS.

**Conclusión de ese momento —que resultó EQUIVOCADA en el punto que importaba—:** se dio por
sentado que el puente de audio del simulador no respondía y que la pregunta pasaba a device.

> [!CAUTION]
> **No era del simulador: era el main thread.** Lo que todos los descartes tenían en común, y
> que no se vio hasta hacer el reopen asincrónico, es que **todas las pruebas activaban
> `playAndRecord` desde el thread principal**. El stack lo decía y se leyó como ruido: el
> `setActive:` despacha una `DeviceAggregateNotification` que normalmente corre en la serial
> queue `DefaultDeviceAggregate`, y ejecutarla re-entrante desde el main thread es lo que la
> trababa. Movida a un thread propio, **la misma llamada abre la captura sin chistar**.
>
> El experimento de `wantCapture=true` en el primer open era correcto como experimento y su
> conclusión era la equivocada: probaba que el reopen no tenía la culpa, no que la tuviera el
> simulador. La variable que quedaba constante era el thread.

> [!TIP]
> **RESUELTO, y resolverlo contestó la pregunta abierta más grande del programa.** Ver la nota
> "el reopen dejó de correr en el thread del llamador" más abajo: `wma_input_start()` ya no
> bloquea, y con eso **el input path de iOS captura de verdad** — `inputData` dejó de ser
> `0x0` y `inputPeak` trae señal real del micrófono.

> [!NOTE]
> **Este arreglo no tiene test automático, y el intento de escribirlo es la parte que enseña.**
> `CoreAudioBackend.mm` sólo se compila en el build de iOS (`ios/CMakeLists.txt`), así que la
> suite C++ de host no lo ve. Se escribió entonces un test de simulador que arrancaba el motor
> y afirmaba `latency_ms > 0` —la latencia es el discriminador correcto: la rama del pedido la
> hardcodea en `0.0f`, y el buffer size podría coincidir de casualidad—. **Dio verde sin afirmar
> nada:** `wma_engine_start()` falla en 8 ms dentro del binario de test de Kotlin/Native, que no
> es una app con bundle. Es exactamente la flakiness que `CinteropSmokeTest` declara evitar en su
> doc comment, confirmada. Se borró en vez de dejarlo: un test que siempre pasa infla la cuenta
> del gate y le saca significado. **El gate de este arreglo es la corrida del harness**, con los
> números de arriba.


### El reopen dejó de correr en el thread del llamador — y ahí SÍ capturó (2026-07-27)

> [!TIP]
> **`inputData=0x10ce38000` · `inputPeak=0.21724` · `Capture: active`.**
> Cero callbacks con `inputData=0x0` después del reopen. **El input path de iOS captura de
> verdad**, que era la pregunta abierta más grande del programa y la razón por la que existe
> el harness.

**El cambio pedido era de seguridad, no de funcionalidad**, y terminó siendo las dos cosas.
`wma_input_start()` hacía un stop + start sincrónico del stream entero **en el thread del que
llama** — el main thread en cualquier app con UI. Ahora agenda el reopen en un worker y vuelve
enseguida.

**Lo que devuelve deja de ser un bool disfrazado.** Un reopen agendado no está vivo ni muerto,
así que `requestCapture()` pasa a devolver `CaptureOutcome{LIVE, NOT_LIVE, PENDING}`: colapsar
PENDING en NOT_LIVE haría indistinguible "todavía abriendo" de "el usuario negó el micrófono",
que es *la* distinción para la que existe todo el camino de entrada. Río abajo: `wma_input_start`
devuelve false sólo si el pedido se rechazó de entrada, y aparece `wma_input_is_starting()`
—cableada al JNI, a `IInputBridge` y a `AudioInput.isStarting`— para poder decir "todavía no"
sin decir "no". El harness muestra tres estados donde antes había dos.

> [!CAUTION]
> **Mandar el reopen a un thread no alcanzaba, y el test lo destapó colgándose.**
> `BackendManager::start()` toma `mMutex` y adentro llama al `start()` del backend, así que el
> worker lo retiene **toda la reapertura**. `getStreamInfo()`, `isRunning()` e `isCaptureLive()`
> —lo que la UI pollea en cada frame— se bloqueaban igual: el freeze se mudaba de llamada, no
> desaparecía. Los tres pasaron a `try_lock`: con el mutex libre leen **en vivo** (un device
> puede renegociar sin que el motor reinicie, y romper eso lo agarró
> `FollowsTheBackendAcrossARenegotiation`), y con el mutex tomado devuelven el último valor
> publicado, que además es fiel — durante el reopen el stream está de verdad caído.

**El worker se joinea, no se detacha.** Es el mismo use-after-free que ya tiene anotado
`stopWithFade` —un thread que captura `this` y sobrevive al objeto— y el destructor lo cierra
por construcción: cortar con `mShuttingDown`, joinear, y recién ahí `stop()`.

#### El residual también salió: `mOpMutex` (2026-07-27)

Quedaba que pedir captura *durante* una reapertura bloqueara al que llama, porque anotar el
pedido necesita `mMutex`. El fix de raíz no era tocar `requestCapture`: era que **`start()` y
`stop()` dejaran de tener `mMutex` tomado alrededor de la llamada lenta al backend**.

Ahora hay dos locks con orden fijo —**`mOpMutex` → `mMutex`, nunca al revés**—:

- **`mOpMutex`** serializa ciclo de vida (start / stop / selectBackend) y es el único que se
  sostiene sobre la llamada lenta. Lo único que espera ahí es otra operación de ciclo de vida,
  que es correcto: son mutuamente excluyentes por naturaleza.
- **`mMutex`** protege estado, en secciones cortas, y **jamás** alrededor de algo que bloquee.

Con eso **los espejos y el `try_lock` desaparecieron**: los lectores vuelven a un `lock_guard`
normal y a leer **en vivo**, que además es lo correcto —un device puede renegociar sin que el
motor reinicie—. El código quedó más simple que antes de empezar.

**Dos backends bloqueaban un escalón más abajo** y no se veía desde el manager:
`CoreAudioBackend::setFullDuplexEnabled` tomaba `mStreamMutex` —retenido toda la apertura— y
`SplitBackend` su `mLifecycleMutex`. Los dos flags pasaron a atómicos. Sin eso, el bloqueo sólo
se mudaba de capa.

**Y salió un bug de reporte que el test destapó:** a mitad de la reapertura el stream está
caído, así que un segundo `wma_input_start()` leía "no hay captura" y devolvía `NOT_LIVE` — que
río abajo el harness muestra como **"permiso denegado"**. Con una reapertura en vuelo la
respuesta honesta es `PENDING`, y ahora lo es. Eso además hizo **determinista** la rama de la
generación, que hasta entonces no tenía test.

**Un corte audible de más, encontrado al escribir ese test:** el worker daba otra pasada ante
cualquier pedido nuevo, incluso si la pasada en curso ya lo había dejado cumplido — el doble tap
sobre "capturar" reabría dos veces. Ahora se corta si el objetivo ya está.

**762 tests C++ (760 → 762) y 6 mutantes, 6 detectados** — incluidos los dos que antes
sobrevivían por el lado de la generación. TSan limpio sobre las suites tocadas.

> [!NOTE]
> **Lo que queda esperando, y es correcto que espere:** cambiar de backend con una reapertura en
> curso. `selectBackend()` toma `mOpMutex` como cualquier otra operación de ciclo de vida. No se
> puede reconfigurar el stream a mitad de una reconfiguración del stream.

**Verificado en el simulador con el esquema nuevo:** `Capture: off` → `Reopening the stream to
add a capture path` → `Capture: active`, **cero `inputData=0x0` después del reopen** y el main
thread en `mach_msg`, idle en el runloop.

**5 tests nuevos (755 → 760) y 5 mutantes.** Tres detectados; **dos sobrevivieron y quedaron
escritos como lo que son**:

| Mutante | Resultado |
|---|---|
| reopen sincrónico otra vez | **detectado** — se cuelga, que es el freeze reproducido |
| `mMutex` de vuelta alrededor de la llamada lenta | **detectado** — se cuelga |
| `PENDING` reportado como `LIVE` | **detectado** — 4 tests fallan |
| no reportar `PENDING` con una reapertura en vuelo | **detectado** |
| no subir la generación de reintento | **detectado** (lo era sólo después de arreglar el reporte de `PENDING`) |
| sin el atajo de "ya cumplido" | **detectado** — el doble tap costaba dos reaperturas |
| destructor `detach()` en vez de `join()` | **NO detectado**, ni bajo TSan: el `stop()` del destructor toma el mismo lock y provee casi la misma barrera. Eso es justo lo que hace peligroso al detach —la corrección quedaría apoyada en una coincidencia— y está dicho en el test |

> [!NOTE]
> **TSan encontró una carrera de verdad en el camino — en el fake.** El test que mueve
> `setCaptureAvailable` desde otro thread mientras el worker está en `start()` la disparó; el
> knob pasó a ser atómico. La carrera era del test, no del motor, pero sin correr TSan sobre
> los tests nuevos habría quedado ahí.


### Decisión — cómo llegan al harness los 5 controles que faltan (aprobada 2026-07-27)

> [!IMPORTANT]
> **4 de los 5 controles restantes están bloqueados por exactamente lo mismo que el monitor de
> entrada: la superficie no llega a `commonMain`.**
>
> | Control | Qué falta |
> |---|---|
> | 3 · rack de efectos | efectos ✅ vía `AudioEngine`; **routing mode** sólo en `IAudioNativeBridge` |
> | 4 · looper | **nada** en commonMain salvo `LooperStateListener` — son 79 funciones en el JNI |
> | 5 · metrónomo | **BPM** sólo en el bridge, no en `AudioEngine` |
> | 6 · diagnóstico | device caps ✅; **backend y captura de logs** no llegan |
>
> **Decisión: (b) — anotación de opt-in, NO ensanchar la API pública.**
>
> El harness recibe acceso al bridge detrás de una anotación tipo
> `@RequiresOptIn`/`@InternalWatermelonApi`, y `getAudioBridge()` deja de ser `internal` bajo
> ese opt-in. Con eso los cuatro se desbloquean **sin que la API publicada crezca**.
>
> **El porqué, que importa más que la decisión:** el input **sí** merecía API pública —un
> cliente real va a querer capturar, y por eso `AudioInput` es una interfaz de primera clase—.
> Routing, looper, BPM y logs son **superficie de diagnóstico**: el harness es tooling, no un
> consumidor. Ensanchar `AudioEngine` para cuatro subsistemas por conveniencia de una app de
> prueba es exactamente cómo una API pública termina llena de cosas que nadie puede sacar.
>
> La regla que queda: **algo entra a la API pública porque un consumidor real lo necesita, no
> porque el harness lo necesite.** Si mañana NoisyPad pide el looper desde `commonMain`, eso es
> un ticket con su propia justificación — y este opt-in no lo estorba.

**Ojo con lo que el opt-in NO resuelve:** `wma_log_capture_*` existe en la C API desde
2026-07-27 pero **el bridge no la tiene en ninguna plataforma** — ni Android ni iOS. La vista
de logs del control 7 necesita ese cableado igual, opt-in o no. Es el mismo trabajo mecánico
que se hizo con §12: agregar al bridge, implementar en las dos, listo.

#### Hecho (2026-07-27) — y la premisa estaba media equivocada

**`@InternalWatermelonApi`** existe, con `@RequiresOptIn` nivel **ERROR**. Marca
**`getAudioBridge()`** y nada más: la anotación va en la *puerta*, no en
`IAudioNativeBridge`. Anotar la interfaz habría obligado a salpicar `@OptIn` por todo el motor
—la implementan y la reciben `AudioInputImpl`, `EffectManagerImpl`, `StateSynchronizer`— sin
agregar una sola garantía, porque sin la función no hay forma de conseguir un puente. Los seis
usos internos del motor llevan su `@OptIn` explícito.

> [!NOTE]
> **`getAudioBridge()` no era `internal`: ya era público.** La decisión decía "deja de ser
> `internal` bajo ese opt-in" y eso no era exacto — la `expect fun` nunca tuvo modificador, así
> que el harness siempre pudo llamarla. O sea que el trabajo no fue *abrir* nada: fue **cerrar**
> lo que estaba abierto de hecho y sin decirlo. La decisión de fondo no cambia y el resultado es
> el mismo que buscaba, pero conviene anotarlo: la superficie de diagnóstico llevaba tiempo
> alcanzable por cualquier consumidor, sin marca y sin opt-in.

Verificado en las dos direcciones con una sonda en `:harness`: sin `@OptIn` **no compila**
(el error trae el mensaje de la anotación); con `@OptIn` compila y `getRoutingMode()` responde.

**`wma_log_capture_*` cableado.** Acá la premisa también estaba media equivocada: **Android ya
lo tenía entero** —`setLogCaptureEnabled` / `drainCapturedLogs` / `getLogCaptureDropped` sobre
tres funciones JNI, incluido el detalle de buscar `java/lang/String` *antes* de drenar porque
el drain es destructivo—. Lo que faltaba era que estuviera en **`IAudioNativeBridge`**, que es
lo que `commonMain` puede ver, y el lado iOS. Ahora los tres están en la interfaz, Android los
`override`, y iOS los implementa sobre cinterop.

**El `WmaLogBatch` no cruza la frontera**, y es deliberado: la C API entrega un handle que hay
que liberar, y dejarlo llegar a Kotlin sería regalarle un leak a cada llamador. El batch nace y
muere dentro de `drainCapturedLogs()`, con el `try/finally` envolviendo también la lectura de
`count`, y las líneas se copian a `String` ahí mismo porque los punteros valen sólo hasta el
`free`.

**4 tests nuevos en el simulador (101 → 105), 3 mutantes, 3 detectados.** Esto **sí** se puede
probar donde el camino de audio no: el anillo de logs es memoria del proceso, sin audio ni
permisos de por medio. Y cada test se ganó el lugar — el mutante que ignora el *disable*
(`set_enabled(true)` siempre) lo agarra **sólo** `disabledCaptureCollectsNothing`; los otros dos
mutantes (drain vacío, enable no-op) los agarran los otros dos tests y ese no.


### Los 7 controles, completos (2026-07-27)

Con los controles **3** (rack de efectos + routing), **5** (tira de looper), **6** (metrónomo) y
**7** (diagnóstico), WA-5.5 tiene sus siete. Todos corriendo en el simulador de iOS.

**El looper subió 11 funciones, no 79.** El JNI tiene 79; a `commonMain` subieron las que la
tira necesita: preparar en compases, armar, grabar, parar, limpiar, leer estado y exportar. Que
sea un subconjunto es la misma regla del opt-in aplicada de nuevo — **algo entra porque un
consumidor lo necesita**, y el consumidor de hoy es el harness. Subir las 79 "por completitud"
sería fabricar superficie sin caller. Es también el **primer código de looper que existe en
iOS**. El transporte subió 10 más, por el mismo criterio.

> [!NOTE]
> **Los defaults de `transportStartMetronome` se movieron a la interfaz.** La clase concreta de
> Android los tenía (`firstIsDownbeat = true, everyBeatPattern = true`); un `override` no puede
> repetirlos, así que dejarlos ahí habría roto en *fuente* a cualquier consumidor —NoisyPad—
> que llamara `transportStartMetronome(4)`. Puestos en la declaración de la interfaz, los dos
> tipos siguen compilando igual.

**Lo que los controles encontraron apenas se los tocó**, todo en la primera corrida:

| Control | Qué mostró |
|---|---|
| 7 · diagnóstico | **Ítem 10 reproducido en pantalla**: pedir `USB Direct` sin USB da `selectBackend() = true` y el motor queda en `Oboe (System)`. El control muestra pedido y realidad juntos, que es la única forma de verlo |
| 7 · logs | `I/BackendManager: Backend selected: Oboe` — una línea de C++ cruzando el anillo, cinterop y Compose. **Items 2 y 3 probados en la app, no sólo en tests** |
| 5 · looper | `prepareTrackBars(0, 2000000) = -1` (ítem 8) · `exportMix(ruta imposible) = false` sin tirar (ítem 9) · `armAtNextBar(0) = 0` con su frame de disparo (ítem 7). **Tres arreglos de WA-2.6 verificados por primera vez desde iOS**, no sólo por la suite de host |
| 6 · metrónomo | `frames/beat: 24000 · frames/bar: 96000` a 120 BPM / 48 kHz — el reloj que cuantiza los loops, cruzado contra el control 5 |
| 7 · device caps | `IOS · API 26 · 16384 MB · 10 cores · low latency: true · gama baja: false`, de `NSProcessInfo` |

**Lo que la tira de looper NO puede probar en este simulador**, y conviene decirlo: no reproduce
ni exporta audio de verdad, porque para eso hace falta llenar una pista — y eso pasa por la
captura, que está bloqueada por el cuelgue de `playAndRecord` de arriba. Lo que sí ejercita, y
no dependía de nada de eso, es **el contrato de los valores de retorno**, que es exactamente
donde estaban los tres bugs.

El permiso de Android (`RECORD_AUDIO`) ya está en el manifest desde el primer commit por la
misma razón que la clave del micrófono en iOS: el caso que más importa probar es el del permiso
**negado**, y agregarlo después obligaría a reinstalar para reproducirlo.


### Dónde retomar (2026-07-27)

**Branch:** `feature/wa-3-2-ios-audio-bridge`, **50 commits sobre `master`** y **44 sin
pushear**. `master` está en el merge del PR #58.

> [!IMPORTANT]
> **El CI de GitHub está caído por falta de pago (2026-07-26).** Mientras dure, el gate es
> la verificación local completa —los **10** comandos de abajo— y hay que dejar constancia de
> su salida en el PR. Un merge sin CI **no** es un merge verificado por defecto: lo es sólo
> si alguien corrió los gates y lo dijo.

**Última verificación local completa (2026-07-27, con el reopen asincrónico):** portabilidad
OK (**325 archivos**), **762 tests C++**, ambos slices de iOS con
link check, **105 tests de simulador** (101 + los 4 de captura de logs), **64 JVM**,
`assembleDebug`, XCFramework, `compileIosMainKotlinMetadata`, los dos guardrails de WA-5.5 y el
harness **arrancando en el simulador**. Todo en verde, con las tasks de test forzadas
(`--rerun-tasks`) y las cuentas leídas de los XML.

> [!IMPORTANT]
> **Ese verde vale más que antes, y por un motivo concreto.** Hasta el bug 3 de §10, un cambio
> en C++ podía no llegar al framework de iOS y el gate igual daba OK — o sea que los tests de
> simulador podían estar corriendo sobre un `.a` viejo. Ya no: el `.a` es input declarado de
> cinterop.

> [!TIP]
> **Ojo con el verde de Gradle que llega en 400 ms.** Las tasks de test se reportan
> `BUILD SUCCESSFUL` estando `UP-TO-DATE`, sin correr un solo test. Para que el gate valga
> hay que forzarlas (`--rerun-tasks`) o contar los tests en
> `audio/build/test-results/*/**.xml`. Al abrir esta sesión los cinco gates de Gradle daban
> verde en 9s/1s/484ms sin ejecutar nada.

Los últimos commits de la branch:

```
9272623 fix(backends): mOpMutex — el residual tambien sale, y con el los espejos
79144cb fix(ios): el reopen de captura sale del main thread — y ahi SI capturo
a160bb0 docs(kmp): estado tras el sexto bug, el opt-in y los 7 controles
a3fd7d1 feat(harness): controles 3, 5, 6 y 7 — WA-5.5 tiene sus siete
4b5dd30 feat(kmp): @InternalWatermelonApi + wma_log_capture_* llega a commonMain
f9b15fe fix(ios): el reopen de captura se disparaba nunca — el motor tiraba lo que medía
```

**Estado:** Fase 3 cerrada, WA-4.1 hecho, **WA-2.5/2.6 CERRADA**, **WA-5.5 con sus 7 controles**
y —lo grande— **el input path de iOS captura de verdad**. Existe `@InternalWatermelonApi`, y el
looper y el transporte llegan a `commonMain` con caller.

**Cómo verificar que todo sigue en pie antes de tocar nada** (todo corre local; el
bloqueo de Xcode de §11 ya no existe):

```bash
bash scripts/check-cpp-portability.sh          # guardrail WA-0.4
bash scripts/run-cpp-tests.sh                  # 762 tests C++
bash scripts/build-ios.sh                      # ambos slices + link check
./gradlew :audio:iosSimulatorArm64Test         # 105 tests iOS  (--rerun-tasks!)
./gradlew :audio:testDebugUnitTest             # 64 tests JVM   (--rerun-tasks!)
./gradlew :audio:assembleDebug                 # Android, 4 ABIs
./gradlew :audio:assembleWatermelonXCFramework # XCFramework (sólo macOS)
./gradlew :audio:compileIosMainKotlinMetadata  # el source set iOS compartido
bash scripts/check-no-ui-in-library.sh         # guardrail WA-5.5
bash scripts/build-harness.sh                  # :harness, ambas plataformas + símbolos
```

> [!NOTE]
> **Los dos últimos son de WA-5.5 y no son decorativos.** El guardrail afirma que la UI del
> harness no puede entrar al artefacto publicado — su assertion útil es la del **classpath
> resuelto de `:audio`**, no la de publicaciones. Y sin el build del harness en el gate, el
> harness se pudre en silencio: **nada más lo compila**.

> [!IMPORTANT]
> **`compileIosMainKotlinMetadata` es nuevo en la lista y no es decorativo.** Los otros
> gates compilan iOS **por target**, y ninguno compila el source set compartido `iosMain`.
> Eso dejó un break invisible durante toda la fase 3 — ver la nota de abajo.

**Deuda de verificación que conviene saldar temprano — la lista del smoke manual en
NoisyPad Android**, que ya tiene tres cosas encima:

1. **WA-1.4**, la migración a `BridgeConcurrency` (26 call sites), verificada con el
   compilador y revisión de diff porque los métodos de `AudioNativeBridge` **no tienen
   tests** (necesitan JNI y device).
2. **WA-1.2**, `AudioEngineFactory.create()` sin argumentos (ver abajo).
3. **WA-2.6 `input/monitor`**, los dos cambios de comportamiento en el camino de input.
   **Prioridad alta**: el análisis estático de abajo encontró una regresión plausible en
   `startInputStream` con el permiso denegado. Reproducción concreta en esa nota.
4. **WA-2.6 en general**: 135 entry points JNI reescritos, 0 validados en device. La suite
   de host cubre la C API, no el JNI.
11. **`setDepthValue` no hace nada, y nunca hizo nada** (la cola, 2026-07-27). No es una
   regresión de esta serie: `mDepthValue` se escribe y nadie lo lee, en las cuatro capas.
   Si un slider de depth en NoisyPad llama sólo a `setDepthValue`, mover ese slider no
   cambia el audio. El eje depth real es `applyAutomation(axis=2, …)`. **Mirar qué llama
   NoisyPad** antes de decidir si esto se arregla o se borra.
10. **`selectBackend` devuelve `true` aunque no consiga el backend pedido** (la cola,
   2026-07-27). Pedir LIBUSB sin USB presente cae al backend de sistema y reporta éxito;
   sólo `getCurrentBackendType()` lo delata. Comportamiento viejo, ahora pinchado con test.
9. **Las seis funciones de I/O de archivo del looper devuelven `false`/-1 en vez de dejar
   escapar una excepción** (`looper` tanda 4). Antes un export imposible abortaba el proceso.
8. **`prepareTrackBars` rechaza un `bars` que desborda int32** (`looper` tanda 3) en vez de
   alocar un track envuelto. Sólo se ve con conteos de barras absurdos.
7. **`armAtNextBar` / `armInFrames` ahora devuelven -1 cuando el arm no prende**
   (`looper` tanda 2, 2026-07-27) en vez de un trigger frame para una grabación que nunca
   arranca. Si NoisyPad usa el valor para una cuenta regresiva, ahí se ve.
6. **`getRecommendedBufferSize` cambia de respuesta** (`benchmark`, 2026-07-27) en dos
   casos: cuando no hay stream corriendo pero sí una tasa preferida distinta de 48 kHz, y
   cuando el target cae entre dos potencias de dos. Sólo lo usa el tooling de latencia.
5. **El fix del off-by-one del metrónomo** (`metronome`, 2026-07-27) cambia el timing del
   click en Android. Es el único cambio de la serie que altera algo que ya sonaba bien —
   o casi bien: iba ~4 ms adelantado. Escuchar un count-in contra una grabación armada al
   compás.

**En el mismo smoke conviene mirar WA-1.2:** `AudioEngineFactory.create()` sin argumentos
ahora recorta `maxEffects` a 6 en un dispositivo de gama baja, y ni el parseo de
`/proc/meminfo` ni el de `/sys/devices/system/cpu/possible` corren en el host de tests
(macOS no los tiene).



| Fase | Estado |
|---|---|
| **Fase 0** — Análisis y fundaciones | ✅ **CERRADA** — WA-0.1 ✅ · WA-0.2 ✅ · WA-0.3 ✅ (+WA-T.1) · WA-0.4 ✅ |
| **Fase 1** — Quick wins | 🟡 **WA-1.2 ✅** · **WA-1.4 ✅** · **WA-1.6 ✅** · WA-1.1 y WA-1.5 parciales (WA-1.4 y WA-1.2 avanzaron ambas) · **falta sólo WA-1.3** |
| **Fase 2** — C++ multiplataforma | 🟢 Prácticamente completa — **WA-2.1 ✅ completo** · WA-2.0 ✅ · WA-2.7 ✅ · **WA-2.4 output ✅ + captura ✅** · WA-2.2 ✅ · **WA-2.3 ✅**. **`libwatermelon_audio.a` linkea de verdad** (link check con `-force_load`, ambos slices). Falta validación en device (WA-4.3). **WA-2.5 + WA-2.6 ✅ CERRADA** — las 10 categorías más la cola de 15; delegación **237/278** por el script, **240/289** real. Los 49 que no delegan son **todos deliberados y con el porqué escrito en el código** (40 USB/D4, 5 Oboe/stubs, 2 listeners, 2 de backend que sólo tienen caminos USB): **cero sin clasificar**. **Murió la duplicación de estado de modo**; el metrónomo dejó de adelantar un bloque |
| **Fase 3** — Kotlin iosMain | ✅ **CERRADA** 2026-07-25 — WA-3.1 ✅ · WA-3.2 ✅ · **WA-3.3 ✅** (lo cerró WA-1.2) · WA-3.4 ✅. `AudioEngineFactory.create()` funciona en iOS; 87 tests en el simulador, 0 fallas. Quedan diferidos WA-3.5 (P2) y la revisión de paths de WA-3.6 |
| **Fase 4** — Empaquetado y publicación | 🟡 **WA-4.1 ✅** — el pipeline ya publica metadata KMP + klibs iOS desde 1.8.0 y ahora ensambla el XCFramework en CI. Falta validar el consumo desde NoisyPad (G1, WA-4.2). **WA-4.3 primera mitad la subsume WA-5.5**, que ya corre en el simulador |
| **Fase 5** — Harness (WA-5.5) | 🟢 **LOS 7 CONTROLES HECHOS · EL INPUT PATH DE iOS CAPTURA** — `:harness` corre en el simulador de iOS y ya encontró **6 bugs de arranque, todos arreglados**; el motor abre un stream real (48 kHz / **480 frames** / 10.10 ms) y ahora **lo reporta bien**. Gate de 8 a **10 comandos**. Controles 1–7 completos: transporte, pad XY, rack+routing, monitor de entrada, tira de looper, metrónomo y diagnóstico con vista de logs. Existe `@InternalWatermelonApi` y el looper/transporte llegan a `commonMain` (11+10 funciones, con caller). **Lo único abierto: que el medidor se mueva** — bloqueado por el cuelgue de `playAndRecord` del simulador, que NO es del motor |

> [!IMPORTANT]
> **Seis bugs de arranque de iOS, todos arreglados** (§10). El sexto es el que explicaba por
> qué la captura difería en vez de reabrir: `CoreAudioBackend::start()` invalidaba el cache de
> stream info **después** de que `openEngineLocked()` publicara los valores negociados, así que
> `getStreamInfo()` devolvía el *pedido* — y `isFullDuplex` con él. Arreglado, la UI reporta
> `480 frames / 10.10 ms` en vez de `256 / 0.0`, y el reopen **se dispara por primera vez**.
>
> **Y el medidor se mueve.** Lo que trababa `playAndRecord` no era el simulador: era hacerlo
> desde el **main thread**. Con el reopen en un worker, `Capture: active`, `inputData` deja de
> ser `0x0` y `inputPeak` trae señal real del micrófono. **El input path de iOS captura** — la
> pregunta abierta más grande del programa, contestada.

**El programa cambió de eje: la pregunta que lo justificaba está contestada.** El input path de
iOS captura (§10). Lo que queda ya no es "¿anda?", es cerrar y validar. En orden de lo que
desbloquea más:

**1 · Pushear la branch y abrir el PR con constancia del gate.** 50 commits sobre `master`, **44
sin pushear**, y el CI caído. Es el riesgo más barato de eliminar y el que más crece con el
tiempo: todo lo de estas dos sesiones vive sólo en local.

**2 · G1 / WA-4.2 — el consumo desde NoisyPad.** Del lado de NoisyPad: declarar la coordenada
raíz, verificar que resuelve para los dos targets iOS y confirmar el lockstep de Kotlin (D8:
este repo está en 2.4.0). El pipeline ya publica metadata KMP + klibs con los bindings adentro.
**Necesita una sesión con los dos repos montados.**

**3 · El smoke manual en NoisyPad Android** (la lista de abajo, 11 ítems). Tres ya no están del
todo a ciegas: **7, 8 y 9 —los tres retornos del looper— quedaron verificados desde iOS** por el
control 5 del harness. Lo que falta ahí es Android, donde el camino es el JNI y no la C API.
Prioridad alta sigue teniendo el ítem 3 (`startInputStream` con permiso denegado).

**4 · WA-4.3 segunda mitad, en device (G2).** Ya no es "probar que captura" — eso está. Es
**sonido real**, latencia round-trip medida, Instruments sobre el render block de
`CoreAudioBackend` (cero allocs, cero locks) y la interrupción por llamada entrante que cierra
el criterio original de WA-3.4. **Necesita un iPhone.**

**5 · Design system (WA-5.5, fase final).** Va último a propósito y ahora sí tiene su
precondición: **existe un consumidor real con siete controles**. Recién ahora se sabe qué
componentes hacen falta en vez de adivinarlos. Sigue necesitando la decisión explícita de si se
comparte con NoisyPad — eso implica que este repo pase a shippear Compose.

**6 · WA-1.3**, lo único que falta de Fase 1.

**Lo tercero: el smoke manual en NoisyPad Android**, cuya lista sigue creciendo (abajo). Tres de
sus ítems —7, 8 y 9, los tres retornos del looper— **ya están verificados desde iOS** por el
control 5; lo que falta ahí es Android.

**Lo que ya NO está pendiente:** el opt-in, el cableado de `wma_log_capture_*` y los 7
controles. Ver §10.

> [!TIP]
> **La regla que dejó esta decisión:** algo entra a la API pública porque **un consumidor real
> lo necesita**, no porque el harness lo necesite. El input sí la merecía —un cliente va a
> querer capturar, por eso `AudioInput` es interfaz de primera clase—; routing, looper, BPM y
> logs son superficie de diagnóstico.

Lo que no delega y no se hace, con el porqué: **40 USB** (D4, 37 en `jni_audio_bridge.cpp` +
3 en `jni_usb.cpp`), **5 de `jni_benchmark.cpp`** que son Oboe puro o stubs deprecados,
**2 listeners del looper** (maquinaria JNI; iOS necesita un callback propio, superficie nueva
a diseñar) y **2 de backend** — `CreateSplitBackend` y `FallbackToOboeBackend`, cuyos únicos
caminos alcanzables requieren el backend USB.

**Método de WA-2.5/2.6 — cerrado, se deja como referencia** (sirvió para diez categorías más
la cola; si aparece otra superficie C API para migrar, es esta receta):

1. Leer `docs/kmp/c_api_coverage.md` — §4 para el gap de la categoría y **§4b para cuántos
   de sus entry points ya delegan**. Correr `python3 scripts/c-api-gap.py` para los números
   al día (imprime; el doc se actualiza a mano con esa salida).
2. **No dimensionar la categoría por su gap.** `lifecycle` tenía gap 8 sobre el papel, gap
   real 0, y 22 funciones para migrar igual. El número que importa para WA-2.6 es el de
   delegación. **La unidad de trabajo es la sección de `watermelon_audio.h`, no la fila de
   la tabla** — las categorías del script son keywords y desparraman (el snapshot de
   metering cae en "Analysis", `isInputClipping` en "Mixer / Regions").
3. **Diffear la semántica antes de migrar, en las dos direcciones.** En `input/monitor` la
   C API hacía *más* que el JNI en tres funciones, y migrar trajo ese comportamiento a
   Android. Donde no coincidan, decidir a conciencia y **anotarlo para el smoke**; no
   descubrirlo después.
4. En el **mismo PR**: agregar las `wma_*` que falten de verdad **y** migrar las `Java_…`
   de esa categoría a llamarlas.
5. Donde el comportamiento sea observable desde la C API, sumar tests a
   `core/tests/test_c_api_*.cpp` — `api/watermelon_audio.cpp` ya está en ese target y hay
   una `support/CApiFixture.h` que arma el motor por `wma_engine_create()`. **Mutar el
   código y ver fallar el test** antes de darlo por bueno; y si algo no se puede cubrir
   (el stub de `InputNode` no tiene comportamiento), decirlo en el archivo en vez de
   escribir una assertion que pasa siempre.
6. Actualizar `c_api_coverage.md` y el estado acá.

**Orden de categorías:** ~~lifecycle~~ ✅ → ~~input/monitor~~ ✅ → ~~effects~~ ✅ →
~~oscillator/synth~~ ✅ → ~~voice~~ ✅ → ~~mode~~ ✅ → ~~análisis~~ ✅ →
~~metronome~~ ✅ → ~~benchmark~~ ✅ → ~~looper~~ ✅ (77/79, 2 no portan) →
~~**la cola de 15**~~ ✅ (routing, backend, XY mapping, captura de logs). **Las 10 categorías
más la cola: WA-2.5/2.6 CERRADA, complemento 49 y cero sin clasificar.**

> [!TIP]
> **Ocho de las nueve categorías cerradas encontraron un bug**, y hay dos mecanismos
> distintos. El primero es divergencia: la C API se escribió transcribiendo el JNI función
> por función, y en alguna se transcribió *la función equivocada* o *de menos*.
> `wma_engine_start` colapsó dos operaciones en una; `wma_input_*` traía de más;
> `wma_effect_set_params_batch` copió el setter individual en vez del batch; `mode` era un
> boceto que decía serlo en su propio doc comment.
>
> El segundo no tiene nada que ver con las dos superficies: es un bug de producción viejo
> que aparece porque **migrar obliga a leer la función entera y a escribirle un test**. Así
> salieron la lectura fuera de rango de `voice`, los medidores de salida muertos de
> `análisis`, el off-by-one del metrónomo y los dos redondeos de `benchmark`.
> `oscillator/synth` fue la única limpia.
> El compilador no ve ninguno de los dos tipos.

> [!TIP]
> **El gap sobrestima el trabajo — salvo cuando no hay sección que abrir.** En las siete
> primeras categorías el gap nominal fue **39** y el trabajo real **11 funciones nuevas**,
> porque la C API abrevia donde el JNI escribe entero. **`metronome` invirtió el signo**:
> gap 9, trabajo real 10, porque su sección de `watermelon_audio.h` no existía y no había
> abreviatura que descontar. `benchmark` volvió a sobrestimar por el motivo opuesto —gap 4,
> trabajo real 2— porque **la mitad de la categoría no porta**. La receta no cambia: abrir
> la sección y contar a mano. Si la sección **no existe**, el gap es el piso; si la
> categoría toca Oboe, es el techo y sobra.

- **`mode` no es una más**: ahí desaparece por construcción la duplicación de
  `currentMode` / `modeTransitionInProgress` / `modeTransitionProgress` entre
  `JniGlobalState` y `WmaEngine` (ver hallazgos abajo).
- **`looper` va último** — 39 funciones, el bloque más grande, y conviene llegar con el
  mecanismo ya rodado.

**Ojo con la verificación en este trabajo:** las funciones JNI **no tienen tests** (necesitan
device). El gate real es el compilador, la suite C++ de host donde aplique, y revisión del
diff. Donde se pueda meter un test de host —como se hizo con `test_capture_requests.cpp` en
la etapa 2 del input path— conviene hacerlo: es la diferencia entre verificar y confiar.

**Después, bloqueado por hardware:**

- **WA-4.3 primera mitad** (sample app en simulador) — **aprobada, sin empezar**. No necesita
  iPhone; ver la decisión en §9. Es lo que probaría que el input path realmente captura.
  **Ojo: la propuesta WA-5.5 (§10) la subsume** — un harness Compose Multiplatform cubre las
  dos plataformas en vez de sólo iOS. Conviene decidir WA-5.5 antes de empezar ésta, para no
  escribir la app de iOS dos veces.
- **WA-4.3 segunda mitad, en device** (M). **Necesita un iPhone.** Sonido real, Instruments
  sobre el render block de `CoreAudioBackend` (cero allocs, cero locks), latencia
  round-trip medida, y la interrupción por llamada entrante que cierra el criterio
  original de WA-3.4. → **G2**

**G1** queda del lado de NoisyPad: el pipeline ya publica los klibs de iOS con los bindings
adentro, así que sólo falta declarar la coordenada raíz allá, verificar que resuelve para
ambos targets y confirmar el lockstep de Kotlin (D8: este repo está en 2.4.0).

> [!NOTE]
> **El publish en Linux hoy funciona — pero WA-3.1 lo rompe.** Verificado 2026-07-25
> sobre los logs del run de release 1.8.1 (`ubuntu-latest`): `compileKotlinIosArm64`,
> `compileKotlinIosSimulatorArm64`, `iosArm64Klib` y ambos
> `publishIos*PublicationToGitHubPackagesRepository` **corrieron y pasaron**. Kotlin/Native
> compila klibs de iOS en Linux sin problema: el toolchain de Apple hace falta para
> *linkear* (frameworks, ejecutables) y para correr tests, no para producir un `.klib` a
> partir de fuentes Kotlin. El único warning de host en ese run es
> `Native task 'iosSimulatorArm64Test' is disabled`, que es el test, no la compilación.
>
> **Lo que sí rompe:** en cuanto WA-3.1 agregue **cinterop**, las compilaciones de iOS
> pasan a depender de `cinteropWatermelonAudio<Target>`, que necesita el **SDK de iOS**
> para parsear `watermelon_audio.h`, más el `.a` que sólo produce Xcode. Eso **no** corre
> en Linux. O sea: el job `publish` de `release-please.yml` hay que moverlo a
> `macos-latest` (o partirlo en matriz host-specific si el minutaje ×10 pesa) **como parte
> de WA-3.1**, no antes. Si se posterga, el síntoma no va a ser un error claro sino una
> publicación que compila sin los bindings.

Para **WA-3.1** falta: `nativeInterop/cinterop/watermelon_audio.def` sobre
`watermelon_audio.h`, el wiring de Gradle en el convention plugin (`cinterops { ... }` +
link estático del `.a` por target, más los cuatro frameworks de Apple, que un `.a` no
linkea por sí solo), y verificar que Kotlin/Native resuelve los símbolos `wma_*`. Después
WA-3.2 (`IosAudioBridge`) y WA-3.3 (el `actual` de `getAudioBridge` que hoy lanza
`NotImplementedError`).

> **La primera decisión de WA-3.1 ya está tomada** (2026-07-25): la sonda se promovió al
> target shipped, así que `build-ios.sh` produce el `.a` completo contra el cual cinterop
> linkea. Lo que queda por resolver es cómo lo invoca Gradle — envolver el script en una
> task o dejar que la task de cinterop dependa de él.

Trabajo paralelo, sin bloquear WA-3:
- **Input path de iOS** — un adapter CoreAudio de captura en la costura `WMA_HAS_OBOE` de
  `InputNode.cpp` (hoy inerte en iOS). Habilita full-duplex / guitar FX. **Ojo:** conviene
  hacerlo junto con la unificación del `InputNode` duplicado (ver hallazgos abajo).
- **WA-4.3** (validación en device) — sonido real + Instruments sobre el render block de
  `CoreAudioBackend`; confirma qué rama del ABL (interleaved vs planar) toma el OS y mide
  latencia. Necesita hardware.

**Deuda técnica registrada al cerrar WA-2.0/2.7 (candidatos a ticket propio):**
- `stopWithFade` detacha un `std::thread` que captura `this` y duerme `fadeMs + 50` antes
  de `stop()`. Si el motor se destruye en esa ventana es use-after-free; el destructor
  cancela el fade pero no tiene handle sobre ese thread.
  **Ya hay precedente de cómo cerrarlo** (2026-07-27): el worker del reopen de captura en
  `BackendManager` es el mismo problema resuelto — thread joinable como miembro, flag
  `mShuttingDown`, y destructor que corta → joinea → recién ahí para. Copiar esa forma.
- Tres declaraciones muertas expuestas al pasar `core/`/`nodes/` por `-Werror` por primera
  vez (`MusicalScale.cpp:131`, `BurstModulator.h:52`, `AudioEngine.h:1027`), hoy tapadas
  por un `-Wno-unused-variable` acotado en `core/tests/` con comentario que las nombra.
- Bug 3 de WA-2.0 (SoundFont al rate negociado): los 3 `loadSoundFont*` siguen sin test
  directo — necesitan un fixture SF2. `currentSampleRate()`, el mecanismo compartido, sí
  está cubierto.

**Hallazgos de la auditoría 2026-07-25 (candidatos a ticket propio):**

- ~~**`InputNode` está duplicado entre el JNI y la C API.**~~ ✅ **RESUELTO 2026-07-25.**
  El JNI usaba `g_jniState.inputNode` y la C API `e->inputNode` — dos instancias distintas,
  ambas enchufables al mismo `AudioEngine`. Nunca rompió en Android porque sólo se recorría
  uno de los dos caminos, pero implicaba que **todo el bloque `wma_input_*` era código
  muerto en producción**, e iOS (WA-3.2) iba a ser su primer usuario real.
  Ahora `g_jniState.inputNode` es un `shared_ptr` **a la misma instancia** que posee el
  `WmaEngine` — el mismo patrón que ya seguía `g_jniState.engine`. El `ensureInputNode()`
  del JNI delega en `wmaEnsureInputNode()` (expuesto en `watermelon_audio_internal.h`), y
  el nuevo `releaseInputNode()` suelta **ambos** handles, porque soltar uno solo
  recrearía el split.
  **Ojo con la verificación:** esto no lo cubre la suite de host (el JNI necesita device y
  `core/tests/` sustituye `InputNode` por un stub). El gate fue el compilador más el
  argumento estructural.
- **El estado de modo también está duplicado** (hallado al unificar el `InputNode`):
  `JniGlobalState` y `WmaEngine` tienen **cada uno** su `currentMode`,
  `modeTransitionInProgress` y `modeTransitionProgress`, y son copias independientes — el
  JNI usa las suyas en 2 lugares y la C API las suyas en 2. Es la misma clase de bug, sin
  arreglar todavía: corresponde a la categoría `mode` de **WA-2.5/2.6**, donde el JNI pasa
  a llamar la C API y la duplicación desaparece por construcción.
- **WA-2.6 es más barato de lo que dice §3/D3.** El ciclo de vida ya está unificado:
  `ensureEngine()` (`jni/jni_engine.cpp:41`) **ya crea el motor con `wma_engine_create()`**
  y cachea un puntero crudo a su `AudioEngine`. El refactor no es cirugía de lifecycle sino
  una reescritura mecánica por función. Eso habilita fusionarlo con WA-2.5 (abajo).

**Decisión de método (2026-07-25): WA-2.5 y WA-2.6 se hacen fusionados, por categoría.**
En vez de completar la C API y después migrar el JNI, cada PR toma una categoría
(lifecycle → input/monitor → effects → oscillator/synth → voice → mode → análisis →
metronome → benchmark → **looper**), agrega las `wma_*` faltantes **y** migra en el mismo
PR las `Java_…` de esa categoría a llamarlas. Cada función se escribe una vez y nace
multiplataforma, que es literalmente el objetivo de D3. Separados, se escriben ~79
funciones nuevas y después se vuelven a tocar 278 puntos del JNI.

**Decisión de producto (2026-07-25): NoisyPad iOS v1 SÍ necesita el looper completo.**
Las 39 funciones de looper del gap portable entran en WA-2.5 sin recortar. Es el bloque
más grande y va **último** en la secuencia de categorías, para llegar con el mecanismo ya
rodado. Esto refuerza la fusión WA-2.5/2.6: el looper es justo donde escribir cada función
dos veces más cuesta.
