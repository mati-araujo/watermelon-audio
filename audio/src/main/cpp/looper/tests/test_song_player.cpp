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
