/**
 * test_attack_in_window.cpp — REQ-036 S1 (AC-036.1, AC-036.2). EL ATAQUE DE LA NOTA DENTRO DE LA
 * VENTANA DE REGRESION DEL ESTIMADOR DE FASE, medido sobre sintesis ANTES de tocar produccion.
 *
 * El estimador regresa la fase desenvuelta sobre 48 ventanas de 4096 frames (4,1 s a 48 kHz) y
 * publica la pendiente como cents y el error estandar como σ. Un glide de ataque deja la fase como
 * un palo de hockey, y una recta ajustada a un palo de hockey deja residuos CHICOS: la lectura sale
 * sesgada con σ chica —CONVERGIDA y equivocada—. Sobre el corpus (REQ-035) eso vale hasta 0,73 c
 * con σ 0,054. Aca se mide con el estimulo controlado: cuanto, durante cuanto tiempo, y si σ lo ve.
 *
 * LO QUE SE AFIRMA, Y POR QUE ESO Y NO MAS
 * ----------------------------------------
 * · Los controles: la misma cuerda SIN glide converge a 0,1 c (sin esto ninguna fila significa
 *   nada), y el generador de trayectoria reproduce BIT A BIT al generador estacionario cuando la
 *   trayectoria es constante (una sola fuente de verdad).
 * · La forma del glide sintetico, verificada con el oraculo por tramos (otro metodo): parte donde
 *   se le pidio, se acerca monotonamente y termina asentado.
 * · AC-036.2: el estadistico de tendencia elegido, con su umbral, da CERO disparos sobre nota
 *   estable (14 cuerdas × 2 rates) y sobre vibrato leve, y dispara en TODOS los parciales de TODA
 *   ventana donde σ es ciega al glide. El umbral es el veredicto de S1 y esta escrito como
 *   constante; la grilla que lo eligio se imprime como evidencia, con su margen.
 * Los numeros del barrido (error en CONVERGIDA, σ, cuantos casos ciegos, cuanto tarda la compuerta
 * simulada) se IMPRIMEN y no se afirman: son la medicion de S1. S2 afirma sobre ellos (AC-036.4/5).
 *
 * 🔴 EL CONTROL EXTERNO SIGUE A LA ALTURA INSTANTANEA, NO A LA FINAL. La etapa decia "control
 * externo en la altura final", y asi el arbitraje por signo de REQ-014 descarta a los cuatro
 * parciales durante un glide grave (leen negativo contra un control positivo) y la lectura queda
 * MEASURING sin que el estadistico haya hecho nada — un resultado del test, no del motor. En
 * produccion el control es la gruesa, que sigue a la nota; el que se usa aca es la gruesa IDEAL
 * (la altura verdadera del instante), que es lo que un McLeod sin error informaria.
 */

#include "support/SyntheticSignal.h"
#include "support/PartialOracle.h"
#include "support/PhaseTrend.h"
#include "support/CorpusSweep.h"
#include "tests/support/TestSanitizer.h"

#include "StrobeTracker.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace wma_test {
namespace {

using wma::analysis::PhaseSlopeEstimator;
using wma::analysis::StrobeTracker;

constexpr int kRate = 48000;
constexpr double kF0 = 146.83238;        // D3, como REQ-035: parciales lejos de Nyquist y del rango
constexpr double kB = 1.0e-4;            // una cuerda real
constexpr double kProbeCents = 1.0;      // DIEZ veces la tolerancia: un estimador que devuelva 0 falla
constexpr double kToleranceCents = 0.1;  // el contrato, y el umbral de "asentada"
constexpr int kBlock = 512;
constexpr int kPartials = StrobeTracker::kPartials;

/// Hasta donde se alimenta el barrido. 12 s y no los 5 de la etapa: el glide mas lento (30 c,
/// τ = 1 s) se asienta a 0,1 c recien a 5,7 s, y el trinquete de S2 cuenta 3 s desde ahi.
constexpr double kFeedSeconds = 12.0;
constexpr double kControlSeconds = 5.0;

// ---------------------------------------------------------------------------
// Una corrida del strobe sobre una trayectoria, muestreada en cada cierre de ventana
// ---------------------------------------------------------------------------
struct WindowSample {
    double sec = 0.0;              ///< segundos alimentados al cerrar la ventana
    bool converged = false;
    double cents = NAN, sigma = NAN;
    int used = 0;
    int admitted = -1;             ///< reconstruido contra `fitStretchedSeries` (REQ-035)
    int measuredMask = 0;          ///< los parciales con medicion: la base del simulador externo
    bool measured[kPartials] = {};
    double pCents[kPartials] = {NAN, NAN, NAN, NAN};
    double pSigma[kPartials] = {NAN, NAN, NAN, NAN};
    trend::Trend trend[kPartials];
    int count = 0;                 ///< fases en la ventana de regresion del parcial 0
    double phase[kPartials] = {};  ///< la fase desenvuelta MAS NUEVA de cada parcial: la historia entera
};

struct Trajectory {
    std::string label;
    int rate = kRate;
    double targetHz = kF0;
    double settleSec = 0.0;       ///< desde cuando la altura esta a < 0,1 c de la final
    int restarts = 0;             ///< `windowRestarts()` del strobe al final (R-PITCH-63)
    std::vector<WindowSample> samples;
};

/// Alimenta un strobe con `sig` (altura final en `kProbeCents` sobre `kF0`) y registra cada
/// cierre de ventana. `centsAt(t)` es la altura verdadera del instante, para el control.
Trajectory runTrajectory(const std::string& label, const std::vector<float>& sig, int rate, double targetHz,
                  const std::function<double(double)>& centsAt, double settleSec) {
    Trajectory r;
    r.label = label;
    r.rate = rate;
    r.targetHz = targetHz;
    r.settleSec = settleSec;
    StrobeTracker t;
    t.prepare(rate);
    t.setTarget(targetHz);
    const int n = static_cast<int>(sig.size());
    int lastWindows = 0;
    for (int i = 0; i < n; i += kBlock) {
        const double tSec = static_cast<double>(i) / rate;
        t.setCoarseFrequencyHz(detune(targetHz, centsAt(tSec)));
        t.process(sig.data() + i, std::min(kBlock, n - i));
        const int windows = t.partialEstimator(0).windowsAnalyzed();
        if (windows == lastWindows) continue;
        lastWindows = windows;
        WindowSample s;
        s.sec = static_cast<double>(i + std::min(kBlock, n - i)) / rate;
        s.converged = t.converged();
        s.cents = t.cents();
        s.sigma = t.uncertaintyCents();
        s.used = t.partialsUsed();
        for (int p = 0; p < kPartials; ++p) {
            s.measured[p] = t.partialHasMeasurement(p);
            if (s.measured[p]) s.measuredMask |= 1 << p;
            s.pCents[p] = s.measured[p] ? t.partialCents(p) : NAN;
            s.pSigma[p] = s.measured[p] ? t.partialUncertaintyCents(p) : NAN;
            s.trend[p] = trend::trendOfPartial(t, p, rate);
            const PhaseSlopeEstimator& e = t.partialEstimator(p);
            s.phase[p] = e.regressionPhaseCount() > 0 ? e.regressionPhaseAt(e.regressionPhaseCount() - 1) : NAN;
        }
        s.count = t.partialEstimator(0).regressionPhaseCount();
        double fitB = NAN;
        s.admitted = t.hasMeasurement() ? corpus::reconstructAdmitted(t, s.used, s.cents, &fitB) : 0;
        r.samples.push_back(s);
    }
    r.restarts = t.windowRestarts();
    return r;
}

// ---------------------------------------------------------------------------
// El barrido X × τ, hecho UNA vez y compartido
// ---------------------------------------------------------------------------
struct GlideCase { double startCents; double tau; };

const std::vector<GlideCase>& glideCases() {
    static const std::vector<GlideCase> kCases = [] {
        std::vector<GlideCase> v;
        // Los dos signos: el corpus glidea desde abajo (bajo-acustico_G2: −27,7 c; ukelele_C4:
        // −3 c), pero un barrido de un solo signo es un corpus uniforme, y esos ya escondieron
        // hallazgos tres veces en este repo.
        for (double sign : {-1.0, +1.0})
            for (double x : {2.0, 5.0, 10.0, 30.0})
                for (double tau : {0.2, 0.5, 1.0}) v.push_back({sign * x, tau});
        return v;
    }();
    return kCases;
}

std::string glideLabel(const GlideCase& g) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "glide %+5.1f c, tau %.1f s", g.startCents, g.tau);
    return buf;
}

const std::vector<Trajectory>& glideRuns() {
    static const std::vector<Trajectory> kRuns = [] {
        std::vector<Trajectory> runs;
        for (const GlideCase& g : glideCases()) {
            const int frames = static_cast<int>(kFeedSeconds * kRate);
            const auto sig = glidingString(detune(kF0, kProbeCents), g.startCents, g.tau, kB, 4,
                                           kRate, frames);
            const double x = g.startCents, tau = g.tau;
            runs.push_back(runTrajectory(
                glideLabel(g), sig, kRate, kF0,
                [x, tau](double t) { return kProbeCents + x * std::exp(-t / tau); },
                glideSettleSeconds(x, tau, kToleranceCents)));
        }
        return runs;
    }();
    return kRuns;
}

/// Nota estable: las 14 cuerdas × 2 rates. Es el control de AC-036.1 y el negativo de AC-036.2.
const std::vector<Trajectory>& stableRuns() {
    static const std::vector<Trajectory> kRuns = [] {
        std::vector<Trajectory> runs;
        for (int rate : {44100, 48000}) {
            for (const CatalogString& s : catalogStrings()) {
                const int frames = static_cast<int>(kControlSeconds * rate);
                const auto sig = partialsWithPitchTrajectory(
                    detune(s.hz, kProbeCents), kB, oneOverNAmplitudes(4), rate, frames,
                    [](double) { return 0.0; });
                runs.push_back(runTrajectory(
                    std::string("estable ") + s.name + (rate == 48000 ? " @48k" : " @44.1k"), sig,
                    rate, s.hz, [](double) { return kProbeCents; }, 0.0));
            }
        }
        return runs;
    }();
    return kRuns;
}

struct VibratoCase { double depthCents; double rateHz; };
const std::vector<VibratoCase>& vibratoCases() {
    static const std::vector<VibratoCase> kCases = {{5.0, 5.0}, {10.0, 6.0}};
    return kCases;
}

/// Vibrato leve sobre D3, a 48 k: ±5 c a 5 Hz y ±10 c a 6 Hz. El control NEGATIVO del estadistico.
const std::vector<Trajectory>& vibratoRuns() {
    static const std::vector<Trajectory> kRuns = [] {
        std::vector<Trajectory> runs;
        for (const VibratoCase& v : vibratoCases()) {
            const int frames = static_cast<int>(kControlSeconds * kRate);
            const auto sig = vibratoString(detune(kF0, kProbeCents), v.depthCents, v.rateHz, kB, 4,
                                           kRate, frames);
            char label[48];
            std::snprintf(label, sizeof label, "vibrato ±%.0f c @ %.0f Hz", v.depthCents, v.rateHz);
            // El control externo va en la MEDIA, no en la altura instantanea: con ±5 c alrededor de
            // +1 c, media onda tiene el signo contrario a la lectura y el arbitraje por signo de
            // REQ-014 descarta a los cuatro parciales — un artefacto del test. La gruesa de
            // produccion promedia su propia ventana; aca se le da la media exacta.
            runs.push_back(runTrajectory(label, sig, kRate, kF0, [](double) { return kProbeCents; }, 0.0));
        }
        return runs;
    }();
    return kRuns;
}

// ---------------------------------------------------------------------------
// Controles del instrumento
// ---------------------------------------------------------------------------
TEST(AttackInWindow, TheTrajectoryGeneratorIsTheStationaryOneWhenTheTrajectoryIsFlat) {
    const int frames = 2 * kRate;
    const auto amps = oneOverNAmplitudes(4);
    const auto a = partialsWithAmplitudes(detune(kF0, kProbeCents), kB, amps, kRate, frames, 0.3);
    const auto b = partialsWithPitchTrajectory(detune(kF0, kProbeCents), kB, amps, kRate, frames,
                                               [](double) { return 0.0; }, 0.3);
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)), 0)
        << "el generador de trayectoria con cents ≡ 0 no reproduce bit a bit al estacionario: "
           "son dos fuentes de verdad";
}

/**
 * La forma del glide, verificada con OTRO metodo: el oraculo por tramos de REQ-035 (Goertzel +
 * Hann sobre 0,75 s). Un glide −30 c / τ 0,5 s tiene que arrancar grave, acercarse monotonamente y
 * terminar asentado en la altura final.
 */
TEST(AttackInWindow, TheSyntheticGlideHasTheShapeItWasAskedFor) {
    constexpr double kStart = -30.0, kTau = 0.5, kSeconds = 4.0;
    const double finalHz = detune(kF0, kProbeCents);
    const auto sig = glidingString(finalHz, kStart, kTau, kB, 4, kRate, static_cast<int>(kSeconds * kRate));
    std::printf("\n  [REQ-036] forma del glide sintetico (%.0f c, tau %.1f s) segun el oraculo por tramos\n",
                kStart, kTau);
    std::vector<double> perSegment;
    for (double t0 = 0.0; t0 + 0.75 <= kSeconds + 1e-9; t0 += 0.75) {
        const oracle::PartialReading r = oracle::measureAt(sig, kRate, finalHz, t0);
        ASSERT_TRUE(r.valid);
        const double c = oracle::centsOf(r.hz[1], finalHz);
        // La media de la altura verdadera sobre el tramo, para leer al lado.
        double mean = 0.0;
        for (int k = 0; k < 100; ++k) mean += kStart * std::exp(-(t0 + 0.75 * (k + 0.5) / 100.0) / kTau);
        mean /= 100.0;
        std::printf("    tramo %.2f-%.2f s: oraculo H1 %+7.2f c   (media verdadera %+7.2f c)\n",
                    t0, t0 + 0.75, c, mean);
        perSegment.push_back(c);
    }
    ASSERT_GE(perSegment.size(), 4u);
    EXPECT_LT(perSegment.front(), kStart / 4.0) << "el primer tramo no arranca grave";
    EXPECT_GT(perSegment.front(), kStart) << "el primer tramo esta mas grave que el punto de partida";
    for (size_t i = 1; i < perSegment.size(); ++i)
        EXPECT_GT(perSegment[i], perSegment[i - 1]) << "el glide no se acerca monotonamente (tramo " << i << ")";
    EXPECT_NEAR(perSegment.back(), 0.0, kToleranceCents) << "el ultimo tramo no esta asentado en la altura final";
}

TEST(AttackInWindow, WithoutAGlideTheSameStringMeetsTheContract) {
    int converged = 0;
    double worst = 0.0;
    for (const Trajectory& r : stableRuns()) {
        ASSERT_FALSE(r.samples.empty()) << r.label;
        const WindowSample& last = r.samples.back();
        EXPECT_TRUE(last.converged) << r.label << ": no convergio en " << kControlSeconds << " s (σ " << last.sigma << ")";
        EXPECT_NEAR(last.cents, kProbeCents, kToleranceCents) << r.label << ": fuera del contrato";
        if (last.converged) ++converged;
        worst = std::max(worst, std::fabs(last.cents - kProbeCents));
        // R-PITCH-63: sobre una nota estable la ventana adaptativa NO se reinicia. Sin esto un
        // reinicio de mas que converge igual pasaria inadvertido.
        EXPECT_EQ(r.restarts, 0) << r.label << ": la ventana se reinicio " << r.restarts << " veces sobre una nota estable";
    }
    std::printf("\n  [REQ-036] control estable: %d de %zu convergidas, peor error %.4f c\n",
                converged, stableRuns().size(), worst);
}

/**
 * LA COMPUERTA SIMULADA DESDE AFUERA sobre la HISTORIA de fases de cada parcial, en dos variantes:
 *
 *  · compuerta sola: en cada cierre se regresa la ventana de las ultimas 48 fases (la de
 *    produccion), se evalua el estadistico, y el parcial entra solo si `admits`;
 *  · ventana ADAPTATIVA (la R-PITCH-63 reservada): cuando el estadistico dispara, la integracion
 *    se reinicia en el punto de quiebre —se descarta la mitad vieja y se sigue con la nueva—.
 *
 * Que el simulador es FIEL lo afirma el control: sin compuerta, su lectura por parcial sobre la
 * ventana entera reproduce `partialCents(i)` del strobe en cada cierre. Sin eso, las latencias de
 * abajo hablarian de otro estimador.
 *
 * Lo que se imprime es lo que S2 va a afirmar (AC-036.4: ninguna convergida con |error| > 0,1;
 * AC-036.5: CONVERGIDA ≤ 3 s tras el asentamiento) y lo que decide si la ventana adaptativa hace
 * falta tambien por sintesis, no solo por el corpus.
 */
struct SimWindow { double sec; bool converged; double cents; double sigma; };
struct SimOutcome {
    double convSec = NAN, convErr = NAN;
    int blind = 0;           ///< ventanas CONVERGIDAS con |error| > 0,1 c
    int restarts = 0;        ///< reinicios de la ventana (solo adaptativa)
    int loneVetoes = 0;      ///< ventanas donde la regla del armonico solo dejo sin lectura
    int fires = 0;           ///< ventanas donde algun parcial disparo (con la ventana admisible)
    int evaluated = 0;       ///< ventanas con algun parcial evaluable
    std::vector<SimWindow> windows;   ///< la lectura simulada, ventana a ventana
};
struct BlindDetail {
    double sec, err, sigma, tMin, delta, bias; int windows, used;
    char why[kPartials];          ///< por parcial: A admitido, F disparo, R sin regrow, X produccion no lo admitio
    int cnt[kPartials]; double pcents[kPartials], pt[kPartials], pbias[kPartials];
};

/**
 * `sync`: cuando ALGUN parcial admitido dispara, se reinician LOS CUATRO en el mismo punto. Un glide
 * sesga a los cuatro por igual, asi que reiniciarlos por separado solo agrega un transitorio en el
 * que rebrotan en ventanas distintas y la lectura pasa por k = 1 — donde un parcial solo publica su
 * estiramiento inarmonico sin corregir (+0,087 c para el fundamental con B = 1e-4, +1,38 para el 4).
 */
/**
 * `majority`: con reinicio sincronizado, se reinicia solo si dispara AL MENOS LA MITAD de los
 * parciales admitidos; si dispara uno solo, ese parcial no se admite en este cierre y la ventana
 * sigue. Un glide sesga a los cuatro por igual; un parcial que oscila solo (batido entre capas del
 * sample, medido en bajo-acustico_D2: el parcial 3 con T 7,9 y Δ 0,125 en una nota que lee 0,01 c)
 * es problema de ese parcial, y reiniciar a los cuatro por el es tirar una integracion sana.
 */
SimOutcome simulate(const Trajectory& r, const trend::Threshold& th, bool adaptive, bool gate, double* worstFidelity,
                    std::vector<BlindDetail>* details = nullptr, bool sync = false, bool majority = false) {
    SimOutcome o;
    const int n = static_cast<int>(r.samples.size());
    int start[kPartials] = {0, 0, 0, 0};
    std::vector<double> hist[kPartials];
    for (const WindowSample& s : r.samples) for (int p = 0; p < kPartials; ++p) hist[p].push_back(s.phase[p]);
    for (int k = 0; k < n; ++k) {
        const WindowSample& s = r.samples[k];
        double pc[kPartials], ps[kPartials];
        int mask = 0;
        BlindDetail bd{s.sec, 0.0, 0.0, INFINITY, 0.0, 0.0, 0, 0, {'X','X','X','X'}, {0,0,0,0}, {NAN,NAN,NAN,NAN}, {NAN,NAN,NAN,NAN}, {NAN,NAN,NAN,NAN}};
        int excludedMask = 0;
        if (sync && adaptive && gate) {
            // Una sola ventana para los cuatro: la mas nueva de todas, y si alguno dispara, se
            // reinician todos en el mismo punto de quiebre.
            int common = 0;
            for (int p = 0; p < kPartials; ++p) common = std::max(common, start[p]);
            common = std::max(common, k + 1 - PhaseSlopeEstimator::kMaxWindows);
            bool any = false;
            int nFire = 0, nAdmitted = 0, excluded = 0;
            for (int p = 0; p < kPartials; ++p) {
                if (!(s.measuredMask & (1 << p))) continue;
                const int cnt = k + 1 - common;
                // Un quiebre se juzga sobre una ventana que tambien se admitiria: reiniciar sobre
                // una mas corta cascadea (medido sobre bajo-acustico_D2: tres reinicios a n = 8 en
                // una nota que hoy converge a 0,01 c).
                if (cnt < th.minWindows) continue;
                ++nAdmitted;
                if (trend::fires(trend::trendOver(hist[p].data() + common, cnt, r.rate, r.targetHz * (p + 1)), th)) {
                    ++nFire;
                    excluded |= 1 << p;
                }
            }
            any = majority ? (nFire > 0 && 2 * nFire >= nAdmitted) : (nFire > 0);
            for (int p = 0; p < kPartials; ++p) start[p] = common;
            if (!any) excludedMask = excluded;
            if (any) {
                const int cnt = k + 1 - common;
                for (int p = 0; p < kPartials; ++p) start[p] = k + 1 - cnt / 2;
                ++o.restarts;
                ++o.fires;
                o.windows.push_back({s.sec, false, NAN, NAN});
                continue;   // la ventana acaba de quebrarse: sin lectura en este cierre
            }
        }
        bool anyEvaluable = false, anyFire = false;
        for (int p = 0; p < kPartials; ++p) {
            // La base es lo MEDIDO, no lo que produccion admitio: sobre sintesis las otras reglas de
            // admision (energia, dominio, signo) dejan pasar a los cuatro, y despues de S2 produccion
            // no admite durante el transitorio — el simulador tiene que seguir midiendo "sin compuerta".
            if (!(s.measuredMask & (1 << p))) continue;
            if (excludedMask & (1 << p)) { bd.why[p] = 'F'; continue; }  // disparo solo: afuera, sin reiniciar
            start[p] = std::max(start[p], k + 1 - PhaseSlopeEstimator::kMaxWindows);
            const int cnt = k + 1 - start[p];
            const double* w = hist[p].data() + start[p];
            const trend::Trend tr = trend::trendOver(w, cnt, r.rate, r.targetHz * (p + 1));
            if (tr.evaluable && cnt >= th.minWindows) { anyEvaluable = true; anyFire = anyFire || trend::fires(tr, th); }
            if (tr.evaluable && std::fabs(tr.tScore) < bd.tMin) { bd.tMin = std::fabs(tr.tScore); bd.delta = tr.deltaCents; bd.bias = tr.biasCents; bd.windows = cnt; }
            const trend::PartialReading pr = trend::readingFromSlope(trend::fitSlope(w, 0, cnt), r.rate, r.targetHz * (p + 1));
            if (!adaptive && !gate && pr.ok && cnt == s.count && worstFidelity != nullptr)
                *worstFidelity = std::max(*worstFidelity, std::fabs(pr.cents - s.pCents[p]));
            bd.cnt[p] = cnt; bd.pcents[p] = pr.cents; bd.pt[p] = tr.tScore; bd.pbias[p] = tr.biasCents;
            if (!pr.ok || cnt < 4) { bd.why[p] = 'R'; continue; }
            if (gate) {
                if (adaptive && !sync && trend::fires(tr, th)) { start[p] = k + 1 - cnt / 2; ++o.restarts; bd.why[p] = 'F'; continue; }
                if (!trend::admits(tr, th)) { bd.why[p] = 'R'; continue; }
            }
            bd.why[p] = 'A';
            pc[p] = pr.cents; ps[p] = pr.sigma; mask |= 1 << p;
        }
        if (anyEvaluable) ++o.evaluated;
        if (anyFire) ++o.fires;
        // La regla del armonico solo va junto con la compuerta: es parte de la admision propuesta.
        const trend::Combined g = trend::combineFrom(pc, ps, mask, gate);
        if (g.loneHarmonicVetoed) ++o.loneVetoes;
        const bool conv = g.hasMeasurement && g.sigma <= StrobeTracker::kConvergedUncertaintyCents;
        o.windows.push_back({s.sec, conv, g.hasMeasurement ? g.cents : NAN, g.hasMeasurement ? g.sigma : NAN});
        if (conv && std::fabs(g.cents - kProbeCents) > kToleranceCents) {
            ++o.blind;
            if (details != nullptr) { bd.err = g.cents - kProbeCents; bd.sigma = g.sigma; bd.used = g.used; details->push_back(bd); }
        }
        if (conv && std::isnan(o.convSec)) { o.convSec = s.sec; o.convErr = g.cents - kProbeCents; }
    }
    return o;
}

/**
 * EL VEREDICTO DE S1, con la evidencia de las tablas de abajo Y de la grilla sobre el corpus en
 * `test_corpus_gate.cpp` (la misma terna, medida sobre los 41 archivos reales):
 *   · T = (b₂ − b₁)/√(se₁² + se₂²) sobre las dos mitades de la ventana, |T| > 5: la significancia.
 *     Sobre estable el maximo desde 12 ventanas es 3,57 (B0 a 44,1 k, n = 12: el rizado de la
 *     imagen negativa de la cuerda mas grave; desde 16 es 1,49) y sobre vibrato 0,43. Con 3 el
 *     rizado dispara a n = 12; con 5 queda 1,4× de margen.
 *   · |Δ| = |b₂ − b₁| en cents > 0,05: la magnitud. Es lo que ve la cola LENTA (τ = 1 s) despues
 *     de un reinicio, que con 0,10 deja 64 convergidas equivocadas de 0,10–0,125 c.
 *   · 12 ventanas como minimo para admitir (1,02 s a 48 k, 1,11 s a 44,1 k): por debajo el rizado
 *     de B0/E1 dispara tambien con T = 5 (n = 8: 2 falsos positivos), y las de 4..7 no tienen dos
 *     mitades. 🔴 16 es mas seguro sobre sintesis pero el corpus lo paga: las notas del ukelele
 *     duran 1,49–1,81 s y dejan de converger (30 de 33 contra 35 con 12).
 *   · Reinicio SINCRONIZADO de los cuatro parciales en el punto de quiebre cuando alguno dispara
 *     (la ventana adaptativa, R-PITCH-63): la compuerta sola tarda 3,5–3,8 s tras el asentamiento
 *     en TODOS los casos, porque mientras el glide esta en la ventana esta en la primera mitad;
 *     y sobre el corpus deja 16 convergidas de 33. Reiniciar por parcial deja transitorios con
 *     k = 1 donde un parcial solo publica su estiramiento inarmonico sin corregir.
 *   · El reinicio se dispara con CUALQUIER parcial admitido, y se juzga solo sobre una ventana que
 *     tambien se admitiria (≥ 12): reiniciar sobre ventanas mas cortas cascadea sobre ruido real.
 *     La variante "por mayoria" (reiniciar solo si dispara la mitad; excluir al parcial solo) da
 *     38 convergidas en el corpus pero el error maximo vuelve a 0,72 c: el glide de limpia_G3
 *     dispara en un parcial solo, y excluirlo sin reiniciar deja la lectura sesgada. Compra
 *     cantidad al precio de la honestidad que este REQ vino a comprar; se mide y no se adopta.
 * Con eso: 0 disparos sobre estable y vibrato, 0 convergidas equivocadas en el barrido, CONVERGIDA
 * a lo sumo 1,42 s despues del asentamiento; y sobre el corpus 35 convergidas (hoy 33) con error
 * maximo 0,20 c (hoy 0,73): pierde bajo-acustico_D2 (el parcial 3 oscila solo en una nota de 2,4 s),
 * bajo-dedos_D2 y bajo-pua_A1 (las dos derivan de verdad: +2,7 → +0,25 c y una cola que decae), y
 * gana bajo-acustico_G2 (el glide de −27,7 c de la spec, ahora a −0,00 c), ukelele_C4 y tres nylon.
 *
 * 🔴 EL ESTADISTICO SOLO, SOBRE LA VENTANA FIJA, NO SEPARA PERFECTO CON 12 VENTANAS: un glide de
 * 30 c a mitad de ventana da |T| ≈ 3,3 y el rizado de B0 a n = 12 da 3,57. Es la ventana adaptativa
 * la que hace que esas ventanas no existan (el quiebre ya se reinicio antes). Por eso AC-036.2 se
 * afirma en el marco en que S2 lo implementa: cero disparos sobre estable y vibrato, y cero
 * convergidas equivocadas bajo la compuerta con reinicio sincronizado. La tabla de la ventana fija
 * se imprime igual, y dice cuantas ciegas pasarian sin el reinicio.
 */
constexpr trend::Threshold kChosen{5.0, 0.05, 12, trend::Magnitude::kDelta};

// ---------------------------------------------------------------------------
// AC-036.1 — el error en CONVERGIDA y si σ lo vio
// ---------------------------------------------------------------------------
struct CaseSummary {
    double settleSec = 0.0;
    double convSec = NAN, convErr = NAN, convSigma = NAN;   ///< primera CONVERGIDA
    bool blindAtConv = false;                              ///< σ ≤ 0,1 con |error| > 0,1 ahi
    int blindWindows = 0;                                  ///< ventanas CONVERGIDAS con |error| > 0,1
    double errAt3s = NAN; bool convAt3s = false;           ///< a los 3 s del asentamiento
};

/// El resumen de una serie de lecturas (sec, convergida, cents, σ) contra la altura final.
template <typename Windows>
CaseSummary summarizeWindows(double settleSec, const Windows& ws) {
    CaseSummary c;
    c.settleSec = settleSec;
    const double at3 = settleSec + 3.0;
    for (const auto& s : ws) {
        const double err = s.cents - kProbeCents;
        if (s.converged && std::isnan(c.convSec)) {
            c.convSec = s.sec; c.convErr = err; c.convSigma = s.sigma;
            c.blindAtConv = std::fabs(err) > kToleranceCents;
        }
        if (s.converged && std::fabs(err) > kToleranceCents) ++c.blindWindows;
        if (std::isnan(c.errAt3s) && s.sec >= at3) { c.errAt3s = err; c.convAt3s = s.converged; }
    }
    return c;
}

/// Lo que PRODUCCION publico, ventana a ventana.
CaseSummary summarizeProduction(const Trajectory& r) { return summarizeWindows(r.settleSec, r.samples); }

TEST(AttackInWindow, AGlideInsideTheWindowLeavesAConvergedReadingWrongAndSigmaBlind) {
    std::printf("\n  [REQ-036] AC-036.1 — barrido de glides sobre D3 a 48 k (ventana de %d × %d = %.2f s), "
                "alimentado %.0f s; asentada = a < %.1f c de la final. SIN compuerta = el estimador de ventana fija "
                "simulado sobre la historia de fases (fiel a 3,8e-13 c); PRODUCCION = lo que el strobe publica hoy\n",
                PhaseSlopeEstimator::kMaxWindows, PhaseSlopeEstimator::kWindowFrames,
                PhaseSlopeEstimator::kMaxWindows * static_cast<double>(PhaseSlopeEstimator::kWindowFrames) / kRate,
                kFeedSeconds, kToleranceCents);
    std::printf("  %-26s %7s | %7s %8s %8s %5s %6s %8s | %7s %8s %6s %7s\n", "caso", "asent_s",
                "conv_s", "err_conv", "sig_conv", "ciega", "vent_c", "err@+3s", "p_conv", "p_err", "p_cieg", "tras_as");
    int blindCases = 0, blindWindows = 0, converged = 0;
    for (const Trajectory& r : glideRuns()) {
        const SimOutcome ungated = simulate(r, kChosen, false, false, nullptr);
        const CaseSummary c = summarizeWindows(r.settleSec, ungated.windows);
        const CaseSummary prod = summarizeProduction(r);
        std::printf("  %-26s %7.2f | %7.2f %+8.3f %8.4f %5s %6d %+8.3f | %7.2f %+8.3f %6d %+7.2f\n", r.label.c_str(),
                    c.settleSec, c.convSec, c.convErr, c.convSigma, c.blindAtConv ? "SI" : "no",
                    c.blindWindows, c.errAt3s, prod.convSec, prod.convErr, prod.blindWindows, prod.convSec - r.settleSec);
        if (!std::isnan(c.convSec)) ++converged;
        if (c.blindAtConv) ++blindCases;
        blindWindows += c.blindWindows;
    }
    std::printf("  resumen SIN compuerta: %d de %zu casos convergen; σ CIEGA en la primera CONVERGIDA en %d; "
                "ventanas convergidas con |error| > %.1f c: %d\n\n",
                converged, glideRuns().size(), blindCases, kToleranceCents, blindWindows);
    RecordProperty("casos_sigma_ciega", blindCases);
    RecordProperty("ventanas_sigma_ciega", blindWindows);
    // Que el instrumento mide: todos los casos terminan convergidos (la nota se asienta y 12 s
    // alcanzan), y hay ventanas ciegas que delatar: es el defecto que S2 arregla, y tiene que seguir
    // siendo visible en el estimador de ventana fija despues del arreglo.
    EXPECT_EQ(converged, static_cast<int>(glideRuns().size()));
    EXPECT_GT(blindWindows, 0) << "ningun glide dejo una CONVERGIDA equivocada sin compuerta: no hay nada que atajar";
}

TEST(AttackInWindow, UnderLightVibratoTheReadingStillConvergesToTheMean) {
    // AC-036.7: el vibrato leve es un control de S2 — lo que hoy hace, lo tiene que seguir haciendo.
    std::printf("\n  [REQ-036] vibrato leve, lo que produccion publica:\n");
    for (const Trajectory& r : vibratoRuns()) {
        ASSERT_FALSE(r.samples.empty());
        const CaseSummary c = summarizeProduction(r);
        const WindowSample& last = r.samples.back();
        std::printf("    %-24s primera CONVERGIDA %5.2f s (err %+6.3f) · ultima %+6.3f c σ %.4f %s\n", r.label.c_str(),
                    c.convSec, c.convErr, last.cents - kProbeCents, last.sigma, last.converged ? "CONVERGIDA" : "midiendo");
        EXPECT_TRUE(last.converged) << r.label << ": no converge a la media bajo vibrato leve";
        EXPECT_NEAR(last.cents, kProbeCents, kToleranceCents) << r.label;
        EXPECT_EQ(r.restarts, 0) << r.label << ": la ventana se reinicio bajo vibrato leve";
    }
}

// ---------------------------------------------------------------------------
// AC-036.4 y AC-036.5 — lo que PRODUCCION tiene que cumplir (S2). Nacieron rojos en S1.
// ---------------------------------------------------------------------------
TEST(AttackInWindow, AC0364_NoConvergedReadingIsWrongWhileTheAttackIsInTheWindow) {
    int blind = 0;
    for (const Trajectory& r : glideRuns()) {
        for (const WindowSample& s : r.samples) {
            if (s.converged && std::fabs(s.cents - kProbeCents) > kToleranceCents) {
                if (blind < 10)
                    ADD_FAILURE() << r.label << " a " << s.sec << " s: CONVERGIDA con " << s.cents - kProbeCents
                                  << " c de error (σ " << s.sigma << ")";
                ++blind;
            }
        }
    }
    EXPECT_EQ(blind, 0) << "ventanas CONVERGIDAS con |error| > 0,1 c sobre el barrido de glides";
}

TEST(AttackInWindow, AC0365_ConvergedAtMostThreeSecondsAfterTheGlideSettles) {
    double worst = 0.0;
    int restarts = 0;
    for (const Trajectory& r : glideRuns()) {
        const CaseSummary c = summarizeProduction(r);
        restarts += r.restarts;
        ASSERT_FALSE(std::isnan(c.convSec)) << r.label << ": nunca convergio en " << kFeedSeconds << " s";
        const double latency = c.convSec - r.settleSec;
        worst = std::max(worst, latency);
        EXPECT_LE(latency, 3.0) << r.label << ": CONVERGIDA " << latency << " s despues del asentamiento";
    }
    std::printf("\n  [REQ-036] AC-036.5: peor latencia CONVERGIDA tras el asentamiento = %.2f s, %d reinicios de ventana en %zu casos\n\n",
                worst, restarts, glideRuns().size());
    RecordProperty("peor_latencia_ms", static_cast<int>(worst * 1000.0));
    RecordProperty("reinicios", restarts);
    // Y la latencia la compra el reinicio: sin ninguno, la compuerta sola tarda 3,3–3,8 s (medido en S1).
    EXPECT_GT(restarts, 0) << "ningun glide reinicio la ventana: la ventana adaptativa no esta actuando";
}

// ---------------------------------------------------------------------------
// AC-036.2 — el estadistico de tendencia, su umbral, y sus controles
// ---------------------------------------------------------------------------
/**
 * EL VEREDICTO DE S1. El umbral se elige donde nota estable y vibrato dan cero disparos y las
 * ventanas con σ ciega disparan en todos sus parciales admitidos; la grilla de abajo lo imprime.
 * (Valores provisionales hasta la primera corrida: la tabla los fija.)
 */
/// Sobre una corrida, cuantas ventanas (de la historia, ventana fija) tienen algun parcial que
/// dispara con `th`, y cuantas fueron evaluables.
int firingWindows(const Trajectory& r, const trend::Threshold& th, int* evaluated) {
    const SimOutcome o = simulate(r, th, false, true, nullptr);
    *evaluated += o.evaluated;
    return o.fires;
}

/// Sobre una corrida de glide, las ventanas CIEGAS del estimador de ventana fija (convergida y
/// |error| > 0,1) que la compuerta SOLA (sin reinicio) dejaria pasar.
int blindWindowsMissed(const Trajectory& r, const trend::Threshold& th, int* blind) {
    *blind += simulate(r, th, false, false, nullptr).blind;
    return simulate(r, th, false, true, nullptr).blind;
}

TEST(AttackInWindow, TheTrendStatisticFiresOnBlindGlidesAndNeverOnStableOrVibrato) {
    // --- las distribuciones, para leer -----------------------------------------------------
    auto extremes = [](const std::vector<Trajectory>& runs, const char* what, bool minOverPartials) {
        double tExt = minOverPartials ? INFINITY : 0.0, dExt = minOverPartials ? INFINITY : 0.0;
        int windows = 0;
        for (const Trajectory& r : runs) {
            const SimOutcome ungated = simulate(r, kChosen, false, false, nullptr);
            for (size_t k = 0; k < r.samples.size(); ++k) {
                const WindowSample& s = r.samples[k];
                const SimWindow& u = ungated.windows[k];
                if (minOverPartials && !(u.converged && std::fabs(u.cents - kProbeCents) > kToleranceCents)) continue;
                double tW = minOverPartials ? INFINITY : 0.0, dW = minOverPartials ? INFINITY : 0.0;
                bool any = false;
                for (int p = 0; p < kPartials; ++p) {
                    if (s.admitted <= 0 || !(s.admitted & (1 << p)) || !s.trend[p].evaluable) continue;
                    any = true;
                    const double at = std::fabs(s.trend[p].tScore), ad = std::fabs(s.trend[p].deltaCents);
                    tW = minOverPartials ? std::min(tW, at) : std::max(tW, at);
                    dW = minOverPartials ? std::min(dW, ad) : std::max(dW, ad);
                }
                if (!any) continue;
                ++windows;
                tExt = minOverPartials ? std::min(tExt, tW) : std::max(tExt, tW);
                dExt = minOverPartials ? std::min(dExt, dW) : std::max(dExt, dW);
            }
        }
        std::printf("  %-34s %5d ventanas: |T| %s = %9.3f   |Δ| %s = %8.4f c\n", what, windows,
                    minOverPartials ? "min" : "max", tExt, minOverPartials ? "min" : "max", dExt);
    };
    std::printf("\n  [REQ-036] AC-036.2 — el estadistico de tendencia (mitades de la ventana): T = (b2−b1)/√(se1²+se2²), Δ en cents\n");
    extremes(stableRuns(), "estable (14 cuerdas × 2 rates)", false);
    extremes(vibratoRuns(), "vibrato (±5 c @5 Hz, ±10 c @6 Hz)", false);
    extremes(glideRuns(), "glides, ventanas con σ CIEGA", true);

    // Por caso de glide: el minimo sobre parciales en sus ventanas ciegas, para ver cual aprieta.
    std::printf("  %-26s %6s | %9s %8s | %9s %8s\n", "caso", "ciegas", "min|T|", "min|Δ|", "max|T|", "max|Δ|");
    for (const Trajectory& r : glideRuns()) {
        double tMin = INFINITY, dMin = INFINITY, tMax = 0.0, dMax = 0.0;
        int blind = 0;
        const SimOutcome ungated = simulate(r, kChosen, false, false, nullptr);
        for (size_t k = 0; k < r.samples.size(); ++k) {
            const WindowSample& s = r.samples[k];
            const SimWindow& u = ungated.windows[k];
            if (!(u.converged && std::fabs(u.cents - kProbeCents) > kToleranceCents)) continue;
            ++blind;
            for (int p = 0; p < kPartials; ++p) {
                if (s.admitted <= 0 || !(s.admitted & (1 << p)) || !s.trend[p].evaluable) continue;
                tMin = std::min(tMin, std::fabs(s.trend[p].tScore)); dMin = std::min(dMin, std::fabs(s.trend[p].deltaCents));
                tMax = std::max(tMax, std::fabs(s.trend[p].tScore)); dMax = std::max(dMax, std::fabs(s.trend[p].deltaCents));
            }
        }
        if (blind == 0) std::printf("  %-26s %6d |\n", r.label.c_str(), blind);
        else std::printf("  %-26s %6d | %9.2f %8.4f | %9.2f %8.4f\n", r.label.c_str(), blind, tMin, dMin, tMax, dMax);
    }

    // --- los outliers: donde estable sube y donde las ciegas bajan ------------------------------
    std::printf("\n  ventanas ESTABLES con |T| > 3 y |Δ| > 0,05 c (cuerda, n, T, Δ, error de la lectura):\n");
    int shown = 0;
    for (const Trajectory& r : stableRuns()) {
        for (const WindowSample& s : r.samples) {
            for (int p = 0; p < kPartials; ++p) {
                if (s.admitted <= 0 || !(s.admitted & (1 << p)) || !s.trend[p].evaluable) continue;
                if (std::fabs(s.trend[p].tScore) > 3.0 && std::fabs(s.trend[p].deltaCents) > 0.05 && shown < 40) {
                    std::printf("    %-28s n=%2d p%d  T=%+8.2f  Δ=%+8.4f c  sesgo=%+8.4f c  lectura %+7.4f c (σ %.4f)%s\n", r.label.c_str(),
                                s.count, p + 1, s.trend[p].tScore, s.trend[p].deltaCents, s.trend[p].biasCents, s.cents - kProbeCents, s.sigma,
                                s.converged ? " CONV" : "");
                    ++shown;
                }
            }
        }
    }
    std::printf("  (las ventanas ciegas con |T| < 5 se ven en la grilla de ventana fija: son las que pasan)\n");

    // --- la grilla de umbrales: falsos positivos (estable + vibrato) y ciegas perdidas ----------
    const double kTs[] = {2.0, 3.0, 4.0, 5.0, 6.0, 8.0};
    const double kDs[] = {0.0, 0.02, 0.05, 0.1, 0.15, 0.2};
    for (int minW : {12, 16, 20}) {
        std::printf("\n  grilla con minimo %d ventanas, magnitud = max(|Δ|, |sesgo|) (falsos positivos estable+vibrato / "
                    "ventanas ciegas que pasan):\n  %6s", minW, "T\\mag");
        for (double d : kDs) std::printf(" %9.2f", d);
        std::printf("\n");
        for (double tt : kTs) {
            std::printf("  %6.1f", tt);
            for (double d : kDs) {
                const trend::Threshold th{tt, d, minW, kChosen.magnitude};
                int fp = 0, ev = 0, blind = 0, missed = 0;
                for (const Trajectory& r : stableRuns()) fp += firingWindows(r, th, &ev);
                for (const Trajectory& r : vibratoRuns()) fp += firingWindows(r, th, &ev);
                for (const Trajectory& r : glideRuns()) missed += blindWindowsMissed(r, th, &blind);
                std::printf(" %4d/%-4d", fp, missed);
            }
            std::printf("\n");
        }
    }
    // El margen del T sobre estable, segun desde que n se mira.
    for (int minW : {8, 10, 12, 16}) {
        double tMax = 0.0, dMax = 0.0;
        for (const Trajectory& r : stableRuns())
            for (const WindowSample& s : r.samples)
                for (int p = 0; p < kPartials; ++p) {
                    if (s.admitted <= 0 || !(s.admitted & (1 << p)) || !s.trend[p].evaluable || s.count < minW) continue;
                    if (std::fabs(s.trend[p].deltaCents) > 0.05) tMax = std::max(tMax, std::fabs(s.trend[p].tScore));
                    dMax = std::max(dMax, std::fabs(s.trend[p].deltaCents));
                }
        std::printf("  estable desde n >= %2d: max |T| (con |Δ| > 0,05) = %6.2f   max |Δ| = %.4f c\n", minW, tMax, dMax);
    }

    // --- el veredicto, afirmado -----------------------------------------------------------------
    int fp = 0, evaluated = 0, blind = 0, missed = 0;
    for (const Trajectory& r : stableRuns()) fp += firingWindows(r, kChosen, &evaluated);
    const int fpStable = fp;
    for (const Trajectory& r : vibratoRuns()) fp += firingWindows(r, kChosen, &evaluated);
    for (const Trajectory& r : glideRuns()) missed += blindWindowsMissed(r, kChosen, &blind);
    // Y bajo la compuerta con reinicio sincronizado: ningun reinicio sobre estable ni vibrato, y
    // ninguna convergida equivocada sobre el barrido.
    int restartsOnControls = 0, blindAdaptive = 0, convergedAdaptive = 0;
    for (const Trajectory& r : stableRuns()) restartsOnControls += simulate(r, kChosen, true, true, nullptr, nullptr, true).restarts;
    for (const Trajectory& r : vibratoRuns()) restartsOnControls += simulate(r, kChosen, true, true, nullptr, nullptr, true).restarts;
    for (const Trajectory& r : glideRuns()) {
        const SimOutcome a = simulate(r, kChosen, true, true, nullptr, nullptr, true);
        blindAdaptive += a.blind;
        if (!std::isnan(a.convSec)) ++convergedAdaptive;
    }
    std::printf("\n  elegido: |T| > %.1f y |Δ| > %.2f c desde %d ventanas → estable %d disparos, vibrato %d, de %d ventanas "
                "evaluadas; sobre la ventana FIJA: ciegas %d, de las que pasan %d; con reinicio sincronizado: %d reinicios "
                "sobre estable+vibrato, %d convergidas equivocadas, %d de %zu casos convergen\n\n", kChosen.t,
                kChosen.deltaCents, kChosen.minWindows, fpStable, fp - fpStable, evaluated, blind, missed,
                restartsOnControls, blindAdaptive, convergedAdaptive, glideRuns().size());
    RecordProperty("umbral_T", std::to_string(kChosen.t));
    RecordProperty("umbral_delta_cents", std::to_string(kChosen.deltaCents));
    RecordProperty("umbral_min_ventanas", kChosen.minWindows);
    RecordProperty("ciegas_que_pasan_sin_reinicio", missed);
    EXPECT_GT(evaluated, 0);
    EXPECT_EQ(fp, 0) << "el estadistico dispara sobre nota estable o vibrato: apagaria al afinador sobre lo que hoy converge";
    EXPECT_EQ(restartsOnControls, 0) << "la ventana adaptativa se reinicia sobre nota estable o vibrato";
    EXPECT_GT(blind, 0) << "el estimador de ventana fija ya no deja ciegas: el instrumento perdio su referencia";
    EXPECT_EQ(blindAdaptive, 0) << "bajo la compuerta con reinicio sincronizado sigue habiendo CONVERGIDAS con |error| > 0,1 c";
    EXPECT_EQ(convergedAdaptive, static_cast<int>(glideRuns().size())) << "hay casos que no convergen bajo la compuerta";
}

struct Policy { const char* name; trend::Threshold th; bool sync = false; bool majority = false; };

const std::vector<Policy>& policies() {
    using trend::Magnitude;
    static const std::vector<Policy> kPolicies = {
        {"Δ    T4 >0.10 c min 8 ", {4.0, 0.10, 8, Magnitude::kDelta}},
        {"Δ    T3 >0.15 c min12 ", {3.0, 0.15, 12, Magnitude::kDelta}},
        {"Δ    T3 >0.15 c min16 ", {3.0, 0.15, 16, Magnitude::kDelta}},
        {"sesgo T3 >0.10 c min12", {3.0, 0.10, 12, Magnitude::kBias}},
        {"sesgo T3 >0.10 c min16", {3.0, 0.10, 16, Magnitude::kBias}},
        {"sesgo T4 >0.10 c min16", {4.0, 0.10, 16, Magnitude::kBias}},
        {"sesgo T3 >0.05 c min16", {3.0, 0.05, 16, Magnitude::kBias}},
        {"sesgo T3 >0.10 c min24", {3.0, 0.10, 24, Magnitude::kBias}},
        {"sesgo T3 >0.05 c min24", {3.0, 0.05, 24, Magnitude::kBias}},
        {"Δ    T3 >0.10 c min16 ", {3.0, 0.10, 16, Magnitude::kDelta}},
        {"max  T3 >0.10 c min16 ", {3.0, 0.10, 16, Magnitude::kMax}},
        {"max  T3 >0.05 c min12 ", {3.0, 0.05, 12, Magnitude::kMax}},
        {"max  T3 >0.05 c min16 ", {3.0, 0.05, 16, Magnitude::kMax}},
        {"max  T3 >0.05 c min20 ", {3.0, 0.05, 20, Magnitude::kMax}},
        {"max  T4 >0.05 c min16 ", {4.0, 0.05, 16, Magnitude::kMax}},
        {"max  T2 >0.05 c min16 ", {2.0, 0.05, 16, Magnitude::kMax}},
        {"max  T3 >0.05 c min24 ", {3.0, 0.05, 24, Magnitude::kMax}},
        {"max  T3 >0.03 c min16 ", {3.0, 0.03, 16, Magnitude::kMax}},
        {"max  T3 >0.02 c min16 ", {3.0, 0.02, 16, Magnitude::kMax}},
        {"max  T3 >0.10 c min24 ", {3.0, 0.10, 24, Magnitude::kMax}},
        {"max  T3 >0.05 c min12 sync", {3.0, 0.05, 12, Magnitude::kMax}, true},
        {"max  T3 >0.05 c min16 sync", {3.0, 0.05, 16, Magnitude::kMax}, true},
        {"max  T3 >0.10 c min16 sync", {3.0, 0.10, 16, Magnitude::kMax}, true},
        {"max  T3 >0.05 c min20 sync", {3.0, 0.05, 20, Magnitude::kMax}, true},
        {"max  T3 >0.05 c min24 sync", {3.0, 0.05, 24, Magnitude::kMax}, true},
        {"sesgo T3 >0.05 c min16 sync", {3.0, 0.05, 16, Magnitude::kBias}, true},
        {"Δ    T3 >0.05 c min16 sync", {3.0, 0.05, 16, Magnitude::kDelta}, true},
        {"Δ    T5 >0.05 c min12 sync", {5.0, 0.05, 12, Magnitude::kDelta}, true},
        {"Δ    T5 >0.05 c min12 mayoria", {5.0, 0.05, 12, Magnitude::kDelta}, true, true},
        {"Δ    T4 >0.05 c min12 mayoria", {4.0, 0.05, 12, Magnitude::kDelta}, true, true},
        {"Δ    T5 >0.10 c min12 mayoria", {5.0, 0.10, 12, Magnitude::kDelta}, true, true},
    };
    return kPolicies;
}

TEST(AttackInWindow, ThePolicyTableThatChoosesTheStatistic) {
    std::printf("\n  [REQ-036] politicas: falsos positivos sobre estable/vibrato (ventanas de produccion), y sobre el "
                "barrido de glides la compuerta SOLA y la ventana ADAPTATIVA (ciegas = convergidas con |error| > 0,1 c; "
                ">3s = casos con CONVERGIDA a mas de 3 s del asentamiento; peor = latencia maxima)\n");
    std::printf("  %-28s | %6s %6s | %6s %4s %6s %5s | %6s %4s %6s %5s %5s %5s\n", "politica", "fp_est", "fp_vib",
                "ciegas", ">3s", "peor_s", "nunca", "ciegas", ">3s", "peor_s", "nunca", "reini", "veto1");
    for (const Policy& pol : policies()) {
        int fpS = 0, fpV = 0, ev = 0;
        for (const Trajectory& r : stableRuns()) fpS += firingWindows(r, pol.th, &ev);
        for (const Trajectory& r : vibratoRuns()) fpV += firingWindows(r, pol.th, &ev);
        int gB = 0, gO = 0, gN = 0, aB = 0, aO = 0, aN = 0, aR = 0, aV = 0;
        double gW = 0.0, aW = 0.0;
        for (const Trajectory& r : glideRuns()) {
            const SimOutcome g = simulate(r, pol.th, false, true, nullptr);
            const SimOutcome a = simulate(r, pol.th, true, true, nullptr, nullptr, pol.sync, pol.majority);
            gB += g.blind; aB += a.blind; aR += a.restarts; aV += a.loneVetoes;
            if (std::isnan(g.convSec)) ++gN; else { const double l = g.convSec - r.settleSec; gW = std::max(gW, l); if (l > 3.0) ++gO; }
            if (std::isnan(a.convSec)) ++aN; else { const double l = a.convSec - r.settleSec; aW = std::max(aW, l); if (l > 3.0) ++aO; }
        }
        std::printf("  %-28s | %6d %6d | %6d %4d %6.2f %5d | %6d %4d %6.2f %5d %5d %5d\n", pol.name, fpS, fpV,
                    gB, gO, gW, gN, aB, aO, aW, aN, aR, aV);
    }
    std::printf("\n");
    // La grilla entera con reinicio SINCRONIZADO: es la forma que S2 va a implementar, y el corpus
    // (test_corpus_gate.cpp) la barre con los mismos ejes para que las dos tablas se lean juntas.
    std::printf("  grilla con reinicio sincronizado, magnitud Δ (fp estable | fp vibrato | ciegas adaptativa | casos > 3 s | peor latencia | reinicios):\n");
    std::printf("  %5s %6s %5s | %6s %6s | %6s %4s %6s %5s\n", "T", "Δ", "min", "fp_est", "fp_vib", "ciegas", ">3s", "peor_s", "reini");
    for (double tt : {3.0, 4.0, 5.0, 6.0}) for (double d : {0.05, 0.10}) for (int minW : {8, 12, 16}) {
        const trend::Threshold th{tt, d, minW, trend::Magnitude::kDelta};
        int fpS = 0, fpV = 0, ev = 0, aB = 0, aO = 0, aR = 0;
        double aW = 0.0;
        for (const Trajectory& r : stableRuns()) fpS += firingWindows(r, th, &ev);
        for (const Trajectory& r : vibratoRuns()) fpV += firingWindows(r, th, &ev);
        for (const Trajectory& r : glideRuns()) {
            const SimOutcome a = simulate(r, th, true, true, nullptr, nullptr, true);
            aB += a.blind; aR += a.restarts;
            if (!std::isnan(a.convSec)) { const double l = a.convSec - r.settleSec; aW = std::max(aW, l); if (l > 3.0) ++aO; }
        }
        std::printf("  %5.1f %6.2f %5d | %6d %6d | %6d %4d %6.2f %5d\n", tt, d, minW, fpS, fpV, aB, aO, aW, aR);
    }
    std::printf("\n");
    // El detalle de las ciegas que quedan bajo la adaptativa, para las politicas que dejan pocas.
    for (const Policy& pol : policies()) {
        int total = 0;
        std::vector<std::string> rows;
        for (const Trajectory& r : glideRuns()) {
            std::vector<BlindDetail> d;
            const SimOutcome a = simulate(r, pol.th, true, true, nullptr, &d, pol.sync, pol.majority);
            total += a.blind;
            for (const BlindDetail& b : d) {
                char buf[200];
                std::snprintf(buf, sizeof buf, "    %-26s t=%5.2f n=%2d k=%d  error %+7.4f c  σ %.4f  |T|min %5.2f  Δ %+7.4f  sesgo %+7.4f",
                              r.label.c_str(), b.sec, b.windows, b.used, b.err, b.sigma, b.tMin, b.delta, b.bias);
                std::string row = buf;
                if (b.used == 1) {
                    for (int p = 0; p < kPartials; ++p) {
                        std::snprintf(buf, sizeof buf, " | p%d %c n=%2d c=%+7.3f T=%+6.1f b=%+6.3f", p + 1, b.why[p], b.cnt[p], b.pcents[p], b.pt[p], b.pbias[p]);
                        row += buf;
                    }
                }
                rows.push_back(row);
            }
        }
        if (total == 0 || total > 40) continue;
        std::printf("  ciegas bajo la adaptativa con %s (%d):\n", pol.name, total);
        for (const std::string& row : rows) std::printf("%s\n", row.c_str());
    }
    std::printf("\n");
}

TEST(AttackInWindow, TheGateSimulatedFromOutsideWithAndWithoutTheAdaptiveWindow) {
    std::printf("\n  [REQ-036] la compuerta simulada (|T| > %.1f, |Δ| > %.2f c, desde %d ventanas) sobre el barrido: "
                "sola, y con ventana adaptativa SINCRONIZADA\n", kChosen.t, kChosen.deltaCents, kChosen.minWindows);
    std::printf("  %-26s %7s | %7s %8s %5s | %7s %8s %7s %5s | %7s %8s %7s %5s %4s %4s\n", "caso", "asent_s",
                "fija_s", "fija_err", "ciega", "gate_s", "gate_err", "tras_as", "ciega", "adap_s", "adap_err", "tras_as", "ciega", "rein", "veto");
    double fidelity = 0.0;
    int gateBlind = 0, gateOver = 0, adapBlind = 0, adapOver = 0, gateNever = 0, adapNever = 0;
    double gateWorst = 0.0, adapWorst = 0.0;
    for (const Trajectory& r : glideRuns()) {
        const SimOutcome today = simulate(r, kChosen, false, false, &fidelity);   // ventana fija, sin compuerta
        const SimOutcome gate = simulate(r, kChosen, false, true, nullptr);
        const SimOutcome adap = simulate(r, kChosen, true, true, nullptr, nullptr, true);
        const double gl = gate.convSec - r.settleSec, al = adap.convSec - r.settleSec;
        std::printf("  %-26s %7.2f | %7.2f %+8.3f %5d | %7.2f %+8.3f %+7.2f %5d | %7.2f %+8.3f %+7.2f %5d %4d %4d\n",
                    r.label.c_str(), r.settleSec, today.convSec, today.convErr, today.blind, gate.convSec, gate.convErr,
                    gl, gate.blind, adap.convSec, adap.convErr, al, adap.blind, adap.restarts, adap.loneVetoes);
        gateBlind += gate.blind; adapBlind += adap.blind;
        if (std::isnan(gate.convSec)) ++gateNever; else { gateWorst = std::max(gateWorst, gl); if (gl > 3.0) ++gateOver; }
        if (std::isnan(adap.convSec)) ++adapNever; else { adapWorst = std::max(adapWorst, al); if (al > 3.0) ++adapOver; }
    }
    std::printf("  fidelidad del simulador (peor |lectura externa − partialCents| sin compuerta): %.2e c\n", fidelity);
    std::printf("  compuerta sola:       convergidas equivocadas %d · sin converger en %.0f s %d · a mas de 3 s del asentamiento %d (peor %+.2f s)\n",
                gateBlind, kFeedSeconds, gateNever, gateOver, gateWorst);
    std::printf("  ventana adaptativa:   convergidas equivocadas %d · sin converger en %.0f s %d · a mas de 3 s del asentamiento %d (peor %+.2f s)\n\n",
                adapBlind, kFeedSeconds, adapNever, adapOver, adapWorst);
    EXPECT_LT(fidelity, 1e-6) << "el simulador externo no reproduce la lectura por parcial del strobe";

    // La HISTORIA de fases del parcial 1, ventana a ventana, contra la altura verdadera: el
    // incremento de fase desenvuelta entre dos cierres es la frecuencia media de esa ventana.
    // La historia del parcial 1 contra la altura verdadera: el incremento de fase entre dos cierres
    // es la frecuencia media de esa ventana. Lee +0,087 c por encima de la altura del FUNDAMENTAL,
    // y no es un defecto: es 600·log2(1+B) con B = 1e-4, el estiramiento inarmonico del propio
    // parcial 1. El ajuste de la serie lo descuenta con k ≥ 2; un parcial solo lo publica tal cual.
    std::printf("  historia del parcial 1 (incremento de fase → cents) contra la altura verdadera del fundamental, "
                "caso +30 c / tau 1 s (el exceso constante de ~0,087 c es 600·log2(1+B)):\n");
    for (const Trajectory& r : glideRuns()) {
        if (r.label != "glide +30.0 c, tau 1.0 s") continue;
        const double k = trend::centsPerSlopeUnit(r.rate, r.targetHz);
        for (size_t i = 1; i < r.samples.size(); ++i) {
            const double t1 = r.samples[i].sec, t0 = r.samples[i - 1].sec;
            if (t1 < 6.8 || t1 > 7.2) continue;
            const double inc = (r.samples[i].phase[0] - r.samples[i - 1].phase[0]) * k;
            double truth = 0.0;
            for (int q = 0; q < 50; ++q) truth += kProbeCents + 30.0 * std::exp(-(t0 + (t1 - t0) * (q + 0.5) / 50.0) / 1.0);
            truth /= 50.0;
            std::printf("    %5.2f-%5.2f s: incremento %+8.4f c   verdadera %+8.4f c   p1 lee %+8.4f   n=%d\n",
                        t0, t1, inc, truth, r.samples[i].pCents[0], r.samples[i].count);
        }
    }
    RecordProperty("compuerta_convergidas_equivocadas", gateBlind);
    RecordProperty("compuerta_casos_mas_de_3s", gateOver);
    RecordProperty("adaptativa_convergidas_equivocadas", adapBlind);
    RecordProperty("adaptativa_casos_mas_de_3s", adapOver);
}

}  // namespace
}  // namespace wma_test
