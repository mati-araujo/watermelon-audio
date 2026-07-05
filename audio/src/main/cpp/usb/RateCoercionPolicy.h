/**
 * RateCoercionPolicy.h
 *
 * Fase 0.4 — Sample-rate coercion handling (hallazgo C5).
 *
 * When a device's GET_CUR reveals it coerced the requested sample rate to a
 * value it actually supports, the TransferConfig (framesPerPacket, rings,
 * ClockController) was already computed with the old rate → systematic drift.
 * The clock hook aborts the transfer-manager start in that case; this pure
 * helper decides what start() does next, so the decision is unit-testable
 * without libusb.
 */

#pragma once

namespace watermelon_audio::usb {

enum class RateCoercionAction {
    Proceed,             // start succeeded — nothing to do
    RetryAtCoercedRate,  // restart the transfer manager at the coerced rate
    Fail                 // give up (already retried, or no usable coerced rate)
};

/**
 * @param startSucceeded  Did transferManager->start() succeed?
 * @param coercedRate     Rate the device coerced to (0 if none reported).
 * @param currentRate     The rate we just tried to start with.
 * @param attempt         0-based attempt counter.
 *
 * At most one retry: only attempt 0 may retry, and only when the device
 * reported a usable coerced rate that differs from what we just tried.
 */
inline RateCoercionAction decideRateCoercion(bool startSucceeded,
                                             int coercedRate,
                                             int currentRate,
                                             int attempt) {
    if (startSucceeded) {
        return RateCoercionAction::Proceed;
    }
    if (attempt == 0 && coercedRate > 0 && coercedRate != currentRate) {
        return RateCoercionAction::RetryAtCoercedRate;
    }
    return RateCoercionAction::Fail;
}

}  // namespace watermelon_audio::usb
