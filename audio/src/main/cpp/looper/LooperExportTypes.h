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

/**
 * @brief Why an import did not happen (REQ-045 D6).
 *
 * The bool importTrack() used to return could not tell a consumer whether to offer
 * "free some space", "pick another file" or "try again" — NoisyPad's own audit asked
 * for exactly this. Every value here is reached BEFORE the destination track is
 * touched, so a non-Ok status also means "the track still holds what it held".
 */
enum class ImportStatus {
    Ok = 0,
    InvalidTrack,        ///< index out of range for the active-track limit
    Io,                  ///< the file did not open
    UnsupportedFormat,   ///< not a RIFF/WAVE, or a format the reader does not decode
    BudgetExceeded,      ///< the resampled size does not fit the memory budget
    OutOfMemory,         ///< the destination track could not reserve the storage
};

}  // namespace wm
