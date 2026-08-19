# Watermelon Audio — C++20 Engine

Real-time audio synthesis and effects engine. Lock-free, zero-allocation audio path.

---

## Architecture

```
Kotlin (AudioNativeBridge.kt)
  |  JNI
C++ JNI layer (jni/jni_audio_bridge.cpp)
  |
WmaEngine (api/watermelon_audio_internal.h)
  |-- owns BackendManager (OboeBackend, LibusbBackend)
  |-- owns AudioEngine (core/AudioEngine.h — facade)
  '-- owns InputNode
          |
AudioEngine : IAudioCallback
  |-- processAudioBlock() — main RT-safe DSP path (~120 LOC)
  |-- 7 subsystems: WaveformCapture, OutputStage, FadeController,
  |     DualTouchManager, ChordHarmony, OscillatorBank, SynthEngineDispatcher
  |-- EffectChain (6 routing modes, 20 effects, dynamic registry)
  |-- VoiceManager / VoicePool (16 voices, ADSR, stealing)
  |-- AudioLooper (8 tracks, loop regions, peak metering)
  '-- ArpSequencer (10 patterns, swing, gate, latch)
```

## Directory Structure

```
cpp/
  api/            C API — watermelon_audio.h (181 functions, pure C, platform-agnostic)
                  watermelon_audio.cpp, watermelon_audio_internal.h (WmaEngine struct)
  platform/       Logger.h (callback-based), Platform.h (denormals, SIMD detection)

  dsp/            watermelon-dsp sub-library (30 files, zero deps, host-compilable)
                  BiquadFilter, FDN, LFO, ADSR, DelayLine, FFT, StereoTools...
  effects/        watermelon-effects sub-library (53 files)
                  EffectChain + EffectRegistry + 20 effects
  engines/        watermelon-engines sub-library (9 files)
                  7 synth engines: Classic, FM, KarplusStrong, Supersaw,
                  Wavetable, Granular, SoundFont (TinySoundFont)
  voice/          watermelon-voice sub-library (10 files)
                  VoiceManager, VoicePool, polyphony, ADSR
  looper/         watermelon-looper sub-library (3 files, header-only)
                  AudioLooper, TrackBuffer, WavFile

  core/           AudioEngine facade + 7 subsystem classes
                  WaveformCapture, OutputStage, FadeController, DualTouchManager,
                  ChordHarmony, OscillatorBank, SynthEngineDispatcher
  nodes/          OscillatorNode, InputNode, MixerNode, OutputNode
  sequencer/      ArpSequencer, PatternGenerator
  backends/       IAudioBackend, OboeBackend, LibusbBackend, BackendManager
  jni/            jni_audio_bridge.cpp (unified JNI), jni_common.h
  usb/            USB audio driver (descriptors, transfers, volume, format)
  oscillators/    Classic oscillators (legacy, used by OscillatorBank)
  modulators/     AM, FM, Ring, PWM, Burst, Envelope, Gate
  analysis/       watermelon-analysis (REQ-001) — la FFT muerta que habia aca se borro
  utils/          MemoryUtils, ThreadUtils, LatencyBenchmark
  thirdparty/     TinySoundFont (tsf.h)
```

## Sub-Libraries (CMake)

| Library | Files | Deps | Description |
|---------|-------|------|-------------|
| **watermelon-dsp** | 30 | none | DSP primitives (host-compilable) |
| **watermelon-effects** | 53 | dsp | 20 effects + EffectChain + EffectRegistry |
| **watermelon-engines** | 9 | dsp, tsf | 7 synth engines + SoundFont |
| **watermelon-voice** | 10 | engines, dsp | VoiceManager, VoicePool, polyphony |
| **watermelon-looper** | 3 | dsp | AudioLooper, TrackBuffer, WavFile (header-only) |

## Effects (20)

Filter, Delay, Reverb, Chorus, Flanger, Phaser, Distortion, Compressor,
Parametric EQ, Lookahead Limiter, Tremolo, AutoPan, Bitcrusher, Ring Modulator,
Vocoder, Cabinet Sim, Noise Gate, Pitch Shifter, Stereo Widener, Tape Saturation.

Registered dynamically via `EffectRegistry` (no switch statements).

## Engines (7)

| Engine | Description |
|--------|-------------|
| Classic | Oscillator bank (sine/saw/square/tri/noise/pulse) |
| FM | 2-operator FM synthesis |
| KarplusStrong | Physical modeling (plucked strings) |
| Supersaw | Detuned unison saw (up to 7 voices) |
| Wavetable | Wavetable with morphing |
| Granular | Granular synthesis |
| SoundFont | SF2 via TinySoundFont (mmap, lock-free SPSC) |

## C API

`watermelon_audio.h` — 181 functions, 21 categories, pure C header.

```c
WmaEngine* wma_engine_create(void);
void wma_engine_destroy(WmaEngine* engine);
int wma_engine_start(WmaEngine* engine);
void wma_set_xy(WmaEngine* engine, float x, float y);
int wma_effect_add(WmaEngine* engine, int type_id);
// ... 176 more
```

## Thread Safety

- Audio callback: **100% lock-free** — no mutex, no new/malloc, no STL containers
- Parameters: `std::atomic<float>` for all UI-to-audio communication
- State changes: `incrementStateVersion()` after any mutation
- Smoothing: one-pole parameter smoothing to avoid zipper noise
- Logging: `platform/Logger.h` — NOT RT-safe, use sparingly outside hot path

## Build

```bash
# Via Gradle (recommended):
./gradlew :audio:assembleDebug

# CMake flags:
-DCMAKE_CXX_STANDARD=20
-DANDROID_STL=c++_shared
-DANDROID_PLATFORM=android-29
```

### Optimizations (Release)

- `-O3`, safe FP flags (no `-ffinite-math-only` — preserves NaN checks)
- LTO (`CMAKE_INTERPROCEDURAL_OPTIMIZATION ON`)
- ARM NEON auto-vectorization (`-mcpu=cortex-a53`)
- 16KB page alignment (Android 15+)
- Dead code elimination (`-ffunction-sections -fdata-sections -Wl,--gc-sections`)

## Tests

36 unit tests via Google Test + MinGW host toolchain:

```bash
cd tests && mkdir build && cd build
cmake .. -G "MinGW Makefiles" && cmake --build . && ./watermelon_audio_tests
```

---

*Extracted from NoisyPad — Watermelon Studios, 2026*
