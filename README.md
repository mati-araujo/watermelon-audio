# Watermelon Audio

Real-time audio synthesis and effects engine for Android. C++20 + Oboe + Kotlin Multiplatform.

**Watermelon Studios** — *Make sound. See sound. Feel sound.*

---

## Features

- **Low Latency** — <10ms via Oboe (AAudio/OpenSL ES)
- **7 Synth Engines** — Classic, FM, Karplus-Strong, Supersaw, Wavetable, Granular, SoundFont
- **20 Professional Effects** — Filter, Delay, Reverb, Chorus, Flanger, Phaser, Distortion, Compressor, EQ, Vocoder, and more
- **16-Voice Polyphony** — VoicePool with stealing strategies, ADSR envelopes
- **8-Track Looper** — Independent playheads, loop regions, overdub, import/export WAV
- **Mixer** — Per-track volume/pan/mute/solo, master volume, peak metering
- **Arpeggiator** — 10 patterns, swing, gate, latch, scale-aware
- **SoundFont Support** — SF2 loading via TinySoundFont, mmap, lock-free SPSC
- **Musical Scales** — 12 scale modes including quarter-tone (24-TET)
- **USB Audio** — Libusb backend for low-latency USB audio interfaces
- **Kotlin Multiplatform** — commonMain (pure Kotlin) + androidMain (JNI)

## Architecture

```
┌─────────────────────────────────────────────┐
│  Kotlin API (commonMain)                    │
│  AudioEngine, IEffectManager, factories     │
│  Domain models, StateFlow, coroutines       │
├─────────────────────────────────────────────┤
│  Bridge (androidMain)                       │
│  AudioNativeBridge (222 JNI functions)      │
│  IAudioNativeBridge interface (~70 methods) │
├─────────────────────────────────────────────┤
│  C API — watermelon_audio.h                 │
│  181 functions, pure C, platform-agnostic   │
├─────────────────────────────────────────────┤
│  C++20 Engine                               │
│  ┌─────────┬──────────┬─────────┐           │
│  │  DSP    │ Effects  │ Engines │           │
│  │ (30 f.) │ (53 f.)  │ (9 f.)  │           │
│  ├─────────┼──────────┼─────────┤           │
│  │  Voice  │ Looper   │ Seq.    │           │
│  │ (10 f.) │ (3 f.)   │ (arp)   │           │
│  └─────────┴──────────┴─────────┘           │
│  AudioEngine facade + 7 subsystems          │
├─────────────────────────────────────────────┤
│  Backends: Oboe | Libusb                    │
└─────────────────────────────────────────────┘
```

### Source Layout

```
audio/src/
  commonMain/kotlin/    52 files — pure Kotlin, zero Android deps
    api/                AudioEngine interface, factories, IAudioNativeBridge
    domain/             Effect types, oscillators, scales, modes, USB types
    callback/           AudioLogger, AudioAnalyticsListener
    internal/           AudioEngineImpl, EffectManagerImpl, StateSynchronizer
  androidMain/kotlin/   18 files — JNI bridge, USB, platform-specific
    internal/bridge/    AudioNativeBridge (2,619 LOC, 222 JNI functions)
    internal/usb/       USB audio driver (DataStore, BroadcastReceiver)
  main/cpp/             C++20 engine
    api/                C API (watermelon_audio.h — 181 functions)
    dsp/                Biquad, FDN, LFO, ADSR, delay lines, FFT
    effects/            20 effects + EffectChain + EffectRegistry
    engines/            7 synth engines + SoundFont
    voice/              VoiceManager, VoicePool, polyphony
    looper/             AudioLooper, TrackBuffer, WavFile
    core/               AudioEngine facade + 7 subsystems
    backends/           IAudioBackend, OboeBackend, LibusbBackend
```

## Installation

### From GitHub Packages

```kotlin
// settings.gradle.kts
dependencyResolutionManagement {
    repositories {
        maven {
            url = uri("https://maven.pkg.github.com/mati-araujo/watermelon-audio")
            credentials {
                username = project.findProperty("gpr.user")?.toString()
                password = project.findProperty("gpr.key")?.toString()
            }
        }
    }
}

// build.gradle.kts
dependencies {
    implementation("com.watermellonstudios:audio-android:1.3.1")
}
```

### From Local Maven

```bash
# In this repo:
./gradlew :audio:publishToMavenLocal

# In consuming project — add mavenLocal() to repositories:
dependencies {
    implementation("com.watermellonstudios:audio-android:1.3.1")
}
```

## Quick Start

```kotlin
// Create engine with defaults
val engine = AudioEngineFactory.create()

// Or with custom config
val engine = AudioEngineFactory.create(
    AudioEngineConfig.builder()
        .logger(myLogger)
        .analyticsListener(myAnalytics)
        .build()
)

// Start audio
engine.start()

// Control synthesis
engine.setOscillator(OscillatorType.SAW)
engine.setXY(0.5f, 0.7f)  // frequency, amplitude
engine.setMasterVolume(0.8f)

// Add effects
engine.addEffect(EffectType.REVERB)
engine.addEffect(EffectType.DELAY)
engine.setEffectParameter(0, ReverbParams.DECAY, 0.7f)

// Observe state
engine.state.collect { state ->
    updateUI(state)
}

// Cleanup
engine.release()
```

## Building

```bash
./gradlew :audio:assembleDebug      # Debug build (4 ABIs)
./gradlew :audio:assembleRelease    # Release build (optimized)
./gradlew :audio:publishToMavenLocal # Publish to local Maven
```

### Requirements

| Requirement | Version                             |
|------------|-------------------------------------|
| Android Min SDK | 29 (Android 10)                     |
| Compile SDK | 36                                  |
| Kotlin | 2.3.20                              |
| AGP | 9.2.1                               |
| CMake | 3.22.1                              |
| C++ | C++20                               |
| NDK ABIs | arm64-v8a, armeabi-v7a, x86_64, x86 |

## Thread Safety

- **UI thread safe** — all Kotlin API methods can be called from any thread
- **Audio thread** — 100% lock-free, zero allocations, atomic parameters
- **Mutex categories** — lifecycle, effects, mode, input (separate locks, no contention)
- **Real-time params** — `setXY()`, `setFrequencyAndAmplitude()` are lock-free (safe during playback)

## Dependencies

| Library | Version | License |
|---------|---------|---------|
| Oboe | 1.10.0 | Apache 2.0 |
| kotlinx-coroutines | 1.10.2 | Apache 2.0 |
| TinySoundFont | 0.9 | MIT |
| AndroidX Core KTX | 1.18.0 | Apache 2.0 |
| AndroidX DataStore | 1.2.1 | Apache 2.0 |
| AndroidX Lifecycle | 2.10.0 | Apache 2.0 |

## License

Copyright 2026 Watermelon Studios. All rights reserved.
