/**
 * test_c_api_logcapture.cpp
 *
 * The log-capture surface (section 23), the only part of the WA-2.5/2.6 tail
 * that needed new C API rather than a mechanical migration.
 *
 * The ring itself is already covered by LogCaptureBuffer's own tests; this file
 * deliberately does NOT re-affirm the ring semantics. What is new — and what
 * only exists at this boundary — is the batch:
 *
 *   Draining is destructive. The JNI could allocate a jobjectArray of exactly
 *   the right size because it had the std::vector in hand. A C caller has no
 *   such luxury, and the obvious C shape (caller buffer + capacity) turns a
 *   too-small buffer into silently discarded lines that are already gone from
 *   the ring. WmaLogBatch exists so the lines are handed over whole and the
 *   caller decides afterwards what to do with them.
 *
 * These tests use LogCaptureBuffer directly for setup. That is on purpose: the
 * buffer is a process-wide singleton shared with every other test in the binary,
 * so each test has to leave it exactly as it found it, and the C API deliberately
 * does not expose clear().
 */

#include "api/watermelon_audio.h"
#include "platform/LogCaptureBuffer.h"
#include "platform/Logger.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace wma_test {
namespace {

class CApiLogCaptureTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Capture runs from logMessage() regardless of the log callback, so a
        // no-op sink keeps ctest readable without suppressing what we capture.
        wma::setLogCallback([](wma::LogLevel, const char*, const char*) {});
        wma::LogCaptureBuffer::instance().clear();
    }

    void TearDown() override {
        // Leave the singleton off and empty: anything else leaks into whichever
        // test happens to run next in this binary.
        wma_log_capture_set_enabled(false);
        wma::LogCaptureBuffer::instance().clear();
        wma::setLogCallback(nullptr);
    }

    /// Drain through the C API into something a test can assert on.
    static std::vector<std::string> drainToVector() {
        WmaLogBatch* batch = wma_log_capture_drain();
        std::vector<std::string> out;
        const int count = wma_log_batch_count(batch);
        out.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            const char* line = wma_log_batch_line(batch, i);
            out.emplace_back(line ? line : "<null>");
        }
        wma_log_batch_free(batch);
        return out;
    }
};

// ===========================================================================
// Enable / drain round trip
// ===========================================================================

TEST_F(CApiLogCaptureTest, NothingIsCapturedUntilCaptureIsEnabled) {
    WMA_LOGI("this line happens before capture is on");

    std::vector<std::string> before = drainToVector();
    EXPECT_TRUE(before.empty());

    wma_log_capture_set_enabled(true);
    WMA_LOGI("this line happens after");

    std::vector<std::string> after = drainToVector();
    ASSERT_EQ(after.size(), 1u);
    EXPECT_NE(after[0].find("this line happens after"), std::string::npos);
}

TEST_F(CApiLogCaptureTest, CapturedLinesKeepTheirOrderLevelAndTag) {
    wma_log_capture_set_enabled(true);

    WMA_LOGI("first");
    WMA_LOGW("second");
    WMA_LOGE("third");

    std::vector<std::string> lines = drainToVector();
    ASSERT_EQ(lines.size(), 3u);

    // Format is "L/TAG: message" — the level char is what a log viewer colours
    // by, so it is part of the contract, not incidental formatting.
    EXPECT_EQ(lines[0][0], 'I');
    EXPECT_EQ(lines[1][0], 'W');
    EXPECT_EQ(lines[2][0], 'E');

    EXPECT_NE(lines[0].find("first"), std::string::npos);
    EXPECT_NE(lines[1].find("second"), std::string::npos);
    EXPECT_NE(lines[2].find("third"), std::string::npos);
}

TEST_F(CApiLogCaptureTest, ADrainEmptiesTheRing) {
    wma_log_capture_set_enabled(true);
    WMA_LOGI("only line");

    ASSERT_EQ(drainToVector().size(), 1u);
    EXPECT_TRUE(drainToVector().empty()) << "the second drain must not repeat the line";
}

TEST_F(CApiLogCaptureTest, DisablingStopsCaptureButKeepsWhatWasAlreadyBuffered) {
    wma_log_capture_set_enabled(true);
    WMA_LOGI("captured");

    wma_log_capture_set_enabled(false);
    WMA_LOGI("not captured");

    std::vector<std::string> lines = drainToVector();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("captured"), std::string::npos);
}

// ===========================================================================
// The batch — the part that is new surface rather than migrated behaviour
// ===========================================================================

TEST_F(CApiLogCaptureTest, AnEmptyDrainIsAnEmptyBatchNotANullOne) {
    // The JNI's contract was "an array, possibly of length zero" and NULL meant
    // failure. The batch keeps that distinction: no lines is still a batch.
    WmaLogBatch* batch = wma_log_capture_drain();
    ASSERT_NE(batch, nullptr);
    EXPECT_EQ(wma_log_batch_count(batch), 0);
    wma_log_batch_free(batch);
}

TEST_F(CApiLogCaptureTest, AnOutOfRangeIndexReturnsNullRatherThanReadingPastTheEnd) {
    wma_log_capture_set_enabled(true);
    WMA_LOGI("only line");

    WmaLogBatch* batch = wma_log_capture_drain();
    ASSERT_NE(batch, nullptr);
    ASSERT_EQ(wma_log_batch_count(batch), 1);

    EXPECT_NE(wma_log_batch_line(batch, 0), nullptr);
    EXPECT_EQ(wma_log_batch_line(batch, 1), nullptr);
    EXPECT_EQ(wma_log_batch_line(batch, -1), nullptr);
    EXPECT_EQ(wma_log_batch_line(batch, 1 << 20), nullptr);

    wma_log_batch_free(batch);
}

TEST_F(CApiLogCaptureTest, TheBatchAccessorsTreatNullAsAnEmptyBatch) {
    // cinterop will happily hand a null back from a failed drain; a Kotlin
    // caller that forgets to check must not take the process down.
    EXPECT_EQ(wma_log_batch_count(nullptr), 0);
    EXPECT_EQ(wma_log_batch_line(nullptr, 0), nullptr);
    wma_log_batch_free(nullptr);
    SUCCEED();
}

TEST_F(CApiLogCaptureTest, LinePointersStayValidForTheLifetimeOfTheBatch) {
    wma_log_capture_set_enabled(true);
    WMA_LOGI("alpha");
    WMA_LOGI("beta");

    WmaLogBatch* batch = wma_log_capture_drain();
    ASSERT_EQ(wma_log_batch_count(batch), 2);

    // Take the pointers first, then churn the ring underneath. The batch owns
    // its strings; if it aliased the ring instead, these would dangle — which is
    // the whole reason the batch owns rather than borrows.
    const char* first = wma_log_batch_line(batch, 0);
    const char* second = wma_log_batch_line(batch, 1);

    WMA_LOGI("gamma");
    wma::LogCaptureBuffer::instance().clear();

    EXPECT_NE(std::string(first).find("alpha"), std::string::npos);
    EXPECT_NE(std::string(second).find("beta"), std::string::npos);

    wma_log_batch_free(batch);
}

// ===========================================================================
// Dropped counter
// ===========================================================================

TEST_F(CApiLogCaptureTest, TheDroppedCounterPassesThroughAndSurvivesADrain) {
    wma_log_capture_set_enabled(true);

    // The ring's drop behaviour is LogCaptureBuffer's own test; overflowing it
    // here would just be a slower copy of that. Setting the buffer up directly
    // keeps this about the passthrough and about drain() not resetting it.
    for (size_t i = 0; i < wma::LogCaptureBuffer::kCapacity + 3; ++i) {
        WMA_LOGI("filler %zu", i);
    }

    const int dropped = wma_log_capture_dropped();
    EXPECT_EQ(dropped, 3);

    ASSERT_EQ(drainToVector().size(), wma::LogCaptureBuffer::kCapacity);
    EXPECT_EQ(wma_log_capture_dropped(), dropped)
        << "draining the lines must not silently reset the drop count — "
           "the count is how a caller learns the log has a hole in it";
}

/*
 * ===========================================================================
 * DELIBERATELY NOT EXPOSED — wma_log_capture_clear()
 * ===========================================================================
 *
 * LogCaptureBuffer::clear() is the only way to reset the dropped counter, and
 * these tests use it. It is not in the C API because the JNI never exposed it,
 * and the tail of WA-2.5/2.6 is a migration: adding surface that no caller has
 * asked for is how a header grows functions nobody can explain later.
 *
 * If an iOS harness ever needs "clear the log view", that is the moment to add
 * it — with a caller to justify the shape.
 */

}  // namespace
}  // namespace wma_test
