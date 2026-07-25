# Requerimiento: KMP/iOS Readiness — watermelon-audio

**Proyecto:** watermelon-audio (v1.8.1). Coordenada **KMP**: `com.watermellonstudios:audio`
— `:audio-android` es el módulo Android suelto, **no** el que debe usar un consumidor KMP
**Documento hermano:** `NoisyPad/docs/kmp/kmp_requirements.md` (consumidor)
**Estado:** EN CURSO — **Fase 0 cerrada**; Fase 2 casi completa (el motor abre stream en iOS y `libwatermelon_audio.a` linkea) · G1 y luego Fase 3 (cinterop) son los próximos eslabones
**Fecha:** 2026-07-05 · **Última actualización:** 2026-07-25 (cerrados: WA-0.1/0.2/0.3/0.4,
**WA-2.1 completo**, WA-2.0, WA-2.7, WA-2.4 output, WA-2.2; InputNode portable a iOS)
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
- Tests C++ (googletest) **host-compilables**: dsp/effects/looper/voice/engine/usb + **core** (nuevo con WA-2.0, 22 tests de `AudioEngine`) suites, scripts `run-cpp-tests.{ps1,sh}`, integrados a `check`. **517 tests en total.**

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
| WA-1.2 | `DeviceCapabilities` común | Definir interfaz/expect en commonMain (RAM, low-latency hint, API level abstracto); actual Android actual queda como está; deja el hueco para el actual iOS | `AudioEngineFactory` consume la abstracción | P1 | S | Pendiente |
| WA-1.3 | API USB segregada | Asegurar que los tipos/factories USB (`IUsbAudioManager`, `UsbAudioManagerFactory`) no sean requeridos para usar el resto de la API (interface segregation). Mover a androidMain lo que no necesite estar en common, o documentar como android-only.<br>**Acoplamiento real medido (2026-07-22): sólo 3 puntos.** (a) `AudioBackendType` vive en `domain/usb/UsbAudioTypes.kt` pero **no es un tipo USB** — lo consumen `AudioEngine` y `AudioEngineImpl`; debería mudarse a `domain/`. (b) `IAudioNativeBridge.isUsbBackendAvailable()`. (c) `IAudioNativeBridge.setUsbLatencyProfile()`. Nada más de commonMain depende de `domain/usb/` | Un consumidor sin USB compila para iOS sin stubs USB | P1 | M | Pendiente — **decisión 2026-07-22:** en WA-0.2 se optó por portar `domain/usb/` en su lugar (sigue en commonMain) para no mezclar un cambio de API pública dentro de WA-0.2 |
| WA-1.4 | Extraer `BridgeConcurrency` | Los mutexes por categoría (lifecycle/effects/mode/input) y el mapeo error-code→excepción de `AudioNativeBridge` (**3.352 LOC**) se extraen a commonMain para reutilizarlos en `IosAudioBridge` sin duplicar | AudioNativeBridge delega en la clase común; tests Android verdes | P1 | M | ✅ **HECHO** 2026-07-25 — `internal/bridge/BridgeConcurrency.kt` + 8 tests en commonTest. Los 26 call sites migrados. Ver nota |
| WA-1.5 | Tests Kotlin de commonMain | ~~hoy: cero tests Kotlin~~ → **hoy hay 5 suites / 34 tests** (`ChordGenerator`, `ScaleQuantizerFlow`, `EffectManagerBatch`, + `Format` y `Time` de WA-0.2). **Falta lo central:** `StateSynchronizer`, `AudioEngineImpl`, mapeo de errores | Suite commonTest corriendo en JVM en CI | P1 | M | Parcial |
| WA-1.6 | Factorizar denormals ARM64 | El código FPCR de `PlatformAndroid.cpp` para arm64 es idéntico al que necesitará Apple Silicon → extraer a `PlatformArm64.inc` compartido | PlatformAndroid compila igual; código listo para PlatformApple | P2 | S | ✅ **HECHO** 2026-07-25 — `platform/PlatformIsa.inc`. **La duplicación era más ancha que el FPCR:** el bloque x86_64 (MXCSR) y **las dos funciones de SIMD caps** también eran byte-for-byte idénticas. De ahí el nombre más amplio que el `PlatformArm64.inc` del ticket. En los `.cpp` queda sólo `setAudioThreadPriority()`, que es lo único que difiere de verdad |

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
| WA-2.4 | **`CoreAudioBackend`** | Implementar `IAudioBackend` para iOS (D2): AVAudioEngine + AVAudioSourceNode (output) y AVAudioSinkNode/inputNode (input full-duplex para guitar/input FX). Reglas: el render block invoca directo el mix C++ (sin ObjC dispatch, sin allocs, sin locks); negociación de sample rate/buffer contra el hardware; manejo de formato (Float32 nativo de Core Audio vs pipeline interno) | Sine + cadena de efectos + looper suenan en device real; callback verificado sin allocs (Instruments) | P0 | L | 🟡 **OUTPUT HECHO** 2026-07-23 — `backends/CoreAudioBackend.{h,mm}`, compila y linkea para ambos slices, enchufado en el seam. **Falta:** input/full-duplex y la validación en device (sonido + Instruments) que es WA-4.3 |
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
| WA-3.3 | actuals iOS | `NativeLibraryLoader` (no-op, link estático), `AudioBridgeProvider` (retorna `IosAudioBridge`), `DeviceCapabilities` (ProcessInfo/UIDevice) | `AudioEngineFactory.create()` funciona en iOS | P0 | S | 🟡 **PARCIAL** 2026-07-25 — `NativeLibraryLoader` ✅ y `AudioBridgeProvider` ✅ (ya no lanza `NotImplementedError`). Falta `DeviceCapabilities` (WA-1.2) |
| WA-3.4 | `AudioSessionManager` | Helper iosMain para AVAudioSession: categoría `playAndRecord`, `preferredIOBufferDuration`/`preferredSampleRate`, notificaciones de interrupción y route change expuestas como Flow para que el consumidor (NoisyPad) las mapee a start/stop | Interrupción por llamada entrante pausa y reanuda el engine en sample app | P0 | M |
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

| Fase | Estado |
|---|---|
| **Fase 0** — Análisis y fundaciones | ✅ **CERRADA** — WA-0.1 ✅ · WA-0.2 ✅ · WA-0.3 ✅ (+WA-T.1) · WA-0.4 ✅ |
| **Fase 1** — Quick wins | 🟡 **WA-1.4 ✅** · **WA-1.6 ✅** · WA-1.1 y WA-1.5 parciales (WA-1.4 avanzó ambas) · **falta WA-1.2 y WA-1.3** |
| **Fase 2** — C++ multiplataforma | 🟢 Prácticamente completa — **WA-2.1 ✅ completo** · WA-2.0 ✅ · WA-2.7 ✅ · WA-2.4 output ✅ · WA-2.2 ✅ · **WA-2.3 ✅**. **`libwatermelon_audio.a` linkea de verdad** (link check con `-force_load`, ambos slices). Falta validación en device (WA-4.3), input path iOS, y el bloque grande: WA-2.5 + WA-2.6 |
| **Fase 3** — Kotlin iosMain | 🟢 **WA-3.1 ✅ · WA-3.2 ✅ · WA-3.3 parcial** — `getAudioBridge()` devuelve un bridge real (62 tests iOS, 0 fallas). Falta `AudioSessionManager` (WA-3.4) y `DeviceCapabilities` |
| **Fase 4** — Empaquetado y publicación | 🟡 Iniciada de hecho — el pipeline **ya publica metadata KMP + klibs iOS** desde 1.8.0; falta validar el consumo desde NoisyPad (G1) y el XCFramework (WA-4.1) |

**Próximo paso recomendado:** **WA-3.2** (`IosAudioBridge`). Ya tiene sus tres
precondiciones: cinterop (WA-3.1), `BridgeConcurrency` (WA-1.4) y el `InputNode` unificado,
así que `wma_input_*` ya no es un camino muerto cuando iOS lo estrene.

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
