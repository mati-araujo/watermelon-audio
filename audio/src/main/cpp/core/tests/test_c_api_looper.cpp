/**
 * test_c_api_looper.cpp
 *
 * The looper — section 19 of watermelon_audio.h. Batch 1 of the category: the
 * 40 entry points that already had a `wma_looper_*` counterpart and were still
 * transcribing AudioLooper by hand in the JNI.
 *
 * WHAT THIS FILE IS AND IS NOT FOR.
 *
 * AudioLooper's own behaviour is already covered, and thoroughly, by
 * looper/tests/ — test_audio_looper.cpp, test_track_buffer.cpp,
 * test_free_loop_autosync.cpp and friends. Re-asserting "recording makes a
 * track active" here would be duplicating a better test one directory over.
 *
 * What was NOT covered, and is what this batch actually changed, is the C API
 * boundary:
 *
 *   - 38 hand-written "no engine" defaults moved out of the JNI and into
 *     wma_looper_*. If any one of them changed value in the move, Android
 *     changes behaviour silently and no compiler says a word. The null-handle
 *     sweep below pins every single one against what the JNI used to return.
 *
 *   - The C API has to reach THE SAME AudioLooper the audio callback renders.
 *     A wma_* that quietly operated on a different instance would pass every
 *     state round-trip and still be silent in production — that is exactly the
 *     bug the InputNode unification fixed, so it is worth proving rather than
 *     assuming. AudibleRoundTrip below records real synth output through the C
 *     API and plays it back.
 *
 *   - wma_looper_set_track_loop_region took `int` while every other link in the
 *     chain —Kotlin Long, jlong, AudioLooper's int64_t— was 64-bit. See below.
 */

#include "support/CApiFixture.h"
#include "tests/support/TestWait.h"

#include "looper/LooperExportTypes.h"

#include <algorithm>
#include <deque>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <chrono>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>

namespace wma_test {
namespace {

constexpr int kBlockFrames = 256;
constexpr int kSampleRate = 48000;

/// Anything above this is the engine making sound rather than the dither floor.
constexpr float kAudible = 0.01f;

/// Junta lo que llega, con candado: el callback corre en el worker, no acá.
///
/// 🔴 **Su vida la maneja el fixture, y no es un detalle de estilo.** Ver
/// `installCollector()` — declarar uno local en el cuerpo de un test es un
/// use-after-free con abort, medido.
struct EventCollector {
    struct Received {
        int type;
        int trackIndex;
        float value;
    };

    std::mutex mutex;
    std::vector<Received> events;

    static void callback(int type, int trackIndex, float value, void* userData) {
        auto* self = static_cast<EventCollector*>(userData);
        std::lock_guard<std::mutex> lk(self->mutex);
        self->events.push_back({type, trackIndex, value});
    }

    size_t size() {
        std::lock_guard<std::mutex> lk(mutex);
        return events.size();
    }

    std::vector<Received> snapshot() {
        std::lock_guard<std::mutex> lk(mutex);
        return events;
    }
};

class CApiLooperTest : public CApiFixture {
protected:
    /// Record real engine output into @p track, so it has content and a length.
    ///
    /// Not a convenience: a track that was only prepare()d has capacity but
    /// mLengthFrames == 0, and TrackBuffer::setLoopRegion returns early on a
    /// zero length. A loop region only exists on a track that has something to
    /// loop, which is worth knowing before writing a test that assumes
    /// otherwise.
    /// A scratch directory that is removed when the test finishes.
    std::string tempDir() {
        if (mTempDir.empty()) {
            mTempDir = (std::filesystem::temp_directory_path()
                        / ("wma-looper-" + std::to_string(
                               ::testing::UnitTest::GetInstance()
                                   ->current_test_info()->line()))).string();
            std::filesystem::remove_all(mTempDir);
            std::filesystem::create_directories(mTempDir);
        }
        return mTempDir;
    }

    std::string tempPath(const std::string& name) {
        return (std::filesystem::path(tempDir()) / name).string();
    }

    /// Raw bytes of a written file, for looking at what actually landed in it.
    static std::string fileBytes(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
    }

    static long fileSize(const std::string& path) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        return ec ? -1 : static_cast<long>(size);
    }

    void TearDown() override {
        if (!mTempDir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(mTempDir, ec);
        }
        CApiFixture::TearDown();
    }

    /**
     * Instala un receptor de eventos **del que el fixture es dueño**, y devuelve
     * la referencia.
     *
     * POR QUE NO SE DECLARA UNO LOCAL EN EL CUERPO DEL TEST
     * -----------------------------------------------------
     * Porque el motor le sobrevive. Las variables del cuerpo se destruyen al
     * salir de el, y `TearDown()` —que es quien llama a `wma_engine_destroy`—
     * corre DESPUES: en esa ventana el worker despacha sobre un `std::mutex` ya
     * destruido y el proceso se cae con
     *
     *     libc++abi: terminating due to uncaught exception of type
     *     std::__1::system_error: mutex lock failed: Invalid argument
     *
     * que es exactamente el mensaje con el que murio `cpp-tests-macos` en master
     * el 2026-08-20 (`bc44b27`). Reproducido 5/5 con un probe deliberado.
     *
     * **El motor no tiene la culpa y no hay nada que arreglarle.** La regla esta
     * escrita en `watermelon_audio.h`, en el KDoc de
     * `wma_looper_set_event_callback`: *"user_data must stay valid after the
     * clear... freeing it right after clearing is a use-after-free, not a race
     * you can win by ordering the calls"*. El que la violaba era este archivo,
     * en sus cuatro tests de eventos.
     *
     * Al ser MIEMBRO del fixture, el receptor se destruye cuando se destruye el
     * objeto del fixture — o sea despues de `TearDown()`, con el motor ya muerto
     * y el worker ya frenado. La disciplina deja de depender de acordarse.
     *
     * `std::deque` y no `vector` porque las referencias que ya se entregaron no
     * se pueden invalidar cuando se pide un segundo receptor.
     */
    EventCollector& installCollector() {
        mCollectors.emplace_back();
        EventCollector& c = mCollectors.back();
        wma_looper_set_event_callback(mWma, &EventCollector::callback, &c);
        return c;
    }

    /// Espera a que @p c haya juntado al menos @p minCount eventos. Por
    /// CONDICION y con techo: el veredicto no puede depender de cuanto tarda el
    /// worker en conseguir un turno, que es como se rompia antes.
    static bool waitForEvents(EventCollector& c, size_t minCount = 1) {
        return waitUntil([&] { return c.size() >= minCount; });
    }

    /**
     * Espera a que @p c DEJE DE CRECER y devuelve el tamanio ya asentado.
     *
     * Existe para los eventos que estaban EN VUELO cuando se cambio el sink: el
     * header declara que todavia aterrizan en el receptor viejo, y el dispatcher
     * no expone contador de despachados (`LooperEventDispatcher` solo lleva
     * `mDropped`, que cuenta desbordes de cola), asi que no hay un observable al
     * que preguntarle "ya terminaste".
     *
     * **No es un sleep disfrazado, y la diferencia es la que separa este REQ de
     * su defecto:** si el worker va lento, la quietud se detecta MAS TARDE pero se
     * detecta igual; un `sleep_for` fijo simplemente se queda corto y afirma sobre
     * un estado a medio formar. Por eso `quiet` no se escala — es parte de la
     * condicion, no una espera ciega.
     */
    static size_t waitUntilQuiet(EventCollector& c,
                                 std::chrono::milliseconds quiet
                                     = std::chrono::milliseconds(120),
                                 std::chrono::milliseconds timeout
                                     = std::chrono::milliseconds(3000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        size_t last = c.size();
        auto lastChange = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const size_t now = c.size();
            if (now != last) {
                last = now;
                lastChange = std::chrono::steady_clock::now();
                continue;
            }
            if (std::chrono::steady_clock::now() - lastChange >= quiet) return last;
        }
        return c.size();
    }

    void recordTrack(int track, int blocks = 4, int capacityBlocks = 0) {
        if (capacityBlocks <= 0) capacityBlocks = blocks;
        wma_looper_set_enabled(mWma, true);
        ASSERT_EQ(wma_looper_prepare_track(mWma, track, capacityBlocks * kBlockFrames,
                                           kSampleRate),
                  WMA_OK);
        wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);
        render(4, kBlockFrames);
        wma_looper_start_recording(mWma, track);
        render(blocks, kBlockFrames);
        wma_looper_stop_recording(mWma);
        wma_set_frequency_amplitude(mWma, 440.0f, 0.0f);
        ASSERT_GT(wma_looper_get_track_length_frames(mWma, track), 0);
    }

    std::string mTempDir;

    /// Los receptores viven ACA para sobrevivir al motor. Ver installCollector().
    std::deque<EventCollector> mCollectors;
};

// ===========================================================================
// The C API drives the looper the engine actually renders
// ===========================================================================

TEST_F(CApiLooperTest, RecordingThroughTheCApiCapturesTheEngineOutput) {
    startAt(kSampleRate, /*fadeTimeMs=*/0);
    wma_looper_set_enabled(mWma, true);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 0, 4 * kBlockFrames, kSampleRate), WMA_OK);

    // Make the engine produce something worth recording.
    wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);
    render(4, kBlockFrames);
    ASSERT_GT(renderBlockPeak(kBlockFrames), kAudible)
        << "the synth is silent, so this test cannot tell recording from nothing";

    wma_looper_start_recording(mWma, 0);
    EXPECT_TRUE(wma_looper_is_recording(mWma));
    render(4, kBlockFrames);
    wma_looper_stop_recording(mWma);

    EXPECT_FALSE(wma_looper_is_recording(mWma));
    EXPECT_TRUE(wma_looper_is_track_active(mWma, 0))
        << "a track recorded through the C API should be active";
    EXPECT_GT(wma_looper_get_track_length_frames(mWma, 0), 0);
}

TEST_F(CApiLooperTest, AudibleRoundTrip) {
    startAt(kSampleRate, 0);
    recordTrack(0);

    // The first attempt at this test asserted silence right after killing the
    // oscillator, and failed at 0.6 — because the freshly recorded loop was
    // already playing. There is no way to prove "the synth is off" while the
    // thing under test is making noise, so the looper gets paused first. That
    // also puts pause/resume under test, which is a better test either way.
    wma_looper_pause(mWma);
    render(60, kBlockFrames);
    ASSERT_LT(renderBlockPeak(kBlockFrames), kAudible)
        << "with the oscillator silent and the looper paused, nothing should come out";

    // Resume: whatever comes out now is the loop, because nothing else is on.
    wma_looper_resume(mWma);
    float loudest = 0.0f;
    for (int i = 0; i < 12; ++i) {
        loudest = std::max(loudest, renderBlockPeak(kBlockFrames));
    }
    EXPECT_GT(loudest, kAudible)
        << "the recorded loop is not reaching the output — the C API and the audio "
           "callback may be looking at different AudioLooper instances";

    // And muting it through the C API silences that same playback.
    //
    // The mute is SMOOTHED — a one-pole at 0.995/sample, so ~0.28 gain left
    // after the first 256-frame block and ~3e-5 after eight. Measuring straight
    // after the call reads the ramp, not the mute: the first version of this
    // assertion failed at 0.61 for exactly that reason, with the code behaving
    // correctly. Let it settle, then measure.
    wma_looper_set_track_muted(mWma, 0, true);
    render(8, kBlockFrames);

    float mutedLoudest = 0.0f;
    for (int i = 0; i < 8; ++i) {
        mutedLoudest = std::max(mutedLoudest, renderBlockPeak(kBlockFrames));
    }
    EXPECT_LT(mutedLoudest, kAudible) << "muting through the C API did nothing";

    // Unmuting brings it back, so the silence above was the mute and not the
    // loop having simply run out.
    wma_looper_set_track_muted(mWma, 0, false);
    render(8, kBlockFrames);
    float unmutedLoudest = 0.0f;
    for (int i = 0; i < 8; ++i) {
        unmutedLoudest = std::max(unmutedLoudest, renderBlockPeak(kBlockFrames));
    }
    EXPECT_GT(unmutedLoudest, kAudible) << "unmuting did not restore the loop";
}

// ===========================================================================
// The loop region, which is 64-bit everywhere except where it used to be
// ===========================================================================

TEST_F(CApiLooperTest, TheLoopRegionRoundTrips) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/32);   // 8192 frames, room for a 1000..5000 region

    wma_looper_set_track_loop_region(mWma, 0, 1000, 5000);
    EXPECT_EQ(wma_looper_get_track_loop_start(mWma, 0), 1000);
    EXPECT_EQ(wma_looper_get_track_loop_end(mWma, 0), 5000);

    wma_looper_reset_track_loop_region(mWma, 0);
    EXPECT_EQ(wma_looper_get_track_loop_start(mWma, 0), 0);
}

TEST_F(CApiLooperTest, AnOutOfRangeLoopRegionSaturatesInsteadOfWrapping) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/32);

    // THE reason this signature is int64_t. AudioLooper::setTrackLoopRegion takes
    // int64_t and clamps into int32 itself, because TrackBuffer stores frames as
    // int32 while the API has to survive a long recording. The C API used to
    // declare `int`, which made it the one narrow link in a chain that is 64-bit
    // from Kotlin's Long down — the clamp could never run, because the value was
    // already truncated by the time it arrived.
    //
    // 0x1'0000'03E8 is 1000 once truncated to 32 bits. If the narrowing were back,
    // this would set a loop region starting at frame 1000 instead of saturating.
    constexpr int64_t kWrapsTo1000 = (int64_t{1} << 32) | 1000;
    wma_looper_set_track_loop_region(mWma, 0, kWrapsTo1000, kWrapsTo1000 + 4000);

    EXPECT_NE(wma_looper_get_track_loop_start(mWma, 0), 1000)
        << "a 64-bit frame index was truncated to 32 bits somewhere in the chain";
}

// ===========================================================================
// Round-trips through the C API — the setters reach the getters
// ===========================================================================

TEST_F(CApiLooperTest, MasterVolumeRoundTrips) {
    startAt(kSampleRate, 0);
    wma_looper_set_master_volume(mWma, 0.25f);
    EXPECT_FLOAT_EQ(wma_looper_get_master_volume(mWma), 0.25f);
}

TEST_F(CApiLooperTest, TrackSpeedRoundTrips) {
    startAt(kSampleRate, 0);
    recordTrack(0);

    wma_looper_set_track_speed(mWma, 0, 0.5f);
    EXPECT_FLOAT_EQ(wma_looper_get_track_speed(mWma, 0), 0.5f);
}

TEST_F(CApiLooperTest, PreparingATrackReportsMemoryFailureRatherThanSucceeding) {
    startAt(kSampleRate, 0);

    // The JNI used to map this by hand to JniError::MEMORY_ALLOCATION_FAILED.
    // JniError and WmaResult are numerically identical entry for entry, which is
    // what makes the delegation value-preserving for the Kotlin side — its
    // NativeErrorCode enum reads the raw int.
    static_assert(static_cast<int>(WMA_ERROR_MEMORY) == -6,
                  "NativeErrorCode.MEMORY_ALLOCATION_FAILED(-6) is wired to this value");
    static_assert(static_cast<int>(WMA_ERROR_NOT_INITIALIZED) == -1,
                  "NativeErrorCode.ENGINE_NOT_INITIALIZED(-1) is wired to this value");

    EXPECT_EQ(wma_looper_prepare_track(mWma, /*track=*/-1, 8000, kSampleRate),
              WMA_ERROR_MEMORY);
}

TEST_F(CApiLooperTest, ClearingATrackDeactivatesIt) {
    startAt(kSampleRate, 0);
    recordTrack(0);
    ASSERT_TRUE(wma_looper_is_track_active(mWma, 0));

    wma_looper_clear_track(mWma, 0);
    EXPECT_FALSE(wma_looper_is_track_active(mWma, 0));
}

TEST_F(CApiLooperTest, TheWaveformHonoursTheBufferItWasGiven) {
    startAt(kSampleRate, 0);

    // A bare pointer plus a count: the C API cannot know how big the buffer is,
    // so the count is the contract. Nothing past it may be touched.
    constexpr int kBins = 8;
    std::vector<float> bins(kBins + 4, -12345.0f);
    const int written = wma_looper_get_track_waveform(mWma, 0, bins.data(), kBins);

    EXPECT_LE(written, kBins);
    for (int i = kBins; i < kBins + 4; ++i) {
        EXPECT_FLOAT_EQ(bins[static_cast<size_t>(i)], -12345.0f)
            << "wrote past max_bins at index " << i;
    }
}

// ===========================================================================
// Export / import — batch 4
// ===========================================================================

TEST_F(CApiLooperTest, TheDefaultOptionsMatchTheEngineSideDefaults) {
    // wma_looper_export_options_default() hand-copies wm::ExportOptions' member
    // initialisers, because a C struct cannot inherit them. This is the assertion
    // that keeps the copy honest.
    const WmaExportOptions opts = wma_looper_export_options_default();
    const wm::ExportOptions reference;

    EXPECT_EQ(opts.repeat_loops, reference.repeatLoops);
    EXPECT_EQ(opts.apply_limiter, reference.applyLimiter);
    EXPECT_EQ(opts.count_in_beats, 0);
    EXPECT_EQ(reference.countInFrames, 0);
    EXPECT_EQ(opts.bit_depth, 16);
    EXPECT_EQ(reference.bitDepth, wav::BitDepth::PCM_16);
    EXPECT_EQ(opts.bpm, 0) << "0 means 'ask the Transport', not 0 BPM";
    EXPECT_EQ(opts.project_name, nullptr);
}

TEST_F(CApiLooperTest, ExportingAMixWritesAFileThatCanBeImportedBack) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/16);

    const std::string path = tempPath("mix.wav");
    WmaExportOptions opts = wma_looper_export_options_default();
    ASSERT_TRUE(wma_looper_export_mix_v2(mWma, path.c_str(), &opts)) << path;
    EXPECT_GT(fileSize(path), 44) << "a WAV header alone is 44 bytes; nothing was written";

    // The strongest thing this suite can say about an export: the engine can read
    // its own output back into a track.
    ASSERT_EQ(wma_looper_prepare_track(mWma, 1, 64 * kBlockFrames, kSampleRate), WMA_OK);
    EXPECT_TRUE(wma_looper_import_track(mWma, 1, path.c_str(), kSampleRate));
    EXPECT_GT(wma_looper_get_track_length_frames(mWma, 1), 0);
}

TEST_F(CApiLooperTest, NullOptionsMeanTheDefaults) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/8);

    const std::string withNull = tempPath("null-opts.wav");
    const std::string withDefaults = tempPath("explicit-defaults.wav");
    WmaExportOptions opts = wma_looper_export_options_default();

    ASSERT_TRUE(wma_looper_export_mix_v2(mWma, withNull.c_str(), nullptr));
    ASSERT_TRUE(wma_looper_export_mix_v2(mWma, withDefaults.c_str(), &opts));

    EXPECT_EQ(fileSize(withNull), fileSize(withDefaults))
        << "passing NULL must be the same request as passing the defaults";
}

TEST_F(CApiLooperTest, TheCountInIsResolvedThroughTheTransport) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/8);

    const std::string plain = tempPath("no-countin.wav");
    const std::string withCountIn = tempPath("countin.wav");

    WmaExportOptions opts = wma_looper_export_options_default();
    ASSERT_TRUE(wma_looper_export_mix_v2(mWma, plain.c_str(), &opts));

    // Four beats of leading silence at 24000 frames/beat is 96000 frames, so the
    // file has to grow by that much times 2 channels times the sample size. The
    // conversion is the Transport's, which is the composition this batch moved.
    opts.count_in_beats = 4;
    ASSERT_TRUE(wma_looper_export_mix_v2(mWma, withCountIn.c_str(), &opts));

    const long grew = fileSize(withCountIn) - fileSize(plain);
    const long expected = 4L * wma_transport_frames_per_beat(mWma) * 2 * 2;  // 16-bit stereo
    EXPECT_NEAR(static_cast<double>(grew), static_cast<double>(expected),
                static_cast<double>(expected) * 0.02)
        << "grew by " << grew << ", expected about " << expected;
}

TEST_F(CApiLooperTest, AnAbsurdCountInIsClampedInsteadOfOverflowing) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/8);

    // Fourth width problem of the category: `countInBeats * framesPerBeat()` was
    // int arithmetic in the JNI, and at 24000 frames/beat it overflows past ~89k
    // beats — a negative or wrapped countInFrames handed to the exporter. Now
    // computed in int64 and clamped, so the worst case is a refusal or a huge
    // file, never a wrapped length.
    WmaExportOptions opts = wma_looper_export_options_default();
    opts.count_in_beats = 1000000;

    // And the request is REFUSED, not attempted. Ojo con POR QUÉ, que cambió:
    // este test pasaba apoyado en UB. `LooperExporter` sumaba
    // `frames * repeats + countIn` en int, y con `countInFrames` ya clampeado a
    // INT32_MAX eso es overflow con signo; el wrap negativo hacía tirar a la
    // alocación y el borde de la C API devolvía false. Verde en un build normal,
    // y UBSan en CI lo destapó (LooperExporter.cpp:119, "signed integer overflow:
    // 2147483647 + 2048"). Ahora la cuenta va en int64 y el exporter rechaza
    // explícitamente lo que no entra en int32 — mismo false, sin UB, y sin
    // depender del ancho de `size_t` (en las ABIs de 32 bits truncaba).
    //
    // La otra propiedad que este test cubría —que una excepción no cruce la C
    // API— no se pierde: la cubre `ExportTelemetryCountsWhatHappened`, que
    // exporta a una ruta imposible y espera false sin que nada escape.
    EXPECT_FALSE(wma_looper_export_mix_v2(mWma, tempPath("huge-countin.wav").c_str(),
                                         &opts));

    // Still usable afterwards — a refused export must not leave the looper broken.
    opts.count_in_beats = 0;
    EXPECT_TRUE(wma_looper_export_mix_v2(mWma, tempPath("after-refusal.wav").c_str(),
                                        &opts));
}

TEST_F(CApiLooperTest, ABpmOfZeroMeansAskTheTransport) {
    startAt(kSampleRate, 0);
    wma_set_bpm(mWma, 140.0f);
    recordTrack(0, /*blocks=*/8);

    // The BPM lands in the WAV's ICMT comment as "BPM=<n>", which is how this is
    // observable at all without a metadata reader.
    WmaExportOptions opts = wma_looper_export_options_default();
    ASSERT_EQ(opts.bpm, 0);
    const std::string fromTransport = tempPath("bpm-transport.wav");
    ASSERT_TRUE(wma_looper_export_mix_v2(mWma, fromTransport.c_str(), &opts));
    EXPECT_NE(fileBytes(fromTransport).find("BPM=140"), std::string::npos)
        << "a bpm of 0 should have been resolved to the Transport's 140";

    // And an explicit value overrides it rather than being ignored.
    opts.bpm = 90;
    const std::string explicitBpm = tempPath("bpm-explicit.wav");
    ASSERT_TRUE(wma_looper_export_mix_v2(mWma, explicitBpm.c_str(), &opts));
    const std::string bytes = fileBytes(explicitBpm);
    EXPECT_NE(bytes.find("BPM=90"), std::string::npos);
    EXPECT_EQ(bytes.find("BPM=140"), std::string::npos);
}

TEST_F(CApiLooperTest, RepeatLoopsOfZeroMeansOneIteration) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/8);

    WmaExportOptions opts = wma_looper_export_options_default();
    const std::string once = tempPath("once.wav");
    ASSERT_TRUE(wma_looper_export_mix_v2(mWma, once.c_str(), &opts));

    // 0 and -3 both mean "one iteration", not "zero iterations" and not a
    // negative buffer length.
    //
    // Where that is ENFORCED is worth knowing: LooperExporter already does
    // std::max(1, opts.repeatLoops) at both of its use sites, so the C API's own
    // `(repeat_loops > 0) ? ... : 1` is belt-and-braces — mutating it away leaves
    // this test green. Second redundant guard found this way, after the memory
    // budget in wma_looper_set_capabilities(). The test still earns its place: it
    // pins the observable contract regardless of which layer happens to hold it
    // up, which is what a caller actually depends on.
    for (int repeats : {0, -3}) {
        opts.repeat_loops = repeats;
        const std::string path = tempPath("repeat" + std::to_string(repeats) + ".wav");
        ASSERT_TRUE(wma_looper_export_mix_v2(mWma, path.c_str(), &opts)) << repeats;
        EXPECT_EQ(fileSize(path), fileSize(once)) << "repeat_loops=" << repeats;
    }

    // And a real repeat count does grow the file, so the field is not simply
    // being ignored.
    opts.repeat_loops = 3;
    const std::string thrice = tempPath("thrice.wav");
    ASSERT_TRUE(wma_looper_export_mix_v2(mWma, thrice.c_str(), &opts));
    EXPECT_GT(fileSize(thrice), fileSize(once));
}

TEST_F(CApiLooperTest, BitDepthPicksTheFormatAndFallsBackToSixteen) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/8);

    WmaExportOptions opts = wma_looper_export_options_default();
    const auto sizeAt = [&](int bits) {
        opts.bit_depth = bits;
        const std::string path = tempPath("depth-" + std::to_string(bits) + ".wav");
        EXPECT_TRUE(wma_looper_export_mix_v2(mWma, path.c_str(), &opts));
        return fileSize(path);
    };

    const long at16 = sizeAt(16);
    const long at24 = sizeAt(24);
    const long at32 = sizeAt(32);
    EXPECT_GT(at24, at16) << "24-bit samples are wider than 16-bit ones";
    EXPECT_GT(at32, at24);

    // Anything that is not 24 or 32 is 16 — the JNI's default arm, now in one
    // place instead of three.
    EXPECT_EQ(sizeAt(99), at16);
    EXPECT_EQ(sizeAt(0), at16);
}

TEST_F(CApiLooperTest, CapturingATrackWritesItsWholeBufferAtTheGivenDepth) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/16);
    // A loop region over part of the take: capture must ignore it and write the
    // whole buffer, which is what makes it "session capture" and not an export.
    wma_looper_set_track_loop_region(mWma, 0, 0, 2048);

    const std::string path = tempPath("capture.wav");
    ASSERT_TRUE(wma_looper_capture_track(mWma, 0, path.c_str(), /*bit_depth=*/32));

    const long expectedSamples =
        static_cast<long>(wma_looper_get_track_length_frames(mWma, 0)) * 2 * 4;  // float32
    EXPECT_GT(fileSize(path), expectedSamples / 2)
        << "the capture looks like it honoured the 2048-frame loop region";
}

TEST_F(CApiLooperTest, ExportingStemsWritesOnePerActiveTrack) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/8);
    recordTrack(1, /*blocks=*/8);

    const std::string dir = tempDir();
    WmaExportOptions opts = wma_looper_export_options_default();
    const int written = wma_looper_export_stems(mWma, dir.c_str(), &opts);

    EXPECT_EQ(written, 2) << "two active tracks should produce two stems";
    EXPECT_EQ(wma_looper_get_stems_written(mWma), 2);
}

TEST_F(CApiLooperTest, ExportTelemetryCountsWhatHappened) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/8);
    ASSERT_EQ(wma_looper_get_exports_completed(mWma), 0);

    WmaExportOptions opts = wma_looper_export_options_default();
    ASSERT_TRUE(wma_looper_export_mix_v2(mWma, tempPath("counted.wav").c_str(), &opts));
    EXPECT_EQ(wma_looper_get_exports_completed(mWma), 1);
    EXPECT_EQ(wma_looper_get_exports_failed(mWma), 0);

    // A path that cannot be written counts as a failure, not a success.
    EXPECT_FALSE(wma_looper_export_mix_v2(
        mWma, "/this/directory/does/not/exist/nope.wav", &opts));
    EXPECT_EQ(wma_looper_get_exports_failed(mWma), 1);
    EXPECT_EQ(wma_looper_get_exports_completed(mWma), 1);
}

TEST_F(CApiLooperTest, NothingIsExportingWhenIdle) {
    startAt(kSampleRate, 0);
    EXPECT_FALSE(wma_looper_is_export_in_progress(mWma));
    EXPECT_FLOAT_EQ(wma_looper_get_export_progress(mWma), 0.0f);

    // Cancelling with nothing in flight is a no-op, not an error.
    wma_looper_cancel_export(mWma);
    EXPECT_FALSE(wma_looper_is_export_in_progress(mWma));
}

TEST_F(CApiLooperTest, TheExportSampleRateIsHonoured) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/8);

    WmaExportOptions opts = wma_looper_export_options_default();
    const std::string at48 = tempPath("at48.wav");
    ASSERT_TRUE(wma_looper_export_mix_v2(mWma, at48.c_str(), &opts));

    // Half the rate, same take: about half the samples.
    wma_looper_set_export_sample_rate(mWma, 24000);
    const std::string at24 = tempPath("at24.wav");
    ASSERT_TRUE(wma_looper_export_mix_v2(mWma, at24.c_str(), &opts));

    EXPECT_LT(fileSize(at24), fileSize(at48))
        << "exporting at 24 kHz produced a file no smaller than 48 kHz";
}

// ===========================================================================
// Track editing & analysis — batch 3
// ===========================================================================

TEST_F(CApiLooperTest, PreparingByBarsReturnsTheLengthTheTransportImplies) {
    startAt(kSampleRate, 0);
    wma_transport_set_beats_per_bar(mWma, 4);   // 4 beats x 24000 = 96000 frames/bar

    const int frames = wma_looper_prepare_track_bars(mWma, 0, /*bars=*/2, kSampleRate);
    const int framesPerBar = wma_transport_frames_per_bar(mWma, 1);
    ASSERT_GT(framesPerBar, 0);

    EXPECT_EQ(frames, 2 * framesPerBar)
        << "the reported length has to be what the Transport says two bars are";
    EXPECT_GE(wma_looper_get_track_length_frames(mWma, 0), 0);
}

TEST_F(CApiLooperTest, PreparingByBarsRefusesNonsenseInsteadOfWrapping) {
    startAt(kSampleRate, 0);

    EXPECT_EQ(wma_looper_prepare_track_bars(mWma, 0, /*bars=*/0, kSampleRate), -1);
    EXPECT_EQ(wma_looper_prepare_track_bars(mWma, 0, /*bars=*/-4, kSampleRate), -1);

    // Third width problem of this category. `bars * framesPerBar` is int
    // arithmetic in AudioLooper too, and prepareTrack only rejects a NON-POSITIVE
    // length — so a bar count big enough to wrap into a SMALL POSITIVE allocates a
    // tiny track and reports the wrapped number as the length.
    //
    // 44740 is the value that isolates this. At 96000 frames/bar the product is
    // 4,295,040,000, which wraps to 72,704 — small, positive, and allocatable, so
    // the buggy version happily answers "72704 frames" to a 44740-bar request.
    // The first version of this test used 100000 bars, whose product wraps to
    // 1,010,065,408; that is positive too, but the ~8 GB allocation fails and the
    // test passed on the allocation failure rather than on the guard. Verified by
    // mutating the guard away: with 100000 the test stayed green.
    EXPECT_EQ(wma_looper_prepare_track_bars(mWma, 0, /*bars=*/44740, kSampleRate), -1)
        << "a bar count that overflows int32 must be refused, not wrapped";
    EXPECT_EQ(wma_looper_prepare_track_bars(mWma, 0, /*bars=*/100000, kSampleRate), -1);
}

TEST_F(CApiLooperTest, ContentBoundsComeBackAsTwoPlainInts) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/16);

    int first = -1, last = -1;
    ASSERT_TRUE(wma_looper_find_content_bounds(mWma, 0, 0.01f, &first, &last));

    // The JNI packs these into one jlong because a JNI call cannot return two
    // ints. The C API does not have that problem and should not inherit the
    // encoding — least of all a sign-sensitive one.
    EXPECT_GE(first, 0);
    EXPECT_GT(last, first) << "a recorded track should have audible content";
    EXPECT_LE(last, wma_looper_get_track_length_frames(mWma, 0));
}

TEST_F(CApiLooperTest, ContentBoundsRefusesRatherThanWritingGarbage) {
    startAt(kSampleRate, 0);
    int first = -7, last = -7;

    EXPECT_FALSE(wma_looper_find_content_bounds(nullptr, 0, 0.01f, &first, &last));
    EXPECT_EQ(first, -7) << "the out-params must be left alone on failure";
    EXPECT_EQ(last, -7);

    EXPECT_FALSE(wma_looper_find_content_bounds(mWma, 0, 0.01f, nullptr, &last));
    EXPECT_FALSE(wma_looper_find_content_bounds(mWma, 0, 0.01f, &first, nullptr));
}

TEST_F(CApiLooperTest, OnsetDetectionHonoursTheBufferAndNeverReportsNegative) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/16);

    constexpr int kMax = 8;
    std::vector<int> onsets(kMax + 4, -999);
    const int n = wma_looper_detect_onsets(mWma, 0, onsets.data(), kMax,
                                           /*hop_frames=*/256, /*sensitivity=*/0.5f);

    // NOT covered, and said so rather than dressed up: the C API clamps a negative
    // count to 0, carried over from the JNI, but TrackBuffer::detectOnsets cannot
    // produce one — every early exit is `return 0` and the counting path only ever
    // increments. Mutating the clamp away leaves this suite green, which is how
    // that was established. The assertion below is real, it just cannot fail for
    // the reason the clamp exists.
    EXPECT_GE(n, 0);
    EXPECT_LE(n, kMax);
    for (int i = kMax; i < kMax + 4; ++i) {
        EXPECT_EQ(onsets[static_cast<size_t>(i)], -999) << "wrote past max_onsets at " << i;
    }
}

TEST_F(CApiLooperTest, OnsetDetectionRefusesAnEmptyRequest) {
    startAt(kSampleRate, 0);
    int one = 0;
    EXPECT_EQ(wma_looper_detect_onsets(mWma, 0, &one, 0, 256, 0.5f), 0);
    EXPECT_EQ(wma_looper_detect_onsets(mWma, 0, nullptr, 8, 256, 0.5f), 0);
}

TEST_F(CApiLooperTest, TrimmingFreesSpareCapacityWithoutTouchingTheRecording) {
    startAt(kSampleRate, 0);
    // Reserve 64 blocks, record 8: 56 blocks of spare capacity for trim to free.
    // A track recorded right up to its capacity has nothing to trim and trim
    // reports false for it — see the test below. Learned by writing this one the
    // other way round first.
    recordTrack(0, /*blocks=*/8, /*capacityBlocks=*/64);
    const int lengthBefore = wma_looper_get_track_length_frames(mWma, 0);

    EXPECT_TRUE(wma_looper_trim_track(mWma, 0));
    EXPECT_TRUE(wma_looper_is_track_active(mWma, 0)) << "trimming must not clear the track";
    EXPECT_EQ(wma_looper_get_track_length_frames(mWma, 0), lengthBefore)
        << "trim frees spare capacity, it does not shorten the recording";
}

TEST_F(CApiLooperTest, TrimmingATrackWithNothingSpareReportsFalse) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/16);   // recorded right up to capacity

    // "Returns true if trimmed" — not "true if the call was valid". Worth pinning
    // because false here reads like an error and is not one.
    EXPECT_FALSE(wma_looper_trim_track(mWma, 0));
    EXPECT_TRUE(wma_looper_is_track_active(mWma, 0));
}

TEST_F(CApiLooperTest, TrimmingRefusesWhileRecordingIntoTheSameTrack) {
    startAt(kSampleRate, 0);
    wma_looper_set_enabled(mWma, true);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 0, 64 * kBlockFrames, kSampleRate), WMA_OK);
    wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);
    wma_looper_start_recording(mWma, 0);
    render(4, kBlockFrames);

    EXPECT_FALSE(wma_looper_trim_track(mWma, 0))
        << "freeing the buffer under the writer would be a use-after-free";
}

TEST_F(CApiLooperTest, FinalizingAFreeLoopSetsTheRegion) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/32);

    ASSERT_TRUE(wma_looper_finalize_free_loop(mWma, 0, /*start=*/0, /*end=*/4096,
                                              /*tail=*/0));
    EXPECT_EQ(wma_looper_get_track_loop_start(mWma, 0), 0);
    EXPECT_EQ(wma_looper_get_track_loop_end(mWma, 0), 4096);
}

TEST_F(CApiLooperTest, FinalizingRefusesWhileRecordingIntoTheSameTrack) {
    startAt(kSampleRate, 0);
    wma_looper_set_enabled(mWma, true);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 0, 64 * kBlockFrames, kSampleRate), WMA_OK);
    wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);
    wma_looper_start_recording(mWma, 0);
    render(4, kBlockFrames);

    EXPECT_FALSE(wma_looper_finalize_free_loop(mWma, 0, 0, 4096, 0))
        << "rewriting a track's loop region mid-take would race the writer";
}

TEST_F(CApiLooperTest, PercussionModeRoundTrips) {
    startAt(kSampleRate, 0);
    recordTrack(0);

    EXPECT_FALSE(wma_looper_is_track_percussion_mode(mWma, 0));
    wma_looper_set_track_percussion_mode(mWma, 0, true);
    EXPECT_TRUE(wma_looper_is_track_percussion_mode(mWma, 0));
    wma_looper_set_track_percussion_mode(mWma, 0, false);
    EXPECT_FALSE(wma_looper_is_track_percussion_mode(mWma, 0));
}

TEST_F(CApiLooperTest, TheTailWindowRoundTrips) {
    startAt(kSampleRate, 0);
    const int original = wma_looper_get_tail_ms(mWma);

    wma_looper_set_tail_ms(mWma, original + 50);
    EXPECT_EQ(wma_looper_get_tail_ms(mWma), original + 50);
}

TEST_F(CApiLooperTest, CapabilitiesLeaveAloneWhateverIsNotAskedFor) {
    startAt(kSampleRate, 0);

    // The ceiling is observable: track 2 is beyond a limit of 2, so preparing it
    // must fail while track 0 still works.
    wma_looper_set_capabilities(mWma, /*budget=*/0, /*max_tracks=*/2, /*max_free_s=*/0);
    EXPECT_EQ(wma_looper_prepare_track(mWma, 2, 4 * kBlockFrames, kSampleRate),
              WMA_ERROR_MEMORY);
    EXPECT_EQ(wma_looper_prepare_track(mWma, 0, 4 * kBlockFrames, kSampleRate), WMA_OK);
}

TEST_F(CApiLooperTest, SettingOnlyTheBudgetLeavesTheTrackCeilingAlone) {
    startAt(kSampleRate, 0);

    // The load-bearing half of "0 means leave it alone". AudioLooper::setCapabilities
    // clamps maxActiveTracks into [1, 16], so passing the struct through with a
    // zero would silently cut the device down to ONE usable track. The default
    // lives in LooperCapabilities' member initialiser, and only overriding when
    // the caller asked for something is what preserves it.
    wma_looper_set_capabilities(mWma, /*budget=*/8 * 1024 * 1024, /*max_tracks=*/0,
                                /*max_free_s=*/0);

    EXPECT_EQ(wma_looper_prepare_track(mWma, 1, 4 * kBlockFrames, kSampleRate), WMA_OK)
        << "asking only for a memory budget collapsed the track ceiling";
    EXPECT_EQ(wma_looper_prepare_track(mWma, 2, 4 * kBlockFrames, kSampleRate), WMA_OK);
}

TEST_F(CApiLooperTest, AZeroBudgetIsAlreadyHandledDownstream) {
    startAt(kSampleRate, 0);

    // Characterization of a REDUNDANT guard, so the next reader does not take the
    // whole "0 means leave alone" block as uniformly load-bearing: for the memory
    // budget specifically, AudioLooper::setCapabilities already substitutes
    // DEFAULT_MEMORY_BUDGET_BYTES when it receives a zero. Removing the C API's
    // `if (budget_bytes > 0)` changes nothing observable — established by mutating
    // it away and watching this suite stay green. It is kept for symmetry with the
    // other two fields, where the guard IS what preserves the default.
    wma_looper_set_capabilities(mWma, /*budget=*/0, /*max_tracks=*/0, /*max_free_s=*/0);
    EXPECT_EQ(wma_looper_prepare_track(mWma, 0, 4 * kBlockFrames, kSampleRate), WMA_OK);
}

TEST_F(CApiLooperTest, AbortingARecordingDiscardsTheTake) {
    startAt(kSampleRate, 0);
    wma_looper_set_enabled(mWma, true);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 0, 64 * kBlockFrames, kSampleRate), WMA_OK);
    wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);

    wma_looper_start_recording(mWma, 0);
    render(4, kBlockFrames);
    ASSERT_TRUE(wma_looper_is_recording(mWma));

    wma_looper_abort_recording(mWma);
    EXPECT_FALSE(wma_looper_is_recording(mWma));
    EXPECT_FALSE(wma_looper_is_track_active(mWma, 0))
        << "abort throws the take away; stop would have kept it";
}

TEST_F(CApiLooperTest, PreRollSeedsTheTakeWithAudioFromBeforeTheButton) {
    startAt(kSampleRate, 0);
    wma_looper_set_enabled(mWma, true);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 0, 128 * kBlockFrames, kSampleRate), WMA_OK);

    // Fill the pre-roll ring with audible output, then start recording with a
    // pre-roll while the engine is SILENT. Anything in the track can only have
    // come from the ring.
    wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);
    render(60, kBlockFrames);
    wma_set_frequency_amplitude(mWma, 440.0f, 0.0f);
    render(60, kBlockFrames);
    ASSERT_LT(renderBlockPeak(kBlockFrames), kAudible);

    wma_looper_start_recording_with_pre_roll(mWma, 0, /*pre_roll_ms=*/100);
    render(2, kBlockFrames);
    wma_looper_stop_recording(mWma);

    ASSERT_TRUE(wma_looper_is_track_active(mWma, 0));
    // 100 ms at 48 kHz is 4800 frames of seed, so the take is far longer than the
    // two blocks that were actually rendered while recording.
    EXPECT_GT(wma_looper_get_track_length_frames(mWma, 0), 4000)
        << "the pre-roll seed did not make it into the take";
}

TEST_F(CApiLooperTest, PreRollOfZeroIsAPlainStart) {
    startAt(kSampleRate, 0);
    wma_looper_set_enabled(mWma, true);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 0, 128 * kBlockFrames, kSampleRate), WMA_OK);
    wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);
    render(60, kBlockFrames);

    wma_looper_start_recording_with_pre_roll(mWma, 0, 0);
    EXPECT_TRUE(wma_looper_is_recording(mWma));
    render(2, kBlockFrames);
    wma_looper_stop_recording(mWma);

    // No seed: the take is only what was rendered while recording.
    EXPECT_LT(wma_looper_get_track_length_frames(mWma, 0), 4000);
}

TEST_F(CApiLooperTest, PreRollIsClampedToWhatTheRingHolds) {
    startAt(kSampleRate, 0);
    wma_looper_set_enabled(mWma, true);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 0, 256 * kBlockFrames, kSampleRate), WMA_OK);
    wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);
    render(60, kBlockFrames);

    // The ring holds 1 s. Asking for an hour must not read past it.
    wma_looper_start_recording_with_pre_roll(mWma, 0, /*pre_roll_ms=*/3600000);
    render(2, kBlockFrames);
    wma_looper_stop_recording(mWma);

    EXPECT_LE(wma_looper_get_track_length_frames(mWma, 0), kSampleRate + 4 * kBlockFrames)
        << "the seed exceeded one second of pre-roll";

    // And a negative request is a plain start, not a huge unsigned one. A fresh
    // track rather than clearing this one: clearTrack() releases the buffer, so a
    // cleared track cannot be recorded into again without preparing it.
    wma_looper_stop_recording(mWma);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 1, 64 * kBlockFrames, kSampleRate), WMA_OK);
    wma_looper_start_recording_with_pre_roll(mWma, 1, -5000);
    EXPECT_TRUE(wma_looper_is_recording(mWma));
    wma_looper_abort_recording(mWma);
}

// ===========================================================================
// Armed recording — batch 2
// ===========================================================================

TEST_F(CApiLooperTest, NothingIsArmedToStartWith) {
    startAt(kSampleRate, 0);
    EXPECT_EQ(wma_looper_get_armed_track(mWma), -1);
}

TEST_F(CApiLooperTest, ArmingInFramesReportsTheTriggerAndArmsTheTrack) {
    startAt(kSampleRate, 0);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 1, 8 * kBlockFrames, kSampleRate), WMA_OK);

    const int64_t trigger = wma_looper_arm_in_frames(mWma, 1, 5000);
    EXPECT_GE(trigger, 5000) << "the trigger is the play position plus the offset";
    EXPECT_EQ(wma_looper_get_armed_track(mWma), 1);
}

TEST_F(CApiLooperTest, ANegativeOffsetIsTreatedAsNow) {
    startAt(kSampleRate, 0);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 0, 8 * kBlockFrames, kSampleRate), WMA_OK);

    const int64_t trigger = wma_looper_arm_in_frames(mWma, 0, -1000);
    EXPECT_GE(trigger, 0) << "a negative offset must not produce a trigger in the past";
    EXPECT_EQ(wma_looper_get_armed_track(mWma), 0);
}

TEST_F(CApiLooperTest, ArmingAnUnpreparedTrackReportsFailureInsteadOfATrigger) {
    startAt(kSampleRate, 0);

    // THE behaviour change of this batch. AudioLooper::armRecording is void and
    // no-ops on a track with no capacity, so the JNI happily returned a positive
    // trigger frame for a recording that was never armed — while its own doc
    // comment promised "-1 on failure". A UI counting down to that frame would
    // count down to nothing at all.
    EXPECT_EQ(wma_looper_arm_in_frames(mWma, 0, 5000), -1)
        << "an unprepared track cannot be armed, and must say so";
    EXPECT_EQ(wma_looper_arm_at_next_bar(mWma, 0), -1);
    EXPECT_EQ(wma_looper_get_armed_track(mWma), -1);

    // Out-of-range indices go the same way.
    EXPECT_EQ(wma_looper_arm_in_frames(mWma, 99, 5000), -1);
    EXPECT_EQ(wma_looper_arm_in_frames(mWma, -1, 5000), -1);
}

TEST_F(CApiLooperTest, ArmingAtTheNextBarLandsOnABarBoundary) {
    startAt(kSampleRate, 0);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 0, 8 * kBlockFrames, kSampleRate), WMA_OK);
    wma_transport_set_beats_per_bar(mWma, 4);

    const int64_t trigger = wma_looper_arm_at_next_bar(mWma, 0);
    ASSERT_GE(trigger, 0);

    // This is the composition the C API took over from the JNI: the Transport's
    // bar grid decides the frame, the looper gets armed at it.
    const int64_t framesPerBar = wma_transport_frames_per_bar(mWma, 1);
    ASSERT_GT(framesPerBar, 0);
    EXPECT_EQ(trigger % framesPerBar, 0) << "trigger " << trigger << " is not on a barline";
}

TEST_F(CApiLooperTest, TheArmedTriggerFiresAndTurnsIntoARecording) {
    startAt(kSampleRate, 0);
    wma_looper_set_enabled(mWma, true);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 0, 64 * kBlockFrames, kSampleRate), WMA_OK);
    wma_set_frequency_amplitude(mWma, 440.0f, 1.0f);

    // Two blocks out, so it fires while we are still rendering.
    ASSERT_GE(wma_looper_arm_in_frames(mWma, 0, 2 * kBlockFrames), 0);
    ASSERT_EQ(wma_looper_get_armed_track(mWma), 0);
    ASSERT_FALSE(wma_looper_is_recording(mWma));

    render(6, kBlockFrames);

    EXPECT_TRUE(wma_looper_is_recording(mWma)) << "the armed trigger never fired";
    EXPECT_EQ(wma_looper_get_armed_track(mWma), -1) << "the arm should be consumed";
    EXPECT_GE(wma_looper_get_armed_triggered(mWma), 1) << "telemetry did not count the fire";
}

TEST_F(CApiLooperTest, CancellingAnArmKeepsTheTriggerFromFiring) {
    startAt(kSampleRate, 0);
    wma_looper_set_enabled(mWma, true);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 0, 64 * kBlockFrames, kSampleRate), WMA_OK);

    ASSERT_GE(wma_looper_arm_in_frames(mWma, 0, 2 * kBlockFrames), 0);
    wma_looper_cancel_arm(mWma);
    EXPECT_EQ(wma_looper_get_armed_track(mWma), -1);

    render(8, kBlockFrames);
    EXPECT_FALSE(wma_looper_is_recording(mWma)) << "a cancelled arm still fired";
}

TEST_F(CApiLooperTest, SyncArmingWithNoReferenceLoopSaysSoRatherThanGuessing) {
    startAt(kSampleRate, 0);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 0, 8 * kBlockFrames, kSampleRate), WMA_OK);

    // Documented contract: -1 when there is no reference track playing, so the
    // caller can fall back to a plain latency-armed start.
    EXPECT_EQ(wma_looper_arm_synced_to_loop(mWma, 0, 480), -1);
    EXPECT_EQ(wma_looper_arm_synced_to_loop_quantized(mWma, 0, 480, 12000), -1);
}

TEST_F(CApiLooperTest, SyncArmingPhaseLocksToATrackThatIsPlaying) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/16);
    ASSERT_TRUE(wma_looper_is_track_playing(mWma, 0));
    ASSERT_EQ(wma_looper_prepare_track(mWma, 1, 64 * kBlockFrames, kSampleRate), WMA_OK);

    const int64_t trigger = wma_looper_arm_synced_to_loop(mWma, 1, 480);
    EXPECT_GE(trigger, 0) << "track 0 is playing, so there is a reference to lock to";
    EXPECT_EQ(wma_looper_get_armed_track(mWma), 1);
}

TEST_F(CApiLooperTest, ANegativeSyncLatencyIsTreatedAsZero) {
    startAt(kSampleRate, 0);
    recordTrack(0, /*blocks=*/16);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 1, 64 * kBlockFrames, kSampleRate), WMA_OK);

    const int64_t withNegative = wma_looper_arm_synced_to_loop(mWma, 1, -5000);
    wma_looper_cancel_arm(mWma);
    const int64_t withZero = wma_looper_arm_synced_to_loop(mWma, 1, 0);

    EXPECT_EQ(withNegative, withZero);
}

// ===========================================================================
// Telemetry — batch 2
// ===========================================================================

TEST_F(CApiLooperTest, TelemetryStartsAtZeroAndResets) {
    startAt(kSampleRate, 0);
    EXPECT_EQ(wma_looper_get_armed_triggered(mWma), 0);
    EXPECT_EQ(wma_looper_get_frames_dropped(mWma), 0);
    EXPECT_EQ(wma_looper_get_dropped_events(mWma), 0);

    wma_looper_set_enabled(mWma, true);
    ASSERT_EQ(wma_looper_prepare_track(mWma, 0, 64 * kBlockFrames, kSampleRate), WMA_OK);
    ASSERT_GE(wma_looper_arm_in_frames(mWma, 0, kBlockFrames), 0);
    render(4, kBlockFrames);
    ASSERT_GE(wma_looper_get_armed_triggered(mWma), 1);

    wma_looper_reset_telemetry(mWma);
    EXPECT_EQ(wma_looper_get_armed_triggered(mWma), 0) << "reset did not clear the counter";
}

TEST_F(CApiLooperTest, DroppedEventsComesFromTheDispatcherNotTheLooper) {
    startAt(kSampleRate, 0);

    // Two different objects behind one section of the API: this counter is about
    // the event queue overflowing, not about audio. resetTelemetry() belongs to
    // the looper and does NOT clear it — pinned here so nobody "tidies" that up
    // into a single reset and quietly loses the distinction.
    EXPECT_EQ(wma_looper_get_dropped_events(mWma), 0);
    wma_looper_reset_telemetry(mWma);
    EXPECT_EQ(wma_looper_get_dropped_events(mWma), 0);
}

// ===========================================================================
// Null handle — every value the JNI used to return by hand
// ===========================================================================
//
// This is the load-bearing test of the batch. Thirty-eight of these defaults
// were written out longhand in jni_audio_bridge.cpp and now live in the C API.
// Each line below is one of them, at the value the JNI returned.

TEST(CApiLooperNullHandle, EveryQueryReturnsTheValueTheJniUsedToReturnByHand) {
    EXPECT_EQ(wma_looper_prepare_track(nullptr, 0, 8000, 48000), WMA_ERROR_NOT_INITIALIZED);

    EXPECT_FLOAT_EQ(wma_looper_get_record_progress(nullptr), 0.0f);
    EXPECT_FLOAT_EQ(wma_looper_get_progress(nullptr), 0.0f);
    EXPECT_FLOAT_EQ(wma_looper_get_track_progress(nullptr, 0), 0.0f);
    EXPECT_FLOAT_EQ(wma_looper_get_track_peak(nullptr, 0), 0.0f);

    // 1.0f, not 0.0f: silence is not the right answer for a volume or a speed,
    // and the JNI was careful about it. Worth pinning precisely because a
    // zero-initialised default would look plausible.
    EXPECT_FLOAT_EQ(wma_looper_get_master_volume(nullptr), 1.0f);
    EXPECT_FLOAT_EQ(wma_looper_get_track_speed(nullptr, 0), 1.0f);

    EXPECT_EQ(wma_looper_get_master_loop_frames(nullptr), 0);
    EXPECT_EQ(wma_looper_get_track_length_frames(nullptr, 0), 0);
    EXPECT_EQ(wma_looper_get_track_loop_start(nullptr, 0), 0);
    EXPECT_EQ(wma_looper_get_track_loop_end(nullptr, 0), 0);

    EXPECT_FALSE(wma_looper_is_playing(nullptr));
    EXPECT_FALSE(wma_looper_is_recording(nullptr));
    EXPECT_FALSE(wma_looper_is_track_active(nullptr, 0));
    EXPECT_FALSE(wma_looper_is_track_playing(nullptr, 0));
    EXPECT_FALSE(wma_looper_has_undo(nullptr, 0));
    EXPECT_FALSE(wma_looper_save_undo(nullptr, 0));
    EXPECT_FALSE(wma_looper_restore_undo(nullptr, 0));

    EXPECT_FALSE(wma_looper_export_mix(nullptr, "/tmp/nope.wav"));
    EXPECT_FALSE(wma_looper_export_track(nullptr, 0, "/tmp/nope.wav"));
    EXPECT_FALSE(wma_looper_import_track(nullptr, 0, "/tmp/nope.wav", 48000));

    float bins[4];
    EXPECT_EQ(wma_looper_get_track_waveform(nullptr, 0, bins, 4), 0);

    // Batch 2. -1 for the arm calls and for the armed track, 0 for counters.
    EXPECT_EQ(wma_looper_arm_at_next_bar(nullptr, 0), -1);
    EXPECT_EQ(wma_looper_arm_in_frames(nullptr, 0, 1000), -1);
    EXPECT_EQ(wma_looper_arm_synced_to_loop(nullptr, 0, 480), -1);
    EXPECT_EQ(wma_looper_arm_synced_to_loop_quantized(nullptr, 0, 480, 1200), -1);
    EXPECT_EQ(wma_looper_get_armed_track(nullptr), -1);

    EXPECT_EQ(wma_looper_get_armed_triggered(nullptr), 0);
    EXPECT_EQ(wma_looper_get_frames_dropped(nullptr), 0);
    EXPECT_EQ(wma_looper_get_dropped_events(nullptr), 0);

    // Batch 3.
    EXPECT_EQ(wma_looper_prepare_track_bars(nullptr, 0, 2, 48000), -1);
    EXPECT_FALSE(wma_looper_trim_track(nullptr, 0));
    EXPECT_FALSE(wma_looper_finalize_free_loop(nullptr, 0, 0, 1000, 0));
    EXPECT_FALSE(wma_looper_is_track_percussion_mode(nullptr, 0));
    EXPECT_EQ(wma_looper_get_tail_ms(nullptr), 0);
    int onset = 0;
    EXPECT_EQ(wma_looper_detect_onsets(nullptr, 0, &onset, 1, 256, 0.5f), 0);

    // Batch 4.
    WmaExportOptions opts = wma_looper_export_options_default();
    EXPECT_FALSE(wma_looper_export_mix_v2(nullptr, "/tmp/nope.wav", &opts));
    EXPECT_EQ(wma_looper_export_stems(nullptr, "/tmp", &opts), -1);
    EXPECT_FALSE(wma_looper_capture_track(nullptr, 0, "/tmp/nope.wav", 16));
    EXPECT_FLOAT_EQ(wma_looper_get_export_progress(nullptr), 0.0f);
    EXPECT_FALSE(wma_looper_is_export_in_progress(nullptr));
    EXPECT_EQ(wma_looper_get_exports_completed(nullptr), 0);
    EXPECT_EQ(wma_looper_get_exports_failed(nullptr), 0);
    EXPECT_EQ(wma_looper_get_stems_written(nullptr), 0);
}

TEST(CApiLooperNullHandle, ANullPathIsRefusedRatherThanDereferenced) {
    // The JNI could not hit this — GetStringUTFChars on a non-null jstring always
    // gives a pointer — but iOS calls these directly, so the guard is real.
    EXPECT_FALSE(wma_looper_export_mix(nullptr, nullptr));
    EXPECT_FALSE(wma_looper_export_track(nullptr, 0, nullptr));
    EXPECT_FALSE(wma_looper_import_track(nullptr, 0, nullptr, 48000));

    WmaExportOptions opts = wma_looper_export_options_default();
    EXPECT_FALSE(wma_looper_export_mix_v2(nullptr, nullptr, &opts));
    EXPECT_EQ(wma_looper_export_stems(nullptr, nullptr, &opts), -1);
    EXPECT_FALSE(wma_looper_capture_track(nullptr, 0, nullptr, 16));
}

TEST(CApiLooperNullHandle, EveryMutatorIsANoOpRatherThanACrash) {
    wma_looper_start_recording(nullptr, 0);
    wma_looper_stop_recording(nullptr);
    wma_looper_start_overdub(nullptr, 0);
    wma_looper_stop_all(nullptr);
    wma_looper_pause(nullptr);
    wma_looper_resume(nullptr);
    wma_looper_pause_track(nullptr, 0);
    wma_looper_resume_track(nullptr, 0);
    wma_looper_clear_track(nullptr, 0);
    wma_looper_clear_all(nullptr);
    wma_looper_set_enabled(nullptr, true);
    wma_looper_set_free_length(nullptr, true);
    wma_looper_set_master_volume(nullptr, 0.5f);
    wma_looper_set_track_muted(nullptr, 0, true);
    wma_looper_set_track_pan(nullptr, 0, 0.5f);
    wma_looper_set_track_volume(nullptr, 0, 0.5f);
    wma_looper_set_track_speed(nullptr, 0, 0.5f);
    wma_looper_set_track_loop_region(nullptr, 0, 0, 100);
    wma_looper_reset_track_loop_region(nullptr, 0);
    wma_looper_reset_track_playhead(nullptr, 0);
    wma_looper_trigger_click(nullptr, true);
    wma_looper_cancel_arm(nullptr);
    wma_looper_reset_telemetry(nullptr);
    wma_looper_abort_recording(nullptr);
    wma_looper_start_recording_with_pre_roll(nullptr, 0, 100);
    wma_looper_set_track_play_count(nullptr, 0, 2);
    wma_looper_set_track_percussion_mode(nullptr, 0, true);
    wma_looper_set_tail_ms(nullptr, 50);
    wma_looper_set_capabilities(nullptr, 1024, 2, 30);
    wma_looper_cancel_export(nullptr);
    wma_looper_set_export_sample_rate(nullptr, 48000);
    SUCCEED();
}


// ===========================================================================
// State events (push) — wma_looper_set_event_callback
//
// La única superficie de la C API por la que el motor llama HACIA AFUERA, y por
// eso la que más necesita un test: el resto se puede verificar preguntando.
//
// La cadena que se ejercita acá es la real y completa: `render()` empuja bloques
// por `onAudioReady`, o sea el thread de audio, que hace `pushFromRT()` a la cola
// lock-free; el worker del despachador la drena cada ~15 ms y desde ese hilo
// —NO el de audio, y NO el del test— invoca el callback. Un test que llamara al
// sink a mano no probaría nada de eso.
// ===========================================================================

TEST_F(CApiLooperTest, StateEventsReachTheCallbackFromTheWorkerThread) {
    startAt(kSampleRate, 0);
    EventCollector& collector = installCollector();

    recordTrack(0, /*blocks=*/32);
    render(64, kBlockFrames);

    ASSERT_TRUE(waitForEvents(collector))
        << "el motor no entregó un solo evento — la cadena RT→cola→worker→callback "
           "está cortada en alguna parte";

    for (const auto& ev : collector.snapshot()) {
        EXPECT_GE(ev.type, WMA_LOOPER_EVENT_PROGRESS);
        EXPECT_LE(ev.type, WMA_LOOPER_EVENT_TRACK_COMPLETED);
        EXPECT_GE(ev.trackIndex, 0);
        EXPECT_LT(ev.trackIndex, 16);
    }
}

TEST_F(CApiLooperTest, TheUserDataPointerArrivesUntouched) {
    startAt(kSampleRate, 0);
    EventCollector& collector = installCollector();

    recordTrack(0, /*blocks=*/32);
    render(64, kBlockFrames);

    // Que `collector` tenga algo YA prueba que el puntero llegó entero: el
    // callback es estático y sin ese `user_data` no tendría dónde escribir.
    EXPECT_TRUE(waitForEvents(collector));
}

/**
 * El clear detiene los eventos — y la AUSENCIA se afirma contra evidencia de que
 * el worker corrió, no contra el paso del tiempo (AC-002.4).
 *
 * POR QUE HACE FALTA UN CENTINELA, Y POR QUE VA DESPUES
 * -----------------------------------------------------
 * Una no-ocurrencia no se puede esperar. La version anterior dormia 120 ms y
 * afirmaba `size() == 0`, lo que da verde por las dos razones opuestas: porque
 * el clear funciona, o **porque el worker no llego a correr nunca**. Bajo carga
 * la segunda es real, y era indistinguible de la primera.
 *
 * El centinela las separa. Se reinstala un receptor DESPUES de la tanda que no
 * debe llegar, y se espera a que reciba: el worker drena EN ORDEN, asi que si el
 * centinela ve un evento posterior, la tanda anterior **ya paso por el worker** y
 * su destino ya se decidio. Recien ahi `victima.size()` significa algo.
 *
 * Y LOS EVENTOS EN VUELO NO SE ESPERAN "UN RATO"
 * ----------------------------------------------
 * Al momento del clear puede haber eventos ya levantados que todavia aterrizan en
 * el receptor viejo: el header lo declara como propiedad del disenio —*"an event
 * picked up just before the clear still reaches the old callback"*— y el
 * dispatcher **no expone contador de despachados** (`LooperEventDispatcher` solo
 * lleva `mDropped`, que cuenta desbordes de cola).
 *
 * Sin observable al que preguntarle, la primera version de esta etapa dormia un
 * rato fijo. **Eso seguia siendo la clase que el REQ persigue**, aunque pasara:
 * un rato fijo alcanza en una maquina ociosa y se queda corto donde importa. Se
 * espera a que el receptor DEJE DE CRECER (`waitUntilQuiet`), que si es una
 * condicion — si el worker va lento, la quietud se detecta mas tarde, pero se
 * detecta.
 *
 * Resultado: en este archivo no queda ninguna espera ciega.
 */
TEST_F(CApiLooperTest, ClearingTheCallbackStopsTheEvents) {
    startAt(kSampleRate, 0);
    EventCollector& victim = installCollector();

    recordTrack(0, /*blocks=*/32);
    render(64, kBlockFrames);
    ASSERT_TRUE(waitForEvents(victim)) << "precondición: los eventos llegaban";

    wma_looper_set_event_callback(mWma, nullptr, nullptr);

    // Lo que estaba en vuelo al momento del clear todavía puede aterrizar en
    // `victim`, y es contractual. Se espera a que DEJE DE CRECER —no un rato fijo—
    // y ese es el numero contra el que se compara despues.
    const size_t settled = waitUntilQuiet(victim);

    // La tanda que no tiene que llegarle a nadie: el sink está en nullptr.
    render(64, kBlockFrames);

    // 🔴 SE ESPERA A QUE `victim` SE ASIENTE **ANTES** DE TOCAR EL SINK, Y EL ORDEN
    // ES LA MITAD DEL TEST. Instalar el centinela primero cambia el sink, y los
    // eventos de la tanda de arriba que el worker todavía no drenó le llegarían a
    // ÉL en vez de a `victim` — comportamiento correcto del producto que deja al
    // test sin nada que observar. Medido: con esa versión el mutante que rompe el
    // clear SOBREVIVE, mientras que el test viejo (que dormía entre generar y
    // afirmar) lo mataba 20 contra 0. Sacar la espera sin poner una condición en
    // su lugar no arregló el test: lo dejó ciego.
    const size_t afterBatch = waitUntilQuiet(victim);

    // Y el centinela, DESPUÉS: prueba de que el worker está vivo. Sin él, un
    // `victim` quieto no distingue "el clear anduvo" de "el worker nunca corrió",
    // que es el falso verde que AC-002.4 persigue.
    EventCollector& sentinel = installCollector();
    render(16, kBlockFrames);
    ASSERT_TRUE(waitForEvents(sentinel))
        << "el centinela no recibió nada, así que no hay forma de saber si el "
           "worker llegó a correr — sin eso, lo de abajo no prueba nada";

    EXPECT_EQ(afterBatch, settled)
        << "siguieron llegando eventos después del clear";
}

/**
 * Registrar de nuevo reemplaza al anterior. La AUSENCIA en el primero se ancla a
 * la PRESENCIA en el segundo (AC-002.4), que es evidencia directa de que el
 * worker despachó.
 */
TEST_F(CApiLooperTest, RegisteringAgainReplacesThePreviousCallback) {
    startAt(kSampleRate, 0);
    EventCollector& first = installCollector();

    recordTrack(0, /*blocks=*/32);
    render(32, kBlockFrames);
    ASSERT_TRUE(waitForEvents(first)) << "precondición: el primero recibía";

    EventCollector& second = installCollector();
    const size_t firstSettled = waitUntilQuiet(first);   // lo en vuelo, igual que arriba

    render(64, kBlockFrames);

    ASSERT_TRUE(waitForEvents(second))
        << "el segundo callback no quedó instalado";
    EXPECT_EQ(first.size(), firstSettled)
        << "el primero siguió recibiendo después de ser reemplazado";
}

TEST_F(CApiLooperTest, ANullEngineIsIgnoredRatherThanCrashing) {
    // Local a proposito y sin riesgo: con `engine == nullptr` la llamada rebota en
    // WMA_CHECK_VOID y no queda ningun sink apuntando aca.
    EventCollector collector;
    wma_looper_set_event_callback(nullptr, &EventCollector::callback, &collector);
    SUCCEED();
}

}  // namespace

// ===========================================================================
// REQ-007.2 — el ruteo de una pista a la cadena, desde la C-API (AC-007.7)
//
// El criterio de muerte del REQ apunta justo a lo que estos tests NO pueden ser:
// un `set` seguido de un `get`. Asi murio `touch.velocity` en SoundFontEngine,
// escrita durante meses sin que nadie la leyera, con un round-trip que habria
// dado verde. Por eso el par se verifica con AUDIO —la pista suena distinta al
// pasar por la cadena— y el round-trip queda como lo que es: el chequeo barato
// de que el getter no miente, no la prueba de que el flag hace algo.
// ===========================================================================

TEST_F(CApiLooperTest, TheSendToFxFlagRoundTripsThroughTheCApi) {
    startAt(kSampleRate, /*fadeTimeMs=*/0);
    recordTrack(0);

    EXPECT_FALSE(wma_looper_is_track_send_to_fx(mWma, 0))
        << "el default es NO rutear: es el comportamiento de siempre";

    wma_looper_set_track_send_to_fx(mWma, 0, true);
    EXPECT_TRUE(wma_looper_is_track_send_to_fx(mWma, 0));

    wma_looper_set_track_send_to_fx(mWma, 0, false);
    EXPECT_FALSE(wma_looper_is_track_send_to_fx(mWma, 0));
}

TEST_F(CApiLooperTest, TheSendToFxFlagActuallyRoutesTheAudioThroughTheChain) {
    // EL EFECTO ELEGIDO IMPORTA, Y ESTA ES LA TERCERA FORMA DE ESTE TEST.
    //
    // Con DISTORTION no se podia concluir: comparar "antes" contra "despues"
    // dentro de un mismo motor compara tramos distintos de un loop en movimiento,
    // asi que la fase sola producia diferencias del orden de la del efecto. Una
    // version paso incluso con el setter NEUTRALIZADO. Y dos motores en paralelo
    // no se pueden: el fixture registra el backend falso en un GLOBAL.
    //
    // Un LPF a 20 Hz sobre un seno de 440 lo aniquila, asi que la pregunta deja
    // de ser "cambio un poco" y pasa a ser "desaparecio": una pista ruteada se
    // cae a silencio y una no ruteada no se entera. Sin baseline, sin umbral
    // inventado y sin sensibilidad a la fase.
    startAt(kSampleRate, /*fadeTimeMs=*/0);
    recordTrack(0);

    ASSERT_GE(wma_effect_add(mWma, 0 /*FILTER*/), 0);
    ASSERT_EQ(wma_effect_set_param(mWma, 0, 2, 0.0f), WMA_OK);      // type = LPF
    ASSERT_EQ(wma_effect_set_param(mWma, 0, 0, 20.0f), WMA_OK);     // cutoff = 20 Hz
    ASSERT_EQ(wma_effect_set_param(mWma, 0, 1, 0.7f), WMA_OK);      // sin resonancia

    render(8, kBlockFrames);                       // que el filtro se asiente
    const float sinRutear = renderBlockPeak(kBlockFrames);
    ASSERT_GT(sinRutear, kAudible)
        << "sin rutear la pista se mezcla downstream: el filtro no la ve. "
        << "Si esto es silencio, el test no mide nada.";

    wma_looper_set_track_send_to_fx(mWma, 0, true);
    render(8, kBlockFrames);                       // el filtro ataca la pista
    const float ruteada = renderBlockPeak(kBlockFrames);

    EXPECT_LT(ruteada, sinRutear * 0.2f)
        << "prendido el flag por la C-API, el LPF a 20 Hz tiene que comerse la "
        << "pista. sin rutear=" << sinRutear << " ruteada=" << ruteada;
}

TEST_F(CApiLooperTest, AnOutOfRangeTrackIndexIsIgnoredInsteadOfCrashing) {
    startAt(kSampleRate, /*fadeTimeMs=*/0);

    // Mismo criterio que el resto de la familia `wma_looper_set_track_*`: se
    // ignora en silencio. No se inventa un codigo de error para un caso que sus
    // hermanas ya tratan asi.
    wma_looper_set_track_send_to_fx(mWma, -1, true);
    wma_looper_set_track_send_to_fx(mWma, 9999, true);

    EXPECT_FALSE(wma_looper_is_track_send_to_fx(mWma, -1));
    EXPECT_FALSE(wma_looper_is_track_send_to_fx(mWma, 9999));
    EXPECT_FALSE(wma_looper_is_track_send_to_fx(mWma, 0))
        << "una pista valida no puede quedar marcada por un indice invalido";
}

TEST_F(CApiLooperTest, ANullEngineIsSurvivedByBothHalvesOfThePair) {
    // El par entero pasa por WMA_CHECK_*: un motor nulo no puede tirar.
    wma_looper_set_track_send_to_fx(nullptr, 0, true);
    EXPECT_FALSE(wma_looper_is_track_send_to_fx(nullptr, 0));
}

}  // namespace wma_test
