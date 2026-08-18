/**
 * test_c_api_master_bus.cpp
 *
 * What the master bus is, measured on the OUTPUT BUFFER.
 *
 * The suite had exactly one master-volume test before this file, and it was a
 * get/set round-trip. That is the shape of test this repo has now been burned by
 * three times (the output meters, setMaxVoices, the mixer setters): it passes
 * identically whether the gain reaches the audio or not. Everything here reads
 * the buffer.
 *
 * The defect these tests were written against — ticket 2 of §16 — was that the
 * master was applied inside applyEffectsAndLooper(), while the monitored input
 * was summed AFTERWARDS in handleMixMonitoring(). In MIX, pulling the master
 * down left the microphone at full level, and at zero the instrument went quiet
 * and the input did not.
 *
 * Two things the ticket said that measuring contradicted, both recorded here as
 * tests so they cannot drift back:
 *
 *   - "moving the master past the looper tap would stop it being baked into
 *     takes, where today it IS baked". It never was. The tap has run upstream of
 *     the master since the tap moved into applyEffectsAndLooper, and the comment
 *     at the call site said so. MasterVolumeNeverReachesWhatTheLooperRecords
 *     pins that.
 *
 *   - the fix is "one line". It is not, because the master could not simply move
 *     after the sum while the output stage ran inside applyEffectsAndLooper: the
 *     mix path was already running the full protection chain a SECOND time on
 *     the same block (processOutput, then processOutputNoClip), through the same
 *     stateful lookahead limiter and the same meters.
 *     TheOutputMeterReadsTheSignalThatActuallyLeaves pins that one.
 *
 * Levels are kept well under full scale on purpose. OutputStage runs a lookahead
 * limiter and a soft clipper, so near full scale the chain is deliberately
 * non-linear and a ratio of levels stops meaning what it looks like — the lesson
 * from the first version of SynthVolumeActuallyScalesTheAudibleOutput.
 */

#include "support/CApiFixture.h"

#include <cmath>

#include <gtest/gtest.h>

namespace wma_test {
namespace {

constexpr int kSampleRate = 48000;
constexpr int kBlockFrames = 256;

// AudioMode, mirroring core/AudioMode.h.
constexpr int kMix = 2;

/// Comfortably inside the linear region of the output chain.
constexpr float kInputLevel = 0.2f;

/// Below this an "is it audible" precondition is not worth trusting.
constexpr float kAudible = 0.01f;

class CApiMasterBusTest : public CApiFixture {
protected:
    /**
     * MIX with the instrument silent, so the only thing in the output buffer is
     * the monitored input and every level read below is unambiguously its.
     *
     * wma_set_audio_mode(MIX) is what attaches the InputNode and turns
     * monitoring on; the engine will not sum anything without both.
     */
    void startMixWithSilentInstrument() {
        startAt(kSampleRate, /*fadeTimeMs=*/0);
        wma_set_audio_mode(mWma, kMix);
        ASSERT_TRUE(wma_input_is_monitoring_enabled(mWma))
            << "MIX has to leave monitoring on, or nothing below sums anything";
        wma_set_frequency_amplitude(mWma, 440.0f, 0.0f);
        wma_input_set_monitoring_volume(mWma, 1.0f);

        // Settle the mixer's per-sample level smoother (~2 ms) so the first
        // measured block is already at full input level.
        renderWithInput(8, kBlockFrames, kInputLevel);
    }
};

// ===========================================================================
// The precondition. Everything below is vacuous without it.
// ===========================================================================

TEST_F(CApiMasterBusTest, TheMonitoredInputReachesTheOutputAtAll) {
    // This is the presence proof, and it is not ceremony: until the InputNode
    // double grew a real monitoring ring, getMonitoringSamples() returned 0 and
    // this whole path was unobservable from the host suite — which is how the
    // defect below shipped. If this fails, no other test in this file means
    // anything, because they would all be measuring silence against silence.
    startMixWithSilentInstrument();

    EXPECT_GT(renderBlockPeakWithInput(kBlockFrames, kInputLevel), kAudible)
        << "MIX has to actually mix the monitored input into the output";
}

// ===========================================================================
// Ticket 2 — the master is the level of everything that leaves
// ===========================================================================

TEST_F(CApiMasterBusTest, MasterVolumeScalesTheMonitoredInput) {
    startMixWithSilentInstrument();

    const float atUnity = renderBlockPeakWithInput(kBlockFrames, kInputLevel);
    ASSERT_GT(atUnity, kAudible)
        << "the monitored input has to be audible for the ratio below to mean anything";

    wma_set_master_volume(mWma, 0.5f);
    renderWithInput(2, kBlockFrames, kInputLevel);
    const float atHalf = renderBlockPeakWithInput(kBlockFrames, kInputLevel);

    EXPECT_NEAR(atHalf, atUnity * 0.5f, atUnity * 0.05f)
        << "halving the master should halve the microphone too — it is the level "
           "of everything that leaves, and the instrument level is a separate "
           "control (wma_set_synth_volume)";
}

TEST_F(CApiMasterBusTest, MasterVolumeAtZeroSilencesTheMonitoredInput) {
    // The sharpest statement of the user-visible bug: at zero master the
    // instrument went quiet and the microphone kept playing.
    startMixWithSilentInstrument();
    ASSERT_GT(renderBlockPeakWithInput(kBlockFrames, kInputLevel), kAudible);

    wma_set_master_volume(mWma, 0.0f);
    renderWithInput(2, kBlockFrames, kInputLevel);

    EXPECT_LT(renderBlockPeakWithInput(kBlockFrames, kInputLevel), 0.001f)
        << "zero master has to be silence, microphone included";
}

// ===========================================================================
// The premise the ticket got wrong
// ===========================================================================

TEST_F(CApiMasterBusTest, MasterVolumeNeverReachesWhatTheLooperRecords) {
    // The looper's recording tap reads the instrument bus BEFORE the master is
    // applied, and did so before this change too. The ticket claimed the
    // opposite ("today it IS baked into takes") and that claim is what made the
    // fix look like two audible changes instead of one. This test is here so the
    // corrected version is the one that survives.
    //
    // Read end to end rather than off the buffer: record at half master, then
    // play the take back at full master with the oscillator silent. What comes
    // out is what went in.
    auto recordAtMasterThenPlayAtUnity = [&](float masterWhileRecording) {
        startAt(kSampleRate, /*fadeTimeMs=*/0);
        wma_looper_set_enabled(mWma, true);
        EXPECT_EQ(wma_looper_prepare_track(mWma, 0, 8 * kBlockFrames, kSampleRate), WMA_OK);

        wma_set_master_volume(mWma, masterWhileRecording);
        wma_set_frequency_amplitude(mWma, 440.0f, 0.3f);
        render(8, kBlockFrames);

        wma_looper_start_recording(mWma, 0);
        render(8, kBlockFrames);
        wma_looper_stop_recording(mWma);

        // Silence the instrument and restore the master, so the only thing left
        // in the output is loop playback at a known gain.
        wma_set_frequency_amplitude(mWma, 440.0f, 0.0f);
        wma_set_master_volume(mWma, 1.0f);
        render(8, kBlockFrames);

        float peak = 0.0f;
        for (int i = 0; i < 8; ++i) {
            peak = std::max(peak, renderBlockPeak(kBlockFrames));
        }
        return peak;
    };

    const float recordedAtUnity = recordAtMasterThenPlayAtUnity(1.0f);
    ASSERT_GT(recordedAtUnity, kAudible) << "nothing was recorded, so there is nothing to compare";

    TearDown();
    SetUp();

    const float recordedAtHalf = recordAtMasterThenPlayAtUnity(0.5f);

    EXPECT_NEAR(recordedAtHalf, recordedAtUnity, recordedAtUnity * 0.05f)
        << "the master must not be baked into takes — the recording tap runs "
           "upstream of it, and moving the master later did not change that";
}

// ===========================================================================
// The output stage runs once per block, on the signal that actually leaves
// ===========================================================================

TEST_F(CApiMasterBusTest, TheOutputMeterReadsTheSignalThatActuallyLeaves) {
    // MIX used to run the whole protection chain twice per block: once at the
    // tail of applyEffectsAndLooper on the instrument bus, and again on the
    // summed buffer. Two things came out of that, and this test catches both
    // through the meter because the meter is where they are cheap to see:
    //
    //   1. The meters were updated twice per block, so their 300 ms integration
    //      ran at double rate — the same class of defect #104 fixed.
    //   2. Worse, the FIRST of the two passes saw the pre-mix instrument bus. With
    //      a silent instrument that is silence, so every block pulled the meter
    //      toward zero and then toward the real level. The settled reading was
    //      X/(1+c) — about half of what was truly leaving.
    //
    // El medidor publica RMS, asi que se lo compara contra el RMS del buffer.
    // Antes se lo comparaba contra el PICO, y eso solo funcionaba porque el
    // estimulo era DC: para una senal constante RMS y pico son el mismo numero.
    // Con el `InputNode` real ese estimulo dejo de ser valido —su DC blocker lo
    // borra— y la comparacion vieja fallaba por 1/raiz(2), que es geometria del
    // seno y no un defecto del motor. Comparar RMS contra RMS no necesita
    // ningun modelo de la cadena del medio y vale para cualquier senal.
    //
    // El defecto que este test existe para cachar seguia siendo visible: la
    // doble pasada dejaba el medidor en la MITAD, y media es 50 % de desvio
    // contra una tolerancia de 5 %.
    startMixWithSilentInstrument();

    // ~1.1 s at 48 kHz — several times the 300 ms integration, so the meter is
    // settled and not still climbing.
    renderWithInput(200, kBlockFrames, kInputLevel);

    const float inTheBuffer = renderBlockRmsWithInput(kBlockFrames, kInputLevel);
    ASSERT_GT(inTheBuffer, kAudible);

    const float onTheMeterL = wma_get_output_rms(mWma, 0);

    EXPECT_NEAR(onTheMeterL, inTheBuffer, inTheBuffer * 0.05f)
        << "the output meter has to read what the engine handed over; reading "
           "about half of it means the protection chain ran twice and the first "
           "pass metered the pre-mix bus";
}

// ===========================================================================
// The stopped engine is still an output path
// ===========================================================================

TEST_F(CApiMasterBusTest, MasterVolumeScalesMonitoringWhileTheEngineIsStopped) {
    // Not a corner: monitoring is live before the engine is started, and
    // handleNotRunning() copied the microphone straight to the device — no
    // master, no protection. It is the same defect as ticket 2 in a second
    // place, and the ticket did not name it.
    wma_set_audio_mode(mWma, kMix);
    ASSERT_TRUE(wma_input_is_monitoring_enabled(mWma));
    wma_input_set_monitoring_volume(mWma, 1.0f);

    const float atUnity = renderBlockPeakWithInput(kBlockFrames, kInputLevel);
    ASSERT_GT(atUnity, kAudible)
        << "a stopped engine still passes monitoring through, or this is vacuous";

    wma_set_master_volume(mWma, 0.5f);
    const float atHalf = renderBlockPeakWithInput(kBlockFrames, kInputLevel);

    EXPECT_NEAR(atHalf, atUnity * 0.5f, atUnity * 0.05f)
        << "the master is the level of everything that leaves, including what "
           "leaves while the engine is stopped";
}

TEST_F(CApiMasterBusTest, MonitoringWhileStoppedIsProtectedAgainstClipping) {
    // The default input gain is +12 dB, so a signal over full scale at this
    // point is ordinary, not pathological. Unprotected it went to the device at
    // 2.0.
    wma_set_audio_mode(mWma, kMix);
    wma_input_set_monitoring_volume(mWma, 1.0f);

    const float peak = renderBlockPeakWithInput(kBlockFrames, 2.0f);

    ASSERT_GT(peak, kAudible) << "nothing came through, so nothing was tested";
    EXPECT_LE(peak, 1.0f + 1e-4f)
        << "nothing may leave above full scale, stopped engine included";
}

}  // namespace
}  // namespace wma_test
