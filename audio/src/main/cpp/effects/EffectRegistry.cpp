/**
 * @file EffectRegistry.cpp
 * @brief Registers all built-in effects in the registry.
 *
 * Phase 1F — Audio Library Extraction.
 */

#include "EffectRegistry.h"
#include "FilterEffect.h"
#include "ReverbEffect.h"
#include "DelayEffect.h"
#include "VocoderEffect.h"
#include "DistortionEffect.h"
#include "CompressorEffect.h"
#include "ChorusEffect.h"
#include "PhaserEffect.h"
#include "AmpSimulator.h"
#include "CabinetSimulator.h"
#include "DecimatorEffect.h"
#include "DeciHpfEffect.h"
#include "AutoPanEffect.h"
#include "ComplexTremEffect.h"
#include "RandomResoEffect.h"
#include "HpfDelayEffect.h"
#include "TapeEchoEffect.h"
#include "HallReverbEffect.h"
#include "RiserReverbEffect.h"
#include "BeatGrainEffect.h"

void registerBuiltinEffects(EffectRegistry& registry) {
    registry.registerEffect(FILTER,       "Filter",       3, []{ return std::make_unique<FilterEffect>(); });
    registry.registerEffect(REVERB,       "Reverb",      12, []{ return std::make_unique<ReverbEffect>(); });
    registry.registerEffect(DELAY,        "Delay",        6, []{ return std::make_unique<DelayEffect>(); });
    registry.registerEffect(VOCODER,      "Vocoder",      7, []{ return std::make_unique<VocoderEffect>(); });
    registry.registerEffect(DISTORTION,   "Distortion",   8, []{ return std::make_unique<DistortionEffect>(); });
    registry.registerEffect(COMPRESSOR,   "Compressor",   6, []{ return std::make_unique<CompressorEffect>(); });
    registry.registerEffect(CHORUS,       "Chorus",       6, []{ return std::make_unique<ChorusEffect>(); });
    registry.registerEffect(PHASER,       "Phaser",       5, []{ return std::make_unique<PhaserEffect>(); });
    registry.registerEffect(AMP_SIM,      "AmpSim",       9, []{ return std::make_unique<AmpSimulator>(); });
    registry.registerEffect(CABINET,      "Cabinet",      4, []{ return std::make_unique<CabinetSimulator>(); });
    registry.registerEffect(DECIMATOR,    "Decimator",    3, []{ return std::make_unique<DecimatorEffect>(); });
    registry.registerEffect(DECI_HPF,     "DeciHPF",      4, []{ return std::make_unique<DeciHpfEffect>(); });
    registry.registerEffect(AUTO_PAN,     "AutoPan",      5, []{ return std::make_unique<AutoPanEffect>(); });
    registry.registerEffect(COMPLEX_TREM, "ComplexTrem",  6, []{ return std::make_unique<ComplexTremEffect>(); });
    registry.registerEffect(RANDOM_RESO,  "RandomReso",   5, []{ return std::make_unique<RandomResoEffect>(); });
    registry.registerEffect(HPF_DELAY,    "HPFDelay",     4, []{ return std::make_unique<HpfDelayEffect>(); });
    registry.registerEffect(TAPE_ECHO,    "TapeEcho",     6, []{ return std::make_unique<TapeEchoEffect>(); });
    registry.registerEffect(HALL_REVERB,  "HallReverb",   8, []{ return std::make_unique<HallReverbEffect>(); });
    registry.registerEffect(RISER_REVERB, "RiserReverb",  6, []{ return std::make_unique<RiserReverbEffect>(); });
    registry.registerEffect(BEAT_GRAIN,   "BeatGrain",    6, []{ return std::make_unique<BeatGrainEffect>(); });
}
