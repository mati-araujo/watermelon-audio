/**
 * test_inharmonicity.cpp — REQ-001 S7.
 *
 * La etapa promete dos cosas y los tests las separan a proposito: que **B se
 * mide** (7.1, 7.2, 7.5) y que **el objetivo perceptual se computa** desde B en
 * vez de tabularse (7.3, 7.4, 7.6, 7.7). La segunda es la que el AC-001.10
 * defiende, y no depende de que la primera sea exacta.
 */

#include "support/SyntheticSignal.h"

#include "support/AnalysisGolden.h"

#include "InharmonicityEstimator.h"
#include "StrobeTracker.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace wma_test {
namespace {

using wma::analysis::InharmonicityEstimator;
using wma::analysis::StringPhysics;
using wma::analysis::StrobeTracker;

constexpr int kRate = 48000;
constexpr int kThreeSeconds = 3 * kRate;

void feed(StrobeTracker& t, const std::vector<float>& sig, int block = 512) {
    int i = 0;
    const int n = static_cast<int>(sig.size());
    while (i < n) {
        const int take = std::min(block, n - i);
        t.process(sig.data() + i, take);
        i += take;
    }
}

/// Corre una cuerda con `B` conocido POR CONSTRUCCION por el strobe y estima.
InharmonicityEstimator measureB(double f0, double B, int frames = kThreeSeconds) {
    StrobeTracker s;
    s.prepare(kRate);
    s.setTarget(f0);
    feed(s, inharmonicString(f0, B, 4, kRate, frames));
    InharmonicityEstimator e;
    e.estimateFrom(s);
    return e;
}

// ---------------------------------------------------------------------------
// 7.1 · AC-001.9 — B se mide, con error relativo < 10 %
// ---------------------------------------------------------------------------
TEST(InharmonicityTest, ItRecoversAKnownBWithinTenPercent) {
    struct Case { const char* name; double hz; double B; };
    const Case kCases[] = {
        {"prima de guitarra", 329.628, 1.0e-5},
        {"guitarra D3",       146.832, 5.0e-5},
        {"bordona E2",         82.407, 1.5e-4},
        {"bajo E1",            41.203, 3.0e-4},
    };

    for (const auto& c : kCases) {
        const auto e = measureB(c.hz, c.B);
        ASSERT_TRUE(e.measured()) << c.name << ": B no convergio";
        const double rel = std::abs(e.b() - c.B) / c.B;
        RecordProperty(std::string("B_rel_error_") + c.name, std::to_string(rel));
        EXPECT_LT(rel, 0.10)
            << c.name << ": B verdadero " << c.B << ", estimado " << e.b()
            << " — error relativo " << rel;
    }
}

// ---------------------------------------------------------------------------
// 7.2 — B crece con el calibre y decrece con la escala, como dice la formula
// ---------------------------------------------------------------------------
TEST(InharmonicityTest, ThePhysicsFallbackMovesTheWayTheFormulaSays) {
    const StringPhysics base{200e9, 0.4e-3, 75.0, 0.648};

    StringPhysics thicker = base; thicker.coreDiameterM = 0.5e-3;
    StringPhysics longer  = base; longer.scaleLengthM   = 0.864;
    StringPhysics tighter = base; tighter.tensionN      = 110.0;

    const double b0 = InharmonicityEstimator::physicsB(base);
    EXPECT_GT(InharmonicityEstimator::physicsB(thicker), b0) << "mas grueso ⇒ mas rigido";
    EXPECT_LT(InharmonicityEstimator::physicsB(longer),  b0) << "mas largo ⇒ menos rigido";
    EXPECT_LT(InharmonicityEstimator::physicsB(tighter), b0) << "mas tenso ⇒ menos rigido";

    // d⁴: duplicar el nucleo tiene que multiplicar B por 16, no "aumentarlo".
    StringPhysics doubled = base; doubled.coreDiameterM = 2.0 * base.coreDiameterM;
    EXPECT_NEAR(InharmonicityEstimator::physicsB(doubled) / b0, 16.0, 1e-9)
        << "B va con d⁴; si esto falla, la formula que se implemento no es la del AC";
    // L²: duplicar la escala tiene que dividir B por 4.
    StringPhysics twiceLong = base; twiceLong.scaleLengthM = 2.0 * base.scaleLengthM;
    EXPECT_NEAR(InharmonicityEstimator::physicsB(twiceLong) / b0, 0.25, 1e-9);
}

// ---------------------------------------------------------------------------
// 7.3 — con B = 0 la correccion es EXACTAMENTE cero
// ---------------------------------------------------------------------------
/**
 * "La correccion no puede ensuciar el caso limpio." Se exige igualdad exacta y no
 * una tolerancia: `log2(1+0) = 0` es exacto en binario, asi que un `EXPECT_NEAR`
 * aca estaria escondiendo un modelo que agrega algo que no deberia.
 */
TEST(InharmonicityTest, WithIdealStringsThePerceptualTargetIsExactlyTheTheoreticalOne) {
    const double freqs[6] = {82.407, 110.000, 146.832, 195.998, 246.942, 329.628};
    const double bs[6] = {0, 0, 0, 0, 0, 0};
    double corr[6] = {9, 9, 9, 9, 9, 9};

    InharmonicityEstimator::perceptualCorrections(freqs, bs, 6, 0, corr);
    for (int i = 0; i < 6; ++i) {
        EXPECT_DOUBLE_EQ(corr[i], 0.0)
            << "cuerda " << i << ": con B=0 la correccion tiene que ser cero exacto";
    }
}

// ---------------------------------------------------------------------------
// 7.4 — el respaldo se computa de la formula, y el test VERIFICA la formula
// ---------------------------------------------------------------------------
/**
 * El numero esperado NO se copia de la implementacion: se escribe la formula del
 * AC aparte, con los mismos parametros. Si alguien cambia el codigo por algo que
 * "da parecido", esto lo agarra.
 */
TEST(InharmonicityTest, TheFallbackIsTheFormulaFromTheAcceptanceCriterion) {
    const StringPhysics p{200e9, 0.43e-3, 75.0, 0.648};

    const double pi = 3.14159265358979323846;
    const double expected = pi * pi * pi * p.youngModulusPa *
                            std::pow(p.coreDiameterM, 4.0) /
                            (64.0 * p.tensionN * p.scaleLengthM * p.scaleLengthM);

    const double got = InharmonicityEstimator::physicsB(p);
    RecordProperty("fallback_B_guitarra_6a", std::to_string(got));
    EXPECT_NEAR(got, expected, expected * 1e-12);

    // Y ademas tiene que caer en el rango PUBLICADO, o la formula estara bien y
    // los parametros mal: prima ~1e-5, bordona gruesa ~5e-4.
    EXPECT_GT(got, 1e-5);
    EXPECT_LT(got, 5e-4);
}

// ---------------------------------------------------------------------------
// 7.5 · AC-001.11 — sin convergencia NO se bloquea la lectura
// ---------------------------------------------------------------------------
TEST(InharmonicityTest, WhenBCannotBeMeasuredItSaysSoInsteadOfReportingZero) {
    // Señal demasiado corta para que el strobe cierre ventanas suficientes.
    const auto e = measureB(110.0, 1e-4, kRate / 20);

    EXPECT_FALSE(e.measured())
        << "declaro B medido con una señal que no alcanza — el respaldo nunca se usaria";
    // 🔴 Y el punto de AC-001.11: `b()` sin medicion NO puede pasar por medicion.
    // Cero es un valor PLAUSIBLE (cuerda ideal), asi que la marca es lo que
    // separa "midio 0" de "no midio", y por eso se chequea la marca y no el valor.
    EXPECT_EQ(e.b(), 0.0);

    // El respaldo esta disponible y es utilizable: la lectura no se bloquea.
    const double fallback =
        InharmonicityEstimator::physicsB({200e9, 0.43e-3, 75.0, 0.648});
    EXPECT_GT(fallback, 0.0);
}

// ---------------------------------------------------------------------------
// 7.6 — un B absurdo no puede mover el objetivo a otra nota
// ---------------------------------------------------------------------------
TEST(InharmonicityTest, AnAbsurdBIsSaturatedAndNeverReachesTheNeighbouringNote) {
    const double freqs[3] = {82.407, 110.000, 146.832};
    const double bs[3] = {0.5, 0.5, 0.5};        // basura: 3 ordenes por encima de lo real
    double corr[3] = {0, 0, 0};

    InharmonicityEstimator::perceptualCorrections(freqs, bs, 3, 0, corr);

    for (int i = 0; i < 3; ++i) {
        EXPECT_LE(std::abs(corr[i]), InharmonicityEstimator::kMaxCorrectionCents)
            << "cuerda " << i << ": la correccion se fue del techo declarado";
        // La promesa real, dicha como la entiende un musico:
        EXPECT_LT(std::abs(corr[i]), 50.0)
            << "cuerda " << i << ": la correccion llega a medio semitono y el "
               "afinador estaria apuntando a otra nota";
    }
}

// ---------------------------------------------------------------------------
// 7.7 — mas B ⇒ mas correccion, y para el lado correcto
// ---------------------------------------------------------------------------
/**
 * Es el test que 7.8 pide matar con un mutante que invierta el signo, asi que la
 * direccion se afirma en ABSOLUTO y no como "difieren": la leccion de 6.5.
 *
 * El sentido correcto sale del modelo, no del gusto: al igualar el parcial p de
 * la grave con el q de la aguda, la grave aporta `p²·B` y la aguda `q²·B`. Con
 * p > q y B parecidos, el termino de la grave manda, asi que la aguda tiene que
 * subir. Una bordona rigida empuja a sus vecinas hacia arriba.
 */
TEST(InharmonicityTest, AStifferBassPushesItsNeighbourSharpAndDoesSoMoreThanATrebleString) {
    const double freqs[2] = {82.407, 110.000};   // cuarta justa
    double corrStiff[2] = {0, 0};
    double corrSoft[2]  = {0, 0};

    const double stiff[2] = {3.0e-4, 1.0e-5};    // bordona rigida
    const double soft[2]  = {1.0e-5, 1.0e-5};    // dos cuerdas finas

    InharmonicityEstimator::perceptualCorrections(freqs, stiff, 2, 0, corrStiff);
    InharmonicityEstimator::perceptualCorrections(freqs, soft, 2, 0, corrSoft);

    RecordProperty("corr_stiff_cents", std::to_string(corrStiff[1]));
    RecordProperty("corr_soft_cents", std::to_string(corrSoft[1]));

    EXPECT_GT(corrStiff[1], 0.0)
        << "una bordona rigida tiene que empujar a su vecina HACIA ARRIBA, y dio "
        << corrStiff[1];
    EXPECT_GT(corrStiff[1], corrSoft[1])
        << "mas inarmonicidad tiene que dar mas correccion: " << corrStiff[1]
        << " contra " << corrSoft[1];
}

// ---------------------------------------------------------------------------
// La cadena: la correccion es propiedad del CONJUNTO
// ---------------------------------------------------------------------------
/**
 * AC-001.10 dice que el offset "no es propiedad de una cuerda sola". Este test lo
 * afirma como comportamiento: cambiar el B de UNA cuerda mueve la correccion de
 * todas las que estan mas lejos de la referencia que ella, y no mueve las que
 * quedan del otro lado.
 */
TEST(InharmonicityTest, ChangingOneStringMovesEveryStringFurtherFromTheReference) {
    const double freqs[4] = {82.407, 110.000, 146.832, 195.998};
    double before[4] = {0, 0, 0, 0};
    double after[4]  = {0, 0, 0, 0};

    double bs[4] = {1e-5, 1e-5, 1e-5, 1e-5};
    InharmonicityEstimator::perceptualCorrections(freqs, bs, 4, 0, before);

    bs[1] = 3e-4;                                  // se cambia SOLO la segunda
    InharmonicityEstimator::perceptualCorrections(freqs, bs, 4, 0, after);

    EXPECT_DOUBLE_EQ(after[0], before[0]) << "la referencia no se mueve nunca";
    EXPECT_NE(after[1], before[1]) << "la cuerda que cambio tiene que moverse";
    EXPECT_NE(after[2], before[2])
        << "la cuerda 2 no se movio: la correccion se esta calculando cuerda por "
           "cuerda en vez de acumularse por la cadena de intervalos";
    EXPECT_NE(after[3], before[3]) << "idem la cuerda 3";
}

// ---------------------------------------------------------------------------
// 7.13 — golden: B de respaldo y correccion, por cuerda y por instrumento
// ---------------------------------------------------------------------------
/**
 * Congela lo que un cambio de modelo movería sin que ningun test de propiedad se
 * entere: los CENTS concretos que el afinador le va a pedir a cada cuerda. Las
 * propiedades (signo, techo, monotonia) ya tienen sus tests; esto es la magnitud.
 *
 * Los B salen de la formula con calibre de nucleo y escala declarados por
 * instrumento — no hay una sola constante sin procedencia.
 */
TEST(GoldenInharmonicity, ThePerceptualCorrectionsMatchTheirGolden) {
    using wma::analysis::InharmonicityEstimator;
    struct Str { const char* name; double hz; StringPhysics phys; };
    struct Inst { const char* name; std::vector<Str> strings; int reference; };

    // E: acero de cuerda 200 GPa, nylon 5 GPa. T y L, tipicos del instrumento.
    // d es el NUCLEO: en las entorchadas el bobinado suma masa, no rigidez.
    const std::vector<Inst> kInstruments = {
        {"guitarra", {
            {"E2", 82.407,  {200e9, 0.43e-3, 75.0, 0.648}},
            {"A2", 110.000, {200e9, 0.36e-3, 75.0, 0.648}},
            {"D3", 146.832, {200e9, 0.30e-3, 75.0, 0.648}},
            {"G3", 195.998, {200e9, 0.25e-3, 75.0, 0.648}},
            {"B3", 246.942, {200e9, 0.41e-3, 75.0, 0.648}},   // plana
            {"E4", 329.628, {200e9, 0.25e-3, 75.0, 0.648}},   // plana
        }, 0},
        {"bajo", {
            {"E1", 41.203, {200e9, 0.66e-3, 180.0, 0.864}},
            {"A1", 55.000, {200e9, 0.58e-3, 180.0, 0.864}},
            {"D2", 73.416, {200e9, 0.50e-3, 180.0, 0.864}},
            {"G2", 97.999, {200e9, 0.43e-3, 180.0, 0.864}},
        }, 0},
        {"ukelele", {
            {"C4", 261.626, {5e9, 0.71e-3, 60.0, 0.380}},
            {"E4", 329.628, {5e9, 0.61e-3, 60.0, 0.380}},
            {"A4", 440.000, {5e9, 0.56e-3, 60.0, 0.380}},
        }, 0},
    };

    std::vector<golden::Sample> rows;
    for (const auto& inst : kInstruments) {
        const int n = static_cast<int>(inst.strings.size());
        std::vector<double> freqs(static_cast<size_t>(n));
        std::vector<double> bs(static_cast<size_t>(n));
        std::vector<double> corr(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            freqs[static_cast<size_t>(i)] = inst.strings[static_cast<size_t>(i)].hz;
            bs[static_cast<size_t>(i)] =
                InharmonicityEstimator::physicsB(inst.strings[static_cast<size_t>(i)].phys);
        }
        InharmonicityEstimator::perceptualCorrections(freqs.data(), bs.data(), n,
                                                      inst.reference, corr.data());
        for (int i = 0; i < n; ++i) {
            // `cents` lleva B escalado a 1e6 para que el diff sea legible; la
            // tolerancia del comparador es 1e-3 y un B crudo de 1e-5 quedaria
            // por debajo del ruido de la comparacion.
            rows.push_back({std::string(inst.name) + " " + inst.strings[static_cast<size_t>(i)].name,
                            inst.strings[static_cast<size_t>(i)].hz,
                            bs[static_cast<size_t>(i)] * 1e6,
                            corr[static_cast<size_t>(i)]});
        }
    }

    ASSERT_EQ(rows.size(), 13u);
    golden::checkOrRegen("inharmonicity_corrections", kRate, 0, rows,
                         {"cuerdaHz", "B_x1e6", "correccionCents"}, "REQ-001 S7");
}

}  // namespace
}  // namespace wma_test
