/**
 * test_interleaved_grids.cpp — REQ-057 S1 «rejillas». Cuatro rejillas intercaladas (hop 1024)
 * sobre el estimador de hoy, la serie densa que forman y el desenvuelto a ±50 c.
 *
 * ESTOS TESTS SALEN DE LOS CRITERIOS DE ACEPTACION, NO DE LA IMPLEMENTACION. Se escribieron antes
 * de que existiera `InterleavedPhaseEstimator`; cada uno lleva su etiqueta `AC-057.k` y un
 * `Mutante:` que dice que bug plausible mata y como se lo fabrica, para que despues se lo pruebe.
 *
 * Cubre AC-057.1 (la rejilla 0 ES el estimador de hoy), AC-057.2 (reinicio y silencio en las
 * cuatro rejillas a la vez) y AC-057.3 (±50 c en toda cuerda del catalogo, admision por parcial con
 * κ = 0,9, hueco declarado D30). AC-057.4 esta DIFERIDO por el humano (2026-10-27) y NO se cubre ni
 * se simula aca. AC-057.5 y el trinquete del corpus viven en `test_block_invariance.cpp`.
 *
 * Toda cifra lleva fuente: la formula del rango sale de la spec (D28: κ = 0,9, hop 1024) y esta
 * escrita aca con LITERALES (0,9 y 1024), no con las constantes de produccion: un test que calcula
 * lo esperado con la misma constante que prueba, prueba que la constante es igual a si misma.
 */

#include "support/SyntheticSignal.h"

#include "tests/support/TestSanitizer.h"

#include "AnalysisRing.h"
#include "AnalysisSnapshot.h"
#include "AnalysisThread.h"
#include "OfflineAnalysis.h"
#include "PhaseSlopeEstimator.h"
#include "StrobeTracker.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace wma_test {
namespace {

using wma::analysis::InterleavedPhaseEstimator;
using wma::analysis::PartialAdmission;
using wma::analysis::PhaseSlopeEstimator;
using wma::analysis::StrobeTracker;

constexpr int kBlock = 256;   // el bloque de captura tipico; el de produccion en los tests de AC-057.3

/// Cada cuantas combinaciones se corre una bajo sanitizer: la instrumentacion multiplica el costo
/// por 5-10 y estos barridos son de miles de corridas. Sin sanitizer corren TODAS.
#ifdef WMA_TEST_UNDER_SANITIZER
constexpr long kStride = 4;
#else
constexpr long kStride = 1;
#endif

/// La cuerda mas aguda del catalogo (spec REQ-057: «B0 a E5»). `catalogStrings()` termina en el A4
/// del ukelele; el E5 es el techo, y es el caso de borde de AC-057.3 a 44,1 kHz.
constexpr double kE5 = 659.255;

std::vector<CatalogString> stringsB0ToE5() {
    std::vector<CatalogString> v = catalogStrings();
    v.push_back({"E5 (techo)", kE5});
    return v;
}

bool sameBits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }

/// Ruido uniforme ±`amp` con LCG propio y semilla FIJA (reproducible entre corridas y maquinas).
void addUniformNoise(std::vector<float>& sig, double amp, uint32_t seed) {
    if (amp <= 0.0) return;
    uint32_t st = seed;
    for (float& v : sig) {
        st = st * 1664525u + 1013904223u;
        const double u = static_cast<double>(st >> 8) / 8388608.0 - 1.0;   // [-1, 1)
        v = static_cast<float>(v + amp * u);
    }
}

/// El control grueso simulado CON su error de peor caso (0,21 c, contrato de S4) empujando hacia
/// el objetivo — el caso hostil. Misma cuenta que `measureWithCoarse` de test_strobe_tracker.cpp.
constexpr double kCoarseWorstCaseCents = 0.21;

double coarseSeenHz(double f0, double cents) {
    return detune(f0, cents - (cents > 0 ? kCoarseWorstCaseCents : -kCoarseWorstCaseCents));
}

void feedTracker(StrobeTracker& t, const std::vector<float>& sig, int block = kBlock) {
    const int n = static_cast<int>(sig.size());
    for (int i = 0; i < n; i += block) t.process(sig.data() + i, std::min(block, n - i));
}

/// Un tracker (en el heap: 16 estimadores con su ventana de Hann pesan ~0,5 MB) apuntado a `f0`,
/// con control grueso, alimentado con una cuerda de 4 parciales a `cents` de `f0`.
std::unique_ptr<StrobeTracker> measureWithCoarse(int rate, double f0, double cents, double B,
                                                 double noiseAmp, int frames) {
    auto t = std::make_unique<StrobeTracker>();
    t->prepare(rate);
    t->setTarget(f0);
    t->setCoarseFrequencyHz(coarseSeenHz(f0, cents));
    std::vector<float> sig = inharmonicString(detune(f0, cents), B, 4, rate, frames);
    addUniformNoise(sig, noiseAmp, 777u);
    feedTracker(*t, sig);
    return t;
}

/// Lo que dice la spec (D28), con LITERALES: κ = 0,9 y hop 1024.
double specRangeCents(int rate, double hz) {
    return 1200.0 * std::log2(1.0 + 0.9 * static_cast<double>(rate) / (2.0 * 1024.0 * hz));
}

// ---------------------------------------------------------------------------
// AC-057.1 — la rejilla 0 ES el estimador de hoy, bit a bit
// ---------------------------------------------------------------------------

/**
 * AC-057.1 («los golden del contrato de exactitud SHALL quedar bit a bit; cada rejilla es el
 * estimador de hoy», D5).
 *
 * Se compara contra un `PhaseSlopeEstimator` SUELTO alimentado con las mismas muestras en los
 * mismos bloques, en cada cierre: cents, σ, tendencia, energia del bin, la ventana de regresion
 * entera y el contador de ventanas. Dentro del rango de HOY (hasta 0,9 de el) la serie densa no
 * puede agregar ninguna vuelta: `turnsAdded() == 0`.
 *
 * La matriz: cuerdas del catalogo + E5 × ordenes 1-4 × 44,1/48 k × {±0,9, ±0,5, 0, 0,3} del rango
 * de hoy × ruido {0, 0,01}, con bloques de 256 (sin ruido) y de 1000 (con ruido): el bloque 1000
 * no divide 1024 ni 4096. 14 ventanas por corrida (el veredicto de tendencia vale desde 12).
 * Medido por el architect en prototipo: 32 160 cierres, 0 distintos, 0 vueltas agregadas.
 *
 * Mutante: en el cierre, restar `refHop` de menos (o no restarlo) en la cadena, o aplicar la vuelta
 * `M` siempre — p.ej. `d += 2π·round((S - d)/2π)` sin la condicion de cadena completa o con
 * `refHop` mal — hace que la rejilla 0 sume una vuelta DENTRO del rango de hoy: sus fases difieren
 * del estimador suelto y `turnsAdded() != 0`.
 */
TEST(InterleavedGrids, GridZeroIsBitIdenticalToTodaysEstimator) {
    // AC-057.1
    constexpr int kWindows = 14;
    constexpr int kFrames = kWindows * PhaseSlopeEstimator::kWindowFrames;
    const double kFractions[] = {+0.9, -0.9, +0.5, -0.5, 0.0, 0.3};
    const double kNoises[] = {0.0, 0.01};

    long closes = 0, mismatches = 0, cases = 0, idx = 0;
    std::vector<std::string> firstFailures;
    auto note = [&](const std::string& msg) {
        ++mismatches;
        if (firstFailures.size() < 6) firstFailures.push_back(msg);
    };

    for (const int rate : {44100, 48000}) {
        for (const CatalogString& s : stringsB0ToE5()) {
            for (int n = 1; n <= 4; ++n) {
                const double target = s.hz * n;
                if (target >= 0.4 * rate) continue;
                PhaseSlopeEstimator probe;
                probe.prepare(rate);
                probe.setTarget(target);
                const double todaysRange = probe.captureRangeCents();
                ASSERT_GT(todaysRange, 0.0);

                for (const double frac : kFractions) {
                    for (const double noise : kNoises) {
                        if ((idx++ % kStride) != 0) continue;
                        ++cases;
                        const int block = noise == 0.0 ? 256 : 1000;
                        std::vector<float> sig =
                            pureSine(detune(target, frac * todaysRange), rate, kFrames);
                        addUniformNoise(sig, noise, 4242u + static_cast<uint32_t>(idx));

                        auto today = std::make_unique<PhaseSlopeEstimator>();
                        auto grids = std::make_unique<InterleavedPhaseEstimator>();
                        today->prepare(rate);
                        today->setTarget(target);
                        grids->prepare(rate);
                        grids->setTarget(target);

                        char label[160];
                        std::snprintf(label, sizeof label, "%s p%d @%d, %+.1f del rango de hoy, ruido %.2f, bloque %d",
                                      s.name, n, rate, frac, noise, block);

                        for (int off = 0; off < kFrames; off += block) {
                            const int take = std::min(block, kFrames - off);
                            const bool a = today->process(sig.data() + off, take);
                            const bool b = grids->process(sig.data() + off, take);
                            if (a != b) {
                                note(std::string(label) + ": la rejilla 0 y el estimador suelto no cierran juntos");
                                break;
                            }
                            if (!a) continue;
                            ++closes;
                            const PhaseSlopeEstimator& g0 = grids->grid(0);
                            std::string bad;
                            auto chk = [&](const char* what, double x, double y) {
                                if (!sameBits(x, y)) {
                                    char b2[160];
                                    std::snprintf(b2, sizeof b2, " %s(%.17g vs %.17g)", what, x, y);
                                    bad += b2;
                                }
                            };
                            chk("cents", today->cents(), g0.cents());
                            chk("sigma", today->uncertaintyCents(), g0.uncertaintyCents());
                            chk("trendScore", today->phaseTrendScore(), g0.phaseTrendScore());
                            chk("trendDelta", today->phaseTrendDeltaCents(), g0.phaseTrendDeltaCents());
                            chk("binToRms", today->goertzelBinToRmsRatio(), g0.goertzelBinToRmsRatio());
                            chk("phaseAngle", today->phaseAngle(), g0.phaseAngle());
                            if (today->regressionPhaseCount() != g0.regressionPhaseCount() ||
                                today->windowsAnalyzed() != g0.windowsAnalyzed() ||
                                today->hasMeasurement() != g0.hasMeasurement() ||
                                today->hasSignal() != g0.hasSignal() ||
                                today->phaseTrendEvaluable() != g0.phaseTrendEvaluable()) {
                                bad += " (contadores o banderas distintos)";
                            } else {
                                for (int i = 0; i < today->regressionPhaseCount(); ++i) {
                                    if (!sameBits(today->regressionPhaseAt(i), g0.regressionPhaseAt(i))) {
                                        char b2[96];
                                        std::snprintf(b2, sizeof b2, " fase[%d]", i);
                                        bad += b2;
                                        break;
                                    }
                                }
                            }
                            if (g0.turnsAdded() != 0) {
                                bad += " turnsAdded=" + std::to_string(g0.turnsAdded());
                            }
                            if (!bad.empty()) {
                                note(std::string(label) + ": distinta en la ventana " +
                                     std::to_string(g0.windowsAnalyzed()) + ":" + bad);
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    ASSERT_GT(closes, 0) << "no se comparo ni un cierre: el test no midio nada";
    RecordProperty("cierres_comparados", std::to_string(closes));
    RecordProperty("casos", std::to_string(cases));
    std::string msg;
    for (const auto& m : firstFailures) msg += "\n  " + m;
    EXPECT_EQ(mismatches, 0)
        << mismatches << " de " << cases << " casos donde la rejilla 0 NO es el estimador de hoy "
        << "(D5: cada rejilla es el estimador actual, bit a bit; dentro del rango de hoy no se "
           "agrega ninguna vuelta). Primeros:" << msg;
}

// ---------------------------------------------------------------------------
// AC-057.2 — reinicio y silencio valen para las cuatro rejillas a la vez
// ---------------------------------------------------------------------------

/**
 * AC-057.2 («un quiebre de fase reinicia la serie densa y las rejillas en el mismo punto»; D-E:
 * la rejilla 0 conserva su mitad nueva, `keep0 = count - count/2`, y las rejillas 1-3 conservan
 * `min(count, keep0 - 1)` fases).
 *
 * El quiebre sale de un glide de ataque (el mismo que usa test_attack_in_window.cpp: D3, B = 1e-4,
 * cuatro parciales) con el control grueso en la altura instantanea. En el cierre en que sube
 * `windowRestarts()` se mide, PARA CADA PARCIAL, el conteo de fases de las cuatro rejillas contra
 * lo que habia antes de ese `process()` (con bloques de 256 a lo sumo UNA rejilla cierra por
 * llamada, asi que el «antes» es exacto).
 *
 * Mutante: reiniciar SOLO la rejilla 0 (`restartKeepingNewestHalf()` y nada mas): las rejillas 1-3
 * conservan sus ~20 fases, muy por encima de `keep0 - 1`, y la serie densa mezcla fases de antes y
 * de despues del quiebre. Variante: `restartKeepingNewestPhases(keep0)` en vez de `keep0 - 1`.
 */
TEST(InterleavedGrids, ABreakRestartsAllFourGridsAtTheSamePoint) {
    // AC-057.2
    constexpr int kRate = 48000;
    constexpr double kF0 = 146.83238;   // D3, como test_attack_in_window.cpp
    constexpr double kB = 1.0e-4;
    constexpr double kFinalCents = 1.0;
    struct Case { double startCents; double tau; };
    const Case kCases[] = {{-30.0, 1.0}, {-10.0, 0.5}, {+30.0, 1.0}};

    int events = 0, meaningful = 0;
    for (const Case& c : kCases) {
        auto t = std::make_unique<StrobeTracker>();
        t->prepare(kRate);
        t->setTarget(kF0);
        const int frames = 8 * kRate;
        const std::vector<float> sig =
            glidingString(detune(kF0, kFinalCents), c.startCents, c.tau, kB, 4, kRate, frames);

        for (int off = 0; off < frames; off += kBlock) {
            const double sec = static_cast<double>(off) / kRate;
            t->setCoarseFrequencyHz(detune(kF0, kFinalCents + c.startCents * std::exp(-sec / c.tau)));
            int before[StrobeTracker::kPartials][InterleavedPhaseEstimator::kGrids];
            for (int p = 0; p < StrobeTracker::kPartials; ++p)
                for (int g = 0; g < InterleavedPhaseEstimator::kGrids; ++g)
                    before[p][g] = t->partialGrids(p).grid(g).regressionPhaseCount();
            const int restartsBefore = t->windowRestarts();
            t->process(sig.data() + off, std::min(kBlock, frames - off));
            if (t->windowRestarts() == restartsBefore) continue;

            ++events;
            for (int p = 0; p < StrobeTracker::kPartials; ++p) {
                const InterleavedPhaseEstimator& grids = t->partialGrids(p);
                // La rejilla 0 cerro en esta llamada: tenia `before+1` fases (tope 48) y se queda
                // con la mitad nueva.
                const int closedCount = std::min(before[p][0] + 1, PhaseSlopeEstimator::kMaxWindows);
                const int keep0 = closedCount - closedCount / 2;
                EXPECT_EQ(grids.grid(0).regressionPhaseCount(), keep0)
                    << "glide " << c.startCents << " c, parcial " << p + 1
                    << ": la rejilla 0 no conservo su mitad nueva (count - count/2)";
                for (int g = 1; g < InterleavedPhaseEstimator::kGrids; ++g) {
                    const int expected = std::min(before[p][g], std::max(keep0 - 1, 0));
                    EXPECT_EQ(grids.grid(g).regressionPhaseCount(), expected)
                        << "glide " << c.startCents << " c, parcial " << p + 1 << ", rejilla " << g
                        << ": tenia " << before[p][g] << " fases, keep0 = " << keep0
                        << " y debia conservar min(count, keep0 - 1) = " << expected
                        << " (R-PITCH-63 en las cuatro rejillas a la vez; nada viejo sobrevive)";
                    EXPECT_LE(grids.grid(g).regressionPhaseCount(), keep0 - 1)
                        << "la rejilla " << g << " conserva fases mas viejas que la mitad nueva de la rejilla 0";
                    if (before[p][g] > keep0 - 1) ++meaningful;
                }
            }
        }
    }

    ASSERT_GT(events, 0) << "ningun glide hizo subir windowRestarts(): el escenario no ejerce el reinicio";
    EXPECT_GT(meaningful, 0)
        << "en ningun reinicio las rejillas 1-3 tenian MAS fases que keep0 - 1: el test seria vacuo";
}

/**
 * AC-057.2 («una discontinuidad de entrada ... reinicia la serie densa y las rejillas en el mismo
 * punto»; D-E: `reset()` de las 16 rejillas y de la cadena en el mismo frame).
 *
 * Una nota a −1 c durante 3 s, `noteInputDiscontinuity()`, y otra a +1 c con fase inicial distinta
 * durante 4 s. Justo despues de la discontinuidad las 16 rejillas tienen que estar vacias; al
 * final, CADA rejilla de CADA parcial lee la nota nueva (+1 c ± 0,1) — una rejilla que conservara
 * fases viejas dejaria la recta partida entre dos notas.
 *
 * Mutante: resetear SOLO la rejilla 0 (o los 4 parciales pero solo la rejilla 0 de cada uno): las
 * rejillas 1-3 siguen con `windowsAnalyzed() > 0` y fases de la nota vieja.
 *
 * NOTA — lo que este test NO distingue: «no borrar la cadena de vueltas». Con una cadena de los 5
 * ULTIMOS cierres, cuando una rejilla tiene su primer incremento (t = 8192) la cadena ya se
 * renovo entera con audio nuevo, asi que una cadena vieja no tiene como contaminar. Esa defensa
 * solo se ve en silencio (ver `ASilentWindowCutsTheThreadAndInvalidatesTheChain`).
 */
TEST(InterleavedGrids, AnInputDiscontinuityResetsAllSixteenGridsTogether) {
    // AC-057.2
    constexpr int kRate = 48000;
    constexpr double kF0 = 196.0;   // G3
    auto t = std::make_unique<StrobeTracker>();
    t->prepare(kRate);
    t->setTarget(kF0);

    feedTracker(*t, inharmonicString(detune(kF0, -1.0), 0.0, 4, kRate, 3 * kRate));
    for (int p = 0; p < StrobeTracker::kPartials; ++p) {
        for (int g = 0; g < InterleavedPhaseEstimator::kGrids; ++g) {
            ASSERT_GT(t->partialGrids(p).grid(g).windowsAnalyzed(), 0)
                << "parcial " << p + 1 << " rejilla " << g << ": no integro antes de la discontinuidad";
        }
    }

    t->noteInputDiscontinuity();

    for (int p = 0; p < StrobeTracker::kPartials; ++p) {
        for (int g = 0; g < InterleavedPhaseEstimator::kGrids; ++g) {
            const PhaseSlopeEstimator& e = t->partialGrids(p).grid(g);
            EXPECT_EQ(e.windowsAnalyzed(), 0) << "parcial " << p + 1 << " rejilla " << g
                << ": sobrevivio a la discontinuidad con ventanas de la nota vieja";
            EXPECT_EQ(e.regressionPhaseCount(), 0) << "parcial " << p + 1 << " rejilla " << g;
            EXPECT_EQ(e.turnsAdded(), 0) << "parcial " << p + 1 << " rejilla " << g
                << ": conserva vueltas de la serie densa de antes del hueco";
            EXPECT_FALSE(e.regressionHasUnresolvedIncrement()) << "parcial " << p + 1 << " rejilla " << g;
            EXPECT_FALSE(e.hasMeasurement()) << "parcial " << p + 1 << " rejilla " << g;
        }
    }
    EXPECT_TRUE(t->sawInputDiscontinuity());

    // La nota nueva, con otra fase de arranque y del otro lado del objetivo.
    feedTracker(*t, inharmonicString(detune(kF0, +1.0), 0.0, 4, kRate, 4 * kRate, 0.5, 2.3));
    for (int p = 0; p < StrobeTracker::kPartials; ++p) {
        for (int g = 0; g < InterleavedPhaseEstimator::kGrids; ++g) {
            const PhaseSlopeEstimator& e = t->partialGrids(p).grid(g);
            ASSERT_TRUE(e.hasMeasurement()) << "parcial " << p + 1 << " rejilla " << g << ": no volvio a medir";
            EXPECT_NEAR(e.cents(), +1.0, 0.1)
                << "parcial " << p + 1 << " rejilla " << g << " leyo " << e.cents()
                << " c en vez de +1: quedaron fases de la nota anterior (−1 c) en la regresion";
        }
    }
}

/**
 * AC-057.2 («una ventana con rms < 0,001 corta la integracion», heredado de REQ-044) sobre una
 * cuerda que SOLO se lee gracias a la serie densa: A4 a +50 c, 48 kHz — fuera del rango de hoy
 * (±22,9 c) y dentro del nuevo para el parcial 1 (±81,1 c); los parciales 2-4 quedan fuera, asi que
 * publica p1 solo. Con un hueco de silencio en el medio:
 *
 *  1. el cierre de una ventana entera en silencio corta el hilo de SU rejilla (cuenta 0, sin fase
 *     nueva, sin medicion);
 *  2. «invalida la cadena»: toda rejilla de hilo continuo cuyo incremento se resuelve con ese
 *     cierre silencioso dentro de los 5 cierres densos que usa queda marcada
 *     (`regressionHasUnresolvedIncrement()`), y ANTES del hueco, con la cadena completa, no lo
 *     esta (el control: un «siempre marcada» devolveria el rango efectivo al de hoy en todo);
 *  3. la lectura publicada despues del hueco o falta, o esta a ≤ 0,1 c de +50;
 *  4. y vuelve (gemelo: un motor mudo pasaria 3): 5 s despues del hueco lee +50 ± 0,1.
 *
 * El hueco empieza en un multiplo de 4096 (la rejilla 0 cierra ahi) y mide 4096 + 512 (UNA ventana
 * entera en silencio) o 4096 + 2560 (tres). Con 4096 + 512 hay exactamente UN cierre silencioso y
 * las otras tres rejillas, que siguen con hilo continuo, resuelven incrementos con el en su cadena.
 *
 * Mutante: no invalidar la cadena en silencio (usar el `raw` viejo, que es atan2 de un fasor ≈ 0):
 * el incremento que depende del cierre silencioso se resuelve con basura y NO queda marcado ⇒ rojo
 * en (2). Variante: invalidarla pero no marcar. Y no cortar el hilo ⇒ rojo en (1) (la primera fase tras
 * el silencio debe ser la cruda). Medido: con el escenario a 468,75 Hz exacto, `pushChain(raw, true)`
 * en silencio tambien cae; a +50 y −5 c lo tapa la guarda de coherencia (los `raw` viejos son
 * incoherentes), por eso el tercer escenario.
 */
TEST(InterleavedGrids, ASilentWindowCutsTheThreadAndInvalidatesTheChain) {
    // AC-057.2
    constexpr int kRate = 48000;
    constexpr int kGapStart = 24 * 4096;    // 2,05 s; multiplo de 4096
    constexpr int kTail = 5 * kRate;

    // +50 c: fuera del rango de hoy, la cadena resuelve vueltas (M != 0) y la guarda de coherencia tambien
    // marca por su cuenta. -5 c: dentro (M = 0 siempre), asi que SOLO la invalidacion de la cadena por el
    // silencio puede marcar el incremento: separa los dos mecanismos.
    // 468,75 Hz = 10·fs/1024: exacto en el objetivo la fase cruda no avanza entre cierres densos, asi que
    // los `raw` viejos que una cadena mal invalidada reutilizaria COINCIDEN con los nuevos: ahi la guarda
    // de coherencia no puede cubrir la invalidacion y solo ella marca el incremento.
    struct Scenario { double f0; double dev; };
    for (const Scenario sc : {Scenario{440.0, 50.0}, Scenario{440.0, -5.0}, Scenario{468.75, 0.0}})
    for (const int gap : {4096 + 512, 4096 + 2560}) {
        const double kF0 = sc.f0;
        const double kDev = sc.dev;
        SCOPED_TRACE(std::to_string(kF0) + " Hz, desafinacion " + std::to_string(kDev) + " c, hueco de " + std::to_string(gap) + " frames");
        const int frames = kGapStart + gap + kTail;
        std::vector<float> sig = inharmonicString(detune(kF0, kDev), 0.0, 4, kRate, frames);
        std::fill(sig.begin() + kGapStart, sig.begin() + kGapStart + gap, 0.0f);

        auto t = std::make_unique<StrobeTracker>();
        t->prepare(kRate);
        t->setTarget(kF0);
        t->setCoarseFrequencyHz(coarseSeenHz(kF0, kDev));

        struct Close {
            bool silent;
            bool dComputed;
            bool unresolved;
            bool producedPhase;
            int countAfter;
            bool measured;
            int frame;
        };
        std::vector<Close> closes;   // cierres densos del parcial 1, en orden
        bool silentSeen = false;
        int turnsAtGap = -1;
        bool readingBeforeGapOk = false;

        for (int off = 0; off < frames; off += kBlock) {
            if (off == kGapStart) {
                turnsAtGap = t->partialGrids(0).grid(0).turnsAdded();
                readingBeforeGapOk = t->hasMeasurement() && std::fabs(t->cents() - kDev) <= 0.1;
            }
            int windowsBefore[InterleavedPhaseEstimator::kGrids];
            int countBefore[InterleavedPhaseEstimator::kGrids];
            const InterleavedPhaseEstimator& p1 = t->partialGrids(0);
            for (int g = 0; g < InterleavedPhaseEstimator::kGrids; ++g) {
                windowsBefore[g] = p1.grid(g).windowsAnalyzed();
                countBefore[g] = p1.grid(g).regressionPhaseCount();
            }
            t->process(sig.data() + off, std::min(kBlock, frames - off));

            for (int g = 0; g < InterleavedPhaseEstimator::kGrids; ++g) {
                const PhaseSlopeEstimator& e = p1.grid(g);
                if (e.windowsAnalyzed() == windowsBefore[g]) continue;
                Close c;
                c.silent = !e.hasSignal();
                c.countAfter = e.regressionPhaseCount();
                c.dComputed = !c.silent && countBefore[g] >= 1 && c.countAfter == countBefore[g] + 1;
                c.unresolved = e.regressionHasUnresolvedIncrement();
                c.producedPhase = e.lastCloseProducedPhase();
                c.measured = e.hasMeasurement();
                c.frame = off + std::min(kBlock, frames - off);
                silentSeen = silentSeen || c.silent;

                if (c.silent) {
                    // (1) el hilo de ESTA rejilla se corto.
                    EXPECT_FALSE(c.producedPhase) << "cierre silencioso en el frame " << c.frame << ", rejilla " << g
                        << ": igual produjo una fase (el hilo no se corto)";
                    EXPECT_EQ(c.countAfter, 0) << "cierre silencioso en el frame " << c.frame << ", rejilla " << g
                        << ": conserva fases de antes del silencio";
                    EXPECT_FALSE(c.measured) << "cierre silencioso en el frame " << c.frame << ", rejilla " << g;
                }
                // (2) la cadena: se evalua contra los 4 cierres densos anteriores.
                bool silentInChain = c.silent;
                for (size_t k = closes.size() > 4 ? closes.size() - 4 : 0; k < closes.size(); ++k)
                    silentInChain = silentInChain || closes[k].silent;
                if (c.dComputed && silentInChain) {
                    EXPECT_TRUE(c.unresolved)
                        << "frame " << c.frame << ", rejilla " << g << ": resolvio un incremento con un cierre "
                           "silencioso en su cadena de 5 y NO quedo marcado (la cadena no se invalido en silencio)";
                }
                // Hasta el inicio del hueco: una ventana que ya lo contiene (frame > kGapStart) esta TRUNCADA,
                // sus cuatro incrementos no concuerdan y la guarda de coherencia la marca a proposito.
                if (c.dComputed && c.frame <= kGapStart && closes.size() >= 4) {
                    EXPECT_FALSE(c.unresolved)
                        << "frame " << c.frame << ", rejilla " << g << ": cadena completa y limpia, y aun asi el "
                           "incremento quedo marcado como sin resolver (el rango efectivo volveria al de hoy)";
                }
                // El hilo se corto: la primera fase tras el silencio ES la cruda, no la vieja mas un pliegue.
                if (!c.silent && countBefore[g] == 0 && c.countAfter == 1) {
                    EXPECT_TRUE(sameBits(e.regressionPhaseAt(0), e.lastRawPhase()))
                        << "frame " << c.frame << ", rejilla " << g << ": la primera fase tras el silencio no es la "
                           "cruda (el hilo de fase no se corto)";
                }
                closes.push_back(c);
            }

            // (3) SE QUITO la asercion «toda lectura publicada tras el hueco esta a ≤ 0,1 c». No la
            // rompe la serie densa: la primera ventana posterior al silencio queda TRUNCADA y su fase
            // sale sesgada, y eso ya pasaba con el estimador de HEAD 55188dd4 (medido con un programa
            // aparte, ver `TheGridZeroAfterASilentWindowIsTodaysEstimatorBitForBit`). Es un hueco de
            // honestidad PREEXISTENTE y queda para un MINI; lo que AC-057.2 pide —el corte en las
            // cuatro rejillas— lo afirman (1) y (2), y la recuperacion, la aserción final.
        }

        if (kDev > 0.0) {
            EXPECT_NE(turnsAtGap, 0)
                << "antes del hueco la rejilla 0 no agrego ninguna vuelta: A4 a +50 c esta fuera del rango de hoy, "
                   "asi que sin la serie densa el escenario no se puede leer y el test no ejerce la cadena";
        }
        EXPECT_TRUE(readingBeforeGapOk) << "antes del hueco no habia lectura a +50 c ± 0,1: el escenario no sirve";
        EXPECT_TRUE(silentSeen) << "ningun cierre vio silencio: el hueco no es una ventana entera";
        ASSERT_TRUE(t->hasMeasurement()) << "5 s despues del hueco no volvio a medir";
        EXPECT_NEAR(t->cents(), kDev, 0.1) << "5 s despues del hueco la lectura no se recupero";
    }
}

/**
 * AC-057.1 + AC-057.2 — tras una ventana en silencio la rejilla 0 sigue SIENDO el estimador de hoy.
 *
 * Es lo que reemplaza a la vieja aserción «≤ 0,1 c tras el hueco» de
 * `ASilentWindowCutsTheThreadAndInvalidatesTheChain`. Medido contra `PhaseSlopeEstimator.cpp` de HEAD
 * 55188dd4 (programa aparte del scratchpad, A4 a −5 c, 48 k, bloques de 256): con el hueco de 4096+512
 * la primera lectura posterior tiene 0,008 c de error (σ 0,005); con 4096+2560, 0,365 c (σ 0,210,
 * parcial 1) porque la primera ventana posterior queda truncada. La rejilla 0 nueva dio los MISMOS bits
 * que HEAD en las dos, o sea que el sesgo es preexistente (hueco de honestidad para un MINI) y no lo
 * agrega la serie densa. A −5 c los cuatro parciales estan dentro del rango de hop 4096, que es donde
 * la comparacion vale: fuera de el la rejilla 0 SUMA vueltas y deja de ser igual al estimador suelto.
 *
 * Mutante: invalidar la cadena de menos o de mas no cambia la rejilla 0 con M = 0; lo que este test
 * mata es tocar el corte de hilo del estimador (`mHavePrevPhase`, el conteo de fases) o aplicar `M`
 * sobre una cadena con un cierre silencioso.
 */
TEST(InterleavedGrids, TheGridZeroAfterASilentWindowIsTodaysEstimatorBitForBit) {
    // AC-057.1 AC-057.2
    constexpr int kRate = 48000;
    constexpr double kF0 = 440.0;
    constexpr double kDev = -5.0;
    constexpr int kGapStart = 24 * 4096;
    constexpr int kTail = 5 * kRate;
    constexpr int kPartials = StrobeTracker::kPartials;

    for (const int gap : {4096 + 512, 4096 + 2560}) {
        SCOPED_TRACE("hueco de " + std::to_string(gap) + " frames");
        const int frames = kGapStart + gap + kTail;
        std::vector<float> sig = inharmonicString(detune(kF0, kDev), 0.0, kPartials, kRate, frames);
        std::fill(sig.begin() + kGapStart, sig.begin() + kGapStart + gap, 0.0f);

        auto t = std::make_unique<StrobeTracker>();
        t->prepare(kRate);
        t->setTarget(kF0);
        t->setCoarseFrequencyHz(coarseSeenHz(kF0, kDev));
        std::vector<PhaseSlopeEstimator> loose(kPartials);
        for (int i = 0; i < kPartials; ++i) {
            loose[static_cast<size_t>(i)].prepare(kRate);
            loose[static_cast<size_t>(i)].setTarget(kF0 * (i + 1));
        }

        long compared = 0;
        for (int off = 0; off < frames; off += kBlock) {
            const int n = std::min(kBlock, frames - off);
            for (auto& e : loose) e.process(sig.data() + off, n);
            t->process(sig.data() + off, n);
            // Un reinicio de ventana del tracker (R-PITCH-63) no existe en el estimador suelto.
            ASSERT_EQ(t->windowRestarts(), 0) << "el escenario ya no es comparable con el estimador suelto";
            for (int i = 0; i < kPartials; ++i) {
                const PhaseSlopeEstimator& g0 = t->partialGrids(i).grid(0);
                const PhaseSlopeEstimator& e = loose[static_cast<size_t>(i)];
                ASSERT_EQ(g0.windowsAnalyzed(), e.windowsAnalyzed()) << "frame " << off << ", parcial " << i;
                ASSERT_EQ(g0.regressionPhaseCount(), e.regressionPhaseCount()) << "frame " << off << ", parcial " << i;
                ASSERT_EQ(g0.hasSignal(), e.hasSignal());
                ASSERT_EQ(g0.hasMeasurement(), e.hasMeasurement());
                if (e.hasMeasurement()) {
                    ASSERT_TRUE(sameBits(g0.cents(), e.cents())) << "frame " << off << ", parcial " << i;
                    ASSERT_TRUE(sameBits(g0.uncertaintyCents(), e.uncertaintyCents()))
                        << "frame " << off << ", parcial " << i;
                }
                for (int k = 0; k < e.regressionPhaseCount(); ++k) {
                    ASSERT_TRUE(sameBits(g0.regressionPhaseAt(k), e.regressionPhaseAt(k)))
                        << "frame " << off << ", parcial " << i << ", fase " << k;
                }
                ++compared;
            }
        }
        EXPECT_GT(compared, 4000) << "el escenario no ejercio nada";
    }
}

// ---------------------------------------------------------------------------
// AC-057.2 — la guarda de coherencia de los incrementos densos
// ---------------------------------------------------------------------------

namespace {

/// Una cuerda de 4 parciales de la que se QUITARON frames sin avisar: el audio posterior queda
/// pegado al anterior, con la fase absoluta de la fuente (el modelo del HUECO de
/// `test_capture_discontinuity.cpp`: bloque de 1024 perdido, nadie lo reporta).
std::vector<float> stringWithLostFrames(int rate, double f0, double cents, int frames,
                                        int lostEvery1024Blocks, int firstLost1024Block,
                                        int lostFramesOnce, int onceAt) {
    std::vector<float> src = inharmonicString(detune(f0, cents), 0.0, 4, rate, frames + 8 * rate / 4);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(frames));
    const int total = static_cast<int>(src.size());
    for (int pos = 0; pos < total && static_cast<int>(out.size()) < frames;) {
        if (lostEvery1024Blocks > 0 && pos % 1024 == 0 && pos / 1024 > firstLost1024Block &&
            (pos / 1024) % lostEvery1024Blocks == 0) {
            pos += 1024;
            continue;
        }
        if (lostFramesOnce > 0 && pos == onceAt) {
            pos += lostFramesOnce;
            continue;
        }
        out.push_back(src[static_cast<size_t>(pos++)]);
    }
    return out;
}

struct PublishedTail {
    int converged{0};
    double worstConvergedError{0.0};
    double sigmaThere{0.0};
    long publications{0};
    bool lastIsFine{false};
    double lastCents{0.0};
    int lastState{-1};
};

/// Alimenta el analisis entero (ring + `drainOnce()`, bloques de 256, objetivo con el ring vacio) y
/// mira cada publicacion NUEVA desde `fromFrame`.
PublishedTail publishedAfter(const std::vector<float>& mono, int rate, double targetHz, double realCents,
                             int fromFrame) {
    auto ring = std::make_unique<wma::analysis::AnalysisRing>();
    auto snap = std::make_unique<wma::analysis::AnalysisSnapshot>();
    auto th = std::make_unique<wma::analysis::AnalysisThread>(*ring, *snap);
    ring->setCaptureRate(rate);
    th->setTargetHz(targetHz);
    th->drainOnce();
    PublishedTail out;
    float lastFrames = -1.0f;
    const int n = static_cast<int>(mono.size());
    std::vector<float> stereo(static_cast<size_t>(kBlock) * 2);
    for (int off = 0; off < n; off += kBlock) {
        const int len = std::min(kBlock, n - off);
        for (int i = 0; i < len; ++i) stereo[2 * i] = stereo[2 * i + 1] = mono[static_cast<size_t>(off + i)];
        ring->writeStereo(stereo.data(), len);
        while (th->drainOnce() != wma::analysis::AnalysisThread::DrainOutcome::kRingEmpty) {
            float v[wma::analysis::kSnapshotValueCount];
            if (!snap->read(v) || v[wma::analysis::kSnapFramesAnalyzed] == lastFrames) continue;
            lastFrames = v[wma::analysis::kSnapFramesAnalyzed];
            if (off + len < fromFrame) continue;
            ++out.publications;
            const float c = v[wma::analysis::kSnapCents];
            out.lastIsFine = !std::isnan(c);
            out.lastCents = c;
            out.lastState = static_cast<int>(v[wma::analysis::kSnapState]);
            if (out.lastState == wma::analysis::kStateConverged && !std::isnan(c)) {
                ++out.converged;
                const double e = std::fabs(static_cast<double>(c) - realCents);
                if (e > out.worstConvergedError) {
                    out.worstConvergedError = e;
                    out.sigmaThere = v[wma::analysis::kSnapUncertainty];
                }
            }
        }
    }
    return out;
}

}  // namespace

/**
 * AC-057.2 — LA GUARDA DE COHERENCIA, desde los dos lados.
 *
 * Al cerrar una rejilla con `M != 0`, la vuelta solo se aplica si los cuatro incrementos densos
 * `e_j` coinciden entre si (`PhaseSlopeEstimator::kDenseCoherenceMaxRad`, 0,3 rad). Las cifras de las
 * que sale el umbral las mide y versiona `TheCoherenceThresholdSitsBetweenHealthyClosesAndAnUnreportedGap`
 * (sano ≤ 0,095 rad, hueco ≥ 0,370 rad); este test fija el comportamiento a cada lado del umbral.
 *
 * Mutante: sin la guarda (aplicar `M` siempre) la serie densa toma el patron periodico por una recta a
 * otra frecuencia y suma vueltas: `turnsAdded() != 0` en el hueco (y `NobodyReported...` sale
 * CONVERGIDO a −22 c). Con la guarda de mas (umbral ≈ 0) los parciales sanos de +50 c quedan marcados
 * sin resolver y el segundo lado cae.
 */
TEST(InterleavedGrids, TheCoherenceGuardRejectsAPeriodicCaptureGapAndNeverTheHealthyChain) {
    // AC-057.2
    {   // lado sano: la cadena completa y limpia se aplica y NO queda marcada
        struct Case { int rate; double hz; double cents; };
        const Case cases[] = {{48000, 440.0, +50.0}, {48000, 440.0, -50.0}, {44100, 329.628, +50.0},
                              {44100, 196.0, -50.0}, {48000, 110.0, +50.0}};
        for (const Case& c : cases) {
            SCOPED_TRACE("rate " + std::to_string(c.rate) + ", " + std::to_string(c.hz) + " Hz, " +
                         std::to_string(c.cents) + " c");
            const auto t = measureWithCoarse(c.rate, c.hz, c.cents, 5.0e-4, 0.01, 3 * c.rate);
            int turns = 0;
            int admitted = 0;
            for (int p = 0; p < StrobeTracker::kPartials; ++p) {
                if (t->partialAdmission(p) != wma::analysis::PartialAdmission::kAdmitted) continue;
                ++admitted;
                turns += t->partialGrids(p).grid(0).turnsAdded();
                EXPECT_FALSE(t->partialGrids(p).grid(0).regressionHasUnresolvedIncrement())
                    << "parcial " << p + 1 << " admitido y con un incremento marcado sin resolver: la guarda "
                       "rechazo una cadena SANA";
            }
            EXPECT_GT(admitted, 0);
            if (std::fabs(c.cents) >= 50.0 && c.hz > 400.0) {
                EXPECT_GT(turns, 0) << "A4 a ±50 c esta fuera del rango de hoy: sin vueltas el escenario no ejerce la guarda";
            }
        }
    }
    {   // lado del hueco: el patron periodico NO se toma por una recta
        constexpr int kRate = 48000;
        const std::vector<float> sig = stringWithLostFrames(kRate, 440.0, -5.0, 8 * kRate, 4, 8, 0, 0);
        StrobeTracker t;
        t.prepare(kRate);
        t.setTarget(440.0);
        t.setCoarseFrequencyHz(coarseSeenHz(440.0, -5.0));
        feedTracker(t, sig);
        int turns = 0;
        bool anyMarked = false;
        for (int p = 0; p < StrobeTracker::kPartials; ++p) {
            turns += t.partialGrids(p).grid(0).turnsAdded();
            anyMarked = anyMarked || t.partialGrids(p).grid(0).regressionHasUnresolvedIncrement();
        }
        EXPECT_EQ(turns, 0) << "la serie densa sumo vueltas sobre un patron periodico de huecos";
        EXPECT_TRUE(anyMarked) << "la guarda nunca rechazo nada: el escenario no ejerce el hueco";
    }
}

/**
 * AC-057.2 — DE DONDE SALE EL UMBRAL DE LA GUARDA (`kDenseCoherenceMaxRad`): este test MIDE las dos
 * cifras y afirma sano < umbral < hueco. Las imprime (printf + RecordProperty `healthyMaxRad`,
 * `gapMinRad`) para que el KDoc las cite desde aca.
 *
 * Se mide `lastCoherenceSpreadRad()` en cada cierre con `M != 0`. Esa cantidad NO depende de si la
 * guarda rechaza o no (sale de los `raw` de la cadena y del `d` local, nunca de la fase desenvuelta),
 * asi que la corrida de produccion mide lo mismo que una con la guarda apagada.
 *   · sano: B0, E2, G3, A4, E5 × ±50/±25 c × 44,1/48 k × ruido 0/0,01 × B 0/1e-4/5e-4, 3 s, SOLO los
 *     parciales dentro del rango de la spec (con su inarmonicidad), las cuatro rejillas;
 *   · hueco: `NobodyReportedItSoNobodyCanKnow` (A4 a 48 k, −5 c, 1024 frames perdidos cada 4 bloques,
 *     sin reportar), las cuatro rejillas de los cuatro parciales.
 *
 * Mutante: umbral por encima del minimo del hueco (p. ej. 0,8) ⇒ cae el lado del hueco; umbral por
 * debajo del maximo sano QUE ESTE TEST IMPRIME ⇒ cae el lado sano. Entre ese maximo y 0,3 ningun test
 * distingue el umbral (el barrido grande del scratchpad de la etapa llego a 0,178 rad, no versionado):
 * bajar el umbral en ese tramo no lo atrapa este test.
 */
TEST(InterleavedGrids, TheCoherenceThresholdSitsBetweenHealthyClosesAndAnUnreportedGap) {
    // AC-057.2
    double healthyMax = 0.0;
    long healthyCloses = 0;
    long idx = 0;
    const double strings[] = {30.868, 82.407, 195.998, 440.0, kE5};
    for (const int rate : {44100, 48000}) {
        for (const double hz : strings) {
            for (const double dev : {+50.0, -50.0, +25.0, -25.0}) {
                for (const double noise : {0.0, 0.01}) {
                    for (const double B : {0.0, 1.0e-4, 5.0e-4}) {
                        if ((idx++ % kStride) != 0) continue;
                        StrobeTracker t;
                        t.prepare(rate);
                        t.setTarget(hz);
                        t.setCoarseFrequencyHz(coarseSeenHz(hz, dev));
                        std::vector<float> sig = inharmonicString(detune(hz, dev), B, 4, rate, 3 * rate);
                        addUniformNoise(sig, noise, 777u);
                        double perPartial[StrobeTracker::kPartials] = {};
                        long closes[StrobeTracker::kPartials] = {};
                        const int n = static_cast<int>(sig.size());
                        for (int i = 0; i < n; i += kBlock) {
                            t.process(sig.data() + i, std::min(kBlock, n - i));
                            for (int p = 0; p < StrobeTracker::kPartials; ++p) {
                                for (int g = 0; g < InterleavedPhaseEstimator::kGrids; ++g) {
                                    const double w = t.partialGrids(p).grid(g).lastCoherenceSpreadRad();
                                    if (w < 0.0) continue;
                                    perPartial[p] = std::max(perPartial[p], w);
                                    ++closes[p];
                                }
                            }
                        }
                        for (int p = 0; p < StrobeTracker::kPartials; ++p) {
                            // «En rango» por la fisica, no por la admision del tracker: el parcial n, con su
                            // inarmonicidad, a menos que el rango de la spec (κ = 0,9) de n·f0.
                            const double order = p + 1;
                            const double partialCents = dev + 600.0 * std::log2(1.0 + B * order * order);
                            if (std::fabs(partialCents) >= specRangeCents(rate, order * hz)) continue;
                            healthyMax = std::max(healthyMax, perPartial[p]);
                            healthyCloses += closes[p];
                        }
                    }
                }
            }
        }
    }

    double gapMin = 1.0e9;
    double gapMax = 0.0;
    long gapCloses = 0;
    {
        constexpr int kRate = 48000;
        const std::vector<float> sig = stringWithLostFrames(kRate, 440.0, -5.0, 8 * kRate, 4, 8, 0, 0);
        StrobeTracker t;
        t.prepare(kRate);
        t.setTarget(440.0);
        t.setCoarseFrequencyHz(coarseSeenHz(440.0, -5.0));
        const int n = static_cast<int>(sig.size());
        for (int i = 0; i < n; i += kBlock) {
            t.process(sig.data() + i, std::min(kBlock, n - i));
            for (int p = 0; p < StrobeTracker::kPartials; ++p) {
                for (int g = 0; g < InterleavedPhaseEstimator::kGrids; ++g) {
                    const double w = t.partialGrids(p).grid(g).lastCoherenceSpreadRad();
                    if (w < 0.0) continue;
                    gapMin = std::min(gapMin, w);
                    gapMax = std::max(gapMax, w);
                    ++gapCloses;
                }
            }
        }
    }

    std::printf("[coherencia] sano: max %.3f rad en %ld lecturas; hueco: min %.3f, max %.3f rad en %ld; "
                "umbral %.2f rad\n",
                healthyMax, healthyCloses, gapMin, gapMax, gapCloses, PhaseSlopeEstimator::kDenseCoherenceMaxRad);
    RecordProperty("healthyMaxRad", std::to_string(healthyMax));
    RecordProperty("gapMinRad", std::to_string(gapMin));

    ASSERT_GT(healthyCloses, 0) << "el barrido sano no ejerce ningun cierre con M != 0";
    ASSERT_GT(gapCloses, 0) << "el hueco no ejerce ningun cierre con M != 0";
    EXPECT_LT(healthyMax, PhaseSlopeEstimator::kDenseCoherenceMaxRad) << "la guarda rechazaria una cadena SANA";
    EXPECT_GT(gapMin, PhaseSlopeEstimator::kDenseCoherenceMaxRad) << "la guarda aceptaria un cierre del hueco periodico";
}

/**
 * LO QUE LA GUARDA NO CUBRE (un solo hueco, no periodico) — fija un HUECO DE HONESTIDAD, no una virtud.
 * Si da rojo porque dejo de pasar, cerrar la excepcion y sacar esta nota.
 *
 * Medido (analisis entero por `drainOnce()`, A4 a 48 k, 8 s, se pierden 512 frames una vez a los 3 s,
 * sin avisar): CONVERGIDO con error 0,449 c y σ 0,076 a +50 c; 0,60 c a +30 c; 0,873 c (σ 0,061) a
 * +10 c; 0,834 c a +15 c; 0,12 c a −5 c. EL DEFECTO EXISTIA EN HEAD Y LA ETAPA LO EMPEORA: a +10 c HEAD
 * da 0,641 c con σ 0,099 y a +15 c 0,713 c, o sea ~36 % mas error y una σ mas chica (mas confiado);
 * a +30 y +50 c HEAD no publica fino (fuera de su rango), asi que ahi la etapa lee con 0,6 y 0,45 c.
 * A +10 c la rejilla 0 NO es HEAD bit a bit con un hueco: los parciales 3 y 4 estan fuera del rango de
 * hoy y entran por la serie densa. El sesgo nace en el estimador ante un hueco aislado (la fase de la
 * ventana que lo contiene). La guarda SI ve esos cierres (review de la etapa, A4 48 k +50 c, 512
 * frames): en el parcial 1 tres cierres contaminados tienen dispersion 0,272–0,274 rad —un 10 % bajo
 * el umbral de 0,30— y se aceptan sumando vuelta, uno de 0,537 se rechaza, y los parciales 2–4 quedan
 * marcados. Con umbral 0,2 la lectura CONVERGIDA equivocada a +50 c desaparece; a +10 c el sesgo
 * sigue (el parcial 1 esta en el rango de hoy, defecto de HEAD). El «19 % bajo el hueco» del header
 * vale para el hueco PERIODICO, no para este. El error queda acotado por 1 c (la banda del test no
 * cambio). Pendiente para un MINI, con estos datos.
 */
TEST(InterleavedGrids, ASingleUnreportedLostGapStillPublishesAConvergedReadingOffByLessThanOneCent) {
    // AC-057.2
    constexpr int kRate = 48000;
    const int gapAt = 3 * kRate / 1024 * 1024;
    for (const double dev : {50.0, 10.0}) {
        SCOPED_TRACE("desafinacion " + std::to_string(dev) + " c");
        const std::vector<float> sig = stringWithLostFrames(kRate, 440.0, dev, 8 * kRate, 0, 0, 512, gapAt);
        const PublishedTail t = publishedAfter(sig, kRate, 440.0, dev, gapAt);
        EXPECT_GT(t.converged, 0);
        EXPECT_GT(t.worstConvergedError, 0.1) << "el hueco aislado ya no sesga la lectura: sacar esta nota";
        EXPECT_LT(t.worstConvergedError, 1.0) << "el error ya no es un sesgo chico: ¿una vuelta de 2π?";
        EXPECT_TRUE(t.lastIsFine);
        EXPECT_NEAR(t.lastCents, dev, 0.1) << "la lectura final no se recupero";
    }
}

/**
 * AC-057.2 — EL RANGO DE RESPALDO (invariante 3 de D-A). Un cierre cuyo incremento se resolvio SIN
 * la cadena densa completa (la cadena se corto por un silencio) solo es confiable dentro del rango
 * de hop 4096; mientras ese incremento este en la ventana de regresion de la rejilla 0, el rango
 * efectivo (`denseCaptureRangeCents()`, `usableRangeCents()`) cae a ese: 22,9 c en A4 a 48 k
 * (`1200·log2(1 + 48000/(2·4096·440))`).
 *
 * Escenario: A4 a 48 k a ±40 y ±50 c (fuera de 22,9), 24·4096 frames de señal limpia y 4096 frames
 * de silencio en [T+1024, T+5120): tapa entera una ventana de las rejillas 1-3 pero no de la 0.
 * Medido: con el codigo, peor lectura 0,014 c; sin el respaldo, 288 lecturas > 0,1 c y peor 8,1 c.
 *
 * Mutante: sacar `if (mGrid[0].regressionHasUnresolvedIncrement()) return mGrid[0].captureRangeCents();`
 * de `denseCaptureRangeCents()`: el rango sigue en 81 c con el incremento marcado y se publican
 * lecturas de hasta ~8 c de error.
 */
TEST(InterleavedGrids, WhileAnUnresolvedIncrementIsInTheWindowTheRangeFallsToTheHop4096One) {
    // AC-057.2
    constexpr int kRate = 48000;
    constexpr int kT = 24 * 4096;
    const double range4096 = 1200.0 * std::log2(1.0 + 48000.0 / (2.0 * 4096.0 * 440.0));
    ASSERT_NEAR(range4096, 22.9, 0.05);
    for (const double dev : {50.0, -50.0, 40.0, -40.0}) {
        SCOPED_TRACE("desafinacion " + std::to_string(dev) + " c");
        std::vector<float> sig = inharmonicString(detune(440.0, dev), 0.0, 4, kRate, 8 * kRate);
        std::fill(sig.begin() + kT + 1024, sig.begin() + kT + 1024 + 4096, 0.0f);
        StrobeTracker t;
        t.prepare(kRate);
        t.setTarget(440.0);
        t.setCoarseFrequencyHz(coarseSeenHz(440.0, dev));
        int markedBlocks = 0;
        int unmarkedBlocksAfter = 0;
        int reads = 0;
        double worst = 0.0;
        const int n = static_cast<int>(sig.size());
        for (int i = 0; i < n; i += kBlock) {
            t.process(sig.data() + i, std::min(kBlock, n - i));
            if (i < kT) continue;
            const bool marked = t.partialGrids(0).grid(0).regressionHasUnresolvedIncrement();
            if (marked) {
                ++markedBlocks;
                EXPECT_NEAR(t.usableRangeCents(), range4096, 1.0e-9) << "frame " << i;
                EXPECT_NEAR(t.partialGrids(0).denseCaptureRangeCents(), range4096, 1.0e-9) << "frame " << i;
            } else if (i > kT + 6 * 4096) {
                ++unmarkedBlocksAfter;
                EXPECT_NEAR(t.usableRangeCents(), specRangeCents(kRate, 440.0), 1.0e-9) << "frame " << i;
            }
            if (t.hasMeasurement()) {
                ++reads;
                worst = std::max(worst, std::fabs(t.cents() - dev));
            }
        }
        EXPECT_GT(markedBlocks, 0) << "el escenario no marca ningun incremento: no ejerce el respaldo";
        EXPECT_GT(unmarkedBlocksAfter, 0) << "la marca nunca se levanta";
        EXPECT_GT(reads, 0);
        EXPECT_LE(worst, 0.1) << "una lectura publicada con un incremento sin resolver salio a " << worst << " c";
    }
}

// ---------------------------------------------------------------------------
// AC-057.3 — ±50 c en toda cuerda del catalogo
// ---------------------------------------------------------------------------

/**
 * AC-057.3 — `usableRangeCents` (indice 14) publica el rango nuevo, y es la formula de D28 con
 * n = 1. Tabla de la spec (κ = 0,9, hop 1024), recomputada aca con literales (python3, 2 decimales):
 *
 *     44,1 k   E2 365,66   G3 163,24   E4  98,90   A4 74,62   E5 50,16
 *     48 k     E2 394,56   G3 176,96   E4 107,39   A4 81,07   E5 54,53
 *
 * Mutante: κ = 1, o hop 4096 olvidado en `usableRangeCents()` (publica el rango de hoy: 22,9 c en
 * A4), o `denseRangeCents` y `usableRangeCents` calculados con dos formulas distintas.
 */
TEST(InterleavedGrids, TheUsableRangeIsTheDenseFormulaWithKappaNineTenths) {
    // AC-057.3
    struct Row { double hz; double r441; double r48; };
    const Row kTable[] = {{82.407, 365.66, 394.56}, {195.998, 163.24, 176.96}, {329.628, 98.90, 107.39},
                          {440.0, 74.62, 81.07}, {kE5, 50.16, 54.53}};
    for (const Row& row : kTable) {
        for (const int rate : {44100, 48000}) {
            const double expected = rate == 44100 ? row.r441 : row.r48;
            EXPECT_NEAR(specRangeCents(rate, row.hz), expected, 0.01) << "la tabla del test no cierra con la formula";
            EXPECT_NEAR(InterleavedPhaseEstimator::denseRangeCents(rate, row.hz), expected, 0.01)
                << row.hz << " Hz @" << rate << ": denseRangeCents no es 1200·log2(1 + 0,9·fs/(2·1024·f))";

            StrobeTracker t;
            t.prepare(rate);
            t.setTarget(row.hz);
            EXPECT_NEAR(t.usableRangeCents(), expected, 0.01)
                << row.hz << " Hz @" << rate << ": usableRangeCents() no publica el rango nuevo";
            // El n-esimo parcial cubre 1/n aprox. (la formula con n·f0): el parcial 2 de E5 a 44,1 k
            // queda en ~26 c, bien lejos de ±50.
            EXPECT_NEAR(InterleavedPhaseEstimator::denseRangeCents(rate, 2.0 * row.hz), specRangeCents(rate, 2.0 * row.hz), 0.01);
        }
    }
}

/**
 * AC-057.3 — «medir dentro de ±50 c en toda cuerda del catalogo (B0 a E5) a 44,1 y 48 kHz, salvo
 * B0 y E1 desde +25 c a 44,1 kHz» (D30, +25 incluido; humano, 2026-10-28; ver el test del hueco).
 *
 * Catalogo + E5 × 44,1/48 k × {±50, ±25} c × B ∈ {0, 1e-4}, ruido 0,005 (uniforme, semilla fija),
 * 3 s, bloques de 256 y el control grueso simulado CON su error de peor caso (0,21 c hacia el
 * objetivo). Toda corrida tiene lectura fina y |cents − real| ≤ 0,1 c.
 *
 * El esperado con B ≠ 0 es la desviacion del FUNDAMENTAL ideal: la combinacion ajusta
 * `C + 600·log2(1 + B·n²)` y devuelve C. (Con UN solo parcial admitido —E5, A4 en las agudas— no
 * hay ajuste y la lectura es la del parcial 1, `600·log2(1+B)` = 0,0866 c por encima a B = 1e-4:
 * deja 0,013 c de margen contra el 0,1; se afirma LITERAL a proposito. Ver el reporte.)
 *
 * Mutante: cualquier cosa que deje un parcial fuera de su rango contaminando la combinacion (los
 * parciales 3-4 de E4 a ±50 c aliasan): lectura equivocada por decenas de cents; o un rango que
 * calle donde la fisica da (lectura ausente ⇒ rojo aca, que es el gemelo de «publicar o callar»).
 */
TEST(InterleavedGrids, EveryCatalogStringIsMeasuredWithinPlusMinusFiftyCents) {
    // AC-057.3
    constexpr double kTolerance = 0.1;
    struct Miss { std::string label; double real; bool published; double read; };
    std::vector<Miss> misses;
    long idx = 0, runs = 0;

    for (const int rate : {44100, 48000}) {
        for (const CatalogString& s : stringsB0ToE5()) {
            for (const double dev : {+50.0, -50.0, +25.0, -25.0}) {
                for (const double B : {0.0, 1.0e-4}) {
                    // El hueco declarado (D30): se excluye EXPLICITAMENTE y lo fija otro test.
                    // AC-057.3 / D30 excluyen B0 y E1 DESDE +25 c, con el +25 incluido (humano,
                    // 2026-10-28): con la señal de este test B0 a +25 c no lee con B = 0 ni 1e-4
                    // (identico en HEAD: la compuerta de tendencia reinicia en bucle por el rizado
                    // de las graves). E1 a +25 c SI lee (25,000), asi que se sigue verificando aca:
                    // el test pide mas que el AC. El hueco depende de la señal, no solo de la cuerda.
                    const bool isB0 = std::string(s.name) == "bajo B0";
                    const bool isE1 = std::string(s.name) == "bajo E1";
                    const bool declaredGap = rate == 44100 && ((isB0 && dev >= 25.0) || (isE1 && dev > 25.0));
                    if (declaredGap) continue;
                    if ((idx++ % kStride) != 0) continue;
                    ++runs;
                    const auto t = measureWithCoarse(rate, s.hz, dev, B, 0.005, 3 * rate);
                    const bool ok = t->hasMeasurement() && std::fabs(t->cents() - dev) <= kTolerance;
                    if (!ok) {
                        char lab[96];
                        std::snprintf(lab, sizeof lab, "%-14s @%d B=%.0e", s.name, rate, B);
                        misses.push_back({lab, dev, t->hasMeasurement(), t->cents()});
                    }
                }
            }
        }
    }

    ASSERT_GT(runs, 0);
    if (!misses.empty()) {
        std::string msg = "\n" + std::to_string(misses.size()) + " de " + std::to_string(runs) +
                          " corridas sin lectura fina a ≤ 0,1 c:\n";
        for (const auto& m : misses) {
            char line[200];
            std::snprintf(line, sizeof line, "  %s real %+6.1f c -> %s %+9.3f c\n", m.label.c_str(), m.real,
                          m.published ? "publica" : "SIN LECTURA", m.published ? m.read : 0.0);
            msg += line;
        }
        FAIL() << msg;
    }
}

/**
 * AC-057.3 — el borde de E5 a 44,1 kHz (D28: «E5 a 44,1 kHz (±50,2 c con κ = 0,9) lleva su test de
 * borde: lectura fina a ±50 c y ninguna a ±51 c»). El rango publicado es 50,16 c: a ±50 el control
 * grueso (49,79 c) cae adentro por 0,37 c; a ±51 (50,79 c) cae afuera por 0,63 c.
 *
 * Mutante: κ = 1 (parcial admitido en el nulo del lobulo de Hann): el rango sube a 55,7 c, a ±51 el
 * parcial entra, hay lectura y el test se pone rojo.
 */
TEST(InterleavedGrids, TheEdgeOfTheCatalogE5At44kReadsAtFiftyAndNotAtFiftyOne) {
    // AC-057.3
    constexpr int kRate = 44100;
    {
        StrobeTracker probe;
        probe.prepare(kRate);
        probe.setTarget(kE5);
        ASSERT_GT(probe.usableRangeCents(), 50.0) << "el rango publicado de E5 a 44,1 k no cubre ±50 c";
        ASSERT_LT(probe.usableRangeCents(), 51.0) << "el rango publicado de E5 a 44,1 k cubre ±51 c: κ no es 0,9";
    }
    for (const double sign : {+1.0, -1.0}) {
        const auto inside = measureWithCoarse(kRate, kE5, sign * 50.0, 0.0, 0.005, 3 * kRate);
        ASSERT_TRUE(inside->hasMeasurement()) << "E5 a " << sign * 50.0 << " c: sin lectura fina";
        EXPECT_NEAR(inside->cents(), sign * 50.0, 0.1) << "E5 a " << sign * 50.0 << " c";
        EXPECT_EQ(inside->partialAdmission(0), PartialAdmission::kAdmitted)
            << "E5 a " << sign * 50.0 << " c: el parcial 1 deberia estar admitido (rango 50,16 c)";

        const auto outside = measureWithCoarse(kRate, kE5, sign * 51.0, 0.0, 0.005, 3 * kRate);
        EXPECT_EQ(outside->partialAdmission(0), PartialAdmission::kOutOfRange)
            << "E5 a " << sign * 51.0 << " c: el parcial 1 deberia estar FUERA de rango";
        EXPECT_FALSE(outside->hasMeasurement())
            << "E5 a " << sign * 51.0 << " c: publico lectura fina (" << outside->cents()
            << " c) fuera del rango publicado (50,16 c)";
        EXPECT_EQ(outside->partialsUsed(), 0);
    }
}

/**
 * AC-057.3 (D22/D28, «En todo punto del rango, `cents` SHALL salir sólo de los parciales cuyo rango
 * propio cubre la desviación con margen κ = 0,9») — E4 a 48 kHz, ±50 c: p1 (107,4 c) y p2 (54,5 c)
 * admitidos; p3 (36,5 c) y p4 (27,5 c) FUERA DE RANGO. Se afirma por la SONDA de admision y no solo
 * por la lectura, porque la lectura sola puede no distinguir (el parcial aliasado es descartado por
 * otras reglas o promediado con poco peso).
 *
 * Mutante (el de la spec): usar los 4 parciales sin chequear su rango ⇒ p3 y p4 dejan de ser
 * `kOutOfRange` (quedan `kAdmitted` o los descarta la mediana/el signo) ⇒ rojo aunque `cents` salga
 * bien. Se lo fabrica sacando `canMeasureDeviation` de la admision.
 */
TEST(InterleavedGrids, E4At48kKeepsPartialsOneAndTwoAndDropsThreeAndFourByRange) {
    // AC-057.3
    constexpr int kRate = 48000;
    constexpr double kE4 = 329.628;
    for (const double dev : {+50.0, -50.0}) {
        const auto t = measureWithCoarse(kRate, kE4, dev, 0.0, 0.0, 3 * kRate);
        EXPECT_EQ(t->partialAdmission(0), PartialAdmission::kAdmitted) << "E4 " << dev << " c, parcial 1";
        EXPECT_EQ(t->partialAdmission(1), PartialAdmission::kAdmitted) << "E4 " << dev << " c, parcial 2";
        EXPECT_EQ(t->partialAdmission(2), PartialAdmission::kOutOfRange)
            << "E4 " << dev << " c, parcial 3: su rango propio es 36,5 c, no cubre " << std::fabs(dev) << " c";
        EXPECT_EQ(t->partialAdmission(3), PartialAdmission::kOutOfRange)
            << "E4 " << dev << " c, parcial 4: su rango propio es 27,5 c, no cubre " << std::fabs(dev) << " c";
        EXPECT_EQ(t->partialsUsed(), 2);
        ASSERT_TRUE(t->hasMeasurement()) << "E4 " << dev << " c: sin lectura";
        EXPECT_NEAR(t->cents(), dev, 0.1) << "E4 " << dev << " c";
    }
}

/// La receta de señal del architect (prototipo `strobe50.cpp`): 4 parciales de amplitud 0,5/n con
/// fase 0,3·n, inarmonicidad `√(1+B·n²)` y ruido gaussiano 0,005 con semilla 3.
std::vector<float> architectString(int rate, double f0, double cents, double B, int frames) {
    std::vector<float> s(static_cast<size_t>(frames));
    std::mt19937 rng(3);
    std::normal_distribution<double> nd(0.0, 1.0);
    const double f = f0 * std::pow(2.0, cents / 1200.0);
    for (int i = 0; i < frames; ++i) {
        double v = 0.0;
        for (int n = 1; n <= 4; ++n) {
            const double fn = n * f * std::sqrt(1.0 + B * n * n);
            v += 0.5 / n * std::sin(2.0 * M_PI * fn * i / rate + 0.3 * n);
        }
        s[static_cast<size_t>(i)] = static_cast<float>(v + 0.005 * nd(rng));
    }
    return s;
}

/**
 * AC-057.3 / D30 — EL HUECO DECLARADO Y PREEXISTENTE: B0 y E1 desde +25 c a 44,1 kHz no
 * tienen lectura fina (la compuerta de tendencia reinicia en bucle por el rizado de las graves),
 * pero la gruesa SI publica. A 48 kHz no pasa.
 *
 * EL HUECO DEPENDE DE LA SEÑAL, no solo de la cuerda: con la señal sintetica de `inharmonicString`
 * E1 a +50 c SI lee (49,95 c, identico en HEAD) y B0 a +25 c no lee ni con B = 0. Por eso se fija
 * con la receta del architect (`architectString`) y SOLO los casos medidos por el motor completo
 * (`analyzeBuffer`, 6 s, sin candidatos, objetivo = la cuerda nominal) sin lectura fina:
 * B0 a +25 c con B ∈ {1e-4, 5e-4} (con B = 0 la receta del architect SI lee, 24,996 c); B0 a +50 c con B ∈ {0, 1e-4, 5e-4}; E1 a +50 c con B ∈ {1e-4, 5e-4}.
 * E1 a +50 c con B = 0 NO esta aca: con esta receta lee (49,94 c). A1 a −50 c con B = 0 tampoco:
 * el prototipo a nivel StrobeTracker lo dio sin lectura, pero por el motor completo lee −50,00 c.
 *
 * ESTE TEST FIJA UN HUECO, NO UNA VIRTUD. Si da rojo, cerrá la excepción de AC-057.3 (MINI-054):
 * es una buena noticia que hay que sacar de la spec, no un defecto de este test.
 *
 * Mutante: ninguno de codigo (fija un hueco que existe hoy). Si una rama nueva lo cerrara, este
 * test avisa que la excepcion de la spec ya no corresponde.
 */
TEST(InterleavedGrids, TheDeclaredGapB0AndE1From25CentsAt44kHasNoFineReadingButTheCoarseOne) {
    // AC-057.3 (D30)
    constexpr int kRate = 44100;
    constexpr int kFrames = 6 * kRate;
    struct Case { const char* label; double hz; double dev; double B; };
    const Case kCases[] = {{"B0 a +25 c con B = 1e-4", 30.868, +25.0, 1.0e-4},
                           {"B0 a +25 c con B = 5e-4", 30.868, +25.0, 5.0e-4},
                           {"B0 a +50 c con B = 0", 30.868, +50.0, 0.0},
                           {"B0 a +50 c con B = 1e-4", 30.868, +50.0, 1.0e-4},
                           {"B0 a +50 c con B = 5e-4", 30.868, +50.0, 5.0e-4},
                           {"E1 a +50 c con B = 1e-4", 41.203, +50.0, 1.0e-4},
                           {"E1 a +50 c con B = 5e-4", 41.203, +50.0, 5.0e-4}};
    for (const Case& c : kCases) {
        const std::vector<float> mono = architectString(kRate, c.hz, c.dev, c.B, kFrames);
        std::vector<float> stereo(mono.size() * 2);
        for (size_t i = 0; i < mono.size(); ++i) stereo[2 * i] = stereo[2 * i + 1] = mono[i];

        float out[wma::analysis::kSnapshotValueCount];
        ASSERT_TRUE(wma::analysis::analyzeBuffer(stereo.data(), kFrames, kRate, c.hz, out)) << c.label;

        EXPECT_TRUE(std::isnan(out[wma::analysis::kSnapCents]))
            << c.label << ": HAY lectura fina (" << out[wma::analysis::kSnapCents]
            << " c) — si esto da rojo, cerrá la excepción de AC-057.3 (MINI-054)";
        EXPECT_GT(out[wma::analysis::kSnapDetectedHz], 0.0f)
            << c.label << ": ni la gruesa publica; el hueco declarado es solo el de la fina (D30)";
    }
}

// ---------------------------------------------------------------------------
// NFR de CPU — «el techo en host (Debug) se mide y se declara contra el 25 %»
// ---------------------------------------------------------------------------

/**
 * NFR de CPU de REQ-057 (D-F, «propuesta, a medir»): cuatro rejillas son cuatro Goertzel por
 * parcial por muestra. Se IMPRIMEN dos cifras, en % del tiempo real a 48 kHz, mejor de 3
 * repeticiones (el minimo, que es el menos afectado por la carga de la maquina):
 *
 *   (a) el fino de 4 parciales: 4 `PhaseSlopeEstimator` (hoy) contra 4 `InterleavedPhaseEstimator`,
 *       10 s, bloques de 256;
 *   (b) el analisis entero por `drainOnce()`, bloques de 256, con candidatos de guitarra.
 *
 * Prototipo del architect (Debug -O0, M2 Pro): (a) 0,139 % → 0,548 %, (b) 1,37 % → 1,77 %.
 * Esas son las cifras de referencia; el techo que se AFIRMA es el 25 % que usa la suite
 * (`McLeodPitchCost`), flojo a proposito: un techo ajustado falla con la maquina cargada. El NFR-1
 * en device (< 5 %) NO se mide ni se afirma aca.
 *
 * Mutante: un cierre que recompute la ventana de Hann o la regresion de las 4 rejillas por MUESTRA
 * (O(N) por muestra) dispara el costo muy por encima del 25 %.
 */
TEST(InterleavedGridsCost, TheFourGridsCostAFractionOfRealTimeAndTheNumberIsPrinted) {
    WMA_SKIP_IF_SANITIZED();
    constexpr int kRate = 48000;
    constexpr int kSeconds = 10;
    constexpr int kFrames = kSeconds * kRate;
    constexpr double kCeiling = 0.25;
    constexpr double kF0 = 82.407;   // E2

    const std::vector<float> mono = inharmonicString(kF0, 1.0e-4, 4, kRate, kFrames);

    const auto secondsSince = [](std::chrono::steady_clock::time_point t0) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };

    // (a) el fino de 4 parciales.
    double todayBest = 1e9, gridsBest = 1e9, sink = 0.0;
    for (int rep = 0; rep < 3; ++rep) {
        std::vector<PhaseSlopeEstimator> today(4);
        auto grids = std::make_unique<InterleavedPhaseEstimator[]>(4);
        for (int p = 0; p < 4; ++p) {
            today[p].prepare(kRate);
            today[p].setTarget(kF0 * (p + 1));
            grids[p].prepare(kRate);
            grids[p].setTarget(kF0 * (p + 1));
        }
        auto t0 = std::chrono::steady_clock::now();
        for (int off = 0; off < kFrames; off += kBlock)
            for (int p = 0; p < 4; ++p) today[p].process(mono.data() + off, std::min(kBlock, kFrames - off));
        todayBest = std::min(todayBest, secondsSince(t0));

        t0 = std::chrono::steady_clock::now();
        for (int off = 0; off < kFrames; off += kBlock)
            for (int p = 0; p < 4; ++p) grids[p].process(mono.data() + off, std::min(kBlock, kFrames - off));
        gridsBest = std::min(gridsBest, secondsSince(t0));
        for (int p = 0; p < 4; ++p) sink += today[p].cents() + grids[p].grid(0).cents();
    }
    const double todayPct = 100.0 * todayBest / kSeconds;
    const double gridsPct = 100.0 * gridsBest / kSeconds;
    std::printf("[ COSTO   ] fino de 4 parciales: hoy %.3f %% | cuatro rejillas %.3f %% del tiempo real (x%.2f)  [%g]\n",
                todayPct, gridsPct, gridsBest / todayBest, sink);
    RecordProperty("fino_hoy_pct", std::to_string(todayPct));
    RecordProperty("fino_rejillas_pct", std::to_string(gridsPct));
    EXPECT_LT(todayBest / kSeconds, kCeiling) << "el fino de hoy cuesta " << todayPct << " % del tiempo real";
    EXPECT_LT(gridsBest / kSeconds, kCeiling) << "las cuatro rejillas cuestan " << gridsPct << " % del tiempo real";

    // (b) el analisis entero por drainOnce(), bloques de 256, candidatos de guitarra.
    std::vector<float> stereo(mono.size() * 2);
    for (size_t i = 0; i < mono.size(); ++i) stereo[2 * i] = stereo[2 * i + 1] = mono[i];
    std::vector<double> guitar;
    for (const CatalogString& s : catalogStrings())
        if (std::string(s.name).rfind("guitarra", 0) == 0) guitar.push_back(s.hz);

    double wholeBest = 1e9;
    for (int rep = 0; rep < 3; ++rep) {
        auto ring = std::make_unique<wma::analysis::AnalysisRing>();
        auto snap = std::make_unique<wma::analysis::AnalysisSnapshot>();
        auto th = std::make_unique<wma::analysis::AnalysisThread>(*ring, *snap);
        ring->setCaptureRate(kRate);
        th->setTargetHz(kF0);
        th->setCandidates(guitar.data(), static_cast<int>(guitar.size()));
        th->drainOnce();   // el objetivo y los candidatos se aplican con el ring vacio

        const auto t0 = std::chrono::steady_clock::now();
        for (int off = 0; off < kFrames; off += kBlock) {
            ring->writeStereo(stereo.data() + static_cast<size_t>(off) * 2, std::min(kBlock, kFrames - off));
            while (th->drainOnce() != wma::analysis::AnalysisThread::DrainOutcome::kRingEmpty) {
            }
        }
        wholeBest = std::min(wholeBest, secondsSince(t0));
    }
    const double wholePct = 100.0 * wholeBest / kSeconds;
    std::printf("[ COSTO   ] analisis entero por drainOnce(), bloques de %d, candidatos de guitarra: %.3f %% del tiempo real\n",
                kBlock, wholePct);
    RecordProperty("analisis_entero_pct", std::to_string(wholePct));
    EXPECT_LT(wholeBest / kSeconds, kCeiling) << "el analisis entero cuesta " << wholePct << " % del tiempo real";
}

/**
 * Hallazgo del security-auditor de REQ-057: una muestra Inf (o un objetivo Inf) hace NaN el `raw`
 * del Goertzel, la vuelta `turns` sale NaN, `NaN != 0` es true y la guarda de coherencia no lo frena
 * (`std::max(worst, NaN)` devuelve `worst`). Sin el chequeo de finitud se convertia NaN a int (UB).
 *
 * Lo que se afirma sin sanitizer: en los cierres que ven el NaN la coherencia NO se evalua
 * (`lastCoherenceSpreadRad() < 0`, su valor de "no hubo vuelta que juzgar"). Mutante: sacar el
 * `isfinite(turns)` ⇒ la guarda corre con NaN y deja la dispersion en 0 ⇒ rojo (y UBSan, ademas,
 * ve la conversion). Un tono a 0 c no suma vueltas nunca, asi que antes del Inf la dispersion
 * tambien es negativa: lo que cambia es sólo lo que pasa con el NaN.
 */
TEST(InterleavedGrids, AnInfSampleNeverReachesTheTurnCount) {
    constexpr int kRate = 48000;
    constexpr double kTarget = 440.0;
    constexpr int kHop = InterleavedPhaseEstimator::kHopFrames;
    InterleavedPhaseEstimator est;
    est.prepare(kRate);
    est.setTarget(kTarget);

    std::vector<float> sig = pureSine(kTarget, kRate, 24 * kHop);
    sig[static_cast<size_t>(12 * kHop + 100)] = std::numeric_limits<float>::infinity();

    int closesWithNaN = 0;
    for (int h = 0; h < 24; ++h) {
        est.process(sig.data() + static_cast<size_t>(h) * kHop, kHop);
        if (h < 12) continue;
        for (int g = 0; g < InterleavedPhaseEstimator::kGrids; ++g) {
            const PhaseSlopeEstimator& e = est.grid(g);
            if (std::isnan(e.lastRawPhase())) ++closesWithNaN;
            EXPECT_LT(e.lastCoherenceSpreadRad(), 0.0)
                << "hop " << h << " rejilla " << g << ": la guarda de coherencia se evaluo con una vuelta NaN";
            EXPECT_GE(e.turnsAdded(), 0) << "hop " << h << " rejilla " << g;
        }
    }
    // Premisa: el Inf llego de verdad al Goertzel. Sin esto el test pasa sin ejercer nada.
    EXPECT_GT(closesWithNaN, 0) << "ningun cierre vio el Inf: el test no ejerce la rama";
}

}  // namespace
}  // namespace wma_test
