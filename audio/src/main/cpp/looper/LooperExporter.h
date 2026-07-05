#pragma once

#include "LooperExportTypes.h"
#include "WavFile.h"
#include <vector>

class AudioLooper;  // friend of this class; full definition in AudioLooper.h
class TrackBuffer;  // read via sampleAt() so export works on both storage backends

namespace wm {

/**
 * @class LooperExporter
 * @brief Offline WAV export/import for AudioLooper (mix / stems / single track,
 *        import + resample, snapshot).
 *
 * Extracted verbatim from AudioLooper (plan §3.3 — move, not redesign). This is
 * NOT real-time code: it runs on an IO/background thread. Pulling it out of the
 * header stops ~500 lines of export/import/resample logic from recompiling into
 * every translation unit that only needs the RT looper.
 *
 * It operates on an AudioLooper by reference and is declared a friend of it, so
 * it reads track buffers and the export-guard atomics directly, exactly as the
 * inline code used to. The RT snapshot contract is unchanged: an
 * AudioLooper::ExportGuard is held for the whole export so the audio thread
 * skips destructive writes while the buffers are read.
 *
 * Lifetime: construct a transient LooperExporter around an AudioLooper for a
 * single call (AudioLooper's public export* / importTrack methods do exactly
 * this). Not thread-safe; serialize export/import on the IO thread as before.
 */
class LooperExporter {
public:
    explicit LooperExporter(AudioLooper& looper) : mL(looper) {}

    bool exportMix(const char* filePath, const ExportOptions& opts);
    int  exportStems(const char* directory, const ExportOptions& opts);
    bool exportTrack(int trackIndex, const char* filePath, const ExportOptions& opts);
    bool captureTrack(int trackIndex, const char* filePath, wav::BitDepth bitDepth);
    bool importTrack(int trackIndex, const char* filePath, int sampleRate);

private:
    struct TrackSnapshot {
        bool active = false;
        bool muted = false;
        int  length = 0;
        int  loopStart = 0;
        int  loopEnd = 0;
        float volume = 1.0f;
        float pan = 0.0f;
        // Read through the track (sampleAt) rather than a raw pointer, so export
        // works with both the dense and the paged (chunked) storage. Valid only
        // while the ExportGuard is held.
        const TrackBuffer* track = nullptr;
    };

    struct ExportSnapshot {
        static constexpr int kMaxTracks = 16;  // must match AudioLooper::MAX_TRACKS_HW
        TrackSnapshot tracks[kMaxTracks];
        int frames = 0;        // longest active track length (no count-in / repeat applied)
        int sampleRate = 48000;
    };

    ExportSnapshot takeSnapshot() const;
    void mixTrackInto(float* output, int outputFrames,
                      const TrackSnapshot& ts, int countInFrames) const;
    static std::vector<float> resampleStereo(const float* in, int inFrames,
                                             int inSR, int outSR);
    bool exportMixInternal(const char* filePath, const ExportOptions& opts);

    AudioLooper& mL;
};

}  // namespace wm
