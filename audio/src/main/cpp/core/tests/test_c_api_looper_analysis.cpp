// ============================================================================
// REQ-043 S1 — AC-043.6 (C) y el contrato de las dos lecturas por la C API:
// wma_looper_analyze_pitch y wma_looper_get_level_envelope.
//
// Entra por wma_engine_create() + wma_looper_import_track(), como un consumidor. Lo que
// afirma es la FRONTERA —hop devuelto, eje, 0/0 en el silencio, 0 elementos, out-params
// que no se tocan— sobre el fixture del glide; las metricas contra el oraculo viven en
// looper/tests/test_track_analysis.cpp, que es donde se midieron.
//
// El fixture es de solo lectura y vive en el arbol (looper/tests/testdata/, sha256 en su
// MANIFEST.txt); WMA_LOOPER_TESTDATA_DIR lo define core/tests/CMakeLists.txt.
// ============================================================================

#include <gtest/gtest.h>

#include "support/CApiFixture.h"
#include "api/watermelon_audio.h"

#include <string>
#include <vector>

#ifndef WMA_LOOPER_TESTDATA_DIR
#error "WMA_LOOPER_TESTDATA_DIR lo define core/tests/CMakeLists.txt"
#endif

namespace {

constexpr int kSampleRate = 48000;

std::string glidePath() {
    return std::string(WMA_LOOPER_TESTDATA_DIR) + "/glide-voz.wav";
}

class CApiLooperAnalysisTest : public wma_test::CApiFixture {
protected:
    void importGlide(int track) {
        startAt(kSampleRate, /*fadeTimeMs=*/0);
        ASSERT_TRUE(wma_looper_import_track(mWma, track, glidePath().c_str(), kSampleRate))
            << glidePath();
        ASSERT_EQ(wma_looper_get_track_length_frames(mWma, track), 216000);
    }
};

}  // namespace

// AC-043.6 (C): una pista inactiva devuelve 0 elementos en las dos lecturas, sin tocar los
// buffers de salida ni first_frame; hop_frames SI se escribe, para dimensionar.
// Que bug atrapa: publicar una serie de una pista vacia; escribir first_frame con 0 bins;
// un hop que dependa de que haya pista.
TEST_F(CApiLooperAnalysisTest, InactiveTrackYieldsZeroElementsInBothReadings) {
    startAt(kSampleRate, 0);
    int frames[4] = {-1, -1, -1, -1};
    float hz[4] = {-1, -1, -1, -1}, conf[4] = {-1, -1, -1, -1}, bins[4] = {-1, -1, -1, -1};
    int hop = -1, first = -7, hopEnv = -1;

    EXPECT_EQ(wma_looper_analyze_pitch(mWma, 5, 10.0f, frames, hz, conf, 4, &hop), 0);
    EXPECT_EQ(hop, 480) << "round(10 ms · 48 kHz), aunque no haya puntos";
    EXPECT_EQ(wma_looper_get_level_envelope(mWma, 5, 100.0f, bins, 4, &first, &hopEnv), 0);
    EXPECT_EQ(hopEnv, 480) << "round(48000 / 100), aunque no haya bins";
    EXPECT_EQ(first, -7) << "first_frame se escribe SOLO con > 0 bins";
    EXPECT_EQ(frames[0], -1);
    EXPECT_EQ(hz[0], -1.0f);
    EXPECT_EQ(bins[0], -1.0f);

    // Sin motor, sin hop, sin buffers: 0, nunca negativo, nunca una escritura.
    EXPECT_EQ(wma_looper_analyze_pitch(nullptr, 0, 10.0f, frames, hz, conf, 4, &hop), 0);
    EXPECT_EQ(wma_looper_analyze_pitch(mWma, 0, 0.0f, frames, hz, conf, 4, &hop), 0);
    EXPECT_EQ(wma_looper_analyze_pitch(mWma, 0, 10.0f, nullptr, hz, conf, 4, nullptr), 0);
    EXPECT_EQ(wma_looper_get_level_envelope(nullptr, 0, 100.0f, bins, 4, &first, nullptr), 0);
    EXPECT_EQ(wma_looper_get_level_envelope(mWma, 0, 0.0f, bins, 4, &first, &hopEnv), 0);
    EXPECT_EQ(wma_looper_get_level_envelope(mWma, 0, 100.0f, nullptr, 4, &first, nullptr), 0);
    EXPECT_EQ(frames[0], -1);
    EXPECT_EQ(bins[0], -1.0f);
}

// La serie por la C API: hop devuelto exacto, primer frame en loopStart + W/2, hop entre
// puntos, 0/0 exactos en el silencio del fixture y pitch en los tramos tonales.
// Que bug atrapa: una conversion ms → frames distinta de la del motor; un eje relativo a
// la region en vez de absoluto; un 0/0 "casi cero" que cruce la frontera.
TEST_F(CApiLooperAnalysisTest, PitchSeriesCarriesTheAxisAndExactZerosAcrossTheBoundary) {
    importGlide(1);
    const int loopStart = 48000, loopEnd = 168000;
    wma_looper_set_track_loop_region(mWma, 1, loopStart, loopEnd);
    ASSERT_EQ(wma_looper_get_track_loop_start(mWma, 1), loopStart);
    ASSERT_EQ(wma_looper_get_track_loop_end(mWma, 1), loopEnd);

    int hop = 0;
    // Primero el hop, para dimensionar como lo haria un bridge.
    EXPECT_EQ(wma_looper_analyze_pitch(mWma, 1, 10.0f, nullptr, nullptr, nullptr, 0, &hop), 0);
    ASSERT_EQ(hop, 480);
    const int capacity = (loopEnd - loopStart) / hop + 1;
    std::vector<int> frames(static_cast<size_t>(capacity), -1);
    std::vector<float> hz(static_cast<size_t>(capacity), -1.0f);
    std::vector<float> conf(static_cast<size_t>(capacity), -1.0f);

    int hopAgain = 0;
    const int n = wma_looper_analyze_pitch(mWma, 1, 10.0f, frames.data(), hz.data(),
                                           conf.data(), capacity, &hopAgain);
    EXPECT_EQ(hopAgain, 480);
    // (120000 − 1920) / 480 + 1 = 247 ventanas enteras.
    ASSERT_EQ(n, 247);
    EXPECT_EQ(frames[0], loopStart + 960) << "primer punto en loopStart + W/2";
    EXPECT_LE(frames[static_cast<size_t>(n - 1)], loopEnd - 960);
    for (int i = 1; i < n; ++i) {
        ASSERT_EQ(frames[static_cast<size_t>(i)] - frames[static_cast<size_t>(i - 1)], 480);
    }
    EXPECT_EQ(frames[static_cast<size_t>(n)], -1) << "no se escribe mas alla del conteo";

    int tonal = 0, silentExact = 0, silentNotExact = 0;
    for (int i = 0; i < n; ++i) {
        const int f = frames[static_cast<size_t>(i)];
        // El silencio del fixture es [96000, 120000); a mas de W/2 de sus bordes, 0/0 exactos.
        if (f > 96000 + 960 && f < 120000 - 960) {
            if (hz[static_cast<size_t>(i)] == 0.0f && conf[static_cast<size_t>(i)] == 0.0f) {
                ++silentExact;
            } else {
                ++silentNotExact;
            }
        } else if (f < 96000 - 960 || f > 120000 + 960) {
            if (hz[static_cast<size_t>(i)] > 0.0f) ++tonal;
        }
    }
    EXPECT_EQ(silentNotExact, 0);
    EXPECT_GT(silentExact, 40);
    EXPECT_GT(tonal, 180) << "glide (1,0 s) + 220 Hz (1,0 s) dentro de la region";
    for (int i = 0; i < n; ++i) {
        ASSERT_GE(conf[static_cast<size_t>(i)], 0.0f);
        ASSERT_LE(conf[static_cast<size_t>(i)], 1.0f);
    }

    // max_points menor que la serie: se escribe hasta ahi y se devuelve lo escrito.
    EXPECT_EQ(wma_looper_analyze_pitch(mWma, 1, 10.0f, frames.data(), hz.data(), conf.data(),
                                       10, nullptr), 10);
}

// La envolvente por la C API: hop = round(sr / bps), first_frame = loopStart, bins =
// floor(loopLength / hop), valores en [0, 1], y el silencio del fixture en cero exacto.
// Que bug atrapa: un origen que no sea loopStart; un bin parcial de mas; dB o normalizado
// donde se pide RMS lineal crudo.
TEST_F(CApiLooperAnalysisTest, EnvelopeCoversTheRegionWithWindowEqualToHop) {
    importGlide(2);
    const int loopStart = 90000, loopEnd = 130000;   // 40000 frames: 83 bins de 480 + cola
    wma_looper_set_track_loop_region(mWma, 2, loopStart, loopEnd);

    std::vector<float> bins(200, -1.0f);
    int first = -1, hop = -1;
    const int n = wma_looper_get_level_envelope(mWma, 2, 100.0f, bins.data(),
                                                static_cast<int>(bins.size()), &first, &hop);
    EXPECT_EQ(hop, 480);
    ASSERT_EQ(n, 40000 / 480) << "floor: la cola de 160 frames no tiene bin";
    EXPECT_EQ(first, loopStart);
    EXPECT_EQ(bins[static_cast<size_t>(n)], -1.0f);

    // [90000, 96000) es glide a −20 dBFS de pico; [96000, 120000) silencio EXACTO;
    // [120000, 130000) 220 Hz. Bin k cubre [90000 + 480k, +480).
    for (int k = 0; k < n; ++k) {
        const float v = bins[static_cast<size_t>(k)];
        ASSERT_GE(v, 0.0f);
        ASSERT_LE(v, 1.0f);
        const int from = loopStart + k * hop;
        if (from >= 96000 && from + hop <= 120000) {
            EXPECT_EQ(v, 0.0f) << "bin " << k << " dentro del silencio exacto";
        } else if (from + hop <= 96000 || from >= 120000) {
            EXPECT_GT(v, 0.02f) << "bin " << k << " con señal (−20 dBFS de pico)";
            EXPECT_LT(v, 0.2f) << "bin " << k << ": RMS lineal, no pico ni dB";
        }
    }
}
