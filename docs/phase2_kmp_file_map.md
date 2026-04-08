# Phase 2 — KMP File Migration Map

**Clasificacion de los 65 archivos Kotlin del modulo audio.**
*Generado 2026-04-07 contra el codebase real.*

## Resumen

| Source Set | Files | Description |
|-----------|-------|-------------|
| **commonMain** | 51 | Pure Kotlin, zero Android deps |
| **androidMain** | 14 | Uses android.*, androidx.*, java.*, System.loadLibrary |

---

## commonMain (51 files)

### domain/ (23 files) — Pure models

| File | Status |
|------|--------|
| `domain/effect/EffectConstants.kt` | READY |
| `domain/effect/EffectParameter.kt` | READY |
| `domain/effect/EffectParameterIds.kt` | READY |
| `domain/effect/EffectState.kt` | READY |
| `domain/effect/EffectType.kt` | READY |
| `domain/effect/PedalPresets.kt` | READY |
| `domain/error/NativeBridgeException.kt` | READY |
| `domain/error/NativeErrorCode.kt` | READY |
| `domain/mode/AudioMode.kt` | READY |
| `domain/mode/ModeTransitionExceptions.kt` | READY |
| `domain/mode/ModeTransitionState.kt` | READY |
| `domain/mode/TransitionPhase.kt` | READY |
| `domain/modulator/ModulatorType.kt` | READY |
| `domain/oscillator/OscillatorType.kt` | READY |
| `domain/scale/ScaleMode.kt` | READY |
| `domain/state/AudioState.kt` | READY |
| `domain/state/EngineLifecycle.kt` | READY |
| `domain/state/StreamInfo.kt` | READY |
| `domain/usb/UsbAudioEvents.kt` | READY |
| `domain/usb/UsbAudioTypes.kt` | READY |
| `domain/usb/UsbDeviceCompatibility.kt` | NEEDS WORK — uses android.os.Build |
| `domain/usb/UsbTestResult.kt` | READY |
| `domain/usb/UsbVolumeTypes.kt` | READY |

### callback/ (2 files) — Dependency inversion interfaces

| File | Status |
|------|--------|
| `callback/AudioAnalyticsListener.kt` | READY |
| `callback/AudioLogger.kt` | READY |

### api/ (15 files) — Public interfaces

| File | Status |
|------|--------|
| `api/AudioEngine.kt` | READY |
| `api/AudioEngineFactory.kt` | READY |
| `api/EffectManagerFactory.kt` | READY |
| `api/IEffectManager.kt` | READY |
| `api/IEffectStateProvider.kt` | READY |
| `api/IEffectStateWriter.kt` | READY |
| `api/IModeStateWriter.kt` | READY |
| `api/IModeTransitionHandler.kt` | READY |
| `api/IUsbAudioManager.kt` | READY |
| `api/ModeTransitionFactory.kt` | READY |
| `api/StateSynchronizerFactory.kt` | READY |
| `api/SyncApi.kt` | READY |
| `api/callbacks/IAudioAnalytics.kt` | READY |
| `api/callbacks/ICrashReporter.kt` | READY |
| `api/config/AudioEngineConfig.kt` | READY |

### internal/ — Pure utilities (11 files)

| File | Status | Notes |
|------|--------|-------|
| `internal/engine/AudioEngineImpl.kt` | READY | No android imports, uses bridge |
| `internal/effect/EffectManagerImpl.kt` | NEEDS WORK | Uses android.util.Log (1 import) |
| `internal/sync/StateDivergence.kt` | READY | |
| `internal/sync/SyncConfig.kt` | READY | |
| `internal/sync/SyncEvent.kt` | READY | |
| `internal/sync/SyncedAudioState.kt` | READY | |
| `internal/optimization/Phase4Types.kt` | READY | |
| `internal/util/ChordGenerator.kt` | READY | Pure math |
| `internal/util/ScaleQuantizer.kt` | READY | Pure math |

**Note:** `EffectManagerImpl.kt` and `StateSynchronizer.kt` need `android.util.Log` replaced with `AudioLogger` callback.

---

## androidMain (14 files)

| File | Android Deps | Notes |
|------|-------------|-------|
| `internal/bridge/AudioNativeBridge.kt` | android.util.Log, java.util.concurrent | **2,619 LOC** — JNI external funs, becomes `actual` |
| `internal/native/NativeLibraryLoader.kt` | System.loadLibrary | becomes `actual` |
| `internal/mode/ModeTransitionManagerImpl.kt` | android.util.Log | Log only — could move to common if abstracted |
| `internal/mode/NativeModeStateWriter.kt` | android.util.Log | Log only |
| `internal/sync/StateSynchronizer.kt` | android.util.Log | Log only — candidate for common if abstracted |
| `internal/usb/UsbAudioManagerImpl.kt` | android.hardware.usb.*, Context, BroadcastReceiver | Heavy Android — stays |
| `internal/usb/TrustedUsbDevicesRepository.kt` | androidx.datastore | DataStore — stays |
| `internal/usb/UsbAudioTestRunner.kt` | android.util.Log | Log only |
| `internal/usb/UsbVolumeRepository.kt` | androidx.datastore, java.io | DataStore — stays |
| `internal/util/DeviceCapabilities.kt` | android.os.Build | Android Build info — stays |
| `internal/optimization/JniMetrics.kt` | java.util.concurrent | AtomicLong — could use kotlinx.atomicfu |
| `api/UsbAudioManagerFactory.kt` | android.content.Context | Context param — stays |
| `api/latency/LatencyAnalyzer.kt` | VERIFY | May be pure |
| `api/latency/LatencyBenchmarkRunner.kt` | android.util.Log | Log only |

### Android deps breakdown

| Dep | Files affected | Solution |
|-----|---------------|----------|
| `android.util.Log` | 7 files | Replace with `AudioLogger` callback (already exists) |
| `androidx.datastore` | 2 files | Stay in androidMain |
| `android.hardware.usb.*` | 1 file | Stay in androidMain |
| `android.content.Context` | 2 files | Stay in androidMain |
| `android.os.Build` | 2 files | Stay in androidMain or expect/actual |
| `java.util.concurrent` | 2 files | Replace with kotlinx.atomicfu or stay |
| `System.loadLibrary` | 1 file | expect/actual |

---

## Migration Execution Order

### Step 1: Pre-migration (THIS SESSION)
- [x] File classification complete (this document)
- [ ] Version catalog updated with KMP entries
- [ ] Convention plugin skeleton created
- [ ] Migration checklist documented

### Step 2: Build system switch (NEXT SESSION)
1. Change `audio/build.gradle.kts` from `noisypad.android.native` to `noisypad.kmp.native`
2. Create `src/commonMain/kotlin/` and `src/androidMain/kotlin/` directories
3. Move 51 pure files to `commonMain/`
4. Move 14 android files to `androidMain/`
5. Fix any compilation issues
6. Verify build

### Step 3: Abstractions
1. Replace `android.util.Log` in 7 files with `AudioLogger`
2. Create expect/actual for `NativeLibraryLoader`
3. Move additional files from androidMain to commonMain as abstractions are ready

### Step 4: Verify
1. `./gradlew assembleDebug` — build green
2. `./gradlew :audio:test` — KMP common tests pass
3. Full app functions identically
