// ============================================================================
// REQ-043 S1 — el motor por hop: pitch y envolvente por pista, offline.
//
//   AC-043.1  el glide contra su oraculo: max, p95, huecos, octavas, silencio exacto
//   AC-043.2  40 vs 30 ms sobre el tramo de 65 Hz: la mas corta que sostiene AC-043.1
//   AC-043.3  el eje: frame al centro, primer punto en loopStart + W/2, hop exacto
//   AC-043.4  determinismo byte a byte con el afinador offline corriendo entre medio
//   AC-043.5  la envolvente contra los 8 onsets de detectOnsets
//   AC-043.6  pista inactiva ⇒ 0 elementos
//   AC-043.8  el costo, impreso, no afirmado
//
// LOS FIXTURES SON DE SOLO LECTURA Y VIVEN EN EL ARBOL (looper/tests/testdata/, con su
// sha256 en MANIFEST.txt). Se leen por WMA_LOOPER_TESTDATA_DIR, no por FixturePath: ese
// helper es para fixtures que se ESCRIBEN y necesitan un nombre unico por proceso.
//
// EL ORACULO DEL GLIDE ES glide-voz.truth.txt, que sale del GENERADOR y nunca del motor
// (R-API-48): una linea `frame hz` por hop de 480 frames, donde `frame` es el INICIO del
// hop; la f verdadera en el centro de una ventana se interpola entre las dos lineas que
// la rodean. Alrededor de cada frontera de tramo se excluyen ±W/2: ahi la ventana mezcla
// dos regimenes por construccion y ningun detector puede acertar.
//
// 🔴 UMBRALES = TECHO PRE-DECLARADO + TRINQUETE AL MEDIDO (decision 9). El techo (< 1 %,
// 0 octavas, 0 huecos, silencio exacto) lo fija la carta y es el falsador. El trinquete
// (kMaxErrRatchetPct, kP95ErrRatchetPct) es el valor MEDIDO en S1 × 1,5, escrito en el
// mismo commit que la medicion: un techo de 1 % con 0,2 % medido dejaria pasar 5× en
// verde. Si el medido supera el techo, es hallazgo, no un techo que se afloja.
// ============================================================================

#include <gtest/gtest.h>

#include "AudioLooper.h"
#include "TrackAnalysis.h"
#include "WavFile.h"
#include "support/FixturePath.h"
#include "../../analysis/OfflineAnalysis.h"
#include "../../analysis/AnalysisSnapshot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef WMA_LOOPER_TESTDATA_DIR
#error "WMA_LOOPER_TESTDATA_DIR lo define looper/tests/CMakeLists.txt"
#endif

namespace {

constexpr int kSR = 48000;
constexpr double kHopMs = 10.0;

// --- trinquetes de AC-043.1 (medido en S1, ver Notas del doc de etapa) ×1,5 ----------
// Medido el 2026-09-17 sobre glide-voz.wav (sha f7bbc012…) con W = 40 ms, hop 10 ms, en
// el mismo commit que esto: max 0,255 %, p95 0,113 % (391 puntos tonales, 0 huecos, 0
// octavas, 47 de silencio exactos). El techo pre-declarado (1 %) sigue vigente ademas.
constexpr double kCeilingErrPct = 1.0;
constexpr double kMaxErrRatchetPct = 0.383;   // 0,255 × 1,5
constexpr double kP95ErrRatchetPct = 0.170;   // 0,113 × 1,5

std::string testdata(const char* name) {
    return std::string(WMA_LOOPER_TESTDATA_DIR) + "/" + name;
}

/// El oraculo: `frame hz` por hop, y las fronteras de tramo del encabezado.
struct GlideTruth {
    std::vector<int> frames;
    std::vector<double> hz;
    std::vector<int> boundaries;

    static GlideTruth load(const std::string& path) {
        GlideTruth t;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("# fronteras de tramo (frames):", 0) == 0) {
                std::istringstream ss(line.substr(line.find(':') + 1));
                int b;
                while (ss >> b) t.boundaries.push_back(b);
                continue;
            }
            if (line.empty() || line[0] == '#') continue;
            int f = 0;
            double h = 0.0;
            if (std::sscanf(line.c_str(), "%d %lf", &f, &h) == 2) {
                t.frames.push_back(f);
                t.hz.push_back(h);
            }
        }
        return t;
    }

    /// f verdadera en `frame`, interpolada entre los dos hops que lo rodean. Sobre una
    /// frontera con silencio no se interpola: manda el hop mas cercano.
    double at(int frame) const {
        if (frames.empty()) return 0.0;
        auto it = std::upper_bound(frames.begin(), frames.end(), frame);
        if (it == frames.begin()) return hz.front();
        const size_t i1 = static_cast<size_t>(it - frames.begin());
        const size_t i0 = i1 - 1;
        if (i1 >= frames.size()) return hz.back();
        const double h0 = hz[i0], h1 = hz[i1];
        if (h0 == 0.0 || h1 == 0.0) {
            return (frame - frames[i0] < frames[i1] - frame) ? h0 : h1;
        }
        const double t = static_cast<double>(frame - frames[i0]) /
                         static_cast<double>(frames[i1] - frames[i0]);
        return h0 + (h1 - h0) * t;
    }

    bool nearBoundary(int frame, int halfWindow) const {
        for (int b : boundaries) {
            if (std::abs(frame - b) < halfWindow) return true;
        }
        return false;
    }
};

struct PitchSeries {
    std::vector<int> frames;
    std::vector<float> hz;
    std::vector<float> conf;
    int count{0};

    void resize(int n) {
        frames.assign(static_cast<size_t>(n), -1);
        hz.assign(static_cast<size_t>(n), -1.0f);
        conf.assign(static_cast<size_t>(n), -1.0f);
    }
};

/// La tabla de metricas de AC-043.1, sobre los puntos de [from, to) del eje.
struct GlideMetrics {
    double maxErrPct{0.0};
    double p95ErrPct{0.0};
    int tonal{0};
    int gaps{0};
    int octaves{0};
    int silent{0};
    int silentNotExact{0};
    int excluded{0};

    static GlideMetrics measure(const PitchSeries& s, const GlideTruth& truth,
                                int halfWindow, int from, int to) {
        GlideMetrics m;
        std::vector<double> errs;
        for (int i = 0; i < s.count; ++i) {
            const int frame = s.frames[static_cast<size_t>(i)];
            if (frame < from || frame >= to) continue;
            const double ft = truth.at(frame);
            const float fe = s.hz[static_cast<size_t>(i)];
            const float ce = s.conf[static_cast<size_t>(i)];
            // ±W/2 alrededor de cada frontera se excluye ANTES de clasificar: una ventana
            // centrada a 480 frames del corte tiene 10 ms de tono adentro y el detector
            // lo lee (medido: 96480 → 437,7 Hz con claridad 0,92; 119520 → 220,7 con
            // 0,70). Eso no es un 0/0 "casi cero": es la ventana mezclando dos regimenes.
            if (truth.nearBoundary(frame, halfWindow)) { ++m.excluded; continue; }
            if (ft == 0.0) {
                ++m.silent;
                if (fe != 0.0f || ce != 0.0f) ++m.silentNotExact;
                continue;
            }
            ++m.tonal;
            if (fe <= 0.0f) { ++m.gaps; continue; }
            const double ratio = static_cast<double>(fe) / ft;
            if (!(ratio > 0.94 && ratio < 1.06)) { ++m.octaves; continue; }
            errs.push_back(std::abs(static_cast<double>(fe) - ft) / ft * 100.0);
        }
        if (!errs.empty()) {
            std::sort(errs.begin(), errs.end());
            m.maxErrPct = errs.back();
            const size_t k = static_cast<size_t>(std::ceil(0.95 * errs.size())) - 1;
            m.p95ErrPct = errs[std::min(k, errs.size() - 1)];
        }
        return m;
    }

    void print(const char* label) const {
        std::printf("[AC-043.1] %-22s max %.3f %%  p95 %.3f %%  tonales %d  huecos %d  "
                    "octavas %d  silencio %d (no exactos %d)  excluidos ±W/2 %d\n",
                    label, maxErrPct, p95ErrPct, tonal, gaps, octaves, silent,
                    silentNotExact, excluded);
    }
};

/// Mono (L+R)/2 del WAV, la misma convencion que el looper y el ring del afinador.
std::vector<float> monoOf(const wav::WavData& d) {
    std::vector<float> mono(static_cast<size_t>(d.numFrames), 0.0f);
    for (int f = 0; f < d.numFrames; ++f) {
        mono[static_cast<size_t>(f)] = 0.5f * (d.buffer[static_cast<size_t>(f) * 2] +
                                               d.buffer[static_cast<size_t>(f) * 2 + 1]);
    }
    return mono;
}

class TrackAnalysisGlide : public ::testing::Test {
protected:
    void SetUp() override {
        looper.setSampleRate(kSR);
        ASSERT_TRUE(looper.importTrack(0, testdata("glide-voz.wav").c_str(), kSR))
            << testdata("glide-voz.wav");
        ASSERT_EQ(looper.getTrackLengthFrames(0), 216000);
        truth = GlideTruth::load(testdata("glide-voz.truth.txt"));
        ASSERT_FALSE(truth.frames.empty()) << testdata("glide-voz.truth.txt");
        ASSERT_EQ(truth.boundaries.size(), 5u) << "el encabezado del oraculo declara 4 tramos";
        hopFrames = wma::track_analysis::hopFramesForMs(kHopMs, kSR);
        windowFrames = wma::track_analysis::voiceWindowFrames(kSR);
    }

    PitchSeries analyze(int track, int hop) {
        PitchSeries s;
        s.resize(looper.getTrackLengthFrames(track) / hop + 2);
        s.count = looper.analyzeTrackPitch(track, hop, s.frames.data(), s.hz.data(),
                                          s.conf.data(),
                                          static_cast<int>(s.frames.size()));
        return s;
    }

    AudioLooper looper;
    GlideTruth truth;
    int hopFrames{0};
    int windowFrames{0};
};

}  // namespace

// ---------------------------------------------------------------------------
// AC-043.1 — el glide contra su oraculo.
//
// Que bug atrapa: sellar `frame` al inicio de la ventana (M2: +1,4 % a 1 oct/s ⇒ rojo en
// el max); `kPeakThreshold = 1,0` en la entrada nueva (M1: "elegi el maximo" ⇒ octavas);
// un piso o un soporte mal cableado (huecos); un 0/0 "casi cero" en el silencio (la
// interpolacion que la decision 3 prohibe).
// ---------------------------------------------------------------------------
TEST_F(TrackAnalysisGlide, GlideMetricsWithinCeilingAndRatchet) {
    ASSERT_EQ(hopFrames, 480);
    ASSERT_EQ(windowFrames, 1920);
    const PitchSeries s = analyze(0, hopFrames);
    ASSERT_EQ(s.count, wma::track_analysis::pitchPointCount(216000, windowFrames, hopFrames));

    const GlideMetrics all = GlideMetrics::measure(s, truth, windowFrames / 2, 0, 216000);
    all.print("glide-voz entero");
    // Por tramo, para leer donde vive el error (no se afirma por tramo salvo el silencio).
    const char* names[] = {"glide 110->440", "silencio", "220 Hz", "65 Hz"};
    for (size_t t = 0; t + 1 < truth.boundaries.size(); ++t) {
        GlideMetrics::measure(s, truth, windowFrames / 2, truth.boundaries[t],
                              truth.boundaries[t + 1]).print(names[t]);
    }

    EXPECT_GT(all.tonal, 300) << "el fixture tiene 4,0 s tonales a 100 puntos/s";
    EXPECT_LT(all.maxErrPct, kCeilingErrPct) << "techo pre-declarado (carta)";
    EXPECT_LE(all.maxErrPct, kMaxErrRatchetPct) << "trinquete: medido x1,5";
    EXPECT_LE(all.p95ErrPct, kP95ErrRatchetPct) << "trinquete: medido x1,5";
    EXPECT_EQ(all.gaps, 0) << "huecos dentro de los tramos tonales";
    EXPECT_EQ(all.octaves, 0) << "f_est/f_true fuera de (0,94; 1,06)";
    EXPECT_GT(all.silent, 40) << "0,5 s de silencio a 100 puntos/s, menos los bordes";
    EXPECT_EQ(all.silentNotExact, 0) << "en el silencio freqHz = 0 y confidence = 0 EXACTOS";
}

// ---------------------------------------------------------------------------
// AC-043.2 — la ventana del preset de voz es LA MAS CORTA de {40, 30} ms que sostiene
// AC-043.1 tambien en el tramo de 65 Hz. Los dos resultados se imprimen.
//
// Que bug atrapa: bajar el preset a 30 ms "porque es mas fino" sin que sostenga el
// grave (con la regla τmax ≤ W/2 el MPM no baja de 66,7 Hz y 65 Hz sale clavado en ese
// techo: medido 3,4 % contra 0,12 % con 40 ms); o dejarlo en 40 cuando 30 SI sostiene
// (entonces 40 no es "la mas corta").
// ---------------------------------------------------------------------------
TEST_F(TrackAnalysisGlide, VoiceWindowIsTheShortestThatHoldsAtSixtyFiveHertz) {
    const wav::WavData d = wav::readWav(testdata("glide-voz.wav").c_str());
    ASSERT_EQ(d.numFrames, 216000);
    const std::vector<float> mono = monoOf(d);
    const int from = truth.boundaries[3], to = truth.boundaries[4];   // el tramo de 65 Hz

    auto holds = [&](int W, GlideMetrics& out) {
        PitchSeries s;
        s.resize(216000 / hopFrames + 2);
        s.count = wma::track_analysis::pitchSeriesOverMono(
            mono.data(), 216000, kSR, 0, hopFrames, W, s.frames.data(), s.hz.data(),
            s.conf.data(), static_cast<int>(s.frames.size()));
        out = GlideMetrics::measure(s, truth, W / 2, from, to);
        return out.tonal > 0 && out.maxErrPct < kCeilingErrPct && out.gaps == 0 &&
               out.octaves == 0;
    };

    const int w40 = wma::track_analysis::hopFramesForMs(40.0, kSR);
    const int w30 = wma::track_analysis::hopFramesForMs(30.0, kSR);
    GlideMetrics m40, m30;
    const bool holds40 = holds(w40, m40);
    const bool holds30 = holds(w30, m30);
    m40.print("65 Hz con W = 40 ms");
    m30.print("65 Hz con W = 30 ms");
    std::printf("[AC-043.2] sostiene AC-043.1 en 65 Hz: 40 ms = %s, 30 ms = %s\n",
                holds40 ? "SI" : "NO", holds30 ? "SI" : "NO");

    ASSERT_TRUE(holds40 || holds30) << "ninguna de las dos sostiene el grave: hallazgo";
    const int shortest = holds30 ? w30 : w40;
    EXPECT_EQ(windowFrames, shortest)
        << "el preset de voz tiene que ser la mas corta de {40, 30} ms que sostiene 65 Hz";
}

// ---------------------------------------------------------------------------
// AC-043.3 — el eje: frame al CENTRO, primer punto en loopStart + W/2, ultimo
// ≤ loopEnd − W/2, hop EXACTO, ventanas enteras dentro de la region.
//
// Que bug atrapa: M2 (frame al inicio: el primero cae en loopStart); M3 (relleno con
// ceros fuera de la region: aparece un punto antes de loopStart + W/2); un hop "minimo"
// que redondee 336 a otra cosa; envolver al dar la vuelta (un punto pasado el final).
// ---------------------------------------------------------------------------
TEST_F(TrackAnalysisGlide, PitchAxisIsCenteredHopExactAndInsideTheRegion) {
    const int loopStart = 10000, loopEnd = 100000;
    looper.setTrackLoopRegion(0, loopStart, loopEnd);
    ASSERT_EQ(looper.getTrackLoopStart(0), loopStart);
    ASSERT_EQ(looper.getTrackLoopEnd(0), loopEnd);

    const int hop = wma::track_analysis::hopFramesForMs(7.0, kSR);
    ASSERT_EQ(hop, 336) << "round(7 ms · 48 kHz) exacto, sin minimo";

    const PitchSeries s = analyze(0, hop);
    const int region = loopEnd - loopStart;
    ASSERT_EQ(s.count, wma::track_analysis::pitchPointCount(region, windowFrames, hop));
    ASSERT_GT(s.count, 100);

    EXPECT_EQ(s.frames[0], loopStart + windowFrames / 2) << "primer punto en loopStart + W/2";
    EXPECT_LE(s.frames[static_cast<size_t>(s.count - 1)], loopEnd - windowFrames / 2)
        << "ultimo punto ≤ loopEnd − W/2: ventanas enteras, sin relleno ni envolver";
    EXPECT_GT(s.frames[static_cast<size_t>(s.count - 1)], loopEnd - windowFrames / 2 - hop)
        << "y no se deja una ventana entera sin analizar";
    for (int i = 1; i < s.count; ++i) {
        ASSERT_EQ(s.frames[static_cast<size_t>(i)] - s.frames[static_cast<size_t>(i - 1)], hop)
            << "hop exacto en el punto " << i;
    }
    // Y el buffer del llamador no se toca mas alla de lo escrito.
    EXPECT_EQ(s.frames[static_cast<size_t>(s.count)], -1);
    EXPECT_EQ(s.hz[static_cast<size_t>(s.count)], -1.0f);

    // hop < W es solapamiento, no un caso rechazado: 10 ms bajo una W de 40 ms.
    const PitchSeries dense = analyze(0, hopFrames);
    EXPECT_EQ(dense.count, wma::track_analysis::pitchPointCount(region, windowFrames, hopFrames));
    EXPECT_EQ(dense.frames[0], loopStart + windowFrames / 2);
}

// ---------------------------------------------------------------------------
// AC-043.4 — determinismo byte a byte, con el afinador OFFLINE corriendo entre medio
// como testigo de que hay otro consumidor de McLeodPitch. (Que ningun golden `.resp` del
// afinador cambie lo verifica `git diff --quiet -- analysis/tests/golden`, en el DoD.)
//
// Que bug atrapa: estado compartido con el afinador (un filtro o un ring que no se
// reinicia por llamada); un scratch que dependa de la llamada anterior.
// ---------------------------------------------------------------------------
TEST_F(TrackAnalysisGlide, SeriesIsByteIdenticalAcrossRunsWithTheTunerInBetween) {
    const PitchSeries a = analyze(0, hopFrames);
    ASSERT_GT(a.count, 0);

    const wav::WavData d = wav::readWav(testdata("glide-voz.wav").c_str());
    float snap[wma::analysis::kSnapshotValueCount];
    ASSERT_TRUE(wma::analysis::analyzeBuffer(d.buffer.data(), d.numFrames, kSR, 0.0, snap));
    ASSERT_GT(snap[wma::analysis::kSnapFramesAnalyzed], 0.0f) << "el afinador corrio de verdad";

    const PitchSeries b = analyze(0, hopFrames);
    ASSERT_EQ(a.count, b.count);
    const size_t n = static_cast<size_t>(a.count);
    EXPECT_EQ(0, std::memcmp(a.frames.data(), b.frames.data(), n * sizeof(int)));
    EXPECT_EQ(0, std::memcmp(a.hz.data(), b.hz.data(), n * sizeof(float)));
    EXPECT_EQ(0, std::memcmp(a.conf.data(), b.conf.data(), n * sizeof(float)));
}

// ---------------------------------------------------------------------------
// AC-043.5 — la envolvente contra los onsets de detectOnsets (audiograma-prueba.wav).
//
// LO QUE SE AFIRMA, Y POR QUE NO ES "maximo local" AL PIE DE LA LETRA. La carta y el AC
// dicen "el bin de cada onset ± 1 es un maximo local por encima de −35 dB". Medido sobre
// el fixture (RMS por 10 ms, la misma definicion del MANIFEST): los golpes son MESETAS
// planas con ±0,4 dB de rizado (pares −28 → −32 con el escalon a 200 ms; impares −30), y
// en una meseta el bin del onset NO es el maximo: el flanco de subida (bin 51, −30,5)
// queda debajo del tercer bin (53, −29,9), asi que el "maximo local" literal es rojo con
// la envolvente CORRECTA — es el observable equivocado para un golpe sostenido, la misma
// trampa que el MANIFEST ya anota ("8 maximos locales dan 12"). Lo que el AC quiere decir
// —que el golpe aparece en la envolvente donde detectOnsets lo ubica— se afirma como
// FLANCO: en onset ± 1 bin hay un bin ≥ −35 dB cuyo antecesor a dos bins esta < −45 dB.
// Los dos numeros del AC (−35, −45) y la ventana de 350 ms quedan iguales. Anotado en
// Notas de la etapa para la revision del lider.
//
// 🔴 Y "± 1 bin" es "± 2": detectOnsets ubica el golpe hasta 768 frames ANTES del
// flanco (medido: 416, 576, 448, 620 frames en este fixture). No es ruido, es el
// instrumento: suaviza la energia ±2 ventanas de 256 frames antes del flujo, asi que
// el escalon aparece 2 ventanas antes, mas la cuantizacion de la ventana = 3 × 256 =
// 768 frames = 1,6 bins de 480. La tolerancia se DERIVA de ahi, no se elige.
//
// 🔴 detectOnsets ve 7 golpes, no 8: el flujo de energia arranca en la ventana 1 y el
// golpe en el frame 0 no tiene "antes" contra el que subir. Es una propiedad del
// detector de onsets (F3.x), no de la envolvente, y se fija aca como trinquete: si
// alguna vez ve 8, este test lo dice. Los 8 golpes de la grilla del MANIFEST se
// verifican igual, con el del frame 0 sin antecesor.
//
// Que bug atrapa: M4 (pico en vez de RMS: los niveles de meseta suben ~3 dB y el escalon
// −28 → −32 deja de estar donde el MANIFEST lo midio); M5 (bin parcial incluido: el
// conteo da uno de mas con una tasa que no divide a la region); un origen que no sea
// loopStart; un hop que no sea round(sr / bps); un golpe que no llegue a −35 o un piso
// que supere −45 fuera de la ventana.
// ---------------------------------------------------------------------------
TEST(TrackAnalysisEnvelope, BinsRiseAtTheOnsetsAndStayQuietBetweenThem) {
    AudioLooper looper;
    looper.setSampleRate(kSR);
    ASSERT_TRUE(looper.importTrack(0, testdata("audiograma-prueba.wav").c_str(), kSR));
    ASSERT_EQ(looper.getTrackLengthFrames(0), 192000);

    int onsets[64];
    const int n = looper.detectTrackOnsets(0, onsets, 64, 256, 1.0f);
    std::printf("[AC-043.5] detectOnsets: %d onsets:", n);
    for (int i = 0; i < n; ++i) std::printf(" %d", onsets[i]);
    std::printf("\n");
    ASSERT_EQ(n, 7) << "detectOnsets es ciego al golpe del frame 0 (ver el encabezado del test)";

    const int hop = wma::track_analysis::hopFramesForBinsPerSecond(100.0, kSR);
    ASSERT_EQ(hop, 480);
    std::vector<float> bins(192000 / hop + 2, -1.0f);
    int firstFrame = -1;
    const int count = looper.getTrackLevelEnvelope(0, hop, bins.data(),
                                                   static_cast<int>(bins.size()), &firstFrame);
    ASSERT_EQ(count, 400) << "floor(192000 / 480), sin bin parcial";
    EXPECT_EQ(firstFrame, 0) << "firstFrame = loopStart";
    EXPECT_EQ(bins[400], -1.0f) << "no se escribe mas alla del conteo";

    auto db = [&](int k) {
        return 20.0 * std::log10(std::max(1e-9, static_cast<double>(bins[static_cast<size_t>(k)])));
    };
    for (int k = 0; k < count; ++k) {
        ASSERT_GE(bins[static_cast<size_t>(k)], 0.0f);
        ASSERT_LE(bins[static_cast<size_t>(k)], 1.0f);
    }

    // Los 8 golpes de la grilla del MANIFEST (0,5 s = 24000 frames; medido aca, los
    // flancos reales corren hasta ~300 frames tarde), y cada onset detectado cae a
    // ± 2 bins de uno de ellos.
    constexpr int kGridFrames = 24000;
    for (int i = 0; i < n; ++i) {
        const int nearest = static_cast<int>(std::lround(static_cast<double>(onsets[i]) / kGridFrames));
        EXPECT_LE(std::abs(onsets[i] - nearest * kGridFrames), 2 * hop)
            << "onset " << i << " (frame " << onsets[i] << ") a mas de dos bins de la grilla";
    }

    // Flanco en onset ± kOnsetToleranceBins: un bin ≥ −35 dB cuyo antecesor a dos bins
    // esta < −45 dB. La tolerancia sale de detectOnsets (ver el encabezado): 3 ventanas
    // de 256 frames sobre bins de 480.
    constexpr int kOnsetHop = 256, kOnsetSmoothing = 2;
    const int kOnsetToleranceBins =
        (kOnsetHop * (1 + kOnsetSmoothing) + hop - 1) / hop;   // ceil(768 / 480) = 2
    ASSERT_EQ(kOnsetToleranceBins, 2);
    auto risesAt = [&](int onsetFrame) {
        const int b = onsetFrame / hop;
        for (int k = std::max(0, b - kOnsetToleranceBins);
             k <= std::min(count - 1, b + kOnsetToleranceBins); ++k) {
            if (db(k) < -35.0) continue;
            if (k < 2) return true;               // el golpe del frame 0 no tiene "antes"
            if (db(k - 2) < -45.0) return true;
        }
        return false;
    };
    for (int i = 0; i < n; ++i) {
        const int b = onsets[i] / hop;
        std::printf("[AC-043.5] onset %d en frame %d -> bin %d: %.1f / %.1f / %.1f / %.1f / %.1f dB,"
                    " flanco %s\n", i, onsets[i], b, db(std::max(0, b - 2)), db(std::max(0, b - 1)),
                    db(b), db(std::min(count - 1, b + 1)), db(std::min(count - 1, b + 2)),
                    risesAt(onsets[i]) ? "SI" : "NO");
        EXPECT_TRUE(risesAt(onsets[i])) << "onset " << i << " (bin " << b << ") sin flanco > -35 dB";
    }
    EXPECT_TRUE(risesAt(0)) << "el golpe del frame 0, que detectOnsets no ve";

    // Y ningun bin supera −45 dB fuera de [golpe, golpe + 350 ms), con los 8 de la grilla.
    const int holdBins = static_cast<int>(std::lround(0.350 * kSR / hop));
    for (int k = 0; k < count; ++k) {
        bool inHold = false;
        for (int g = 0; g < 8; ++g) {
            const int b = g * kGridFrames / hop;
            if (k >= b && k < b + holdBins) { inHold = true; break; }
        }
        if (inHold) continue;
        EXPECT_LT(db(k), -45.0) << "bin " << k << " fuera de todo [golpe, golpe + 350 ms)";
    }

    // El NIVEL, en absoluto, contra lo que el MANIFEST midio con esta misma definicion
    // (RMS por 10 ms, mono = (L+R)/2): pares −28 dB hasta los 200 ms y −32 despues;
    // impares −30. Es lo que separa RMS de pico (M4): un pico de senoide sube ~3 dB.
    auto plateau = [&](int fromBin, int toBin) {
        double sum = 0.0;
        for (int k = fromBin; k < toBin; ++k) sum += db(k);
        return sum / (toBin - fromBin);
    };
    for (int g = 0; g < 8; g += 2) {
        const int b = g * kGridFrames / hop;
        EXPECT_NEAR(plateau(b + 5, b + 15), -28.0, 1.0) << "par " << g << " antes del escalon";
        EXPECT_NEAR(plateau(b + 22, b + 29), -32.0, 1.0) << "par " << g << " despues del escalon";
    }
    for (int g = 1; g < 8; g += 2) {
        const int b = g * kGridFrames / hop;
        EXPECT_NEAR(plateau(b + 3, b + 13), -30.0, 1.0) << "impar " << g;
    }

    // M5: con una tasa que no divide a la region, la cola menor que un hop NO tiene bin.
    const int hop97 = wma::track_analysis::hopFramesForBinsPerSecond(97.0, kSR);
    ASSERT_EQ(hop97, 495) << "round(48000 / 97)";
    std::vector<float> bins97(192000 / hop97 + 2, -1.0f);
    int first97 = -1;
    EXPECT_EQ(looper.getTrackLevelEnvelope(0, hop97, bins97.data(),
                                           static_cast<int>(bins97.size()), &first97),
              192000 / 495) << "floor(192000 / 495) = 387; la cola de 435 frames se descarta";
}

// ---------------------------------------------------------------------------
// AC-043.6 — pista inactiva o sin contenido ⇒ 0 elementos, en las dos lecturas.
// Que bug atrapa: publicar una serie de una pista que no existe (o de la que se esta
// grabando), o tocar el buffer de salida cuando no hay nada.
// ---------------------------------------------------------------------------
TEST(TrackAnalysisEmpty, InactiveTrackYieldsZeroElementsInBothReadings) {
    AudioLooper looper;
    looper.setSampleRate(kSR);
    int frames[4] = {-1, -1, -1, -1};
    float hz[4] = {-1, -1, -1, -1}, conf[4] = {-1, -1, -1, -1}, bins[4] = {-1, -1, -1, -1};
    int first = -7;

    EXPECT_EQ(looper.analyzeTrackPitch(3, 480, frames, hz, conf, 4), 0);
    EXPECT_EQ(looper.getTrackLevelEnvelope(3, 480, bins, 4, &first), 0);
    EXPECT_EQ(frames[0], -1);
    EXPECT_EQ(bins[0], -1.0f);
    EXPECT_EQ(first, -7) << "outFirstFrame se escribe SOLO con > 0 bins";

    // Una pista preparada pero sin contenido (longitud 0) tampoco tiene serie.
    ASSERT_TRUE(looper.prepareTrack(2, 48000, kSR));
    EXPECT_EQ(looper.analyzeTrackPitch(2, 480, frames, hz, conf, 4), 0);
    EXPECT_EQ(looper.getTrackLevelEnvelope(2, 480, bins, 4, &first), 0);

    // Indices y argumentos invalidos: 0, nunca negativo ni una escritura.
    EXPECT_EQ(looper.analyzeTrackPitch(-1, 480, frames, hz, conf, 4), 0);
    EXPECT_EQ(looper.analyzeTrackPitch(0, 0, frames, hz, conf, 4), 0);
    EXPECT_EQ(looper.analyzeTrackPitch(0, 480, nullptr, hz, conf, 4), 0);
    EXPECT_EQ(looper.getTrackLevelEnvelope(0, 480, bins, 0, &first), 0);
}

// ---------------------------------------------------------------------------
// AC-043.8 — el costo de 30 s a 48 kHz con hop 10 ms, IMPRESO (host, −O0), no afirmado.
// ---------------------------------------------------------------------------
TEST(TrackAnalysisCost, ThirtySecondsAtTenMillisecondHopIsPrinted) {
    constexpr int kSeconds = 30;
    constexpr int kFrames = kSeconds * kSR;
    // Una "voz" sintetica: f0 + 6 armonicos a −6 dB/oct con un vibrato lento, para que
    // el detector trabaje de verdad (una senoide pura elige el primer pico sin esfuerzo).
    std::vector<float> stereo(static_cast<size_t>(kFrames) * 2, 0.0f);
    double phase = 0.0;
    for (int i = 0; i < kFrames; ++i) {
        const double t = static_cast<double>(i) / kSR;
        const double f0 = 180.0 * std::pow(2.0, 0.25 * std::sin(2.0 * M_PI * 0.2 * t));
        phase += 2.0 * M_PI * f0 / kSR;
        double v = 0.0;
        for (int h = 1; h <= 7; ++h) v += std::sin(h * phase) / h;
        const float s = static_cast<float>(0.1 * v);
        stereo[static_cast<size_t>(i) * 2] = s;
        stereo[static_cast<size_t>(i) * 2 + 1] = s;
    }
    wma_test::ScopedFixture wavPath("req043_cost_30s.wav");
    ASSERT_TRUE(wav::writeWav(wavPath.c_str(), stereo.data(), kFrames, kSR));

    AudioLooper looper;
    looper.setSampleRate(kSR);
    ASSERT_TRUE(looper.importTrack(0, wavPath.c_str(), kSR));
    ASSERT_EQ(looper.getTrackLengthFrames(0), kFrames);

    const int hop = wma::track_analysis::hopFramesForMs(kHopMs, kSR);
    std::vector<int> frames(kFrames / hop + 2);
    std::vector<float> hz(frames.size()), conf(frames.size());
    const auto t0 = std::chrono::steady_clock::now();
    const int points = looper.analyzeTrackPitch(0, hop, frames.data(), hz.data(), conf.data(),
                                                static_cast<int>(frames.size()));
    const auto t1 = std::chrono::steady_clock::now();
    std::vector<float> bins(kFrames / hop + 2);
    int first = 0;
    const int nb = looper.getTrackLevelEnvelope(0, hop, bins.data(),
                                                static_cast<int>(bins.size()), &first);
    const auto t2 = std::chrono::steady_clock::now();

    const double pitchMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double envMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    int voiced = 0;
    for (int i = 0; i < points; ++i) if (hz[static_cast<size_t>(i)] > 0.0f) ++voiced;
    std::printf("[AC-043.8] costo en host (%s): pitch %d puntos (%d con voz) en %.1f ms = "
                "%.4fx tiempo real; envolvente %d bins en %.2f ms = %.5fx tiempo real\n",
#ifdef NDEBUG
                "NDEBUG",
#else
                "-O0/Debug",
#endif
                points, voiced, pitchMs, pitchMs / (kSeconds * 1000.0), nb, envMs,
                envMs / (kSeconds * 1000.0));
    EXPECT_EQ(points, wma::track_analysis::pitchPointCount(kFrames, 1920, hop));
    EXPECT_EQ(nb, kFrames / hop);
    EXPECT_GT(voiced, points * 9 / 10) << "la voz sintetica tiene pitch casi en todo el tramo";
}
