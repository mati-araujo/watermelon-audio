/**
 * UsbLatencyMath.h
 *
 * Fase 0.5 — Host-side software latency math (hallazgo L7).
 *
 * Pure helpers so the latency arithmetic is unit-testable without libusb.
 * These report the SOFTWARE latency on the host side (ring buffer fill plus
 * transfers in flight). The total analog round-trip (converters + URB
 * scheduling) is measured separately in Fase 5.
 */

#pragma once

#include <algorithm>
#include <cstddef>

namespace watermelon_audio::usb {

/**
 * Output (playback) latency in milliseconds.
 *
 * @param ringSamples        Samples currently queued in the output ring.
 * @param channelCount       Output channels (>=1).
 * @param pendingTransfers   Output transfers in flight.
 * @param framesPerTransfer  Frames carried by one transfer
 *                           (packetsPerTransfer * framesPerPacket).
 * @param sampleRate         Hz (>0).
 *
 * In-flight transfers are counted at half their frames on average: at any
 * instant a pending transfer is partway through being consumed by the device.
 */
inline float computeOutputLatencyMs(double ringSamples,
                                    int channelCount,
                                    int pendingTransfers,
                                    double framesPerTransfer,
                                    int sampleRate) {
    if (sampleRate <= 0 || channelCount <= 0) {
        return 0.0f;
    }
    const double ringFrames = ringSamples / double(channelCount);
    const double inflight =
        double(std::max(0, pendingTransfers)) * framesPerTransfer * 0.5;
    return float((ringFrames + inflight) * 1000.0 / double(sampleRate));
}

/**
 * Input (capture) latency in milliseconds. The capture path holds the ring
 * fill plus, on average, half a transfer being assembled.
 */
inline float computeInputLatencyMs(double ringSamples,
                                   int channelCount,
                                   double framesPerTransfer,
                                   int sampleRate) {
    if (sampleRate <= 0 || channelCount <= 0) {
        return 0.0f;
    }
    const double ringFrames = ringSamples / double(channelCount);
    return float((ringFrames + framesPerTransfer * 0.5) * 1000.0 / double(sampleRate));
}


/**
 * REQ-050 S3 (AC-050.9, D16) — the output latency CEILING the backend declares, in ms.
 *
 * Same definition as computeOutputLatencyMs (ring fill + half of what is in flight), at the
 * worst point the backend's own pacer allows:
 *   - the ring just below its target with the jitter budget at its MAXIMUM (the event
 *     thread ratchets it up on its own after underruns, up to initial + cap), plus one DSP
 *     block produced on top (the pacer only checks "below target" before producing);
 *   - the whole output queue in flight (numTransfers).
 *
 * The USB test runner compares the measured latency against this instead of a fixed
 * number: a fixed 20 ms failed by construction against a SAFE backend that sits at ~37 ms
 * with perfect traffic (MINI-039).
 *
 * @param framesPerTransfer  packetsPerTransfer * framesPerPacket.
 * @param jitterBudgetMaxMs  upper clamp of the live jitter budget.
 * @param dspBlockFrames     frames the DSP produces per iteration.
 * @param numTransfers       output transfers kept in flight.
 * @param sampleRate         Hz. 0 or less → 0 (no ceiling can be declared).
 */
inline float declaredOutputLatencyCeilingMs(int framesPerTransfer,
                                            int jitterBudgetMaxMs,
                                            int dspBlockFrames,
                                            int numTransfers,
                                            int sampleRate) {
    if (sampleRate <= 0) {
        return 0.0f;
    }
    const double transfer = double(std::max(0, framesPerTransfer));
    // Integer frames, exactly like outputRingTargetSamples computes the target.
    const double jitter = double(std::max(0, jitterBudgetMaxMs) * sampleRate / 1000);
    const double block = double(std::max(0, dspBlockFrames));
    const double inflight = double(std::max(0, numTransfers)) * transfer * 0.5;
    return float((transfer + jitter + block + inflight) * 1000.0 / double(sampleRate));
}

/**
 * REQ-050 S3 (AC-050.9, D16) — output packets the backend keeps in flight: the whole queue
 * (numTransfers × packetsPerTransfer).
 *
 * It is the DECLARED depth, not a live count, on purpose: the runner subtracts it from
 * `packetsSubmitted` (which only counts output packets) to get the packets whose fate is
 * already known. A live pending count read a few instructions after `packetsSubmitted`
 * can be transiently one transfer short (a completion reaped but not yet resubmitted),
 * and one high-speed transfer is 64 packets — 0.16 % of a 5 s row, more than the 0.1 %
 * the runner tolerates. The queue never holds more than this, so the denominator can only
 * err towards "fewer packets resolved", never towards inventing a loss.
 */
inline std::size_t outputPacketsInFlightDepth(int numTransfers, int packetsPerTransfer) {
    if (numTransfers <= 0 || packetsPerTransfer <= 0) {
        return 0;
    }
    return static_cast<std::size_t>(numTransfers) * static_cast<std::size_t>(packetsPerTransfer);
}

}  // namespace watermelon_audio::usb
