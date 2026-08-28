/**
 * test_c_api_diagnostics.cpp
 *
 * Latency diagnostics — section 21 of watermelon_audio.h.
 *
 * The `benchmark` category of WA-2.5/2.6 only partly moved, and that is the
 * point of this file as much as the assertions are. Four of its entry points
 * ask a question that only exists on Android — "did I get AAudio in exclusive
 * mode", "how low does an exclusive stream go on this device" — and open an
 * oboe::AudioStreamBuilder to answer it. Those stay in jni_benchmark.cpp, which
 * says so at the top. What moved is the portable half, and this is it.
 *
 * The two functions here replaced hand-rolled code in the JNI, and both
 * replacements changed behaviour on purpose. Each change has a test below that
 * fails against the old logic:
 *
 *   - wma_get_recommended_buffer_size() resolves the sample rate through
 *     AudioEngine::currentSampleRate() instead of "getStreamInfo() or else
 *     48000". The old shortcut ignored the preferred rate, so a device set to
 *     44.1 kHz that had not started a stream yet got a size computed for 48.
 *     That is the exact anti-pattern the comment above currentSampleRate()
 *     warns about, and the one that put SoundFonts on the wrong rate in WA-2.0.
 *
 *   - It also stopped truncating the frame requirement to an int before
 *     comparing. Truncating rounds the requirement DOWN, so a target needing
 *     128.6 frames used to be answered with 128 — a buffer shorter than the
 *     latency asked for, which is the one answer the function must not give.
 *
 * NOT covered here: everything that needs a real oboe::AudioStream. The tail of
 * getDetailedLatencyInfo() ([4..7]) and of the JNI's latency report are filled
 * from getOutputStream(), which returns nullptr under BackendManager — the fake
 * backend included. Those four floats and three lines are Android-device-only
 * and are not exercised by any host test; the split itself was verified by
 * reading both call sites.
 */

#include "support/CApiFixture.h"

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace wma_test {
namespace {

constexpr int kSampleRate = 48000;

using CApiDiagnosticsTest = CApiFixture;

/// The report as a std::string, sized from the C API's own length answer.
std::string reportOf(const WmaEngine* engine) {
    const int needed = wma_get_latency_report(engine, nullptr, 0);
    if (needed <= 0) return {};
    std::vector<char> buffer(static_cast<size_t>(needed) + 1, '\0');
    wma_get_latency_report(engine, buffer.data(), static_cast<int>(buffer.size()));
    return std::string(buffer.data());
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

// ===========================================================================
// Recommended buffer size — the arithmetic
// ===========================================================================

TEST_F(CApiDiagnosticsTest, ANonPositiveTargetIsRejected) {
    EXPECT_EQ(wma_get_recommended_buffer_size(mWma, 0.0f), -1);
    EXPECT_EQ(wma_get_recommended_buffer_size(mWma, -5.0f), -1);
    // NaN fails `> 0` too, which is why the guard is written that way round.
    EXPECT_EQ(wma_get_recommended_buffer_size(mWma, std::nanf("")), -1);
}

TEST_F(CApiDiagnosticsTest, TheAnswerIsAPowerOfTwoWithinTheDeviceRange) {
    startAt(kSampleRate, 0);

    for (float target : {0.5f, 1.0f, 2.0f, 5.0f, 11.0f, 50.0f, 500.0f}) {
        const int frames = wma_get_recommended_buffer_size(mWma, target);
        EXPECT_GE(frames, 64) << "target " << target;
        EXPECT_LE(frames, 2048) << "target " << target;
        EXPECT_EQ(frames & (frames - 1), 0) << target << " ms gave " << frames
                                            << ", which is not a power of two";
    }
}

TEST_F(CApiDiagnosticsTest, ATinyTargetGetsTheSmallestBufferRatherThanZero) {
    startAt(kSampleRate, 0);
    // 0.1 ms is 4.8 frames. The floor is 64, not "whatever rounds down".
    EXPECT_EQ(wma_get_recommended_buffer_size(mWma, 0.1f), 64);
}

TEST_F(CApiDiagnosticsTest, AHugeTargetIsCappedRatherThanRunningAway) {
    startAt(kSampleRate, 0);
    // 10 s would be 480000 frames; the loop stops at 2048.
    EXPECT_EQ(wma_get_recommended_buffer_size(mWma, 10000.0f), 2048);
}

TEST_F(CApiDiagnosticsTest, TheBufferIsNeverShorterThanTheLatencyAsked) {
    startAt(kSampleRate, 0);

    // 2.68 ms at 48 kHz needs 128.64 frames. The old code truncated that to 128
    // and answered 128 — a buffer that only covers 2.67 ms, i.e. LESS than the
    // caller asked for. Rounding the requirement down is the one direction a
    // "recommended size for a target latency" must not round.
    EXPECT_EQ(wma_get_recommended_buffer_size(mWma, 2.68f), 256);

    // And it still stops at the FIRST power of two that clears the bar rather
    // than over-allocating: 2.0 ms is 96 frames, which 128 covers.
    EXPECT_EQ(wma_get_recommended_buffer_size(mWma, 2.0f), 128);

    // The exact-boundary case is deliberately not pinned. A target that lands
    // on precisely 128.0 frames is not reachable through a float millisecond
    // value — 128.0f / 48000 * 1000 does not round-trip — so asserting it would
    // be testing float representation, not the function.
}

// ===========================================================================
// Recommended buffer size — which sample rate it resolves against
// ===========================================================================

TEST_F(CApiDiagnosticsTest, TheRunningStreamRateWins) {
    startAt(44100, /*fadeTimeMs=*/0);

    // 2.9 ms is 127.9 frames at 44.1 kHz but 139.2 at 48 kHz — the two rates
    // land on different powers of two, which is what makes this test able to
    // tell them apart at all.
    EXPECT_EQ(wma_get_recommended_buffer_size(mWma, 2.9f), 128);
}

TEST_F(CApiDiagnosticsTest, WithNoStreamRunningTheOfflineRenderRateIsUsed) {
    // THE behaviour change. No startAt() — nothing is running, so the old code
    // took its 48000 fallback and answered 256. currentSampleRate() knows the
    // engine is rendering at 44.1 and answers 128.
    //
    // MINI-007: el rung del medio se plantaba con `setPreferredSampleRate()`, que
    // ningun consumidor podia alcanzar. Ahora se planta por `startOffline()`, que
    // es su unico escritor de produccion — y de paso este test pasa a cubrir el
    // caso REAL del render offline pidiendo su tamaño de buffer.
    ASSERT_TRUE(mWma->engine->startOffline(44100, 4096));

    EXPECT_EQ(wma_get_recommended_buffer_size(mWma, 2.9f), 128);
}

TEST_F(CApiDiagnosticsTest, WithNothingConfiguredAtAllItFallsBackTo48000) {
    // The documented floor of currentSampleRate(). Same answer the old code
    // gave, reached deliberately rather than by accident.
    EXPECT_EQ(wma_get_recommended_buffer_size(mWma, 2.9f), 256);
}

// ===========================================================================
// The latency report
// ===========================================================================

TEST_F(CApiDiagnosticsTest, TheReportIsHeadedAndNamesTheBackend) {
    startAt(kSampleRate, 0);
    const std::string report = reportOf(mWma);

    EXPECT_TRUE(contains(report, "NoisyPad Latency Report"));
    // The backend was reachable all along and the old report never printed it,
    // so a report taken on the USB path said nothing about USB. The fake
    // registers itself as OBOE.
    EXPECT_TRUE(contains(report, "Backend: Oboe")) << report;
}

TEST_F(CApiDiagnosticsTest, ARunningStreamIsDescribedInFull) {
    startAt(44100, 0);
    const std::string report = reportOf(mWma);

    EXPECT_TRUE(contains(report, "Sample Rate: 44100 Hz")) << report;
    EXPECT_TRUE(contains(report, "Buffer Size:")) << report;
    EXPECT_TRUE(contains(report, "Output Latency:")) << report;
}

TEST_F(CApiDiagnosticsTest, NoStreamSaysSoInsteadOfOmittingTheNumbers) {
    // Engine created, never started. The old report just dropped the three
    // lines, which reads as a report of a zero-latency system rather than of a
    // system with nothing to measure.
    const std::string report = reportOf(mWma);

    EXPECT_TRUE(contains(report, "No stream running")) << report;
    EXPECT_FALSE(contains(report, "Sample Rate:")) << report;
}

TEST_F(CApiDiagnosticsTest, NoEngineIsItsOwnMessage) {
    const std::string report = reportOf(nullptr);

    EXPECT_TRUE(contains(report, "Engine not initialized")) << report;
    EXPECT_FALSE(contains(report, "Backend:")) << report;
}

// ===========================================================================
// The report's buffer contract — the part a caller can get wrong
// ===========================================================================

TEST_F(CApiDiagnosticsTest, MeasuringWithANullBufferReturnsTheFullLength) {
    startAt(kSampleRate, 0);

    const int needed = wma_get_latency_report(mWma, nullptr, 0);
    ASSERT_GT(needed, 0);

    std::vector<char> buffer(static_cast<size_t>(needed) + 1, '\xAA');
    const int written = wma_get_latency_report(mWma, buffer.data(),
                                               static_cast<int>(buffer.size()));
    EXPECT_EQ(written, needed) << "the measuring call and the writing call disagree";
    EXPECT_EQ(std::strlen(buffer.data()), static_cast<size_t>(needed));
}

TEST_F(CApiDiagnosticsTest, ASmallBufferTruncatesAndStillTerminates) {
    startAt(kSampleRate, 0);
    const int needed = wma_get_latency_report(mWma, nullptr, 0);
    ASSERT_GT(needed, 16);

    constexpr int kSmall = 16;
    // Sentinel past the end: if the write runs over, this is what catches it.
    std::vector<char> buffer(kSmall + 8, '\xAA');
    const int returned = wma_get_latency_report(mWma, buffer.data(), kSmall);

    // snprintf convention: the answer is what it WOULD have needed.
    EXPECT_EQ(returned, needed);
    EXPECT_EQ(std::strlen(buffer.data()), static_cast<size_t>(kSmall - 1));
    EXPECT_EQ(buffer[kSmall - 1], '\0') << "truncated output was not terminated";
    for (int i = kSmall; i < kSmall + 8; ++i) {
        EXPECT_EQ(buffer[i], '\xAA') << "wrote past buffer_size at byte " << i;
    }
}

TEST_F(CApiDiagnosticsTest, ASingleByteBufferGetsJustTheTerminator) {
    startAt(kSampleRate, 0);

    std::vector<char> buffer(4, '\xAA');
    wma_get_latency_report(mWma, buffer.data(), 1);

    EXPECT_EQ(buffer[0], '\0');
    EXPECT_EQ(buffer[1], '\xAA') << "one byte of capacity means one byte written";
}

// ===========================================================================
// Null handle
// ===========================================================================

TEST(CApiDiagnosticsNullHandle, EveryQueryReturnsTheValueTheJniUsedToReturnByHand) {
    // The JNI's own fallback was 48000, so 2.9 ms lands on 256 either way.
    EXPECT_EQ(wma_get_recommended_buffer_size(nullptr, 2.9f), 256);
    EXPECT_EQ(wma_get_recommended_buffer_size(nullptr, 0.0f), -1);

    EXPECT_GT(wma_get_latency_report(nullptr, nullptr, 0), 0);
}

TEST(CApiDiagnosticsNullHandle, AZeroSizedOrNullBufferIsNotWrittenTo) {
    char sentinel[4] = {'\xAA', '\xAA', '\xAA', '\xAA'};

    wma_get_latency_report(nullptr, sentinel, 0);
    EXPECT_EQ(sentinel[0], '\xAA');

    wma_get_latency_report(nullptr, sentinel, -1);
    EXPECT_EQ(sentinel[0], '\xAA');

    wma_get_latency_report(nullptr, nullptr, 64);
    SUCCEED();
}

}  // namespace
}  // namespace wma_test
