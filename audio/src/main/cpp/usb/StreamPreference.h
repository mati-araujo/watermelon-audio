/**
 * StreamPreference.h
 *
 * Preference structure for USB audio altsetting selection.
 *
 * Encodes both hard constraints (must-have) and soft weights (prefer-if-possible)
 * that the AltsettingSelector uses to score and pick the best altsetting+format
 * combination for a given streaming session.
 *
 * Stage 2 — USB Audio Discovery & Directed Selection.
 */

#pragma once

#include <cstdint>

namespace watermelon_audio {
namespace usb {

struct StreamPreference {
    // ===== Hard constraints — altsettings that fail these are excluded =====

    /** Required sample rate in Hz. 0 means any rate is acceptable. */
    int requiredSampleRate = 48000;

    /** Minimum number of channels. */
    int minChannels = 2;

    /** If true, only altsettings with a feedback endpoint are considered. */
    bool requireFeedback = false;

    /** If true, skip the sample-rate check (useful for UAC2 where the parser
     *  doesn't populate format.sampleRates). */
    bool skipRateCheck = false;

    // ===== Soft weights — higher total score wins (0.0 = don't care) =====

    /** Weight for bit depth. Higher bit depth scores higher. */
    float bitDepthWeight = 1.0f;

    /** Weight for channel count. More channels score higher. */
    float channelCountWeight = 0.5f;

    /** Weight for sync type. Async > Adaptive > Sync. */
    float syncTypeWeight = 0.8f;

    /** Weight for having a feedback endpoint present. */
    float feedbackPresentWeight = 0.3f;

    // ===== Factory presets =====

    static StreamPreference defaultPro() {
        StreamPreference p;
        p.bitDepthWeight = 1.0f;
        p.syncTypeWeight = 1.0f;
        return p;
    }

    static StreamPreference lowestLatency() {
        StreamPreference p;
        p.requireFeedback = false;
        p.bitDepthWeight = 0.3f;
        p.syncTypeWeight = 0.5f;
        p.feedbackPresentWeight = 0.0f;
        return p;
    }

    static StreamPreference highestFidelity() {
        StreamPreference p;
        p.bitDepthWeight = 1.5f;
        p.channelCountWeight = 0.8f;
        return p;
    }
};

}  // namespace usb
}  // namespace watermelon_audio
