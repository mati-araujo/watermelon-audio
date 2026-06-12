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

}  // namespace watermelon_audio::usb
