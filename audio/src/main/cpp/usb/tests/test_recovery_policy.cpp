#include "../RecoveryPolicy.h"

#include <gtest/gtest.h>

using namespace watermelon_audio::usb;

namespace {

TEST(RecoveryPolicyTest, ResubmitsUntilThresholdThenRequestsRestart) {
    RecoveryPolicyConfig config;
    config.maxConsecutiveErrorsBeforeRestart = 3;
    RecoveryPolicy policy(config);

    EXPECT_EQ(policy.onTransientError(1000), RecoveryPolicy::Action::RESUBMIT);
    EXPECT_EQ(policy.onTransientError(1001), RecoveryPolicy::Action::RESUBMIT);
    EXPECT_EQ(policy.onTransientError(1002), RecoveryPolicy::Action::REQUEST_RESTART);
}

TEST(RecoveryPolicyTest, SuccessResetsConsecutiveErrors) {
    RecoveryPolicyConfig config;
    config.maxConsecutiveErrorsBeforeRestart = 3;
    RecoveryPolicy policy(config);

    EXPECT_EQ(policy.onTransientError(1000), RecoveryPolicy::Action::RESUBMIT);
    EXPECT_EQ(policy.onTransientError(1001), RecoveryPolicy::Action::RESUBMIT);
    policy.onSuccess();
    EXPECT_EQ(policy.onTransientError(1002), RecoveryPolicy::Action::RESUBMIT);
}

TEST(RecoveryPolicyTest, RateLimitsRestartsPerMinute) {
    RecoveryPolicyConfig config;
    config.maxConsecutiveErrorsBeforeRestart = 1;
    config.maxRestartsPerMinute = 2;
    config.quietPeriodMsBetweenRestarts = 0;
    RecoveryPolicy policy(config);

    EXPECT_EQ(policy.onTransientError(1000), RecoveryPolicy::Action::REQUEST_RESTART);
    policy.onRestartStarted(1000);
    EXPECT_EQ(policy.onTransientError(2000), RecoveryPolicy::Action::REQUEST_RESTART);
    policy.onRestartStarted(2000);
    EXPECT_EQ(policy.onTransientError(3000), RecoveryPolicy::Action::DECLARE_DISCONNECTED);
}

TEST(RecoveryPolicyTest, QuietPeriodKeepsResubmittingBeforeNextRestart) {
    RecoveryPolicyConfig config;
    config.maxConsecutiveErrorsBeforeRestart = 1;
    config.maxRestartsPerMinute = 3;
    config.quietPeriodMsBetweenRestarts = 500;
    RecoveryPolicy policy(config);

    EXPECT_EQ(policy.onTransientError(1000), RecoveryPolicy::Action::REQUEST_RESTART);
    policy.onRestartStarted(1000);
    EXPECT_EQ(policy.onTransientError(1200), RecoveryPolicy::Action::RESUBMIT);
    EXPECT_EQ(policy.onRestartRequested(1600), RecoveryPolicy::Action::REQUEST_RESTART);
}

TEST(RecoveryPolicyTest, WindowResetsAfterOneMinute) {
    RecoveryPolicyConfig config;
    config.maxConsecutiveErrorsBeforeRestart = 1;
    config.maxRestartsPerMinute = 1;
    config.quietPeriodMsBetweenRestarts = 0;
    RecoveryPolicy policy(config);

    EXPECT_EQ(policy.onTransientError(1000), RecoveryPolicy::Action::REQUEST_RESTART);
    policy.onRestartStarted(1000);
    EXPECT_EQ(policy.onTransientError(2000), RecoveryPolicy::Action::DECLARE_DISCONNECTED);
    EXPECT_EQ(policy.onRestartRequested(62000), RecoveryPolicy::Action::REQUEST_RESTART);
}

} // namespace
