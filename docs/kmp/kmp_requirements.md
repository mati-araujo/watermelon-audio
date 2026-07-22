# Requerimiento: KMP/iOS Readiness — watermelon-audio

**Proyecto:** watermelon-audio (`com.watermellonstudios:audio-android`, v1.6.0)
**Documento hermano:** `NoisyPad/docs/kmp/kmp_requirements.md` (consumidor)
**Estado:** EN CURSO — Fase 0 parcial (WA-0.2 cerrado)
**Fecha:** 2026-07-05 · **Última actualización:** 2026-07-22 (cifras del diagnóstico
re-auditadas contra el código y resultado de WA-0.2)
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
- Tests C++ (googletest) **host-compilables**: dsp/effects/looper/voice/engine/usb suites, scripts `run-cpp-tests.{ps1,sh}`, integrados a `check`.

**Trabajo pendiente (el objeto de este requerimiento):**

| Área | Estado | Problema |
|---|---|---|
| Audio I/O | `OboeBackend` (Android-only) | No hay backend iOS |
| Puente nativo | JNI: `jni_audio_bridge.cpp` 3.583 LOC, **278 JNIEXPORT** | iOS no tiene JNI; además el JNI llama al engine directo, no vía C API |
| C API | **189 funciones** | **Gap ~89 funciones** vs JNI (cobertura incompleta: looper avanzado, mixer, regions, transiciones de modo, análisis) |
| Platform C++ | Solo `PlatformAndroid.cpp` | Falta implementación Apple |
| Kotlin androidMain | `Mp4AacTranscoder` (MediaCodec), `UsbAudioManagerImpl`, `TrustedUsbDevicesRepository` (DataStore), `DeviceCapabilities` (Context) | Sin contrapartes iOS ni interfaces comunes en algunos casos |
| Build | CMake vía AGP `externalNativeBuild` | No hay toolchain iOS ni empaquetado XCFramework |
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
| WA-0.4 | Guardrail de portabilidad C++ | Check de CI que falle si aparece `#include <jni.h>`/`<android/...>` fuera de `jni/`, `backends/Oboe*`, `backends/Libusb*`, `usb/` y `platform/PlatformAndroid.cpp`. **Ampliar a Kotlin:** el equivalente para `commonMain` es que WA-0.3 compile los targets iOS en cada PR — es lo que habría atajado los 34 errores de WA-0.2 | CI rojo ante include prohibido (probado con PR sintético) | P1 | S | Pendiente |

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
| WA-1.2 | `DeviceCapabilities` común | Definir interfaz/expect en commonMain (RAM, low-latency hint, API level abstracto); actual Android actual queda como está; deja el hueco para el actual iOS | `AudioEngineFactory` consume la abstracción | P1 | S | Pendiente |
| WA-1.3 | API USB segregada | Asegurar que los tipos/factories USB (`IUsbAudioManager`, `UsbAudioManagerFactory`) no sean requeridos para usar el resto de la API (interface segregation). Mover a androidMain lo que no necesite estar en common, o documentar como android-only.<br>**Acoplamiento real medido (2026-07-22): sólo 3 puntos.** (a) `AudioBackendType` vive en `domain/usb/UsbAudioTypes.kt` pero **no es un tipo USB** — lo consumen `AudioEngine` y `AudioEngineImpl`; debería mudarse a `domain/`. (b) `IAudioNativeBridge.isUsbBackendAvailable()`. (c) `IAudioNativeBridge.setUsbLatencyProfile()`. Nada más de commonMain depende de `domain/usb/` | Un consumidor sin USB compila para iOS sin stubs USB | P1 | M | Pendiente — **decisión 2026-07-22:** en WA-0.2 se optó por portar `domain/usb/` en su lugar (sigue en commonMain) para no mezclar un cambio de API pública dentro de WA-0.2 |
| WA-1.4 | Extraer `BridgeConcurrency` | Los mutexes por categoría (lifecycle/effects/mode/input) y el mapeo error-code→excepción de `AudioNativeBridge` (**3.352 LOC**) se extraen a commonMain para reutilizarlos en `IosAudioBridge` sin duplicar | AudioNativeBridge delega en la clase común; tests Android verdes | P1 | M | Pendiente |
| WA-1.5 | Tests Kotlin de commonMain | ~~hoy: cero tests Kotlin~~ → **hoy hay 5 suites / 34 tests** (`ChordGenerator`, `ScaleQuantizerFlow`, `EffectManagerBatch`, + `Format` y `Time` de WA-0.2). **Falta lo central:** `StateSynchronizer`, `AudioEngineImpl`, mapeo de errores | Suite commonTest corriendo en JVM en CI | P1 | M | Parcial |
| WA-1.6 | Factorizar denormals ARM64 | El código FPCR de `PlatformAndroid.cpp` para arm64 es idéntico al que necesitará Apple Silicon → extraer a `PlatformArm64.inc` compartido | PlatformAndroid compila igual; código listo para PlatformApple | P2 | S | Pendiente |

---

## 7. Requerimientos — Fase 2: C++ multiplataforma

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WA-2.1 | Build CMake iOS | Toolchain/presets iOS (device arm64 + simulator arm64): compilar `watermelon-dsp/effects/engines/voice/looper` + core + nodes + api como **librería estática** por slice. Excluir del build iOS: `jni/`, `usb/`, `OboeBackend`, `LibusbBackend`, `PlatformAndroid.cpp`. Definir `USE_NEON=1` en arm64 Apple | `libwatermelon_audio.a` para ambos slices compila con Xcode clang, C++20 | P0 | L |
| WA-2.2 | `PlatformApple.cpp` | Implementar `wma::platform`: denormals (FPCR, reutiliza WA-1.6), `setAudioThreadPriority()` (pthread `THREAD_TIME_CONSTRAINT_POLICY` — solo como refuerzo: el thread de Core Audio ya viene priorizado), SIMD caps (NEON fijo en arm64) | `engine_tests` linkea y pasa en macOS con PlatformApple | P0 | M |
| WA-2.3 | Logger Apple | Sink `os_log` por defecto en builds Apple; callback configurable idéntico a Android | Log visible en Console.app desde sample app | P1 | S |
| WA-2.4 | **`CoreAudioBackend`** | Implementar `IAudioBackend` para iOS (D2): AVAudioEngine + AVAudioSourceNode (output) y AVAudioSinkNode/inputNode (input full-duplex para guitar/input FX). Reglas: el render block invoca directo el mix C++ (sin ObjC dispatch, sin allocs, sin locks); negociación de sample rate/buffer contra el hardware; manejo de formato (Float32 nativo de Core Audio vs pipeline interno) | Sine + cadena de efectos + looper suenan en device real; callback verificado sin allocs (Instruments) | P0 | L |
| WA-2.5 | **Completar la C API** | Cerrar el gap de WA-0.1: agregar a `watermelon_audio.h` las funciones faltantes (excluyendo USB). **Dimensionado por WA-0.1: 110 portables, de las cuales ~79 netas tras descartar near-matches — y 39 son del looper.** Ver `docs/kmp/c_api_coverage.md` para el detalle por categoría. Reglas de ABI: handles opacos, códigos de error enteros (sin excepciones cruzando la frontera), sin tipos C++ en firmas, documentación de thread-safety por función (RT-safe vs coordinación) | Cobertura 1:1 con el JNI no-USB según tabla WA-0.1 | P0 | L |
| WA-2.6 | **JNI → wrapper de la C API** (alto valor) | Refactor incremental por categorías (lifecycle → effects → looper → mode → análisis): cada `Java_..._nativeXxx` pasa a llamar `wma_xxx` en vez del engine directo. El JNI queda como capa de traducción de tipos JNI↔C de ~1 línea por función | Paridad Android/iOS por construcción; tests C++ y smoke Android verdes tras cada categoría | P1 | L |
| WA-2.7 | Selección de backend por plataforma | `BackendManager` compila con Oboe+Libusb en Android y CoreAudio en iOS (compile-time, `#if` mínimos en un solo archivo de registro de backends) | Sin `#ifdef` dispersos; un único punto de registro | P1 | S |

---

## 8. Requerimientos — Fase 3: Kotlin iosMain

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WA-3.1 | cinterop | `watermelon_audio.def` sobre `watermelon_audio.h`; link estático de los `.a` por target; verificación de que los 181+ símbolos resuelven | Kotlin/Native llama `wma_start_engine()` desde un test de simulador | P0 | M |
| WA-3.2 | `IosAudioBridge` | Implementación de `IAudioNativeBridge` en iosMain sobre cinterop: mismos contratos `Result<T>`, mismos mutexes por categoría (reutiliza `BridgeConcurrency` de WA-1.4), mapeo error-code→excepción idéntico. Los métodos RT (`setXY`, `setFrequencyAndAmplitude`) llaman la función C directa sin suspend ni locks | Suite commonTest de bridge (WA-1.5) pasa contra el bridge iOS en simulador | P0 | L |
| WA-3.3 | actuals iOS | `NativeLibraryLoader` (no-op, link estático), `AudioBridgeProvider` (retorna `IosAudioBridge`), `DeviceCapabilities` (ProcessInfo/UIDevice) | `AudioEngineFactory.create()` funciona en iOS | P0 | S |
| WA-3.4 | `AudioSessionManager` | Helper iosMain para AVAudioSession: categoría `playAndRecord`, `preferredIOBufferDuration`/`preferredSampleRate`, notificaciones de interrupción y route change expuestas como Flow para que el consumidor (NoisyPad) las mapee a start/stop | Interrupción por llamada entrante pausa y reanuda el engine en sample app | P0 | M |
| WA-3.5 | Transcoder abstracto | `Mp4AacTranscoder` (MediaCodec) → interfaz `IAudioTranscoder` en commonMain; actual Android existente; actual iOS con `AVAssetWriter` (diferible: el export WAV no lo necesita) | Interfaz común; iOS actual puede llegar después | P2 | M |
| WA-3.6 | Regla RT documentada | Documentar y hacer cumplir D6: ningún callback del thread RT entra a Kotlin; estado via polling/colas. Incluir en el README de contribución | Doc + revisión de que ningún path actual lo viola | P1 | S |

---

## 9. Requerimientos — Fase 4: Empaquetado y publicación

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WA-4.1 | XCFramework en el pipeline | Task Gradle que ensambla el XCFramework (device+simulator) y lo integra al klib/publicación; cache para no recompilar C++ sin cambios | `./gradlew :audio:assembleWatermelonXCFramework` reproducible en CI | P0 | M |
| WA-4.2 | Publicación KMP | Publicar a GitHub Packages el artefacto KMP completo (metadata común + AAR Android + klibs iOS). Release Please sigue gobernando la versión. Validar consumo desde un proyecto de prueba iOS y desde NoisyPad Android (sin cambios para el consumidor Android actual) | NoisyPad resuelve la misma coordenada para ambos targets — **desbloquea gate G1 de NoisyPad** | P0 | M |
| WA-4.3 | Sample app iOS | Mini app (puede vivir en el repo) que haga smoke de la librería: start engine, sine, un efecto, looper record/play, medición de latencia round-trip | Smoke manual documentado; latencia medida y registrada | P1 | M |

---

## 10. Requerimientos — Fase 5: Diferidos / backlog

| ID | Requerimiento | Detalle | Prio |
|---|---|---|---|
| WA-5.1 | USB en iPadOS | Investigación DriverKit/entitlements para clase de audio USB — solo si el negocio lo pide | P3 |
| WA-5.2 | Latency benchmark iOS | Port de `LatencyBenchmarkRunner` (hoy androidMain) sobre la infraestructura de WA-4.3 | P2 |
| WA-5.3 | AUv3 | Empaquetar el motor como Audio Unit v3 (extensión) — habilitaría NoisyPad como plugin en GarageBand/Logic/AUM. Análisis de arquitectura propio | P3 |
| WA-5.4 | CoreMIDI | Si el roadmap de NoisyPad incorpora MIDI-in en iOS | P3 |

---

## 11. Testing

| ID | Requerimiento | Detalle | Prio | Estado |
|---|---|---|---|---|
| WA-T.1 | Tests C++ en macOS | Los googletest existentes (dsp/effects/looper/voice/engine) ya compilan en host: agregar job macOS con clang de Xcode — detecta problemas de portabilidad (MSVC/MinGW vs clang-apple) antes de tocar iOS | P0 (parte de WA-0.3) | ✅ **HECHO** — paso en el job `ios`; ya encontró un bug (script roto en bash 3.2, ver §5) |
| WA-T.2 | commonTest Kotlin | Cobertura de la lógica común (WA-1.5) corriendo en JVM y luego contra `IosAudioBridge` en simulador (WA-3.2) | P1 | Parcial — 34 tests verdes en JVM; el binario K/N de test linkea |
| WA-T.3 | Smoke cinterop | Test de simulador: round-trip completo `AudioEngineFactory.create()` → start → setXY → addEffect → stop | P0 | Bloqueado por WA-2.x + WA-3.1 |
| WA-T.4 | Verificación RT | Sesión de Instruments (Time Profiler + Allocations) sobre el render callback de `CoreAudioBackend`: cero allocs, cero locks, sin prioridad invertida | P0 (criterio de WA-2.4) | Pendiente |

### Prerrequisito de entorno local (detectado 2026-07-22)

`./gradlew :audio:iosSimulatorArm64Test` **se cuelga indefinidamente** en la máquina de
desarrollo: Xcode 26.6 no tiene completado su *first launch*, y Gradle dispara
`xcodebuild -runFirstLaunch`, que espera privilegios de admin sin TTY disponible.

```bash
sudo xcodebuild -runFirstLaunch
```

Hasta entonces, la verificación local de tests K/N se hace con
`:audio:linkDebugTestIosSimulatorArm64` (compila y linkea el binario sin ejecutarlo).
**No afecta a WA-0.3:** los runners macOS de GitHub Actions vienen con Xcode ya
inicializado.

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
  **Estado 2026-07-22: WA-0.2 ✅ — G1 queda a un solo paso.** Lo único que falta es la
  publicación intermedia (WA-4.2 parcial). Antes de dispararla conviene cerrar WA-0.3,
  para no publicar metadata desde una máquina de desarrollo, y confirmar el lockstep de
  Kotlin con NoisyPad (D8: este repo está en 2.4.0).
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

| Fase | Estado |
|---|---|
| **Fase 0** — Análisis y fundaciones | 🟢 Casi cerrada — WA-0.1 ✅ · WA-0.2 ✅ · WA-0.3 ✅ (+WA-T.1) · **solo falta WA-0.4** (esfuerzo S) |
| **Fase 1** — Quick wins | 🟡 Tocada de refilón por WA-0.2 (WA-1.1 y WA-1.5 parciales) |
| **Fase 2** — C++ multiplataforma | ⬜ No iniciada — **ruta crítica** |
| **Fase 3** — Kotlin iosMain | ⬜ No iniciada (salvo los 2 `actual` mínimos de WA-0.2) |
| **Fase 4** — Empaquetado y publicación | ⬜ No iniciada |

**Próximo paso recomendado:** **WA-2.1** (build CMake para iOS). Es el primer eslabón de
la ruta crítica hacia G2 y desbloquea WA-2.2 (`PlatformApple`) y sobre todo **WA-2.4**
(`CoreAudioBackend`), que el propio § 14 marca como lo que conviene arrancar apenas
cierre WA-2.1. El andamiaje de Fase 0 ya está: targets iOS que compilan, un CI que los
vigila con clang, y el gap de la C API dimensionado.

**Pendiente de bajo costo:** WA-0.4 (guardrail de `#include <jni.h>`), esfuerzo S y
puramente aditivo — el equivalente Kotlin del guardrail ya lo cubre el job `ios`. Cierra
la Fase 0 en una sentada.

**Decisión de producto a tomar temprano:** si NoisyPad iOS v1 necesita el looper completo.
Son 39 de las 110 funciones portables del gap (WA-0.1) — el bloque más grande de WA-2.5.
