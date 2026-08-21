/**
 * test_mcleod_pitch.cpp — REQ-001 S4: qué nota ES.
 *
 * LO QUE ESTA SUITE VIGILA, Y POR QUÉ NO ES LA EXACTITUD
 * ------------------------------------------------------
 * El detector grueso no compite con el estimador de fase: le sobra con **50 cents**, mil
 * veces más grosero. Lo que sí no puede hacer —nunca— es equivocarse de **octava**, porque
 * un afinador que muestra la nota equivocada es peor que uno que no muestra nada: el usuario
 * afina de verdad hacia el lugar equivocado.
 *
 * Por eso el peso de estos tests está en los modos de falla y no en los decimales:
 * octava, ruido, silencio, y el caso que los provoca todos —el fundamental débil.
 *
 * EL CORPUS ES EL DE S2, Y NO SE DUPLICA
 * --------------------------------------
 * `SyntheticSignal.h` ya genera seno puro, cuerda inarmónica con B configurable, decaimiento
 * y ruido a SNR declarado, con **f0 exacto por construcción**. Escribir un generador nuevo
 * acá daría dos fuentes de verdad para la misma pregunta.
 */

#include "support/SyntheticSignal.h"
#include "tests/support/TestSanitizer.h"

#include "McLeodPitch.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

// La deteccion de sanitizer vive en `tests/support/TestSanitizer.h` desde
// REQ-005 S2. Estaba aca adentro, y por eso el test de costo SIGUIENTE
// —`PhaseSlopeCost`, de otro archivo— nacio sin la guarda: pasaba por holgura,
// no por proteccion. Un guardrail que hay que acordarse de copiar no es un
// guardrail.

namespace wma_test {
namespace {

using wma::dsp::McLeodPitch;

constexpr int kRate = 48000;
constexpr int kBlock = 256;

struct Note {
    const char* name;
    double hz;
};

/// El rango del AC, con el extremo grave que justifica todo el diseño (A0, B0) y el agudo
/// donde la decimación duele (C7).
const std::vector<Note>& notes() {
    static const std::vector<Note> kNotes = {
        {"A0", 27.500}, {"B0", 30.868}, {"E1", 41.203}, {"E2", 82.407},
        {"A2", 110.000}, {"D3", 146.832}, {"G3", 195.998}, {"E4", 329.628},
        {"A4", 440.000}, {"E5", 659.255}, {"A5", 880.000}, {"C7", 2093.005},
    };
    return kNotes;
}

void feed(McLeodPitch& mpm, const std::vector<float>& sig, int block = kBlock) {
    int i = 0;
    const int n = static_cast<int>(sig.size());
    while (i < n) {
        const int take = std::min(block, n - i);
        mpm.process(sig.data() + i, take);
        i += take;
    }
}

double centsError(double measured, double truth) {
    if (measured <= 0.0) return 1e9;
    return std::abs(1200.0 * std::log2(measured / truth));
}

// ---------------------------------------------------------------------------
// 4.1 — la nota correcta en todo el rango
// ---------------------------------------------------------------------------

TEST(McLeodPitchTest, ItFindsTheRightNoteAcrossTheWholeRange) {
    for (const auto& note : notes()) {
        McLeodPitch mpm;
        mpm.prepare(kRate);
        // Un segundo entero: alcanza para varias ventanas incluso en A0.
        feed(mpm, pureSine(note.hz, kRate, kRate));

        ASSERT_TRUE(mpm.hasPitch()) << note.name << ": no encontró ninguna nota";
        const double err = centsError(mpm.frequencyHz(), note.hz);
        RecordProperty(std::string("error_cents_") + note.name, std::to_string(err));

        EXPECT_LT(err, 50.0)
            << note.name << " (" << note.hz << " Hz): detectó " << mpm.frequencyHz()
            << " Hz — error " << err << " cents";
    }
}

/**
 * 4.2 · AC-001.6 — **CERO errores de octava** con el fundamental 20 dB por debajo del
 * segundo parcial.
 *
 * Es el caso de la bordona grave y del banjo, y es donde la autocorrelación cruda falla
 * sistemáticamente: su máximo global cae en 2·τ. Un error de octava no es "un poco de error"
 * —son 1200 cents— así que se mide aparte y con su propio umbral.
 */
TEST(McLeodPitchTest, ItNeverPicksTheWrongOctaveWhenTheFundamentalIsWeak) {
    for (const auto& note : notes()) {
        if (note.hz > 400.0) continue;   // el caso es de cuerdas graves

        // Fundamental atenuado 20 dB (factor 0,1) contra el segundo parcial.
        std::vector<float> sig(static_cast<size_t>(kRate), 0.0f);
        const auto weak = pureSine(note.hz, kRate, kRate, 0.05);
        const auto strong = pureSine(note.hz * 2.0, kRate, kRate, 0.5);
        const auto third = pureSine(note.hz * 3.0, kRate, kRate, 0.25);
        for (size_t i = 0; i < sig.size(); ++i) sig[i] = weak[i] + strong[i] + third[i];

        McLeodPitch mpm;
        mpm.prepare(kRate);
        feed(mpm, sig);

        ASSERT_TRUE(mpm.hasPitch()) << note.name;
        const double err = centsError(mpm.frequencyHz(), note.hz);
        EXPECT_LT(err, 50.0)
            << note.name << ": con el fundamental 20 dB abajo detectó " << mpm.frequencyHz()
            << " Hz contra " << note.hz << " — error " << err << " cents"
            << (err > 1100.0 && err < 1300.0 ? "  ← ERROR DE OCTAVA" : "");
    }
}

// ---------------------------------------------------------------------------
// 4.4 / 4.5 — cuándo NO hay nota
// ---------------------------------------------------------------------------

/** AC-001.4: por debajo del gate se reporta "sin señal", **no** un pitch. */
TEST(McLeodPitchTest, BelowTheGateItReportsNoPitchInsteadOfAValue) {
    McLeodPitch mpm;
    mpm.prepare(kRate);

    // Un tono real, pero 60 dB por debajo del piso.
    feed(mpm, pureSine(110.0, kRate, kRate, 0.00001));

    EXPECT_FALSE(mpm.hasPitch()) << "inventó " << mpm.frequencyHz() << " Hz sobre silencio";
    EXPECT_EQ(mpm.frequencyHz(), 0.0);
}

/**
 * 4.5 — con ruido blanco **no inventa una nota**.
 *
 * Es distinto del silencio: acá hay energía de sobra, así que el gate de nivel no lo salva.
 * Lo único que puede rechazarlo es la **claridad**: el ruido no tiene periodicidad y sus
 * picos de NSDF quedan por debajo del umbral.
 */
TEST(McLeodPitchTest, WhiteNoiseDoesNotProduceAnInventedNote) {
    McLeodPitch mpm;
    mpm.prepare(kRate);

    std::vector<float> noise(static_cast<size_t>(kRate), 0.0f);
    addNoiseAtSnr(noise, -100.0);          // señal despreciable: prácticamente ruido puro
    feed(mpm, noise);

    EXPECT_FALSE(mpm.hasPitch())
        << "sobre ruido blanco reportó " << mpm.frequencyHz() << " Hz con claridad "
        << mpm.clarity();
    EXPECT_LT(mpm.clarity(), McLeodPitch::kMinClarity);
}

/** Y la otra mitad: sobre una nota REAL la claridad es alta. Sin esto, un detector que
 *  devolviera claridad 0 siempre pasaría el test de arriba. */
TEST(McLeodPitchTest, ARealNoteHasHighClarity) {
    McLeodPitch mpm;
    mpm.prepare(kRate);
    feed(mpm, pureSine(220.0, kRate, kRate));

    ASSERT_TRUE(mpm.hasPitch());
    EXPECT_GT(mpm.clarity(), 0.9)
        << "un seno puro tendría que dar claridad cercana a 1, dio " << mpm.clarity();
}

// ---------------------------------------------------------------------------
// 4.3 — latencia
// ---------------------------------------------------------------------------

/**
 * 4.3 — desde el onset hasta la primera detección, **≤ 100 ms**, medido en A0 que es el peor
 * caso: la ventana de análisis tiene que cubrir varios períodos de 36 ms.
 *
 * Se mide en MUESTRAS y no con un reloj: lo que importa es cuánta señal necesita el detector,
 * no lo rápido que corre esta máquina.
 */
TEST(McLeodPitchTest, TheLatencyFromOnsetIsUnderOneHundredMilliseconds) {
    for (const auto& note : {Note{"A0", 27.5}, Note{"E2", 82.407}, Note{"A4", 440.0}}) {
        McLeodPitch mpm;
        mpm.prepare(kRate);

        const auto sig = pureSine(note.hz, kRate, kRate);
        int consumed = 0;
        int firstAt = -1;
        while (consumed < static_cast<int>(sig.size())) {
            const int take = std::min(kBlock, static_cast<int>(sig.size()) - consumed);
            mpm.process(sig.data() + consumed, take);
            consumed += take;
            if (mpm.hasPitch() && firstAt < 0) { firstAt = consumed; break; }
        }

        ASSERT_GT(firstAt, 0) << note.name << ": nunca detectó";
        const double ms = 1000.0 * firstAt / kRate;
        RecordProperty(std::string("latency_ms_") + note.name, std::to_string(ms));
        EXPECT_LE(ms, 100.0) << note.name << ": primera detección a los " << ms << " ms";
    }
}

// ---------------------------------------------------------------------------
// 4.7 / 4.8 — contrato
// ---------------------------------------------------------------------------

TEST(McLeodPitchTest, ResetMakesItIndistinguishableFromFreshlyPrepared) {
    const auto sig = pureSine(196.0, kRate, kRate);

    McLeodPitch dirty;
    dirty.prepare(kRate);
    feed(dirty, pureSine(82.407, kRate, kRate));    // otra nota, bien distinta
    dirty.reset();

    McLeodPitch fresh;
    fresh.prepare(kRate);

    EXPECT_FALSE(dirty.hasPitch()) << "reset() dejó una detección viva";
    EXPECT_EQ(dirty.windowsAnalyzed(), 0);

    feed(dirty, sig);
    feed(fresh, sig);
    EXPECT_DOUBLE_EQ(dirty.frequencyHz(), fresh.frequencyHz())
        << "el estado viejo sobrevivió al reset y corrió la detección";
    EXPECT_DOUBLE_EQ(dirty.clarity(), fresh.clarity());
}

/** El tamaño de bloque del llamador no puede cambiar el resultado — bit a bit. */
TEST(McLeodPitchTest, TheResultIsBitIdenticalRegardlessOfTheCallersBlockSize) {
    const auto sig = pureSine(146.832, kRate, kRate);

    McLeodPitch small, large, odd;
    for (auto* m : {&small, &large, &odd}) m->prepare(kRate);
    feed(small, sig, 16);
    feed(large, sig, 1024);
    feed(odd, sig, 337);

    EXPECT_DOUBLE_EQ(small.frequencyHz(), large.frequencyHz());
    EXPECT_DOUBLE_EQ(small.frequencyHz(), odd.frequencyHz());
    EXPECT_EQ(small.windowsAnalyzed(), large.windowsAnalyzed());
}

/**
 * 4.14 — el costo, para el presupuesto de S10.
 *
 * Mismo criterio que el del estimador de fase: se **mide y se reporta siempre**, y la
 * aserción es floja a propósito. Un techo ajustado falla cuando la máquina está cargada, y
 * un guardrail que falla por ruido se termina silenciando.
 */
TEST(McLeodPitchCost, TheDetectorCostsAFractionOfRealTimeAndTheNumberIsRecorded) {
    // 🔴 MEDIR PERFORMANCE CON EL CODIGO INSTRUMENTADO NO MIDE NADA.
    //
    // El comentario de arriba dice que el techo es flojo porque "un techo
    // ajustado falla cuando la maquina esta cargada". Preveia la CARGA; no
    // preveia el SANITIZER, que multiplica el costo por un factor de 5 a 10 y
    // deja sin sentido a cualquier techo razonable.
    //
    // Medido el 2026-08-20 en el gate local bajo TSan: 0,404 contra el techo de
    // 0,25 — y con CERO carreras reportadas por TSan. Un rojo que no es un
    // defecto es exactamente lo que termina haciendo que alguien silencie el
    // guardrail entero, asi que el que se saltea es este test y no el job.
    //
    // El numero de costo sigue saliendo de la corrida normal, que es donde
    // significa algo.
    WMA_SKIP_IF_SANITIZED();
    McLeodPitch mpm;
    mpm.prepare(kRate);
    const int frames = 10 * kRate;
    const auto sig = pureSine(110.0, kRate, frames);

    const auto t0 = std::chrono::steady_clock::now();
    feed(mpm, sig);
    const auto t1 = std::chrono::steady_clock::now();

    const double elapsed = std::chrono::duration<double>(t1 - t0).count();
    const double fraction = elapsed / (static_cast<double>(frames) / kRate);
    RecordProperty("real_time_fraction_pct", std::to_string(fraction * 100.0));
    std::printf("[ COSTO   ] deteccion gruesa: %.3f %% del tiempo real  |  decimacion x%d\n",
                fraction * 100.0, mpm.decimation());

    EXPECT_LT(fraction, 0.25)
        << "la deteccion gruesa cuesta " << fraction * 100.0 << " % del tiempo real";
}

}  // namespace
}  // namespace wma_test
