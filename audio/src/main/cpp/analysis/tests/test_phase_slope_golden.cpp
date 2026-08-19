/**
 * test_phase_slope_golden.cpp — REQ-001 S2, tareas 2.12 y 2.13.
 *
 * DOS COSAS QUE LOS TESTS DE PROPIEDAD NO PUEDEN DAR
 * ---------------------------------------------------
 * `test_phase_slope.cpp` afirma PROPIEDADES: que el error entra en el
 * presupuesto, que el signo es el del afinador, que la degradacion es monotona.
 * Todas son verdaderas para un rango de implementaciones — y esa holgura es
 * deliberada, porque una propiedad que solo la cumple una implementacion es una
 * transcripcion del codigo.
 *
 * Lo que falta es lo que esa holgura deja pasar: un cambio de DSP que mueva la
 * curva de convergencia SIN salirse del presupuesto. El golden lo congela. Su
 * diff es texto y se lee en el PR — es la parte revisable de una recaptura.
 *
 * Y el costo (2.13), que no es una propiedad sino un NUMERO que S10 necesita
 * para el presupuesto de CPU del NFR-1.
 */

#include "support/AnalysisGolden.h"
#include "support/SyntheticSignal.h"

#include "PhaseSlopeEstimator.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace wma_test {
namespace {

using wma::analysis::PhaseSlopeEstimator;

constexpr int kRate = 48000;

/// Los casos que se congelan. Cubren el rango del AC por los dos extremos, los
/// dos signos, y el caso inarmonico — que es el que mas se puede mover si
/// alguien toca la ventana o el enventanado.
struct Case {
    const char* label;
    double targetHz;
    double detuneCents;
    double inharmonicityB;   ///< 0 = seno puro
};

const std::vector<Case>& cases() {
    static const std::vector<Case> kCases = {
        {"A0_plus1",        27.500, +1.0, 0.0},
        {"E2_plus1",        82.407, +1.0, 0.0},
        {"E2_minus1",       82.407, -1.0, 0.0},
        {"A4_plus1",       440.000, +1.0, 0.0},
        {"C7_plus1",      2093.005, +1.0, 0.0},
        {"E2_resolution",   82.407, +0.063, 0.0},
        {"E2_string_B5e4",  82.407, 0.0, 5e-4},
    };
    return kCases;
}

/// La curva: que se lee a 1, 2 y 3 s de integracion.
const std::vector<double>& checkpoints() {
    static const std::vector<double> kSeconds = {1.0, 2.0, 3.0};
    return kSeconds;
}

std::vector<float> signalFor(const Case& c, int frames) {
    if (c.inharmonicityB > 0.0) {
        return inharmonicString(c.targetHz, c.inharmonicityB, 6, kRate, frames);
    }
    return pureSine(detune(c.targetHz, c.detuneCents), kRate, frames);
}

/**
 * TAREA 2.12. La curva de convergencia, congelada.
 *
 * Se muestrea en bloques de 512 y en los checkpoints declarados. El bloque entra
 * en el golden por omision: el resultado NO puede depender de él (hay un test que
 * lo exige bit a bit), asi que fijarlo acá no esconde nada.
 */
TEST(GoldenPhaseSlope, TheConvergenceCurveMatchesItsGolden) {
    std::vector<golden::Sample> rows;

    for (const auto& c : cases()) {
        PhaseSlopeEstimator est;
        est.prepare(kRate);
        est.setTarget(c.targetHz);

        const int total = static_cast<int>(checkpoints().back() * kRate);
        const auto sig = signalFor(c, total);

        size_t next = 0;
        int i = 0;
        while (i < total && next < checkpoints().size()) {
            const int take = std::min(512, total - i);
            est.process(sig.data() + i, take);
            i += take;

            const double elapsed = static_cast<double>(i) / kRate;
            if (elapsed + 1e-9 >= checkpoints()[next]) {
                rows.push_back({std::string(c.label), checkpoints()[next],
                                est.cents(), est.uncertaintyCents()});
                ++next;
            }
        }
    }

    ASSERT_EQ(rows.size(), cases().size() * checkpoints().size());
    golden::checkOrRegen("phase_slope_convergence", kRate,
                         PhaseSlopeEstimator::kWindowFrames, rows);
}

// ---------------------------------------------------------------------------
// 2.13 — el costo
// ---------------------------------------------------------------------------

/**
 * TAREA 2.13. Cuanto cuesta un objetivo, para el presupuesto de NFR-1.
 *
 * QUE MIDE Y QUE NO
 * -----------------
 * Mide el costo del estimador **en esta maquina de desarrollo**, que es un
 * Apple Silicon y NO el piso declarado del NFR (Snapdragon serie 6 / A12). El
 * numero que vale para el contrato lo mide S10 · 10.2 **en dispositivo**. Este
 * sirve para dos cosas mas modestas y utiles: dimensionar el orden de magnitud
 * antes de que S5 corra varios objetivos a la vez, y **atrapar una regresion de
 * orden de magnitud** si alguien mete una FFT donde hay un Goertzel.
 *
 * POR ESO LA ASERCION ES FLOJA A PROPOSITO
 * -----------------------------------------
 * Se afirma que un objetivo cuesta menos del **10 % del tiempo real**, que es
 * ~30x mas que lo esperado. Un techo ajustado seria un test que falla cuando la
 * maquina esta cargada —este repo ya se comio cuatro timeouts de TSan que eran
 * de la maquina y no del codigo— y un guardrail que falla por ruido se termina
 * silenciando. El numero medido se reporta igual, siempre.
 */
TEST(PhaseSlopeCost, OneTargetCostsAFractionOfRealTimeAndTheNumberIsRecorded) {
    const double target = 82.407;
    const int frames = 10 * kRate;                 // 10 s de audio
    const auto sig = pureSine(detune(target, 1.0), kRate, frames);

    PhaseSlopeEstimator est;
    est.prepare(kRate);
    est.setTarget(target);

    const auto t0 = std::chrono::steady_clock::now();
    int i = 0;
    while (i < frames) {
        const int take = std::min(256, frames - i);   // bloque de audio realista
        est.process(sig.data() + i, take);
        i += take;
    }
    const auto t1 = std::chrono::steady_clock::now();

    const double elapsedSec = std::chrono::duration<double>(t1 - t0).count();
    const double audioSec = static_cast<double>(frames) / kRate;
    const double realTimeFraction = elapsedSec / audioSec;
    const double nsPerFrame = elapsedSec * 1e9 / frames;
    const double usPerWindow =
        nsPerFrame * PhaseSlopeEstimator::kWindowFrames / 1000.0;

    RecordProperty("real_time_fraction_pct", std::to_string(realTimeFraction * 100.0));
    RecordProperty("ns_per_frame", std::to_string(nsPerFrame));
    RecordProperty("us_per_window", std::to_string(usPerWindow));

    std::printf("[ COSTO   ] un objetivo: %.4f %% del tiempo real  |  %.2f ns/frame  |  %.1f us por ventana de %d\n",
                realTimeFraction * 100.0, nsPerFrame, usPerWindow,
                PhaseSlopeEstimator::kWindowFrames);

    EXPECT_LT(realTimeFraction, 0.10)
        << "un solo objetivo cuesta " << realTimeFraction * 100.0
        << " % del tiempo real. El presupuesto del NFR-1 es 5 % para el motor "
           "ENTERO con varios objetivos: a este costo no entra.";
}

}  // namespace
}  // namespace wma_test
