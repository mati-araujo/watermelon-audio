/**
 * test_fast_mode.cpp — REQ-001 S5.
 *
 * La etapa es una MAQUINA DE ESTADOS, asi que los tests la alimentan con
 * secuencias de detecciones gruesas —que es lo que le llega de S4— en vez de con
 * audio. La exactitud fina es de S6 y ya esta medida alla; lo que se prueba aca
 * es a QUE CUERDA dice que te referis, y cuando cambia de opinion.
 */

#include "support/AnalysisGolden.h"
#include "support/SyntheticSignal.h"

#include "FastModeTracker.h"
#include "StrobeTracker.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace wma_test {
namespace {

using wma::analysis::FastModeTracker;

/// La ventana de analisis, en segundos: cada `update()` es una lectura nueva.
constexpr double kUpdateSeconds = 4096.0 / 48000.0;

/// Guitarra estandar, EN ORDEN DE CUERDA (6ª a 1ª).
const std::vector<double>& guitar() {
    static const std::vector<double> k = {82.407, 110.000, 146.832,
                                          195.998, 246.942, 329.628};
    return k;
}

/// Ukelele high-G: la cuerda 4 es MAS AGUDA que la 3. El orden es de CUERDA.
const std::vector<double>& ukuleleHighG() {
    static const std::vector<double> k = {391.995, 261.626, 329.628, 440.000};
    return k;
}

FastModeTracker withCandidates(const std::vector<double>& c) {
    FastModeTracker t;
    t.setCandidates(c.data(), static_cast<int>(c.size()));
    return t;
}

/// Alimenta un barrido de `fromHz` a `toHz` en `seconds`, y devuelve los indices
/// enganchados en cada lectura.
std::vector<int> sweep(FastModeTracker& t, double fromHz, double toHz, double seconds) {
    const int steps = std::max(2, static_cast<int>(seconds / kUpdateSeconds));
    std::vector<int> locks;
    for (int i = 0; i <= steps; ++i) {
        const double f = fromHz * std::pow(toHz / fromHz,
                                           static_cast<double>(i) / steps);
        t.update(f, 0.95);
        locks.push_back(t.lockedIndex());
    }
    return locks;
}

/// Sostiene una nota quieta durante `n` lecturas.
void hold(FastModeTracker& t, double hz, int n, double clarity = 0.95) {
    for (int i = 0; i < n; ++i) t.update(hz, clarity);
}

// ---------------------------------------------------------------------------
// 5.1 — el caso que define la etapa: barrido desde floja
// ---------------------------------------------------------------------------
/**
 * 🔴 EL ESCENARIO LITERAL DE LA TAREA NO EJERCITA LA HISTERESIS, Y ESTA MEDIDO.
 *
 * "Una cuerda que sube de 60 a 82,41 Hz enganchada a E2": en guitarra, el
 * candidato mas cercano a lo largo de TODO ese trayecto es E2 y no cambia nunca
 * (verificado: 0 cambios en 41 puntos). O sea que ese barrido pasa incluso con la
 * histeresis anulada — que es exactamente lo que 5.9 advierte.
 *
 * Se conserva porque sigue siendo una regresion valida, pero el caso que ATA es
 * el segundo: una D3 desde floja **pasa por encima de A2** (a 127 Hz el mas
 * cercano ya es A2). Ahi es donde un afinador sin histeresis salta de cuerda.
 */
TEST(FastModeTest, TuningUpFromSlackNeverChangesTheTargetUnderneathYou) {
    // El literal de la tarea: E2 desde floja.
    {
        auto t = withCandidates(guitar());
        // El musico ELIGE la 6ª: a 60 Hz esta a 548 cents de E2, o sea lejos de
        // todo, y el enganche automatico —correctamente— no la agarraria.
        t.lockTo(0);
        const auto locks = sweep(t, 60.0, 82.407, 6.0);
        for (size_t i = 0; i < locks.size(); ++i) {
            EXPECT_EQ(locks[i], 0) << "lectura " << i << ": solto E2 durante el barrido";
        }
    }

    // El que de verdad ata: D3 desde floja, cruzando A2 por el camino.
    {
        auto d3 = withCandidates(guitar());
        d3.lockTo(2);
        ASSERT_EQ(d3.lockedIndex(), 2);
        const auto locks = sweep(d3, 100.0, 146.832, 6.0);
        int switches = 0;
        for (int l : locks) if (l != 2) ++switches;
        EXPECT_EQ(switches, 0)
            << "salto de cuerda " << switches << " veces mientras se afinaba la D3 "
               "desde floja — paso por A2 y se lo quedo";
    }
}

// ---------------------------------------------------------------------------
// 5.2 · AC-001.5 — cambio real de cuerda CON silencio: ≤ 150 ms
// ---------------------------------------------------------------------------
TEST(FastModeTest, ARealStringChangeWithSilenceSwitchesWithinTheBudget) {
    auto t = withCandidates(guitar());
    hold(t, 82.407, 5);
    ASSERT_EQ(t.lockedIndex(), 0);

    // Silencio: suelta.
    hold(t, 0.0, FastModeTracker::kSilentUpdatesToRelease, 0.0);
    EXPECT_EQ(t.state(), FastModeTracker::kNoSignal);
    EXPECT_EQ(t.lockedIndex(), -1);

    // Nueva cuerda: cuantas lecturas hasta engancharla.
    int updates = 0;
    while (t.lockedIndex() != 1 && updates < 20) {
        t.update(110.0, 0.95);
        ++updates;
    }
    const double ms = updates * kUpdateSeconds * 1000.0;
    RecordProperty("switch_with_silence_ms", std::to_string(ms));
    EXPECT_EQ(t.lockedIndex(), 1) << "no engancho a la cuerda nueva";
    EXPECT_LE(ms, 150.0) << "tardo " << ms << " ms en conmutar";
}

// ---------------------------------------------------------------------------
// 5.3 — cambio real SIN silencio: la histeresis no puede ser una trampa
// ---------------------------------------------------------------------------
TEST(FastModeTest, ARealStringChangeWithoutSilenceStillSwitches) {
    auto t = withCandidates(guitar());
    hold(t, 82.407, 5);
    ASSERT_EQ(t.lockedIndex(), 0);

    int updates = 0;
    while (t.lockedIndex() != 1 && updates < 20) {
        t.update(110.0, 0.95);      // salto directo, sin silencio
        ++updates;
    }
    const double ms = updates * kUpdateSeconds * 1000.0;
    RecordProperty("switch_without_silence_ms", std::to_string(ms));
    EXPECT_EQ(t.lockedIndex(), 1)
        << "se quedo pegado a E2 con el musico tocando A2: la histeresis se "
           "convirtio en una trampa";
    EXPECT_LE(ms, 150.0) << "tardo " << ms << " ms";
}

// ---------------------------------------------------------------------------
// 5.5 · AC-001.21 — sin cuerda en rango, la verdad cromatica
// ---------------------------------------------------------------------------
TEST(FastModeTest, WithNoStringInRangeItReportsTheChromaticNoteAndSaysItIsNotLocked) {
    auto t = withCandidates(guitar());
    // 170 Hz: a 240 cents de D3 y a 262 de G3 — ninguna dentro de kLockCents.
    hold(t, 170.0, 4);

    EXPECT_EQ(t.state(), FastModeTracker::kNoLock)
        << "dijo que estaba enganchado a una cuerda que no esta en rango";
    EXPECT_EQ(t.lockedIndex(), -1);
    EXPECT_TRUE(std::isnan(t.centsFromTarget()));

    // F3 = 174,614. Es la cromatica mas cercana a 170.
    EXPECT_NEAR(t.chromaticHz(), 174.614, 0.01)
        << "la nota cromatica reportada no es la mas cercana";
    EXPECT_LT(std::abs(t.centsFromChromatic()), 100.0);
}

// ---------------------------------------------------------------------------
// 5.6 · AC-001.2 — ≤ 0,5 cent en toda cuerda de todo instrumento
// ---------------------------------------------------------------------------
/**
 * 🔴 EL NUMERO NO SALE DEL TRACKER, Y ESO ES EL DISEÑO.
 *
 * `centsFromTarget()` es GRUESO: viene de S4, con ±50 cents de presupuesto. El
 * ±0,5 que pide el AC sale de apuntar el strobe de S6 al objetivo que el tracker
 * ENGANCHO. Este test recorre el camino entero del producto —enganchar, reapuntar,
 * medir— porque probar las dos mitades por separado dejaria justo el empalme sin
 * probar, que es donde S1+S2 se habian quedado sin producir una lectura.
 */
TEST(FastModeTest, TheFineReadingThroughTheLockedTargetMeetsHalfACent) {
    struct Inst { const char* name; const std::vector<double>* strings; };
    const Inst kInstruments[] = {
        {"guitarra", &guitar()},
        {"ukelele high-G", &ukuleleHighG()},
    };
    constexpr double kProbeCents = 1.0;

    for (const auto& inst : kInstruments) {
        for (size_t i = 0; i < inst.strings->size(); ++i) {
            const double hz = (*inst.strings)[i];

            auto t = withCandidates(*inst.strings);
            hold(t, detune(hz, kProbeCents), 4);
            ASSERT_EQ(t.lockedIndex(), static_cast<int>(i))
                << inst.name << " cuerda " << i << ": engancho a otra";

            wma::analysis::StrobeTracker s;
            s.prepare(48000);
            s.setTarget(t.lockedTargetHz());          // ← el empalme
            const auto sig = inharmonicString(detune(hz, kProbeCents), 0.0, 4,
                                              48000, 3 * 48000);
            int k = 0;
            while (k < static_cast<int>(sig.size())) {
                const int take = std::min(512, static_cast<int>(sig.size()) - k);
                s.process(sig.data() + k, take);
                k += take;
            }

            ASSERT_TRUE(s.hasMeasurement()) << inst.name << " cuerda " << i;
            const double err = std::abs(s.cents() - kProbeCents);
            RecordProperty(std::string("fine_err_") + inst.name + "_" +
                               std::to_string(i), std::to_string(err));
            EXPECT_LT(err, 0.5)
                << inst.name << " cuerda " << i << " (" << hz << " Hz): error "
                << err << " cents";
        }
    }
}

// ---------------------------------------------------------------------------
// 5.8 — enganche por INDICE, no por vecindad de frecuencia
// ---------------------------------------------------------------------------
/**
 * En un ukelele high-G la cuerda 1 (G4, 392) es mas aguda que la 2 (C4) y que la
 * 3 (E4). Reportar "la mas cercana en Hz" **como si fuera el numero de cuerda**
 * es el bug de AC-001.15 visto desde el enganche: le diria al ukelelista que su
 * cuarta esta una octava baja.
 */
TEST(FastModeTest, OnAReentrantInstrumentItLocksByStringIndexNotByPitchOrder) {
    auto t = withCandidates(ukuleleHighG());
    hold(t, 391.995, 4);
    EXPECT_EQ(t.lockedIndex(), 0)
        << "G4 es la cuerda 1 de un ukelele high-G; si esto da 2 es que se esta "
           "ordenando por frecuencia";
    EXPECT_NEAR(t.lockedTargetHz(), 391.995, 1e-6);

    hold(t, 0.0, FastModeTracker::kSilentUpdatesToRelease, 0.0);
    hold(t, 261.626, 4);
    EXPECT_EQ(t.lockedIndex(), 1) << "C4 es la cuerda 2, aunque sea la mas grave";
}

// ---------------------------------------------------------------------------
// Una lectura sin señal suelta no puede soltar el enganche
// ---------------------------------------------------------------------------
TEST(FastModeTest, ASingleQuietReadingDoesNotDropTheLock) {
    auto t = withCandidates(guitar());
    hold(t, 82.407, 5);
    ASSERT_EQ(t.lockedIndex(), 0);

    t.update(0.0, 0.0);   // el hueco entre dos pulsaciones
    EXPECT_EQ(t.lockedIndex(), 0)
        << "solto la cuerda por un solo hueco: la pantalla parpadearia mientras "
           "el musico toca normal";
}

// ---------------------------------------------------------------------------
// 5.13 — golden del barrido
// ---------------------------------------------------------------------------
TEST(GoldenFastMode, TheSweepFromSlackMatchesItsGolden) {
    std::vector<golden::Sample> rows;
    struct Case { const char* name; double from; double to; int expect; };
    const Case kCases[] = {
        {"E2 desde floja", 60.0, 82.407, 0},
        {"D3 cruzando A2", 100.0, 146.832, 2},
        {"G3 cruzando D3", 130.0, 195.998, 3},
    };

    for (const auto& c : kCases) {
        auto t = withCandidates(guitar());
        t.lockTo(c.expect);                     // el musico elige la cuerda
        ASSERT_EQ(t.lockedIndex(), c.expect) << c.name;
        const auto locks = sweep(t, c.from, c.to, 6.0);
        int switches = 0;
        for (int l : locks) if (l != c.expect) ++switches;
        rows.push_back({std::string(c.name), c.from,
                        static_cast<double>(c.expect), static_cast<double>(switches)});
    }

    ASSERT_EQ(rows.size(), 3u);
    golden::checkOrRegen("fast_mode_sweep", 48000, 4096, rows,
                         {"desdeHz", "cuerdaEsperada", "saltos"}, "REQ-001 S5");
}

}  // namespace
}  // namespace wma_test
