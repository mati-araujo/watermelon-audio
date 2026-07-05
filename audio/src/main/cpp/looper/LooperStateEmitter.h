#pragma once

#include "TrackBuffer.h"
#include "LooperEventDispatcher.h"
#include <cmath>
#include <cstdlib>

namespace wm {

/**
 * @class LooperStateEmitter
 * @brief Owns the "last emitted" per-track state and pushes LooperEvents when a
 *        track's observable state crosses a threshold.
 *
 * Extracted verbatim from AudioLooper::emitStateEvents (plan §3.3 — move, not
 * redesign). Called once per audio block from the RT thread; lock-free. Keeps
 * AudioLooper::process() readable (capture → mix → master → click → emit) and
 * isolates the coalescing bookkeeping.
 *
 * Push-based replacement for the per-track polling NoisyPad's LooperViewModel
 * did every 33ms (8 tracks × 3 fields ≈ 800 JNI calls/sec). At the configured
 * thresholds we emit at most ~70 events/sec per active track (progress) plus
 * discrete events on play/stop and ≥0.5 dB peak changes. Inactive tracks emit
 * nothing.
 */
class LooperStateEmitter {
public:
    static constexpr int kMaxTracks = 16;  // matches AudioLooper::MAX_TRACKS_HW
    static constexpr int   kProgressFrameThreshold = 2048;  // ~43ms @ 48k
    static constexpr float kPeakDbThreshold = 0.5f;         // 0.5 dB
    // Emit RecordProgress when |Δprogress| crosses this (~50 events over a full
    // take) — the push replacement for NoisyPad's 33ms record polling.
    static constexpr float kRecordProgressThreshold = 0.02f;

    /**
     * @brief Diff each track's observable state against the last emitted
     *        snapshot and push events onto the dispatcher when a threshold is
     *        crossed. RT-safe. Call once per audio block.
     * @param tracks          Track array (length numTracks, ≤ kMaxTracks).
     * @param numTracks        Number of tracks to scan.
     * @param dispatcher       Event sink (non-owning; must be non-null).
     * @param recordingTrack   Track currently being captured, or < 0 for none.
     * @param recordProgress   Record progress [0,1] of recordingTrack.
     */
    void emit(TrackBuffer* tracks, int numTracks,
              LooperEventDispatcher* dispatcher,
              int recordingTrack, float recordProgress) {
        if (numTracks > kMaxTracks) numTracks = kMaxTracks;
        for (int i = 0; i < numTracks; ++i) {
            auto& track = tracks[i];
            const bool active = track.isActive();

            // Inactive tracks: only emit a one-shot "stopped" if we previously
            // told the UI they were playing — then skip everything else.
            if (!active) {
                if (mLastEmittedPlaying[i]) {
                    wm::LooperEvent ev{
                        wm::LooperEvent::Type::PlayingChanged, i, 0.0f
                    };
                    dispatcher->pushFromRT(ev);
                    mLastEmittedPlaying[i] = false;
                }
                continue;
            }

            // --- isPlaying ---
            const bool playing = track.isTrackPlaying();
            if (playing != mLastEmittedPlaying[i]) {
                wm::LooperEvent ev{
                    wm::LooperEvent::Type::PlayingChanged,
                    i,
                    playing ? 1.0f : 0.0f
                };
                dispatcher->pushFromRT(ev);
                mLastEmittedPlaying[i] = playing;
            }

            // --- finished finite play count (F3.4) --- follows the
            // PlayingChanged(false) above so the UI can tell it apart.
            if (track.consumeCompleted()) {
                dispatcher->pushFromRT(
                    wm::LooperEvent{wm::LooperEvent::Type::TrackCompleted, i, 0.0f});
            }

            // --- progress (only meaningful while playing) ---
            if (playing) {
                const int head = track.getPlayHead();
                const int last = mLastEmittedPlayhead[i];
                // Wrap-aware delta: if head jumped backward (loop wrap) the
                // unsigned diff still triggers an emit, which is what we want.
                const int delta = std::abs(head - last);
                if (delta >= kProgressFrameThreshold || last < 0) {
                    wm::LooperEvent ev{
                        wm::LooperEvent::Type::Progress, i, track.getProgress()
                    };
                    dispatcher->pushFromRT(ev);
                    mLastEmittedPlayhead[i] = head;
                }
            } else {
                // Invalidate cached playhead so next play start re-emits.
                mLastEmittedPlayhead[i] = -1;
            }

            // --- peak level (dB-domain threshold) ---
            // peak is a linear amplitude 0..1. Compare in dB so small
            // changes near silence aren't drowned out by large absolute
            // changes near full-scale, and vice versa.
            const float peak = track.getPeakLevel();
            const float peakDb = (peak > 1e-6f)
                ? 20.0f * std::log10(peak)
                : -120.0f;
            if (std::abs(peakDb - mLastEmittedPeakDb[i]) >= kPeakDbThreshold) {
                wm::LooperEvent ev{
                    wm::LooperEvent::Type::PeakChanged, i, peak
                };
                dispatcher->pushFromRT(ev);
                mLastEmittedPeakDb[i] = peakDb;
            }
        }

        // --- record progress (QW-5) ---
        // Push the record progress of the track being captured on threshold
        // crossings, and a single value<0 sentinel when it stops, so the UI can
        // retire its polling loop entirely.
        if (recordingTrack >= 0) {
            if (recordingTrack != mLastRecordTrack ||
                std::abs(recordProgress - mLastEmittedRecordProgress) >= kRecordProgressThreshold) {
                dispatcher->pushFromRT(
                    wm::LooperEvent{wm::LooperEvent::Type::RecordProgress,
                                    recordingTrack, recordProgress});
                mLastEmittedRecordProgress = recordProgress;
                mLastRecordTrack = recordingTrack;
            }
        } else if (mLastRecordTrack >= 0) {
            // Recording just ended — one terminal event (value<0) so the UI can
            // clear its recording state without polling isRecording().
            dispatcher->pushFromRT(
                wm::LooperEvent{wm::LooperEvent::Type::RecordProgress, mLastRecordTrack, -1.0f});
            mLastRecordTrack = -1;
            mLastEmittedRecordProgress = -1.0f;
        }
    }

private:
    // Last-emitted state per track — audio thread only, no synchronization.
    // playhead==-1 sentinel means "no prior emission since (re)play start",
    // forcing the next playing frame to emit a fresh progress event. Initialised
    // in the constructor (kMaxTracks == 16) rather than a brace list.
    int   mLastEmittedPlayhead[kMaxTracks];
    bool  mLastEmittedPlaying [kMaxTracks];
    float mLastEmittedPeakDb  [kMaxTracks];
    // Record-progress push state.
    float mLastEmittedRecordProgress{-1.0f};
    int   mLastRecordTrack{-1};

public:
    LooperStateEmitter() {
        for (int i = 0; i < kMaxTracks; ++i) {
            mLastEmittedPlayhead[i] = -1;
            mLastEmittedPlaying[i]  = false;
            mLastEmittedPeakDb[i]   = -120.0f;
        }
    }
};

}  // namespace wm
