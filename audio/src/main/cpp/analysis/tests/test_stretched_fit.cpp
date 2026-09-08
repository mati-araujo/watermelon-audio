/**
 * test_stretched_fit.cpp — REQ-035 S1 (AC-035.2). LA SENSIBILIDAD DEL AJUSTE DE LA SERIE ESTIRADA
 * a lo que el modelo NO explica, medida sobre sintesis ANTES de tocar el ajuste.
 *
 * El strobe ajusta `cents_n = C + 600·log2(1+B·n²)` a los parciales admitidos y publica C. Sobre
 * una serie exacta eso da 0,1 c (el contrato). La pregunta de REQ-035 es que pasa cuando un parcial
 * NO sigue la serie —un sample puede traer uno corrido, uno ausente, uno con batido entre capas—:
 * cuanto se mueve C, y si σ lo delata. Se mide con el estimulo controlado, y el control (la serie
 * exacta a 0,1 c) es lo que dice que el instrumento mide.
 *
 * LO QUE SE AFIRMA, Y POR QUE ESO Y NO MAS
 * ----------------------------------------
 * · El control: |C − desafinacion| ≤ 0,1 c, CONVERGIDO. Sin esto ninguna otra fila significa nada.
 * · Un parcial corrido: C se mueve (la sensibilidad existe y se imprime) y **σ sube por encima del
 *   umbral de convergencia**: es AC-027.5 —un parcial que la serie no puede explicar infla σ y apaga
 *   CONVERGIDO— ejercido con el residuo que REQ-027 nunca genero (sus estimulos eran todos series).
 * · Un parcial ausente: no es un sesgo. El piso de energia de REQ-027 lo deja afuera y el ajuste
 *   sigue en 0,1 c con los tres que quedan.
 * · Un parcial con batido: la lectura es HONESTA — o esta dentro de 0,1 c, o no esta CONVERGIDA.
 *   Un afinador puede no saber; lo que no puede es afirmar un numero equivocado.
 *
 * Los numeros de sensibilidad (cuanto se mueve C por cada perturbacion) se IMPRIMEN y no se
 * afirman: son la medicion de S1, y un umbral sobre ellos hoy seria un numero elegido a ojo.
 */

#include "support/SyntheticSignal.h"

#include "StrobeTracker.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace wma_test {
namespace {

using wma::analysis::StrobeTracker;

constexpr int kRate = 48000;
constexpr int kSeconds = 3;
constexpr double kF0 = 146.83238;        // D3: parciales hasta 587 Hz, lejos de Nyquist y del rango
constexpr double kB = 1.0e-4;            // una cuerda real, ni la prima ni la bordona
constexpr double kProbeCents = 1.0;      // DIEZ veces la tolerancia: un estimador que devuelva 0 falla
constexpr double kToleranceCents = 0.1;  // el contrato

struct Component { double hz; double amp; };

/// Suma de senos arbitrarios: es lo que permite CORRER un parcial o ponerle batido sin tocar el
/// generador compartido, que genera series y tiene que seguir generando series.
std::vector<float> components(const std::vector<Component>& cs, int frames) {
    std::vector<float> out(static_cast<size_t>(frames), 0.0f);
    for (const Component& c : cs) {
        const double dp = 2.0 * M_PI * c.hz / kRate;
        double p = 0.0;
        for (int i = 0; i < frames; ++i) {
            out[static_cast<size_t>(i)] += static_cast<float>(c.amp * std::sin(p));
            p += dp;
            if (p >= 2.0 * M_PI) p -= 2.0 * M_PI;
        }
    }
    return out;
}

/// El parcial n de la cuerda desafinada `cents`, con inarmonicidad B: `n·f0'·√(1+B·n²)`.
double partialHz(int n, double cents) {
    return n * detune(kF0, cents) * std::sqrt(1.0 + kB * n * n);
}

struct Reading {
    double C = NAN;
    double sigma = NAN;
    bool converged = false;
    int used = 0;
    double partial[StrobeTracker::kPartials] = {NAN, NAN, NAN, NAN};
};

/// Como en produccion: objetivo en el nominal y el CONTROL externo en la frecuencia verdadera.
Reading measure(const std::vector<float>& sig) {
    StrobeTracker t;
    t.prepare(kRate);
    t.setTarget(kF0);
    t.setCoarseFrequencyHz(detune(kF0, kProbeCents));
    const int n = static_cast<int>(sig.size());
    for (int i = 0; i < n; i += 512) t.process(sig.data() + i, std::min(512, n - i));
    Reading r;
    r.C = t.cents();
    r.sigma = t.uncertaintyCents();
    r.converged = t.converged();
    r.used = t.partialsUsed();
    for (int i = 0; i < StrobeTracker::kPartials; ++i)
        r.partial[i] = t.partialHasMeasurement(i) ? t.partialCents(i) : NAN;
    return r;
}

void print(const char* label, const Reading& r) {
    std::printf("  %-34s C=%+7.3f  dC=%+7.3f  sigma=%8.5f  %s  k=%d  p=[%+.2f %+.2f %+.2f %+.2f]\n",
                label, r.C, r.C - kProbeCents, r.sigma, r.converged ? "CONVERGIDO" : "midiendo  ",
                r.used, r.partial[0], r.partial[1], r.partial[2], r.partial[3]);
}

std::vector<Component> series(double cents) {
    std::vector<Component> cs;
    for (int n = 1; n <= 4; ++n) cs.push_back({partialHz(n, cents), 0.5 / n});
    return cs;
}

// ---------------------------------------------------------------------------
// AC-035.2 — la sensibilidad, con su control
// ---------------------------------------------------------------------------
TEST(StretchedFitSensitivity, TheExactSeriesIsTheControlAndMeetsTheContract) {
    const Reading r = measure(components(series(kProbeCents), kSeconds * kRate));
    std::printf("\n  [REQ-035] sensibilidad del ajuste (D3, B=%.0e, +%.1f c)\n", kB, kProbeCents);
    print("serie exacta (control)", r);
    ASSERT_TRUE(r.converged) << "el control no convergio: nada de lo que sigue significa algo";
    EXPECT_NEAR(r.C, kProbeCents, kToleranceCents) << "el control no cumple el contrato de 0,1 c";
    EXPECT_EQ(r.used, 4);
}

TEST(StretchedFitSensitivity, AShiftedPartialMovesCAndInflatesSigmaPastConvergence) {
    for (int shifted = 2; shifted <= 4; ++shifted) {
        std::vector<Component> cs = series(kProbeCents);
        cs[static_cast<size_t>(shifted - 1)].hz = detune(cs[static_cast<size_t>(shifted - 1)].hz, 10.0);
        const Reading r = measure(components(cs, kSeconds * kRate));
        char label[48];
        std::snprintf(label, sizeof label, "parcial %d corrido +10 c", shifted);
        print(label, r);
        // La sensibilidad EXISTE (se imprime cuanto) y σ la delata: un parcial que la serie
        // estirada no puede explicar deja residuos, y los residuos son σ (REQ-027 S2).
        EXPECT_GT(std::fabs(r.C - kProbeCents), kToleranceCents)
            << "el parcial " << shifted << " corrido +10 c no movio C: el ajuste no lo esta usando";
        EXPECT_FALSE(r.converged)
            << "el parcial " << shifted << " corrido +10 c dejo CONVERGIDO con C = " << r.C
            << ": σ no vio el residuo (AC-027.5)";
        EXPECT_GT(r.sigma, StrobeTracker::kConvergedUncertaintyCents);
    }
}

TEST(StretchedFitSensitivity, AMissingPartialIsNotABias) {
    for (int missing = 2; missing <= 4; ++missing) {
        std::vector<Component> cs = series(kProbeCents);
        cs[static_cast<size_t>(missing - 1)].amp = 0.0;
        const Reading r = measure(components(cs, kSeconds * kRate));
        char label[48];
        std::snprintf(label, sizeof label, "parcial %d ausente", missing);
        print(label, r);
        EXPECT_TRUE(r.converged) << "sin el parcial " << missing << " no convergio";
        EXPECT_NEAR(r.C, kProbeCents, kToleranceCents)
            << "el parcial " << missing << " ausente sesgo C: el piso de REQ-027 no lo dejo afuera";
        EXPECT_EQ(r.used, 3) << "el bin vacio del parcial " << missing << " entro al ajuste";
    }
}

TEST(StretchedFitSensitivity, ABeatingPartialLeavesTheReadingHonest) {
    for (int beating = 1; beating <= 4; ++beating) {
        std::vector<Component> cs = series(kProbeCents);
        const Component orig = cs[static_cast<size_t>(beating - 1)];
        // Dos capas del mismo parcial a ±0,25 Hz: batido de 0,5 Hz, amplitud total igual.
        cs[static_cast<size_t>(beating - 1)] = {orig.hz - 0.25, 0.5 * orig.amp};
        cs.push_back({orig.hz + 0.25, 0.5 * orig.amp});
        const Reading r = measure(components(cs, kSeconds * kRate));
        char label[48];
        std::snprintf(label, sizeof label, "parcial %d con batido 0,5 Hz", beating);
        print(label, r);
        const bool honest = !r.converged || std::fabs(r.C - kProbeCents) <= kToleranceCents;
        EXPECT_TRUE(honest) << "con batido en el parcial " << beating << " publico CONVERGIDO C = "
                            << r.C << " (error " << r.C - kProbeCents << " c)";
    }
    std::printf("\n");
}

}  // namespace
}  // namespace wma_test
