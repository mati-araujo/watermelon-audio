/**
 * ImplicitFeedbackEstimator.h
 *
 * Fase 0.2 — Implicit feedback (C2).
 *
 * Full-duplex asynchronous interfaces without an explicit feedback endpoint
 * still carry the device's clock: the CAPTURE stream is synchronous to the
 * device clock, so the device delivers exactly the frames its clock produced
 * per service interval. By measuring the real input rate over a window of
 * service intervals (packets) we recover the same information an explicit
 * feedback endpoint would have given us, and feed it to the ClockController
 * as the output target.
 *
 * This estimator is intentionally pure (no libusb / Android) so it can be
 * unit-tested in isolation. It is only ever touched from the USB event
 * thread, so it carries no atomics of its own.
 */

#pragma once

#include <cstdint>
#include <optional>

namespace watermelon_audio::usb {

class ImplicitFeedbackEstimator {
public:
    // Window length in service intervals (packets). ~256 ms at full speed
    // (1 ms intervals); ~32 ms at high speed bInterval=1. Long enough that
    // the 1-frame quantization (~81 ppm/window FS) is averaged away by the
    // ClockController's EMA within a handful of windows.
    static constexpr uint64_t kWindowPackets = 256;

    /**
     * Accumulate one input transfer's worth of activity.
     *
     * @param newFrames   Input frames seen in this transfer (zero-length
     *                    packets contribute 0 frames but DO count as elapsed
     *                    intervals — that is exactly the rate information).
     * @param newPackets  Service intervals elapsed (i.e. ctx->packetCount).
     * @return            frames-per-input-packet once a full window closes,
     *                    nullopt otherwise. The window then resets.
     */
    std::optional<double> onPackets(uint64_t newFrames, uint64_t newPackets) {
        mFrames += newFrames;
        mPackets += newPackets;

        if (mPackets < kWindowPackets) {
            return std::nullopt;
        }

        const double framesPerPacket =
            static_cast<double>(mFrames) / static_cast<double>(mPackets);
        mFrames = 0;
        mPackets = 0;
        return framesPerPacket;
    }

    void reset() {
        mFrames = 0;
        mPackets = 0;
    }

private:
    uint64_t mFrames = 0;    // input frames accumulated this window
    uint64_t mPackets = 0;   // input packets (service intervals) this window
};

}  // namespace watermelon_audio::usb
