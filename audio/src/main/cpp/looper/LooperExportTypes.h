#pragma once

#include "WavFile.h"

namespace wm {

/**
 * @brief Options for LooperExporter::exportMix / exportStems.
 *
 * Extracted from AudioLooper (plan §3.3). Kept in its own tiny header so both
 * AudioLooper.h (which aliases it as AudioLooper::ExportOptions for API
 * compatibility) and LooperExporter can depend on it without a cycle.
 *
 * Most fields have safe defaults — backward-compat callers use the single-arg
 * exportMix(path) overload.
 */
struct ExportOptions {
    wav::BitDepth bitDepth = wav::BitDepth::PCM_16;
    int repeatLoops = 1;       // export N iterations of the loop length
    int countInFrames = 0;     // leading silence (e.g. = N beats * framesPerBeat)
    bool applyLimiter = true;  // true-peak limiter instead of tanh soft-clip
    wav::WavMetadata metadata; // BPM, project name, etc. — embedded in WAV
};

}  // namespace wm
