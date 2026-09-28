// ============================================================================
// LooperExporter.cpp — offline WAV export/import for AudioLooper.
//
// Extracted verbatim from AudioLooper (plan §3.3 — move, not redesign). Bodies
// live here (not in the header) because export/import/resample is IO-thread
// code, not RT, and it recompiled into every consumer of AudioLooper.h.
//
// LooperExporter is a friend of AudioLooper, so it reads track buffers and the
// export-guard atomics directly — same access the inline code had.
// ============================================================================
#include "LooperExporter.h"
#include "AudioLooper.h"
#include "Limiter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "../platform/Logger.h"
#define EXP_LOGD(...) wma::logMessage(wma::LogLevel::DEBUG, "Looper", __VA_ARGS__)
#define EXP_LOGE(...) wma::logMessage(wma::LogLevel::ERROR, "Looper", __VA_ARGS__)

namespace wm {

// ---------------------------------------------------------------------------
// Snapshot + mixing helpers
// ---------------------------------------------------------------------------

LooperExporter::ExportSnapshot LooperExporter::takeSnapshot() const {
    ExportSnapshot s;
    s.sampleRate = mL.mSampleRate.load(std::memory_order_acquire);
    for (int t = 0; t < AudioLooper::MAX_TRACKS; ++t) {
        const auto& track = mL.mTracks[t];
        auto& ts = s.tracks[t];
        ts.active = track.isActive();
        if (!ts.active) continue;
        ts.muted = track.isMuted();
        ts.length = track.getLengthFrames();
        ts.loopStart = track.getLoopStart();
        ts.loopEnd = track.getLoopEnd();
        ts.volume = track.getVolume();
        ts.pan = track.getPan();
        ts.track = &track;
        // 🔴 The mix is as long as the longest LOOP, not the longest buffer.
        // `mixTrackInto` wraps each track on its region (`loopStart + t % loopLen`),
        // so sizing the output by the buffer wrote MORE than one lap: measured on a
        // real free take, a `repeatLoops = 1` export came out 1.25 laps long, cut
        // mid-lap — an exported loop that does not loop. Same rule as
        // `AudioLooper::getMasterLoopFrames`, so a rendered video and its audio
        // cannot drift apart.
        const int loopEnd = (ts.loopEnd > 0) ? ts.loopEnd : ts.length;
        const int loopLen = loopEnd - ts.loopStart;
        if (loopLen > s.frames) s.frames = loopLen;
        if (s.sampleRate <= 0) s.sampleRate = track.getSampleRate();
    }
    return s;
}

void LooperExporter::mixTrackInto(float* output, int outputFrames,
                                  const TrackSnapshot& ts, int countInFrames) const {
    if (!ts.active || ts.muted || !ts.track || ts.length <= 0) return;
    const int loopStart = ts.loopStart;
    const int loopEnd = (ts.loopEnd > 0) ? ts.loopEnd : ts.length;
    const int loopLen = std::max(1, loopEnd - loopStart);

    const auto pp = wm::EqualPowerPanLUT::instance().lookup(ts.pan);
    const float gainL = ts.volume * pp.l;
    const float gainR = ts.volume * pp.r;

    for (int i = countInFrames; i < outputFrames; ++i) {
        const int t = i - countInFrames;
        const int pos = loopStart + (t % loopLen);
        output[i * 2]     += ts.track->sampleAt(pos, 0) * gainL;
        output[i * 2 + 1] += ts.track->sampleAt(pos, 1) * gainR;
    }
}

// Catmull-Rom cubic resample of an interleaved-stereo buffer from [inSR] to
// [outSR]. Used to export at a target rate (e.g. 48k → 44.1k). Boundary frames
// clamp their neighbours (no wrap — the render is not a seamless loop here).
std::vector<float> LooperExporter::resampleStereo(const float* in, int inFrames,
                                                  int inSR, int outSR) {
    if (!in || inFrames <= 0 || inSR <= 0 || outSR <= 0 || inSR == outSR) {
        const int n = (in && inFrames > 0) ? inFrames : 0;
        return std::vector<float>(in, in + static_cast<size_t>(n) * 2);
    }
    const double ratio = static_cast<double>(outSR) / static_cast<double>(inSR);
    const int outFrames = static_cast<int>(std::ceil(inFrames * ratio));
    std::vector<float> out(static_cast<size_t>(outFrames) * 2);
    const int srcLast = inFrames - 1;
    for (int i = 0; i < outFrames; ++i) {
        const double srcPos = i / ratio;
        const int s1 = std::min(static_cast<int>(srcPos), srcLast);
        const int s0 = (s1 > 0) ? s1 - 1 : 0;
        const int s2 = std::min(s1 + 1, srcLast);
        const int s3 = std::min(s1 + 2, srcLast);
        const float t = static_cast<float>(srcPos - s1);
        const float t2 = t * t;
        const float t3 = t2 * t;
        for (int ch = 0; ch < 2; ++ch) {
            const float p0 = in[s0 * 2 + ch];
            const float p1 = in[s1 * 2 + ch];
            const float p2 = in[s2 * 2 + ch];
            const float p3 = in[s3 * 2 + ch];
            out[i * 2 + ch] = 0.5f * ((2.0f * p1)
                + (-p0 + p2) * t
                + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
                + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Export mix
// ---------------------------------------------------------------------------

bool LooperExporter::exportMixInternal(const char* filePath, const ExportOptions& opts) {
    if (!filePath) return false;
    AudioLooper::ExportGuard guard(mL);

    const ExportSnapshot snap = takeSnapshot();
    if (snap.frames <= 0) return false;

    const int repeats = std::max(1, opts.repeatLoops);
    const int countIn = std::max(0, opts.countInFrames);
    // En int64 y con rechazo explícito. En int esta cuenta desbordaba: el clamp
    // de `wma_looper_*` deja `countInFrames` en INT32_MAX, y sumarle un solo
    // frame ya es overflow con signo — UB. Daba verde en un build normal porque
    // el wrap negativo hacía tirar a la alocación y el borde de la C API lo
    // convertía en el `false` documentado, o sea que el test pasaba *por* el UB.
    // Ahora se rechaza antes, que además no depende del ancho de `size_t` (en las
    // ABIs de 32 bits el mismo camino truncaba en vez de tirar).
    const int64_t total64 = static_cast<int64_t>(snap.frames) * repeats + countIn;
    if (total64 > INT32_MAX) return false;
    const int totalFrames = static_cast<int>(total64);
    const int sr = (snap.sampleRate > 0) ? snap.sampleRate : 48000;

    std::vector<float> mixBuffer(static_cast<size_t>(totalFrames) * 2, 0.0f);

    // Mix each track. Progress is reported per active track.
    int activeCount = 0;
    for (int t = 0; t < AudioLooper::MAX_TRACKS; ++t) {
        if (snap.tracks[t].active && !snap.tracks[t].muted) ++activeCount;
    }
    if (activeCount == 0) {
        // All tracks muted/inactive — write silence (still useful for count-in tests).
        return wav::writeWav(filePath, mixBuffer.data(), totalFrames,
                             sr, opts.bitDepth, opts.metadata);
    }

    int processed = 0;
    for (int t = 0; t < AudioLooper::MAX_TRACKS; ++t) {
        if (mL.mCancelExport.load(std::memory_order_acquire)) return false;
        const auto& ts = snap.tracks[t];
        if (!ts.active || ts.muted) continue;
        mixTrackInto(mixBuffer.data(), totalFrames, ts, countIn);
        ++processed;
        // Reserve [0..0.85] for mixing, [0.85..1.0] for limiter+IO.
        mL.updateExportProgress(0.85f * static_cast<float>(processed) /
                                static_cast<float>(activeCount));
    }

    if (mL.mCancelExport.load(std::memory_order_acquire)) return false;

    if (opts.applyLimiter) {
        wm::OfflineLimiter limiter;
        limiter.prepare(sr);
        limiter.processStereo(mixBuffer.data(), totalFrames);
    }
    mL.updateExportProgress(0.95f);

    // Resample the finished mix to the requested export rate (0 = engine rate).
    int outSr = sr;
    const float* writePtr = mixBuffer.data();
    int writeFrames = totalFrames;
    std::vector<float> resampled;
    const int targetSr = mL.mExportSampleRate.load(std::memory_order_acquire);
    if (targetSr > 0 && targetSr != sr) {
        resampled = resampleStereo(mixBuffer.data(), totalFrames, sr, targetSr);
        writePtr = resampled.data();
        writeFrames = static_cast<int>(resampled.size() / 2);
        outSr = targetSr;
    }
    const bool ok = wav::writeWav(filePath, writePtr, writeFrames,
                                  outSr, opts.bitDepth, opts.metadata);
    mL.mExportProgress.store(1.0f, std::memory_order_release);
    if (ok) mL.mExportsCompleted.fetch_add(1, std::memory_order_relaxed);
    else    mL.mExportsFailed.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool LooperExporter::exportMix(const char* filePath, const ExportOptions& opts) {
    return exportMixInternal(filePath, opts);
}

// ---------------------------------------------------------------------------
// Export stems
// ---------------------------------------------------------------------------

int LooperExporter::exportStems(const char* directory, const ExportOptions& opts) {
    if (!directory) return -1;
    const AudioLooper::ExportGuard guard(mL);

    const ExportSnapshot snap = takeSnapshot();
    if (snap.frames <= 0) return -1;

    // Mismo desborde que en exportMixInternal, misma forma de rechazarlo. No lo
    // acusó ningún test —el de UBSan entra por exportMix— pero es el mismo bug:
    // arreglar uno solo dejaría el otro esperando a que alguien lo llame.
    const int64_t total64 = static_cast<int64_t>(snap.frames)
                              * std::max(1, opts.repeatLoops)
                          + std::max(0, opts.countInFrames);
    if (total64 > INT32_MAX) return -1;
    const int totalFrames = static_cast<int>(total64);
    const int sr = (snap.sampleRate > 0) ? snap.sampleRate : 48000;

    std::string base = directory;
    if (!base.empty() && base.back() != '/' && base.back() != '\\') base.push_back('/');

    wm::OfflineLimiter limiter;
    if (opts.applyLimiter) limiter.prepare(sr);

    std::vector<float> stem(static_cast<size_t>(totalFrames) * 2, 0.0f);
    int written = 0;
    for (int t = 0; t < AudioLooper::MAX_TRACKS; ++t) {
        if (mL.mCancelExport.load(std::memory_order_acquire)) return -1;
        const auto& ts = snap.tracks[t];
        if (!ts.active || ts.muted || ts.length <= 0) continue;

        std::fill(stem.begin(), stem.end(), 0.0f);
        mixTrackInto(stem.data(), totalFrames, ts, opts.countInFrames);
        if (opts.applyLimiter) limiter.processStereo(stem.data(), totalFrames);

        const std::string path = base + "track_" + std::to_string(t) + ".wav";
        // Resample each stem to the requested export rate (0 = engine rate).
        const float* stemPtr = stem.data();
        int stemFrames = totalFrames;
        int stemSr = sr;
        std::vector<float> stemResampled;
        const int targetSr = mL.mExportSampleRate.load(std::memory_order_acquire);
        if (targetSr > 0 && targetSr != sr) {
            stemResampled = resampleStereo(stem.data(), totalFrames, sr, targetSr);
            stemPtr = stemResampled.data();
            stemFrames = static_cast<int>(stemResampled.size() / 2);
            stemSr = targetSr;
        }
        if (!wav::writeWav(path.c_str(), stemPtr, stemFrames,
                           stemSr, opts.bitDepth, opts.metadata)) {
            EXP_LOGE("exportStems: failed to write %s", path.c_str());
            continue;
        }
        ++written;
        mL.mStemsWritten.fetch_add(1, std::memory_order_relaxed);
        mL.updateExportProgress(static_cast<float>(written) /
                                static_cast<float>(AudioLooper::MAX_TRACKS));
    }
    mL.mExportProgress.store(1.0f, std::memory_order_release);
    if (written > 0) mL.mExportsCompleted.fetch_add(1, std::memory_order_relaxed);
    else             mL.mExportsFailed.fetch_add(1, std::memory_order_relaxed);
    return written;
}

// ---------------------------------------------------------------------------
// Export / capture single track
// ---------------------------------------------------------------------------

bool LooperExporter::exportTrack(int trackIndex, const char* filePath,
                                 const ExportOptions& opts) {
    if (trackIndex < 0 || trackIndex >= AudioLooper::MAX_TRACKS) return false;
    if (!mL.mTracks[trackIndex].isActive()) return false;

    const AudioLooper::ExportGuard guard(mL);
    const TrackBuffer& track = mL.mTracks[trackIndex];
    int len = track.getLengthFrames();
    int sr = track.getSampleRate();

    // If a custom loop region is defined, export only the region.
    int regionStart = track.getLoopStart();
    int regionEnd = track.getLoopEnd();
    int writeStart = 0;
    int writeLen = len;
    if (regionStart > 0 || regionEnd < len) {
        int regionLen = regionEnd - regionStart;
        if (regionLen > 0) {
            writeStart = regionStart;
            writeLen = regionLen;
        }
    }
    if (writeLen <= 0) return false;

    // Materialise a dense copy of the region (storage-agnostic: works for the
    // paged backend too). IO thread under the guard — the transient copy is fine.
    std::vector<float> dense(static_cast<size_t>(writeLen) * 2);
    for (int i = 0; i < writeLen; ++i) {
        dense[i * 2]     = track.sampleAt(writeStart + i, 0);
        dense[i * 2 + 1] = track.sampleAt(writeStart + i, 1);
    }
    return wav::writeWav(filePath, dense.data(), writeLen, sr,
                         opts.bitDepth, opts.metadata);
}

bool LooperExporter::captureTrack(int trackIndex, const char* filePath,
                                  wav::BitDepth bitDepth) {
    if (trackIndex < 0 || trackIndex >= AudioLooper::MAX_TRACKS) return false;
    if (!mL.mTracks[trackIndex].isActive()) return false;

    const AudioLooper::ExportGuard guard(mL);
    const TrackBuffer& track = mL.mTracks[trackIndex];
    const int len = track.getLengthFrames();
    const int sr = track.getSampleRate();
    if (len <= 0) return false;

    // Full-buffer dense copy (storage-agnostic). IO thread under the guard.
    std::vector<float> dense(static_cast<size_t>(len) * 2);
    for (int i = 0; i < len; ++i) {
        dense[i * 2]     = track.sampleAt(i, 0);
        dense[i * 2 + 1] = track.sampleAt(i, 1);
    }
    wav::WavMetadata meta;
    return wav::writeWav(filePath, dense.data(), len, sr, bitDepth, meta);
}

// ---------------------------------------------------------------------------
// Import (+ resample)
// ---------------------------------------------------------------------------

bool LooperExporter::importTrack(int trackIndex, const char* filePath, int sampleRate) {
    return importTrackChecked(trackIndex, filePath, sampleRate) == ImportStatus::Ok;
}

/**
 * REQ-045 D6 — import that rejects from the HEADER and never destroys the target.
 *
 * What changed, and why the order is the fix:
 *
 *  1. The memory-budget decision is taken from the header alone (frames, rate,
 *     channels), so a 5-minute file over the budget costs one open + a few reads
 *     instead of decoding and resampling ~55 MB first. The size is derived with the
 *     SAME `ceil(numFrames × ratio)` formula the resample below uses — two formulas
 *     would mean the check and the allocation disagree by a frame at some ratio.
 *  2. Everything VALIDABLE is done BEFORE the destination track is touched: argument,
 *     header, representability, budget (of the source decode AND of the reservation),
 *     decode and resample. Before this, `setMuted(true)` + `clear()` ran first and a
 *     failing `allocate()` left the track EMPTY AND MUTED: an out-of-memory import
 *     silently deleted a take.
 *
 *     🔴 **What is NOT closed, and cannot be without the budget the import is enforcing:**
 *     the allocator itself failing when the track reserves, after the decode already
 *     proved the same amount of RAM. Keeping the old take through that needs A and B
 *     alive at once, which is the very peak the budget forbids — and the `clear()` is
 *     also what performs the `waitForRenderIdle()` handshake and returns the chunks. In
 *     that one case the track is left EMPTY, **UNMUTED** and the cause is reported as
 *     `OutOfMemory`. That is the contract (AC-045.8, re-declared after measuring); the
 *     four doc comments on this path say exactly that and nothing wider.
 *  3. writeFrame()'s return is read. It reports a dropped frame (pool exhausted);
 *     ignoring it produced a track that reported the full length with silence in it.
 *  4. **Transient peak.** A rejected import costs O(header). An accepted one peaks at
 *     `source decode (numFrames × 2 × 4) + resample (outputFrames × 2 × 4) + the
 *     track's reservation`, plus the raw payload `readWav` holds while converting. With
 *     a ratio < 1 (192 kHz → 48 kHz) the SOURCE buffer is the big one and the output
 *     says nothing about it, so the source decode is budget-checked on its own — that
 *     was a 430 MB peak sneaking past a 48 MB budget.
 */
ImportStatus LooperExporter::importTrackChecked(int trackIndex, const char* filePath,
                                               int sampleRate) {
    if (trackIndex < 0 || trackIndex >= mL.getMaxActiveTracks()) {
        EXP_LOGE("importTrack FAILED: track %d is outside the active-track limit (%d)",
                 trackIndex, mL.getMaxActiveTracks());
        return ImportStatus::InvalidTrack;
    }
    // A target rate of 0 or less is the caller's mistake, not a memory problem: it used
    // to fall through the resample arithmetic and come back as OutOfMemory.
    if (sampleRate <= 0) {
        EXP_LOGE("importTrack FAILED: target sample rate %d is not positive", sampleRate);
        return ImportStatus::InvalidArgument;
    }

    // ---- 1. Header only: not one sample decoded, not one byte of payload read.
    wav::WavInfo info;
    const wav::WavReadStatus headerStatus = wav::readWavInfo(filePath, info);
    if (headerStatus == wav::WavReadStatus::Io) {
        EXP_LOGE("importTrack FAILED: the file did not open");
        return ImportStatus::Io;
    }
    if (headerStatus != wav::WavReadStatus::Ok || info.numFrames <= 0) {
        EXP_LOGE("importTrack FAILED: unsupported format or corrupt file");
        return ImportStatus::UnsupportedFormat;
    }

    // ---- 2. The resampled size, from the header.
    const bool needsResampleFromHeader = (info.sampleRate > 0 && info.sampleRate != sampleRate);
    int64_t outputFrames64 = info.numFrames;
    if (needsResampleFromHeader) {
        const double headerRatio = static_cast<double>(sampleRate)
                                 / static_cast<double>(info.sampleRate);
        outputFrames64 = static_cast<int64_t>(std::ceil(info.numFrames * headerRatio));
    }
    // A length no track can address. Not a format problem and not the budget's
    // doing: the request is simply larger than the storage can hold, which is the
    // same answer an allocation failure gives.
    if (outputFrames64 <= 0 || outputFrames64 > static_cast<int64_t>(INT32_MAX)) {
        EXP_LOGE("importTrack FAILED: %lld frames after resample is unallocatable",
                 static_cast<long long>(outputFrames64));
        return ImportStatus::OutOfMemory;
    }

    // ---- 3. Budget, before decoding. TWO questions, and the second one is not
    // implied by the first.
    const size_t currentUsage = mL.getTotalReservedBytes();
    const size_t trackCurrent = mL.mTracks[trackIndex].reservedBytes();
    const size_t budget = mL.getMemoryBudgetBytes();

    // (a) the SOURCE decode. With a ratio < 1 the output is smaller than the file, so
    // the reservation below says nothing about the transient: a 192 MB source
    // downsampled to 48 kHz reserves 24 MB and peaks at ~430 MB while decoding. The
    // budget is the only ceiling this engine has, so it bounds the transient too.
    const size_t sourceDecodeBytes = static_cast<size_t>(info.numFrames) * 2 * sizeof(float);
    if (sourceDecodeBytes > budget) {
        EXP_LOGE("importTrack FAILED: decoding the source needs %zu B, budget is %zu B",
                 sourceDecodeBytes, budget);
        return ImportStatus::BudgetExceeded;
    }

    // (b) what the track will RESERVE — computed by the same function the reservation
    // uses, because the paged backend rounds up to whole 256 KB chunks. Using the
    // logical size here let a check pass whose reservation then landed over the budget.
    const size_t needed = TrackStorage::reservationBytesFor(static_cast<int>(outputFrames64));
    if (currentUsage - trackCurrent + needed > budget) {
        EXP_LOGE("importTrack FAILED: memory budget exceeded (need %zu, budget %zu, used %zu)",
                 needed, budget, currentUsage - trackCurrent);
        return ImportStatus::BudgetExceeded;
    }

    EXP_LOGD("importTrack: reading %s", filePath);
    wav::WavData wavData = wav::readWav(filePath);
    if (wavData.numFrames <= 0) {
        EXP_LOGE("importTrack FAILED: readWav returned 0 frames (unsupported format or corrupt file)");
        return ImportStatus::UnsupportedFormat;
    }
    EXP_LOGD("importTrack: %d frames, %dHz, %d ch", wavData.numFrames, wavData.sampleRate, wavData.numChannels);

    // Resample if source sample rate differs from target (e.g., 44100 → 48000)
    bool needsResample = (wavData.sampleRate > 0 && wavData.sampleRate != sampleRate);
    int outputFrames = wavData.numFrames;
    std::vector<float> resampledBuffer;

    if (needsResample) {
        double ratio = static_cast<double>(sampleRate) / static_cast<double>(wavData.sampleRate);
        outputFrames = static_cast<int>(std::ceil(wavData.numFrames * ratio));
        EXP_LOGD("Resampling %dHz -> %dHz (ratio=%.4f, %d -> %d frames)",
                 wavData.sampleRate, sampleRate, ratio, wavData.numFrames, outputFrames);

        resampledBuffer.resize(static_cast<size_t>(outputFrames) * 2);
        // Catmull-Rom cubic resample. For boundary frames the neighbours
        // clamp to [0, numFrames-1] (no wrap — sources are not loops).
        const int srcLast = wavData.numFrames - 1;
        for (int i = 0; i < outputFrames; ++i) {
            const double srcPos = i / ratio;
            const int s1 = std::min(static_cast<int>(srcPos), srcLast);
            const int s0 = (s1 > 0) ? s1 - 1 : 0;
            const int s2 = std::min(s1 + 1, srcLast);
            const int s3 = std::min(s1 + 2, srcLast);
            const float t = static_cast<float>(srcPos - s1);
            const float t2 = t * t;
            const float t3 = t2 * t;
            for (int ch = 0; ch < 2; ++ch) {
                const float p0 = wavData.buffer[s0 * 2 + ch];
                const float p1 = wavData.buffer[s1 * 2 + ch];
                const float p2 = wavData.buffer[s2 * 2 + ch];
                const float p3 = wavData.buffer[s3 * 2 + ch];
                resampledBuffer[i * 2 + ch] = 0.5f * ((2.0f * p1)
                    + (-p0 + p2) * t
                    + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
                    + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
            }
        }
    }

    const float* srcBuffer = needsResample ? resampledBuffer.data() : wavData.buffer.data();

    // Nothing above this line touched the destination track: a failure so far leaves
    // the previous take playing exactly as it was (AC-045.8).

    // Stop playback on this track before clearing to avoid race with audio thread.
    mL.mTracks[trackIndex].setMuted(true);
    mL.mTracks[trackIndex].setPlaying(false);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // Clear existing content
    mL.mTracks[trackIndex].clear();

    // Allocate and fill with (possibly resampled) data. allocate() pre-reserves the
    // pool for the full capacity, so the writes below never hit an empty pool.
    size_t allocated = mL.mTracks[trackIndex].allocate(outputFrames, sampleRate);
    if (allocated == 0) {
        // REACHABLE, and this is the one case AC-045.8 leaves open: the chunk pool (or
        // the dense vector) could not get the memory even though the decode above just
        // held the same amount. The track is already cleared — undoing that would need
        // both takes alive, the peak the budget forbids — so it is left EMPTY and
        // UNMUTED, with the cause reported. Unmuting matters: empty-and-muted reads
        // from the UI as a volume bug instead of a failed import.
        mL.mTracks[trackIndex].setMuted(false);
        EXP_LOGE("importTrack FAILED: the track could not reserve %d frames", outputFrames);
        return ImportStatus::OutOfMemory;
    }

    for (int i = 0; i < outputFrames; ++i) {
        if (!mL.mTracks[trackIndex].writeFrame(srcBuffer[i * 2], srcBuffer[i * 2 + 1])) {
            // A dropped frame means the storage ran out mid-fill: the track would
            // report the full length with a hole in it. Report the cause instead.
            mL.mTracks[trackIndex].clear();
            mL.mTracks[trackIndex].setMuted(false);
            EXP_LOGE("importTrack FAILED: storage exhausted at frame %d of %d", i, outputFrames);
            return ImportStatus::OutOfMemory;
        }
    }
    mL.mTracks[trackIndex].finalizeRecording();
    mL.mTracks[trackIndex].setMuted(false);
    mL.mEnabled.store(true, std::memory_order_release);
    return ImportStatus::Ok;
}

}  // namespace wm

// ---------------------------------------------------------------------------
// AudioLooper thin forwarders (defined here so the bodies stay out of the
// header; the API surface AudioLooper exposes is unchanged).
// ---------------------------------------------------------------------------

bool AudioLooper::exportMix(const char* filePath, const ExportOptions& opts) {
    return wm::LooperExporter(*this).exportMix(filePath, opts);
}
int AudioLooper::exportStems(const char* directory, const ExportOptions& opts) {
    return wm::LooperExporter(*this).exportStems(directory, opts);
}
bool AudioLooper::exportTrack(int trackIndex, const char* filePath, const ExportOptions& opts) {
    return wm::LooperExporter(*this).exportTrack(trackIndex, filePath, opts);
}
bool AudioLooper::captureTrack(int trackIndex, const char* filePath, wav::BitDepth bitDepth) {
    return wm::LooperExporter(*this).captureTrack(trackIndex, filePath, bitDepth);
}
bool AudioLooper::importTrack(int trackIndex, const char* filePath, int sampleRate) {
    return wm::LooperExporter(*this).importTrack(trackIndex, filePath, sampleRate);
}
wm::ImportStatus AudioLooper::importTrackChecked(int trackIndex, const char* filePath,
                                                int sampleRate) {
    return wm::LooperExporter(*this).importTrackChecked(trackIndex, filePath, sampleRate);
}
