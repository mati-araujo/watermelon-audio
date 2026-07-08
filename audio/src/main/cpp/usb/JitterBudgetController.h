#pragma once

/**
 * JitterBudgetController.h
 *
 * Convergent control loop for the output pacer's jitter budget (Fase 2 / 2.1,
 * hallazgo H4). The budget is the wall-clock absorber the output ring keeps
 * above one transfer (see UsbTransferManager::getOutputRingTargetLevel). Until
 * now it only ever ratcheted UP on underruns and never came back down, so a
 * single transient (thermal, a background app) left the whole session paying
 * extra latency forever.
 *
 * This controller adds the DOWN direction with asymmetric hysteresis: raise fast
 * (handled immediately by the event thread's underrun ratchet — NOT here), lower
 * slowly and only after a long clean stretch, and remember a per-session floor so
 * a value that proved too aggressive is never revisited. It is a pure state
 * machine driven one call per evaluation window (~2 s); the caller owns the clock
 * and the xrun signal, so this is fully host-testable with no timing.
 *
 * SAFE stays bit-identical to the legacy up-only behaviour via the explicit
 * `downConvergeEnabled = false` flag: the controller then NEVER emits LOWER, so
 * the live budget only ever moves via the event thread's underrun ratchet — even
 * after a ratchet has raised it above the configured min. (The old comment claimed
 * "min == initial so currentBudget is never > floor"; that was false — the ratchet
 * routinely pushes currentBudget above the floor, and without this flag the loop
 * would walk it back down, which the legacy SAFE never did.)
 */

#include <algorithm>
#include <atomic>

namespace watermelon_audio {
namespace usb {

class JitterBudgetController {
public:
    enum class Action {
        HOLD,   // no change this window
        LOWER,  // caller should decrement the live budget by 1 ms (toward floor)
    };

    struct Config {
        int minBudgetMs = 1;          // absolute floor (profile-derived by caller)
        int stableWindowsToLower = 30; // clean windows required before a lower (~60 s @ 2 s)
        int cooldownWindows = 15;      // windows to wait after any change (~30 s)
        // When false the controller never emits LOWER (frozen / up-only profile,
        // e.g. SAFE). Derived by the caller from the profile in configure().
        bool downConvergeEnabled = true;
    };

    JitterBudgetController() = default;
    explicit JitterBudgetController(const Config& cfg) : mConfig(cfg) {}

    void configure(const Config& cfg) {
        mConfig = cfg;
        reset(cfg.minBudgetMs);
    }

    /** Reset for a new session; @p initialBudget is the profile's starting budget. */
    void reset(int initialBudget) {
        setFloor(mConfig.minBudgetMs);
        mStableCount = 0;
        mCooldown = 0;
        mWindowsSinceLower = kNeverLowered;
        (void)initialBudget;  // initial budget is owned by the caller's atomic
    }

    /**
     * Advance one evaluation window.
     * @param hadXrun       true if any underrun/overrun/input-fail happened this window
     * @param currentBudget the live budget right now (may have been raised by the
     *                      event-thread ratchet since the last call)
     * @return HOLD, or LOWER when a long clean stretch allows stepping down 1 ms.
     */
    Action onWindow(bool hadXrun, int currentBudget) {
        // Frozen / up-only profile (SAFE): never converge down. The event thread's
        // underrun ratchet is the only thing that moves the budget — bit-identical
        // to the legacy behaviour. No bookkeeping needed since we never lower.
        if (!mConfig.downConvergeEnabled) {
            return Action::HOLD;
        }

        if (hadXrun) {
            // An xrun shortly after we lowered means the lower was too aggressive:
            // pin the floor at the (already-ratcheted-up) current budget so this
            // session never dips below it again.
            if (mWindowsSinceLower <= mConfig.cooldownWindows) {
                setFloor(std::max(mFloor, currentBudget));
            }
            mStableCount = 0;
            mCooldown = mConfig.cooldownWindows;
            mWindowsSinceLower = kNeverLowered;
            return Action::HOLD;
        }

        if (mWindowsSinceLower < kNeverLowered) {
            ++mWindowsSinceLower;
        }

        // During cooldown, don't accumulate toward the next lower.
        if (mCooldown > 0) {
            --mCooldown;
            mStableCount = 0;
            return Action::HOLD;
        }

        ++mStableCount;
        if (mStableCount >= mConfig.stableWindowsToLower && currentBudget > mFloor) {
            mStableCount = 0;
            mCooldown = mConfig.cooldownWindows;
            mWindowsSinceLower = 0;
            return Action::LOWER;
        }
        return Action::HOLD;
    }

    /**
     * Per-session floor discovered so far (>= configured min). Telemetry / 2.3.
     * mFloor itself is DSP-thread-owned (written only from onWindow/reset); this
     * getter reads an atomic mirror so a JNI/UI thread polling the floor doesn't
     * race the DSP writer (F2).
     */
    int sessionFloorMs() const {
        return mFloorPublished.load(std::memory_order_relaxed);
    }

private:
    static constexpr int kNeverLowered = 1 << 20;

    // Single point of truth for floor writes: keeps the atomic mirror in sync.
    void setFloor(int value) {
        mFloor = value;
        mFloorPublished.store(value, std::memory_order_relaxed);
    }

    Config mConfig{};
    int mFloor = 1;
    std::atomic<int> mFloorPublished{1};  // cross-thread mirror of mFloor (F2)
    int mStableCount = 0;
    int mCooldown = 0;
    int mWindowsSinceLower = kNeverLowered;
};

}  // namespace usb
}  // namespace watermelon_audio
