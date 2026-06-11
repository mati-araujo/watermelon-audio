# ============================================================================
# watermelon-effects: Audio effects sub-library
#
# Phase 1B — Audio Library Extraction
#
# Depends on: watermelon-dsp (BiquadFilter, DelayLine, LFO, FDN, etc.)
# No other external dependencies.
# ============================================================================

cmake_minimum_required(VERSION 3.22.1)

add_library(watermelon-effects STATIC
    # Core
    Effect.cpp
    EffectChain.cpp
    EffectRegistry.cpp
    LookaheadLimiter.cpp

    # Filters
    FilterEffect.cpp
    ParametricEQ.cpp

    # Reverb
    ReverbEffect.cpp
    HallReverbEffect.cpp
    RiserReverbEffect.cpp
    SpringReverbEffect.cpp
    PlateReverbEffect.cpp
    ShimmerReverbEffect.cpp

    # Delay
    DelayEffect.cpp
    HpfDelayEffect.cpp
    TapeEchoEffect.cpp

    # Distortion / Amp
    DistortionEffect.cpp
    AmpSimulator.cpp
    CabinetSimulator.cpp

    # Dynamics
    CompressorEffect.cpp

    # Modulation
    ChorusEffect.cpp
    PhaserEffect.cpp
    AutoPanEffect.cpp
    ComplexTremEffect.cpp
    RandomResoEffect.cpp
    DecimatorEffect.cpp
    DeciHpfEffect.cpp

    # Special
    VocoderEffect.cpp
    BeatGrainEffect.cpp
)

target_include_directories(watermelon-effects PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(watermelon-effects PUBLIC watermelon-dsp)
target_compile_features(watermelon-effects PUBLIC cxx_std_20)
