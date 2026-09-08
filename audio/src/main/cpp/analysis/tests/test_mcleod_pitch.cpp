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

#include "support/AscentVsSweep.h"
#include "support/SyntheticSignal.h"
#include "tests/support/TestSanitizer.h"

#include "McLeodPitch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
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

/**
 * MINI-019 — `kPeakThreshold` fijada contra un mutante PLAUSIBLE, no sólo contra uno
 * aniquilante.
 *
 * 🔴 POR QUE HACE FALTA OTRO TEST SI YA HAY UNO DE OCTAVA
 * ------------------------------------------------------
 * `ItNeverPicksTheWrongOctaveWhenTheFundamentalIsWeak` mata el mutante `0,9 -> 0`, que
 * APAGA la defensa entera. Eso no dice nada sobre `0,9 -> 0,80`, que es la mutación que
 * produciría un cambio real — y esa **sobrevivía la suite entera** (1224 tests, medido).
 *
 * La razón es el ESTIMULO: aquel test usa f0 + H2 + H3, y el H3 impar rompe la
 * periodicidad en tau/2, así que la NSDF en tau/2 nunca compite. Medido: de -6 a -60 dB,
 * los umbrales 0,80 · 0,90 · 0,95 dan los tres razón 1,000. El estímulo es INSENSIBLE a
 * la constante que el test dice vigilar.
 *
 * Con **sólo pares** (f0 + H2) tau/2 casi explica la señal y el umbral DECIDE. Medido, la
 * profundidad a partir de la cual el detector se va a la octava:
 *
 *     kPeakThreshold   se equivoca desde
 *          0,80             -9 dB
 *          0,90            -12 dB     <- el valor de hoy
 *          0,95            -13 dB
 *          0,99 / 1,00     mas alla de -14 dB
 *
 * De ahí sale la ventana de este test: **-9 a -11 dB**, donde 0,90 acierta y 0,80 falla en
 * los tres puntos.
 *
 * 🔴 OJO CON LEER ESA TABLA DE MAS. Sobre ESTE estímulo subir el umbral es monótonamente
 * MAS robusto, lo que parecería contradecir al KDoc de la constante (*"subirlo a 1,0 trae
 * de vuelta la octava"*). No lo contradice: el KDoc tiene razón y el test de arriba lo
 * demuestra — con f0+H2+H3 se pone **rojo con 0,95 y con 1,00**. O sea que los dos tests se
 * reparten las direcciones:
 *
 *     mutante        0,9 -> 0     0,9 -> 0,80    0,9 -> 0,95    0,9 -> 1,00
 *     octava (H3)      ROJO          verde          ROJO           ROJO
 *     este (pares)     ROJO          ROJO           verde          verde
 *
 * La conclusión "subir es inocuo" salió de mirar un solo estímulo, y es falsa. Es la misma
 * trampa que REQ-031: una no-reproducción es una afirmación sobre **el estímulo probado**,
 * nunca sobre la clase.
 */
TEST(McLeodPitchTest, ThePeakThresholdIsPinnedAgainstAPlausibleMutant) {
    constexpr double kF0 = 82.4069;   // E2

    for (const double db : {9.0, 10.0, 11.0}) {
        std::vector<float> sig(static_cast<size_t>(kRate), 0.0f);
        const double weakAmp = 0.5 * std::pow(10.0, -db / 20.0);
        const auto fundamental = pureSine(kF0, kRate, kRate, weakAmp);
        const auto second      = pureSine(kF0 * 2.0, kRate, kRate, 0.5);
        for (size_t i = 0; i < sig.size(); ++i) sig[i] = fundamental[i] + second[i];

        McLeodPitch mpm;
        mpm.prepare(kRate);
        feed(mpm, sig);

        ASSERT_TRUE(mpm.hasPitch())
            << "sin altura no hay veredicto que juzgar, con el fundamental a -" << db << " dB";
        const double ratio = mpm.frequencyHz() / kF0;
        EXPECT_NEAR(ratio, 1.0, 0.01)
            << "con el fundamental a -" << db << " dB de H2 el detector se fue a "
            << mpm.frequencyHz() << " Hz (razon " << ratio << "). Razon ~2 es LA OCTAVA, y "
            << "aca la trae bajar `kPeakThreshold`: con 0,80 los tres puntos de este barrido "
            << "fallan. Si este test se pone rojo, mira esa constante antes que el estimulo.";
    }
}

// ---------------------------------------------------------------------------
// REQ-033 S1 — QUE LAGS EVALUA EL DETECTOR, Y CON QUE VALOR
// ---------------------------------------------------------------------------

/**
 * 🔴 POR QUE HAY UNA TABLA EN UN TEST
 * ----------------------------------
 * El detector lee f0/3 sobre el reproductor de Tunio (siete senos armonicos de E4 con H7 a
 * −9,6 dB) y sobre la E4 real del corpus. MEDIDO afuera del motor (2026-09-07, sonda en
 * Python sobre la misma ventana y la misma decimacion): la NSDF "de libro" en τ vale 0,9994
 * contra un umbral 0,9·max = 0,8995 — la regla del primer pico ELEGIRIA τ si viera ese
 * numero. El motor no lo ve. O sea que la diferencia esta ANTES de la regla: en que lags
 * evalua `analyzeWindow` (barrido con paso τ/12, criterio de maximo local entre muestras,
 * refinamiento solo alrededor del elegido) y no en `kPeakThreshold` (MINI-019 midio que
 * moverlo desplaza el defecto).
 *
 * Este test es el INSTRUMENTO: pide al detector real los candidatos de su ultima ventana con
 * su valor grueso, evalua la NSDF fina en la vecindad de cada uno, y lo imprime. Lo unico
 * que AFIRMA es la propiedad de libro adentro del detector (AC-033.2): el maximo fino en la
 * vecindad de τ supera `kPeakThreshold` por el maximo fino de todos los candidatos. Si eso
 * NO se cumple, el diagnostico de REQ-033 es falso y se re-planifica antes de tocar
 * produccion. Que el detector lea o no la nota correcta NO se afirma aca: eso es S2, y
 * fijarlo en S1 escribiria el defecto en el contrato.
 *
 * Bug plausible que atrapa: una sonda que devuelva los lags del barrido pero un valor
 * distinto del que la eleccion uso (p.ej. el refinado en vez del grueso) haria que la tabla
 * no explique la eleccion, y la columna "elegido por el barrido" no coincidiria con lo que
 * `frequencyHz()` dice. Se compara.
 */
namespace req033 {

double dB(double d) { return std::pow(10.0, d / 20.0); }

struct Stimulus {
    const char* name;
    double f0;
    double B;
    std::vector<double> amps;
};

/// Los estimulos de la spec (REQ-033, "Lo que ya esta medido"). Niveles de la nota del
/// 07/09 b §3; el falso de REQ-031 tal cual esta en test_spectral_support.cpp.
std::vector<Stimulus> stimuli() {
    const std::vector<double> seis = {0.5 * dB(-7.2), 0.5 * dB(-3.2), 0.5 * dB(-0.6),
                                      0.5,            0.5 * dB(-15.1), 0.5 * dB(-15.0)};
    std::vector<double> siete = seis;
    siete.push_back(0.5 * dB(-9.6));
    return {
        {"seis armonicos (control: converge)", 329.6276, 0.0, seis},
        {"siete, H7 a -9,6 dB (el reproductor)", 329.6276, 0.0, siete},
        {"seis estirados, B = 3e-4", 329.6276, 3e-4, seis},
        {"el falso de REQ-031 (f0 -20, H3, H5)", 329.6276, 0.0,
         {0.5 * dB(-20.0), 0.0, 0.5, 0.0, 0.5 * dB(-6.0)}},
    };
}

struct Row {
    int lag;
    double coarse;    // lo que el barrido vio
    double fine;      // max de nsdfAt en ±2 (la sonda de S1, independiente del detector)
    int fineLag;
    double refined;   // el pico REAL que el detector refino en ±lag/12 (S2): lo que la eleccion usa
    int refinedLag;
};

/**
 * 🔴 LA VENTANA TIENE QUE ESTAR INTACTA PARA QUE `nsdfAt` HABLE DE LA MISMA VENTANA.
 * `process` sigue escribiendo `mWindow` desde el indice 0 despues de analizarla, asi que si el
 * feed no es un multiplo entero de ventanas, la columna "fino" se evalua sobre un buffer
 * MEZCLADO (cola vieja + cabeza nueva, con una discontinuidad de fase) y no explica nada.
 * Medido en la primera corrida de este test: fino 0,807 contra grueso 0,992 en el MISMO lag.
 * Por eso se alimenta un numero entero de ventanas y `tableOf` exige igualdad bit a bit.
 */
int wholeWindows(int rate, int n) {
    const int decimation = static_cast<int>(std::lround(rate / 24000.0));
    return n * McLeodPitch::kWindowFrames * std::max(1, decimation);
}

std::vector<Row> tableOf(const McLeodPitch& mpm) {
    std::vector<Row> rows;
    for (int i = 0; i < mpm.sweepCandidateCount(); ++i) {
        const int lag = mpm.sweepCandidateLag(i);
        Row r{lag, mpm.sweepCandidateNsdf(i), -2.0, lag,
              mpm.sweepCandidateRefinedNsdf(i), mpm.sweepCandidateRefinedLag(i)};
        // La sonda muestra lo que la eleccion uso, sobre la ventana que la eleccion vio.
        EXPECT_EQ(mpm.nsdfAt(lag), r.coarse)
            << "lag " << lag << ": nsdfAt no coincide con el valor grueso — la ventana ya no "
            << "es la analizada (feed no multiplo de ventanas) o la sonda lee otra cosa";
        for (int l = lag - 2; l <= lag + 2; ++l) {
            const double v = mpm.nsdfAt(l);
            if (v > r.fine) { r.fine = v; r.fineLag = l; }
        }
        rows.push_back(r);
    }
    return rows;
}

/// La regla del detector, replicada para la columna "elegido": primer candidato cuyo pico
/// REFINADO supera kPeakThreshold · mejor refinado (desde S2; en S1 se comparaban los
/// gruesos, y ESO era el defecto). Si esta columna no coincide con `frequencyHz()`, la sonda
/// no esta mostrando lo que la eleccion uso.
int chosenByTheRule(const std::vector<Row>& rows) {
    double best = -1.0;
    for (const Row& r : rows) best = std::max(best, r.refined);
    for (const Row& r : rows)
        if (r.refined >= McLeodPitch::kPeakThreshold * best) return r.refinedLag;
    return -1;
}

/// Y la regla VIEJA, sobre los gruesos: se imprime al lado para que la tabla siga diciendo
/// donde estaba el defecto.
int chosenByTheOldSweepRule(const std::vector<Row>& rows) {
    double best = -1.0;
    for (const Row& r : rows) best = std::max(best, r.coarse);
    for (const Row& r : rows)
        if (r.coarse >= McLeodPitch::kPeakThreshold * best) return r.lag;
    return -1;
}

}  // namespace req033

TEST(McLeodPitchTest, AC0332_TheBookPropertyHoldsInsideTheRealDetectorAndTheTableSaysWhy) {
    using namespace req033;
    for (const int rate : {44100, 48000}) {
        for (const Stimulus& st : stimuli()) {
            SCOPED_TRACE(std::string(st.name) + " @ " + std::to_string(rate));
            McLeodPitch mpm;
            mpm.prepare(rate);
            // Cinco ventanas ENTERAS: la tabla es la de la ultima, con el antialias en regimen
            // y `mWindow` todavia intacta (ver `wholeWindows`).
            feed(mpm, partialsWithAmplitudes(st.f0, st.B, st.amps, rate, wholeWindows(rate, 5)));
            ASSERT_GT(mpm.windowsAnalyzed(), 1);
            ASSERT_GT(mpm.sweepCandidateCount(), 0) << "sin candidatos no hay tabla";

            const double working = static_cast<double>(rate) / mpm.decimation();
            const double tau = working / st.f0;
            const auto rows = tableOf(mpm);

            const int tauLag = static_cast<int>(std::lround(tau));
            double fineAtTau = -2.0;
            int fineAtTauLag = tauLag;
            for (int l = tauLag - 2; l <= tauLag + 2; ++l) {
                const double v = mpm.nsdfAt(l);
                if (v > fineAtTau) { fineAtTau = v; fineAtTauLag = l; }
            }
            double globalFine = fineAtTau;
            for (const Row& r : rows) globalFine = std::max(globalFine, r.fine);

            const int ruleChoice = chosenByTheRule(rows);
            const int oldChoice = chosenByTheOldSweepRule(rows);
            const double ratio = mpm.hasPitch() ? mpm.frequencyHz() / st.f0 : 0.0;
            std::printf("\n  [REQ-033] %s @ %d Hz  (working %.0f, tau = %.2f = lag %d)\n",
                        st.name, rate, working, tau, tauLag);
            std::printf("  detector: %s %.3f Hz  (razon %.4f, claridad %.4f)  |  "
                        "regla sobre refinados eligio lag %d (la vieja, sobre gruesos: %d)  |  "
                        "fino en tau: %.4f en %d  |  umbral 0,9*maxfino = %.4f\n",
                        mpm.hasPitch() ? "leyo" : "SIN ALTURA", mpm.frequencyHz(), ratio,
                        mpm.clarity(), ruleChoice, oldChoice, fineAtTau, fineAtTauLag,
                        McLeodPitch::kPeakThreshold * globalFine);
            std::printf("  %6s %8s %9s %6s %9s %6s %7s %s\n", "lag", "grueso", "fino(+-2)", "en",
                        "refinado", "en", "x tau", "");
            for (const Row& r : rows) {
                std::printf("  %6d %8.4f %9.4f %6d %9.4f %6d %7.3f %s\n", r.lag, r.coarse,
                            r.fine, r.fineLag, r.refined, r.refinedLag, r.lag / tau,
                            r.refinedLag == ruleChoice ? "<- elegido" : "");
            }

            // La sonda muestra LO QUE LA ELECCION USO: la regla replicada sobre los picos
            // refinados tiene que dar el mismo lag que el detector publico (a menos de la
            // interpolacion parabolica, que es sub-muestra).
            ASSERT_TRUE(mpm.hasPitch());
            const double chosenLag = working / mpm.frequencyHz();
            EXPECT_LE(std::abs(chosenLag - ruleChoice), 1.0)
                << "la tabla no explica la eleccion: el detector publico el lag " << chosenLag
                << " y la regla sobre los refinados elige " << ruleChoice;

            // AC-033.2 — la propiedad de libro, ADENTRO del detector real.
            EXPECT_GE(fineAtTau, McLeodPitch::kPeakThreshold * globalFine)
                << "la NSDF fina en la vecindad de tau (" << fineAtTau << ") NO supera el "
                << "umbral (" << McLeodPitch::kPeakThreshold * globalFine << "): el "
                << "diagnostico de REQ-033 no vale para este estimulo";
        }
    }
}

/**
 * AC-033.1 — leer las sondas no cambia el resultado. Dos detectores identicos, uno leido
 * entre bloques y otro nunca: misma altura bit a bit, misma claridad, mismos candidatos.
 *
 * Bug plausible: una sonda que evalue la NSDF sobre `mNsdf` y la deje escrita, o que refine
 * y pise el candidato. `nsdfAt` es const y no escribe; esto lo fija.
 */
TEST(McLeodPitchTest, AC0331_ReadingTheProbesDoesNotChangeTheVerdict) {
    using namespace req033;
    const Stimulus st = stimuli()[1];   // el reproductor
    const auto sig = partialsWithAmplitudes(st.f0, st.B, st.amps, 44100, wholeWindows(44100, 5));

    McLeodPitch quiet, probed;
    quiet.prepare(44100);
    probed.prepare(44100);
    int i = 0;
    const int n = static_cast<int>(sig.size());
    double sink = 0.0;
    while (i < n) {
        const int take = std::min(kBlock, n - i);
        quiet.process(sig.data() + i, take);
        probed.process(sig.data() + i, take);
        for (int k = 0; k < probed.sweepCandidateCount(); ++k)
            sink += probed.sweepCandidateNsdf(k) + probed.nsdfAt(probed.sweepCandidateLag(k) + 1)
                  + probed.sweepCandidateRefinedNsdf(k) + probed.sweepCandidateRefinedLag(k);
        i += take;
    }
    (void)sink;
    ASSERT_TRUE(quiet.hasPitch());
    EXPECT_EQ(quiet.frequencyHz(), probed.frequencyHz());
    EXPECT_EQ(quiet.clarity(), probed.clarity());
    ASSERT_EQ(quiet.sweepCandidateCount(), probed.sweepCandidateCount());
    for (int k = 0; k < quiet.sweepCandidateCount(); ++k) {
        EXPECT_EQ(quiet.sweepCandidateLag(k), probed.sweepCandidateLag(k));
        EXPECT_EQ(quiet.sweepCandidateNsdf(k), probed.sweepCandidateNsdf(k));
    }
    // Fuera de rango: la sonda no inventa un candidato.
    EXPECT_EQ(probed.sweepCandidateLag(-1), -1);
    EXPECT_EQ(probed.sweepCandidateLag(probed.sweepCandidateCount()), -1);
    EXPECT_EQ(probed.sweepCandidateNsdf(probed.sweepCandidateCount()), 0.0);
    EXPECT_EQ(probed.sweepCandidateRefinedLag(-1), -1);
    EXPECT_EQ(probed.sweepCandidateRefinedNsdf(probed.sweepCandidateCount()), 0.0);
    // REQ-034: el corte temprano refina un prefijo, y el prefijo es el mismo en los dos.
    EXPECT_EQ(quiet.refinedCandidateCount(), probed.refinedCandidateCount());
    EXPECT_GT(quiet.refinedCandidateCount(), 0);
    EXPECT_LE(quiet.refinedCandidateCount(), quiet.sweepCandidateCount());
}

/**
 * AC-033.3 a nivel DETECTOR: el reproductor de Tunio (siete senos de E4, H7 a −9,6 dB) se lee
 * en E4 a 44,1 y a 48 kHz. Es el gemelo, en `McLeodPitch` solo, del control negativo que vive
 * en `test_spectral_support.cpp` con el camino entero; aca no hay bandera ni compuerta que
 * puedan tapar una lectura en f0/3.
 *
 * Bug plausible: revertir el refinamiento por candidato (volver a comparar gruesos) lo pone
 * rojo en los dos rates — 109,875 Hz, razon 0,333. Y la variante "refinar solo hasta el
 * primer pico ≥ 0,9" tambien: el maximo verdadero se necesita ANTES de aplicar el umbral.
 */
TEST(McLeodPitchTest, AC0333_SevenHarmonicsWithAStrongSeventhReadTheirFundamentalAtBothRates) {
    using namespace req033;
    const Stimulus st = stimuli()[1];
    for (const int rate : {44100, 48000}) {
        SCOPED_TRACE("rate " + std::to_string(rate));
        McLeodPitch mpm;
        mpm.prepare(rate);
        feed(mpm, partialsWithAmplitudes(st.f0, st.B, st.amps, rate, wholeWindows(rate, 5)));
        ASSERT_TRUE(mpm.hasPitch());
        const double ratio = mpm.frequencyHz() / st.f0;
        EXPECT_NEAR(ratio, 1.0, 0.01)
            << "leyo " << mpm.frequencyHz() << " Hz (razon " << ratio << "): razon 0,333 es f0/3";
    }
}

// ---------------------------------------------------------------------------
// REQ-034 S1 — EL ASCENSO CONTRA EL BARRIDO ENTERO, sobre todo el conjunto de control
// ---------------------------------------------------------------------------

/**
 * AC-034.1 — las sondas de costo cuentan lo que el detector evaluo y no cambian nada.
 *
 * Bug plausible: contar solo el barrido y no los vecinos de la parabola, o no resetear por
 * ventana (el conteo creceria sin techo y la comparacion de S1 sumaria dos veces).
 */
TEST(McLeodPitchTest, AC0341_TheCostProbesCountTheWindowsEvaluationsAndDoNotChangeTheVerdict) {
    McLeodPitch a, b;
    a.prepare(kRate);
    b.prepare(kRate);
    const auto sig = pureSine(110.0, kRate, kRate);
    feed(a, sig);
    feed(b, sig);
    // Antes de cualquier ventana, cero; despues, el barrido evaluo al menos sus ~50 puntos y
    // el refinamiento al menos los 2·span+1 del candidato mas grave mas los dos vecinos.
    EXPECT_GT(a.nsdfEvaluationsSweep(), 40);
    EXPECT_GT(a.nsdfEvaluationsRefine(), 2);
    EXPECT_EQ(a.nsdfEvaluationsSweep(), b.nsdfEvaluationsSweep()) << "no es determinista";
    EXPECT_EQ(a.nsdfEvaluationsRefine(), b.nsdfEvaluationsRefine());
    EXPECT_EQ(a.frequencyHz(), b.frequencyHz());
    // Es POR VENTANA: dos ventanas mas no lo duplican.
    const int sweepOne = a.nsdfEvaluationsSweep();
    feed(a, sig);
    EXPECT_EQ(a.nsdfEvaluationsSweep(), sweepOne) << "el conteo no se resetea por ventana";
    McLeodPitch c;
    c.prepare(kRate);
    EXPECT_EQ(c.nsdfEvaluationsSweep(), 0);
    EXPECT_EQ(c.nsdfEvaluationsRefine(), 0);
}

/**
 * AC-034.2 — la MEDICION: sobre el conjunto de control entero, ¿el ascenso desde la muestra
 * gruesa termina en el mismo pico que el barrido ±span entero, y cuanto cuesta cada uno?
 *
 * No afirma que sean iguales: eso es lo que S1 mide y S2 decide. Afirma que la medicion se
 * hizo sobre todo el conjunto (ventanas > 0 en cada grupo) y la imprime, con la cuenta de la
 * ELECCION distinta —lo unico que un consumidor veria— aparte de la de picos distintos.
 *
 * Los estimulos son los de los tests que vigilan el detector, generados igual: los 8 golden
 * (senos puros, 48 k), el reproductor de Tunio y el falso de REQ-031 a 44,1 y 48 k, la octava
 * (f0 −20 dB + H2 + H3, notas ≤ 400 Hz), los pares de MINI-019 (E2, −9..−11 dB) y la matriz
 * de estimulos pobres entera (14 x 4 x 2 x 2 x 2 = 448, 2 s cada uno). El corpus grabado se
 * compara en test_corpus_gate.cpp, donde vive.
 */
TEST(McLeodPitchTest, AC0342_AscentVersusFullRefinementOverTheWholeControlSet) {
    using namespace wma_test::ascent;
    Tally total;
    std::printf("\n");

    {   // los golden de deteccion gruesa
        Tally t;
        const struct { const char* label; double hz; } kCases[] = {
            {"A0", 27.500}, {"B0", 30.868}, {"E2", 82.407}, {"A2", 110.000},
            {"D3", 146.832}, {"A4", 440.000}, {"E5", 659.255}, {"C7", 2093.005},
        };
        for (const auto& c : kCases) compare(kRate, pureSine(c.hz, kRate, kRate), t, std::string("golden ") + c.label);
        EXPECT_GT(t.windows, 0); print("golden (8 senos, 48 k)", t); add(total, t);
    }
    {   // el reproductor de Tunio y el falso de REQ-031, a los dos rates
        Tally t;
        for (const int rate : {44100, 48000})
            for (const auto& st : req033::stimuli())
                compare(rate, partialsWithAmplitudes(st.f0, st.B, st.amps, rate, req033::wholeWindows(rate, 5)),
                        t, std::string(st.name) + " @" + std::to_string(rate));
        EXPECT_GT(t.windows, 0); print("REQ-033 (4 estimulos x 2 rates)", t); add(total, t);
    }
    {   // la octava: fundamental 20 dB abajo, H2 y H3
        Tally t;
        for (const auto& note : notes()) {
            if (note.hz > 400.0) continue;
            std::vector<float> sig(static_cast<size_t>(kRate), 0.0f);
            const auto weak = pureSine(note.hz, kRate, kRate, 0.05);
            const auto strong = pureSine(note.hz * 2.0, kRate, kRate, 0.5);
            const auto third = pureSine(note.hz * 3.0, kRate, kRate, 0.25);
            for (size_t i = 0; i < sig.size(); ++i) sig[i] = weak[i] + strong[i] + third[i];
            compare(kRate, sig, t, std::string("octava ") + note.name);
        }
        EXPECT_GT(t.windows, 0); print("octava (f0 -20 dB + H2 + H3)", t); add(total, t);
    }
    {   // los pares de MINI-019
        Tally t;
        for (const double db : {9.0, 10.0, 11.0}) {
            std::vector<float> sig(static_cast<size_t>(kRate), 0.0f);
            const auto f = pureSine(82.4069, kRate, kRate, 0.5 * std::pow(10.0, -db / 20.0));
            const auto h2 = pureSine(82.4069 * 2.0, kRate, kRate, 0.5);
            for (size_t i = 0; i < sig.size(); ++i) sig[i] = f[i] + h2[i];
            compare(kRate, sig, t, "pares -" + std::to_string(static_cast<int>(db)) + " dB");
        }
        EXPECT_GT(t.windows, 0); print("MINI-019 (pares, -9..-11 dB)", t); add(total, t);
    }
    {   // la matriz de estimulos pobres entera — y bajo sanitizer un subconjunto ROTATIVO,
        // 1 de cada 8 combinaciones por par (cuerda, riqueza), la misma forma que MINI-020: este
        // test dio timeout bajo TSan a -j4 en el gate de REQ-034.1 (la clase que MINI-020 acaba
        // de cerrar). La medicion que importa sale del build normal, con los 448.
        Tally t;
        int par = 0;
        for (const auto& cuerda : catalogStrings())
            for (int nPart = 1; nPart <= 4; ++nPart, ++par) {
                int combo = 0;
                for (const int rate : {44100, 48000})
                    for (const double B : {0.0, 1e-4})
                        for (const double fase : {0.0, M_PI / 3.0}) {
#ifdef WMA_TEST_UNDER_SANITIZER
                            if (combo++ != par % 8) continue;
#else
                            (void)combo;
#endif
                            compare(rate, inharmonicString(cuerda.hz, B, nPart, rate, rate * 2, 0.3, fase), t,
                                    std::string(cuerda.name) + " @" + std::to_string(rate) + " n=" +
                                        std::to_string(nPart) + " B=" + std::to_string(B));
                        }
            }
        EXPECT_GT(t.windows, 0); print("matriz pobre (448 x 2 s; 56 bajo sanitizer)", t); add(total, t);
    }
    print("TOTAL sintetico", total);
    std::printf("\n");
    RecordProperty("candidatos_lag_distinto", std::to_string(total.differLag));
    RecordProperty("ventanas_eleccion_distinta", std::to_string(total.chosenDiffer));

    // AC-034.3 (REQ-034 S2): el corte temprano es EXACTO — en ninguna ventana el detector
    // eligio distinto que la regla entera replicada desde el test. Y corto de verdad: en la
    // mayoria de las ventanas periodicas no refino todo.
    EXPECT_EQ(total.fullRuleDiffer, 0) << "el corte temprano cambio una eleccion";
    EXPECT_LT(total.refinedAll, total.windows / 2)
        << "el corte temprano casi nunca corta: refino todo en " << total.refinedAll << " de "
        << total.windows;
    EXPECT_LT(total.evalFull * 2, total.evalFullRule)
        << "produccion evaluo " << total.evalFull << " contra " << total.evalFullRule
        << " de refinar todo: el ahorro no llega a la mitad";
}

/**
 * AC-034.3 / AC-034.5 — LA ZONA AMBIGUA SE EJERCE, y decide como la regla entera.
 *
 * El corte temprano para en el primer pico >= 0,9 solo si ningun anterior alcanza
 * 0,9 · (maximo visto). Un par f0 + H2 con el fundamental hundido pone el pico de τ/2 cerca del
 * de τ, y con ruido encima el de τ baja de 1: ahi hay ventanas donde τ/2 queda en la zona
 * ambigua [0,9·M, 0,9), el corte NO puede parar, refina todo y la regla entera decide. Se
 * afirma que en esas ventanas `refinedCandidateCount() == sweepCandidateCount()` y que la
 * eleccion coincide con la regla entera — y que hubo al menos una, porque un camino que no se
 * ejerce es un camino que no se prueba.
 *
 * Bug plausible (el mutante m2 de REQ-033.2, "parar en el primero >= 0,9"): en esas ventanas
 * elige τ aunque la regla entera elija τ/2. Este test lo mata por `fullRuleDiffer`.
 */
TEST(McLeodPitchTest, AC0343_TheAmbiguousZoneIsExercisedAndDecidesLikeTheFullRule) {
    using namespace wma_test::ascent;
    Tally t;
    long ambiguousWindows = 0;
    for (const double db : {11.5, 12.0, 12.5}) {
        for (const double snr : {6.0, 10.0}) {
            std::vector<float> sig(static_cast<size_t>(kRate * 2), 0.0f);
            const auto f = pureSine(82.4069, kRate, kRate * 2, 0.5 * std::pow(10.0, -db / 20.0));
            const auto h2 = pureSine(82.4069 * 2.0, kRate, kRate * 2, 0.5);
            for (size_t i = 0; i < sig.size(); ++i) sig[i] = f[i] + h2[i];
            addNoiseAtSnr(sig, snr, 4242u + static_cast<uint32_t>(db * 10 + snr));
            Tally one;
            compare(kRate, sig, one, "par -" + std::to_string(db) + " dB, SNR " + std::to_string(snr));
            ambiguousWindows += one.refinedAll;
            add(t, one);
        }
    }
    print("zona ambigua (pares ruidosos)", t);
    ASSERT_GT(t.windows, 0);
    EXPECT_GT(ambiguousWindows, 0) << "ningun estimulo entro en la zona ambigua: el camino no se ejercio";
    EXPECT_EQ(t.fullRuleDiffer, 0) << "en la zona ambigua el detector eligio distinto que la regla entera";
}

}  // namespace
}  // namespace wma_test
