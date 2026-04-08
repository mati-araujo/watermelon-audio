# Phase 2 — KMP Kotlin Layer — COMPLETADA 2026-04-08

**Preparar la capa Kotlin para Kotlin Multiplatform**

*Prerequisito: Phase 1 completada (C++ modularizado, C API funcional)*
*Phase 0 completada 2026-04-06. Phase 1 completada 2026-04-07.*
*Phase 2 completada 2026-04-08.*

**Estado post-Phase 1 relevante para Phase 2:**
- C++ modularizado: 5 sub-libraries (dsp, effects, engines, voice, looper) + 7 facade subsistemas
- C API: `watermelon_audio.h` (181 funciones), `watermelon_audio_internal.h` (WmaEngine struct)
- Platform abstractions: `Logger.h`, `Platform.h`
- AudioEngine es facade (~3,200 LOC), processAudioBlock ~120 LOC
- EffectRegistry dinámico (20 effects)
- 36 C++ unit tests passing (Google Test + MinGW host)
- Build green en 4 ABIs + app completa

---

## Tabla de Contenidos

1. [Objetivo](#1-objetivo)
2. [Sub-fase 2A: Mover Domain a commonMain](#2-sub-fase-2a-mover-domain-a-commonmain)
3. [Sub-fase 2B: Crear expect/actual para Bridge](#3-sub-fase-2b-crear-expectactual-para-bridge)
4. [Sub-fase 2C: Abstraer Dependencias Android](#4-sub-fase-2c-abstraer-dependencias-android)
5. [Sub-fase 2D: KMP AudioEngine Interface](#5-sub-fase-2d-kmp-audioengine-interface)
6. [Sub-fase 2E: Validacion End-to-End](#6-sub-fase-2e-validacion-end-to-end)

---

## 1. Objetivo

Reorganizar la capa Kotlin del modulo audio para que los modelos, interfaces,
y logica de negocio sean **Kotlin Multiplatform**, mientras que las implementaciones
platform-specific (JNI, native loading) queden en source sets correspondientes.

### Estructura target

```
audio-kotlin/
├── commonMain/
│   └── com/watermellonstudios/audio/
│       ├── api/                        Interfaces publicas
│       │   ├── AudioEngine.kt              Main engine interface
│       │   ├── IEffectManager.kt            Effect chain control
│       │   └── IAudioBackendManager.kt      Backend control
│       ├── domain/                     Pure models (zero deps)
│       │   ├── effect/                     EffectType, EffectState, EffectParameter
│       │   ├── engine/                     EngineType, EngineParameterDef
│       │   ├── state/                      AudioState, EngineLifecycle, StreamInfo
│       │   ├── mode/                       AudioMode, TransitionState
│       │   ├── oscillator/                 OscillatorType
│       │   ├── modulator/                  ModulatorType
│       │   ├── scale/                      ScaleMode, ScaleConfig
│       │   └── error/                      NativeBridgeException
│       ├── callback/                   Dependency inversion
│       │   ├── AudioLogger.kt
│       │   └── IAudioAnalytics.kt
│       └── internal/                   Shared internal logic
│           ├── bridge/
│           │   └── NativeAudioBridge.kt    expect class
│           ├── sync/
│           │   └── StateSynchronizer.kt    Reusable (uses bridge expect)
│           └── util/
│               ├── ScaleQuantizer.kt       Pure math
│               └── ChordGenerator.kt       Pure math
│
├── androidMain/
│   └── com/watermellonstudios/audio/
│       └── internal/
│           ├── bridge/
│           │   └── NativeAudioBridge.kt    actual (JNI implementation)
│           ├── native/
│           │   └── NativeLibraryLoader.kt  System.loadLibrary
│           └── engine/
│               └── AudioEngineImpl.kt      Android-specific impl
│
└── iosMain/  (futuro, Phase 4)
    └── com/watermellonstudios/audio/
        └── internal/
            ├── bridge/
            │   └── NativeAudioBridge.kt    actual (cinterop C API)
            └── native/
                └── NativeLibraryLoader.kt  iOS dynamic framework
```

---

## 2. Sub-fase 2A: Mover Domain a commonMain

### Pre-condiciones
- Phase 1 completada

### Contexto

Los archivos en `audio/domain/` son pure Kotlin sin dependencias Android.
Son el candidato ideal para commonMain — se pueden mover casi sin cambios.

### Archivos a mover

| Package actual | Archivos | Dependencias Android |
|---------------|---------|---------------------|
| `domain/effect/` | EffectType.kt, EffectState.kt, EffectParameter.kt, EffectConstants.kt, PedalPresets.kt | NINGUNA |
| `domain/engine/` | EngineType.kt, EngineParameterDef.kt | NINGUNA |
| `domain/state/` | AudioState.kt, EngineLifecycle.kt, StreamInfo.kt | NINGUNA |
| `domain/mode/` | AudioMode.kt, ModeTransitionState.kt, TransitionPhase.kt | NINGUNA |
| `domain/oscillator/` | OscillatorType.kt | NINGUNA |
| `domain/modulator/` | ModulatorType.kt | NINGUNA |
| `domain/scale/` | ScaleMode.kt, ScaleConfig.kt | NINGUNA |
| `domain/error/` | NativeBridgeException.kt | NINGUNA |
| `domain/usb/` | UsbAudioEvents.kt, UsbAudioTypes.kt, UsbDeviceCompatibility.kt | NINGUNA |
| `callback/` | AudioLogger.kt, AudioAnalyticsListener.kt, IAudioAnalytics.kt, ICrashReporter.kt | NINGUNA |

**Nota (Phase 13):** Los modelos del looper (`LooperState`, `TrackInfo` con
`loopStartFrame`, `loopEndFrame`, `peakLevel`, `masterVolume`, `soloTrackIndex`)
viven en `core-domain` de NoisyPad, no en el modulo `audio/`. Sin embargo,
la interface `ILooperController` y sus metodos (incluyendo los nuevos
`setMasterVolume`, `setTrackLoopRegion`, `resetTrackLoopRegion`) deben
considerarse como parte del API surface de la libreria. Evaluar si mover
la interfaz del looper controller a `audio-kotlin/commonMain/api/`.

### Tareas

#### 2A.1 — Migrar build.gradle.kts a KMP

```kotlin
// audio/build.gradle.kts
plugins {
    id("noisypad.kmp.native")  // New convention plugin for KMP + native
    // OR manual setup:
    kotlin("multiplatform")
    id("com.android.library")
}

kotlin {
    androidTarget()
    // iosArm64()  // Phase 4
    // iosSimulatorArm64()  // Phase 4

    sourceSets {
        commonMain.dependencies {
            implementation(libs.kotlinx.coroutines.core)
        }
        androidMain.dependencies {
            implementation(libs.kotlinx.coroutines.android)
            implementation(libs.androidx.datastore)
            implementation(libs.androidx.lifecycle.runtime)
        }
    }
}

android {
    namespace = "com.watermellonstudios.audio"
    // CMake config remains in android block
}
```

#### 2A.2 — Mover archivos de domain/ a commonMain/

Reorganizar la estructura de directorios:
```
src/main/kotlin/  →  src/commonMain/kotlin/  (domain, callback, api interfaces)
                     src/androidMain/kotlin/  (bridge, engine impl, native loader)
```

#### 2A.3 — Verificar que commonMain compila sin Android SDK

Los archivos en commonMain no deben importar:
- `android.*`
- `androidx.*`
- `java.io.File` (usar `kotlinx.io` o expect/actual)
- Ninguna clase de Android framework

#### 2A.4 — Mover utilities puras a commonMain

- `ScaleQuantizer.kt` — pure math, zero Android deps
- `ChordGenerator.kt` — pure math
- `DeviceCapabilities.kt` — QUEDA en androidMain (usa Android APIs)

### Verificacion

```bash
# V-2A.1: commonMain directory exists with domain models
test -d audio/src/commonMain/kotlin/com/watermellonstudios/audio/domain && echo "PASS" || echo "FAIL"

# V-2A.2: No Android imports in commonMain
grep -rn "import android\.\|import androidx\." \
  audio/src/commonMain/kotlin/ && echo "FAIL" || echo "PASS"

# V-2A.3: Build compiles
./gradlew :audio:assembleDebug

# V-2A.4: Domain models accessible from commonMain
grep -rn "EffectType\|AudioState\|EngineType" audio/src/commonMain/ --include="*.kt" | head -5
```

### Post-condiciones
- `commonMain/` contiene todos los domain models
- Zero imports de Android en commonMain
- `androidMain/` contiene solo implementaciones platform-specific
- Build green

---

## 3. Sub-fase 2B: Crear expect/actual para Bridge

### Pre-condiciones
- 2A completada

### Contexto

`AudioNativeBridge.kt` (2602 lineas) es el corazon del JNI bridge.
Necesitamos:
1. Extraer la **interfaz** del bridge a commonMain (expect)
2. Mantener la **implementacion JNI** en androidMain (actual)
3. Disenar para que iOS (cinterop) pueda implementar su propio actual

### Tareas

#### 2B.1 — Definir la interfaz del bridge en commonMain

```kotlin
// commonMain: internal/bridge/NativeAudioBridge.kt

expect class NativeAudioBridge {
    // Lifecycle
    suspend fun startEngine(): Result<Unit>
    suspend fun stopEngine(): Result<Unit>
    suspend fun startEngineWithFade(fadeTimeMs: Int): Result<Unit>
    suspend fun stopEngineWithFade(fadeTimeMs: Int): Result<Unit>
    suspend fun pauseEngineWithFade(fadeTimeMs: Int): Result<Unit>
    suspend fun resumeEngineWithFade(fadeTimeMs: Int): Result<Unit>

    // Real-time (lock-free, no suspend)
    fun setXY(x: Float, y: Float)
    fun setFrequencyAndAmplitude(freq: Float, amp: Float)
    fun setFrequencyRange(minHz: Float, maxHz: Float)
    fun setMasterVolume(volume: Float)
    fun setBpm(bpm: Float)
    fun setOscillatorType(type: Int)
    fun setModulatorType(type: Int)

    // Engine
    fun setEngineType(type: Int)
    fun setEngineParameter(paramId: Int, value: Float)
    fun getEngineType(): Int

    // Effects
    suspend fun addEffect(type: Int): Result<Int>
    suspend fun removeEffect(index: Int): Result<Unit>
    fun setEffectParameter(effectIndex: Int, paramId: Int, value: Float)
    fun getEffectParameter(effectIndex: Int, paramId: Int): Float
    fun setEffectBypass(index: Int, bypass: Boolean)
    fun reorderEffects(fromIndex: Int, toIndex: Int)
    fun getEffectChainSize(): Int
    fun getEffectType(index: Int): Int

    // Routing
    fun setRoutingMode(mode: Int)
    fun setParallelMix(mix: Float)
    fun setFeedbackAmount(amount: Float)

    // State
    fun getEngineState(): Int
    fun getStateVersion(): Long
    fun isPaused(): Boolean
    fun hasError(): Boolean
    fun getStreamInfo(): Triple<Int, Int, Float>?

    // SoundFont
    fun loadSoundFontFromPath(path: String): Boolean
    fun unloadSoundFont()
    fun setSoundFontPreset(presetIndex: Int)
    fun soundFontPresetCount(): Int
    fun sfNoteOn(touchId: Int, midiNote: Int, velocity: Float)
    fun sfNoteOff(touchId: Int)

    // Looper (Phase 11 + 13)
    fun looperPrepareTrack(trackIndex: Int, lengthFrames: Int, sampleRate: Int): Boolean
    fun looperStartRecording(trackIndex: Int)
    fun looperStopRecording()
    fun looperStartOverdub(trackIndex: Int)
    fun looperPause()
    fun looperResume()
    fun looperClearTrack(trackIndex: Int)
    fun looperClearAll()
    fun looperSetTrackVolume(trackIndex: Int, volume: Float)
    fun looperSetTrackPan(trackIndex: Int, pan: Float)
    fun looperSetTrackMuted(trackIndex: Int, muted: Boolean)
    fun looperSetTrackSpeed(trackIndex: Int, speed: Float)
    fun looperGetTrackPeakLevel(trackIndex: Int): Float
    fun looperGetTrackProgress(trackIndex: Int): Float
    fun looperSetMasterVolume(volume: Float)       // Phase 13A
    fun looperGetMasterVolume(): Float              // Phase 13A
    fun looperSetTrackLoopRegion(trackIndex: Int, startFrame: Int, endFrame: Int)  // Phase 13D
    fun looperResetTrackLoopRegion(trackIndex: Int)  // Phase 13D
    fun looperGetTrackLoopStart(trackIndex: Int): Int   // Phase 13D
    fun looperGetTrackLoopEnd(trackIndex: Int): Int     // Phase 13D
    fun looperImportTrack(trackIndex: Int, wavPath: String): Boolean
    fun looperExportTrack(trackIndex: Int, wavPath: String): Boolean
    fun looperExportMix(wavPath: String): Boolean

    // Waveform
    fun getWaveformSamples(buffer: FloatArray, maxSamples: Int): Int
}
```

#### 2B.2 — Implementar actual en androidMain

```kotlin
// androidMain: internal/bridge/NativeAudioBridge.kt

actual class NativeAudioBridge {
    // Copy existing implementation from current AudioNativeBridge.kt
    // All external fun declarations stay here
    // All mutex-protected suspend functions stay here

    private external fun nativeStartEngine()
    private external fun nativeStopEngine()
    // ... (existing 170+ JNI bindings)

    actual suspend fun startEngine(): Result<Unit> = withContext(Dispatchers.Default) {
        lifecycleMutex.withLock {
            // existing implementation
        }
    }

    actual fun setXY(x: Float, y: Float) {
        nativeSetXY(x, y)
    }

    // ... etc
}
```

#### 2B.3 — Preparar stub para iosMain (placeholder)

```kotlin
// iosMain: internal/bridge/NativeAudioBridge.kt

actual class NativeAudioBridge {
    // C interop calls to watermelon_audio.h
    // Will be implemented in Phase 4

    actual suspend fun startEngine(): Result<Unit> {
        TODO("iOS bridge not yet implemented")
    }

    // ... stubs for all functions
}
```

**Nota:** Los stubs permiten que el proyecto KMP compile para iOS target
sin implementacion real. Esto es util para CI y para validar la estructura.

#### 2B.4 — Mover StateSynchronizer a commonMain

`StateSynchronizer` usa `NativeAudioBridge` para polling. Si el bridge es
expect/actual, el synchronizer puede vivir en commonMain.

Verificar que no tenga dependencias Android directas (logging, etc.).

### Verificacion

```bash
# V-2B.1: expect class exists in commonMain
grep -rn "expect class NativeAudioBridge" audio/src/commonMain/ && echo "PASS" || echo "FAIL"

# V-2B.2: actual class exists in androidMain
grep -rn "actual class NativeAudioBridge" audio/src/androidMain/ && echo "PASS" || echo "FAIL"

# V-2B.3: StateSynchronizer is in commonMain
find audio/src/commonMain/ -name "StateSynchronizer.kt" | head -1 | \
  xargs test -f && echo "PASS" || echo "FAIL"

# V-2B.4: Build compiles
./gradlew :audio:assembleDebug
```

### Post-condiciones
- `expect class NativeAudioBridge` en commonMain
- `actual class NativeAudioBridge` en androidMain (JNI)
- `StateSynchronizer` en commonMain
- Build green

---

## 4. Sub-fase 2C: Abstraer Dependencias Android

### Pre-condiciones
- 2B completada

### Contexto

Varios archivos que deberian estar en commonMain usan APIs de Android:
- `android.util.Log` — 7 archivos
- `NativeLibraryLoader` — `System.loadLibrary()`
- `DeviceCapabilities` — Android Build info
- DataStore — USB trusted devices

### Tareas

#### 2C.1 — expect/actual para logging

```kotlin
// commonMain:
expect object AudioLog {
    fun d(tag: String, msg: String)
    fun i(tag: String, msg: String)
    fun w(tag: String, msg: String)
    fun e(tag: String, msg: String)
    fun e(tag: String, msg: String, throwable: Throwable)
}

// androidMain:
actual object AudioLog {
    actual fun d(tag: String, msg: String) = Log.d(tag, msg)
    actual fun i(tag: String, msg: String) = Log.i(tag, msg)
    // ...
}

// iosMain:
actual object AudioLog {
    actual fun d(tag: String, msg: String) = NSLog("D/$tag: $msg")
    // ...
}
```

#### 2C.2 — expect/actual para native library loading

```kotlin
// commonMain:
expect object NativeLibraryLoader {
    fun loadAudioLibrary()
    fun isLoaded(): Boolean
}

// androidMain:
actual object NativeLibraryLoader {
    private var loaded = false
    actual fun loadAudioLibrary() {
        System.loadLibrary("noisypad-audio")
        loaded = true
    }
    actual fun isLoaded() = loaded
}

// iosMain:
actual object NativeLibraryLoader {
    actual fun loadAudioLibrary() {
        // iOS: framework linkado en compile time, no runtime loading
    }
    actual fun isLoaded() = true  // Always linked
}
```

#### 2C.3 — Mover DeviceCapabilities a androidMain

`DeviceCapabilities.kt` usa `android.os.Build` y no tiene sentido en
commonMain. Moverlo a androidMain y crear una interfaz minima si algo
en commonMain lo necesita.

#### 2C.4 — USB types: commonMain vs androidMain

- `UsbAudioTypes.kt`, `UsbAudioEvents.kt` — domain models, van a commonMain
- `UsbDeviceCompatibility.kt` — si no usa Android APIs, va a commonMain
- Todo lo que use `android.hardware.usb.*` queda en androidMain

#### 2C.5 — Actualizar todos los imports

Reemplazar `android.util.Log` → `AudioLog` en todos los archivos de commonMain.

### Verificacion

```bash
# V-2C.1: Zero Android imports in commonMain
grep -rn "import android\.\|import androidx\.\|import java\.io\." \
  audio/src/commonMain/kotlin/ && echo "FAIL" || echo "PASS"

# V-2C.2: expect/actual pairs exist
grep -rn "expect object AudioLog" audio/src/commonMain/ && \
grep -rn "actual object AudioLog" audio/src/androidMain/ && \
echo "PASS" || echo "FAIL"

# V-2C.3: Build compiles
./gradlew :audio:assembleDebug
```

### Post-condiciones
- commonMain tiene zero dependencias de Android
- Logging, native loading via expect/actual
- USB domain models en commonMain, Android USB APIs en androidMain

---

## 5. Sub-fase 2D: KMP AudioEngine Interface

### Pre-condiciones
- 2C completada

### Contexto

La interfaz publica `AudioEngine.kt` (en `api/`) debe vivir en commonMain
para que consumidores KMP puedan programar contra ella sin conocer la plataforma.

### Tareas

#### 2D.1 — Mover api/ interfaces a commonMain

```kotlin
// commonMain: api/AudioEngine.kt
interface AudioEngine {
    val state: StateFlow<AudioState>
    val isRunning: Boolean
    val isPaused: Boolean

    suspend fun start(fadeMs: Int? = null)
    suspend fun stop(fadeMs: Int? = null)
    suspend fun pause(fadeMs: Int = 300)
    suspend fun resume(fadeMs: Int = 300)

    fun setOscillator(type: OscillatorType)
    fun setXY(x: Float, y: Float)
    fun setFrequencyAndAmplitude(frequency: Float, amplitude: Float)
    fun setMasterVolume(volume: Float)
    // ... rest of public API
}
```

#### 2D.2 — Factory en commonMain con expect/actual

```kotlin
// commonMain:
expect class AudioEngineFactory {
    fun create(config: AudioEngineConfig = AudioEngineConfig()): AudioEngine
}

// androidMain:
actual class AudioEngineFactory {
    actual fun create(config: AudioEngineConfig): AudioEngine {
        NativeLibraryLoader.loadAudioLibrary()
        val bridge = NativeAudioBridge()
        return AudioEngineImpl(bridge, config)
    }
}
```

#### 2D.3 — Mover IEffectManager a commonMain

```kotlin
// commonMain: api/IEffectManager.kt
interface IEffectManager {
    val effectsState: StateFlow<List<EffectState>>
    suspend fun addEffect(type: EffectType): Result<EffectState>
    suspend fun removeEffect(index: Int): Result<Unit>
    suspend fun setParameter(effectIndex: Int, paramId: Int, value: Float): Result<Unit>
    // ...
}
```

#### 2D.4 — Verificar que AudioEngineImpl queda en androidMain

La implementacion (`AudioEngineImpl.kt`) que depende de JNI queda en androidMain.
Solo la interfaz y factory-expect estan en commonMain.

### Verificacion

```bash
# V-2D.1: AudioEngine interface in commonMain
find audio/src/commonMain/ -name "AudioEngine.kt" -path "*/api/*" | head -1 | \
  xargs test -f && echo "PASS" || echo "FAIL"

# V-2D.2: AudioEngineImpl in androidMain
find audio/src/androidMain/ -name "AudioEngineImpl.kt" | head -1 | \
  xargs test -f && echo "PASS" || echo "FAIL"

# V-2D.3: Factory expect/actual
grep -rn "expect class AudioEngineFactory" audio/src/commonMain/ && \
grep -rn "actual class AudioEngineFactory" audio/src/androidMain/ && \
echo "PASS" || echo "FAIL"

# V-2D.4: Build compiles
./gradlew :audio:assembleDebug
```

---

## 6. Sub-fase 2E: Validacion End-to-End

### Pre-condiciones
- 2A, 2B, 2C, 2D completadas

### Tareas

#### 2E.1 — Build verification

```bash
./gradlew clean assembleDebug          # Full clean build
./gradlew assembleRelease              # Release con R8
./gradlew :audio:assembleDebug         # Module alone
./gradlew lint                         # No new warnings
```

#### 2E.2 — Verificar estructura KMP

```bash
# Source set structure correct
test -d audio/src/commonMain/kotlin && \
test -d audio/src/androidMain/kotlin && \
echo "PASS" || echo "FAIL"

# commonMain has no Android deps
grep -rn "import android\.\|import androidx\." audio/src/commonMain/ && echo "FAIL" || echo "PASS"

# All expect classes have actual in androidMain
# (compare expect declarations vs actual)
```

#### 2E.3 — API surface validation

Verificar que la API publica expuesta en commonMain es suficiente para:
- Crear un engine
- Start/stop con fade
- Agregar/remover effects
- Cambiar engine type
- Cargar SoundFont
- Control de XY, volume, BPM
- Lectura de state y waveform

#### 2E.4 — Audio functionality test (manual)

Misma matrix que Phase 0E.2 — la app debe funcionar identico.

#### 2E.5 — Performance baseline

Comparar latencia y CPU usage con baseline de Phase 0.
KMP no deberia agregar overhead (es compile-time, no runtime).

### Verificacion

Todos los tests 2E.1-2E.5 deben pasar.

### Post-condiciones (Phase 2 completa)

- [x] `commonMain/` contiene: domain models, API interfaces, expect classes
- [x] `androidMain/` contiene: JNI bridge actual, native loader, engine impl
- [x] Zero Android imports en commonMain
- [x] expect/actual para: NativeLibraryLoader. Interface-based: IAudioNativeBridge + getAudioBridge()
- [x] StateSynchronizer en commonMain (reusable)
- [x] NoisyPad funciona identico
- [x] Preparado para agregar iosMain en Phase 4

---

## 7. Audit: Propuesto vs Implementado

*Completada 2026-04-08 en una sesion.*

### Desviaciones del spec original

| Aspecto | Spec propuesto | Implementado | Razon |
|---------|---------------|-------------|-------|
| **Bridge abstraction** | `expect class NativeAudioBridge` (222 methods) | `IAudioNativeBridge` interface (~70 methods) + `expect fun getAudioBridge()` | Interface es mas flexible que expect/actual class. 70 methods cubren los consumers que movimos a commonMain; USB/looper/arp/soundfont methods no necesitan abstraccion (consumers quedan en androidMain) |
| **Logging** | `expect object AudioLog` | `AudioLogger` interface (ya existia) + inject via constructor param | Callback pattern mas testeable que singleton expect/actual |
| **AudioEngineImpl** | Spec decia queda en androidMain | Movido a commonMain | Gracias a IAudioNativeBridge, no necesita deps Android |
| **EffectManagerImpl** | No mencionado en spec 2B | Movido a commonMain en 2B | Dependia solo de Log + bridge, ambos abstraidos |
| **Factories** | `expect class AudioEngineFactory` | Object factories movidos a commonMain (usan `getAudioBridge()`) | Mas simple que expect/actual — factories no tienen platform-specific logic |
| **File counts** | 51 commonMain / 14 androidMain | 52 commonMain / 18 androidMain | Mas files en commonMain (movimos impl+factories). Mas en androidMain (+4: SyncApi, UsbTestRunnerFactory, UsbCompatibilityTypes split, latency) |
| **ModeTransition** | No abordado | Queda en androidMain | ModeTransitionManagerImpl y NativeModeStateWriter usan Log, no se migraron |
| **iOS stubs** | 2B.3: crear stubs en iosMain | No creados | No hay iosMain target configurado (Phase 4). Sin valor agregar stubs ahora |
| **AGP 9 compat** | KMP plugin directo | `android.builtInKotlin=false` + explicit `kotlin-android` en non-KMP modules | AGP 9 no soporta `com.android.library` + `kotlin-multiplatform`. `com.android.kotlin.multiplatform.library` no soporta CMake/NDK. Workaround funcional |

### Resultado final

```
audio/src/
  commonMain/kotlin/  (52 files)
    api/              AudioEngine, IAudioNativeBridge, IEffectManager, IEffectStateProvider,
                      IEffectStateWriter, IModeStateWriter, IModeTransitionHandler,
                      IUsbAudioManager, factories (3), callbacks (2), config (1)
    callback/         AudioLogger, AudioAnalyticsListener
    domain/           effect (7), error (2), mode (4), modulator (1), oscillator (1),
                      scale (1), state (3), usb (5)
    internal/
      bridge/         AudioBridgeProvider (expect)
      effect/         EffectManagerImpl
      engine/         AudioEngineImpl
      native/         NativeLibraryLoader (expect)
      optimization/   Phase4Types
      sync/           StateSynchronizer, StateDivergence, SyncConfig, SyncEvent, SyncedAudioState
      util/           ChordGenerator, ScaleQuantizer

  androidMain/kotlin/  (18 files)
    api/              ModeTransitionFactory, SyncApi, UsbAudioManagerFactory,
                      UsbAudioTestRunnerFactory, latency/ (2)
    internal/
      bridge/         AudioNativeBridge (2,619 LOC, implements IAudioNativeBridge),
                      AudioBridgeProvider (actual)
      mode/           ModeTransitionManagerImpl, NativeModeStateWriter
      native/         NativeLibraryLoader (actual)
      optimization/   JniMetrics
      usb/            UsbAudioManagerImpl, TrustedUsbDevicesRepository,
                      UsbAudioTestRunner, UsbVolumeRepository, UsbDeviceCompatibility
      util/           DeviceCapabilities

  main/
    AndroidManifest.xml
    cpp/              (C++ unchanged — 135+ headers, 60+ sources)
```

### Build system changes

- `audio/build.gradle.kts`: `noisypad.android.native` → `noisypad.kmp.native`
- `KmpNativeConventionPlugin.kt`: aplica `kotlin-multiplatform` + `com.android.library`
- `AndroidLibraryConventionPlugin.kt`: agrega explicit `kotlin-android` + JVM target 11
- `AndroidApplicationConventionPlugin.kt`: agrega explicit `kotlin-android` + JVM target 11
- `gradle.properties`: `android.builtInKotlin=false`, `android.newDsl=false`
- `gradle/libs.versions.toml`: `kotlin-multiplatform` plugin + `kotlinx-coroutines-core` library

### Proximos pasos: Phase 3

Phase 2 esta completa. El modulo audio es KMP-ready con 52 archivos pure Kotlin
en commonMain. Los 18 archivos androidMain tienen deps genuinas de Android
(JNI, USB hardware, DataStore, Context, Build) que no pueden ni deben moverse.

Phase 3 (Repositorio y Distribucion) puede empezar directamente:
- 3A: Crear repo `watermelon-audio` en GitHub
- 3B: Build system standalone (extraer CMake + Kotlin)
- 3C: GitHub Packages publishing
- 3D: CI/CD pipeline
- 3E: Migrar NoisyPad a consumir el package
- 3F: Documentacion publica
