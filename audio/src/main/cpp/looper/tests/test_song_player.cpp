// ============================================================================
// test_song_player — la cancion programada por frame (REQ-055 S11), a nivel de AudioLooper.
//
// El looper se maneja a mano, bloque a bloque, como lo hace el motor: `beginBlock(frame, n)` y
// `process(buf, n, frame)`. Sin motor, sin OutputStage: lo que se mide es la salida del looper,
// donde la envolvente del fundido es analitica y no la tapa un limitador.
//
// AC-055.12: la cancion programada suena como se armo (fase por seccion, fundidos en las
//            uniones, volumen/paneo/mute/envio respetados).
// AC-055.13: reprogramar mientras suena no corta (adopcion tardia con fundido).
// Cada test dice que bug plausible atrapa.
// ============================================================================
#include "tests/support/TestWait.h"
#include "support/FixturePath.h"

#include <gtest/gtest.h>
#include "AudioLooper.h"
#include "SongPlayer.h"
#include "WavFile.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kSR = 48000;
constexpr int kF = 240;                     // songFadeFrames(48000)
constexpr int64_t kBlockOffset = 37;        // el borde cae dentro del bloque, no en su limite
constexpr int64_t kStride = 4096;           // multiplo de todos los tamaños de bloque probados

static_assert(wm::kSongMaxTracks == 16);

/// Ganancia del paneo al centro: lo que ve una voz con vol=1, pan=0. La tabla no es simetrica al
/// bit: L y R difieren en el tercer decimal, asi que cada canal se compara con el suyo.
float centerGain(int ch = 0) {
    const auto pp = wm::EqualPowerPanLUT::instance().lookup(0.0f);
    return ch == 0 ? pp.l : pp.r;
}

/// Un looper con clips en archivos y un render bloque a bloque.
class Rig {
public:
    Rig() { looper.setEnabled(true); }
    ~Rig() {
        for (const auto& p : mPaths) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    }

    std::string write(const std::string& name, int frames,
                      const std::function<float(int, int)>& sample, int rate = kSR) {
        const auto path = wma_test::fixturePath("song_" + name + ".wav");
        std::vector<float> buf(static_cast<size_t>(frames) * 2);
        for (int n = 0; n < frames; ++n) {
            buf[static_cast<size_t>(n) * 2] = sample(n, 0);
            buf[static_cast<size_t>(n) * 2 + 1] = sample(n, 1);
        }
        EXPECT_TRUE(wav::writeWav(path.c_str(), buf.data(), frames, rate, wav::BitDepth::FLOAT_32));
        mPaths.push_back(path);
        return path.string();
    }

    /// Carga un clip de `loop` frames desde un audio nuevo; devuelve su id.
    int32_t load(const std::string& name, int frames, const std::function<float(int, int)>& sample,
                 int64_t loop, int64_t rs = 0, int64_t re = 0) {
        const std::string path = write(name, frames, sample);
        int32_t id = -1;
        EXPECT_EQ(looper.songLoadClip(path.c_str(), kSR, rs, re, loop, false, id),
                  wm::SongStatus::Ok);
        return id;
    }

    wm::SongStatus schedule(int64_t loop, const std::vector<wm::SongSection>& secs, int64_t end) {
        return looper.songSetSchedule(loop, secs.data(), static_cast<int32_t>(secs.size()), end,
                                      false);
    }

    /// Renderiza [from, from + frames) en bloques de `block`; el bloque siguiente arranca donde
    /// termino el anterior, asi que el borde de una seccion cae donde le toque.
    std::vector<float> render(int64_t from, int64_t frames, int block) {
        std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
        for (int64_t done = 0; done < frames; done += block) {
            const int n = static_cast<int>(std::min<int64_t>(block, frames - done));
            looper.beginBlock(from + done, n);
            looper.process(out.data() + static_cast<size_t>(done) * 2, n, from + done);
        }
        return out;
    }

    AudioLooper looper;

private:
    std::vector<std::filesystem::path> mPaths;
};

wm::SongSection sec(int64_t start, int32_t clip0, int32_t clip1 = wm::kSongSilence) {
    wm::SongSection s;
    s.startFrame = start;
    s.clip[0] = clip0;
    s.clip[1] = clip1;
    return s;
}

auto dc(float v) {
    return [v](int, int) { return v; };
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Fundidos: la envolvente es analitica, muestra a muestra, y no depende del bloque
// ---------------------------------------------------------------------------------------

// Bug plausible: la envolvente se calcula por bloque (rampa lineal por bloque, fundido que
// arranca en el limite del bloque) en vez de por frame absoluto: con el borde en bloque+37 la
// forma depende del tamaño de bloque y el fundido no termina en B. DC porque cualquier
// desviacion de la ganancia se ve directo. Cubre los tres fundidos: entrar a silencio-sonido
// (coseno elevado), cambio de clip (equal-power), salir a silencio y el fin de la agenda.
TEST(SongFade, AC_055_12_EnvelopeIsAnalyticSampleBySampleAndBitIdenticalAcrossBlockSizes) {
    constexpr int64_t kLoop = 2048;
    const int64_t s0 = 2 * kStride + kBlockOffset;
    const int64_t s1 = s0 + kStride, s2 = s1 + kStride, s3 = s2 + kStride, end = s3 + kStride;

    const auto expected = [&](int64_t f) -> double {
        const auto cosIn = [&](int64_t b) {
            return 0.5 * (1.0 - std::cos(M_PI * double(f - (b - kF)) / kF));
        };
        const auto cosOut = [&](int64_t b) {
            return 0.5 * (1.0 + std::cos(M_PI * double(f - (b - kF)) / kF));
        };
        const double th = M_PI / 2 * double(f - (s1 - kF)) / kF;
        if (f < s0 - kF) return 0.0;
        if (f < s0) return 0.5 * cosIn(s0);
        if (f < s1 - kF) return 0.5;
        if (f < s1) return 0.5 * std::cos(th) + 0.25 * std::sin(th);
        if (f < s2 - kF) return 0.25;
        if (f < s2) return 0.25 * cosOut(s2);
        if (f < s3 - kF) return 0.0;
        if (f < s3) return 0.5 * cosIn(s3);
        if (f < end - kF) return 0.5;
        if (f < end) return 0.5 * cosOut(end);
        return 0.0;
    };

    const int64_t total = end + 1000;
    std::vector<float> reference;
    for (int block : {1, 64, 256, 4096}) {
        SCOPED_TRACE(block);
        // Cada tamaño arranca con un looper limpio: la comparacion es entre renders, no entre
        // dos pasadas del mismo estado.
        Rig fresh;
        const int32_t fx = fresh.load("env_x", kLoop, dc(0.5f), kLoop);
        const int32_t fz = fresh.load("env_z", kLoop, dc(0.25f), kLoop);
        ASSERT_EQ(fresh.schedule(kLoop,
                                 {sec(s0, fx), sec(s1, fz), sec(s2, wm::kSongSilence), sec(s3, fx)},
                                 end),
                  wm::SongStatus::Ok);
        const std::vector<float> out = fresh.render(0, total, block);
        double worst = 0.0;
        int64_t worstAt = -1;
        int64_t inFades = 0;
        for (int64_t f = 0; f < total; ++f) {
            for (int ch = 0; ch < 2; ++ch) {
                const double want = expected(f) * centerGain(ch);
                const double d = std::abs(out[static_cast<size_t>(f) * 2 + ch] - want);
                if (d > worst) {
                    worst = d;
                    worstAt = f;
                }
            }
            if ((f >= s0 - kF && f < s0) || (f >= s1 - kF && f < s1) ||
                (f >= s2 - kF && f < s2) || (f >= s3 - kF && f < s3) || (f >= end - kF && f < end)) {
                ++inFades;
            }
        }
        EXPECT_EQ(inFades, 5 * kF);
        EXPECT_LT(worst, 1e-6) << "la envolvente se aparta de la analitica, peor en el frame "
                               << worstAt << " (obtenido " << out[static_cast<size_t>(worstAt) * 2] << ", esperado "
                               << expected(worstAt) * centerGain() << ", s0=" << s0 << " s1=" << s1 << " s2=" << s2
                               << " s3=" << s3 << " end=" << end << ")";
        if (reference.empty()) {
            reference = out;
        } else {
            ASSERT_EQ(out.size(), reference.size());
            EXPECT_EQ(std::memcmp(out.data(), reference.data(), out.size() * sizeof(float)), 0)
                << "la salida depende del tamaño de bloque";
        }
    }
}

// Bug plausible: el estado del borde se adelanta o se atrasa un frame (el downbeat entra en
// B-1 o en B+1), o la pre-entrada lee el frame 0 del clip nuevo en vez de su cola y mete el
// impulso ANTES de B. Con el borde en bloque+37 y bloques de 1/64/256/4096.
TEST(SongFade, AC_055_12_ImpulseLandsExactlyAtTheBorderForEveryBlockSize) {
    constexpr int64_t kLoop = 2048;
    const int64_t s0 = 2 * kStride + kBlockOffset;
    const int64_t border = s0 + kStride;
    const int64_t end = border + 3000;
    std::vector<float> reference;
    for (int block : {1, 64, 256, 4096}) {
        SCOPED_TRACE(block);
        Rig rig;
        const int32_t quiet = rig.load("imp_q", kLoop, dc(0.0f), kLoop);
        const int32_t impulse =
            rig.load("imp_y", kLoop, [](int n, int) { return n == 0 ? 1.0f : 0.0f; }, kLoop);
        ASSERT_EQ(rig.schedule(kLoop, {sec(s0, quiet), sec(border, impulse)}, end),
                  wm::SongStatus::Ok);
        const std::vector<float> out = rig.render(0, end + 500, block);
        std::vector<int64_t> hits;
        for (int64_t f = 0; f * 2 < static_cast<int64_t>(out.size()); ++f) {
            for (int ch = 0; ch < 2; ++ch) {
                if (out[static_cast<size_t>(f) * 2 + ch] != 0.0f) {
                    hits.push_back(f);
                    EXPECT_FLOAT_EQ(out[static_cast<size_t>(f) * 2 + ch], centerGain(ch))
                        << "frame " << f;
                }
            }
        }
        // El impulso entra en B y vuelve a pasar una vuelta de clip despues (fase 0 otra vez).
        ASSERT_EQ(hits.size(), 4u) << "dos impulsos, dos canales";
        EXPECT_EQ(hits[0], border);
        EXPECT_EQ(hits[2], border + kLoop);
        if (reference.empty()) {
            reference = out;
        } else {
            EXPECT_EQ(std::memcmp(out.data(), reference.data(), out.size() * sizeof(float)), 0);
        }
    }
}

// Bug plausible: la fase se reinicia en cada bloque o se toma del playhead de la pista en vez
// de (frame - start) mod L: un loop que no es multiplo del bloque sale desfasado, y el
// reinicio por seccion ("el loop se reinicia en cada seccion") no ocurre.
TEST(SongPhase, AC_055_12_EachSectionRestartsTheLoopAtItsStart) {
    constexpr int64_t kLoop = 1000;
    Rig rig;
    const int32_t ramp = rig.load("ph_ramp", kLoop, [](int n, int) { return float(n) / 1000.0f; },
                                  kLoop);
    const int64_t a = 3000 + 7, b = a + 3 * kLoop + 450;   // la seccion 0 termina a mitad de vuelta
    ASSERT_EQ(rig.schedule(kLoop, {sec(a, ramp), sec(b, ramp)}, b + 2000), wm::SongStatus::Ok);
    const std::vector<float> out = rig.render(0, b + 2500, 256);
    const float g = centerGain();
    for (int64_t f : {a + 300, a + kLoop + 300, a + 2 * kLoop + 999 - 300}) {
        SCOPED_TRACE(f);
        EXPECT_NEAR(out[static_cast<size_t>(f) * 2], float((f - a) % kLoop) / 1000.0f * g, 1e-6);
    }
    // Seccion 1: la rampa arranca de cero en b + 300 (aunque la 0 iba por la fase 450), y no hay
    // fundido ahi porque la fase de salida (450) no coincide: hay salto de fase -> equal-power.
    for (int64_t f : {b + 300, b + 700, b + kLoop + 100}) {
        SCOPED_TRACE(f);
        EXPECT_NEAR(out[static_cast<size_t>(f) * 2], float((f - b) % kLoop) / 1000.0f * g, 1e-6);
    }
}

// Bug plausible: el mismo clip que sigue alineado en la seccion siguiente recibe un fundido
// igual (una "toma que sigue" se ensucia en cada union), o un salto de fase NO recibe fundido.
TEST(SongFade, AC_055_12_SameClipStaysUntouchedWhenAlignedAndFadesWhenItJumps) {
    constexpr int64_t kLoop = 1000;
    Rig rig;
    // 10 periodos exactos: el clip es un loop sin costura, como una toma de verdad
    const auto wave = [](int n, int) { return std::sin(6.2831853f * float(n) / 100.0f); };
    const int32_t ramp = rig.load("al_wave", kLoop, wave, kLoop);
    const int64_t a = 3000;
    const int64_t alignedBorder = a + 2 * kLoop;          // fase 0 al salir y al entrar: alineado
    const int64_t jumpBorder = alignedBorder + kLoop + 425;   // sale en la fase 425 (seno = 1)
    const int64_t end = jumpBorder + 2000;
    ASSERT_EQ(rig.schedule(kLoop, {sec(a, ramp), sec(alignedBorder, ramp), sec(jumpBorder, ramp)},
                           end),
              wm::SongStatus::Ok);
    const std::vector<float> out = rig.render(0, end + 100, 256);
    const float g = centerGain();
    for (int64_t f = alignedBorder - kF; f < alignedBorder; ++f) {
        EXPECT_NEAR(out[static_cast<size_t>(f) * 2], wave(int((f - a) % kLoop), 0) * g, 1e-6)
            << "el clip alineado se toco en el frame " << f;
    }
    // Salto de fase: sale en la fase 425 (seno = 1) y entra en la 0; sin fundido habria un
    // escalon de ~1 entre jumpBorder-1 y jumpBorder, con fundido solo el paso natural del seno.
    const float before = out[static_cast<size_t>(jumpBorder - 1) * 2];
    const float after = out[static_cast<size_t>(jumpBorder) * 2];
    EXPECT_LT(std::abs(after - before), 0.15f) << "salto de fase sin fundido";
}

// ---------------------------------------------------------------------------------------
// SongVoice == TrackBuffer::mixInto
// ---------------------------------------------------------------------------------------

namespace {

struct VoiceCase {
    const char* name;
    float volume;
    float pan;
    bool muted;
    bool sendToFx;
};

}  // namespace

// Bug plausible: la voz de la cancion se aparta de la pista del looper en nivel, paneo, mute o
// envio a efectos (volumen aplicado dos veces, canales cruzados, el envio ignorado y la pista
// mezclada en la pasada equivocada). Mismo audio, mismo L, mismos setters: tienen que dar lo
// mismo que mixInto. Se mira la parte del loop sin el crossfade de costura de mixInto.
TEST(SongVoice, AC_055_12_MatchesTrackBufferMixIntoForVolumePanMuteAndSend) {
    constexpr int kLen = 48000;
    constexpr int kBlock = 240;
    constexpr int kPreroll = 4800;   // 20 bloques: los smoothers de las dos pistas parten igual
    const VoiceCase cases[] = {
        {"plain", 0.7f, -0.4f, false, false},
        {"loud-right", 1.6f, 0.9f, false, false},
        {"muted", 0.7f, -0.4f, true, false},
        {"send-to-fx", 0.7f, -0.4f, false, true},
    };
    const auto sample = [](int n, int ch) {
        return ch == 0 ? 0.6f * std::sin(6.2831853f * float(n) / 137.0f)
                       : 0.4f * std::sin(6.2831853f * float(n) / 53.0f + 1.0f);
    };
    for (const VoiceCase& c : cases) {
        SCOPED_TRACE(c.name);
        // A: el camino de siempre (TrackBuffer::mixInto)
        Rig a;
        const std::string path = a.write("vc_audio", kLen, sample);
        ASSERT_TRUE(a.looper.importTrack(0, path.c_str(), kSR));
        a.looper.resumeTrack(0);
        // B: la cancion con el mismo audio
        Rig b;
        int32_t id = -1;
        ASSERT_EQ(b.looper.songLoadClip(path.c_str(), kSR, 0, 0, kLen, false, id),
                  wm::SongStatus::Ok);
        for (Rig* r : {&a, &b}) {
            r->looper.setTrackVolume(0, c.volume);
            r->looper.setTrackPan(0, c.pan);
            r->looper.setTrackMuted(0, c.muted);
            r->looper.setTrackSendToFx(0, c.sendToFx);
        }
        ASSERT_EQ(b.schedule(kLen, {sec(kPreroll, id)}, kPreroll + 3 * kLen), wm::SongStatus::Ok);

        const auto runBlock = [&](Rig& r, int64_t frame, std::vector<float>& fx,
                                  std::vector<float>& main) {
            std::fill(fx.begin(), fx.end(), 0.0f);
            std::fill(main.begin(), main.end(), 0.0f);
            r.looper.beginBlock(frame, kBlock);
            r.looper.mixFxTracks(fx.data(), kBlock);
            r.looper.process(main.data(), kBlock, frame);
        };
        std::vector<float> fxA(kBlock * 2), mainA(kBlock * 2), fxB(kBlock * 2), mainB(kBlock * 2);
        for (int64_t f = 0; f < kPreroll; f += kBlock) {
            runBlock(a, f, fxA, mainA);
            runBlock(b, f, fxB, mainB);
        }
        a.looper.resetTrackPlayHead(0);   // A y B arrancan el loop en el mismo frame

        double worst = 0.0;
        double peak = 0.0;
        for (int64_t k = 0; k < (kLen - 2400 - kBlock) / kBlock; ++k) {
            runBlock(a, kPreroll + k * kBlock, fxA, mainA);
            runBlock(b, kPreroll + k * kBlock, fxB, mainB);
            for (int i = 0; i < kBlock * 2; ++i) {
                worst = std::max({worst, double(std::abs(fxA[i] - fxB[i])),
                                  double(std::abs(mainA[i] - mainB[i]))});
                peak = std::max({peak, double(std::abs(fxA[i])), double(std::abs(mainA[i]))});
            }
        }
        if (c.muted) {
            EXPECT_LT(peak, 1e-3) << "el mute no apago la referencia";
        } else {
            EXPECT_GT(peak, 0.05) << "el caso no suena: no prueba nada";
        }
        EXPECT_LT(worst, 1e-5) << "la voz de la cancion se aparta de mixInto";
    }
}

// ---------------------------------------------------------------------------------------
// Normalizacion de clips (pura)
// ---------------------------------------------------------------------------------------

namespace {

std::vector<float> sineTake(int frames, float period) {
    std::vector<float> take(static_cast<size_t>(frames) * 2);
    for (int n = 0; n < frames; ++n) {
        take[static_cast<size_t>(n) * 2] = std::sin(6.2831853f * float(n) / period);
        take[static_cast<size_t>(n) * 2 + 1] = std::sin(6.2831853f * float(n) / period + 0.5f);
    }
    return take;
}

}  // namespace

// Bug plausible: una toma mas larga que L se estira/comprime en vez de recortarse, o un L
// multiplo exacto introduce un fundido que ensucia la union natural del mosaico.
TEST(SongClip, NormalisationTakeAtLeastLIsAViewAndExactMultipleIsATilingAsIs) {
    const std::vector<float> take = sineTake(3000, 110.0f);
    const std::vector<float> view = wm::normalizeSongClip(take.data(), 3000, 2000, kF);
    ASSERT_EQ(view.size(), 4000u);
    EXPECT_EQ(std::memcmp(view.data(), take.data(), view.size() * sizeof(float)), 0);

    const std::vector<float> small = sineTake(500, 100.0f);
    const std::vector<float> tiled = wm::normalizeSongClip(small.data(), 500, 2000, kF);
    ASSERT_EQ(tiled.size(), 4000u);
    for (int k = 0; k < 4; ++k) {
        EXPECT_EQ(std::memcmp(tiled.data() + k * 1000, small.data(), 1000 * sizeof(float)), 0)
            << "tile " << k;
    }
}

// Bug plausible: el cierre del mosaico con resto queda cortado a mitad de la toma y el
// salto de vuelta al principio es un escalon (click en cada vuelta de la cancion).
TEST(SongClip, NormalisationWithRemainderFadesTheJunctionWithoutAStep) {
    constexpr int kTake = 770;   // 7 periodos de 110: la toma es un loop sin costura
    constexpr int kLoop = 2000;  // resto 460
    const std::vector<float> take = sineTake(kTake, 110.0f);
    const std::vector<float> clip = wm::normalizeSongClip(take.data(), kTake, kLoop, kF);
    ASSERT_EQ(clip.size(), static_cast<size_t>(kLoop) * 2);
    // sin fundido el cierre seria take[r-1] y el salto a take[0] valdria esto:
    const float naive = std::abs(take[(kLoop % kTake - 1) * 2] - take[0]);
    ASSERT_GT(naive, 0.3f) << "el caso no tiene escalon que arreglar";
    // con fundido el salto es el de la toma misma entre su ultimo frame y su primero
    const float natural = std::abs(take[(kTake - 1) * 2] - take[0]);
    EXPECT_LT(std::abs(clip[(kLoop - 1) * 2] - clip[0]), natural + 0.01f);
    EXPECT_LT(std::abs(clip[(kLoop - 1) * 2 + 1] - clip[1]),
              std::abs(take[(kTake - 1) * 2 + 1] - take[1]) + 0.01f);
    // Antes del fundido es el mosaico tal cual.
    const int plainEnd = kLoop - kF;
    for (int n = 0; n < plainEnd; ++n) {
        ASSERT_EQ(clip[static_cast<size_t>(n) * 2], take[static_cast<size_t>(n % kTake) * 2]) << n;
    }
}

// Bug plausible: el recorte horneado no usa clampLoopRegion (una region < 1024 deja un clip
// vacio o pasa del largo de la toma, el bug de #403 en el clip). Toma de 59 frames como la de S10.
TEST(SongClip, BakedTrimUsesClampLoopRegionIncludingShortRegionsAndShortTakes) {
    const std::vector<float> take = sineTake(59, 20.0f);
    // region (10, 30) de una toma de 59: el minimo se topea a lo que queda desde el start
    const wm::LoopRegion r = wm::clampLoopRegion(10, 30, 59);
    ASSERT_EQ(r.start, 10);
    ASSERT_EQ(r.end, 59);
    const std::vector<float> clip = wm::bakeSongClip(take.data(), 59, 10, 30, 98, kF);
    ASSERT_EQ(clip.size(), 98u * 2);
    for (int n = 0; n < 98; ++n) {
        EXPECT_EQ(clip[static_cast<size_t>(n) * 2], take[static_cast<size_t>(10 + n % 49) * 2])
            << n;
    }
    // region_end <= 0: toda la toma; region_start pasado el final: el ultimo frame.
    const std::vector<float> all = wm::bakeSongClip(take.data(), 59, 0, 0, 59, kF);
    EXPECT_EQ(std::memcmp(all.data(), take.data(), 59 * 2 * sizeof(float)), 0);
    const std::vector<float> beyond = wm::bakeSongClip(take.data(), 59, 5000, 9000, 4, kF);
    for (int n = 0; n < 4; ++n) EXPECT_EQ(beyond[static_cast<size_t>(n) * 2], take[58 * 2]);
    // una region corta en una toma larga se estira hasta el minimo de 1024
    const std::vector<float> longTake = sineTake(5000, 300.0f);
    const std::vector<float> stretched = wm::bakeSongClip(longTake.data(), 5000, 1000, 1500, 1024, kF);
    for (int n = 0; n < 1024; ++n) {
        ASSERT_EQ(stretched[static_cast<size_t>(n) * 2], longTake[static_cast<size_t>(1000 + n) * 2]);
    }
}

// Bug plausible: la carga por archivo normaliza distinto que la funcion pura (el camino real
// decodifica y resamplea; este test cierra el circuito archivo -> clip -> sonido).
TEST(SongClip, LoadingFromAFileResamplesBakesAndSounds) {
    Rig rig;
    constexpr int kSrcRate = 24000;
    const std::string path = rig.write("res", 12000, dc(0.5f), kSrcRate);   // 0,5 s a 24 kHz
    int32_t id = -1;
    ASSERT_EQ(rig.looper.songLoadClip(path.c_str(), kSR, 0, 0, 24000, false, id),
              wm::SongStatus::Ok);
    EXPECT_EQ(rig.looper.song().reservedBytes(), 24000u * 2 * sizeof(float));
    ASSERT_EQ(rig.schedule(24000, {sec(5000, id)}, 5000 + 30000), wm::SongStatus::Ok);
    const std::vector<float> out = rig.render(0, 20000, 256);
    EXPECT_NEAR(out[static_cast<size_t>(12000) * 2], 0.5f * centerGain(), 1e-5);
}

// ---------------------------------------------------------------------------------------
// Validaciones de set_schedule
// ---------------------------------------------------------------------------------------

// Bug plausible: una agenda mal formada llega al callback (starts desordenados, secciones mas
// cortas que el fundido, clip de otro largo de loop, clip fantasma, clip en una pista que no
// esta activa). Cada caso es un check distinto; el borde exacto (2F) tiene que pasar.
TEST(SongSchedule, ValidationRejectsMalformedAgendasAndAcceptsTheExactEdge) {
    Rig rig;
    constexpr int64_t kLoop = 2048;
    const int32_t c0 = rig.load("v0", kLoop, dc(0.1f), kLoop);
    const int32_t other = rig.load("v1", 1024, dc(0.1f), 1024);
    const auto st = [&](std::vector<wm::SongSection> s, int64_t end, int64_t loop = kLoop) {
        return rig.schedule(loop, s, end);
    };
    using S = wm::SongStatus;
    const int64_t a = 5000;
    // starts no crecientes / iguales
    EXPECT_EQ(st({sec(a, c0), sec(a, c0)}, a + 5000), S::OutOfRange);
    EXPECT_EQ(st({sec(a + 1000, c0), sec(a, c0)}, a + 5000), S::OutOfRange);
    // seccion < 2F (479) vs exactamente 2F (480)
    EXPECT_EQ(st({sec(a, c0), sec(a + 2 * kF - 1, c0)}, a + 5000), S::OutOfRange);
    EXPECT_EQ(st({sec(a, c0), sec(a + 2 * kF, c0)}, a + 5000), S::Ok);
    // end_frame < ultimo start + 2F vs exacto
    EXPECT_EQ(st({sec(a, c0)}, a + 2 * kF - 1), S::OutOfRange);
    EXPECT_EQ(st({sec(a, c0)}, a + 2 * kF), S::Ok);
    // start negativo, loop <= 0
    EXPECT_EQ(st({sec(-1, c0)}, 5000), S::OutOfRange);
    EXPECT_EQ(st({sec(a, c0)}, a + 5000, 0), S::OutOfRange);
    // clip en la pista 8 (>= activas por defecto)
    {
        wm::SongSection s = sec(a, c0);
        s.clip[8] = c0;
        EXPECT_EQ(st({s}, a + 5000), S::OutOfRange);
    }
    // clip de otro loop_frames, inexistente, fuera de rango
    EXPECT_EQ(st({sec(a, other)}, a + 5000), S::InvalidOperation);
    EXPECT_EQ(st({sec(a, 5)}, a + 5000), S::InvalidOperation);
    EXPECT_EQ(st({sec(a, 99)}, a + 5000), S::InvalidOperation);
    EXPECT_EQ(st({sec(a, -2)}, a + 5000), S::InvalidOperation);
    // mas secciones que el tope
    {
        std::vector<wm::SongSection> many;
        for (int k = 0; k < wm::kSongMaxSections + 1; ++k) many.push_back(sec(a + k * 1000, c0));
        EXPECT_EQ(rig.looper.songSetSchedule(kLoop, many.data(), int32_t(many.size()),
                                             a + 2000000, false),
                  S::OutOfRange);
        many.pop_back();
        EXPECT_EQ(rig.looper.songSetSchedule(kLoop, many.data(), int32_t(many.size()),
                                             a + 2000000, false),
                  S::Ok);
    }
    // una agenda rechazada no pisa la anterior
    ASSERT_EQ(st({sec(a, c0)}, a + 5000), S::Ok);
    const int64_t adoptedBefore = rig.looper.song().adoptedAgendas();
    EXPECT_EQ(st({sec(a, c0), sec(a, c0)}, a + 5000), S::OutOfRange);
    rig.render(0, 256, 256);
    EXPECT_EQ(rig.looper.song().adoptedAgendas(), adoptedBefore + 1)
        << "se publico una agenda ademas de la ultima valida";
}

// ---------------------------------------------------------------------------------------
// Grabar / armar con cancion activa; sample rate
// ---------------------------------------------------------------------------------------

// Bug plausible: se graba o se arma encima de pistas que la cancion ocupa (la grabacion
// escribiria en un TrackBuffer que no suena), o se publica una agenda con una grabacion en
// curso. Los dos sentidos.
TEST(SongSchedule, AC_055_12_RecordingAndTheSongExcludeEachOther) {
    Rig rig;
    constexpr int64_t kLoop = 2048;
    const int32_t c0 = rig.load("rec0", kLoop, dc(0.1f), kLoop);
    ASSERT_TRUE(rig.looper.prepareTrack(1, 4800, kSR));
    ASSERT_TRUE(rig.looper.prepareTrack(2, 4800, kSR));

    // grabacion en curso -> no se publica
    rig.looper.startRecording(1);
    EXPECT_EQ(rig.schedule(kLoop, {sec(5000, c0)}, 9000), wm::SongStatus::InvalidOperation);
    rig.looper.stopRecording();
    rig.looper.abortRecording();
    // armada -> tampoco
    rig.looper.armRecording(2, 99999);
    ASSERT_EQ(rig.looper.getArmedTrack(), 2);
    EXPECT_EQ(rig.schedule(kLoop, {sec(5000, c0)}, 9000), wm::SongStatus::InvalidOperation);
    rig.looper.cancelArm();

    // con la cancion activa, grabar/armar no hacen nada
    ASSERT_EQ(rig.schedule(kLoop, {sec(5000, c0)}, 9000), wm::SongStatus::Ok);
    rig.looper.startRecording(1);
    EXPECT_FALSE(rig.looper.isRecording());
    rig.looper.armRecording(1, 12345);
    EXPECT_EQ(rig.looper.getArmedTrack(), -1);
    EXPECT_EQ(rig.looper.armSyncedToLoop(1, 0, 0), -1);
    rig.looper.startOverdub(1);
    EXPECT_FALSE(rig.looper.isRecording());

    // apagada, vuelve a funcionar
    ASSERT_EQ(rig.looper.songSetSchedule(kLoop, nullptr, 0, 0, false), wm::SongStatus::Ok);
    rig.looper.startRecording(1);
    EXPECT_TRUE(rig.looper.isRecording());
}

// Bug plausible: tras un cambio de sample rate el clip viejo (resampleado al rate anterior)
// sigue en la agenda y suena a otra velocidad / otra altura.
TEST(SongSchedule, SampleRateChangeInvalidatesClipsAndSwitchesTheSongOff) {
    Rig rig;
    constexpr int64_t kLoop = 2048;
    const int32_t c0 = rig.load("sr0", kLoop, dc(0.3f), kLoop);
    ASSERT_EQ(rig.schedule(kLoop, {sec(5000, c0)}, 9000), wm::SongStatus::Ok);
    ASSERT_TRUE(rig.looper.song().active());
    ASSERT_GT(rig.looper.song().reservedBytes(), 0u);

    rig.looper.setSampleRate(kSR);   // el mismo rate no invalida
    EXPECT_TRUE(rig.looper.song().active());

    rig.looper.setSampleRate(44100);
    EXPECT_FALSE(rig.looper.song().active());
    EXPECT_EQ(rig.looper.song().reservedBytes(), 0u);
    EXPECT_EQ(rig.schedule(kLoop, {sec(5000, c0)}, 9000), wm::SongStatus::InvalidOperation)
        << "el id viejo sigue valiendo";
    // un clip nuevo, cargado al rate nuevo, sirve
    const std::string path = rig.write("sr1", 4096, dc(0.3f), 44100);
    int32_t id = -1;
    ASSERT_EQ(rig.looper.songLoadClip(path.c_str(), 44100, 0, 0, kLoop, false, id),
              wm::SongStatus::Ok);
    EXPECT_EQ(rig.schedule(kLoop, {sec(5000, id)}, 9000), wm::SongStatus::Ok);
    // ... y uno cargado al rate viejo no
    int32_t old = -1;
    ASSERT_EQ(rig.looper.songLoadClip(path.c_str(), kSR, 0, 0, kLoop, false, old),
              wm::SongStatus::Ok);
    EXPECT_EQ(rig.schedule(kLoop, {sec(5000, old)}, 9000), wm::SongStatus::InvalidOperation)
        << "un clip de otro rate no puede entrar en la agenda";
}

// Bug plausible: la memoria de los clips queda fuera del presupuesto (un proyecto de 8 pistas
// agota la RAM sin que load_clip diga MEMORY_BUDGET), o un rechazo deja el clip a medias.
TEST(SongClip, ClipsCountAgainstTheLooperMemoryBudget) {
    Rig rig;
    AudioLooper::LooperCapabilities caps;
    caps.memoryBudgetBytes = 4096 * 2 * sizeof(float) * 2 + 100;   // dos clips de 4096 frames
    rig.looper.setCapabilities(caps);
    const std::string path = rig.write("bud", 4096, dc(0.1f));
    int32_t a = -1, b = -1, c = -1;
    ASSERT_EQ(rig.looper.songLoadClip(path.c_str(), kSR, 0, 0, 4096, false, a), wm::SongStatus::Ok);
    ASSERT_EQ(rig.looper.songLoadClip(path.c_str(), kSR, 0, 0, 4096, false, b), wm::SongStatus::Ok);
    EXPECT_EQ(rig.looper.songLoadClip(path.c_str(), kSR, 0, 0, 4096, false, c),
              wm::SongStatus::BudgetExceeded);
    // las tomas ven lo que ocupan los clips
    EXPECT_FALSE(rig.looper.prepareTrack(0, 4096, kSR));
    // y liberar uno devuelve el lugar
    ASSERT_EQ(rig.looper.songUnloadClip(a, true), wm::SongStatus::Ok);
    EXPECT_EQ(rig.looper.songLoadClip(path.c_str(), kSR, 0, 0, 4096, false, c), wm::SongStatus::Ok);
    // otros errores de load, sin tocar el pool (con lugar de sobra: el presupuesto se mira primero)
    ASSERT_EQ(rig.looper.songUnloadClip(b, true), wm::SongStatus::Ok);
    ASSERT_EQ(rig.looper.songUnloadClip(c, true), wm::SongStatus::Ok);
    EXPECT_EQ(rig.looper.songLoadClip("/nonexistent/x.wav", kSR, 0, 0, 4096, false, c),
              wm::SongStatus::Io);
    EXPECT_EQ(rig.looper.songLoadClip(path.c_str(), 0, 0, 0, 4096, false, c),
              wm::SongStatus::OutOfRange);
    EXPECT_EQ(rig.looper.songLoadClip(path.c_str(), kSR, 0, 0, 0, false, c),
              wm::SongStatus::OutOfRange);
    EXPECT_EQ(rig.looper.songLoadClip(nullptr, kSR, 0, 0, 4096, false, c),
              wm::SongStatus::OutOfRange);
}

// ---------------------------------------------------------------------------------------
// Unload: el clip se libera cuando el RT ya no lo puede estar leyendo
// ---------------------------------------------------------------------------------------

// Bug plausible: unload libera el clip mientras la agenda publicada lo referencia (el callback
// lee memoria liberada), o libera el que acaba de sacarse de la agenda antes de que el RT haya
// adoptado la agenda nueva (use-after-free bajo ASan), o nunca lo libera (fuga).
TEST(SongUnload, ReferencedClipIsRefusedAndReleasedOnlyAfterTheCallbackMovedOn) {
    Rig rig;
    constexpr int64_t kLoop = 2048;
    const int32_t c0 = rig.load("ul0", kLoop, dc(0.3f), kLoop);
    const size_t bytes = rig.looper.song().reservedBytes();
    ASSERT_EQ(rig.schedule(kLoop, {sec(100, c0)}, 100 + 50000), wm::SongStatus::Ok);
    EXPECT_EQ(rig.looper.songUnloadClip(c0, false), wm::SongStatus::InvalidOperation)
        << "la agenda publicada lo usa";
    EXPECT_EQ(rig.looper.songUnloadClip(77, false), wm::SongStatus::InvalidOperation);

    rig.render(0, 512, 256);   // el RT adopta la agenda con el clip y ya suena
    ASSERT_EQ(rig.looper.songSetSchedule(kLoop, nullptr, 0, 0, false), wm::SongStatus::Ok);
    ASSERT_EQ(rig.looper.songUnloadClip(c0, false), wm::SongStatus::Ok);
    EXPECT_EQ(rig.looper.song().retiredClips(), 1);
    EXPECT_EQ(rig.looper.song().reclaim(false), bytes)
        << "se libero antes de que el callback adoptara la agenda sin el clip";
    EXPECT_EQ(rig.looper.songUnloadClip(c0, false), wm::SongStatus::InvalidOperation)
        << "un clip retirado no se retira dos veces";
    EXPECT_EQ(rig.schedule(kLoop, {sec(100, c0)}, 100 + 50000), wm::SongStatus::InvalidOperation)
        << "un clip retirado no vuelve a la agenda";

    rig.render(512, 128, 128);   // el RT adopta la agenda apagada: la cola de fundido lee el clip
    EXPECT_EQ(rig.looper.song().reclaim(false), bytes)
        << "todavia suena la cola de fundido que lee el clip";
    rig.render(640, 256, 128);   // la cola dura F frames (hasta 752); despues, nadie lo tiene
    EXPECT_EQ(rig.looper.song().reclaim(false), 0u);
    EXPECT_EQ(rig.looper.song().retiredClips(), 0);
}

// Bug plausible (la carrera real): el control publica/retira/libera mientras el callback
// renderiza. Bajo ASan un clip liberado en uso es un use-after-free; bajo TSan, una carrera
// sobre la tabla de clips o el triple buffer. Cada vuelta carga un clip, lo programa para el
// futuro, lo saca y lo libera, sin esperar a que el RT se entere.
TEST(SongUnload, RacingTheCallbackNeverFreesAClipInUse) {
    Rig rig;
    constexpr int64_t kLoop = 2048;
    const std::string path = rig.write("race", kLoop, dc(0.3f));
    std::atomic<bool> stop{false};
    std::atomic<int64_t> frame{0};
    std::thread rt([&] {
        std::vector<float> buf(128 * 2);
        while (!stop.load(std::memory_order_acquire)) {
            const int64_t f = frame.load(std::memory_order_relaxed);
            std::fill(buf.begin(), buf.end(), 0.0f);
            rig.looper.beginBlock(f, 128);
            rig.looper.process(buf.data(), 128, f);
            frame.store(f + 128, std::memory_order_relaxed);
        }
    });
    constexpr int kCycles = 150;
    int loaded = 0;
    for (int i = 0; i < kCycles; ++i) {
        int32_t id = -1;
        if (rig.looper.songLoadClip(path.c_str(), kSR, 0, 0, kLoop, false, id) !=
            wm::SongStatus::Ok) {
            continue;   // pool lleno de retirados todavia en uso: no es el caso a probar
        }
        ++loaded;
        const int64_t now = frame.load(std::memory_order_relaxed);
        std::vector<wm::SongSection> secs = {sec(now + 100, id), sec(now + 100 + 4096, id)};
        EXPECT_EQ(rig.looper.songSetSchedule(kLoop, secs.data(), 2, now + 100 + 10000, false),
                  wm::SongStatus::Ok);
        if (i % 3 == 0) {   // dejarlo sonar un rato: que la cola de fundido lea el clip
            const int64_t until = frame.load() + 512;
            wma_test::waitUntil([&] { return frame.load() >= until; });
        }
        EXPECT_EQ(rig.looper.songSetSchedule(kLoop, nullptr, 0, 0, false), wm::SongStatus::Ok);
        EXPECT_EQ(rig.looper.songUnloadClip(id, false), wm::SongStatus::Ok);
    }
    EXPECT_GT(loaded, kCycles / 2);
    stop.store(true, std::memory_order_release);
    rt.join();
    // con el callback parado en una foto posterior a todo, no queda nada retenido
    rig.render(frame.load(), 1024, 256);
    EXPECT_EQ(rig.looper.song().reclaim(false), 0u) << "fuga de clips retirados";
}

// ---------------------------------------------------------------------------------------
// Adopcion tardia (editar sonando) y la seccion publicada
// ---------------------------------------------------------------------------------------

// Bug plausible: reprogramar la seccion que SUENA cambia el audio de un frame al otro (click) o
// no se cuenta; y reprogramar una seccion futura la cuenta como tardia (falsa alarma) o la
// corta. AC-055.13.
TEST(SongLateAdoption, AC_055_13_EditingTheSoundingSectionFadesOverFAndIsCounted) {
    Rig rig;
    constexpr int64_t kLoop = 2048;
    const int32_t x = rig.load("late_x", kLoop, dc(0.5f), kLoop);
    const int32_t z = rig.load("late_z", kLoop, dc(0.25f), kLoop);
    const int64_t s0 = 4 * kStride, s1 = s0 + 8 * kStride, end = s1 + 8 * kStride;
    ASSERT_EQ(rig.schedule(kLoop, {sec(s0, x), sec(s1, x)}, end), wm::SongStatus::Ok);

    std::vector<float> out = rig.render(0, s0 + 3 * kStride, 256);   // bloques de 256 hasta t
    const int64_t t = s0 + 3 * kStride;   // multiplo de 256: el primer bloque tras la edicion
    EXPECT_EQ(rig.looper.songGetLateAdoptions(), 0);

    // (a) una edicion de la seccion futura NO es tardia y no se oye
    ASSERT_EQ(rig.schedule(kLoop, {sec(s0, x), sec(s1, z)}, end), wm::SongStatus::Ok);
    std::vector<float> next = rig.render(t, 1024, 256);
    EXPECT_EQ(rig.looper.songGetLateAdoptions(), 0) << "una edicion futura se conto como tardia";
    const float g = centerGain();
    for (int64_t f = 0; f < 1024; ++f) {
        ASSERT_NEAR(next[static_cast<size_t>(f) * 2], 0.5f * g, 1e-6) << f;
    }

    // (b) la seccion que suena cambia de clip: fundido en [t2, t2 + F), equal-power
    const int64_t t2 = t + 1024;
    ASSERT_EQ(rig.schedule(kLoop, {sec(s0, z), sec(s1, z)}, end), wm::SongStatus::Ok);
    const std::vector<float> after = rig.render(t2, 1024, 256);
    EXPECT_EQ(rig.looper.songGetLateAdoptions(), 1);
    for (int64_t j = 0; j < 1024; ++j) {
        const double th = M_PI / 2 * double(j) / kF;
        const double want = (j < kF ? 0.5 * std::cos(th) + 0.25 * std::sin(th) : 0.25) * g;
        ASSERT_NEAR(after[static_cast<size_t>(j) * 2], want, 1e-6) << "j=" << j;
    }
    // (c) sin escalon en la costura entre lo que sono y lo que sigue
    EXPECT_LT(std::abs(after[0] - next[1022 * 2]), 0.01f);
}

// Bug plausible: apagar la cancion corta de golpe (escalon) o deja a la pista muda para
// siempre en vez de devolverla al looper; y la cola de fundido de un apagado se cuenta como
// una edicion tardia.
TEST(SongLateAdoption, AC_055_13_SwitchingTheSongOffFadesOutAndHandsTheTracksBack) {
    Rig rig;
    constexpr int64_t kLoop = 2048;
    const int32_t x = rig.load("off_x", kLoop, dc(0.5f), kLoop);
    ASSERT_EQ(rig.schedule(kLoop, {sec(4096, x)}, 4096 + 50000), wm::SongStatus::Ok);
    rig.render(0, 8192, 256);
    ASSERT_EQ(rig.looper.songSetSchedule(kLoop, nullptr, 0, 0, false), wm::SongStatus::Ok);
    const int64_t t = 8192;
    const std::vector<float> out = rig.render(t, 1024, 256);
    const float g = centerGain();
    for (int64_t j = 0; j < 1024; ++j) {
        const double want = j < kF ? 0.5 * 0.5 * (1.0 + std::cos(M_PI * double(j) / kF)) * g : 0.0;
        ASSERT_NEAR(out[static_cast<size_t>(j) * 2], want, 1e-6) << "j=" << j;
    }
    EXPECT_EQ(rig.looper.songGetLateAdoptions(), 0);
    EXPECT_FALSE(rig.looper.song().active());
}

// Bug plausible: get_section publica la seccion del bloque anterior o no vuelve a -1; el
// numero que se muestra en pantalla no es el que suena.
TEST(SongSection, GetSectionPublishesTheSoundingSectionAndMinusOneOtherwise) {
    Rig rig;
    constexpr int64_t kLoop = 2048;
    const int32_t x = rig.load("sec_x", kLoop, dc(0.5f), kLoop);
    EXPECT_EQ(rig.looper.songGetSection(), -1) << "inactivo";
    const int64_t a = 4096, b = a + 4096, c = b + 4096, end = c + 4096;
    ASSERT_EQ(rig.schedule(kLoop, {sec(a, x), sec(b, x), sec(c, wm::kSongSilence)}, end),
              wm::SongStatus::Ok);
    const auto sectionAfter = [&](int64_t from, int64_t frames) {
        rig.render(from, frames, 256);
        return rig.looper.songGetSection();
    };
    EXPECT_EQ(sectionAfter(0, 256), -1) << "antes del primer start";
    EXPECT_EQ(sectionAfter(a - 256, 256), -1) << "el bloque justo antes de a";
    EXPECT_EQ(sectionAfter(a, 256), 0);
    EXPECT_EQ(sectionAfter(b - 256, 256), 0);
    EXPECT_EQ(sectionAfter(b, 256), 1);
    EXPECT_EQ(sectionAfter(c, 256), 2);
    EXPECT_EQ(sectionAfter(end - 256, 256), 2);
    EXPECT_EQ(sectionAfter(end, 256), -1) << "despues de end_frame";
}

// Bug plausible: el RT no se entera de una agenda publicada con la cancion "dormida" (sin
// agenda adoptada nunca) o la adopta dos veces; y sin beginBlock las pistas del looper siguen
// sonando como siempre (la cancion nunca toca un uso que no la pidio).
TEST(SongSection, WithoutASongOrWithoutBeginBlockTheTakesPlayAsAlways) {
    Rig rig;
    constexpr int kLen = 4800;
    const std::string path = rig.write("plain", kLen, dc(0.5f));
    ASSERT_TRUE(rig.looper.importTrack(0, path.c_str(), kSR));
    rig.looper.resumeTrack(0);
    std::vector<float> buf(256 * 2, 0.0f);
    rig.looper.process(buf.data(), 256, 0);   // sin beginBlock
    EXPECT_NEAR(buf[200], 0.5f * centerGain(), 1e-3);

    int32_t id = -1;
    ASSERT_EQ(rig.looper.songLoadClip(path.c_str(), kSR, 0, 0, kLen, false, id), wm::SongStatus::Ok);
    ASSERT_EQ(rig.schedule(kLen, {sec(100000, id)}, 200000), wm::SongStatus::Ok);
    std::fill(buf.begin(), buf.end(), 0.0f);
    rig.looper.process(buf.data(), 256, 0);   // agenda publicada pero el dueño del reloj no abrio el bloque
    EXPECT_NEAR(buf[200], 0.5f * centerGain(), 1e-3);
    std::fill(buf.begin(), buf.end(), 0.0f);
    rig.looper.beginBlock(256, 256);   // con beginBlock la cancion se hace cargo: la pista calla
    rig.looper.process(buf.data(), 256, 256);
    EXPECT_EQ(buf[200], 0.0f);
}

// Bug plausible: la adopcion del triple buffer pierde la ultima agenda o adopta una vieja tras
// una rafa. Se publican N agendas distintas sin que el RT corra: al correr, el RT queda con la
// ULTIMA, y el checksum de la adoptada es el de la publicada (sin desgarro).
TEST(SongSchedule, ABurstOfPublicationsLeavesTheCallbackWithTheLastOne) {
    Rig rig;
    constexpr int64_t kLoop = 2048;
    const int32_t x = rig.load("burst_x", kLoop, dc(0.5f), kLoop);
    const int32_t z = rig.load("burst_z", kLoop, dc(0.25f), kLoop);
    rig.looper.song().setVerifyAgendas(true);
    for (int i = 0; i < 50; ++i) {
        ASSERT_EQ(rig.schedule(kLoop, {sec(4096, i % 2 ? x : z)}, 50000), wm::SongStatus::Ok);
    }
    ASSERT_EQ(rig.schedule(kLoop, {sec(4096, z)}, 50000), wm::SongStatus::Ok);   // la ultima: z
    const std::vector<float> out = rig.render(0, 4096 + 2048, 256);
    EXPECT_EQ(rig.looper.song().adoptedAgendas(), 1);
    EXPECT_EQ(rig.looper.song().tornAgendas(), 0);
    EXPECT_NEAR(out[static_cast<size_t>(4096 + 1000) * 2], 0.25f * centerGain(), 1e-6);
}

// Bug plausible: el checksum no detecta un campo pisado (el detector de desgarro no detecta).
TEST(SongSchedule, TheAgendaChecksumNoticesAnyChangedField) {
    auto a = std::make_unique<wm::SongAgenda>();
    a->loopFrames = 1000;
    a->endFrame = 9000;
    a->fade = 240;
    a->clipMask = 3;
    a->seq = 7;
    a->count = 2;
    a->starts[0] = 100;
    a->starts[1] = 3000;
    for (int k = 0; k < 2; ++k) {
        for (int t = 0; t < wm::kSongMaxTracks; ++t) a->clips[k][t] = t == 0 ? k : -1;
    }
    const uint64_t base = wm::songAgendaChecksum(*a);
    const auto changed = [&](const std::function<void(wm::SongAgenda&)>& edit) {
        auto b = std::make_unique<wm::SongAgenda>(*a);
        edit(*b);
        return wm::songAgendaChecksum(*b) != base;
    };
    EXPECT_TRUE(changed([](wm::SongAgenda& b) { b.starts[1] += 1; }));
    EXPECT_TRUE(changed([](wm::SongAgenda& b) { b.clips[1][15] = 4; }));
    EXPECT_TRUE(changed([](wm::SongAgenda& b) { b.endFrame += 1; }));
    EXPECT_TRUE(changed([](wm::SongAgenda& b) { b.seq += 1; }));
    EXPECT_TRUE(changed([](wm::SongAgenda& b) { b.fade = 0; }));
    EXPECT_TRUE(changed([](wm::SongAgenda& b) { b.count = 1; }));
}

// ---------------------------------------------------------------------------------------
// AC-055.13: reprogramar DENTRO de un fundido, colas encadenadas, carga fuera del lock
// ---------------------------------------------------------------------------------------

namespace {

constexpr int64_t kEditLoop = 2048;

/// Salto maximo entre muestras consecutivas del canal izquierdo en [from, to).
double maxStep(const std::vector<float>& out, int64_t from, int64_t to) {
    double worst = 0.0;
    for (int64_t f = std::max<int64_t>(from, 1); f < to; ++f) {
        worst = std::max(worst, double(std::abs(out[static_cast<size_t>(f) * 2] -
                                                out[static_cast<size_t>(f - 1) * 2])));
    }
    return worst;
}

/// El salto de un fundido NORMAL del mayor barrido que usan estos tests (+0,5 a -0,5, diferencia
/// 1,0), medido con el mismo motor sin editar nada: la vara contra la que se mide un click.
double normalFadeStep() {
    Rig rig;
    const int32_t x = rig.load("edit_base_x", kEditLoop, dc(0.5f), kEditLoop);
    const int32_t z = rig.load("edit_base_z", kEditLoop, dc(-0.5f), kEditLoop);
    const int64_t s0 = 4096, s1 = s0 + 8192;
    EXPECT_EQ(rig.schedule(kEditLoop, {sec(s0, x), sec(s1, z)}, s1 + 8192), wm::SongStatus::Ok);
    const std::vector<float> out = rig.render(0, s1 + 1000, 64);
    const double step = maxStep(out, s1 - kF - 8, s1 + 8);
    EXPECT_GT(step, 0.001) << "el fundido normal no se midio";
    return step;
}

struct EditRun {
    std::vector<float> out;   // estereo intercalado, indexado por frame absoluto
    int64_t late = 0;
};

/// Renderiza [0, upTo) en bloques de `block`, deja que `edit` publique, y sigue hasta `total`.
EditRun runWithEdit(int block, int64_t upTo, int64_t total,
                    const std::function<void(Rig&)>& setup, const std::function<void(Rig&)>& edit) {
    Rig rig;
    setup(rig);
    EditRun run;
    run.out = rig.render(0, upTo, block);
    edit(rig);
    const std::vector<float> rest = rig.render(upTo, total - upTo, block);
    run.out.insert(run.out.end(), rest.begin(), rest.end());
    run.late = rig.looper.songGetLateAdoptions();
    return run;
}

}  // namespace

// Bug plausible (BLOQUEANTE 1, S1): la adopcion compara el estado ESTABLE y no ve el fundido de
// borde en curso. Cambiar el clip de la seccion SIGUIENTE cuando faltan 128 frames para B (dentro
// de [B-F, B)) cambia lo que suena (cur*gOut + nxt*gIn) de un frame al otro: salto de ~0,36 y
// late_adoptions en 0. Tiene que ser un fundido cercano al de un borde normal y contarse.
TEST(SongLateAdoption, AC_055_13_ChangingTheNextClipInsideTheEntryFadeDoesNotClickAndIsCounted) {
    const int64_t s0 = 4096, s1 = s0 + 8192, end = s1 + 8192, t = s1 - 128;
    const double base = normalFadeStep();
    EditRun reference;
    for (int block : {32, 64, 128}) {
        SCOPED_TRACE(block);
        int32_t x = -1, w = -1;
        const EditRun run = runWithEdit(
            block, t, end + 500,
            [&](Rig& rig) {
                x = rig.load("s1_x", kEditLoop, dc(0.5f), kEditLoop);
                const int32_t z = rig.load("s1_z", kEditLoop, dc(0.25f), kEditLoop);
                w = rig.load("s1_w", kEditLoop, dc(-0.5f), kEditLoop);
                ASSERT_EQ(rig.schedule(kEditLoop, {sec(s0, x), sec(s1, z)}, end), wm::SongStatus::Ok);
            },
            [&](Rig& rig) {
                ASSERT_EQ(rig.schedule(kEditLoop, {sec(s0, x), sec(s1, w)}, end), wm::SongStatus::Ok);
            });
        EXPECT_LE(maxStep(run.out, t - 300, t + 400), 2.0 * base)
            << "click al reprogramar dentro del fundido de entrada (fundido normal: " << base << ")";
        EXPECT_EQ(run.late, 1) << "la edicion cambia lo que suena en t: es tardia";
        EXPECT_NEAR(run.out[static_cast<size_t>(s1 + 1000) * 2], -0.5f * centerGain(), 1e-6)
            << "el estado final es el clip nuevo";
        if (reference.out.empty()) {
            reference = run;
        } else {
            EXPECT_EQ(std::memcmp(run.out.data(), reference.out.data(),
                                  run.out.size() * sizeof(float)), 0)
                << "la salida depende del tamaño de bloque";
        }
    }
}

// Bug plausible (BLOQUEANTE 1, S2): la cola de la adopcion tardia guarda UN clip a ganancia 1 y
// pierde el fundido de borde que sonaba: cambiar el clip de la seccion que suena dentro de
// [B-F, B) deja saltar la salida (~0,33) aunque late_adoptions cuente 1.
TEST(SongLateAdoption, AC_055_13_ChangingTheSoundingClipInsideTheExitFadeKeepsTheFadeInTheTail) {
    const int64_t s0 = 4096, s1 = s0 + 8192, end = s1 + 8192, t = s1 - 128;
    const double base = normalFadeStep();
    EditRun reference;
    for (int block : {32, 64, 128}) {
        SCOPED_TRACE(block);
        int32_t x = -1, y = -1, z = -1;
        const EditRun run = runWithEdit(
            block, t, end + 500,
            [&](Rig& rig) {
                x = rig.load("s2_x", kEditLoop, dc(0.5f), kEditLoop);
                z = rig.load("s2_z", kEditLoop, dc(-0.5f), kEditLoop);
                y = rig.load("s2_y", kEditLoop, dc(0.1f), kEditLoop);
                ASSERT_EQ(rig.schedule(kEditLoop, {sec(s0, x), sec(s1, z)}, end), wm::SongStatus::Ok);
            },
            [&](Rig& rig) {
                ASSERT_EQ(rig.schedule(kEditLoop, {sec(s0, y), sec(s1, z)}, end), wm::SongStatus::Ok);
            });
        EXPECT_LE(maxStep(run.out, t - 300, t + 400), 2.0 * base)
            << "click al reprogramar la seccion que suena dentro de su fundido de salida";
        EXPECT_EQ(run.late, 1);
        EXPECT_NEAR(run.out[static_cast<size_t>(s1 + 1000) * 2], -0.5f * centerGain(), 1e-6);
        if (reference.out.empty()) {
            reference = run;
        } else {
            EXPECT_EQ(std::memcmp(run.out.data(), reference.out.data(),
                                  run.out.size() * sizeof(float)), 0);
        }
    }
}

// Bug plausible (BLOQUEANTE 1, S3): dos adopciones tardias separadas por menos de F; la cola de
// la segunda arranca desde el estado estable del clip viejo y no hereda la cola en curso: la
// salida salta ~0,5 cuando se corta la primera cola.
TEST(SongLateAdoption, AC_055_13_ASecondEditInsideTheFirstTailInheritsTheTailInProgress) {
    const int64_t s0 = 4096, end = s0 + 16384, t = s0 + 4096;
    const double base = normalFadeStep();
    EditRun reference;
    for (int block : {32, 64}) {
        SCOPED_TRACE(block);
        int32_t x = -1, y = -1, z = -1;
        Rig rig;
        x = rig.load("s3_x", kEditLoop, dc(0.5f), kEditLoop);
        y = rig.load("s3_y", kEditLoop, dc(-0.5f), kEditLoop);
        z = rig.load("s3_z", kEditLoop, dc(0.1f), kEditLoop);
        ASSERT_EQ(rig.schedule(kEditLoop, {sec(s0, x)}, end), wm::SongStatus::Ok);
        EditRun run;
        run.out = rig.render(0, t, block);
        ASSERT_EQ(rig.schedule(kEditLoop, {sec(s0, y)}, end), wm::SongStatus::Ok);
        std::vector<float> part = rig.render(t, 64, block);
        run.out.insert(run.out.end(), part.begin(), part.end());
        ASSERT_EQ(rig.schedule(kEditLoop, {sec(s0, z)}, end), wm::SongStatus::Ok);
        part = rig.render(t + 64, end + 500 - (t + 64), block);
        run.out.insert(run.out.end(), part.begin(), part.end());
        run.late = rig.looper.songGetLateAdoptions();
        EXPECT_LE(maxStep(run.out, t - 100, t + 600), 2.0 * base)
            << "click al encadenar dos ediciones dentro de F";
        EXPECT_EQ(run.late, 2);
        EXPECT_NEAR(run.out[static_cast<size_t>(t + 1000) * 2], 0.1f * centerGain(), 1e-6)
            << "pasadas ambas colas queda el ultimo clip, sin restos de los anteriores";
        if (reference.out.empty()) {
            reference = run;
        } else {
            EXPECT_EQ(std::memcmp(run.out.data(), reference.out.data(),
                                  run.out.size() * sizeof(float)), 0);
        }
    }
}

// Bug plausible (BLOQUEANTE 1, S4): la edicion mueve el borde siguiente de modo que el frame de
// adopcion cae dentro del NUEVO [B'-F, B'): el clip y la fase de t son los mismos, asi que el
// estado estable no cambia, pero el fundido de borde ya esta andando y aparece de golpe.
TEST(SongLateAdoption, AC_055_13_MovingTheNextBorderSoTheAdoptionLandsInItsFadeDoesNotClick) {
    const int64_t s0 = 4096, s1 = s0 + 8192, end = s1 + 16384, t = s0 + 4096;
    const double base = normalFadeStep();
    EditRun reference;
    for (int block : {32, 64, 128}) {
        SCOPED_TRACE(block);
        int32_t x = -1, z = -1;
        const EditRun run = runWithEdit(
            block, t, end + 500,
            [&](Rig& rig) {
                x = rig.load("s4_x", kEditLoop, dc(0.5f), kEditLoop);
                z = rig.load("s4_z", kEditLoop, dc(-0.5f), kEditLoop);
                ASSERT_EQ(rig.schedule(kEditLoop, {sec(s0, x), sec(s1, z)}, end), wm::SongStatus::Ok);
            },
            [&](Rig& rig) {
                ASSERT_EQ(rig.schedule(kEditLoop, {sec(s0, x), sec(t + 128, z)}, end),
                          wm::SongStatus::Ok);
            });
        EXPECT_LE(maxStep(run.out, t - 300, t + 700), 2.0 * base)
            << "click al mover el borde siguiente hasta caer en su propio fundido";
        EXPECT_EQ(run.late, 1);
        EXPECT_NEAR(run.out[static_cast<size_t>(t + 1500) * 2], -0.5f * centerGain(), 1e-6);
        if (reference.out.empty()) {
            reference = run;
        } else {
            EXPECT_EQ(std::memcmp(run.out.data(), reference.out.data(),
                                  run.out.size() * sizeof(float)), 0);
        }
    }
}

// Bug plausible (BLOQUEANTE 1, `play` tardio): el start_0 de una agenda nueva cae a menos de F
// del frame de adopcion; el fundido de entrada ya paso en parte y la salida arranca a mitad de
// rampa (escalon desde el silencio), ni rechazado ni contado.
TEST(SongLateAdoption, AC_055_13_PlayWhoseFirstStartIsLessThanFAwayFadesInFromTheAdoption) {
    const int64_t t = 4096, end = t + 20000;
    EditRun reference;
    for (int block : {32, 64, 128}) {
        SCOPED_TRACE(block);
        int32_t x = -1;
        const EditRun run = runWithEdit(
            block, t, end + 500,
            [&](Rig& rig) { x = rig.load("s5_x", kEditLoop, dc(0.5f), kEditLoop); },
            [&](Rig& rig) {
                ASSERT_EQ(rig.schedule(kEditLoop, {sec(t + 64, x)}, end), wm::SongStatus::Ok);
            });
        // La entrada normal (raised cosine de 0,5) tiene pendiente maxima 0,5*pi/(2F)*g; tolera 2x.
        const double normalEntry = 0.5 * M_PI / (2.0 * kF) * centerGain();
        EXPECT_LE(maxStep(run.out, t - 100, t + 600), 2.0 * normalEntry)
            << "escalon al entrar con el fundido de entrada ya empezado";
        EXPECT_EQ(run.late, 1) << "el fundido de entrada truncado se cuenta como adopcion tardia";
        EXPECT_NEAR(run.out[static_cast<size_t>(t + 1000) * 2], 0.5f * centerGain(), 1e-6);
        if (reference.out.empty()) {
            reference = run;
        } else {
            EXPECT_EQ(std::memcmp(run.out.data(), reference.out.data(),
                                  run.out.size() * sizeof(float)), 0);
        }
    }
}

// Bug plausible (la otra cara): un `play` con start_0 LEJOS no es tardio ni agrega fundidos
// (se cuenta 0 y la salida es el fundido de entrada normal). Guarda contra un arreglo que cuente
// o suavice cualquier diferencia futura.
TEST(SongLateAdoption, AC_055_13_AFutureOnlyChangeIsNeitherCountedNorSmoothed) {
    const int64_t t = 4096, s0 = t + 4 * kF, end = s0 + 20000;
    Rig rig;
    const int32_t x = rig.load("s6_x", kEditLoop, dc(0.5f), kEditLoop);
    std::vector<float> out = rig.render(0, t, 64);
    ASSERT_EQ(rig.schedule(kEditLoop, {sec(s0, x)}, end), wm::SongStatus::Ok);
    const std::vector<float> rest = rig.render(t, end + 500 - t, 64);
    out.insert(out.end(), rest.begin(), rest.end());
    EXPECT_EQ(rig.looper.songGetLateAdoptions(), 0);
    for (int64_t f = s0 - kF; f < s0; ++f) {
        const double want = 0.5 * 0.5 * (1.0 - std::cos(M_PI * double(f - (s0 - kF)) / kF));
        ASSERT_NEAR(out[static_cast<size_t>(f) * 2], want * centerGain(), 1e-6) << f;
    }
}

// Bug plausible (MENOR 5): `start[1] - start[0]` con INT64_MIN desborda (UB) y la agenda pasa
// la validacion con un start negativo disfrazado de salto enorme; igual con end_frame.
TEST(SongSchedule, AC_055_13_ValidationDoesNotOverflowOnExtremeFrames) {
    Rig rig;
    const int32_t c0 = rig.load("ov0", kEditLoop, dc(0.1f), kEditLoop);
    using S = wm::SongStatus;
    EXPECT_EQ(rig.schedule(kEditLoop, {sec(1, c0), sec(INT64_MIN, c0)}, 100000), S::OutOfRange);
    EXPECT_EQ(rig.schedule(kEditLoop, {sec(0, c0), sec(INT64_MIN, c0)}, 100000), S::OutOfRange);
    EXPECT_EQ(rig.schedule(kEditLoop, {sec(1, c0)}, INT64_MIN), S::OutOfRange);
    EXPECT_EQ(rig.schedule(kEditLoop, {sec(INT64_MIN, c0)}, 100000), S::OutOfRange);
    EXPECT_EQ(rig.schedule(kEditLoop, {sec(1, c0)}, INT64_MAX), S::OutOfRange)
        << "INT64_MAX es el centinela de 'sin borde siguiente'";
    // lo valido sigue valiendo, incluso muy lejos
    EXPECT_EQ(rig.schedule(kEditLoop, {sec(INT64_MAX / 2, c0)}, INT64_MAX / 2 + 100000), S::Ok);
}

// Bug plausible (MENOR 3): la grabacion armada se dispara con la cancion activa (la carrera
// entre set_schedule y arm no tiene lock comun): startRecording no hace nada pero se limpia el
// armado y se cuenta como disparado, y el llamador cree que grabo.
TEST(SongSchedule, AC_055_13_AnArmedRecordingThatFiresUnderTheSongIsNotCountedAsTriggered) {
    Rig rig;
    const int32_t c0 = rig.load("arm0", kEditLoop, dc(0.1f), kEditLoop);
    ASSERT_TRUE(rig.looper.prepareTrack(1, 4800, kSR));
    rig.looper.armRecording(1, 0);
    ASSERT_EQ(rig.looper.getArmedTrack(), 1);
    // la carrera: set_schedule leyo recordingBusy=false ANTES de que se armara
    wm::SongContext ctx;
    ctx.sampleRate = kSR;
    ctx.maxActiveTracks = 8;
    ctx.budgetBytes = rig.looper.getMemoryBudgetBytes();
    const wm::SongSection s = sec(5000, c0);
    ASSERT_EQ(rig.looper.song().setSchedule(kEditLoop, &s, 1, 9000, ctx), wm::SongStatus::Ok);
    ASSERT_TRUE(rig.looper.song().active());
    rig.render(0, 256, 256);
    EXPECT_FALSE(rig.looper.isRecording());
    EXPECT_EQ(rig.looper.getArmedTriggered(), 0)
        << "el disparo se conto aunque la cancion impidio grabar";
    EXPECT_EQ(rig.looper.getArmedTrack(), -1) << "el armado fantasma sigue vivo";
}

// ---------------------------------------------------------------------------------------
// Clips retirados con el callback parado; carga fuera del lock
// ---------------------------------------------------------------------------------------

// Bug plausible (IMPORTANTE 2): con el stream parado la foto del RT queda en la ultima agenda y
// cada unload+load retiene slot y bytes: a los 64 ciclos el pool da PARAMETER_OUT_OF_RANGE.
TEST(SongUnload, AC_055_13_WithTheCallbackStoppedRetiredClipsAreReleasedAtOnce) {
    Rig rig;
    const int32_t c0 = rig.load("stop0", kEditLoop, dc(0.3f), kEditLoop);
    const std::string path = rig.write("stop_cycle", kEditLoop, dc(0.3f));
    ASSERT_EQ(rig.schedule(kEditLoop, {sec(100, c0)}, 100 + 50000), wm::SongStatus::Ok);
    rig.render(0, 512, 256);   // el RT adopta y publica su foto; despues el stream se para
    ASSERT_EQ(rig.looper.songSetSchedule(kEditLoop, nullptr, 0, 0, false, true), wm::SongStatus::Ok);
    ASSERT_EQ(rig.looper.songUnloadClip(c0, false, true), wm::SongStatus::Ok);
    EXPECT_EQ(rig.looper.song().retiredClips(), 0) << "el clip quedo retenido con el callback parado";
    EXPECT_EQ(rig.looper.song().reservedBytes(), 0u);
    for (int i = 0; i < 3 * wm::kSongMaxClips; ++i) {
        int32_t id = -1;
        ASSERT_EQ(rig.looper.songLoadClip(path.c_str(), kSR, 0, 0, kEditLoop, false, id, true),
                  wm::SongStatus::Ok)
            << "ciclo " << i << ": el pool se agoto con clips retirados";
        ASSERT_EQ(rig.looper.songUnloadClip(id, false, true), wm::SongStatus::Ok);
    }
    EXPECT_EQ(rig.looper.song().reservedBytes(), 0u);
}

// Bug plausible (IMPORTANTE 2, la carrera): "parado" es una pista, no una prueba. Con un bloque
// ABIERTO (el callback esta leyendo clips) liberar sin esperar es un use-after-free; el control
// tiene que verificarlo y retener.
TEST(SongUnload, AC_055_13_AStoppedHintNeverFreesWhileABlockIsOpen) {
    Rig rig;
    const int32_t c0 = rig.load("blk0", kEditLoop, dc(0.3f), kEditLoop);
    ASSERT_EQ(rig.schedule(kEditLoop, {sec(100, c0)}, 100 + 50000), wm::SongStatus::Ok);
    rig.render(0, 512, 256);
    ASSERT_EQ(rig.looper.songSetSchedule(kEditLoop, nullptr, 0, 0, false), wm::SongStatus::Ok);
    rig.looper.beginBlock(512, 256);   // el callback esta adentro de un bloque
    ASSERT_EQ(rig.looper.songUnloadClip(c0, false, true), wm::SongStatus::Ok);
    EXPECT_EQ(rig.looper.song().retiredClips(), 1) << "se libero con un bloque abierto";
    std::vector<float> buf(256 * 2, 0.0f);
    rig.looper.process(buf.data(), 256, 512);   // el bloque termina
    EXPECT_EQ(rig.looper.song().reclaim(false, true), 0u);
    EXPECT_EQ(rig.looper.song().retiredClips(), 0);
}

// Bug plausible (IMPORTANTE 2, ghost): liberar con el callback parado y reutilizar el id deja a
// las colas de fundido del RT apuntando a un clip que ya es OTRO: al reanudar suena un clip
// ajeno fundiendose a la salida.
TEST(SongUnload, AC_055_13_ReusingAReleasedIdDoesNotResurrectItInTheFadeTail) {
    Rig rig;
    const int32_t old = rig.load("ghost_old", kEditLoop, dc(0.5f), kEditLoop);
    ASSERT_EQ(rig.schedule(kEditLoop, {sec(100, old)}, 100 + 50000), wm::SongStatus::Ok);
    rig.render(0, 1024, 256);   // suena el clip viejo
    // el stream se para: se apaga, se descarga y se carga OTRO clip (reusa el id)
    ASSERT_EQ(rig.looper.songSetSchedule(kEditLoop, nullptr, 0, 0, false, true), wm::SongStatus::Ok);
    ASSERT_EQ(rig.looper.songUnloadClip(old, false, true), wm::SongStatus::Ok);
    const int32_t fresh = rig.load("ghost_new", kEditLoop, dc(0.9f), kEditLoop);
    ASSERT_EQ(fresh, old) << "el test espera que el id se reuse";
    ASSERT_EQ(rig.schedule(kEditLoop, {sec(20000, fresh)}, 20000 + 50000), wm::SongStatus::Ok);
    // el stream arranca: a esta altura no suena nada
    const std::vector<float> out = rig.render(2048, 4 * kF, 64);
    for (size_t i = 0; i < out.size(); ++i) {
        ASSERT_EQ(out[i], 0.0f) << "suena un fantasma en la muestra " << i;
    }
}

// Bug plausible (IMPORTANTE 2, carrera real): con una pista de "parado" equivocada y el callback
// corriendo, el control libera mientras el RT lee. Bajo ASan es un use-after-free; bajo TSan,
// una carrera sobre la tabla de clips.
TEST(SongUnload, AC_055_13_RacingTheCallbackWithAWrongStoppedHintNeverFreesAClipInUse) {
    Rig rig;
    const std::string path = rig.write("race_hint", kEditLoop, dc(0.3f));
    std::atomic<bool> stop{false};
    std::atomic<int64_t> frame{0};
    std::thread rt([&] {
        std::vector<float> buf(64 * 2);
        while (!stop.load(std::memory_order_acquire)) {
            const int64_t f = frame.load(std::memory_order_relaxed);
            std::fill(buf.begin(), buf.end(), 0.0f);
            rig.looper.beginBlock(f, 64);
            rig.looper.process(buf.data(), 64, f);
            frame.store(f + 64, std::memory_order_relaxed);
        }
    });
    int loaded = 0;
    for (int i = 0; i < 200; ++i) {
        int32_t id = -1;
        if (rig.looper.songLoadClip(path.c_str(), kSR, 0, 0, kEditLoop, false, id, true) !=
            wm::SongStatus::Ok) {
            continue;
        }
        ++loaded;
        const int64_t now = frame.load(std::memory_order_relaxed);
        std::vector<wm::SongSection> secs = {sec(std::max<int64_t>(0, now - 500), id),
                                              sec(now + 1000, id)};
        EXPECT_EQ(rig.looper.songSetSchedule(kEditLoop, secs.data(), 2, now + 100000, false, true),
                  wm::SongStatus::Ok);
        if (i % 3 == 0) {
            const int64_t until = frame.load() + 256;
            wma_test::waitUntil([&] { return frame.load() >= until; });
        }
        EXPECT_EQ(rig.looper.songSetSchedule(kEditLoop, nullptr, 0, 0, false, true),
                  wm::SongStatus::Ok);
        EXPECT_EQ(rig.looper.songUnloadClip(id, false, true), wm::SongStatus::Ok);
    }
    EXPECT_GT(loaded, 0);   // el pool puede llenarse de retirados en uso: depende del reparto de hilos
    stop.store(true, std::memory_order_release);
    rt.join();
    EXPECT_EQ(rig.looper.song().reclaim(false, true), 0u) << "fuga de clips retirados";
}

namespace {

/// Una carga en curso detenida DENTRO de la decodificacion, para probar que el control sigue vivo.
class SlowLoad {
public:
    SlowLoad(Rig& rig, const std::string& path, int sampleRate) : mRig(rig) {
        mRig.looper.song().setLoadHook([this] {
            mInDecode.store(true, std::memory_order_release);
            wma_test::waitUntil([this] { return mRelease.load(std::memory_order_acquire); },
                                std::chrono::milliseconds(20000));
        });
        mThread = std::thread([this, path, sampleRate] {
            mStatus = mRig.looper.songLoadClip(path.c_str(), sampleRate, 0, 0, kEditLoop, false, mId);
        });
        mStarted = wma_test::waitUntil([this] { return mInDecode.load(std::memory_order_acquire); },
                                       std::chrono::milliseconds(5000));
    }
    ~SlowLoad() { finish(); }
    bool started() const { return mStarted; }
    /// Corre `fn` en otro hilo y dice si termino mientras la carga seguia detenida.
    bool completesWhileLoading(const std::function<void()>& fn) {
        std::atomic<bool> done{false};
        std::thread t([&] {
            fn();
            done.store(true, std::memory_order_release);
        });
        const bool ok = wma_test::waitUntil([&] { return done.load(std::memory_order_acquire); },
                                            std::chrono::milliseconds(3000));
        if (!ok) release();   // que el hilo pueda terminar y el join no cuelgue
        t.join();
        return ok;
    }
    void release() { mRelease.store(true, std::memory_order_release); }
    wm::SongStatus finish() {
        release();
        if (mThread.joinable()) mThread.join();
        return mStatus;
    }
    int32_t id() const { return mId; }

private:
    Rig& mRig;
    std::atomic<bool> mInDecode{false};
    std::atomic<bool> mRelease{false};
    std::thread mThread;
    bool mStarted = false;
    wm::SongStatus mStatus = wm::SongStatus::Io;
    int32_t mId = -1;
};

}  // namespace

// Bug plausible (IMPORTANTE 4): loadClip decodifica con el mutex de control tomado: una edicion
// sonando (set_schedule) espera la decodificacion de otro clip, segundos con un archivo grande.
TEST(SongClip, AC_055_13_ScheduleAndUnloadDoNotWaitForASlowLoad) {
    Rig rig;
    const int32_t c0 = rig.load("slow0", kEditLoop, dc(0.3f), kEditLoop);
    const std::string path = rig.write("slow_big", kEditLoop, dc(0.2f));
    ASSERT_EQ(rig.schedule(kEditLoop, {sec(100, c0)}, 100 + 50000), wm::SongStatus::Ok);
    SlowLoad load(rig, path, kSR);
    ASSERT_TRUE(load.started());
    EXPECT_TRUE(load.completesWhileLoading([&] {
        EXPECT_EQ(rig.schedule(kEditLoop, {sec(100, c0), sec(10000, c0)}, 100 + 50000),
                  wm::SongStatus::Ok);
    })) << "set_schedule espero a la decodificacion de otro clip";
    EXPECT_TRUE(load.completesWhileLoading([&] {
        EXPECT_EQ(rig.looper.songUnloadClip(77, false), wm::SongStatus::InvalidOperation);
    })) << "unload espero a la decodificacion de otro clip";
    EXPECT_EQ(load.finish(), wm::SongStatus::Ok);
    EXPECT_EQ(rig.looper.song().reservedBytes(), 2u * kEditLoop * 2 * sizeof(float))
        << "la reserva se contabiliza una sola vez";
}

// Bug plausible (IMPORTANTE 4): el cambio de sample rate (con el RT en quiesce) se bloquea en el
// mutex toda la decodificacion; y el clip que se estaba decodificando se instala despues al rate
// viejo (suena a otra altura). Tiene que descartarse con un error que diga la causa.
TEST(SongClip, AC_055_13_ASampleRateChangeDuringALoadDoesNotWaitAndDiscardsTheClip) {
    Rig rig;
    const std::string path = rig.write("slow_sr", kEditLoop, dc(0.2f));
    SlowLoad load(rig, path, kSR);
    ASSERT_TRUE(load.started());
    EXPECT_TRUE(load.completesWhileLoading([&] { rig.looper.setSampleRate(44100); }))
        << "el cambio de sample rate espero a la decodificacion (audio en quiesce)";
    EXPECT_EQ(load.finish(), wm::SongStatus::InvalidOperation)
        << "el clip decodificado al rate viejo no puede instalarse";
    EXPECT_EQ(rig.looper.song().reservedBytes(), 0u);
    // el slot quedo libre y el siguiente clip (al rate nuevo) carga
    const std::string path2 = rig.write("slow_sr2", 4096, dc(0.2f), 44100);
    int32_t id = -1;
    EXPECT_EQ(rig.looper.songLoadClip(path2.c_str(), 44100, 0, 0, kEditLoop, false, id),
              wm::SongStatus::Ok);
}

// ---------------------------------------------------------------------------------------
// AC-055.13: la cola de fundido tras liberar con el stream parado (re-review 2)
// ---------------------------------------------------------------------------------------

namespace {

/// Edita X->Y con el stream parado dentro de la cola de X, descarga X parado y reanuda sin
/// republicar; 8192 frames despues edita Y->Z. Devuelve el escalon de esa ultima edicion.
struct ScrubRun {
    double step = 0.0;
    int64_t late = 0;
};

ScrubRun editAfterScrubThatAdoptedNothing() {
    Rig rig;
    const int32_t x = rig.load("scrub_x", kEditLoop, dc(0.5f), kEditLoop);
    const int32_t y = rig.load("scrub_y", kEditLoop, dc(-0.5f), kEditLoop);
    const int32_t z = rig.load("scrub_z", kEditLoop, dc(0.25f), kEditLoop);
    const int64_t end = int64_t{1} << 22;
    EXPECT_EQ(rig.schedule(kEditLoop, {sec(0, x)}, end), wm::SongStatus::Ok);
    std::vector<float> out = rig.render(0, 8192, 64);
    EXPECT_EQ(rig.schedule(kEditLoop, {sec(0, y)}, end), wm::SongStatus::Ok);
    std::vector<float> seg = rig.render(8192, 64, 64);   // la cola de X sigue viva al parar
    out.insert(out.end(), seg.begin(), seg.end());
    EXPECT_EQ(rig.looper.songUnloadClip(x, false, true), wm::SongStatus::Ok);
    EXPECT_EQ(rig.looper.song().retiredClips(), 0) << "con el stream parado se libera";
    seg = rig.render(8256, 8192, 64);   // arranca sin republicar: el scrub no adopta nada
    out.insert(out.end(), seg.begin(), seg.end());
    const int64_t t = 8256 + 8192;
    EXPECT_EQ(rig.schedule(kEditLoop, {sec(0, z)}, end), wm::SongStatus::Ok);
    seg = rig.render(t, 4096, 64);
    out.insert(out.end(), seg.begin(), seg.end());
    return {maxStep(out, t - 300, t + 600), rig.looper.songGetLateAdoptions()};
}

}  // namespace

// Bug plausible (re-review 2, importante): el scrub deja `mSkipTailOnce` prendido cuando el bloque
// que lo consume no adopta nada; la proxima edicion LEGITIMA (Y->Z, muchos frames despues) pierde
// su cola de fundido: corte seco (~0,53) y una adopcion tardia sin contar.
TEST(SongUnload, AC_055_13_AScrubThatAdoptedNothingDoesNotEatTheNextEditsFade) {
    const ScrubRun r = editAfterScrubThatAdoptedNothing();
    EXPECT_LE(r.step, 2.0 * normalFadeStep()) << "corte seco: la edicion perdio su fundido";
    EXPECT_EQ(r.late, 3);
}

// Bug plausible (re-review 2, otra entrada): un cambio de sample rate sin cancion publicada
// prende el scrub; el `play` siguiente con start_0 a menos de F adopta con la bandera vieja y
// entra con escalon en vez de fundido de entrada.
TEST(SongUnload, AC_055_13_ARateChangeWithoutASongDoesNotEatThePlayFadeIn) {
    Rig rig;
    rig.looper.setSampleRate(44100);
    const int64_t fade = 220;   // songFadeFrames(44100)
    const std::string path = rig.write("scrub_rate", kEditLoop, dc(0.5f), 44100);
    int32_t x = -1;
    ASSERT_EQ(rig.looper.songLoadClip(path.c_str(), 44100, 0, 0, kEditLoop, false, x),
              wm::SongStatus::Ok);
    std::vector<float> out = rig.render(0, 8192, 64);
    const int64_t t = 8192;
    ASSERT_EQ(rig.schedule(kEditLoop, {sec(t + 64, x)}, t + 64 + 100000), wm::SongStatus::Ok);
    const std::vector<float> rest = rig.render(t, 4096, 64);
    out.insert(out.end(), rest.begin(), rest.end());
    const double normalEntry = 0.5 * M_PI / (2.0 * fade) * centerGain();
    EXPECT_LE(maxStep(out, t - 10, t + 600), 2.0 * normalEntry) << "escalon al entrar";
    EXPECT_EQ(rig.looper.songGetLateAdoptions(), 1);
}

// Bug plausible (re-review 2, hueco de test): la foto de lo en uso publica sólo la capa de arriba
// de la cola. Con dos adopciones tardias separadas 32 frames, la capa 0 todavia lee X: liberarlo
// es un use-after-free (y un fantasma si el id se reusa).
TEST(SongUnload, AC_055_13_TheInUseMaskCoversEveryTailLayer) {
    Rig rig;
    const int32_t x = rig.load("mask_x", kEditLoop, dc(0.5f), kEditLoop);
    const int32_t y = rig.load("mask_y", kEditLoop, dc(-0.5f), kEditLoop);
    const int32_t z = rig.load("mask_z", kEditLoop, dc(0.25f), kEditLoop);
    const int64_t end = int64_t{1} << 22;
    ASSERT_EQ(rig.schedule(kEditLoop, {sec(100, x)}, end), wm::SongStatus::Ok);
    rig.render(0, 4096, 32);
    ASSERT_EQ(rig.schedule(kEditLoop, {sec(100, y)}, end), wm::SongStatus::Ok);
    rig.render(4096, 32, 32);   // capa 0: cola sobre X
    ASSERT_EQ(rig.schedule(kEditLoop, {sec(100, z)}, end), wm::SongStatus::Ok);
    rig.render(4128, 32, 32);   // capa 1: cola sobre Y; la de X sigue viva
    ASSERT_EQ(rig.looper.songUnloadClip(x, false), wm::SongStatus::Ok);
    EXPECT_EQ(rig.looper.song().retiredClips(), 1) << "X se libero con una capa de cola leyendolo";
    rig.render(4160, 32, 32);
    rig.looper.song().reclaim(false);
    EXPECT_EQ(rig.looper.song().retiredClips(), 1) << "sigue en la ventana de la capa 0";
    rig.render(4192, 2 * kF, 32);   // terminan las ventanas
    rig.looper.song().reclaim(false);
    EXPECT_EQ(rig.looper.song().retiredClips(), 0) << "pasada la ventana tiene que liberarse";
}

// Bug plausible (re-review 2, menor): en offline la baja inmediata no mira las colas de fundido:
// una cola sobre X, descargar X y cargar G (reusa el id) hace sonar G en la cola (salto ~0,26).
TEST(SongUnload, AC_055_13_OfflineUnloadKeepsATailedClipAndNeverPlaysTheReusedId) {
    Rig rig;
    auto loadOffline = [&](const std::string& name, float v) {
        const std::string path = rig.write(name, kEditLoop, dc(v));
        int32_t id = -1;
        EXPECT_EQ(rig.looper.songLoadClip(path.c_str(), kSR, 0, 0, kEditLoop, true, id),
                  wm::SongStatus::Ok);
        return id;
    };
    const int32_t x = loadOffline("off_x", 0.5f);
    const int32_t y = loadOffline("off_y", -0.5f);
    const int64_t end = int64_t{1} << 22;
    const std::vector<wm::SongSection> a{sec(0, x)}, b{sec(0, y)};
    ASSERT_EQ(rig.looper.songSetSchedule(kEditLoop, a.data(), 1, end, true), wm::SongStatus::Ok);
    std::vector<float> out = rig.render(0, 8192, 64);
    ASSERT_EQ(rig.looper.songSetSchedule(kEditLoop, b.data(), 1, end, true), wm::SongStatus::Ok);
    std::vector<float> seg = rig.render(8192, 64, 64);   // cola viva sobre X
    out.insert(out.end(), seg.begin(), seg.end());
    ASSERT_EQ(rig.looper.songUnloadClip(x, true), wm::SongStatus::Ok);
    const int32_t g = loadOffline("off_g", 0.9f);
    seg = rig.render(8256, 512, 64);
    out.insert(out.end(), seg.begin(), seg.end());
    EXPECT_LE(maxStep(out, 8193, 8256 + 300), 2.0 * normalFadeStep())
        << "la cola de X leyo el clip reusado (id " << g << ")";
}

// Bug plausible (review 3): offline libera al instante un clip que la PROXIMA adopcion va a
// congelar como cola. Agenda B publicada y no adoptada; la agenda adoptada (A) nombra X; X no esta
// en la publicada, asi que el unload pasa; cargar G reusa el id y la cola de A lee G (escalon).
TEST(SongUnload, AC_055_13_OfflineUnloadKeepsAClipThatTheNextAdoptionWillTail) {
    Rig rig;
    auto loadOffline = [&](const std::string& name, float v) {
        const std::string path = rig.write(name, kEditLoop, dc(v));
        int32_t id = -1;
        EXPECT_EQ(rig.looper.songLoadClip(path.c_str(), kSR, 0, 0, kEditLoop, true, id),
                  wm::SongStatus::Ok);
        return id;
    };
    const int32_t x = loadOffline("off2_x", 0.5f);
    const int32_t y = loadOffline("off2_y", -0.5f);
    const int64_t end = int64_t{1} << 22;
    const std::vector<wm::SongSection> a{sec(0, x)}, b{sec(0, y)};
    ASSERT_EQ(rig.looper.songSetSchedule(kEditLoop, a.data(), 1, end, true), wm::SongStatus::Ok);
    std::vector<float> out = rig.render(0, 8192, 64);
    ASSERT_EQ(rig.looper.songSetSchedule(kEditLoop, b.data(), 1, end, true), wm::SongStatus::Ok);
    ASSERT_EQ(rig.looper.songUnloadClip(x, true), wm::SongStatus::Ok);
    const int32_t g = loadOffline("off2_g", 0.9f);
    std::vector<float> seg = rig.render(8192, 512, 64);   // adopta B: la cola de A lee X
    out.insert(out.end(), seg.begin(), seg.end());
    EXPECT_LE(maxStep(out, 8180, 8192 + 300), 2.0 * normalFadeStep())
        << "la cola de A leyo el clip reusado (id " << g << ")";
}

// Bug plausible (review 3): un render offline reiniciado arranca con la agenda del render
// anterior en `mCur`: la primera adopcion la congela como cola y suena un clip ajeno. Tiene que
// dar los mismos bits que una instancia fresca.
TEST(SongOffline, AC_055_13_ARestartedRenderLeavesNoTailOfThePreviousSchedule) {
    constexpr int64_t kStart = 4096;
    const int64_t end = int64_t{1} << 22;
    auto run = [&](bool reuse) {
        Rig rig;
        auto loadOffline = [&](const std::string& name, float v) {
            const std::string path = rig.write(name, kEditLoop, dc(v));
            int32_t id = -1;
            EXPECT_EQ(rig.looper.songLoadClip(path.c_str(), kSR, 0, 0, kEditLoop, true, id),
                      wm::SongStatus::Ok);
            return id;
        };
        const int32_t x = loadOffline("rst_x", 0.5f);
        const int32_t y = loadOffline("rst_y", -0.5f);
        if (reuse) {
            const std::vector<wm::SongSection> a{sec(0, x)};
            EXPECT_EQ(rig.looper.songSetSchedule(kEditLoop, a.data(), 1, end, true),
                      wm::SongStatus::Ok);
            rig.render(0, kStart, 64);
            rig.looper.resetSongForRender();
        }
        const std::vector<wm::SongSection> b{sec(kStart, y)};
        EXPECT_EQ(rig.looper.songSetSchedule(kEditLoop, b.data(), 1, end, true),
                  wm::SongStatus::Ok);
        return rig.render(kStart, 1024, 64);
    };
    const std::vector<float> fresh = run(false), reused = run(true);
    ASSERT_EQ(fresh.size(), reused.size());
    EXPECT_EQ(0, std::memcmp(fresh.data(), reused.data(), fresh.size() * sizeof(float)))
        << "el render reiniciado arranco con un fundido de la agenda anterior";
}
