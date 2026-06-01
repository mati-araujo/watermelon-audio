#pragma once

#include <cstdint>

namespace watermelon_audio {
namespace usb {

struct RecoveryPolicyConfig {
    int maxConsecutiveErrorsBeforeRestart = 5;
    int maxRestartsPerMinute = 3;
    int quietPeriodMsBetweenRestarts = 500;
};

class RecoveryPolicy {
public:
    enum class Action {
        RESUBMIT,
        REQUEST_RESTART,
        DECLARE_DISCONNECTED
    };

    explicit RecoveryPolicy(RecoveryPolicyConfig config = {})
        : mConfig(config) {}

    void reset(uint64_t nowMs = 0) {
        mConsecutiveErrors = 0;
        mRestartWindowStartMs = nowMs;
        mRestartsInWindow = 0;
        mLastRestartMs = 0;
    }

    void onSuccess() {
        mConsecutiveErrors = 0;
    }

    Action onTransientError(uint64_t nowMs) {
        ++mConsecutiveErrors;
        if (mConsecutiveErrors < mConfig.maxConsecutiveErrorsBeforeRestart) {
            return Action::RESUBMIT;
        }
        return actionForRestartAvailability(nowMs);
    }

    Action onRestartRequested(uint64_t nowMs) {
        return actionForRestartAvailability(nowMs);
    }

    void onRestartStarted(uint64_t nowMs) {
        if (mRestartWindowStartMs == 0 || nowMs - mRestartWindowStartMs >= 60000) {
            mRestartWindowStartMs = nowMs;
            mRestartsInWindow = 0;
        }
        ++mRestartsInWindow;
        mLastRestartMs = nowMs;
        mConsecutiveErrors = 0;
    }

    int consecutiveErrors() const { return mConsecutiveErrors; }
    int restartsInWindow() const { return mRestartsInWindow; }

private:
    enum class RestartAvailability {
        ALLOWED,
        QUIET_PERIOD,
        RATE_LIMITED
    };

    Action actionForRestartAvailability(uint64_t nowMs) {
        switch (restartAvailability(nowMs)) {
            case RestartAvailability::ALLOWED:
                return Action::REQUEST_RESTART;
            case RestartAvailability::QUIET_PERIOD:
                return Action::RESUBMIT;
            case RestartAvailability::RATE_LIMITED:
                return Action::DECLARE_DISCONNECTED;
        }
        return Action::DECLARE_DISCONNECTED;
    }

    RestartAvailability restartAvailability(uint64_t nowMs) {
        if (mRestartWindowStartMs == 0 || nowMs - mRestartWindowStartMs >= 60000) {
            mRestartWindowStartMs = nowMs;
            mRestartsInWindow = 0;
        }
        if (mRestartsInWindow >= mConfig.maxRestartsPerMinute) {
            return RestartAvailability::RATE_LIMITED;
        }
        if (mLastRestartMs != 0 &&
            nowMs - mLastRestartMs < static_cast<uint64_t>(mConfig.quietPeriodMsBetweenRestarts)) {
            return RestartAvailability::QUIET_PERIOD;
        }
        return RestartAvailability::ALLOWED;
    }

    RecoveryPolicyConfig mConfig;
    int mConsecutiveErrors = 0;
    uint64_t mRestartWindowStartMs = 0;
    int mRestartsInWindow = 0;
    uint64_t mLastRestartMs = 0;
};

} // namespace usb
} // namespace watermelon_audio
