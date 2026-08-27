// ============================================================================
// test_looper_stress — concurrent audio-thread playback vs UI-thread mutations
// (plan §4.3). Exercises the RT-safety contract that protects mixInto()'s reads
// against clear()/trim()/finalizeFreeLoop()/restoreUndo() on another thread
// (mPlaying=false + fence + waitForRenderIdle before any buffer mutation), for
// BOTH the dense and the paged (chunked) storage backend.
//
// ThreadSanitizer note: MSVC has no TSan, so on this host the test is a
// use-after-free / crash / hang smoke test (data races that don't corrupt won't
// be flagged). Run it under clang `-fsanitize=thread` on Linux/macOS CI for full
// race detection — the test body is written to be TSan-clean.
// ============================================================================
#include "support/FixturePath.h"

#include <gtest/gtest.h>
#include "AudioLooper.h"
#include "WavFile.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <random>
#include <thread>
#include <vector>

namespace {
constexpr int kSR = 48000;
constexpr int kNumTracks = 4;

// Record `frames` of content into a track (single-threaded setup).
void recordTrack(AudioLooper& looper, int tk, int frames) {
    looper.setTailMs(0);
    if (!looper.prepareTrack(tk, frames, kSR)) return;
    looper.startRecording(tk);
    std::vector<float> buf(256 * 2);
    int remaining = frames;
    int64_t pf = 0;
    while (remaining > 0 && looper.isRecording()) {
        const int n = std::min(256, remaining);
        for (int i = 0; i < n; ++i) {
            const float v = 0.3f * std::sin(static_cast<float>(pf + i) * 0.01f);
            buf[i * 2] = v; buf[i * 2 + 1] = v;
        }
        looper.process(buf.data(), n, pf);
        pf += n; remaining -= n;
    }
}
}  // namespace

TEST(LooperStress, PlaybackVsControlNoCrash) {
    AudioLooper looper;
    looper.setSampleRate(kSR);
    looper.prepareMixBuffer(4096);

    // Seed a few tracks with content and start them playing.
    for (int t = 0; t < kNumTracks; ++t) {
        recordTrack(looper, t, 20000 + t * 5000);
        looper.resumeTrack(t);
    }

    std::atomic<bool> stop{false};
    std::atomic<int64_t> callbacks{0};

    // Audio thread: hammer process() (capture is idle → this is pure playback mix).
    std::thread audio([&] {
        std::vector<float> out(256 * 2, 0.0f);
        int64_t pf = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            std::fill(out.begin(), out.end(), 0.0f);
            looper.process(out.data(), 256, pf);
            pf += 256;
            callbacks.fetch_add(1, std::memory_order_relaxed);
            // Sanity: output must stay finite even as tracks are mutated under us.
            for (float s : out) ASSERT_TRUE(std::isfinite(s));
        }
    });

    // UI thread: random buffer-mutating + lock-free control ops. All the
    // buffer-freeing ops honour the render-idle contract, so playback must never
    // read freed memory.
    std::mt19937 rng(20260703);
    auto frand = [&](float lo, float hi) {
        return lo + (hi - lo) * (static_cast<float>(rng() & 0xFFFF) / 65535.0f);
    };
    for (int i = 0; i < 4000; ++i) {
        const int tk = static_cast<int>(rng() % kNumTracks);
        switch (rng() % 9) {
            case 0: looper.trimTrack(tk); break;
            case 1: {
                const int len = looper.getTrackLengthFrames(tk);
                if (len > 4096) looper.finalizeFreeLoop(tk, 0, len / 2 + 1024, 256);
            } break;
            case 2: looper.saveUndoSnapshot(tk); break;
            case 3: looper.restoreUndo(tk); break;
            case 4: looper.setTrackVolume(tk, frand(0.0f, 1.5f)); break;
            case 5: looper.setTrackPan(tk, frand(-1.0f, 1.0f)); break;
            case 6: looper.setTrackMuted(tk, (rng() & 1) != 0); break;
            case 7: (rng() & 1) ? looper.pauseTrack(tk) : looper.resumeTrack(tk); break;
            case 8: {
                const int len = looper.getTrackLengthFrames(tk);
                if (len > 2048) looper.setTrackLoopRegion(tk, 0, len);
            } break;
        }
        // ESTIMULO: jitter deliberado para variar el interleaving entre hilos.
        // WAIT-OK: estimulo — jitter deliberado para variar el interleaving.
        if ((i & 63) == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    stop.store(true, std::memory_order_relaxed);
    audio.join();

    EXPECT_GT(callbacks.load(), 0);
    EXPECT_GE(looper.getFramesDropped(), 0);  // telemetry accessible, no crash
}

// Clear vs playback: repeatedly free and re-seed a track while another thread
// plays it. clear() and importTrack() both free the buffer under the render-idle
// guard, and importTrack re-fills it with playback disabled — so the audio
// thread's mixInto must never read freed memory. (Re-seeding uses importTrack,
// NOT process(), so only ONE thread is ever inside process().)
TEST(LooperStress, ClearVsPlaybackNoUseAfterFree) {
    AudioLooper looper;
    looper.setSampleRate(kSR);
    looper.prepareMixBuffer(4096);

    // A WAV to re-import as the track content.
    //
    // 🔴 UNICA POR PROCESO (MINI-009), y no es cosmetico: este test se compila en
    // `looper_tests` y en `looper_tests_dense`, ctest corre las dos EN PARALELO, y
    // abajo se relee este archivo 120 veces durante ~8 s. Con un nombre compartido,
    // la otra copia lo re-escribe en el medio y la lectura sale truncada
    // (`readWav returned 0 frames`) — que es como se puso rojo master en 44a9a4d.
    const wma_test::ScopedFixture seedFile("stress_seed.wav");
    const std::filesystem::path wav = seedFile.path();
    {
        std::vector<float> seed(30000 * 2);
        for (int i = 0; i < 30000; ++i) {
            const float v = 0.3f * std::sin(static_cast<float>(i) * 0.01f);
            seed[i * 2] = v; seed[i * 2 + 1] = v;
        }
        ASSERT_TRUE(wav::writeWav(wav.string().c_str(), seed.data(), 30000, kSR));
    }
    ASSERT_TRUE(looper.importTrack(0, wav.string().c_str(), kSR));
    looper.resumeTrack(0);

    std::atomic<bool> stop{false};
    std::thread audio([&] {
        std::vector<float> out(256 * 2, 0.0f);
        int64_t pf = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            std::fill(out.begin(), out.end(), 0.0f);
            looper.process(out.data(), 256, pf);
            for (float s : out) ASSERT_TRUE(std::isfinite(s));
            pf += 256;
        }
    });

    for (int i = 0; i < 120; ++i) {
        looper.clearTrack(0);                 // frees the buffer (render-idle guarded)
        // WAIT-OK: estimulo — deja que el render agarre el buffer recien liberado;
        //          es la ventana que este stress existe para abrir.
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        looper.importTrack(0, wav.string().c_str(), kSR);  // re-seed on this (UI) thread
        looper.resumeTrack(0);
        // WAIT-OK: estimulo — deja que el render agarre el buffer recien liberado;
        //          es la ventana que este stress existe para abrir.
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    stop.store(true, std::memory_order_relaxed);
    audio.join();
    std::error_code ec; std::filesystem::remove(wav, ec);
    SUCCEED();
}
