/**
 * ClockSourceRangeParser.h
 *
 * Pure-function helper for decoding UAC 2.0 Clock Source RANGE responses.
 *
 * The Audio Class 2.0 RANGE response for CS_SAM_FREQ_CONTROL has the layout:
 *
 *     [0..1]   wNumSubRanges (u16 LE)
 *     per sub-range (12 bytes each):
 *       [0..3]  dMIN         (u32 LE)
 *       [4..7]  dMAX         (u32 LE)
 *       [8..11] dRES         (u32 LE)
 *
 * Sub-ranges where dMIN == dMAX represent a discrete supported rate.
 * Sub-ranges where dMIN < dMAX represent a continuous rate range.
 *
 * Stage 3 — Clock sync (minimum viable).
 */

#pragma once

#include <cstdint>
#include <vector>

#include "UsbAudioTypes.h"

namespace watermelon_audio {
namespace usb {

struct ClockSampleRateRange {
    uint32_t minHz = 0;
    uint32_t maxHz = 0;
    uint32_t resolutionHz = 0;

    bool isDiscrete() const { return minHz == maxHz; }
};

/**
 * Parse a raw UAC2 RANGE response buffer into structured sub-ranges.
 * Returns empty vector if the buffer is malformed or too short.
 *
 * `buffer` must include the 2-byte wNumSubRanges header.
 */
inline std::vector<ClockSampleRateRange> parseClockRangeResponse(
    const uint8_t* buffer, size_t length)
{
    std::vector<ClockSampleRateRange> out;
    if (buffer == nullptr || length < 2) return out;

    const uint16_t numSub =
        static_cast<uint16_t>(buffer[0]) |
        (static_cast<uint16_t>(buffer[1]) << 8);

    const size_t expectedLen = 2 + static_cast<size_t>(numSub) * 12;
    if (length < expectedLen) return out;

    out.reserve(numSub);
    for (uint16_t i = 0; i < numSub; ++i) {
        const uint8_t* p = buffer + 2 + (i * 12);
        ClockSampleRateRange r;
        r.minHz =
            static_cast<uint32_t>(p[0]) |
            (static_cast<uint32_t>(p[1]) << 8) |
            (static_cast<uint32_t>(p[2]) << 16) |
            (static_cast<uint32_t>(p[3]) << 24);
        r.maxHz =
            static_cast<uint32_t>(p[4]) |
            (static_cast<uint32_t>(p[5]) << 8) |
            (static_cast<uint32_t>(p[6]) << 16) |
            (static_cast<uint32_t>(p[7]) << 24);
        r.resolutionHz =
            static_cast<uint32_t>(p[8]) |
            (static_cast<uint32_t>(p[9]) << 8) |
            (static_cast<uint32_t>(p[10]) << 16) |
            (static_cast<uint32_t>(p[11]) << 24);
        out.push_back(r);
    }
    return out;
}

/**
 * Apply a list of parsed sub-ranges to a UsbClockSource, populating
 * sampleRates / hasContinuousRates / minSampleRate / maxSampleRate
 * according to stage 3 semantics.
 *
 * - Discrete sub-ranges (min==max) contribute a single rate each.
 * - Continuous sub-ranges (min<max) set hasContinuousRates=true and
 *   enumerate a handful of common rates (44100/48000/88200/96000/...)
 *   that fall inside the range, so higher layers have something concrete
 *   to report. The min/max fields always reflect the outer bounds.
 */
inline void applyRangesToClockSource(
    const std::vector<ClockSampleRateRange>& ranges,
    UsbClockSource& cs)
{
    if (ranges.empty()) return;

    uint32_t overallMin = UINT32_MAX;
    uint32_t overallMax = 0;
    bool anyContinuous = false;

    // Common USB Audio sample rates we probe when a range is continuous.
    static const int commonRates[] = {
        8000, 11025, 16000, 22050, 32000,
        44100, 48000, 88200, 96000, 176400, 192000, 384000,
    };

    for (const auto& r : ranges) {
        if (r.minHz < overallMin) overallMin = r.minHz;
        if (r.maxHz > overallMax) overallMax = r.maxHz;

        if (r.isDiscrete()) {
            if (r.minHz > 0) {
                int rate = static_cast<int>(r.minHz);
                bool exists = false;
                for (int existing : cs.sampleRates) {
                    if (existing == rate) { exists = true; break; }
                }
                if (!exists) cs.sampleRates.push_back(rate);
            }
        } else {
            anyContinuous = true;
            // Enumerate common rates inside the continuous range so callers
            // have a concrete list to show. The device still advertises
            // min/max for any-rate consumers.
            for (int probe : commonRates) {
                if (static_cast<uint32_t>(probe) >= r.minHz &&
                    static_cast<uint32_t>(probe) <= r.maxHz) {
                    bool exists = false;
                    for (int existing : cs.sampleRates) {
                        if (existing == probe) { exists = true; break; }
                    }
                    if (!exists) cs.sampleRates.push_back(probe);
                }
            }
        }
    }

    cs.hasContinuousRates = anyContinuous;
    cs.minSampleRate = (overallMin == UINT32_MAX) ? 0 : static_cast<int>(overallMin);
    cs.maxSampleRate = static_cast<int>(overallMax);

    // Sort for deterministic consumption downstream.
    std::sort(cs.sampleRates.begin(), cs.sampleRates.end());
}

}  // namespace usb
}  // namespace watermelon_audio
