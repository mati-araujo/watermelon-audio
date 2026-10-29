/**
 * test_song_mix.cpp — la mezcla propia de la cancion y su bus «Espacio» (REQ-055 S12, W2),
 * con el motor entero y los taps de `wma_offline_render_taps`.
 *
 * Cada test responde a un defecto plausible (cabecera de cada uno). Los taps son la unica forma
 * de ver el seco por pista y el retorno del bus sin suponer nada del reverb.
 */

#include "support/CApiFixture.h"
#include "support/RtAllocCounter.h"
#include "tests/support/TestSanitizer.h"
#include "tests/support/TestWait.h"

#include "core/SongSpaceBus.h"
#include "looper/AudioLooper.h"
#include "looper/WavFile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

namespace wma_test {
namespace {

constexpr int kRate = 48000;
constexpr int kLoop = 4800;   // 100 ms
constexpr int kBlock = 512;

std::filesystem::path mixFixture(const std::string& name) {
    return std::filesystem::temp_directory_path() /
           ("wm" + std::to_string(static_cast<long long>(getpid())) + "_smix_" + name + ".wav");
}

/// `badValue` reemplaza la muestra `badFrame` (ambos canales) cuando `badFrame >= 0`.
std::string writeTone(const std::string& name, float period, float phase, float amp, int badFrame = -1,
                      float badValue = 0.0f) {
    std::vector<float> buf(static_cast<size_t>(kLoop) * 2);
    for (int n = 0; n < kLoop; ++n) {
        float v = amp * std::sin(6.2831853f * static_cast<float>(n) / period + phase);
        if (n == badFrame) v = badValue;
        buf[static_cast<size_t>(n) * 2] = v;
        buf[static_cast<size_t>(n) * 2 + 1] = v;
    }
    const auto path = mixFixture(name);
    EXPECT_TRUE(wav::writeWav(path.c_str(), buf.data(), kLoop, kRate, wav::BitDepth::FLOAT_32));
    return path.string();
}

int32_t loadClip(WmaEngine* e, const std::string& path) {
    int32_t id = -1;
    EXPECT_EQ(wma_song_load_clip(e, path.c_str(), kRate, 0, 0, kLoop, &id), WMA_OK);
    return id;
}

WmaSongSection section2(int64_t start, int32_t clip0, int32_t clip1) {
    WmaSongSection s{};
    s.start_frame = start;
    for (int t = 0; t < WMA_SONG_MAX_TRACKS; ++t) s.clip[t] = WMA_SONG_SILENCE;
    s.clip[0] = clip0;
    s.clip[1] = clip1;
    return s;
}

WmaMixSpace space(float reverbReturn, float delayReturn) {
    WmaMixSpace s{};
    s.reverb_return = reverbReturn;
    s.reverb_size = 0.9f;
    s.reverb_decay_s = 1.5f;
    s.reverb_damping = 0.4f;
    s.reverb_predelay_ms = 12.0f;
    s.reverb_low_cut_hz = 90.0f;
    s.reverb_high_cut_hz = 9000.0f;
    s.delay_return = delayReturn;
    s.delay_time_ms = 125.0f;
    s.delay_feedback = 0.4f;
    return s;
}

struct Rendered {
    std::vector<float> mix, fx;
    std::vector<std::vector<float>> tracks;   // por pista, estereo intercalado
};

float peakOf(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
    float p = 0.0f;
    for (size_t i = from; i < std::min(to, v.size()); ++i) p = std::max(p, std::abs(v[i]));
    return p;
}

double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double w = 0.0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) w = std::max(w, double(std::abs(a[i] - b[i])));
    return w;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

class SongMix : public ::testing::Test {
protected:
    void SetUp() override { wma::setLogCallback([](wma::LogLevel, const char*, const char*) {}); }
    void TearDown() override {
        for (const auto& p : mFiles) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
        for (WmaEngine* e : mEngines) wma_engine_destroy(e);
        wma::setLogCallback(nullptr);
    }

    std::string tone(const std::string& name, float period, float phase, float amp = 0.3f, int badFrame = -1,
                     float badValue = 0.0f) {
        mFiles.push_back(mixFixture(name));
        return writeTone(name, period, phase, amp, badFrame, badValue);
    }

    /// Un motor offline en el estado que usa un export: sin instrumento, salida RAW.
    WmaEngine* offlineEngine(WmaOutputProfile profile = WMA_OUTPUT_RAW) {
        WmaEngine* e = wma_engine_create();
        mEngines.push_back(e);
        wma_set_audio_mode(e, 0);
        wma_set_master_volume(e, 1.0f);
        wma_set_synth_volume(e, 0.0f);
        wma_set_frequency_amplitude(e, 440.0f, 0.0f);
        EXPECT_EQ(wma_offline_start(e, kRate, kBlock), WMA_OK);
        EXPECT_EQ(wma_mix_set_output_profile(e, profile), WMA_OK);
        return e;
    }

    /// Programa 2 pistas (un tono cada una) y renderiza `loops` loops con taps.
    Rendered renderTaps(WmaEngine* e, int loops, int trackCount = 2) {
        const int64_t total = static_cast<int64_t>(loops) * kLoop;
        Rendered r;
        r.mix.assign(static_cast<size_t>(total) * 2, 0.0f);
        r.fx.assign(static_cast<size_t>(total) * 2, 0.0f);
        r.tracks.assign(static_cast<size_t>(trackCount), std::vector<float>(static_cast<size_t>(total) * 2, 0.0f));
        std::vector<float> mix(kBlock * 2), fx(kBlock * 2), tr(static_cast<size_t>(trackCount) * kBlock * 2);
        for (int64_t done = 0; done < total; done += kBlock) {
            const int n = static_cast<int>(std::min<int64_t>(kBlock, total - done));
            EXPECT_EQ(wma_offline_render_taps(e, mix.data(), fx.data(), tr.data(), trackCount, n), WMA_OK);
            const size_t at = static_cast<size_t>(done) * 2;
            std::copy_n(mix.begin(), static_cast<size_t>(n) * 2, r.mix.begin() + at);
            std::copy_n(fx.begin(), static_cast<size_t>(n) * 2, r.fx.begin() + at);
            for (int t = 0; t < trackCount; ++t) {
                std::copy_n(tr.begin() + static_cast<size_t>(t) * n * 2, static_cast<size_t>(n) * 2,
                            r.tracks[static_cast<size_t>(t)].begin() + at);
            }
        }
        return r;
    }

    /// Renderiza `frames` con taps y descarta todo: para dejar pasar una cola larga sin guardarla.
    void skip(WmaEngine* e, int64_t frames) {
        std::vector<float> mix(kBlock * 2), fx(kBlock * 2), tr(kBlock * 2);
        for (int64_t done = 0; done < frames; done += kBlock) {
            const int n = static_cast<int>(std::min<int64_t>(kBlock, frames - done));
            ASSERT_EQ(wma_offline_render_taps(e, mix.data(), fx.data(), tr.data(), 1, n), WMA_OK);
        }
    }

    /// Una cancion de una pista con envio 1 y un delay con la realimentacion al maximo de la C API
    /// (0,9); la agenda ya se saco. Con 250 ms la cola dura ~35 s.
    WmaEngine* echoingEngineWithoutSchedule(const std::string& name, float delayReturn,
                                            float delayMs = 250.0f, float period = 100.0f) {
        const std::string pa = tone(name, period, 0.0f);
        WmaEngine* e = offlineEngine();
        const int32_t a = loadClip(e, pa);
        EXPECT_EQ(wma_mix_set_track(e, 0, 1.0f, 0.0f, 1.0f), WMA_OK);
        mEchoSpace = space(0.0f, delayReturn);
        mEchoSpace.delay_time_ms = delayMs;
        mEchoSpace.delay_feedback = 0.9f;
        EXPECT_EQ(wma_mix_set_space(e, &mEchoSpace), WMA_OK);
        const int64_t p0 = wma_transport_get_play_frame(e);
        const WmaSongSection s = section2(p0 + 2 * kLoop, a, WMA_SONG_SILENCE);
        EXPECT_EQ(wma_song_set_schedule(e, kLoop, &s, 1, p0 + 600 * kLoop), WMA_OK);
        (void)renderTaps(e, 8, 1);
        EXPECT_EQ(wma_song_set_schedule(e, kLoop, nullptr, 0, 0), WMA_OK);
        return e;
    }

    WmaMixSpace mEchoSpace{};

    /// Cancion de dos pistas: arranca a los 2 loops y dura 6.
    void scheduleSong(WmaEngine* e, int32_t a, int32_t b) {
        const int64_t p0 = wma_transport_get_play_frame(e);
        const WmaSongSection s = section2(p0 + 2 * kLoop, a, b);
        EXPECT_EQ(wma_song_set_schedule(e, kLoop, &s, 1, p0 + 12 * kLoop), WMA_OK);
    }

    std::vector<std::filesystem::path> mFiles;
    std::vector<WmaEngine*> mEngines;
};

}  // namespace

// Defecto: un envio que no se apaga con send = 0 (el bus recibe algo, o el retorno deja un
// residuo) o una salida que no es la suma de los secos. Con los retornos al maximo y los dos
// envios en cero el retorno es exactamente 0 y la premezcla, la suma de las dos pistas.
TEST_F(SongMix, SendZeroGivesNoFxAndTheMixIsTheSumOfTheDryTaps) {
    const std::string pa = tone("z_a", 100.0f, 0.0f), pb = tone("z_b", 75.0f, 1.0f);
    WmaEngine* e = offlineEngine();
    const int32_t a = loadClip(e, pa), b = loadClip(e, pb);
    ASSERT_EQ(wma_mix_set_track(e, 0, 1.0f, 0.0f, 0.0f), WMA_OK);
    ASSERT_EQ(wma_mix_set_track(e, 1, 1.0f, 0.3f, 0.0f), WMA_OK);
    const WmaMixSpace sp = space(1.0f, 1.0f);
    ASSERT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
    scheduleSong(e, a, b);
    const Rendered r = renderTaps(e, 7);
    ASSERT_GT(peakOf(r.tracks[0]), 0.1f);
    ASSERT_GT(peakOf(r.tracks[1]), 0.1f);
    EXPECT_EQ(peakOf(r.fx), 0.0f) << "send 0 y el retorno no es cero";
    std::vector<float> sum(r.mix.size());
    for (size_t i = 0; i < sum.size(); ++i) sum[i] = r.tracks[0][i] + r.tracks[1][i];
    EXPECT_LE(maxDiff(r.mix, sum), 1e-6);
}

// Defecto: la salida RAW deja de ser la suma de los taps (un tap sin la rampa del master del
// looper, el retorno sumado dos veces, una pista contada en el tap y no en la mezcla). Con
// envios y retornos activos: salida = Σ taps de pista + FX, a 1e-6. El retorno tiene que sonar.
TEST_F(SongMix, RawOutputEqualsTheSumOfTheTapsPlusTheFxReturn) {
    const std::string pa = tone("r_a", 100.0f, 0.0f), pb = tone("r_b", 75.0f, 1.0f);
    WmaEngine* e = offlineEngine();
    const int32_t a = loadClip(e, pa), b = loadClip(e, pb);
    ASSERT_EQ(wma_mix_set_track(e, 0, 0.8f, -0.5f, 0.6f), WMA_OK);
    ASSERT_EQ(wma_mix_set_track(e, 1, 1.3f, 0.7f, 0.3f), WMA_OK);
    const WmaMixSpace sp = space(0.7f, 0.5f);
    ASSERT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
    wma_looper_set_master_volume(e, 0.6f);   // los taps van DESPUES del master del looper
    scheduleSong(e, a, b);
    const Rendered r = renderTaps(e, 7);
    ASSERT_GT(peakOf(r.fx), 0.02f) << "el bus no devuelve nada: el test no prueba nada";
    std::vector<float> sum(r.mix.size());
    for (size_t i = 0; i < sum.size(); ++i) sum[i] = r.tracks[0][i] + r.tracks[1][i] + r.fx[i];
    EXPECT_LE(maxDiff(r.mix, sum), 1e-6);
    // el master del looper llego a cada tap: sin el, un tap seria ~1/0,6 mayor que su parte de mix
    EXPECT_LT(peakOf(r.tracks[0]), 0.8f * 0.3f / 0.6f);
}

// Defecto: el envio es PRE-fader (o pre-paneo). Dos configuraciones con el MISMO seco-por-envio
// (ganancia 0,5 x envio 1 contra ganancia 1 x envio 0,5) tienen que dar el mismo retorno; un
// mutante pre-fader da el doble en la primera. Y con paneo -1 el delay (estereo) solo devuelve
// por L: el envio sale del seco POST-paneo.
TEST_F(SongMix, SendIsPostFaderAndPostPan) {
    const std::string pa = tone("pf_a", 100.0f, 0.0f);
    const auto fxFor = [&](float gain, float pan, float send) {
        WmaEngine* e = offlineEngine();
        const int32_t a = loadClip(e, pa);
        EXPECT_EQ(wma_mix_set_track(e, 0, gain, pan, send), WMA_OK);
        const WmaMixSpace sp = space(0.0f, 1.0f);   // solo el delay (estereo, sin mezclar L y R)
        EXPECT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
        const int64_t p0 = wma_transport_get_play_frame(e);
        const WmaSongSection s = section2(p0 + 2 * kLoop, a, WMA_SONG_SILENCE);
        EXPECT_EQ(wma_song_set_schedule(e, kLoop, &s, 1, p0 + 12 * kLoop), WMA_OK);
        return renderTaps(e, 7, 1);
    };
    const Rendered halfGain = fxFor(0.5f, 0.0f, 1.0f);
    const Rendered halfSend = fxFor(1.0f, 0.0f, 0.5f);
    ASSERT_GT(peakOf(halfGain.fx), 0.02f);
    EXPECT_LE(maxDiff(halfGain.fx, halfSend.fx), 1e-6) << "el envio no es post-fader";

    const Rendered left = fxFor(1.0f, -1.0f, 1.0f);
    const Rendered centre = fxFor(1.0f, 0.0f, 1.0f);
    float peakR = 0.0f, peakLeftL = 0.0f, peakCentreL = 0.0f;
    for (size_t i = 0; i < left.fx.size(); i += 2) {
        peakLeftL = std::max(peakLeftL, std::abs(left.fx[i]));
        peakR = std::max(peakR, std::abs(left.fx[i + 1]));
        peakCentreL = std::max(peakCentreL, std::abs(centre.fx[i]));
    }
    ASSERT_GT(peakLeftL, 0.02f);
    EXPECT_EQ(peakR, 0.0f) << "con paneo -1 el envio sale tambien por R: es pre-paneo";
    EXPECT_NEAR(peakLeftL / peakCentreL, 1.0f / 0.7071f, 0.01f);
}

// Defecto: la ley de paneo no es la del looper (EqualPowerPanLUT): -1 deja pasar canal R, o el
// centro no esta a -3 dB.
TEST_F(SongMix, PanLawIsTheLooperEqualPowerOne) {
    const std::string pa = tone("p_a", 100.0f, 0.0f, 0.5f);
    const auto tapsFor = [&](float pan) {
        WmaEngine* e = offlineEngine();
        const int32_t a = loadClip(e, pa);
        EXPECT_EQ(wma_mix_set_track(e, 0, 1.0f, pan, 0.0f), WMA_OK);
        const int64_t p0 = wma_transport_get_play_frame(e);
        const WmaSongSection s = section2(p0 + 2 * kLoop, a, WMA_SONG_SILENCE);
        EXPECT_EQ(wma_song_set_schedule(e, kLoop, &s, 1, p0 + 12 * kLoop), WMA_OK);
        return renderTaps(e, 7, 1);
    };
    const auto chPeak = [](const Rendered& r, int ch) {
        float p = 0.0f;
        for (size_t i = static_cast<size_t>(ch); i < r.tracks[0].size(); i += 2) p = std::max(p, std::abs(r.tracks[0][i]));
        return p;
    };
    const Rendered left = tapsFor(-1.0f), centre = tapsFor(0.0f), right = tapsFor(1.0f);
    EXPECT_EQ(chPeak(left, 1), 0.0f);
    EXPECT_NEAR(chPeak(left, 0), 0.5f, 0.005f);
    EXPECT_LT(chPeak(right, 0), 1e-7f);   // cos(pi/2) en float: -4e-8, no 0
    EXPECT_NEAR(chPeak(centre, 0) / 0.5f, 0.7071f, 0.003f);
    EXPECT_NEAR(chPeak(centre, 1) / 0.5f, 0.7071f, 0.003f);
}

// Defecto: el primer bloque rampea desde una voz fresca (ganancia 1, paneo 0) hacia la mezcla
// pedida: un export con una pista a -12 dB arranca con un fundido. La cancion arranca EN el frame
// actual, asi que la voz suena ya en el primer bloque que la ve activa: ahi la ganancia tiene que
// ser la del target (un smoother sin clavar da un tap casi del doble de fuerte y con canal L).
TEST_F(SongMix, FirstBlockStartsAtTheMixTargetWithoutARamp) {
    const std::string pa = tone("s_a", 100.0f, 1.5707963f, 0.5f);   // coseno: pico en el frame 0
    WmaEngine* e = offlineEngine();
    const int32_t a = loadClip(e, pa);
    ASSERT_EQ(wma_mix_set_track(e, 0, 0.25f, 1.0f, 0.0f), WMA_OK);
    const int64_t p0 = wma_transport_get_play_frame(e);
    const WmaSongSection s = section2(p0, a, WMA_SONG_SILENCE);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, &s, 1, p0 + 12 * kLoop), WMA_OK);
    const Rendered r = renderTaps(e, 1, 1);
    float peakL = 0.0f, peakR = 0.0f;
    for (size_t i = 0; i < static_cast<size_t>(kBlock) * 2; i += 2) {
        peakL = std::max(peakL, std::abs(r.tracks[0][i]));
        peakR = std::max(peakR, std::abs(r.tracks[0][i + 1]));
    }
    ASSERT_GT(peakR, 0.05f) << "la voz no suena en el primer bloque: el test no prueba nada";
    EXPECT_NEAR(peakR, 0.5f * 0.25f, 1e-4f) << "el primer bloque rampea desde la voz fresca";
    EXPECT_LT(peakL, 1e-5f) << "el paneo no estaba en su target en el primer bloque";
}

// Defecto (cambio de contrato): el volumen, paneo, mute o sendToFx de la pista del looper siguen
// alcanzando a la cancion. Mismos taps con los controles del TrackBuffer en extremos.
TEST_F(SongMix, TrackBufferControlsHaveNoEffectWhileTheSongIsActive) {
    const std::string pa = tone("t_a", 100.0f, 0.0f), pb = tone("t_b", 75.0f, 1.0f);
    const auto run = [&](bool moveTrackBuffer) {
        WmaEngine* e = offlineEngine();
        const int32_t a = loadClip(e, pa), b = loadClip(e, pb);
        EXPECT_EQ(wma_mix_set_track(e, 0, 0.9f, -0.2f, 0.4f), WMA_OK);
        EXPECT_EQ(wma_mix_set_track(e, 1, 0.7f, 0.5f, 0.2f), WMA_OK);
        const WmaMixSpace sp = space(0.6f, 0.4f);
        EXPECT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
        if (moveTrackBuffer) {
            for (int t = 0; t < 2; ++t) {
                wma_looper_set_track_volume(e, t, 0.05f);
                wma_looper_set_track_pan(e, t, 1.0f);
                wma_looper_set_track_muted(e, t, true);
                wma_looper_set_track_send_to_fx(e, t, true);
            }
        }
        scheduleSong(e, a, b);
        return renderTaps(e, 7);
    };
    const Rendered plain = run(false), moved = run(true);
    ASSERT_GT(peakOf(plain.mix), 0.1f);
    EXPECT_TRUE(sameBits(plain.mix, moved.mix));
    EXPECT_TRUE(sameBits(plain.fx, moved.fx));
    EXPECT_TRUE(sameBits(plain.tracks[0], moved.tracks[0]));
    EXPECT_TRUE(sameBits(plain.tracks[1], moved.tracks[1]));
}

// Defecto: el bus no vuelve al estado de una instancia fresca en wma_offline_start (la cola del
// reverb o el delay del render anterior suena en el siguiente, o el tiempo del delay se desliza
// desde el default). Una instancia reusada tiene que dar los bits de una fresca, con bus.
TEST_F(SongMix, AReusedInstanceWithTheBusGivesTheBitsOfAFreshOne) {
    const std::string pa = tone("f_a", 100.0f, 0.0f), pb = tone("f_b", 75.0f, 1.0f);
    const auto configure = [&](WmaEngine* e) {
        const int32_t a = loadClip(e, pa), b = loadClip(e, pb);
        EXPECT_EQ(wma_mix_set_track(e, 0, 0.9f, -0.2f, 0.7f), WMA_OK);
        EXPECT_EQ(wma_mix_set_track(e, 1, 0.7f, 0.5f, 0.5f), WMA_OK);
        const WmaMixSpace sp = space(0.8f, 0.6f);
        EXPECT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
        scheduleSong(e, a, b);
        return renderTaps(e, 8);
    };
    WmaEngine* reused = offlineEngine();
    const int32_t a0 = loadClip(reused, pa), b0 = loadClip(reused, pb);
    ASSERT_EQ(wma_mix_set_track(reused, 0, 1.4f, 0.9f, 1.0f), WMA_OK);   // otra mezcla, y crea el bus
    WmaMixSpace dirty = space(1.0f, 1.0f);
    dirty.delay_time_ms = 333.0f;
    ASSERT_EQ(wma_mix_set_space(reused, &dirty), WMA_OK);
    scheduleSong(reused, a0, b0);
    (void)renderTaps(reused, 8);   // ensucia voces, bus y master
    ASSERT_EQ(wma_offline_start(reused, kRate, kBlock), WMA_OK);   // reinicio de la misma instancia
    ASSERT_EQ(wma_mix_set_output_profile(reused, WMA_OUTPUT_RAW), WMA_OK);
    wma_song_unload_clip(reused, a0);
    wma_song_unload_clip(reused, b0);
    const Rendered second = configure(reused);
    WmaEngine* fresh = offlineEngine();
    const Rendered first = configure(fresh);
    ASSERT_GT(peakOf(first.fx), 0.02f);
    EXPECT_TRUE(sameBits(first.mix, second.mix));
    EXPECT_TRUE(sameBits(first.fx, second.fx));
    EXPECT_TRUE(sameBits(first.tracks[0], second.tracks[0]));
    EXPECT_TRUE(sameBits(first.tracks[1], second.tracks[1]));
}

// Defecto: un envio que se apaga deja el bus alimentado (el smoother del envio nunca llega a 0, o
// el bus se corta de golpe y la cola desaparece). Tras pasar el envio a 0 la cola del delay SIGUE
// sonando (el bus no se corta) y se agota con su realimentacion: 0,4 por eco de 125 ms.
TEST_F(SongMix, TurningTheSendOffLetsTheTailDecayInsteadOfCuttingItOrFeedingItForever) {
    const std::string pa = tone("o_a", 100.0f, 0.0f);
    WmaEngine* e = offlineEngine();
    const int32_t a = loadClip(e, pa);
    ASSERT_EQ(wma_mix_set_track(e, 0, 1.0f, 0.0f, 1.0f), WMA_OK);
    const WmaMixSpace sp = space(0.0f, 1.0f);
    ASSERT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
    const int64_t p0 = wma_transport_get_play_frame(e);
    const WmaSongSection s = section2(p0 + 2 * kLoop, a, WMA_SONG_SILENCE);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, &s, 1, p0 + 60 * kLoop), WMA_OK);
    const Rendered on = renderTaps(e, 7, 1);
    ASSERT_GT(peakOf(on.fx), 0.3f);
    ASSERT_EQ(wma_mix_set_track(e, 0, 1.0f, 0.0f, 0.0f), WMA_OK);
    (void)renderTaps(e, 1, 1);                    // el smoother del envio llega a 0
    const Rendered justAfter = renderTaps(e, 2, 1);
    EXPECT_GT(peakOf(justAfter.fx), 0.02f) << "el bus se corto de golpe";
    (void)renderTaps(e, 20, 1);
    const Rendered later = renderTaps(e, 2, 1);
    EXPECT_LT(peakOf(later.fx), 1e-3f) << "el envio no se apago: el bus sigue alimentado";
    EXPECT_GT(peakOf(later.tracks[0]), 0.1f) << "la cancion ya no suena: el test no prueba nada";
}

// Defecto (BLOQUEANTE del review): al sacar la agenda el bus deja de procesar y la cola se corta en
// seco (y, al reactivar, suenan los ecos congelados). Con envio 1, delay 125 ms y realimentacion
// 0,9, tras sacar la agenda la cola sigue sonando y decae como en un DAW: sigue siendo audible en
// el bloque siguiente, cada segundo vale ~0,43 del anterior, y la premezcla sigue siendo la suma
// de los taps (el retorno de la cola tambien entra al tap de FX).
TEST_F(SongMix, TheTailKeepsDecayingAfterTheScheduleIsRemoved) {
    const std::string pa = tone("c_a", 100.0f, 0.0f);
    WmaEngine* e = offlineEngine();
    const int32_t a = loadClip(e, pa);
    ASSERT_EQ(wma_mix_set_track(e, 0, 1.0f, 0.0f, 1.0f), WMA_OK);
    WmaMixSpace sp = space(0.0f, 1.0f);
    sp.delay_feedback = 0.9f;
    ASSERT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
    const int64_t p0 = wma_transport_get_play_frame(e);
    const WmaSongSection s = section2(p0 + 2 * kLoop, a, WMA_SONG_SILENCE);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, &s, 1, p0 + 600 * kLoop), WMA_OK);
    const Rendered playing = renderTaps(e, 8, 1);
    const float peakWhilePlaying = peakOf(playing.fx, playing.fx.size() / 2);
    ASSERT_GT(peakWhilePlaying, 0.3f);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, nullptr, 0, 0), WMA_OK);
    (void)renderTaps(e, 1, 1);   // el fundido de salida de la voz
    const Rendered tail = renderTaps(e, 40, 1);   // 4 s
    const size_t second = static_cast<size_t>(kRate) * 2;
    const float w0 = peakOf(tail.fx, 0, second / 2), w1 = peakOf(tail.fx, second, second + second / 2);
    const float w2 = peakOf(tail.fx, 2 * second, 2 * second + second / 2);
    EXPECT_GT(w0, 0.05f) << "la cola se corto en seco al sacar la agenda";
    EXPECT_LT(w1, 0.7f * w0);
    EXPECT_LT(w2, 0.7f * w1);
    EXPECT_GT(w2, 0.0f);
    std::vector<float> sum(tail.mix.size());
    for (size_t i = 0; i < sum.size(); ++i) sum[i] = tail.tracks[0][i] + tail.fx[i];
    EXPECT_LE(maxDiff(tail.mix, sum), 1e-6) << "la cola no entra al tap de FX";
}

// Defecto (BLOQUEANTE del review): el bus queda congelado con la cola cuando se saca la agenda y al
// reprogramar con envio 0 suenan los ecos viejos a su nivel de entonces (pico 0,87 en la sonda). Con
// la cola corriendo mientras no hay cancion, al reprogramar 4 s despues la cola ya decayo (un
// segundo vale ~0,43), y como el bus no se reinicia, sigue ahi (no es cero).
TEST_F(SongMix, AScheduleReprogrammedDuringTheTailFindsTheDecayedTailNotAFrozenOne) {
    const std::string pa = tone("c_b", 100.0f, 0.0f);
    WmaEngine* e = offlineEngine();
    const int32_t a = loadClip(e, pa);
    ASSERT_EQ(wma_mix_set_track(e, 0, 1.0f, 0.0f, 1.0f), WMA_OK);
    WmaMixSpace sp = space(0.0f, 1.0f);
    sp.delay_feedback = 0.9f;
    ASSERT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
    const int64_t p0 = wma_transport_get_play_frame(e);
    const WmaSongSection s = section2(p0 + 2 * kLoop, a, WMA_SONG_SILENCE);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, &s, 1, p0 + 600 * kLoop), WMA_OK);
    const Rendered playing = renderTaps(e, 8, 1);
    const float peakWhilePlaying = peakOf(playing.fx, playing.fx.size() / 2);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, nullptr, 0, 0), WMA_OK);
    (void)renderTaps(e, 41, 1);   // 4 s de cola
    ASSERT_EQ(wma_mix_set_track(e, 0, 1.0f, 0.0f, 0.0f), WMA_OK);   // envio 0 al reactivar
    const int64_t p1 = wma_transport_get_play_frame(e);
    const WmaSongSection again = section2(p1 + 2 * kLoop, a, WMA_SONG_SILENCE);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, &again, 1, p1 + 600 * kLoop), WMA_OK);
    const Rendered back = renderTaps(e, 3, 1);
    const float resumed = peakOf(back.fx);
    EXPECT_LT(resumed, 0.2f * peakWhilePlaying) << "suenan los ecos congelados de antes de sacar la agenda";
    EXPECT_GT(resumed, 1e-5f) << "la cola se reinicio al reactivar: se corto en seco";
}

// Defecto: tras agotarse la cola (retorno bajo -120 dBFS por mas del delay maximo) el bus tiene que
// quedar mudo y reactivar con envio 0 no puede traer nada (< 1e-6): ni residuo congelado ni eco.
TEST_F(SongMix, AfterTheTailIsGoneReactivatingWithSendZeroGivesNoFx) {
    const std::string pa = tone("c_c", 100.0f, 0.0f);
    WmaEngine* e = offlineEngine();
    const int32_t a = loadClip(e, pa);
    ASSERT_EQ(wma_mix_set_track(e, 0, 1.0f, 0.0f, 1.0f), WMA_OK);
    const WmaMixSpace sp = space(0.5f, 1.0f);   // realimentacion 0,4 y reverb: cola de ~2 s
    ASSERT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
    const int64_t p0 = wma_transport_get_play_frame(e);
    const WmaSongSection s = section2(p0 + 2 * kLoop, a, WMA_SONG_SILENCE);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, &s, 1, p0 + 600 * kLoop), WMA_OK);
    const Rendered playing = renderTaps(e, 8, 1);
    ASSERT_GT(peakOf(playing.fx), 0.1f);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, nullptr, 0, 0), WMA_OK);
    (void)renderTaps(e, 1, 1);
    const Rendered early = renderTaps(e, 5, 1);
    EXPECT_GT(peakOf(early.fx), 1e-3f) << "la cola no suena tras sacar la agenda";
    (void)renderTaps(e, 120, 1);   // 12 s: agotada y apagada
    ASSERT_EQ(wma_mix_set_track(e, 0, 1.0f, 0.0f, 0.0f), WMA_OK);
    const int64_t p1 = wma_transport_get_play_frame(e);
    const WmaSongSection again = section2(p1 + 2 * kLoop, a, WMA_SONG_SILENCE);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, &again, 1, p1 + 600 * kLoop), WMA_OK);
    const Rendered back = renderTaps(e, 4, 1);
    EXPECT_LT(peakOf(back.fx), 1e-6f);
    EXPECT_GT(peakOf(back.tracks[0]), 0.1f) << "la cancion no suena: el test no prueba nada";
}

// Defecto (hallazgo 1 del re-review): el silencio se media DESPUES de los retornos. Con el retorno
// del delay en 0 el bus se apagaba a los 2,5 s sin marcarse viejo, con la realimentacion (0,9) de
// la linea congelada; al subir el retorno y reprogramar con envio 0 volvia el eco (pico 0,328 en la
// sonda). Midiendo la salida 100 % wet, el bus sigue hasta que las lineas decaen de verdad.
TEST_F(SongMix, AReturnAtZeroDoesNotSwitchOffABusWhoseLinesAreStillFull) {
    WmaEngine* e = echoingEngineWithoutSchedule("h1", 1.0f);
    skip(e, kRate / 2);
    WmaMixSpace muted = mEchoSpace;
    muted.delay_return = 0.0f;
    ASSERT_EQ(wma_mix_set_space(e, &muted), WMA_OK);
    skip(e, static_cast<int64_t>(kRate) * 60);   // 0,9^n < 1e-6 tarda ~35 s
    ASSERT_EQ(wma_mix_set_space(e, &mEchoSpace), WMA_OK);
    ASSERT_EQ(wma_mix_set_track(e, 0, 1.0f, 0.0f, 0.0f), WMA_OK);
    const int64_t p1 = wma_transport_get_play_frame(e);
    const WmaSongSection again = section2(p1 + 2 * kLoop, 0, WMA_SONG_SILENCE);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, &again, 1, p1 + 600 * kLoop), WMA_OK);
    const Rendered back = renderTaps(e, 4, 1);
    EXPECT_LT(peakOf(back.fx), 1e-6f) << "volvio el eco congelado de la linea del delay";
}

// El mismo escenario con el retorno mudo sin pasar de ahi: si solo se mira la salida, el bus se
// apaga a los 2,5 s y la linea queda congelada; con la medicion antes del retorno sigue corriendo y
// decae. Se ve en el tap de FX cuando el retorno vuelve a 1 a los 10 s: el eco que suena es el
// decaido (0,9^16 = 0,19 del inicial), no el congelado.
TEST_F(SongMix, AMutedReturnKeepsTheLineDecayingInsteadOfFreezingIt) {
    WmaEngine* e = echoingEngineWithoutSchedule("h1b", 1.0f);
    const Rendered before = renderTaps(e, 5, 1);
    const float level = peakOf(before.fx);
    ASSERT_GT(level, 0.3f);
    WmaMixSpace muted = mEchoSpace;
    muted.delay_return = 0.0f;
    ASSERT_EQ(wma_mix_set_space(e, &muted), WMA_OK);
    skip(e, static_cast<int64_t>(kRate) * 4);   // 16 ecos de 250 ms: 0,9^16 = 0,19
    ASSERT_EQ(wma_mix_set_space(e, &mEchoSpace), WMA_OK);
    const Rendered after = renderTaps(e, 12, 1);
    EXPECT_GT(peakOf(after.fx), 0.02f) << "la linea se vacio o se corto";
    EXPECT_LT(peakOf(after.fx), 0.3f * level) << "la linea quedo congelada: no decayo mientras estuvo muda";
}

// Defecto (hallazgo 3 del re-review): una cola que sigue audible a los 30 s se cortaba en el tope sin
// fundido (un salto del tamano de la muestra). Sin tope por tiempo ya no hay corte audible: el eco
// (0,9 por 1000 ms) sigue pasando por los 30 s sin discontinuidad ni vaciado del bus.
TEST_F(SongMix, ALoudTailGoesPastThirtySecondsWithoutAStep) {
    WmaEngine* e = echoingEngineWithoutSchedule("h3", 1.0f, 1000.0f);
    skip(e, static_cast<int64_t>(kRate) * 29 + kRate / 2);
    const Rendered around = renderTaps(e, 20, 1);   // 29,5 s .. 31,5 s
    const float level = peakOf(around.fx);
    EXPECT_GT(level, 1e-4f) << "la cola dejo de sonar antes de los 31 s";
    double step = 0.0;
    for (size_t i = 2; i < around.fx.size(); ++i) {
        step = std::max(step, double(std::abs(around.fx[i] - around.fx[i - 2])));
    }
    EXPECT_LT(step, 0.2 * level) << "salto en el fx: un corte sin fundido";
    EXPECT_GT(peakOf(around.fx, around.fx.size() - 2 * kBlock), 1e-5f) << "se corto antes del final";
}

// Defecto (BLOQUEANTE del review de be8d1a70): el interpolador cubico del delay (Bourke) llega a una
// ganancia de 1,0887 hacia los 9,4 kHz (a 48 kHz) con un tiempo fraccional; con realimentacion > 0,9186
// cada vuelta ganaba y la cola divergia sin tope (22398 a los 180 s). Con el maximo que acepta la C API
// (0,9 · 1,0887 = 0,98) el peor caso (tono de 9,4 kHz, fraccion 0,5: 1200,5 muestras = 25,0104 ms; el
// mismo retardo a 250 ms tarda 10x mas, ~190 s) decae, el bus se apaga y no queda eco. La realimentacion
// de la sonda (0,95) se rechaza.
TEST_F(SongMix, AFractionalDelayAtTheMaximumFeedbackWithATopTonePassesAndTheBusGoesQuiet) {
    WmaEngine* e = echoingEngineWithoutSchedule("i1", 1.0f, 25.0104f, 48000.0f / 9400.0f);
    const Rendered early = renderTaps(e, 5, 1);
    ASSERT_GT(peakOf(early.fx), 0.05f) << "la cola no suena: el test no prueba nada";
    WmaMixSpace over = mEchoSpace;
    over.delay_feedback = 0.95f;
    EXPECT_EQ(wma_mix_set_space(e, &over), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    skip(e, static_cast<int64_t>(kRate) * 40);   // 0,98^n < 1e-6 tarda ~19 s, mas los 2,5 s de silencio
    const Rendered late = renderTaps(e, 4, 1);
    for (const float v : late.fx) ASSERT_TRUE(std::isfinite(v));
    EXPECT_LT(peakOf(late.fx), 1e-6f) << "la cola no decayo: el bus diverge o no se apaga";
    auto& looper = e->engine->getAudioLooper();
    EXPECT_FALSE(looper.isSongBusLive()) << "el bus sigue vivo tras agotarse la cola";
    EXPECT_EQ(looper.getSongBusGuardTrips(), 0) << "el saneo del envio tuvo que actuar sobre un clip finito";
}

// Defecto (hallazgo 2 del review): con damping 1 el filtro de cada comb de la reverb no actualiza su
// estado (`filtered = prev`): queda congelado y el bus no baja de 1e-6 (pico antes de los retornos 1,88e-6
// constante en la sonda). El maximo que acepta la C API es 0,95 (1 se rechaza: es lo que este test ata);
// con 0,95 puesto durante la cola de un ruido de +-4 y corte bajo en 20 Hz el bus se agota y se apaga.
TEST_F(SongMix, AReverbTailOfLoudNoiseWithTheMaximumDampingEndsAndTheBusGoesQuiet) {
    std::vector<float> noise(static_cast<size_t>(kLoop) * 2);
    uint32_t seed = 12345u;
    for (float& v : noise) {
        seed = seed * 1664525u + 1013904223u;
        v = (static_cast<float>(seed >> 8) / static_cast<float>(1u << 24) * 2.0f - 1.0f) * 2.0f;
    }
    const auto path = mixFixture("i2_noise");
    mFiles.push_back(path);
    ASSERT_TRUE(wav::writeWav(path.c_str(), noise.data(), kLoop, kRate, wav::BitDepth::FLOAT_32));
    WmaEngine* e = offlineEngine();
    const int32_t a = loadClip(e, path.string());
    ASSERT_EQ(wma_mix_set_track(e, 0, 2.0f, 0.0f, 1.0f), WMA_OK);   // +-4 al bus
    WmaMixSpace sp = space(1.0f, 0.0f);
    sp.reverb_low_cut_hz = 20.0f;
    ASSERT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
    WmaMixSpace one = sp;
    one.reverb_damping = 1.0f;
    EXPECT_EQ(wma_mix_set_space(e, &one), WMA_ERROR_PARAMETER_OUT_OF_RANGE);
    const int64_t p0 = wma_transport_get_play_frame(e);
    const WmaSongSection s = section2(p0 + 2 * kLoop, a, WMA_SONG_SILENCE);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, &s, 1, p0 + 600 * kLoop), WMA_OK);
    ASSERT_GT(peakOf(renderTaps(e, 8, 1).fx), 0.1f);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, nullptr, 0, 0), WMA_OK);
    sp.reverb_damping = 0.95f;   // durante la cola
    ASSERT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
    skip(e, static_cast<int64_t>(kRate) * 30);
    const Rendered late = renderTaps(e, 4, 1);
    EXPECT_LT(peakOf(late.fx), 1e-6f);
    EXPECT_FALSE(e->engine->getAudioLooper().isSongBusLive()) << "el bus de la reverb no se apaga";
}

// Defecto (BLOQUEANTE del review de 8855c93d): una muestra NaN/Inf en un clip entraba al bus; reverb y
// delay ponen en cero su salida no finita pero sus lineas internas quedaban en NaN para siempre, el bus
// se apagaba «por silencio» sin marcarse viejo y la cancion limpia que venia despues daba FX 0. Con el
// bus REAL: el envio se sanea antes de entrar (el bloque se cuenta), el FX es finito, y una cancion
// limpia posterior suena bit a bit igual que en un motor cuyo clip tenia un 0 en esa muestra.
TEST_F(SongMix, ANonFiniteSampleInAClipNeverPoisonsTheRealBusSoALaterCleanSongSoundsTheSame) {
    constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
    constexpr float kInf = std::numeric_limits<float>::infinity();
    struct Case { const char* name; float bad; float reverb; float delay; };
    for (const Case& c : {Case{"nan_rv", kNan, 1.0f, 0.0f}, Case{"inf_rv", kInf, 1.0f, 0.0f},
                          Case{"ninf_rv", -kInf, 1.0f, 0.0f}, Case{"nan_dl", kNan, 0.0f, 1.0f},
                          Case{"inf_dl", kInf, 0.0f, 1.0f}}) {
        SCOPED_TRACE(c.name);
        const auto run = [&](const std::string& tag, float bad, int64_t& guardTrips) {
            const std::string poisoned = tone(tag + "_p", 100.0f, 0.0f, 0.3f, 100, bad);
            const std::string clean = tone(tag + "_c", 75.0f, 1.0f);
            WmaEngine* e = offlineEngine();
            const int32_t a = loadClip(e, poisoned), b = loadClip(e, clean);
            EXPECT_EQ(wma_mix_set_track(e, 0, 1.0f, 0.0f, 1.0f), WMA_OK);
            WmaMixSpace sp = space(c.reverb, c.delay);
            sp.delay_time_ms = 100.0f;
            EXPECT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
            const int64_t p0 = wma_transport_get_play_frame(e);
            const WmaSongSection s1 = section2(p0 + 2 * kLoop, a, WMA_SONG_SILENCE);
            EXPECT_EQ(wma_song_set_schedule(e, kLoop, &s1, 1, p0 + 600 * kLoop), WMA_OK);
            Rendered first = renderTaps(e, 6, 1);
            EXPECT_EQ(wma_song_set_schedule(e, kLoop, nullptr, 0, 0), WMA_OK);
            const Rendered tail = renderTaps(e, 20, 1);
            const int64_t p1 = wma_transport_get_play_frame(e);
            const WmaSongSection s2 = section2(p1 + 2 * kLoop, b, WMA_SONG_SILENCE);
            EXPECT_EQ(wma_song_set_schedule(e, kLoop, &s2, 1, p1 + 600 * kLoop), WMA_OK);
            Rendered second = renderTaps(e, 8, 1);
            guardTrips = e->engine->getAudioLooper().getSongBusGuardTrips();
            for (const float v : first.fx) EXPECT_TRUE(std::isfinite(v)) << "el NaN llego al retorno";
            for (const float v : tail.fx) EXPECT_TRUE(std::isfinite(v)) << "el NaN llego a la cola";
            for (const float v : second.fx) EXPECT_TRUE(std::isfinite(v));
            first.fx.insert(first.fx.end(), tail.fx.begin(), tail.fx.end());
            first.fx.insert(first.fx.end(), second.fx.begin(), second.fx.end());
            return first.fx;
        };
        int64_t trips = 0, controlTrips = 0;
        const std::vector<float> withBad = run(std::string(c.name) + "_a", c.bad, trips);
        const std::vector<float> control = run(std::string(c.name) + "_b", 0.0f, controlTrips);
        EXPECT_GE(trips, 1) << "el envio no finito no se conto";
        EXPECT_EQ(controlTrips, 0);
        EXPECT_GT(peakOf(control, control.size() - static_cast<size_t>(kLoop) * 2), 0.01f)
            << "la cancion limpia de control no suena: el test no prueba nada";
        EXPECT_TRUE(sameBits(withBad, control)) << "el bus quedo envenenado: la cancion limpia suena distinto";
    }
}

// Defecto (BLOQUEANTE del review de 042a0c27): el saneo solo descartaba lo no finito. Una muestra
// FINITA enorme (3e38, un float WAV crudo) hace que la reverb sume L+R y desborde a Inf: sus lineas
// quedaban en NaN, la salida en 0 y el bus muerto sin contarse. Con el bus REAL el envio se acota a
// ±kSongSendLimit (y el bloque se cuenta): la cancion limpia posterior da el FX del control.
TEST_F(SongMix, AHugeFiniteSampleInAClipIsClampedSoALaterCleanSongSoundsLikeTheControl) {
    struct Case { const char* name; float bad; float reverb; float delay; };
    for (const Case& c : {Case{"big_rv", 3e38f, 1.0f, 0.0f}, Case{"big_dl", 3e38f, 0.0f, 1.0f},
                          Case{"mid_rv", 1e37f, 1.0f, 0.0f}, Case{"mid_dl", 1e37f, 0.0f, 1.0f},
                          Case{"nbig_rv", -3e38f, 1.0f, 0.0f}}) {
        SCOPED_TRACE(c.name);
        const auto run = [&](const std::string& tag, float bad, int64_t& guardTrips) {
            const std::string poisoned = tone(tag + "_p", 100.0f, 0.0f, 0.3f, 100, bad);
            const std::string clean = tone(tag + "_c", 75.0f, 1.0f);
            WmaEngine* e = offlineEngine();
            const int32_t a = loadClip(e, poisoned), b = loadClip(e, clean);
            EXPECT_EQ(wma_mix_set_track(e, 0, 1.0f, 0.0f, 1.0f), WMA_OK);
            WmaMixSpace sp = space(c.reverb, c.delay);
            sp.delay_time_ms = 100.0f;
            EXPECT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
            const int64_t p0 = wma_transport_get_play_frame(e);
            const WmaSongSection s1 = section2(p0 + 2 * kLoop, a, WMA_SONG_SILENCE);
            EXPECT_EQ(wma_song_set_schedule(e, kLoop, &s1, 1, p0 + 600 * kLoop), WMA_OK);
            const Rendered first = renderTaps(e, 6, 1);
            EXPECT_EQ(wma_song_set_schedule(e, kLoop, nullptr, 0, 0), WMA_OK);
            (void)renderTaps(e, 20, 1);
            const int64_t p1 = wma_transport_get_play_frame(e);
            const WmaSongSection s2 = section2(p1 + 2 * kLoop, b, WMA_SONG_SILENCE);
            EXPECT_EQ(wma_song_set_schedule(e, kLoop, &s2, 1, p1 + 600 * kLoop), WMA_OK);
            const Rendered second = renderTaps(e, 8, 1);
            guardTrips = e->engine->getAudioLooper().getSongBusGuardTrips();
            for (const float v : first.fx) EXPECT_TRUE(std::isfinite(v));
            for (const float v : second.fx) EXPECT_TRUE(std::isfinite(v));
            return second.fx;
        };
        int64_t trips = 0, controlTrips = 0;
        const std::vector<float> withBad = run(std::string(c.name) + "_a", c.bad, trips);
        const std::vector<float> control = run(std::string(c.name) + "_b", 0.0f, controlTrips);
        EXPECT_GE(trips, 1) << "el envio enorme no se conto";
        EXPECT_EQ(controlTrips, 0);
        ASSERT_EQ(withBad.size(), control.size());
        EXPECT_GT(peakOf(control, control.size() - static_cast<size_t>(kLoop) * 2), 0.01f)
            << "la cancion limpia de control no suena: el test no prueba nada";
        float worst = 0.0f;
        for (size_t i = 0; i < control.size(); ++i) worst = std::max(worst, std::abs(withBad[i] - control[i]));
        // No es bit a bit: el pico recortado a 64 deja una cola VIVA en el bus (isSongBusLive() es
        // true, ~-82 dBFS de salida) que asoma ~1e-4 en la cancion siguiente. Un bus envenenado da
        // un FX de 0 (diferencia ~0,2, el pico del control) o de ±4 (la linea en el limite).
        EXPECT_LE(worst, 5e-4f) << "el bus quedo envenenado: la cancion limpia suena distinto";
    }
}

// Defecto (BLOQUEANTE del review de 8855c93d): la guarda por pico absoluto (> 16) cortaba material
// legitimo. La salida wet del delay llega a ~10x el envio con feedback 0,9 y material periodico; con
// ganancia 2, paneo −1, seno de 100 Hz a escala plena, envio 1 y 250 ms, el FX pasaba de 16 cada
// ~4 s, se ponia a cero y el bus se reiniciaba (memset de 1,5 MB en el RT). Ahora: ni un disparo, el
// FX nunca baja a cero en un bloque una vez que suena, y su salto entre muestras queda acotado.
TEST_F(SongMix, LoudPeriodicMaterialThroughTheDelayAtMaximumFeedbackIsNeverCutOrResetAndIsContinuous) {
    for (const float delayReturn : {0.25f, 0.0f}) {
        SCOPED_TRACE(delayReturn);
        const std::string pa = tone(delayReturn > 0.0f ? "pk_a" : "pk_b", 480.0f, 0.0f, 1.0f);
        WmaEngine* e = offlineEngine();
        const int32_t a = loadClip(e, pa);
        ASSERT_EQ(wma_mix_set_track(e, 0, 2.0f, -1.0f, 1.0f), WMA_OK);
        WmaMixSpace sp = space(0.3f, delayReturn);
        sp.delay_time_ms = 250.0f;
        sp.delay_feedback = 0.9f;
        ASSERT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
        const int64_t p0 = wma_transport_get_play_frame(e);
        const WmaSongSection s = section2(p0 + 2 * kLoop, a, WMA_SONG_SILENCE);
        ASSERT_EQ(wma_song_set_schedule(e, kLoop, &s, 1, p0 + 400 * kLoop), WMA_OK);
        const Rendered r = renderTaps(e, 220, 1);   // 22 s: la guarda vieja disparaba cada ~4 s
        EXPECT_EQ(e->engine->getAudioLooper().getSongBusGuardTrips(), 0);
        const size_t from = static_cast<size_t>(kRate) * 2;   // pasados los primeros 2 s
        double step = 0.0;
        for (size_t i = from + 2; i < r.fx.size(); i += 2) {
            step = std::max(step, double(std::abs(r.fx[i] - r.fx[i - 2])));
        }
        const float peak = peakOf(r.fx);
        for (size_t blockStart = from; blockStart + kBlock * 2 <= r.fx.size(); blockStart += kBlock * 2) {
            ASSERT_GT(peakOf(r.fx, blockStart, blockStart + kBlock * 2), 1e-4f)
                << "un bloque del FX quedo en cero en " << blockStart / 2 << " frames";
        }
        EXPECT_LT(step, 0.3 * peak) << "salto en el FX: un corte";
    }
}

// Defecto (BLOQUEANTE del review): prepareMixBuffer agranda los buffers del looper pero no el bus; un
// bloque mayor que el delay del bus escribe fuera de memoria (heap overflow bajo ASan en
// DelayEffect::process) y el contador de saltos queda en 0. El export offline no pasa de 4096
// frames, asi que el escenario del callback vivo (prepareMixBuffer(8192), bloque de 6000) se maneja
// con el looper y el bus REAL a mano. Con la capacidad del bus creciendo con los buffers el bloque
// corre con el bus, devuelve algo, y nada se salta.
TEST_F(SongMix, ABlockLargerThanThePreparedMixBufferStillRunsTheRealBusWithoutOverflowing) {
    constexpr int kBig = 6000;
    const std::string pa = tone("g_a", 100.0f, 0.0f);
    AudioLooper looper;
    looper.setEnabled(true);
    wm::SongSpaceBusImpl bus;
    looper.publishSongBus(&bus, 4096);
    looper.setSongMixTrack(0, 1.0f, 0.0f, 1.0f);
    wm::SongSpaceParams params;
    params.delayReturn = 1.0f;
    params.delayTimeMs = 125.0f;
    looper.setSongMixSpace(params);
    looper.prepareMixBuffer(8192);
    ASSERT_GE(bus.spaceBusCapacityFrames(), 8192);
    int32_t id = -1;
    ASSERT_EQ(looper.songLoadClip(pa.c_str(), kRate, 0, 0, kLoop, false, id), wm::SongStatus::Ok);
    wm::SongSection sec;
    sec.startFrame = 0;
    sec.clip[0] = id;
    ASSERT_EQ(looper.songSetSchedule(kLoop, &sec, 1, int64_t{1} << 24, false), wm::SongStatus::Ok);
    std::vector<float> out(kBig * 2);
    float peak = 0.0f;
    for (int b = 0; b < 12; ++b) {
        std::fill(out.begin(), out.end(), 0.0f);
        const int64_t frame = static_cast<int64_t>(b) * kBig;
        looper.beginBlock(frame, kBig);
        looper.process(out.data(), kBig, frame);
        peak = std::max(peak, peakOf(out));
    }
    EXPECT_EQ(looper.getSongBusOverflowBlocks(), 0);
    EXPECT_GT(peak, 0.1f);
}

// Defecto (hallazgo 7 del review): la primera aplicacion de parametros de un bus fresco corre en el
// callback y hacia DelayEffect::reset (~1,5 MB de memset en el RT), redundante con el reset de
// prepareSpaceBus en control. Lo unico que el RT tiene que hacer es clavar el tiempo del delay en
// su objetivo SIN vaciar la linea: lo que entro al delay antes de esa primera aplicacion sigue ahi
// y su eco llega al tiempo pedido (125 ms = 6000 frames).
TEST_F(SongMix, TheFirstParameterApplyPinsTheDelayTimeWithoutClearingTheLine) {
    wm::SongSpaceBusImpl bus;
    bus.prepareSpaceBus(kRate, 4096);
    std::vector<float> send(kBlock * 2, 0.0f), fx(kBlock * 2, 0.0f);
    send[0] = send[1] = 1.0f;   // un impulso en el frame 0, antes de aplicar nada
    bus.runSpaceBus(send.data(), fx.data(), kBlock);
    wm::SongSpaceParams p;
    p.delayReturn = 1.0f;
    p.delayTimeMs = 125.0f;
    p.delayFeedback = 0.0f;
    bus.applySpaceParams(p);   // la primera: clava el tiempo, no vacia
    std::fill(send.begin(), send.end(), 0.0f);
    float echo = 0.0f;
    int64_t echoAt = -1;
    for (int64_t done = kBlock; done < 8000; done += kBlock) {
        bus.runSpaceBus(send.data(), fx.data(), kBlock);
        for (int i = 0; i < kBlock; ++i) {
            const float v = std::abs(fx[static_cast<size_t>(i) * 2]);
            if (v > echo) {
                echo = v;
                echoAt = done + i;
            }
        }
    }
    EXPECT_GT(echo, 0.2f) << "el primer apply vacio la linea del delay (memset en el RT)";
    EXPECT_NEAR(static_cast<double>(echoAt), 6000.0, 4.0) << "el tiempo no esta clavado en su objetivo";
}

// Defecto (hallazgo 4): un cambio de sample rate no re-prepara el bus y el delay conserva su tiempo
// en muestras del rate viejo. 125 ms son 6000 muestras a 48 kHz y 5512 a 44,1 kHz: el primer eco
// (retorno del delay solo, realimentacion 0) tiene que llegar al retardo del rate NUEVO.
TEST_F(SongMix, TheDelayTimeFollowsASampleRateChangeOfTheEngine) {
    constexpr int kNewRate = 44100;
    constexpr int kLoop2 = 4410;
    std::vector<float> clip(static_cast<size_t>(kLoop2) * 2, 0.0f);
    for (int n = 0; n < kLoop2; ++n) {
        clip[static_cast<size_t>(n) * 2] = clip[static_cast<size_t>(n) * 2 + 1] = 0.5f;   // DC
    }
    const auto path = mixFixture("sr_dc");
    mFiles.push_back(path);
    ASSERT_TRUE(wav::writeWav(path.c_str(), clip.data(), kLoop2, kNewRate, wav::BitDepth::FLOAT_32));

    WmaEngine* e = offlineEngine();   // 48 kHz: aca nace el bus
    ASSERT_EQ(wma_mix_set_track(e, 0, 1.0f, 0.0f, 1.0f), WMA_OK);
    WmaMixSpace sp = space(0.0f, 1.0f);
    sp.delay_feedback = 0.0f;
    ASSERT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
    ASSERT_EQ(wma_offline_start(e, kNewRate, kBlock), WMA_OK);   // 44,1 kHz: el bus se re-prepara
    ASSERT_EQ(wma_mix_set_output_profile(e, WMA_OUTPUT_RAW), WMA_OK);
    int32_t id = -1;
    ASSERT_EQ(wma_song_load_clip(e, path.c_str(), kNewRate, 0, 0, kLoop2, &id), WMA_OK);
    // La cancion arranca con DC desde el frame 0 de la seccion (despues del fundido de entrada).
    const int64_t p0 = wma_transport_get_play_frame(e);
    const WmaSongSection s = section2(p0 + 1000, id, WMA_SONG_SILENCE);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop2, &s, 1, p0 + 100 * kLoop2), WMA_OK);
    constexpr int kFrames = 12000;
    std::vector<float> mix(kBlock * 2), fx(kBlock * 2), tr(kBlock * 2);
    int64_t firstDry = -1, firstFx = -1;
    for (int done = 0; done < kFrames; done += kBlock) {
        ASSERT_EQ(wma_offline_render_taps(e, mix.data(), fx.data(), tr.data(), 1, kBlock), WMA_OK);
        for (int i = 0; i < kBlock; ++i) {
            if (firstDry < 0 && std::abs(tr[static_cast<size_t>(i) * 2]) > 1e-4f) firstDry = done + i;
            if (firstFx < 0 && std::abs(fx[static_cast<size_t>(i) * 2]) > 1e-4f) firstFx = done + i;
        }
    }
    ASSERT_GE(firstDry, 0);
    ASSERT_GE(firstFx, 0) << "el delay no devolvio nada";
    const double delay125ms = 125.0 * kNewRate / 1000.0;   // 5512,5 muestras
    EXPECT_NEAR(static_cast<double>(firstFx - firstDry), delay125ms, 4.0)
        << "el delay conserva el tiempo en muestras del rate viejo (6000)";
}

// Defecto: el costo de la pasada del export (motor + bus + 8 taps de pista) cae por debajo del
// piso de 10x tiempo real que prometio el diseno. CPU DEL HILO (el reloj de pared mediria la carga de
// la maquina). Debug y sanitizers: SKIPPED, nunca PASSED ni con el umbral bajado; se corre con la
// suite en Release (docs/dev/mezcla-y-master.md).
TEST_F(SongMix, APassWithEightTrackTapsRendersAtLeastTenTimesFasterThanRealTime) {
    WMA_SKIP_IF_SANITIZED();
#if !defined(__OPTIMIZE__)
    GTEST_SKIP() << "el costo se mide en un build optimizado (la suite del gate es Debug)";
#endif
    constexpr double kMinRealTimeFactor = 10.0;
    constexpr int kSeconds = 10;
    constexpr int kBlockFrames = 4096;   // el bloque del export (ADR-0013 §10)
    constexpr int kTracks = 8;
    WmaEngine* e = wma_engine_create();
    mEngines.push_back(e);
    wma_set_audio_mode(e, 0);
    wma_set_synth_volume(e, 0.0f);
    ASSERT_EQ(wma_offline_start(e, kRate, kBlockFrames), WMA_OK);
    ASSERT_EQ(wma_mix_set_output_profile(e, WMA_OUTPUT_RAW), WMA_OK);
    WmaSongSection sec{};
    sec.start_frame = wma_transport_get_play_frame(e) + kLoop;
    for (int t = 0; t < WMA_SONG_MAX_TRACKS; ++t) sec.clip[t] = WMA_SONG_SILENCE;
    for (int t = 0; t < kTracks; ++t) {
        sec.clip[t] = loadClip(e, tone("perf_" + std::to_string(t), 50.0f + 13.0f * t, 0.2f * t));
        ASSERT_EQ(wma_mix_set_track(e, t, 0.7f, -0.8f + 0.2f * t, 0.4f), WMA_OK);
    }
    const WmaMixSpace sp = space(0.6f, 0.5f);
    ASSERT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, &sec, 1, sec.start_frame + (kSeconds + 1) * kRate), WMA_OK);
    std::vector<float> mix(kBlockFrames * 2), fx(kBlockFrames * 2), tracks(static_cast<size_t>(kTracks) * kBlockFrames * 2);
    const int blocks = kSeconds * kRate / kBlockFrames;
    timespec a{}, b{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &a);
    for (int i = 0; i < blocks; ++i) {
        ASSERT_EQ(wma_offline_render_taps(e, mix.data(), fx.data(), tracks.data(), kTracks, kBlockFrames), WMA_OK);
    }
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b);
    const double cpu = static_cast<double>(b.tv_sec - a.tv_sec) + static_cast<double>(b.tv_nsec - a.tv_nsec) * 1e-9;
    const double factor = static_cast<double>(blocks) * kBlockFrames / kRate / cpu;
    std::printf("[ COSTO   ] pasada con bus + %d taps de pista, bloque %d: %.1fx tiempo real (%.3f s de CPU)\n",
                kTracks, kBlockFrames, factor, cpu);
    RecordProperty("real_time_factor", std::to_string(factor));
    EXPECT_GE(factor, kMinRealTimeFactor);
}

// Defecto (hallazgo 2 del re-review): la cola no tiene tope, asi que el bus corre con envio cero
// hasta que sus lineas se agotan (Ambient a 60 bpm: delay 1,5 s, realimentacion 0,5, ~35 s). Lo que
// cuesta mantenerlo vivo por callback de 256 frames tiene que ser una fraccion chica del periodo.
// CPU DEL HILO; Debug y sanitizers: SKIPPED. Imprime `[ COLA    ]`.
TEST_F(SongMix, KeepingTheBusAliveForTheTailCostsAFractionOfTheCallbackPeriod) {
    WMA_SKIP_IF_SANITIZED();
#if !defined(__OPTIMIZE__)
    GTEST_SKIP() << "el costo se mide en un build optimizado (la suite del gate es Debug)";
#endif
    constexpr int kCb = 256;
    constexpr int kFive = 5 * kRate / kCb;
    WmaEngine* e = wma_engine_create();
    mEngines.push_back(e);
    wma_set_audio_mode(e, 0);
    wma_set_synth_volume(e, 0.0f);
    ASSERT_EQ(wma_offline_start(e, kRate, kCb), WMA_OK);
    ASSERT_EQ(wma_mix_set_output_profile(e, WMA_OUTPUT_RAW), WMA_OK);
    const int32_t a = loadClip(e, tone("cola", 100.0f, 0.0f));
    ASSERT_EQ(wma_mix_set_track(e, 0, 1.0f, 0.0f, 1.0f), WMA_OK);
    WmaMixSpace sp = space(0.45f, 0.45f);
    sp.delay_time_ms = 1500.0f;
    sp.delay_feedback = 0.5f;
    ASSERT_EQ(wma_mix_set_space(e, &sp), WMA_OK);
    const int64_t p0 = wma_transport_get_play_frame(e);
    const WmaSongSection s = section2(p0 + kLoop, a, WMA_SONG_SILENCE);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, &s, 1, p0 + 600 * kLoop), WMA_OK);
    std::vector<float> mix(kCb * 2), fx(kCb * 2), tr(kCb * 2);
    auto cpuUs = [&](int callbacks) {
        timespec t0{}, t1{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
        for (int i = 0; i < callbacks; ++i) {
            EXPECT_EQ(wma_offline_render_taps(e, mix.data(), fx.data(), tr.data(), 1, kCb), WMA_OK);
        }
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t1);
        const double s = static_cast<double>(t1.tv_sec - t0.tv_sec) + static_cast<double>(t1.tv_nsec - t0.tv_nsec) * 1e-9;
        return s * 1e6 / callbacks;
    };
    (void)cpuUs(2 * kRate / kCb);
    ASSERT_EQ(wma_song_set_schedule(e, kLoop, nullptr, 0, 0), WMA_OK);
    (void)cpuUs(2);
    const double live = cpuUs(kFive);   // la cola suena y el bus corre
    (void)cpuUs(100 * kRate / kCb);     // agotada
    const double idle = cpuUs(kFive);   // el bus ya se apago
    const double period = 1e6 * kCb / kRate;
    std::printf("[ COLA    ] callback de %d frames: cola viva %.2f us, bus apagado %.2f us (periodo %.0f us)\n",
                kCb, live, idle, period);
    RecordProperty("tail_cb_us", std::to_string(live));
    EXPECT_LT(live - idle, 0.10 * period) << "mantener el bus vivo cuesta mas del 10 % del periodo";
}

namespace {

struct LiveMix {
    WmaEngine* e = nullptr;
    FakeAudioBackend* fake = nullptr;
    void bringUp() {
        resetLastCreatedSystemBackend();
        e = wma_engine_create();
        ASSERT_NE(e, nullptr);
        fake = lastCreatedSystemBackend();
        ASSERT_NE(fake, nullptr);
        fake->setNegotiatedSampleRate(kRate);
        wma_set_use_backend_manager(e, true);
        ASSERT_TRUE(wma_select_backend(1));
        wma_set_audio_mode(e, 0);
        wma_set_master_volume(e, 1.0f);
        wma_set_frequency_amplitude(e, 440.0f, 0.0f);
        ASSERT_EQ(wma_engine_start(e, 0), WMA_OK);
    }
    void tearDown() {
        if (e == nullptr) return;
        wma_engine_stop(e, 0);
        wma_engine_destroy(e);
        e = nullptr;
    }
};

int64_t threadCpuNs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

}  // namespace

// Defecto: el callback aloca o se bloquea cuando llegan setters de mezcla (el bus se crea en RT,
// un setter toma un lock, los parametros del bus se aplican desde el hilo de control en vez de
// desde el que procesa -> carrera, que TSan ve). Una rafaga de setters de mezcla / espacio /
// master / perfil contra el callback en marcha con 8 pistas, bus y perfil MASTER. Mide el costo
// del callback contra su periodo (maximo y p99) con el metodo de AC-055.13.
TEST_F(SongMix, ABurstOfMixSettersAgainstTheCallbackAllocatesNothingAndStaysInsideThePeriod) {
    constexpr int kTracks = 8;
    LiveMix live;
    live.bringUp();
    ASSERT_NE(live.e, nullptr);
    ASSERT_EQ(wma_mix_set_output_profile(live.e, WMA_OUTPUT_RAW), WMA_ERROR_INVALID_OPERATION);
    ASSERT_EQ(wma_mix_set_output_profile(live.e, WMA_OUTPUT_MASTER), WMA_OK);
    int32_t clips[kTracks];
    for (int t = 0; t < kTracks; ++t) {
        clips[t] = loadClip(live.e, tone("live_" + std::to_string(t), 60.0f + 11.0f * t, 0.3f * t));
    }
    // una seccion con un clip por pista, republicada desde el principio (como Studio)
    for (int t = 0; t < kTracks; ++t) ASSERT_EQ(wma_mix_set_track(live.e, t, 0.5f, 0.2f * t - 0.7f, 0.3f), WMA_OK);
    const WmaMixSpace sp0 = space(0.5f, 0.4f);
    ASSERT_EQ(wma_mix_set_space(live.e, &sp0), WMA_OK);
    const WmaMixMaster m0 = {0.0f, -1.0f, 150.0f};
    ASSERT_EQ(wma_mix_set_master(live.e, &m0), WMA_OK);

    {
        std::vector<float> warm(512);
        for (int i = 0; i < 10; ++i) live.fake->callback()->onAudioReady(warm.data(), nullptr, 256);
        WmaSongSection s{};
        s.start_frame = wma_transport_get_play_frame(live.e) + 2 * kLoop;
        for (int t = 0; t < WMA_SONG_MAX_TRACKS; ++t) s.clip[t] = t < kTracks ? clips[t] : WMA_SONG_SILENCE;
        ASSERT_EQ(wma_song_set_schedule(live.e, kLoop, &s, 1, s.start_frame + 100000 * kLoop), WMA_OK);
    }

    constexpr int kCallbackFrames = 256;
    constexpr double kPeriodNs = 1e9 * kCallbackFrames / kRate;
    std::atomic<bool> stop{false};
    std::atomic<int64_t> allocs{-1};
    std::vector<int64_t> costs;
    costs.reserve(2000000);
    std::atomic<int64_t> blocks{0};
    std::atomic<int64_t> rtBlocks{0};
    std::thread rt([&] {
        std::vector<float> buf(static_cast<size_t>(kCallbackFrames) * 2);
        for (int i = 0; i < 40; ++i) live.fake->callback()->onAudioReady(buf.data(), nullptr, kCallbackFrames);
        armAllocCounter();
        while (!stop.load(std::memory_order_acquire)) {
            const int64_t t0 = threadCpuNs();
            live.fake->callback()->onAudioReady(buf.data(), nullptr, kCallbackFrames);
            if (costs.size() < costs.capacity()) costs.push_back(threadCpuNs() - t0);
            rtBlocks.fetch_add(1, std::memory_order_relaxed);
        }
        allocs.store(disarmAllocCounter());
        blocks.store(static_cast<int64_t>(costs.size()));
    });

    constexpr int kBursts = 1000;
    constexpr int64_t kMinBlocks = 6000;   // ~32 s de audio de callbacks reales
    for (int i = 0; i < kBursts || rtBlocks.load(std::memory_order_relaxed) < kMinBlocks; ++i) {
        for (int t = 0; t < kTracks; ++t) {
            const float g = 0.2f + 0.8f * static_cast<float>((i + t) % 5) / 4.0f;
            EXPECT_EQ(wma_mix_set_track(live.e, t, g, -1.0f + 0.25f * static_cast<float>((i + t) % 9),
                                        0.1f * static_cast<float>((i * 3 + t) % 11)), WMA_OK);
        }
        WmaMixSpace sp = space(0.1f * static_cast<float>(i % 10), 0.1f * static_cast<float>((i + 3) % 10));
        sp.delay_time_ms = 50.0f + static_cast<float>(i % 40) * 10.0f;
        sp.reverb_low_cut_hz = 40.0f + static_cast<float>(i % 20) * 20.0f;
        EXPECT_EQ(wma_mix_set_space(live.e, &sp), WMA_OK);
        const WmaMixMaster m = {static_cast<float>(i % 12) - 6.0f, -1.0f - 0.1f * static_cast<float>(i % 5),
                                50.0f + static_cast<float>(i % 7) * 20.0f};
        EXPECT_EQ(wma_mix_set_master(live.e, &m), WMA_OK);
        EXPECT_EQ(wma_mix_set_output_profile(live.e, (i % 2) != 0 ? WMA_OUTPUT_MASTER : WMA_OUTPUT_PROTECTED), WMA_OK);
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    rt.join();

    EXPECT_EQ(allocs.load(), 0) << "el callback alocó mientras llegaban setters de mezcla";
    EXPECT_EQ(live.e->engine->getAudioLooper().getSongBusOverflowBlocks(), 0);
    std::sort(costs.begin(), costs.end());
    ASSERT_FALSE(costs.empty());
    const double maxUs = static_cast<double>(costs.back()) / 1000.0;
    const double p99Us = static_cast<double>(costs[costs.size() * 99 / 100]) / 1000.0;
    std::printf("[ mix ] 8 pistas + bus + master, %d frames: %lld callbacks, max %.1f us, p99 %.1f us "
                "(periodo %.1f us)\n", kCallbackFrames, static_cast<long long>(blocks.load()), maxUs,
                p99Us, kPeriodNs / 1000.0);
#if defined(__OPTIMIZE__) && !defined(WMA_TEST_UNDER_SANITIZER)
    EXPECT_LT(static_cast<double>(costs.back()), kPeriodNs) << "un callback tardo mas que su periodo";
#endif
    live.tearDown();
}

}  // namespace wma_test
