// Fase 0.4 — Rate coercion retry decision (hallazgo C5)
//
// The full start() flow (claim interfaces, negotiate rate, reconfigure rings)
// is validated on hardware. Here we lock down the pure decision that drives
// the retry: (startSucceeded, coercedRate, currentRate, attempt) → action.

#include <gtest/gtest.h>

#include "../RateCoercionPolicy.h"

using watermelon_audio::usb::RateCoercionAction;
using watermelon_audio::usb::decideRateCoercion;

TEST(RateCoercionPolicyTest, SuccessProceeds) {
    EXPECT_EQ(decideRateCoercion(true, 0, 48000, 0), RateCoercionAction::Proceed);
    // Even if a coerced rate lingers, a successful start proceeds.
    EXPECT_EQ(decideRateCoercion(true, 44100, 48000, 0), RateCoercionAction::Proceed);
}

TEST(RateCoercionPolicyTest, CoercedRateOnFirstAttemptRetries) {
    EXPECT_EQ(decideRateCoercion(false, 44100, 48000, 0),
              RateCoercionAction::RetryAtCoercedRate);
}

TEST(RateCoercionPolicyTest, NoRetryOnSecondAttempt) {
    // Device coerced again after we already retried → give up.
    EXPECT_EQ(decideRateCoercion(false, 32000, 44100, 1),
              RateCoercionAction::Fail);
}

TEST(RateCoercionPolicyTest, FailureWithoutCoercedRateFails) {
    EXPECT_EQ(decideRateCoercion(false, 0, 48000, 0), RateCoercionAction::Fail);
}

TEST(RateCoercionPolicyTest, CoercedRateEqualToCurrentDoesNotLoop) {
    // A reported "coercion" that matches what we tried is not actionable.
    EXPECT_EQ(decideRateCoercion(false, 48000, 48000, 0), RateCoercionAction::Fail);
}
