# NoisyPad - Native C++ Audio Engine

## Overview

Professional-grade native audio engine for Android, implementing a real-time synthesizer with effects processing. Built with C++20 and Google Oboe for ultra-low latency audio (<10ms). Features SIMD optimization (ARM NEON), modular node-based architecture, and thread-safe lock-free design.

**Version:** 2.0 (Post-Optimization)
**Last Updated:** 2025-12-17

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         JNI BRIDGE                                  │
│                      (native-lib.cpp)                               │
└─────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│                        AUDIO ENGINE                                 │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐                │
│  │ Oscillators │  │ Modulators  │  │   Mixer     │                │
│  │  (6 types)  │  │  (8 types)  │  │   Node      │                │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘                │
│         │                │                │                        │
│         └────────────────┴────────────────┘                        │
│                          │                                          │
│                          ▼                                          │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    EFFECT CHAIN                              │   │
│  │  Filter → Delay → Reverb → ParametricEQ → LookaheadLimiter  │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                          │                                          │
│                          ▼                                          │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    OUTPUT NODE                               │   │
│  │  DC Block → Soft Clip → Dither → Master Volume → Limiter    │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    OBOE (Google)                                    │
│              AAudio / OpenSL ES → Hardware                          │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Directory Structure

```
cpp/
├── CMakeLists.txt              # Build configuration with pro optimizations
├── native-lib.cpp              # JNI bridge (Kotlin ↔ C++)
├── README.md                   # This file
│
├── core/                       # Core audio engine
│   ├── AudioEngine.h/cpp       # Main engine (Oboe callbacks, state machine)
│   ├── AudioSource.h           # Abstract interface for audio sources
│   ├── constants.h             # Global constants (sample rate, buffer sizes)
│   ├── AudioMode.h             # Audio mode definitions
│   ├── ModeConfigurations.h    # Mode-specific configurations
│   ├── ModeManager.h/cpp       # Mode state management
│   ├── XYMapper.h/cpp          # XY controller → parameter mapping
│   ├── MusicalScale.h/cpp      # [Phase 5] Musical scale quantization
│   │
│   └── graph/                  # Node-based audio graph
│       ├── AudioBuffer.h       # Non-interleaved audio buffer
│       ├── AudioNode.h         # Abstract node interface
│       └── AudioGraph.h/cpp    # Graph processing engine
│
├── effects/                    # Audio effects
│   ├── Effect.h/cpp            # Abstract effect base class
│   ├── EffectChain.h/cpp       # Dynamic effect chain container
│   ├── FilterEffect.h/cpp      # IIR filter (LPF/HPF/BPF), Q: 0.5-30
│   ├── DelayEffect.h/cpp       # Delay with cubic interpolation
│   ├── ReverbEffect.h/cpp      # Algorithmic reverb
│   ├── LookaheadLimiter.h/cpp  # [Phase 4] Pro limiter (5ms lookahead)
│   └── ParametricEQ.h/cpp      # [Phase 5] 3-band parametric EQ
│
├── oscillators/                # Waveform generators
│   └── Oscillators.h           # Sine, Square, Saw, Triangle, Noise, Pulse
│
├── modulators/                 # Signal modulators
│   ├── SignalModulator.h       # Abstract modulator interface
│   ├── AMModulator.h           # Amplitude modulation
│   ├── FMModulator.h           # Frequency modulation
│   ├── RingModulator.h         # Ring modulation
│   ├── PWMModulator.h          # Pulse width modulation
│   ├── BurstModulator.h        # Burst/stutter effect
│   ├── EnvelopeModulator.h     # ADSR envelope
│   └── GateModulator.h         # Gate/trigger
│
├── nodes/                      # Audio graph nodes
│   ├── OscillatorNode.h/cpp    # Oscillator + modulator wrapper
│   ├── EffectChainNode.h/cpp   # Effect chain wrapper with wet/dry
│   ├── OutputNode.h/cpp        # Output stage (DC block, clip, dither)
│   ├── InputNode.h/cpp         # Audio input (microphone monitoring)
│   └── MixerNode.h/cpp         # Multi-input mixer with crossfade
│
├── utils/                      # Utilities
│   ├── ParameterSmoother.h     # One-pole parameter smoothing
│   ├── DCBlocker.h             # DC offset removal (stereo)
│   ├── SoftClipper.h           # Soft saturation (tanh)
│   ├── Dithering.h             # TPDF dithering for bit depth
│   ├── NoiseGate.h             # Noise gate processor
│   ├── LevelMeter.h            # Peak/RMS level metering
│   ├── LockFreeRingBuffer.h    # Lock-free SPSC ring buffer
│   ├── SIMDUtils.h             # [Phase 2] ARM NEON optimizations
│   ├── EffectTest.cpp          # Unit tests
│   │
│   └── dsp/                    # DSP building blocks
│       ├── DSPMath.h           # Math utilities (dB, interpolation, etc.)
│       ├── BiquadFilter.h/cpp  # Biquad IIR filter (all types)
│       ├── DelayLine.h/cpp     # Delay line with interpolation
│       ├── LFO.h/cpp           # Low-frequency oscillator
│       ├── StereoTools.h/cpp   # Stereo width, pan, M/S
│       └── EarlyReflections.h/cpp  # Reverb early reflections
│
└── analysis/                   # Audio analysis
    └── SpectrumAnalyzer.h/cpp  # [Phase 5] FFT spectrum analyzer
```

---

## Components

### Core Engine

| Component | Description |
|-----------|-------------|
| **AudioEngine** | Main engine class. Manages Oboe stream, state machine (Stopped→Starting→Running→Stopping), dual-touch support, and coordinates all audio components. |
| **AudioSource** | Abstract interface for sound sources (oscillators, samplers). |
| **ModeManager** | Manages audio modes (ChaosPad, InputFX, Mix). |
| **XYMapper** | Maps XY controller coordinates to audio parameters with configurable curves. |
| **MusicalScale** | Quantizes frequencies to musical scales (10 scales, configurable root/octaves). |
| **AudioGraph** | Node-based processing graph for modular routing. |

### Effects (7 Total)

| Effect | Parameters | Description |
|--------|------------|-------------|
| **FilterEffect** | Cutoff (20-20kHz), Q (0.5-30), Type | IIR biquad filter. LPF/HPF/BPF. Self-oscillation support at high Q. |
| **DelayEffect** | Time (1-2000ms), Feedback (0-0.9), Wet | Stereo delay with cubic interpolation. BPM sync option. |
| **ReverbEffect** | Size, Damping, Pre-delay, Wet | Algorithmic reverb with early reflections. |
| **ParametricEQ** | Low/Mid/High freq, Gain (±15dB), Mid Q | 3-band parametric EQ (Low Shelf, Peaking Mid, High Shelf). |
| **LookaheadLimiter** | Threshold (-12 to 0dB), Attack, Release | Professional brickwall limiter with 5ms lookahead. |

### Oscillators (6 Types)

| Type | Description |
|------|-------------|
| Sine | Pure sine wave, no harmonics |
| Square | Square wave with odd harmonics |
| Sawtooth | Rich harmonics, classic synth sound |
| Triangle | Soft, flute-like tone |
| Noise | White noise generator |
| Pulse | Variable-width pulse wave |

### Modulators (8 Types)

| Modulator | Description |
|-----------|-------------|
| None | No modulation (bypass) |
| Burst | Rhythmic burst/stutter effect |
| AM | Amplitude modulation (tremolo) |
| FM | Frequency modulation (vibrato/FM synthesis) |
| PWM | Pulse width modulation |
| Envelope | ADSR envelope shaping |
| Ring | Ring modulation (metallic tones) |
| Gate | Gate/trigger-based modulation |

### Analysis

| Component | Description |
|-----------|-------------|
| **SpectrumAnalyzer** | Real-time FFT analysis. 256-2048 bins, Hann/Hamming/Blackman windows, smoothed output with peak hold. |

---

## Optimization Summary

### Phase 1: CMake Professional Optimization ✅

```cmake
# Safe floating-point optimizations (replaces unsafe -ffast-math)
-fno-math-errno -fno-trapping-math -ffinite-math-only
-fno-signed-zeros -freciprocal-math -fassociative-math

# Auto-vectorization and LTO
-ftree-vectorize
CMAKE_INTERPROCEDURAL_OPTIMIZATION ON

# ARM64 NEON baseline
-mcpu=cortex-a53
-DUSE_NEON=1
```

### Phase 2: SIMD NEON Integration ✅

`SIMDUtils.h` provides vectorized operations:
- `applyStereoGainRamp()` - Fade in/out
- `applyStereoGain()` - Volume control
- `mixStereoBuffers()` - Buffer mixing
- `addStereoBuffers()` - Buffer addition
- `softClipStereo()` - Saturation
- `hardLimitStereo()` - Brickwall limiting
- `clearBuffer()` - Buffer zeroing
- `findPeak()` / `calculateRMS()` - Metering

### Phase 3: Audio Path Efficiency ✅

- **Dual Touch Optimization**: Single-touch mode skips redundant oscillator render (50% CPU savings)
- **Modulator Early-Exit**: `mHasActiveModulator` flag avoids unnecessary atomic loads
- **SIMD Mix Functions**: Vectorized `mixDualTouchSignals()`

### Phase 4: DSP Improvements ✅

- **Delay**: Upgraded from linear to cubic interpolation (Hermite spline)
- **Filter**: Expanded Q range (0.5-30.0) with gain compensation for self-oscillation
- **LookaheadLimiter**: New professional limiter (5ms lookahead, 1ms attack, 100ms release)

### Phase 5: Pro Features ✅

- **ParametricEQ**: 3-band EQ (Low Shelf 20-500Hz, Mid Peak 100-10kHz, High Shelf 2k-20kHz)
- **MusicalScale**: 10 scales, configurable root note, X→frequency quantization
- **SpectrumAnalyzer**: Cooley-Tukey FFT, 256-2048 bins, multiple window functions

---

## Thread Safety Model

### Lock-Free Audio Path

The audio callback (`onAudioReady`) is **100% lock-free**:
- No mutex locks
- No memory allocation
- No system calls
- Only atomic reads/writes

### Parameter Updates

```cpp
// UI Thread (safe to call anytime)
engine.updateXY(x, y);           // Atomic store
engine.setMasterVolume(0.8f);    // Atomic store
engine.setOscillatorType(2);     // Atomic store

// Audio Thread (reads atomically)
float freq = mFrequency.load(std::memory_order_acquire);
```

### State Machine

```
Stopped ←→ Starting → Running → Stopping → Stopped
   ↑                                         │
   └─────────────────────────────────────────┘
```

State transitions protected by `mStateMutex`. Audio callback checks state with atomic load.

---

## Build Configuration

### Requirements

- **NDK**: Latest (C++20 support)
- **CMake**: 3.22.1+
- **Oboe**: 1.10.0 (via Prefab)

### Build Types

| Build | Flags | Use Case |
|-------|-------|----------|
| Debug | `-O0 -g` | Development, debugging |
| Release | `-O3 -DNDEBUG -flto` | Production |

### ABI Support

| ABI | SIMD | Notes |
|-----|------|-------|
| arm64-v8a | NEON | Primary target, cortex-a53 baseline |
| armeabi-v7a | NEON | Legacy 32-bit support |
| x86_64 | SSE4.2 | Emulator support |

---

## Adding New Components

### New Effect

1. Create header in `effects/`:
```cpp
// effects/MyEffect.h
class MyEffect : public Effect {
public:
    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;
};
```

2. Implement in `effects/MyEffect.cpp`

3. Add to `CMakeLists.txt`:
```cmake
effects/MyEffect.cpp
```

4. Register in `EffectChain` if needed

### New Oscillator

1. Add to `oscillators/Oscillators.h`:
```cpp
class MyOscillator : public AudioSource {
    void setParameters(float frequency, float amplitude) override;
    void render(float* audioData, int32_t numFrames) override;
};
```

2. Register in `AudioEngine` constructor

### New Node

1. Create in `nodes/`:
```cpp
// nodes/MyNode.h
class MyNode : public AudioNode {
public:
    void process(AudioBuffer& input, AudioBuffer& output, int numFrames) override;
};
```

2. Add to `CMakeLists.txt` and integrate with `AudioGraph`

---

## Performance Guidelines

### DO ✅

- Pre-allocate all buffers in constructor
- Use `std::atomic` for UI↔Audio communication
- Process in blocks (not sample-by-sample)
- Use SIMD utilities from `SIMDUtils.h`
- Flush denormals to zero

### DON'T ❌

- Allocate memory in audio callback
- Use mutex/locks in audio callback
- Call system functions in audio callback
- Use STL containers that allocate (push_back, etc.)
- Ignore denormal values (CPU slowdown)

---

## Quality Metrics

| Metric | Target | Current |
|--------|--------|---------|
| Latency | <10ms | ~5-8ms (AAudio) |
| Sample Rate | 48kHz | 48kHz |
| Bit Depth | 32-bit float | 32-bit float |
| THD+N | <0.01% | <0.01% (clean signal) |
| CPU Load | <30% | ~15-25% (typical) |

---

## File Statistics

| Category | Headers | Source Files | Total |
|----------|---------|--------------|-------|
| Core | 10 | 6 | 16 |
| Effects | 7 | 7 | 14 |
| Oscillators | 1 | 0 | 1 |
| Modulators | 8 | 0 | 8 |
| Nodes | 5 | 5 | 10 |
| Utils | 8 | 1 | 9 |
| Utils/DSP | 6 | 5 | 11 |
| Analysis | 1 | 1 | 2 |
| **Total** | **46** | **25** | **71** |

---

## License

Copyright (c) 2025 Watermellon Studios. All rights reserved.

---

*Documentation updated: 2025-12-17*
