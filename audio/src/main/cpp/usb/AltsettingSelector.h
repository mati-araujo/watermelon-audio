/**
 * AltsettingSelector.h
 *
 * Preference-driven altsetting selection for USB Audio streaming.
 *
 * Replaces the simple "highest bit depth" heuristic from stage 1 with a
 * weighted scoring algorithm that considers bit depth, channel count,
 * sync type, and feedback endpoint presence.
 *
 * All methods are static and stateless — the selector is a pure function
 * from (topology, preference) → scored match.
 *
 * Stage 2 — USB Audio Discovery & Directed Selection.
 */

#pragma once

#include "UsbAudioTypes.h"
#include "StreamPreference.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace watermelon_audio {
namespace usb {

/**
 * Result of scoring a single (altsetting, format) pair.
 */
struct ScoredMatch {
    const UsbStreamingInterface* altsetting = nullptr;
    const UsbAudioFormat* format = nullptr;
    float score = 0.0f;
};

/**
 * Stateless selector that picks the best altsetting for a given preference.
 */
class AltsettingSelector {
public:
    /**
     * Pick the best playback altsetting from the device's playbackInterfaces.
     * Returns nullopt if no altsetting satisfies the hard constraints.
     */
    static std::optional<ScoredMatch> pickPlayback(
        const UsbAudioDevice& device,
        const StreamPreference& pref)
    {
        return pickBest(device.playbackInterfaces, pref,
                        device.uacVersion == 2);
    }

    /**
     * Pick the best capture altsetting from the device's captureInterfaces.
     * Returns nullopt if no altsetting satisfies the hard constraints.
     */
    static std::optional<ScoredMatch> pickCapture(
        const UsbAudioDevice& device,
        const StreamPreference& pref)
    {
        return pickBest(device.captureInterfaces, pref,
                        device.uacVersion == 2);
    }

    /**
     * Score all (altsetting, format) combinations and return the full list.
     * Only entries that pass the hard constraints are included.
     * Useful for UI ranking.
     */
    static std::vector<ScoredMatch> scoreAll(
        const std::vector<UsbStreamingInterface>& altsettings,
        const StreamPreference& pref,
        bool isUac2)
    {
        std::vector<ScoredMatch> results;

        for (const auto& alt : altsettings) {
            for (const auto& fmt : alt.formats) {
                if (!passesHardConstraints(alt, fmt, pref, isUac2)) {
                    continue;
                }
                float s = scoreFormat(alt, fmt, pref);
                results.push_back({&alt, &fmt, s});
            }
        }

        // Sort descending by score; tie-break by lowest alternateSetting
        std::sort(results.begin(), results.end(),
            [](const ScoredMatch& a, const ScoredMatch& b) {
                if (std::abs(a.score - b.score) > 1e-6f) {
                    return a.score > b.score;
                }
                return a.altsetting->alternateSetting < b.altsetting->alternateSetting;
            });

        return results;
    }

private:
    static std::optional<ScoredMatch> pickBest(
        const std::vector<UsbStreamingInterface>& altsettings,
        const StreamPreference& pref,
        bool isUac2)
    {
        auto scored = scoreAll(altsettings, pref, isUac2);
        if (scored.empty()) {
            return std::nullopt;
        }
        return scored[0];
    }

    /**
     * Hard constraint filter. Returns false if this (alt, format) pair
     * must not be considered.
     */
    static bool passesHardConstraints(
        const UsbStreamingInterface& alt,
        const UsbAudioFormat& fmt,
        const StreamPreference& pref,
        bool isUac2)
    {
        // Must have valid channels and bit depth
        if (fmt.channels == 0 || fmt.bitResolution == 0) {
            return false;
        }

        // Channel count constraint
        if (fmt.channels < pref.minChannels) {
            return false;
        }

        // Feedback constraint
        if (pref.requireFeedback && !alt.feedbackEndpoint.has_value()) {
            return false;
        }

        // Sample rate constraint
        if (pref.requiredSampleRate > 0 && !pref.skipRateCheck && !isUac2) {
            // UAC1: check if format lists the requested rate
            if (!fmt.sampleRates.empty() &&
                !fmt.supportsSampleRate(pref.requiredSampleRate)) {
                return false;
            }
        }

        return true;
    }

    /**
     * Score a single format within an altsetting.
     *
     * Each dimension is normalized to [0..1] and multiplied by its weight.
     * The total score is the sum of weighted dimensions.
     */
    static float scoreFormat(
        const UsbStreamingInterface& alt,
        const UsbAudioFormat& fmt,
        const StreamPreference& pref)
    {
        float score = 0.0f;

        // Bit depth: normalize 16→0.0, 32→1.0
        score += pref.bitDepthWeight * normalize(
            static_cast<float>(fmt.bitResolution), 16.0f, 32.0f);

        // Channel count: normalize 1→0.0, 8→1.0
        score += pref.channelCountWeight * normalize(
            static_cast<float>(fmt.channels), 1.0f, 8.0f);

        // Sync type: async=1.0, adaptive=0.5, sync=0.25, other=0.0
        score += pref.syncTypeWeight * syncScore(alt.dataEndpoint);

        // Feedback endpoint presence
        if (alt.feedbackEndpoint.has_value()) {
            score += pref.feedbackPresentWeight;
        }

        return score;
    }

    /** Normalize a value from [lo, hi] to [0, 1], clamped. */
    static float normalize(float value, float lo, float hi) {
        if (hi <= lo) return 0.0f;
        return std::clamp((value - lo) / (hi - lo), 0.0f, 1.0f);
    }

    /** Score sync type from endpoint attributes: async > adaptive > sync. */
    static float syncScore(const UsbEndpointInfo& ep) {
        if (ep.isAsync()) return 1.0f;
        if (ep.isAdaptive()) return 0.5f;
        if (ep.isSynchronous()) return 0.25f;
        return 0.0f;
    }
};

}  // namespace usb
}  // namespace watermelon_audio
