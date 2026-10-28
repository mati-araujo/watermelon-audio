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

std::string writeTone(const std::string& name, float period, float phase, float amp) {
    std::vector<float> buf(static_cast<size_t>(kLoop) * 2);
    for (int n = 0; n < kLoop; ++n) {
        const float v = amp * std::sin(6.2831853f * static_cast<float>(n) / period + phase);
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

    std::string tone(const std::string& name, float period, float phase, float amp = 0.3f) {
        mFiles.push_back(mixFixture(name));
        return writeTone(name, period, phase, amp);
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
#ifndef WMA_TEST_UNDER_SANITIZER
    EXPECT_LT(static_cast<double>(costs.back()), kPeriodNs) << "un callback tardo mas que su periodo";
#endif
    live.tearDown();
}

}  // namespace wma_test
