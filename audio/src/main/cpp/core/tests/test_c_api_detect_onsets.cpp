/**
 * REQ-055 S15 — `wma_detect_onsets`: los onsets de un buffer cualquiera, con el MISMO algoritmo
 * que `TrackBuffer::detectOnsets` (una sola implementacion: `looper/OnsetDetection.h`).
 *
 * Que se prueba: (1) que encuentra golpes en frames conocidos; (2) que da EXACTAMENTE lo mismo que
 * el detector del looper sobre el mismo contenido (si fueran dos copias, la calibracion de S2 —hop
 * 256, 10 ms— valdria para una sola); (3) que sus fronteras (nulos, capacidad, hop enorme) no
 * escriben fuera ni desbordan: la entrada llega por JNI desde la JVM del consumidor.
 */

#include "api/watermelon_audio.h"
#include "looper/TrackBuffer.h"
#include "support/RenderChecks.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr int kHop = 256;
constexpr int kFirstHit = 24000;
constexpr int kHitSpacing = 12000;
constexpr int kHits = 6;
constexpr int kFrames = kFirstHit + kHitSpacing * kHits;

/// Golpes de seno decreciente (con un piso de ruido bajo) en frames conocidos, mono duplicado a L y R.
std::vector<float> synthHits(int hitOffset = 0) {
    std::vector<float> stereo(static_cast<size_t>(kFrames) * 2, 0.0f);
    for (int n = 0; n < kFrames; ++n) {
        const float floorNoise = 0.0005f * std::sin(0.37f * static_cast<float>(n));
        stereo[2 * n] = stereo[2 * n + 1] = floorNoise;
    }
    for (int k = 0; k < kHits; ++k) {
        const int start = kFirstHit + hitOffset + k * kHitSpacing;
        for (int i = 0; i < 6000 && start + i < kFrames; ++i) {
            const float env = std::exp(-static_cast<float>(i) / 1200.0f);
            const float v = 0.6f * env * std::sin(2.0f * static_cast<float>(M_PI) * 220.0f * static_cast<float>(i) / kRate);
            stereo[2 * (start + i)] += v;
            stereo[2 * (start + i) + 1] += v;
        }
    }
    return stereo;
}

std::vector<int> detect(const std::vector<float>& stereo, int hop = kHop, float sens = 1.0f, int cap = 256) {
    std::vector<int> out(static_cast<size_t>(cap), -1);
    const int n = wma_detect_onsets(stereo.data(), static_cast<int>(stereo.size() / 2), kRate,
                                    out.data(), cap, hop, sens);
    EXPECT_GE(n, 0);
    out.resize(static_cast<size_t>(std::max(0, n)));
    return out;
}

}  // namespace

// Bug plausible: la funcion lee el buffer con otro paso (planar en vez de intercalado) o con el
// frame corrido: los onsets caen en otro lado, o no hay ninguno.
//
// 🔴 El detector ubica el golpe ANTES de su frame, no en el: la energia se suaviza con una media de
// ±2 ventanas ANTES del flujo (para no disparar con un tono sostenido), y el flujo sube en la
// ventana donde el suavizado ya lo "ve". Medido con estos golpes: 544..704 frames (2,1..2,75 hops)
// antes. La cota es la del diseño: 2 ventanas de suavizado + la ventana del golpe = 3 hops, y nunca
// despues de la ventana del golpe. "A <= 1 hop" de S15 es la DIFERENCIA entre dos renders (ver el
// test de corrimiento de abajo y `RenderChecks.h`), no la distancia al frame del golpe.
TEST(DetectOnsets, AC_055_15_FindsSyntheticHitsWhereTheLooperDetectorPlacesThem) {
    const auto onsets = detect(synthHits());
    ASSERT_EQ(onsets.size(), static_cast<size_t>(kHits)) << "un onset por golpe";
    for (int k = 0; k < kHits; ++k) {
        const int expected = kFirstHit + k * kHitSpacing;
        const int found = onsets[static_cast<size_t>(k)];
        EXPECT_LE(found, expected) << "golpe " << k << ": un onset despues del golpe";
        EXPECT_GE(found, expected - 3 * kHop) << "golpe " << k << ": mas de 3 hops antes";
    }
    EXPECT_TRUE(std::is_sorted(onsets.begin(), onsets.end())) << "ascendentes";
    EXPECT_GE(onsets.front(), 0) << "nunca negativos";
}

// Bug plausible: la deteccion pierde resolucion (un hop efectivo mayor que el pedido): un golpe
// corrido 100 frames (2,1 ms) tiene que mover su onset a lo sumo un hop, el margen de S15 (10 ms).
TEST(DetectOnsets, AC_055_15_AShiftedHitMovesItsOnsetByAtMostOneHop) {
    const auto base = detect(synthHits(0));
    const auto shifted = detect(synthHits(100));
    ASSERT_EQ(base.size(), static_cast<size_t>(kHits));
    ASSERT_EQ(shifted.size(), base.size());
    EXPECT_LE(wma_test::maxOnsetDifferenceMs(base, shifted, kRate),
              1000.0 * kHop / kRate + 1e-9);
}

// Bug plausible: una copia del algoritmo que deriva del looper (otra constante, otro orden de
// float): el re-render se mediria contra una regla distinta de la de la toma. Mismo contenido en
// una pista del looper => EXACTAMENTE los mismos frames, con varios hop y sensibilidades.
TEST(DetectOnsets, AC_055_15_MatchesTheLooperDetectorOnTheSameContentExactly) {
    const auto stereo = synthHits();
    for (int hop : {16, 32, 128, 256, 512}) {
        for (float sens : {0.5f, 1.0f, 2.0f}) {
            const auto viaCApi = detect(stereo, hop, sens);
            const auto viaLooper = wma_test::onsetsOf(stereo, kRate, hop, sens);
            EXPECT_EQ(viaCApi, viaLooper) << "hop " << hop << " sensibilidad " << sens;
        }
    }
    // y la comparacion no es vacia: con el hop de S2 hay onsets que comparar
    EXPECT_FALSE(detect(stereo).empty());
}

// Bug plausible: la capacidad no se respeta (se escribe un onset de mas) o un tope chico cambia
// QUE onsets salen (deben ser los primeros, no otros).
TEST(DetectOnsets, AC_055_15_NeverWritesPastTheCapacityAndKeepsTheFirstOnes) {
    const auto stereo = synthHits();
    const auto all = detect(stereo);
    ASSERT_GE(all.size(), 4u);
    constexpr int kGuard = 4;
    for (int cap : {1, 2, 3}) {
        std::vector<int> out(static_cast<size_t>(cap + kGuard), -7);
        const int n = wma_detect_onsets(stereo.data(), static_cast<int>(stereo.size() / 2), kRate,
                                        out.data(), cap, kHop, 1.0f);
        EXPECT_EQ(n, cap);
        for (int i = 0; i < cap; ++i) EXPECT_EQ(out[static_cast<size_t>(i)], all[static_cast<size_t>(i)]);
        for (int i = cap; i < cap + kGuard; ++i) {
            EXPECT_EQ(out[static_cast<size_t>(i)], -7) << "se escribio fuera de la capacidad " << cap;
        }
    }
}

// Bug plausible: un argumento invalido derreferencia o devuelve basura negativa (el JNI lo usa como
// largo de array). Todo invalido da 0.
TEST(DetectOnsets, AC_055_15_InvalidArgumentsGiveZeroNeverNegative) {
    const auto stereo = synthHits();
    const int frames = static_cast<int>(stereo.size() / 2);
    int out[8] = {};
    EXPECT_EQ(wma_detect_onsets(nullptr, frames, kRate, out, 8, kHop, 1.0f), 0);
    EXPECT_EQ(wma_detect_onsets(stereo.data(), frames, kRate, nullptr, 8, kHop, 1.0f), 0);
    EXPECT_EQ(wma_detect_onsets(stereo.data(), 0, kRate, out, 8, kHop, 1.0f), 0);
    EXPECT_EQ(wma_detect_onsets(stereo.data(), -5, kRate, out, 8, kHop, 1.0f), 0);
    EXPECT_EQ(wma_detect_onsets(stereo.data(), frames, kRate, out, 0, kHop, 1.0f), 0);
    EXPECT_EQ(wma_detect_onsets(stereo.data(), frames, kRate, out, -1, kHop, 1.0f), 0);
    // demasiado corto para cuatro ventanas, y silencio
    EXPECT_EQ(wma_detect_onsets(stereo.data(), kHop * 4, kRate, out, 8, kHop, 1.0f), 0);
    const std::vector<float> silence(static_cast<size_t>(frames) * 2, 0.0f);
    EXPECT_EQ(wma_detect_onsets(silence.data(), frames, kRate, out, 8, kHop, 1.0f), 0);
}

// Bug plausible: `hop * 4` desborda con un hop enorme (UB de int con signo, y un `len <= negativo`
// pasa el guard): con la entrada no confiable de la JVM eso lee fuera del buffer. Tiene que dar 0.
TEST(DetectOnsets, AC_055_15_AnHugeHopCannotOverflowTheGuard) {
    const auto stereo = synthHits();
    const int frames = static_cast<int>(stereo.size() / 2);
    int out[8] = {};
    for (int hop : {INT_MAX, INT_MAX / 2, INT_MAX / 4 + 1, INT_MIN}) {
        const int n = wma_detect_onsets(stereo.data(), frames, kRate, out, 8, hop, 1.0f);
        if (hop == INT_MIN) {
            // un hop negativo sube a 32: es el hop minimo, no un error
            EXPECT_GE(n, 0);
        } else {
            EXPECT_EQ(n, 0) << "hop " << hop;
        }
    }
}

// Bug plausible: el resultado depende de dónde empieza el buffer en memoria (un offset global en
// vez de relativo al inicio): recortar la toma para re-renderizar movería todos los onsets.
TEST(DetectOnsets, AC_055_15_FramesAreRelativeToTheStartOfTheBuffer) {
    const auto stereo = synthHits();
    const int shift = 4 * kHop;   // multiplo del hop: la grilla de ventanas no cambia
    const std::vector<float> trimmed(stereo.begin() + static_cast<long>(shift) * 2, stereo.end());
    const auto full = detect(stereo);
    const auto cut = detect(trimmed);
    ASSERT_FALSE(cut.empty());
    ASSERT_GE(full.size(), cut.size());
    // los golpes siguen en el mismo contenido: cada onset del recorte es uno del completo, corrido
    for (int onset : cut) {
        EXPECT_NE(std::find(full.begin(), full.end(), onset + shift), full.end())
            << "onset " << onset << " del recorte no corresponde a ninguno del buffer completo";
    }
}

// ---- Golden de 2.27.0 -------------------------------------------------------------------------
// Los frames de abajo los dio `TrackBuffer::detectOnsets` ANTES de extraer `wma::onset::detect`
// (commit 31451966, el master que publico 2.27.0), sobre ESTE buffer. Los tests de equivalencia de
// arriba comparan la implementacion nueva consigo misma; esto la compara con la vieja.
// Si los senos/exp de otra libm movieran un frame, ver primero que el buffer siga teniendo 6 golpes
// bien separados antes de tocar los numeros: un cambio legitimo del algoritmo se re-calibra a
// proposito (S2 calibro hop 256 = 10 ms), no se acomoda aca.
namespace {

constexpr int kGoldRate = 48000;
constexpr int kGoldFrames = 240000;   // 5 s

/// Golpes de seno decreciente (algunos con rafaga de ruido), un piso de ruido y un tono sostenido
/// desde la mitad: todo con aritmetica entera (LCG) o floats simples, determinista.
std::vector<float> goldenBuffer() {
    std::vector<float> stereo(static_cast<size_t>(kGoldFrames) * 2, 0.0f);
    uint32_t lcg = 12345u;
    auto noise = [&lcg]() {
        lcg = lcg * 1664525u + 1013904223u;
        return (static_cast<float>(lcg >> 8) / 8388608.0f) - 1.0f;   // [-1, 1)
    };
    for (int n = 0; n < kGoldFrames; ++n) {
        const float floorNoise = 0.003f * noise();
        float l = floorNoise;
        float r = floorNoise * 0.5f;
        if (n >= 150000) {   // tono sostenido: no es un golpe, es energia estable
            const float t = 0.12f * std::sin(2.0f * static_cast<float>(M_PI) * 440.0f *
                                              static_cast<float>(n - 150000) / kGoldRate);
            l += t;
            r += t;
        }
        stereo[2 * n] = l;
        stereo[2 * n + 1] = r;
    }
    const int starts[] = {20000, 50000, 80000, 110000, 135000, 190000};
    const float amps[] = {0.7f, 0.25f, 0.6f, 0.4f, 0.7f, 0.5f};
    for (int k = 0; k < 6; ++k) {
        for (int i = 0; i < 7000 && starts[k] + i < kGoldFrames; ++i) {
            const float env = std::exp(-static_cast<float>(i) / 1500.0f);
            float v = amps[k] * env * std::sin(2.0f * static_cast<float>(M_PI) * 330.0f *
                                                static_cast<float>(i) / kGoldRate);
            if (k % 2 == 1 && i < 2000) v += 0.3f * env * noise();   // golpe con ataque ruidoso
            stereo[2 * (starts[k] + i)] += v;
            stereo[2 * (starts[k] + i) + 1] += v;
        }
    }
    return stereo;
}

struct OnsetGolden {
    int hop;
    float sensitivity;
    std::vector<int> frames;
};

const std::vector<OnsetGolden>& onsetGoldens() {
    static const std::vector<OnsetGolden> g = {
        {256, 1.0f, {19456, 49408, 79360, 109312, 134400, 149504, 189440}},
        {128, 1.0f, {19712, 49664, 79744, 109696, 134656, 149632, 189696}},
        {256, 1.5f, {19456, 49408, 79360, 109312, 134400, 149504, 189440}},
        {128, 4.0f, {19712, 49664, 79744, 109696, 134656, 149632, 189696, 192000}},
        {512, 0.5f, {18944, 48640, 78848, 108544, 133632, 148992, 188928}},
    };
    return g;
}

}  // namespace

// Bug plausible: un cambio del algoritmo respecto del de 2.27.0 (otra constante, otra ventana de
// suavizado, otro orden de float, otro piso de energia) que los tests de equivalencia no ven porque
// mueven las DOS rutas a la vez. Frames EXACTOS, por la C API y por el looper, en tres hops y
// cuatro sensibilidades; el buffer lleva golpes, ruido y un tono sostenido.
TEST(DetectOnsets, AC_055_15_GoldenFramesOfRelease2_27_0ByCApiAndByTheLooper) {
    const auto stereo = goldenBuffer();
    for (const auto& g : onsetGoldens()) {
        const std::string at = "hop " + std::to_string(g.hop) + " sens " + std::to_string(g.sensitivity);
        std::vector<int> out(256, -1);
        const int n = wma_detect_onsets(stereo.data(), kGoldFrames, kGoldRate, out.data(),
                                        static_cast<int>(out.size()), g.hop, g.sensitivity);
        ASSERT_GE(n, 0) << at;
        out.resize(static_cast<size_t>(n));
        EXPECT_EQ(out, g.frames) << "wma_detect_onsets, " << at;
        EXPECT_EQ(wma_test::onsetsOf(stereo, kGoldRate, g.hop, g.sensitivity), g.frames)
            << "TrackBuffer::detectOnsets, " << at;
    }
}
