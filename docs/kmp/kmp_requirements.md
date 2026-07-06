# Requerimiento: KMP/iOS Readiness — watermelon-audio

**Proyecto:** watermelon-audio (`com.watermellonstudios:audio-android`, v1.4.0)
**Documento hermano:** `NoisyPad/docs/kmp/kmp_requirements.md` (consumidor)
**Estado:** PROPUESTO
**Fecha:** 2026-07-05
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

Auditoría 2026-07-05. Puntos de partida excepcionales — el trabajo de extracción de 2026-04 dejó la librería medio camino andado:

**Ya resuelto (no tocar, capitalizar):**

- El módulo `audio/` **ya es KMP**: `commonMain` (52 archivos Kotlin puros: `api/`, `domain/`, `callback/`, `internal/` con `AudioEngineImpl`, `EffectManagerImpl`, `StateSynchronizer`) + `androidMain` (18 archivos). Ya existe el patrón `expect/actual` (`AudioBridgeProvider`, `NativeLibraryLoader`).
- La interfaz `IAudioNativeBridge` (commonMain) ya define el contrato completo del puente — iOS solo necesita otra implementación.
- El C++ está **modularizado en sub-librerías CMake sin dependencias Android**: `watermelon-dsp`, `watermelon-effects`, `watermelon-engines`, `watermelon-voice`, `watermelon-looper` (INTERFACE). Third-party portable: TinySoundFont (MIT), stb_vorbis (PD).
- Abstracciones de plataforma ya existen: `platform/Logger.h` (callback configurable, sin `__android_log_print` en DSP) y `platform/Platform.h` (denormals, prioridad de thread, SIMD caps) con `PlatformAndroid.cpp` como única implementación.
- Ya existe una **C API pura**: `api/watermelon_audio.h/.cpp` con 181 funciones.
- Backends detrás de interfaz: `IAudioBackend` + `BackendManager` (hot-swap) — el diseño ya contempla múltiples backends.
- Tests C++ (googletest) **host-compilables**: dsp/effects/looper/voice/engine/usb suites, scripts `run-cpp-tests.{ps1,sh}`, integrados a `check`.

**Trabajo pendiente (el objeto de este requerimiento):**

| Área | Estado | Problema |
|---|---|---|
| Audio I/O | `OboeBackend` (Android-only) | No hay backend iOS |
| Puente nativo | JNI: `jni_audio_bridge.cpp` 3.332 LOC, **268 JNIEXPORT** | iOS no tiene JNI; además el JNI llama al engine directo, no vía C API |
| C API | 181 funciones | **Gap ~87 funciones** vs JNI (cobertura incompleta: looper avanzado, mixer, regions, transiciones de modo, análisis) |
| Platform C++ | Solo `PlatformAndroid.cpp` | Falta implementación Apple |
| Kotlin androidMain | `Mp4AacTranscoder` (MediaCodec), `UsbAudioManagerImpl`, `TrustedUsbDevicesRepository` (DataStore), `DeviceCapabilities` (Context) | Sin contrapartes iOS ni interfaces comunes en algunos casos |
| Build | CMake vía AGP `externalNativeBuild` | No hay toolchain iOS ni empaquetado XCFramework |
| Targets Gradle | Solo `androidTarget` | Faltan `iosArm64`, `iosSimulatorArm64` |
| Tests Kotlin | Inexistentes | La lógica de `commonMain` (StateSynchronizer, impls) no tiene cobertura propia |

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
| D8 | **Versión Kotlin** | Alinear con NoisyPad (hoy 2.3.20 vs 2.3.21) y mantener lockstep en adelante | La metadata KMP es sensible a la versión del compilador. |

---

## 4. Arquitectura objetivo

```
audio/src/
├── commonMain/kotlin/            (INTACTO: api/, domain/, callback/, internal/ puros)
│   └── internal/bridge/BridgeConcurrency.kt   (NUEVO: mutexes por categoría extraídos
│                                               de AudioNativeBridge, compartidos con iOS)
├── androidMain/kotlin/           (INTACTO: JNI bridge, USB, MediaCodec, DataStore)
├── iosMain/kotlin/               (NUEVO)
│   ├── internal/bridge/IosAudioBridge.kt      (IAudioNativeBridge sobre cinterop)
│   ├── internal/native/NativeLibraryLoader.kt (actual: no-op, link estático)
│   ├── internal/audio/AudioSessionManager.kt  (AVAudioSession: categoría, buffer, interrupciones)
│   └── internal/util/DeviceCapabilities.kt    (actual: ProcessInfo/UIDevice)
├── nativeInterop/cinterop/watermelon_audio.def (NUEVO)
└── main/cpp/
    ├── api/watermelon_audio.h/.cpp   (COMPLETADA: cobertura 1:1 con JNI)
    ├── jni/                          (refactorizado: wrapper delgado de la C API)
    ├── backends/
    │   ├── OboeBackend.*             (Android-only, sin cambios)
    │   ├── LibusbBackend.*           (Android-only, sin cambios)
    │   └── CoreAudioBackend.mm/.cpp  (NUEVO, iOS)
    ├── platform/
    │   ├── PlatformAndroid.cpp       (sin cambios)
    │   └── PlatformApple.cpp         (NUEVO: denormals FPCR arm64 factorizado,
    │                                  pthread time-constraint, os_log sink)
    └── dsp/ effects/ engines/ voice/ looper/ thirdparty/   (SIN CAMBIOS — se recompilan)
```

---

## 5. Requerimientos — Fase 0: Análisis y fundaciones

Prioridades: P0 = bloqueante, P1 = importante, P2 = diferible. Esfuerzo: S (< 1 día), M (días), L (semanas).

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WA-0.1 | **Gap analysis C API vs JNI** | Inventario función por función: 268 JNIEXPORT vs 181 `wma_*`. Clasificar el gap (~87) por categoría (looper avanzado, mixer/regions, mode transitions, spectrum/waveform, metronome, input monitor, benchmark) y marcar cuáles son Android-only (USB) y no se portan | Tabla de cobertura en `docs/kmp/c_api_coverage.md` con estado por función | P0 | M |
| WA-0.2 | Targets iOS en Gradle | Ampliar el convention plugin `watermelon.kmp.native`: `iosArm64` + `iosSimulatorArm64`; alinear Kotlin con NoisyPad (D8). Los 52 archivos de commonMain deben compilar para iOS sin cambios (verificación) | `:audio:compileKotlinIosArm64` verde | P0 | M |
| WA-0.3 | CI macOS | Job en GitHub Actions (runner macOS): compila targets iOS + tests C++ con clang de Xcode (WA-T.1) | Job verde en PR de prueba | P0 | M |
| WA-0.4 | Guardrail de portabilidad C++ | Check de CI que falle si aparece `#include <jni.h>`/`<android/...>` fuera de `jni/`, `backends/Oboe*`, `backends/Libusb*`, `usb/` y `platform/PlatformAndroid.cpp` | CI rojo ante include prohibido (probado con PR sintético) | P1 | S |

---

## 6. Requerimientos — Fase 1: Quick wins (sin tocar el critical path)

Mejoras de valor inmediato para el mantenimiento Android actual, que además despejan el camino iOS.

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WA-1.1 | Logging unificado en Kotlin | `AudioNativeBridge.kt` usa `android.util.Log` directo; migrar a la interfaz `AudioLogger` ya existente en `commonMain/callback/` | Cero `android.util.Log` fuera de un actual Android de `AudioLogger` | P1 | S |
| WA-1.2 | `DeviceCapabilities` común | Definir interfaz/expect en commonMain (RAM, low-latency hint, API level abstracto); actual Android actual queda como está; deja el hueco para el actual iOS | `AudioEngineFactory` consume la abstracción | P1 | S |
| WA-1.3 | API USB segregada | Asegurar que los tipos/factories USB (`IUsbAudioManager`, `UsbAudioManagerFactory`) no sean requeridos para usar el resto de la API (interface segregation). Mover a androidMain lo que no necesite estar en common, o documentar como android-only | Un consumidor sin USB compila para iOS sin stubs USB | P1 | M |
| WA-1.4 | Extraer `BridgeConcurrency` | Los mutexes por categoría (lifecycle/effects/mode/input) y el mapeo error-code→excepción de `AudioNativeBridge` (2.619 LOC) se extraen a commonMain para reutilizarlos en `IosAudioBridge` sin duplicar | AudioNativeBridge delega en la clase común; tests Android verdes | P1 | M |
| WA-1.5 | Tests Kotlin de commonMain | Crear `commonTest`: cobertura de `StateSynchronizer`, `EffectManagerImpl`, mapeo de errores (hoy: cero tests Kotlin — deuda) | Suite commonTest corriendo en JVM en CI | P1 | M |
| WA-1.6 | Factorizar denormals ARM64 | El código FPCR de `PlatformAndroid.cpp` para arm64 es idéntico al que necesitará Apple Silicon → extraer a `PlatformArm64.inc` compartido | PlatformAndroid compila igual; código listo para PlatformApple | P2 | S |

---

## 7. Requerimientos — Fase 2: C++ multiplataforma

| ID | Requerimiento | Detalle | Criterio de aceptación | Prio | Esf |
|---|---|---|---|---|---|
| WA-2.1 | Build CMake iOS | Toolchain/presets iOS (device arm64 + simulator arm64): compilar `watermelon-dsp/effects/engines/voice/looper` + core + nodes + api como **librería estática** por slice. Excluir del build iOS: `jni/`, `usb/`, `OboeBackend`, `LibusbBackend`, `PlatformAndroid.cpp`. Definir `USE_NEON=1` en arm64 Apple | `libwatermelon_audio.a` para ambos slices compila con Xcode clang, C++20 | P0 | L |
| WA-2.2 | `PlatformApple.cpp` | Implementar `wma::platform`: denormals (FPCR, reutiliza WA-1.6), `setAudioThreadPriority()` (pthread `THREAD_TIME_CONSTRAINT_POLICY` — solo como refuerzo: el thread de Core Audio ya viene priorizado), SIMD caps (NEON fijo en arm64) | `engine_tests` linkea y pasa en macOS con PlatformApple | P0 | M |
| WA-2.3 | Logger Apple | Sink `os_log` por defecto en builds Apple; callback configurable idéntico a Android | Log visible en Console.app desde sample app | P1 | S |
| WA-2.4 | **`CoreAudioBackend`** | Implementar `IAudioBackend` para iOS (D2): AVAudioEngine + AVAudioSourceNode (output) y AVAudioSinkNode/inputNode (input full-duplex para guitar/input FX). Reglas: el render block invoca directo el mix C++ (sin ObjC dispatch, sin allocs, sin locks); negociación de sample rate/buffer contra el hardware; manejo de formato (Float32 nativo de Core Audio vs pipeline interno) | Sine + cadena de efectos + looper suenan en device real; callback verificado sin allocs (Instruments) | P0 | L |
| WA-2.5 | **Completar la C API** | Cerrar el gap de WA-0.1: agregar a `watermelon_audio.h` las ~87 funciones faltantes (excluyendo USB). Reglas de ABI: handles opacos, códigos de error enteros (sin excepciones cruzando la frontera), sin tipos C++ en firmas, documentación de thread-safety por función (RT-safe vs coordinación) | Cobertura 1:1 con el JNI no-USB según tabla WA-0.1 | P0 | L |
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

| ID | Requerimiento | Detalle | Prio |
|---|---|---|---|
| WA-T.1 | Tests C++ en macOS | Los googletest existentes (dsp/effects/looper/voice/engine) ya compilan en host: agregar job macOS con clang de Xcode — detecta problemas de portabilidad (MSVC/MinGW vs clang-apple) antes de tocar iOS | P0 (parte de WA-0.3) |
| WA-T.2 | commonTest Kotlin | Cobertura de la lógica común (WA-1.5) corriendo en JVM y luego contra `IosAudioBridge` en simulador (WA-3.2) | P1 |
| WA-T.3 | Smoke cinterop | Test de simulador: round-trip completo `AudioEngineFactory.create()` → start → setXY → addEffect → stop | P0 |
| WA-T.4 | Verificación RT | Sesión de Instruments (Time Profiler + Allocations) sobre el render callback de `CoreAudioBackend`: cero allocs, cero locks, sin prioridad invertida | P0 (criterio de WA-2.4) |

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
| Gap real de la C API mayor al estimado (~87) | Cronograma F2 | WA-0.1 primero — es análisis puro y barato; el refactor WA-2.6 es incremental por categorías |
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
- **G2 (ruta crítica):** el sonido en iOS depende de WA-2.4 + WA-3 + WA-4. Es la ruta crítica de todo el programa KMP; conviene arrancar WA-2.4 (CoreAudioBackend) apenas cierre WA-2.1.

---

## 15. Métricas de éxito

1. **Reutilización C++:** 100% de dsp/effects/engines/voice/looper compilando para iOS sin modificaciones de código (solo build).
2. **Reutilización Kotlin:** los 52 archivos de commonMain sin cambios; iosMain < 10 archivos nuevos.
3. **Paridad de API:** cobertura C API = 100% del JNI no-USB (tabla WA-0.1 en verde).
4. **Latencia iOS:** round-trip medido y documentado; objetivo indicativo ≤ 20 ms output-only con buffer 128–256 frames (validar contra la experiencia Android actual).
5. **Regresión Android:** suite C++ + smoke NoisyPad Android sin desviaciones tras WA-2.6.
