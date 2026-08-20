/**
 * test_intonation.cpp — REQ-001 S9.
 *
 * La etapa no trae DSP: trae un PROTOCOLO. Asi que los tests miran lo que un
 * protocolo puede romper —comparar cosas distintas, comparar cosas a medio
 * medir, seguir mostrando lo viejo— y no la exactitud, que es la de S6 y ya esta
 * medida alla.
 */

#include "support/AnalysisGolden.h"
#include "support/SyntheticSignal.h"

#include "IntonationMode.h"
#include "StrobeTracker.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace wma_test {
namespace {

using wma::analysis::IntonationMode;
using wma::analysis::StrobeTracker;

constexpr int kRate = 48000;
constexpr int kThreeSeconds = 3 * kRate;
constexpr double kTolerance = 0.1;

void feed(StrobeTracker& t, const std::vector<float>& sig, int block = 512) {
    int i = 0;
    const int n = static_cast<int>(sig.size());
    while (i < n) {
        const int take = std::min(block, n - i);
        t.process(sig.data() + i, take);
        i += take;
    }
}

/// Una medida convergida contra `targetHz`, con la cuerda `cents` desafinada.
/// `scale` simula un reloj de ADC corrido (1 + ppm/1e6).
StrobeTracker measured(double targetHz, double cents, double scale = 1.0,
                       int frames = kThreeSeconds) {
    StrobeTracker t;
    t.prepare(kRate);
    t.setTarget(targetHz);
    feed(t, inharmonicString(detune(targetHz, cents) * scale, 0.0, 4, kRate, frames));
    return t;
}

// ---------------------------------------------------------------------------
// 9.1 · AC-001.16 — la diferencia es exacta
// ---------------------------------------------------------------------------
TEST(IntonationTest, TheReportedDifferenceMatchesTheTwoKnownDeviations) {
    // Octava del 12º traste de la 6ª de guitarra: E2 al aire ⇒ objetivo 2·E2.
    const double target = 2.0 * 82.407;

    struct Case { const char* name; double harmonic; double fretted; };
    const Case kCases[] = {
        {"pisada 3 cents alta", -1.0, +2.0},
        {"pisada 4 cents baja", +2.0, -2.0},
        {"intonacion perfecta", +1.5, +1.5},
    };

    for (const auto& c : kCases) {
        IntonationMode m;
        ASSERT_TRUE(m.capture(IntonationMode::kHarmonic, measured(target, c.harmonic)))
            << c.name;
        ASSERT_TRUE(m.capture(IntonationMode::kFretted, measured(target, c.fretted)))
            << c.name;

        ASSERT_TRUE(m.hasResult()) << c.name;
        const double want = c.fretted - c.harmonic;
        const double got = m.differenceCents();
        RecordProperty(std::string("diff_err_") + c.name, std::to_string(std::abs(got - want)));
        EXPECT_NEAR(got, want, kTolerance)
            << c.name << ": esperaba " << want << " cents y dio " << got;
    }
}

// ---------------------------------------------------------------------------
// 9.2 — no hay resultado hasta que las DOS convergieron
// ---------------------------------------------------------------------------
TEST(IntonationTest, ThereIsNoReadingUntilBothMeasurementsHaveConverged) {
    const double target = 2.0 * 82.407;
    IntonationMode m;

    EXPECT_EQ(m.state(), IntonationMode::kNeedHarmonic);
    EXPECT_FALSE(m.hasResult());
    EXPECT_TRUE(std::isnan(m.differenceCents()))
        << "sin medidas devolvio un numero; cero seria 'intonacion perfecta'";

    // Una señal corta NO converge, y por lo tanto NO se acepta.
    StrobeTracker tooShort = measured(target, 2.0, 1.0, kRate / 20);
    ASSERT_FALSE(tooShort.converged()) << "el fixture no representa lo que dice";
    EXPECT_FALSE(m.capture(IntonationMode::kHarmonic, tooShort))
        << "acepto una medida sin converger";
    EXPECT_EQ(m.state(), IntonationMode::kNeedHarmonic);

    // Con la primera buena, sigue sin haber resultado.
    ASSERT_TRUE(m.capture(IntonationMode::kHarmonic, measured(target, -1.0)));
    EXPECT_EQ(m.state(), IntonationMode::kNeedFretted);
    EXPECT_FALSE(m.hasResult());
    EXPECT_TRUE(std::isnan(m.differenceCents()))
        << "con una sola medida ya entregaba un numero";

    ASSERT_TRUE(m.capture(IntonationMode::kFretted, measured(target, +2.0)));
    EXPECT_EQ(m.state(), IntonationMode::kReady);
    EXPECT_FALSE(std::isnan(m.differenceCents()));
}

// ---------------------------------------------------------------------------
// 9.3 — si una caduca, el resultado caduca
// ---------------------------------------------------------------------------
TEST(IntonationTest, LosingASignalExpiresTheResultInsteadOfShowingTheLastGoodOne) {
    const double target = 2.0 * 110.0;
    IntonationMode m;
    ASSERT_TRUE(m.capture(IntonationMode::kHarmonic, measured(target, -1.0)));
    ASSERT_TRUE(m.capture(IntonationMode::kFretted, measured(target, +2.0)));
    ASSERT_TRUE(m.hasResult());

    m.invalidate();

    EXPECT_FALSE(m.hasResult());
    EXPECT_EQ(m.state(), IntonationMode::kNeedHarmonic)
        << "tras caducar no volvio a pedir la primera medida";
    EXPECT_TRUE(std::isnan(m.differenceCents()))
        << "sigue entregando el ultimo valido como si fuera actual — y con ese "
           "numero el usuario mueve un saddle";
}

// ---------------------------------------------------------------------------
// 9.4 — dos cuerdas distintas no se restan
// ---------------------------------------------------------------------------
TEST(IntonationTest, TwoDifferentStringsAreReportedInsteadOfSubtracted) {
    IntonationMode m;
    ASSERT_TRUE(m.capture(IntonationMode::kHarmonic, measured(2.0 * 82.407, -1.0)));
    ASSERT_TRUE(m.capture(IntonationMode::kFretted,  measured(2.0 * 110.000, +2.0)));

    EXPECT_FALSE(m.sameString());
    EXPECT_EQ(m.state(), IntonationMode::kStringMismatch)
        << "no detecto que las dos medidas son de cuerdas distintas";
    EXPECT_FALSE(m.hasResult());
    EXPECT_TRUE(std::isnan(m.differenceCents()))
        << "resto el armonico de una cuerda con la pisada de OTRA: el numero "
           "existe, es preciso, y no significa nada";
}

// ---------------------------------------------------------------------------
// 9.5 — el error de reloj de modo comun se cancela
// ---------------------------------------------------------------------------
/**
 * Es el mismo experimento que 2.3, ahora sobre el resultado del PRODUCTO: las dos
 * señales corridas por el mismo 50 ppm tienen que dar exactamente la misma
 * diferencia. Es la razon por la que este modo tiene la exactitud relativa entera
 * del strobe.
 */
TEST(IntonationTest, ACommonClockErrorCancelsOutOfTheDifference) {
    const double target = 2.0 * 146.832;
    const double kFiftyPpm = 1.0 + 50.0e-6;

    IntonationMode plain;
    ASSERT_TRUE(plain.capture(IntonationMode::kHarmonic, measured(target, -1.0)));
    ASSERT_TRUE(plain.capture(IntonationMode::kFretted,  measured(target, +2.0)));
    ASSERT_TRUE(plain.hasResult());

    IntonationMode skewed;
    ASSERT_TRUE(skewed.capture(IntonationMode::kHarmonic,
                               measured(target, -1.0, kFiftyPpm)));
    ASSERT_TRUE(skewed.capture(IntonationMode::kFretted,
                               measured(target, +2.0, kFiftyPpm)));
    ASSERT_TRUE(skewed.hasResult());

    const double drift = std::abs(skewed.differenceCents() - plain.differenceCents());
    RecordProperty("clock_drift_leak_cents", std::to_string(drift));
    EXPECT_LT(drift, 0.01)
        << "50 ppm de error de reloj se filtraron a la diferencia (" << drift
        << " cents): el modo comun no se esta cancelando";

    // Y para que el test signifique algo: 50 ppm SI mueven cada medida por
    // separado. Si no, estaria comparando dos ceros.
    const double perMeasurement =
        std::abs(skewed.capturedCents(IntonationMode::kHarmonic) -
                 plain.capturedCents(IntonationMode::kHarmonic));
    RecordProperty("clock_shift_per_measurement_cents", std::to_string(perMeasurement));
    EXPECT_GT(perMeasurement, 0.05)
        << "50 ppm no movieron ni una medida sola: el fixture no prueba nada";
}

// ---------------------------------------------------------------------------
// 9.6 — la correccion por inarmonicidad no se puede aplicar a una sola
// ---------------------------------------------------------------------------
/**
 * 🔴 ESTE TEST AFIRMA UNA PROPIEDAD DEL DISEÑO, NO DEL CALCULO.
 *
 * Las dos medidas son de la MISMA cuerda contra el MISMO objetivo, asi que
 * cualquier correccion que dependa de la cuerda las desplaza a las dos por igual
 * y se cancela en la resta. Aplicarla a una sola seria un sesgo puro.
 *
 * `IntonationMode` no expone ninguna forma de aplicarla por slot: guarda cents y
 * objetivo, y la resta es simetrica. La propiedad se cumple **por construccion**,
 * y lo que este test fija es que siga siendo asi — si alguien agregara un
 * parametro por slot, el segundo bloque dejaria de compilar o de pasar.
 */
TEST(IntonationTest, AnyPerStringCorrectionCancelsBecauseBothSlotsShareTheString) {
    const double target = 2.0 * 82.407;
    IntonationMode m;
    ASSERT_TRUE(m.capture(IntonationMode::kHarmonic, measured(target, -1.0)));
    ASSERT_TRUE(m.capture(IntonationMode::kFretted,  measured(target, +2.0)));
    ASSERT_TRUE(m.hasResult());

    // Las dos medidas comparten el objetivo: cualquier funcion del objetivo les
    // aplica identico.
    EXPECT_DOUBLE_EQ(m.capturedTargetHz(IntonationMode::kHarmonic),
                     m.capturedTargetHz(IntonationMode::kFretted))
        << "los dos slots no comparten objetivo, y entonces una correccion por "
           "cuerda NO se cancelaria";

    // Y la resta es simetrica: sumarle lo mismo a las dos no la mueve.
    const double before = m.differenceCents();
    const double bias = 7.3;   // una correccion cualquiera
    const double after = (m.capturedCents(IntonationMode::kFretted) + bias) -
                         (m.capturedCents(IntonationMode::kHarmonic) + bias);
    EXPECT_DOUBLE_EQ(after, before);
}

// ---------------------------------------------------------------------------
// 9.10 — golden del ciclo completo
// ---------------------------------------------------------------------------
TEST(GoldenIntonation, TheFullTwoMeasurementCycleMatchesItsGolden) {
    struct Case { const char* name; double openHz; double harmonic; double fretted; };
    const Case kCases[] = {
        {"guitarra E2", 82.407,  -1.0, +2.0},
        {"guitarra A2", 110.000, +0.5, +3.5},
        {"guitarra E4", 329.628, -2.0, -0.5},
        {"bajo E1",     41.203,  +1.0, +5.0},
    };

    std::vector<golden::Sample> rows;
    for (const auto& c : kCases) {
        const double target = 2.0 * c.openHz;
        IntonationMode m;
        m.capture(IntonationMode::kHarmonic, measured(target, c.harmonic));
        m.capture(IntonationMode::kFretted,  measured(target, c.fretted));
        ASSERT_TRUE(m.hasResult()) << c.name;
        rows.push_back({std::string(c.name), target,
                        m.differenceCents(), m.capturedCents(IntonationMode::kHarmonic)});
    }

    ASSERT_EQ(rows.size(), 4u);
    golden::checkOrRegen("intonation_cycle", kRate, 0, rows,
                         {"objetivoHz", "diferenciaCents", "armonicoCents"}, "REQ-001 S9");
}

}  // namespace
}  // namespace wma_test
