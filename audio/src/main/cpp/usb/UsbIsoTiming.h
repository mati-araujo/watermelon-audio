#pragma once

#include <algorithm>

namespace watermelon_audio::usb {

struct IsoTransferTiming {
    int endpointInterval = 1;
    int packetsPerSecond = 1000;
    int framesPerPacket = 48;
    int packetsPerTransfer = 8;
};

inline int normalizeIsoEndpointInterval(int endpointInterval) {
    return std::max(1, endpointInterval);
}

inline int isoServiceSlots(bool highSpeed, int endpointInterval) {
    const int normalized = normalizeIsoEndpointInterval(endpointInterval);
    if (!highSpeed) {
        return normalized;
    }
    return 1 << std::clamp(normalized - 1, 0, 7);
}

inline int isoPacketsPerSecond(bool highSpeed, int endpointInterval) {
    const int slots = isoServiceSlots(highSpeed, endpointInterval);
    return highSpeed
        ? std::max(1, 8000 / slots)
        : std::max(1, 1000 / slots);
}

inline IsoTransferTiming calculateIsoTransferTiming(
    int sampleRate,
    bool highSpeed,
    int endpointInterval,
    int targetTransferMs = 8) {
    IsoTransferTiming timing;
    timing.endpointInterval = normalizeIsoEndpointInterval(endpointInterval);
    timing.packetsPerSecond = isoPacketsPerSecond(highSpeed, timing.endpointInterval);
    timing.framesPerPacket = std::max(1, sampleRate / timing.packetsPerSecond);
    timing.packetsPerTransfer = std::max(
        1,
        (timing.packetsPerSecond * std::max(1, targetTransferMs)) / 1000);
    return timing;
}

}  // namespace watermelon_audio::usb
