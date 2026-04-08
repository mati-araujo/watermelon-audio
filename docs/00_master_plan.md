# NoisyPad — Audio Module Extraction: Master Plan

**De modulo Android monolitico a libreria multiplataforma independiente**

*v1.0 — Abril 2026 | Plan de implementacion*

---

## Tabla de Contenidos

1. [Objetivo](#1-objetivo)
2. [Estado Actual](#2-estado-actual)
3. [Arquitectura Target](#3-arquitectura-target)
4. [Fases de Implementacion](#4-fases-de-implementacion)
5. [Dependencias Entre Fases](#5-dependencias-entre-fases)
6. [Criterios de Exito Globales](#6-criterios-de-exito-globales)
7. [Riesgos Transversales](#7-riesgos-transversales)
8. [Convenciones](#8-convenciones)

---

## 1. Objetivo

Extraer el modulo `audio/` de NoisyPad a un repositorio independiente publicable via GitHub Packages, con una arquitectura que permita:

- **Consumo por multiples proyectos Android** sin modificaciones
- **Extensibilidad** — agregar effects/engines sin tocar el core
- **Testabilidad** — unit tests de C++ y Kotlin sin dispositivo fisico
- **Base para multiplataforma** — iOS, desktop, web, server via KMP + backends nativos

### No-Goals (explicitamente fuera de alcance)

- Implementar backends iOS/desktop/web (Phase 4 es spec-only)
- Cambiar la funcionalidad de audio existente
- Agregar nuevos effects o engines
- Migrar feature-usb (queda en NoisyPad, consume la libreria)

---

## 2. Estado Actual

### Metricas del modulo

| Metrica | Valor |
|---------|-------|
| C++ headers | 135+ |
| C++ source files | 60+ |
| Kotlin files | 65+ |
| LOC estimado (C++) | ~42,000 |
| LOC estimado (Kotlin) | ~14,000 |
| Effects DSP | 20 |
| Synth engines | 7 |
| Max polyphony | 16 voices |
| Looper tracks | 8 (with loop regions, master volume, peak metering) |
| Mixer | 8 track faders + master, mute/solo, peak meters |
| Backends | 2 (Oboe, libusb) |
| JNI functions | 170+ (incluyendo ~37 looper functions) |

### Problemas identificados (de la auditoria)

| ID | Problema | Severidad | Fase que lo resuelve |
|----|----------|-----------|---------------------|
| P1 | AudioEngine es god class (~1000 LOC, 40+ atomics, 35+ includes) | CRITICO | Phase 1 |
| P2 | Singleton global (g_jniState, BackendManager) | CRITICO | Phase 0 |
| P3 | AudioEngine hereda oboe::AudioStreamCallback | ALTO | Phase 0 |
| P4 | JNI es la unica API de C++ (no C API) | ALTO | Phase 0 |
| P5 | Logging hardcoded a Android | MEDIO | Phase 0 |
| P6 | Platform specifics sin abstraccion | MEDIO | Phase 0 |
| P7 | Convention plugin acoplada a NoisyPad | MEDIO | Phase 3 |

---

## 3. Arquitectura Target

### Estructura del repositorio extraido

```
watermelon-audio/
├── audio-core/                        C++ library (CMake, zero platform deps)
│   ├── include/watermelon/audio/
│   │   ├── watermelon_audio.h             C API publica
│   │   ├── watermelon_audio_effects.h     Effect types y parametros
│   │   ├── watermelon_audio_engines.h     Engine types y parametros
│   │   └── watermelon_audio_types.h       Tipos compartidos
│   ├── src/
│   │   ├── platform/                      Abstraccion: logging, SIMD, threading
│   │   ├── dsp/                           DSP primitives (BiquadFilter, FDN, LFO...)
│   │   ├── effects/                       20 effects + EffectChain
│   │   ├── engines/                       7 synth engines + registry
│   │   ├── voice/                         VoiceManager, VoicePool
│   │   ├── graph/                         AudioGraph + nodes
│   │   ├── looper/                        AudioLooper, TrackBuffer
│   │   ├── sequencer/                     ArpSequencer
│   │   ├── core/                          AudioEngine facade
│   │   └── backends/                      IAudioBackend + implementations
│   ├── tests/                             Google Test unit tests
│   └── CMakeLists.txt
│
├── audio-kotlin/                      KMP Kotlin wrapper
│   ├── commonMain/                        domain models, interfaces, expect
│   ├── androidMain/                       JNI bridge (actual)
│   └── iosMain/                           cinterop bridge (futuro)
│
├── audio-android/                     Android integration module
│   ├── OboeBackend integration
│   ├── USB backend integration
│   └── Android lifecycle helpers
│
├── gradle/libs.versions.toml
├── build-logic/                       Convention plugins propios
└── .github/workflows/                 CI/CD
```

### Diagrama de dependencias

```
NoisyPad (app)
    └── depends on ──→ watermelon-audio (GitHub Package)
                           ├── audio-android  (Android-specific)
                           ├── audio-kotlin   (KMP common + platform)
                           └── audio-core     (C++ via CMake)

Future iOS App
    └── depends on ──→ watermelon-audio
                           ├── audio-ios      (CoreAudio backend)
                           ├── audio-kotlin   (KMP common + iOS actual)
                           └── audio-core     (C++ via CMake)
```

---

## 4. Fases de Implementacion

| Fase | Nombre | Spec | Estado | Prerequisito |
|------|--------|------|--------|-------------|
| **0** | Preparacion | [phase0_preparation.md](phase0_preparation.md) | **COMPLETADA** (0A-0D done, 0E pendiente) | — |
| **1** | Modularizacion C++ | [phase1_cpp_modularization.md](phase1_cpp_modularization.md) | **COMPLETADA** (1A-1G done) | Phase 0 ✓ |
| **2** | KMP Kotlin Layer | [phase2_kmp_kotlin.md](phase2_kmp_kotlin.md) | **COMPLETADA** (2A-2E done) | Phase 1 ✓ |
| **3** | Repositorio y Distribucion | [phase3_repo_distribution.md](phase3_repo_distribution.md) | **COMPLETADA** (3A-3E done, 3D/3F pendiente) | Phase 2 ✓ |
| **4** | Multiplataforma | [phase4_multiplatform.md](phase4_multiplatform.md) | Spec-only | Phase 3 |

### Resumen de sub-fases

**Phase 0 — Preparacion (desacoplar sin mover)**
- 0A: Abstraer logging y platform specifics
- 0B: Eliminar herencia oboe::AudioStreamCallback
- 0C: Crear C API (watermelon_audio.h)
- 0D: Eliminar singletons, habilitar instanciacion
- 0E: Validacion — NoisyPad funciona identico post-refactor

**Phase 1 — Modularizacion C++ (reestructurar el core)**
- 1A: Extraer DSP primitives como sub-library
- 1B: Extraer Effects como sub-library
- 1C: Extraer Engines como sub-library
- 1D: Extraer Voice System
- 1D.2: Extraer Looper (loop regions, master volume, adaptive crossfade, peak metering)
- 1E: Refactorizar AudioEngine de god class a facade
- 1F: Engine/Effect dynamic registry
- 1G: C++ unit tests con Google Test (incluyendo looper/mixer tests)

**Phase 2 — KMP Kotlin Layer (preparar para multiplataforma) — COMPLETADA 2026-04-08**
- 2A: Mover domain/ a commonMain ✓ (52 files)
- 2B: Crear expect/actual para bridge ✓ (IAudioNativeBridge interface + getAudioBridge() expect/actual)
- 2C: Abstraer dependencias Android en Kotlin ✓ (Log→AudioLogger, NativeLibraryLoader expect/actual)
- 2D: KMP AudioEngine interface ✓ (AudioEngine.kt + AudioEngineImpl.kt + factories en commonMain)
- 2E: Validacion end-to-end ✓ (build green + app funciona identico en device)

**Phase 3 — Repositorio y Distribucion (ship it) — COMPLETADA 2026-04-08**
- 3A: Crear repositorio independiente ✓ (watermelon-audio, standalone build green)
- 3B: Build system standalone ✓ (convention plugin, libs.versions.toml, maven-publish)
- 3C: GitHub Packages publishing ✓ (mati-araujo/watermelon-audio, audio + audio-android artifacts)
- 3D: CI/CD pipeline — PENDIENTE (GitHub Actions)
- 3E: Migrar NoisyPad a consumir el package ✓ (audio/ eliminado, consume audio-android:1.0.0-SNAPSHOT)
- 3F: Documentacion publica ✓ (README.md, C++ README actualizado, proguard corregido)

**Phase 4 — Multiplataforma (disenar el futuro)**
- 4A: iOS backend spec (CoreAudio)
- 4B: Desktop backend spec (PortAudio)
- 4C: Web backend spec (Emscripten + WebAudio)
- 4D: Server backend spec (offline rendering)

---

## 5. Dependencias Entre Fases

```
Phase 0A ──→ Phase 0B ──→ Phase 0C ──→ Phase 0D ──→ Phase 0E
  (logging)   (oboe)       (C API)      (singletons)  (validate)
                                │
                                ▼
                           Phase 1A ──→ Phase 1B ──→ Phase 1C
                           (dsp)        (effects)     (engines)
                              │            │             │
                              ├────────────┴─────────────┘
                              │            │
                              ▼            ▼
                         Phase 1D.2   Phase 1D ──→ Phase 1E ──→ Phase 1F
                         (looper)     (voice)      (facade)     (registry)
                              │                       │
                              └───────────────────────┘
                                                      │
                              Phase 1G ◄──────────────┘
                              (tests)        │
                                             ▼
                                        Phase 2A ──→ Phase 2B ──→ Phase 2C
                                        (domain)     (bridge)     (android)
                                                        │
                                                        ▼
                                        Phase 2D ──→ Phase 2E
                                        (KMP API)    (validate)
                                                        │
                                                        ▼
                                        Phase 3A ──→ Phase 3B ──→ Phase 3C
                                        (repo)       (build)      (publish)
                                                                      │
                                        Phase 3D ──→ Phase 3E ──→ Phase 3F
                                        (CI/CD)      (migrate)    (docs)
```

**Fases paralelizables:**
- 1A, 1B, 1C pueden hacerse en paralelo (DSP, Effects, Engines son independientes)
- 1D y 1D.2 pueden hacerse en paralelo (Voice y Looper son independientes)
- 1G (tests) puede empezar tan pronto como 1A este listo
- 3D (CI/CD) puede hacerse en paralelo con 3B/3C

---

## 6. Criterios de Exito Globales

### Al finalizar Phase 0 (Preparacion) — COMPLETADA 2026-04-06
- [x] NoisyPad compila (build green 4 ABIs + app). Tests manuales pendientes.
- [x] AudioEngine.h NO incluye `<oboe/Oboe.h>` (forward declaration only)
- [x] `watermelon_audio.h` con C API funcional (181 funciones, 21 categorias, zero stubs)
- [x] JNI lifecycle usa C API (`ensureEngine()` → `wma_engine_create()`). JNI functions
      aun llaman `g_jniState.engine->` directamente (deferido, no blocker).
- [x] BackendManager instanciable (constructor publico, `setGlobalInstance()`).
      `getInstance()` sigue disponible para legacy (~40 call sites).
- [x] Logging via `platform/Logger.h` (28 archivos migrados, zero `__android_log_print` directo)
- [x] Denormal flushing via `platform/Platform.h` (zero inline assembly fuera de platform/)

### Al finalizar Phase 1 (Modularizacion) — COMPLETADA 2026-04-07
- [x] 5 CMake sub-libraries: watermelon-dsp (30 files), watermelon-effects (53 files),
      watermelon-engines (9 files), watermelon-voice (10 files), watermelon-looper (3 files)
- [x] 7 subsistemas extraidos de AudioEngine: WaveformCapture, OutputStage, FadeController,
      DualTouchManager, ChordHarmony, OscillatorBank, SynthEngineDispatcher (1,716 LOC total)
- [x] AudioEngine: 4,418 → 3,208 LOC (-27%). processAudioBlock: 660 → 120 LOC (-82%)
- [x] Member variables: 65+ → ~30 (-35 miembros)
- [x] EffectRegistry: 20 effects registrados dinamicamente (switch de 60 LOC eliminado)
- [x] 8 test files, 36 unit tests passing (Google Test + MinGW host toolchain)
- [x] Build green en 4 ABIs + app completa
- [ ] **PENDIENTE:** AudioEngine.h aun tiene 1,169 LOC (target era <250). Los includes y
      delegates inline se pueden reducir con forward declarations, pero es cosmético.
- [ ] **PENDIENTE:** Tests manuales de funcionalidad en device + latency benchmark

### Al finalizar Phase 2 (KMP) — COMPLETADA 2026-04-08
- [x] `commonMain/` contiene 52 archivos: domain models, API interfaces, factories, engine impl, sync, utils
- [x] `IAudioNativeBridge` interface (~70 methods) en commonMain + `getAudioBridge()` expect/actual
- [x] `NativeLibraryLoader` expect/actual (System.loadLibrary en Android)
- [x] `AudioLogger` callback reemplaza `android.util.Log` en archivos commonMain
- [x] `AudioEngine.kt` interface + `AudioEngineImpl.kt` en commonMain
- [x] `EffectManagerImpl.kt` + `StateSynchronizer.kt` en commonMain
- [x] 3 factories (AudioEngine, EffectManager, StateSynchronizer) en commonMain
- [x] Zero imports de Android en commonMain (verified)
- [x] NoisyPad funciona identico (manual test passed on device)
- [x] 18 archivos en androidMain (JNI bridge, USB, mode, latency, DeviceCapabilities)
- **Nota:** Se uso interface-based abstraction en vez de expect/actual class para el bridge
  (mas flexible, menos acoplamiento). AGP 9.0 requiere `android.builtInKotlin=false` +
  explicit `kotlin-android` en non-KMP modules.

### Al finalizar Phase 3 (Distribucion) — COMPLETADA 2026-04-08
- [x] Repo `mati-araujo/watermelon-audio` en GitHub (privado)
- [x] Build system standalone: convention plugin `watermelon.kmp.native`, libs.versions.toml minimal
- [x] Package publicado en GitHub Packages: `audio` (KMP metadata) + `audio-android` (AAR)
- [x] NoisyPad consume `com.watermellonstudios:audio-android:1.0.0-SNAPSHOT` — app funciona identico
- [x] `audio/` eliminado de NoisyPad (306 files, -71,898 LOC)
- [x] Convention plugins KMP/Native removidos de NoisyPad build-logic
- [x] AGP 9 workaround revertido (builtInKotlin=false ya no necesario)
- [x] README.md con architecture, installation, quick start
- [x] C++ README actualizado (20 effects, 7 engines, 5 sub-libraries)
- [x] proguard-rules.pro corregido (packages actualizados)
- [ ] **PENDIENTE:** CI/CD pipeline (GitHub Actions — build + test on PR, publish on tag)
- [ ] **PENDIENTE:** Version 1.0.0 release (actualmente SNAPSHOT)

---

## 7. Riesgos Transversales

| Riesgo | Prob. | Impacto | Mitigacion |
|--------|-------|---------|------------|
| **Regresion de latencia** al agregar indirection (C API, facade) | Media | Critico | Benchmark antes/despues de cada sub-fase con `LatencyBenchmark.h`. Zero vtable lookups en hot path. |
| **Rotura de RT-safety** al refactorizar | Baja | Critico | Checklist RT-safety en cada PR. No cambiar el audio callback path, solo ownership. |
| **LTO degradation** al separar en sub-libraries | Media | Medio | Mantener `INTERPROCEDURAL_OPTIMIZATION` en CMake cross-library. Medir binary size. |
| **KMP complexity** para audio nativo | Alta | Medio | Empezar Android-only. KMP solo para Kotlin layer. C++ se compila per-platform. |
| **Breaking NoisyPad** durante migracion | Media | Alto | Feature branch dedicado. NoisyPad tests como gate. Rollback plan en cada fase. |
| **Scope creep** — agregar features durante refactor | Alta | Medio | Regla: ZERO features nuevas. Solo refactoring. Features van despues. |

---

## 8. Convenciones

### Nomenclatura

| Concepto | Convencion | Ejemplo |
|----------|-----------|---------|
| C API functions | `wma_` prefix + snake_case | `wma_engine_create()` |
| C API types | `WmaEngine`, `WmaEffectType` | `WmaEngine*` |
| C++ internal | CamelCase (sin cambios) | `AudioEngine`, `EffectChain` |
| Kotlin package | `com.watermellonstudios.audio` | (sin cambios) |
| Gradle artifact | `audio-{platform}` | `audio-android`, `audio-kotlin` |

### Branches

- `refactor/phase0-preparation`
- `refactor/phase1-cpp-modularization`
- `refactor/phase2-kmp-kotlin`
- `refactor/phase3-repo-distribution`

### Checkpoints

Cada sub-fase (0A, 0B, ...) tiene:
1. **Pre-condiciones** — que debe estar listo antes de empezar
2. **Tareas** — pasos concretos de implementacion
3. **Verificacion** — comandos o tests que confirman completitud
4. **Post-condiciones** — estado del sistema al terminar

El formato de verificacion es executable:
```bash
# Ejemplo: verificar que AudioEngine no depende de Oboe
grep -r "oboe::" audio/src/main/cpp/core/AudioEngine.h && echo "FAIL" || echo "PASS"
```

### Versionado

- Pre-extraction: version interna del modulo en NoisyPad
- Post-extraction: Semantic Versioning (`1.0.0`)
  - MAJOR: breaking C API changes
  - MINOR: new effects, engines, features
  - PATCH: bug fixes, performance improvements
