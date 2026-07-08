// Etapa 3 — Host tests for the convergent jitter-budget controller (Fase 2/H4).
//
// The controller only ever decides the DOWN direction + the per-session floor;
// the UP ratchet lives in the event thread and is simulated here by raising the
// tracked budget before an xrun window.

#include <gtest/gtest.h>

#include "../JitterBudgetController.h"

using watermelon_audio::usb::JitterBudgetController;
using Action = watermelon_audio::usb::JitterBudgetController::Action;

namespace {

JitterBudgetController::Config smallCfg(int minMs = 1) {
    JitterBudgetController::Config c;
    c.minBudgetMs = minMs;
    c.stableWindowsToLower = 3;
    c.cooldownWindows = 2;
    return c;
}

// Feed a clean window and apply a LOWER to the tracked budget.
int stableWindow(JitterBudgetController& c, int budget) {
    return c.onWindow(/*hadXrun=*/false, budget) == Action::LOWER ? budget - 1 : budget;
}

TEST(JitterBudgetController, LowersAfterStableStretch) {
    JitterBudgetController c(smallCfg());
    c.reset(8);
    int budget = 8;

    // 3 clean windows → first lower.
    EXPECT_EQ(Action::HOLD, c.onWindow(false, budget));
    EXPECT_EQ(Action::HOLD, c.onWindow(false, budget));
    EXPECT_EQ(Action::LOWER, c.onWindow(false, budget));
    budget = 7;

    // Cooldown (2 windows) blocks an immediate re-lower even while clean.
    EXPECT_EQ(Action::HOLD, c.onWindow(false, budget));
    EXPECT_EQ(Action::HOLD, c.onWindow(false, budget));

    // Then 3 more clean windows → second lower.
    EXPECT_EQ(Action::HOLD, c.onWindow(false, budget));
    EXPECT_EQ(Action::HOLD, c.onWindow(false, budget));
    EXPECT_EQ(Action::LOWER, c.onWindow(false, budget));
    budget = 6;
    EXPECT_EQ(6, budget);
}

TEST(JitterBudgetController, ConvergesDownToFloor) {
    JitterBudgetController c(smallCfg(/*minMs=*/5));
    c.reset(8);
    int budget = 8;
    // Run many windows; budget should walk down and stop at the floor (5).
    for (int i = 0; i < 200; ++i) {
        budget = stableWindow(c, budget);
    }
    EXPECT_EQ(5, budget);
    EXPECT_EQ(5, c.sessionFloorMs());
}

TEST(JitterBudgetController, NeverLowersWhenAlreadyAtFloor) {
    JitterBudgetController c(smallCfg(/*minMs=*/4));
    c.reset(4);
    int budget = 4;
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(Action::HOLD, c.onWindow(false, budget));
    }
    EXPECT_EQ(4, budget);
}

TEST(JitterBudgetController, SafeProfileIsFrozen) {
    // SAFE: caller sets min == initial (24) → can never lower.
    JitterBudgetController c(smallCfg(/*minMs=*/24));
    c.reset(24);
    int budget = 24;
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(Action::HOLD, c.onWindow(false, budget));
    }
    EXPECT_EQ(24, budget);
}

TEST(JitterBudgetController, XrunResetsProgressTowardLower) {
    JitterBudgetController c(smallCfg());
    c.reset(8);
    int budget = 8;

    EXPECT_EQ(Action::HOLD, c.onWindow(false, budget));  // stable 1
    EXPECT_EQ(Action::HOLD, c.onWindow(false, budget));  // stable 2
    // xrun wipes the streak and starts a cooldown.
    EXPECT_EQ(Action::HOLD, c.onWindow(true, budget));

    // Need cooldown(2) + stable(3) clean windows before the next lower.
    EXPECT_EQ(Action::HOLD, c.onWindow(false, budget));  // cooldown 1
    EXPECT_EQ(Action::HOLD, c.onWindow(false, budget));  // cooldown 2
    EXPECT_EQ(Action::HOLD, c.onWindow(false, budget));  // stable 1
    EXPECT_EQ(Action::HOLD, c.onWindow(false, budget));  // stable 2
    EXPECT_EQ(Action::LOWER, c.onWindow(false, budget)); // stable 3 → lower
}

TEST(JitterBudgetController, PinsFloorWhenXrunFollowsALower) {
    JitterBudgetController c(smallCfg());
    c.reset(8);
    int budget = 8;

    // Walk down to a lower (8 → 7).
    EXPECT_EQ(Action::HOLD, c.onWindow(false, budget));
    EXPECT_EQ(Action::HOLD, c.onWindow(false, budget));
    EXPECT_EQ(Action::LOWER, c.onWindow(false, budget));
    budget = 7;

    // An xrun right after the lower: the event thread ratchets the budget back
    // up to 8; the controller sees that and pins the floor at 8.
    budget = 8;  // simulated event-thread up-ratchet
    EXPECT_EQ(Action::HOLD, c.onWindow(true, budget));
    EXPECT_EQ(8, c.sessionFloorMs());

    // From now on the budget can never dip below 8 again this session.
    for (int i = 0; i < 200; ++i) {
        budget = stableWindow(c, budget);
    }
    EXPECT_EQ(8, budget);
    EXPECT_EQ(8, c.sessionFloorMs());
}

TEST(JitterBudgetController, LateXrunDoesNotPinFloor) {
    // An xrun past the probe window is a fresh transient, not evidence the last
    // lower was too aggressive — so the floor stays at the configured min.
    JitterBudgetController c(smallCfg());  // min=1, stable=3, cooldown=2
    c.reset(8);
    int budget = 8;
    // First lower: 8 → 7 at the 3rd clean window.
    c.onWindow(false, budget);
    c.onWindow(false, budget);
    ASSERT_EQ(Action::LOWER, c.onWindow(false, budget));
    budget = 7;
    // Let the probe window (cooldownWindows = 2) fully elapse with clean windows.
    c.onWindow(false, budget);  // windowsSinceLower = 1
    c.onWindow(false, budget);  // windowsSinceLower = 2
    c.onWindow(false, budget);  // windowsSinceLower = 3 (past the probe window)
    // Now an xrun arrives; the floor must NOT be pinned to the high value.
    c.onWindow(true, budget);
    EXPECT_EQ(1, c.sessionFloorMs());
}

}  // namespace
