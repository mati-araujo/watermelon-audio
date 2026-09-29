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
 * for exactly this.
 *
 * 🔴 **What each value says about the destination track.** Every value EXCEPT
 * `OutOfMemory` is decided before the track is touched, so it also means "the track
 * still holds what it held, still unmuted". `OutOfMemory` splits in two: when the
 * request is unallocatable by construction it is refused up front like the others,
 * but when the allocator fails at the reservation — after the decode already held the
 * same amount of RAM — the track is left EMPTY and UNMUTED. Keeping the old take
 * through that would require both takes alive at once, which is the peak the memory
 * budget exists to forbid (AC-045.8, re-declared after measuring).
 */
enum class ImportStatus {
    Ok = 0,
    InvalidTrack,        ///< index out of range for the active-track limit
    InvalidArgument,     ///< a caller argument makes no sense (target sample rate <= 0)
    Io,                  ///< the file did not open
    UnsupportedFormat,   ///< not a RIFF/WAVE, or a format the reader does not decode
    BudgetExceeded,      ///< the source decode or the reservation does not fit the budget
    OutOfMemory,         ///< the request is unallocatable, or the reservation failed
};

}  // namespace wm
