/**
 * test_c_api_transport.cpp
 *
 * The musical clock and the metronome — section 20 of watermelon_audio.h.
 *
 * This is the first category whose behaviour is audible in the host suite
 * end to end. The metronome is not a flag someone reads back: Transport::tick()
 * runs on the audio thread inside AudioEngine::onAudioReady, and the click it
 * schedules is rendered into the output buffer by AudioLooper::process a few
 * lines later. So "did the metronome click" is answered by looking at the
 * samples, not by asking the object whether it thinks it clicked.
 *
 * That is worth stating because the two categories before this one could not do
 * it. `mode` had to settle for "the output carries no residue" because the
 * reverb tail never reaches the output in the harness, and `analysis` found the
 * output meters were reading a node nothing drives. Here the whole path is
 * live, so the tests below assert on rendered audio wherever the behaviour is
 * about sound.
 *
 * Two things the tests depend on, both verified by reading the render path:
 *
 *   - The click starts at the first sample of the block whose tick() fired it.
 *     OutputStage's lookahead limiter then delays it by a fixed 5 ms (240
 *     frames at 48 kHz). With a 10 ms click and 1000-frame blocks, a click
 *     occupies frames [240, 720) of its block and never crosses into the next
 *     one — which is why block indices below are exact rather than fuzzy.
 *
 *   - Nothing else is making sound. No touches are down, so the oscillator is
 *     silent and the only thing above the dither floor (~1e-5) is the click.
 *
 * NOT covered here, and deliberately not faked:
 *
 *   - The USB render path (AudioEngine.cpp's LibusbBackend fast path) has its
 *     own mTransport.tick() call. The fake backend reports OBOE, so only the
 *     main path is exercised. The two call sites are one line apart and were
 *     diffed by hand; a divergence between them would not fail this suite.
 */

#include "support/CApiFixture.h"
#include "tests/support/TestWait.h"

#include <mutex>
#include <vector>

#include <gtest/gtest.h>

namespace wma_test {
namespace {

// 1000 frames keeps each click whole inside one block (see the header note) and
// divides 24000 evenly, so a beat is exactly 24 blocks at 120 BPM / 48 kHz.
constexpr int kBlockFrames = 1000;
constexpr int kSampleRate = 48000;

// MetronomeClick renders downbeats at gain 0.35 and off-beats at 0.25. The
// threshold sits well below the quieter of the two and far above the dither
// floor, so "a click happened" is not a judgement call.
constexpr float kClickThreshold = 0.05f;
// Midpoint of the two gains: anything above is a downbeat, below an off-beat.
constexpr float kDownbeatThreshold = 0.30f;

class CApiTransportTest : public CApiFixture {
protected:
    /// Render @p blocks blocks and return the peak of each one, in order.
    std::vector<float> renderBlockPeaks(int blocks) {
        std::vector<float> peaks;
        peaks.reserve(static_cast<size_t>(blocks));
        for (int i = 0; i < blocks; ++i) {
            peaks.push_back(renderBlockPeak(kBlockFrames));
        }
        return peaks;
    }

    /// Indices of the blocks that carried a click.
    static std::vector<int> clickBlocks(const std::vector<float>& peaks) {
        std::vector<int> hits;
        for (size_t i = 0; i < peaks.size(); ++i) {
            if (peaks[i] > kClickThreshold) hits.push_back(static_cast<int>(i));
        }
        return hits;
    }

    /// The peak of each block that carried a click, in order.
    static std::vector<float> clickPeaks(const std::vector<float>& peaks) {
        std::vector<float> hits;
        for (float p : peaks) {
            if (p > kClickThreshold) hits.push_back(p);
        }
        return hits;
    }
};

// ===========================================================================
// The clock
// ===========================================================================

TEST_F(CApiTransportTest, TheDefaultMeterIsFourFour) {
    EXPECT_EQ(wma_transport_get_beats_per_bar(mWma), 4);
}

TEST_F(CApiTransportTest, TheMeterRoundTrips) {
    wma_transport_set_beats_per_bar(mWma, 3);
    EXPECT_EQ(wma_transport_get_beats_per_bar(mWma), 3);

    wma_transport_set_beats_per_bar(mWma, 7);
    EXPECT_EQ(wma_transport_get_beats_per_bar(mWma), 7);
}

TEST_F(CApiTransportTest, TheMeterIsClampedRatherThanRejected) {
    // 1..16. A zero would be a modulo-by-zero on the audio thread in tick()'s
    // every-beat pattern branch, so the floor is load-bearing, not cosmetic.
    wma_transport_set_beats_per_bar(mWma, 0);
    EXPECT_EQ(wma_transport_get_beats_per_bar(mWma), 1);

    wma_transport_set_beats_per_bar(mWma, -4);
    EXPECT_EQ(wma_transport_get_beats_per_bar(mWma), 1);

    wma_transport_set_beats_per_bar(mWma, 99);
    EXPECT_EQ(wma_transport_get_beats_per_bar(mWma), 16);
}

TEST_F(CApiTransportTest, FramesPerBeatFollowsTheBpm) {
    startAt(kSampleRate, /*fadeTimeMs=*/0);

    // 120 BPM at 48 kHz — the default.
    EXPECT_EQ(wma_transport_frames_per_beat(mWma), 24000);

    wma_set_bpm(mWma, 60.0f);
    EXPECT_EQ(wma_transport_frames_per_beat(mWma), 48000);

    wma_set_bpm(mWma, 240.0f);
    EXPECT_EQ(wma_transport_frames_per_beat(mWma), 12000);
}

TEST_F(CApiTransportTest, TheBpmIsClampedToTheTransportRange) {
    startAt(kSampleRate, 0);

    // 20..300. Below the floor the clock would run so slow that framesPerBeat
    // overflows a beat's worth of int arithmetic in framesPerBar().
    wma_set_bpm(mWma, 1.0f);
    EXPECT_FLOAT_EQ(wma_get_bpm(mWma), 20.0f);

    wma_set_bpm(mWma, 10000.0f);
    EXPECT_FLOAT_EQ(wma_get_bpm(mWma), 300.0f);
}

TEST_F(CApiTransportTest, FramesPerBeatFollowsTheNegotiatedSampleRate) {
    // The rate the backend actually gave us, not the one we asked for. This is
    // the same mechanism test_current_sample_rate.cpp covers for the engine —
    // it reaches the Transport through AudioEngine::prepare().
    startAt(44100, /*fadeTimeMs=*/0);
    EXPECT_EQ(wma_transport_frames_per_beat(mWma), 22050);
}

TEST_F(CApiTransportTest, FramesPerBarIsTheBeatTimesTheMeterTimesTheBars) {
    startAt(kSampleRate, 0);
    wma_transport_set_beats_per_bar(mWma, 3);

    const int beat = wma_transport_frames_per_beat(mWma);
    EXPECT_EQ(wma_transport_frames_per_bar(mWma, 1), beat * 3);
    EXPECT_EQ(wma_transport_frames_per_bar(mWma, 2), beat * 6);
}

TEST_F(CApiTransportTest, AskingForNoBarsIsZeroFrames) {
    startAt(kSampleRate, 0);

    // NoisyPad feeds this straight into looperPrepareTrack as a length. A
    // negative bar count returning a negative length would size a buffer.
    EXPECT_EQ(wma_transport_frames_per_bar(mWma, 0), 0);
    EXPECT_EQ(wma_transport_frames_per_bar(mWma, -1), 0);
}

// ===========================================================================
// The scheduler — state
// ===========================================================================

TEST_F(CApiTransportTest, TheMetronomeStartsIdle) {
    EXPECT_FALSE(wma_transport_is_metronome_running(mWma));
    EXPECT_FALSE(wma_transport_is_metronome_continuous(mWma));
    EXPECT_EQ(wma_transport_get_remaining_beats(mWma), 0);
}

TEST_F(CApiTransportTest, StartingACountedScheduleArmsIt) {
    startAt(kSampleRate, 0);
    wma_transport_start_metronome(mWma, 4, true, false);

    EXPECT_TRUE(wma_transport_is_metronome_running(mWma));
    EXPECT_FALSE(wma_transport_is_metronome_continuous(mWma));
    EXPECT_EQ(wma_transport_get_remaining_beats(mWma), 4);
}

TEST_F(CApiTransportTest, AScheduleOfZeroBeatsStopsInsteadOfStarting) {
    startAt(kSampleRate, 0);
    wma_transport_start_metronome(mWma, 4, true, false);
    ASSERT_TRUE(wma_transport_is_metronome_running(mWma));

    // startMetronome() routes beats <= 0 to stopMetronome(). A count-in of zero
    // is "no count-in", not "a schedule that fires forever".
    wma_transport_start_metronome(mWma, 0, true, false);
    EXPECT_FALSE(wma_transport_is_metronome_running(mWma));

    wma_transport_start_metronome(mWma, -1, true, false);
    EXPECT_FALSE(wma_transport_is_metronome_running(mWma));
}

TEST_F(CApiTransportTest, RemainingBeatsIsASentinelInContinuousMode) {
    startAt(kSampleRate, 0);
    wma_transport_start_metronome_continuous(mWma, true);

    // CHARACTERIZATION, not an endorsement. Continuous mode arms the scheduler
    // by storing 1 into the same counter a counted schedule counts down, and
    // never decrements it. So this reads 1 forever, which is not a number of
    // anything. It is documented on wma_transport_get_remaining_beats(); a
    // caller must gate on is_metronome_continuous() first.
    EXPECT_EQ(wma_transport_get_remaining_beats(mWma), 1);

    render(240, kBlockFrames);  // ten beats' worth
    EXPECT_EQ(wma_transport_get_remaining_beats(mWma), 1);
    EXPECT_TRUE(wma_transport_is_metronome_running(mWma));
}

// ===========================================================================
// The scheduler — what it actually plays
// ===========================================================================

TEST_F(CApiTransportTest, ACountedScheduleEmitsOneClickPerBeatAtTheBeatInterval) {
    startAt(kSampleRate, 0);
    ASSERT_EQ(wma_transport_frames_per_beat(mWma), 24000);
    constexpr int kBlocksPerBeat = 24000 / kBlockFrames;

    wma_transport_start_metronome(mWma, 4, true, false);

    // Five beats' worth of blocks, so the fifth beat would be visible if the
    // schedule failed to stop itself.
    const std::vector<int> hits = clickBlocks(renderBlockPeaks(5 * kBlocksPerBeat));

    ASSERT_EQ(hits.size(), 4u) << "expected exactly four clicks for a four-beat count-in";
    EXPECT_EQ(hits[0], 0) << "the first click fires on the very next block, not a beat later";
    for (size_t i = 1; i < hits.size(); ++i) {
        EXPECT_EQ(hits[i] - hits[i - 1], kBlocksPerBeat)
            << "click " << i << " did not land one beat after click " << (i - 1);
    }
}

TEST_F(CApiTransportTest, TheBeatIntervalTracksTheBpm) {
    startAt(kSampleRate, 0);
    wma_set_bpm(mWma, 240.0f);  // half the frames per beat → twice the clicks
    ASSERT_EQ(wma_transport_frames_per_beat(mWma), 12000);

    wma_transport_start_metronome(mWma, 4, true, false);
    const std::vector<int> hits = clickBlocks(renderBlockPeaks(60));

    ASSERT_EQ(hits.size(), 4u);
    EXPECT_EQ(hits[1] - hits[0], 12);
    EXPECT_EQ(hits[3] - hits[2], 12);
}

TEST_F(CApiTransportTest, TheCountedScheduleFallsSilentAfterItsLastBeat) {
    startAt(kSampleRate, 0);
    wma_transport_start_metronome(mWma, 2, true, false);

    render(24 * 3, kBlockFrames);  // past the end of a two-beat schedule
    EXPECT_FALSE(wma_transport_is_metronome_running(mWma));
    EXPECT_EQ(wma_transport_get_remaining_beats(mWma), 0);

    // And nothing more comes out.
    EXPECT_TRUE(clickBlocks(renderBlockPeaks(24 * 3)).empty());
}

TEST_F(CApiTransportTest, TheFirstClickIsLouderWhenItIsADownbeat) {
    startAt(kSampleRate, 0);
    wma_transport_start_metronome(mWma, 3, /*first_is_downbeat=*/true,
                                  /*every_beat_pattern=*/false);

    const std::vector<float> peaks = clickPeaks(renderBlockPeaks(24 * 3));
    ASSERT_EQ(peaks.size(), 3u);
    EXPECT_GT(peaks[0], kDownbeatThreshold) << "the downbeat should be the loud one";
    EXPECT_LT(peaks[1], kDownbeatThreshold);
    EXPECT_LT(peaks[2], kDownbeatThreshold);
}

TEST_F(CApiTransportTest, WithoutADownbeatEveryClickIsTheSame) {
    startAt(kSampleRate, 0);
    wma_transport_start_metronome(mWma, 3, /*first_is_downbeat=*/false,
                                  /*every_beat_pattern=*/false);

    const std::vector<float> peaks = clickPeaks(renderBlockPeaks(24 * 3));
    ASSERT_EQ(peaks.size(), 3u);
    for (float p : peaks) EXPECT_LT(p, kDownbeatThreshold);
}

TEST_F(CApiTransportTest, TheEveryBeatPatternPutsADownbeatOnEachBarline) {
    startAt(kSampleRate, 0);
    wma_transport_set_beats_per_bar(mWma, 2);
    // every_beat_pattern overrides first_is_downbeat: the pattern is
    // "index % beats_per_bar == 0", so with a 2/4 meter it alternates.
    wma_transport_start_metronome(mWma, 4, /*first_is_downbeat=*/false,
                                  /*every_beat_pattern=*/true);

    const std::vector<float> peaks = clickPeaks(renderBlockPeaks(24 * 4));
    ASSERT_EQ(peaks.size(), 4u);
    EXPECT_GT(peaks[0], kDownbeatThreshold);
    EXPECT_LT(peaks[1], kDownbeatThreshold);
    EXPECT_GT(peaks[2], kDownbeatThreshold) << "beat 2 opens the second bar in 2/4";
    EXPECT_LT(peaks[3], kDownbeatThreshold);
}

TEST_F(CApiTransportTest, ContinuousKeepsClickingPastAnyCountedHorizon) {
    startAt(kSampleRate, 0);
    wma_transport_start_metronome_continuous(mWma, true);

    // Ten beats. A counted schedule armed with the continuous sentinel of 1
    // would give exactly one click here.
    const std::vector<int> hits = clickBlocks(renderBlockPeaks(24 * 10));

    EXPECT_EQ(hits.size(), 10u);
    EXPECT_TRUE(wma_transport_is_metronome_continuous(mWma));
    EXPECT_TRUE(wma_transport_is_metronome_running(mWma));
}

TEST_F(CApiTransportTest, StoppingSilencesTheScheduleFromTheNextBlockOn) {
    startAt(kSampleRate, 0);
    wma_transport_start_metronome_continuous(mWma, true);
    ASSERT_FALSE(clickBlocks(renderBlockPeaks(24 * 2)).empty());

    wma_transport_stop_metronome(mWma);
    EXPECT_FALSE(wma_transport_is_metronome_running(mWma));
    EXPECT_FALSE(wma_transport_is_metronome_continuous(mWma));

    EXPECT_TRUE(clickBlocks(renderBlockPeaks(24 * 4)).empty());
}

TEST_F(CApiTransportTest, ACountedScheduleReplacesAContinuousOne) {
    startAt(kSampleRate, 0);
    wma_transport_start_metronome_continuous(mWma, true);
    render(24, kBlockFrames);

    wma_transport_start_metronome(mWma, 2, true, false);
    EXPECT_FALSE(wma_transport_is_metronome_continuous(mWma));
    EXPECT_EQ(wma_transport_get_remaining_beats(mWma), 2);

    // Two clicks, then silence — not the endless run it was in before.
    const std::vector<int> hits = clickBlocks(renderBlockPeaks(24 * 5));
    EXPECT_EQ(hits.size(), 2u);
}

TEST_F(CApiTransportTest, AManualClickSoundsWithoutArmingTheScheduler) {
    startAt(kSampleRate, 0);

    // wma_looper_trigger_click keeps its looper_ name (§19) but it is the same
    // click generator the scheduler drives — the one-shot path NoisyPad uses
    // for a tap-tempo tick.
    wma_looper_trigger_click(mWma, /*is_downbeat=*/true);
    EXPECT_GT(renderBlockPeak(kBlockFrames), kDownbeatThreshold);

    EXPECT_FALSE(wma_transport_is_metronome_running(mWma));
    EXPECT_TRUE(clickBlocks(renderBlockPeaks(24 * 2)).empty());
}

TEST_F(CApiTransportTest, AManualOffBeatClickIsTheQuieterOne) {
    startAt(kSampleRate, 0);
    wma_looper_trigger_click(mWma, /*is_downbeat=*/false);

    const float peak = renderBlockPeak(kBlockFrames);
    EXPECT_GT(peak, kClickThreshold);
    EXPECT_LT(peak, kDownbeatThreshold);
}

TEST_F(CApiTransportTest, AnIdleTransportPutsNothingInTheOutput) {
    startAt(kSampleRate, 0);

    // The baseline the click tests are measured against. If the oscillator or
    // anything else were bleeding into the output above the dither floor,
    // every clickBlocks() assertion above would be reading noise.
    EXPECT_TRUE(clickBlocks(renderBlockPeaks(24 * 2)).empty());
}

// ===========================================================================
// Null handle — the values the JNI used to return by hand
// ===========================================================================

TEST(CApiTransportNullHandle, EveryQueryReturnsTheValueTheJniUsedToReturnByHand) {
    // 4, not 0: the JNI picked the default meter here precisely so a caller
    // dividing by it would not fault. The C API now owns that choice.
    EXPECT_EQ(wma_transport_get_beats_per_bar(nullptr), 4);

    EXPECT_EQ(wma_transport_frames_per_beat(nullptr), 0);
    EXPECT_EQ(wma_transport_frames_per_bar(nullptr, 4), 0);
    EXPECT_FALSE(wma_transport_is_metronome_running(nullptr));
    EXPECT_FALSE(wma_transport_is_metronome_continuous(nullptr));
    EXPECT_EQ(wma_transport_get_remaining_beats(nullptr), 0);
    EXPECT_FLOAT_EQ(wma_get_bpm(nullptr), 120.0f);
}

TEST(CApiTransportNullHandle, EveryMutatorIsANoOpRatherThanACrash) {
    wma_transport_set_beats_per_bar(nullptr, 4);
    wma_transport_start_metronome(nullptr, 4, true, false);
    wma_transport_start_metronome_continuous(nullptr, true);
    wma_transport_stop_metronome(nullptr);
    wma_looper_trigger_click(nullptr, true);
    wma_set_bpm(nullptr, 140.0f);
    SUCCEED();
}


// ===========================================================================
// REQ-017 — el beat pusheado, y su ancla
// ===========================================================================

class CApiBeatTest : public CApiTransportTest {
protected:
    struct Beat {
        int     index;
        int64_t nextBeatFrame;
    };

    /**
     * El receptor es MIEMBRO del fixture, no un local del test, y eso no es
     * estilo: el header de `wma_looper_set_event_callback` declara que un evento
     * en vuelo cuando se limpia el sink TODAVIA aterriza en el receptor viejo, asi
     * que `user_data` tiene que seguir vivo despues. Un local muere al salir del
     * test —antes de `TearDown()`, que es quien destruye el motor— y el worker
     * despacha sobre un mutex ya muerto. Ese exacto use-after-free mato a
     * `cpp-tests-macos` en master una vez.
     */
    struct Sink {
        std::mutex mutex;
        std::vector<Beat> beats;

        static void callback(int type, int trackIndex, float value, void* userData) {
            if (type != WMA_LOOPER_EVENT_BEAT) return;
            auto* self = static_cast<Sink*>(userData);
            std::lock_guard<std::mutex> lk(self->mutex);
            self->beats.push_back(Beat{static_cast<int>(value), trackIndex});
        }

        size_t size() {
            std::lock_guard<std::mutex> lk(mutex);
            return beats.size();
        }
    };

    Sink mSink;

    void installBeatSink() {
        wma_looper_set_event_callback(mWma, &Sink::callback, &mSink);
    }
};

// AC-017.4, de punta a punta y contra el ORACULO MAS DURO que hay: el sonido.
// El ancla del beat k tiene que apuntar al bloque donde el click k+1 se ESCUCHA,
// no al que dice una cuenta interna. Si el ancla fuera decorativa o estuviera
// corrida, esto lo ve.
TEST_F(CApiBeatTest, TheBeatAnchorPointsAtTheBlockWhereTheNextClickIsAudible) {
    startAt(kSampleRate, 0);
    installBeatSink();
    wma_set_bpm(mWma, 120.0f);          // 24000 frames/beat = 24 bloques
    wma_transport_start_metronome_continuous(mWma, true);

    const int64_t frame0 = wma_transport_get_play_frame(mWma);
    const std::vector<float> peaks = renderBlockPeaks(100);
    const std::vector<int> audible = clickBlocks(peaks);

    ASSERT_GE(audible.size(), 3u) << "hacen falta al menos 3 clicks audibles";
    ASSERT_TRUE(waitUntil([&] { return mSink.size() >= audible.size(); }))
        << "llegaron " << mSink.size() << " beats para " << audible.size()
        << " clicks audibles";

    std::lock_guard<std::mutex> lk(mSink.mutex);
    ASSERT_GE(mSink.beats.size(), audible.size());

    for (size_t k = 0; k + 1 < audible.size(); ++k) {
        const int64_t anchor = mSink.beats[k].nextBeatFrame;
        const int anchorBlock =
            static_cast<int>((anchor - frame0) / kBlockFrames);
        EXPECT_EQ(anchorBlock, audible[k + 1])
            << "el ancla del beat " << k << " (frame " << anchor
            << ") no cae en el bloque donde suena el click siguiente";
    }
}

// AC-017.4 — la propiedad que hace utilizable al ancla: restarle el play frame da
// los frames que FALTAN, y son positivos. Es lo que le permite al consumidor
// animar a tiempo en vez de reaccionar tarde, sin conocer su propio atraso.
TEST_F(CApiBeatTest, SubtractingThePlayFrameFromTheAnchorLeavesTimeStillToGo) {
    startAt(kSampleRate, 0);
    installBeatSink();
    wma_set_bpm(mWma, 120.0f);
    wma_transport_start_metronome_continuous(mWma, true);

    // 30 bloques = 30000 frames, con beats en 0 y 24000: exactamente DOS.
    //
    // Se espera por los DOS y no por "al menos uno", y eso no es prolijidad: los
    // eventos llegan por un worker que polea, asi que con `>= 1` el `back()` de
    // abajo podia ser todavia el beat 0 —cuya ancla (24000) ya quedo atras de los
    // 30000 frames renderizados— y el test fallaba SEGUN QUE OTROS TESTS
    // corrieran al lado. Pasaba solo y fallaba acompanado.
    renderBlockPeaks(30);
    ASSERT_TRUE(waitUntil([&] { return mSink.size() >= 2; }))
        << "llegaron " << mSink.size() << " beats, se esperaban 2";

    // El ULTIMO beat recibido, no el primero: el ancla apunta al futuro en el
    // momento de emitirse, y para cuando se renderizaron 30 bloques el ancla del
    // beat 0 ya quedo atras — como corresponde.
    int64_t anchor = 0;
    {
        std::lock_guard<std::mutex> lk(mSink.mutex);
        anchor = mSink.beats.back().nextBeatFrame;
    }
    const int64_t now = wma_transport_get_play_frame(mWma);
    const int64_t remaining = anchor - now;

    EXPECT_GT(remaining, 0) << "el proximo beat ya paso: el ancla no sirve para anticipar";
    EXPECT_LE(remaining, 24000) << "no puede faltar mas de un beat entero";
}

TEST_F(CApiBeatTest, ThePlayFrameAdvancesWithTheRenderAndIsNullSafe) {
    startAt(kSampleRate, 0);
    const int64_t before = wma_transport_get_play_frame(mWma);
    renderBlockPeaks(5);
    const int64_t after = wma_transport_get_play_frame(mWma);
    EXPECT_EQ(after - before, 5 * kBlockFrames);

    EXPECT_EQ(wma_transport_get_play_frame(nullptr), 0);
}

}  // namespace
}  // namespace wma_test
