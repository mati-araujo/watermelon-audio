/**
 * REQ-055 S2, tarea 2.6 — los umbrales de S11 y S15, CALIBRADOS con renders reales.
 *
 * Los instrumentos viven en `support/RenderChecks.h`; aca se mide de donde salen sus
 * numeros, y el test queda como vigia: si el motor cambia y un render limpio empieza
 * a saltar mas que el umbral, o un corte deja de detectarse, o el onset de un
 * re-render se aleja mas de lo fijado, se pone rojo aca y no en S11/S15.
 *
 * Todo entra por la API C de render offline (`wma_offline_*`), igual que lo va a
 * usar Studio.
 */

#include "support/MinimalSoundFont.h"
#include "support/RenderChecks.h"

#include "api/watermelon_audio.h"
#include "platform/Logger.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr int kEngineCount = 7;
constexpr int kSoundFont = 6;
constexpr int kHallReverb = 17, kDelay = 2, kChorus = 6, kTapeEcho = 16;

const char* engineName(int t) {
    static const char* const kNames[kEngineCount] = {
        "CLASSIC", "KARPLUS_STRONG", "FM", "WAVETABLE", "GRANULAR", "SUPERSAW", "SOUNDFONT"};
    return kNames[t];
}

const std::vector<uint8_t>& sf() {
    static const std::vector<uint8_t> kBytes = wma_test::makeMinimalSoundFont(22050, true);
    return kBytes;
}

/// A volumen de EXPORTACION (master y amplitud a 1): el peor caso para el piso de
/// saltos de un render limpio.
WmaEngine* hotEngine(int t, bool withChain) {
    WmaEngine* e = wma_engine_create();
    if (!e) return nullptr;
    wma_set_master_volume(e, 1.0f);
    if (t == kSoundFont) {
        wma_sf_load_data(e, sf().data(), static_cast<int>(sf().size()));
        wma_sf_set_preset(e, 0);
    }
    wma_set_engine_type(e, t);
    if (withChain) {
        for (int fx : {kChorus, kDelay, kTapeEcho, kHallReverb}) wma_effect_add(e, fx);
    }
    return e;
}

/// Un gesto: frecuencia/amplitud (y note on/off en SoundFont) en el frame `at`.
struct Gesture {
    int at;
    float freq;
    float amp;
};

/// Renderiza `frames` frames en bloques de `block`, aplicando cada gesto en el
/// primer borde de bloque >= su frame (asi aplica los gestos un re-render).
std::vector<float> render(WmaEngine* e, int t, int block, int frames,
                          const std::vector<Gesture>& gestures) {
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    if (wma_offline_start(e, kRate, block) != WMA_OK) return {};
    size_t next = 0;
    bool noteOn = false;
    for (int done = 0; done < frames; done += block) {
        while (next < gestures.size() && gestures[next].at <= done) {
            const auto& g = gestures[next++];
            wma_set_frequency_amplitude(e, g.freq, g.amp);
            if (t == kSoundFont) {
                if (g.amp > 0.0f && !noteOn) {
                    wma_sf_note_on(e, 1, static_cast<int>(std::lround(
                                          69 + 12 * std::log2(g.freq / 440.0f))),
                                   1.0f);
                    noteOn = true;
                } else if (g.amp == 0.0f && noteOn) {
                    wma_sf_note_off(e, 1);
                    noteOn = false;
                }
            }
        }
        const int n = std::min(block, frames - done);
        if (wma_offline_render(e, out.data() + static_cast<size_t>(done) * 2, nullptr, n) != WMA_OK) {
            return {};
        }
    }
    wma_offline_stop(e);
    return out;
}

/// La agenda de gestos de los renders limpios: un barrido de alturas sostenidas,
/// cambios de amplitud y un final con cola.
std::vector<Gesture> sweepSchedule() {
    std::vector<Gesture> g;
    for (int k = 0; k < 24; ++k) {
        g.push_back({k * 2048, 110.0f * std::pow(2.0f, static_cast<float>(k) / 5.0f),
                     k < 20 ? 1.0f : 0.0f});
    }
    return g;
}

/// Los margenes de la calibracion (docs/dev/render-offline.md, RenderChecks.h):
/// el umbral tiene que quedar >= 1,5x sobre la razon maxima de las uniones limpias
/// (medido 1,28 contra 2,0: x1,57), y el detector tiene que seguir viendo al menos la
/// mitad de los cortes inyectados (medido 54 %): un piso de REGRESION del detector,
/// no una promesa de cobertura.
constexpr float kCleanMargin = 1.5f;
constexpr double kMinCutDetection = 0.5;

class RenderChecks : public ::testing::Test {
protected:
    void SetUp() override { wma::setLogCallback([](wma::LogLevel, const char*, const char*) {}); }
    void TearDown() override { wma::setLogCallback(nullptr); }
};

}  // namespace

TEST_F(RenderChecks, TheJumpThresholdSeparatesCleanRendersFromCuts) {
    // Las uniones candidatas: 40 frames dentro de lo sostenido, los mismos para los
    // renders limpios (donde NO hay union) y para los cortes inyectados.
    std::vector<size_t> junctions;
    for (int c = 0; c < 40; ++c) junctions.push_back(4096 + static_cast<size_t>(c) * 997);

    // (a) Renders LIMPIOS, a volumen de exportacion, 7 engines, secos y con cadena: la
    // razon salto/entorno en esos frames es el piso que el umbral deja abajo.
    constexpr int kFrames = 24 * 2048;
    std::vector<std::vector<float>> clean;
    std::vector<float> cleanRatios;
    for (int t = 0; t < kEngineCount; ++t) {
        for (bool chain : {false, true}) {
            WmaEngine* e = hotEngine(t, chain);
            ASSERT_NE(e, nullptr);
            auto out = render(e, t, 256, kFrames, sweepSchedule());
            wma_engine_destroy(e);
            ASSERT_FALSE(out.empty());
            float worst = 0.0f;
            for (size_t n : junctions) worst = std::max(worst, wma_test::junctionRatio(out, n));
            std::printf("[ SALTOS  ] limpio %-15s %s: salto absoluto max %.3f, razon max en las "
                        "uniones %.2f\n",
                        engineName(t), chain ? "con cadena" : "seco      ",
                        wma_test::maxSampleJump(out), worst);
            cleanRatios.push_back(worst);
            EXPECT_TRUE(wma_test::findJunctionDiscontinuities(out, junctions).empty())
                << engineName(t) << (chain ? " con cadena" : " seco")
                << ": un render limpio tiene una discontinuidad en una union que no existe";
            clean.push_back(std::move(out));
        }
    }
    const float cleanMax = *std::max_element(cleanRatios.begin(), cleanRatios.end());

    // (b) Cortes INYECTADOS: la union de dos renders distintos sin fundido (lo que S11
    // tiene que no hacer) y el corte a silencio, en cada union candidata.
    std::vector<float> cutRatios;
    int detected = 0, total = 0;
    for (size_t c = 0; c < junctions.size(); ++c) {
        const size_t at = junctions[c];
        const auto& a = clean[c % clean.size()];
        const auto& b = clean[(c + 5) % clean.size()];
        for (bool toSilence : {false, true}) {
            std::vector<float> spliced(a.begin(), a.begin() + static_cast<long>(at * 2));
            if (toSilence) {
                spliced.resize(a.size(), 0.0f);
            } else {
                spliced.insert(spliced.end(), b.begin() + static_cast<long>(at * 2), b.end());
            }
            ++total;
            cutRatios.push_back(wma_test::junctionRatio(spliced, at));
            if (!wma_test::findJunctionDiscontinuities(spliced, {at}).empty()) ++detected;
        }
    }
    std::sort(cutRatios.begin(), cutRatios.end());
    std::printf("[ SALTOS  ] uniones limpias: razon max %.2f | umbral %.1f | cortes %d: razon "
                "p10 %.2f, p25 %.2f, mediana %.2f; detectados %d (%.0f %%)\n",
                cleanMax, wma_test::kJumpRatio, total, cutRatios[cutRatios.size() / 10],
                cutRatios[cutRatios.size() / 4], cutRatios[cutRatios.size() / 2], detected,
                100.0 * detected / total);
    for (float k : {1.5f, 2.0f, 3.0f, 4.0f}) {
        const auto above = std::count_if(cutRatios.begin(), cutRatios.end(),
                                         [k](float r) { return r > k; });
        std::printf("[ SALTOS  ]   con umbral %.1f: margen x%.2f sobre las uniones limpias, "
                    "cortes detectados %ld de %d\n", k, k / cleanMax, static_cast<long>(above), total);
    }
    RecordProperty("clean_max_ratio", std::to_string(cleanMax));
    RecordProperty("cuts_detected_pct", std::to_string(100.0 * detected / total));

    EXPECT_LT(cleanMax * kCleanMargin, wma_test::kJumpRatio)
        << "el piso de las uniones limpias ya no deja el margen declarado bajo el umbral";
    EXPECT_GE(static_cast<double>(detected) / total, kMinCutDetection)
        << "el detector dejo de ver los cortes que vio al calibrarse";
}

TEST_F(RenderChecks, TheOnsetThresholdCoversQuantizationAndDetector) {
    // La "toma grabada" aplica cada gesto en su frame exacto (bloques de 32); el
    // re-render, en el borde de bloque de 256 siguiente, que es lo que hace S15. Los
    // gestos se corren dentro del bloque para barrer la cuantizacion. Notas separadas 250 ms (> los 50 ms de refractario).
    constexpr int kFrames = 12 * 12000;
    double worst = 0.0;
    int compared = 0;
    for (int t : {1, 2, 5, kSoundFont}) {
        // 0, el medio y el borde del bloque: los extremos de la cuantizacion. (Con los 8
        // valores 0..224 de a 32 el peor fue el mismo, 5,33 ms: 2026-10-26.)
        for (int shift : {0, 96, 160, 224}) {
            std::vector<Gesture> g;
            for (int k = 0; k < 10; ++k) {
                const int on = 6000 + k * 12000 + shift;
                g.push_back({on, 220.0f * std::pow(2.0f, static_cast<float>(k % 5) / 12.0f), 1.0f});
                g.push_back({on + 6000, 220.0f, 0.0f});
            }
            WmaEngine* rec = hotEngine(t, false);
            WmaEngine* re = hotEngine(t, false);
            ASSERT_TRUE(rec && re);
            const auto recorded = render(rec, t, 32, kFrames, g);
            const auto rerendered = render(re, t, 256, kFrames, g);
            wma_engine_destroy(rec);
            wma_engine_destroy(re);

            const auto a = wma_test::onsetsOf(recorded, kRate);
            const auto b = wma_test::onsetsOf(rerendered, kRate);
            const double diff = wma_test::maxOnsetDifferenceMs(a, b, kRate);
            ASSERT_EQ(a.size(), 10u) << engineName(t) << " shift " << shift
                                     << ": el detector no vio las 10 notas de la toma";
            ASSERT_EQ(b.size(), a.size()) << engineName(t) << " shift " << shift;
            worst = std::max(worst, diff);
            ++compared;
        }
        std::printf("[ ONSETS  ] %-15s peor diferencia hasta aca %.2f ms\n", engineName(t), worst);
    }
    std::printf("[ ONSETS  ] %d comparaciones, peor %.2f ms, umbral %.1f ms\n", compared, worst,
                wma_test::kMaxOnsetDifferenceMs);
    RecordProperty("worst_onset_ms", std::to_string(worst));
    EXPECT_LE(worst, wma_test::kMaxOnsetDifferenceMs);
}
