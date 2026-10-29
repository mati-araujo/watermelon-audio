/**
 * test_early_reading.cpp — REQ-058 S1. LA LECTURA TEMPRANA: σ = residuos + deriva sobre la serie densa
 * (D-1), `MEASURING` con cents antes del veredicto (D-2), y el estado que solo el veredicto convierte en
 * `CONVERGED` (AC-058.6).
 *
 * Tres niveles, de abajo hacia arriba:
 *   · el ajuste corto PURO (`InterleavedPhaseEstimator::fitDensePhases`): geometria del solape, centro
 *     de la ventana, deriva;
 *   · el estimador de cuatro rejillas con audio: la serie densa, el silencio, el segmento;
 *   · el motor entero (`AnalysisThread`, tandas de 256): cobertura de 2σ, no inflacion, estados.
 *
 * Toda cifra de umbral de abajo es la de la spec (AC-058.4: 95 %, AC-058.5: 3×) o la de la tarea 1.2
 * ([0,8; 1,25]); lo que el test mide se imprime con `RecordProperty` y es la fuente de las cifras de la
 * etapa.
 */

#include "support/SyntheticSignal.h"

#include "AnalysisRing.h"
#include "AnalysisSnapshot.h"
#include "AnalysisThread.h"
#include "PhaseSlopeEstimator.h"
#include "StrobeTracker.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace wma_test {
namespace {

using wma::analysis::InterleavedPhaseEstimator;
using wma::analysis::StrobeTracker;

constexpr int kHop = InterleavedPhaseEstimator::kHopFrames;
constexpr int kWin = wma::analysis::PhaseSlopeEstimator::kWindowFrames;

// ---------------------------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------------------------

/// xorshift32: ruido BLANCO de verdad (un LCG sobre el indice es periodico y el motor le encuentra
/// altura: la leccion de REQ-013 S4 en Tunio).
struct XorShift {
    uint32_t x;
    explicit XorShift(uint32_t seed) : x(seed ? seed : 2463534242u) {}
    double uniform() {   // en [-1, 1)
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return (static_cast<double>(x) / 4294967296.0) * 2.0 - 1.0;
    }
};

void addWhiteNoise(std::vector<float>* sig, double amp, uint32_t seed) {
    XorShift r(seed);
    for (float& s : *sig) s += static_cast<float>(amp * r.uniform());
}

double centsFromSlope(double radPerHop, int rate, double targetHz) {
    return 1200.0 * std::log2(1.0 + radPerHop * rate / (2.0 * M_PI * kHop) / targetHz);
}

/// Una publicacion del motor, con las sondas que la etapa agrega.
struct Pub {
    double sec = 0.0;
    int state = -1;
    double cents = NAN;
    double sigma = NAN;
    bool early = false;
    int k = 0;
    bool bMeasured = false;
};

/// Alimenta `mono` al motor entero en tandas de 256 (como el instrumento), objetivo puesto con el ring
/// vacio, y registra cada publicacion.
std::vector<Pub> runEngine(const std::vector<float>& mono, int rate, double target) {
    wma::analysis::AnalysisRing ring;
    wma::analysis::AnalysisSnapshot snap;
    wma::analysis::AnalysisThread th(ring, snap);
    ring.setCaptureRate(rate);
    th.setTargetHz(target);
    (void)th.drainOnce();
    std::vector<Pub> out;
    std::vector<float> st(512);
    for (size_t off = 0; off < mono.size(); off += 256) {
        const int c = static_cast<int>(std::min<size_t>(256, mono.size() - off));
        for (int i = 0; i < c; ++i) st[2 * i] = st[2 * i + 1] = mono[off + i];
        ring.writeStereo(st.data(), c);
        for (;;) {
            const auto o = th.drainOnce();
            if (o == wma::analysis::AnalysisThread::DrainOutcome::kRingEmpty) break;
            if (o != wma::analysis::AnalysisThread::DrainOutcome::kPublished) continue;
            float v[wma::analysis::kSnapshotValueCount];
            if (!snap.read(v)) continue;
            Pub p;
            p.sec = static_cast<double>(ring.readPosition()) / rate;
            p.state = static_cast<int>(v[wma::analysis::kSnapState]);
            p.cents = v[wma::analysis::kSnapCents];
            p.sigma = v[wma::analysis::kSnapUncertainty];
            p.early = th.strobe().readingIsEarly();
            p.k = th.strobe().partialsUsed();
            p.bMeasured = v[wma::analysis::kSnapInharmonicityMeasured] > 0.5f;
            out.push_back(p);
        }
    }
    return out;
}

double median(std::vector<double> v) {
    if (v.empty()) return NAN;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/// Cobertura y no inflacion de un conjunto de publicaciones contra una verdad.
struct Honesty {
    int n = 0;
    int covered = 0;
    int convergedEarly = 0;          ///< publicaciones CONVERGED que vinieron del camino temprano
    int convergedBigSigma = 0;       ///< CONVERGED con σ > 0,1
    int bFromEarly = 0;              ///< tempranas con B publicado
    std::vector<double> ratio;       ///< σ / |error|
    double coverage() const { return n ? static_cast<double>(covered) / n : NAN; }
    double medianRatio() const { return median(ratio); }
    void add(const Honesty& o) {
        n += o.n;
        covered += o.covered;
        convergedEarly += o.convergedEarly;
        convergedBigSigma += o.convergedBigSigma;
        bFromEarly += o.bFromEarly;
        ratio.insert(ratio.end(), o.ratio.begin(), o.ratio.end());
    }
};

/// Juzga las publicaciones TEMPRANAS con cents contra `truth(t)` (los cents verdaderos del f0 contra
/// el objetivo en el instante de la publicacion: la altura instantanea, D4). `minK`/`maxK` filtran por
/// parciales usados.
Honesty judgeEarly(const std::vector<Pub>& pubs, const std::function<double(double)>& truth, int minK = 1,
                   int maxK = 4) {
    Honesty h;
    for (const Pub& p : pubs) {
        if (p.state == wma::analysis::kStateConverged && p.early) ++h.convergedEarly;
        if (p.state == wma::analysis::kStateConverged && p.sigma > StrobeTracker::kConvergedUncertaintyCents)
            ++h.convergedBigSigma;
        if (std::isnan(p.cents) || !p.early) continue;
        if (p.bMeasured) ++h.bFromEarly;
        if (p.k < minK || p.k > maxK) continue;
        const double err = p.cents - truth(p.sec);
        ++h.n;
        if (std::fabs(err) <= 2.0 * p.sigma) ++h.covered;
        if (std::fabs(err) > 0.0) h.ratio.push_back(p.sigma / std::fabs(err));
    }
    return h;
}

// ---------------------------------------------------------------------------------------------
// La sintesis estable (R-PITCH-53): 6 cuerdas de guitarra × 2 rates, 1–4 parciales 1/n, B = 1e-4,
// +3 c, ruido blanco a 0,001 (−60 dBFS). La verdad es C: el desvio del FUNDAMENTAL ideal.
// ---------------------------------------------------------------------------------------------

constexpr double kStableDevCents = 3.0;
constexpr double kStableB = 1e-4;
constexpr double kGuitar[6] = {82.40689, 110.0, 146.83238, 195.99772, 246.94165, 329.62756};

struct StableRun {
    int partials = 0;
    std::vector<std::vector<Pub>> runs;
};

const StableRun& stableRuns(int partials) {
    static StableRun cache[5];
    StableRun& c = cache[partials];
    if (c.partials == partials) return c;
    c.partials = partials;
    uint32_t seed = 1000u + static_cast<uint32_t>(partials);
    for (int rate : {44100, 48000}) {
        for (double f0 : kGuitar) {
            const int frames = 4 * rate;
            auto sig = partialsWithPitchTrajectory(detune(f0, kStableDevCents), kStableB,
                                                   oneOverNAmplitudes(partials, 0.3), rate, frames,
                                                   [](double) { return 0.0; });
            addWhiteNoise(&sig, 0.001, seed++);
            c.runs.push_back(runEngine(sig, rate, f0));
        }
    }
    return c;
}

Honesty stableHonesty(int partials, int minK = 1, int maxK = 4) {
    Honesty h;
    for (const auto& pubs : stableRuns(partials).runs)
        h.add(judgeEarly(pubs, [](double) { return kStableDevCents; }, minK, maxK));
    return h;
}

// ---------------------------------------------------------------------------------------------
// Los 24 glides de REQ-036: D3, 48 k, B = 1e-4, 4 parciales, sonda +1 c, X ∈ {±2, ±5, ±10, ±30} c,
// τ ∈ {0,2; 0,5; 1} s. La verdad es la altura INSTANTANEA en el instante de la publicacion (D4).
// ---------------------------------------------------------------------------------------------

constexpr double kGlideF0 = 146.83238;
constexpr double kGlideProbe = 1.0;

struct GlideRun {
    double x = 0.0;
    double tau = 0.0;
    std::vector<Pub> pubs;
};

const std::vector<GlideRun>& glideRuns() {
    static const std::vector<GlideRun> kRuns = [] {
        std::vector<GlideRun> v;
        for (double sign : {-1.0, 1.0})
            for (double x : {2.0, 5.0, 10.0, 30.0})
                for (double tau : {0.2, 0.5, 1.0}) {
                    constexpr int rate = 48000;
                    const auto sig = glidingString(detune(kGlideF0, kGlideProbe), sign * x, tau, 1e-4, 4, rate, 6 * rate);
                    v.push_back({sign * x, tau, runEngine(sig, rate, kGlideF0)});
                }
        return v;
    }();
    return kRuns;
}

std::function<double(double)> glideTruth(double x, double tau) {
    return [x, tau](double t) { return kGlideProbe + x * std::exp(-std::max(t, 0.0) / tau); };
}

/// Por fase de la nota (D21): (0, 0,25], (0,25, 1], > 1 s.
int phaseOf(double sec) { return sec <= 0.25 ? 0 : (sec <= 1.0 ? 1 : 2); }

// =============================================================================================
// D-1 — el ajuste corto, puro
// =============================================================================================

TEST(EarlyReading, OverlapFactorMatchesTheHannWindow) {
    // AC-058.4/058.5 (tarea 1.2). Recalcula ρ(d) de la Hann de 4096 con hop 1024 y, para cada n, el
    // factor √(Var verdadera del MCO bajo esa covarianza / E[s²]·(X'X)⁻¹) de `b` y de `c`.
    // Mutante `f = 1` (sacar la correccion) ⇒ rojo aca y en `ReportedSigmaMatchesTheScatter`.
    std::vector<double> w(kWin);
    for (int i = 0; i < kWin; ++i) w[i] = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / kWin);
    double ww = 0.0;
    for (double x : w) ww += x * x;
    double rho[4];
    for (int d = 0; d < 4; ++d) {
        double acc = 0.0;
        for (int i = 0; i + d * kHop < kWin; ++i) acc += w[i] * w[i + d * kHop];
        rho[d] = acc / ww;
    }
    EXPECT_NEAR(rho[1], 0.6592, 1e-4);
    EXPECT_NEAR(rho[2], 0.1667, 1e-4);
    EXPECT_NEAR(rho[3], 0.0075, 1e-4);
    for (int n = 4; n <= InterleavedPhaseEstimator::kEarlyWindow; ++n) {
        // Columnas ortogonales (abscisas centradas): (X'X) es diagonal.
        std::vector<double> u(n), v(n);
        double su2 = 0.0;
        for (int i = 0; i < n; ++i) { u[i] = i - 0.5 * (n - 1); su2 += u[i] * u[i]; }
        const double u2m = su2 / n;
        double sv2 = 0.0;
        for (int i = 0; i < n; ++i) { v[i] = u[i] * u[i] - u2m; sv2 += v[i] * v[i]; }
        auto S = [&](int i, int j) { const int d = std::abs(i - j); return d < 4 ? rho[d] : 0.0; };
        // Var verdadera de b = u'Su / su2², de c = v'Sv / sv2².
        double uSu = 0.0, vSv = 0.0;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) { uSu += u[i] * S(i, j) * u[j]; vSv += v[i] * S(i, j) * v[j]; }
        // E[s²] = tr((I − H)·S)/(n − 3), H = proyector sobre {1, u, v}.
        double trHS = 0.0;
        for (int i = 0; i < n; ++i)
            for (int k = 0; k < n; ++k) {
                const double h = 1.0 / n + u[i] * u[k] / su2 + v[i] * v[k] / sv2;
                trHS += h * S(k, i);
            }
        const double es2 = (n - trHS) / (n - 3);
        const double fs = std::sqrt((uSu / (su2 * su2)) / (es2 / su2));
        const double fc = std::sqrt((vSv / (sv2 * sv2)) / (es2 / sv2));
        EXPECT_NEAR(InterleavedPhaseEstimator::kOverlapSlopeFactor[n - 4], fs, 1e-4) << "n = " << n;
        EXPECT_NEAR(InterleavedPhaseEstimator::kOverlapCurvatureFactor[n - 4], fc, 1e-4) << "n = " << n;
    }
}

TEST(EarlyReading, ReadsTheCentreOfTheWindowNotAPrediction) {
    // AC-058.2 / R-PITCH-67 (D4). Una fase CUADRATICA exacta (frecuencia que cambia linealmente):
    // `cents` es la frecuencia del CENTRO de las 8 fases, no la del borde (extrapolar).
    // Mutante «derivada en el borde» ⇒ lee 2,1 c de mas.
    constexpr int rate = 48000;
    constexpr double target = 110.0;
    const double f0 = 0.010, k = 0.006;    // rad/hop y rad/hop²: un glide de ~5 c en la ventana
    std::vector<double> ph(8);
    for (int i = 0; i < 8; ++i) ph[i] = 0.3 + f0 * i + 0.5 * k * i * i;
    const auto f = InterleavedPhaseEstimator::fitDensePhases(ph.data(), 8, rate, target);
    ASSERT_EQ(f.n, 8);
    const double centre = centsFromSlope(f0 + k * 3.5, rate, target);
    const double edge = centsFromSlope(f0 + k * 7.0, rate, target);
    EXPECT_NEAR(f.cents, centre, 1e-6);
    EXPECT_GT(std::fabs(edge - centre), 1.0);
    // Sin ruido los residuos del cuadratico son cero: σ tambien. La deriva es el cambio de frecuencia
    // del centro al borde: 2·(k/2)·3,5 = 3,5·k rad/hop, con signo.
    EXPECT_NEAR(f.sigmaCents, 0.0, 1e-9);
    EXPECT_NEAR(f.driftCents, edge - centre, 0.01);   // la deriva se convierte linealizada (dc/df en el objetivo)
}

TEST(EarlyReading, TheShortFitNeedsFivePhasesAndUsesTheLastEight) {
    // D-1: n ≥ 5 (2 g.l.) y W = 8. Una serie de 12 fases cuyas 4 primeras son basura lee la recta de
    // las 8 ultimas. Mutante W = 12 ⇒ la basura entra y lee otra cosa.
    constexpr int rate = 44100;
    std::vector<double> ph(12);
    for (int i = 0; i < 12; ++i) ph[i] = 0.02 * i + (i < 4 ? 5.0 * i * i : 0.0);
    EXPECT_EQ(InterleavedPhaseEstimator::fitDensePhases(ph.data(), 4, rate, 82.4).n, 0);
    const auto f = InterleavedPhaseEstimator::fitDensePhases(ph.data(), 12, rate, 82.4);
    ASSERT_EQ(f.n, 8);
    EXPECT_NEAR(f.cents, centsFromSlope(0.02, rate, 82.4), 1e-9);
}

// =============================================================================================
// D-1 — el estimador de cuatro rejillas, con audio
// =============================================================================================

InterleavedPhaseEstimator preparedGrids(int rate, double target) {
    InterleavedPhaseEstimator g;
    g.prepare(rate);
    g.setTarget(target);
    return g;
}

TEST(EarlyReading, AStableNoteReadsItsSlopeAndNoDrift) {
    // AC-058.2 (tarea 1.3): seno puro a +7 c. A los 8192 frames hay 5 fases densas y una lectura; con
    // 8 fases lee +7 c a 0,01 y su deriva cabe en su propia σ (sin sesgo: en nota estable `c` es ruido).
    constexpr int rate = 48000;
    constexpr double target = 196.0;
    auto g = preparedGrids(rate, target);
    const auto sig = pureSine(detune(target, 7.0), rate, 4 * rate, 0.3);
    g.process(sig.data(), 8191);
    EXPECT_EQ(g.denseEarlyFit().n, 0) << "antes de 8192 frames no hay 5 fases";
    g.process(sig.data() + 8191, 1);
    EXPECT_EQ(g.densePhaseCount(), 5);
    EXPECT_EQ(g.denseEarlyFit().n, 5);
    g.process(sig.data() + 8192, 3 * kHop);
    const auto& f = g.denseEarlyFit();
    ASSERT_EQ(f.n, 8);
    EXPECT_NEAR(f.cents, 7.0, 0.01);
    EXPECT_LE(f.driftCents, f.driftSigmaCents + 1e-6);
}

TEST(EarlyReading, AGlideShowsUpAsDrift) {
    // AC-058.4 (tarea 1.3): en el glide la curvatura sale como deriva y no como ruido de la pendiente.
    constexpr int rate = 48000;
    auto g = preparedGrids(rate, kGlideF0);
    const auto sig = glidingString(kGlideF0, -30.0, 0.2, 0.0, 1, rate, rate);
    g.process(sig.data(), 12288);   // 8 fases densas: el glide va por −30·e^(−0,256/0,2) ≈ −8 c
    const auto& f = g.denseEarlyFit();
    ASSERT_EQ(f.n, 8);
    EXPECT_GT(f.driftCents, 3.0 * f.driftSigmaCents) << "la curvatura del glide no aparece como deriva";
    EXPECT_GT(f.driftCents, 0.5);
}

TEST(EarlyReading, ASilentWindowEmptiesTheDenseSeries) {
    // D-1 (tarea 1.3). Mutante «serie no vaciada con rms < 0,001» ⇒ la serie cruza el silencio.
    constexpr int rate = 48000;
    auto g = preparedGrids(rate, 110.0);
    auto sig = pureSine(110.0, rate, 3 * rate, 0.3);
    std::fill(sig.begin() + 20000, sig.begin() + 20000 + 6000, 0.0f);   // 6000 frames de silencio
    g.process(sig.data(), 20000);
    ASSERT_GT(g.densePhaseCount(), 5);
    g.process(sig.data() + 20000, 6000);
    EXPECT_EQ(g.densePhaseCount(), 0) << "un cierre silencioso no vacio la serie densa";
    EXPECT_EQ(g.denseEarlyFit().n, 0);
}

TEST(Segments, TheNewSegmentStartsAfterTheAttackBlock) {
    // D-3 (1): abierto el segmento en el frame F, ninguna ventana que empiece antes de F entra a la
    // serie densa; la primera es la que empieza en F y cierra en F + 4096. Mutante «aceptar ventanas
    // que empiezan antes del ataque» ⇒ hay fases antes de F + 4096.
    constexpr int rate = 48000;
    auto g = preparedGrids(rate, 110.0);
    const auto sig = pureSine(110.0, rate, 2 * rate, 0.3);
    g.process(sig.data(), 16384);
    ASSERT_GT(g.densePhaseCount(), 0);
    g.openDenseSegmentAt(16384);
    EXPECT_EQ(g.densePhaseCount(), 0);
    g.process(sig.data() + 16384, kWin - 1);
    EXPECT_EQ(g.densePhaseCount(), 0) << "entro una ventana que empezo antes del segmento";
    g.process(sig.data() + 16384 + kWin - 1, 1);
    EXPECT_EQ(g.densePhaseCount(), 1);
}

TEST(EarlyReading, ReportedSigmaMatchesTheScatter) {
    // Tarea 1.2: sobre un seno con ruido blanco, la σ que reporta el ajuste corto (n = 8) tiene que
    // ser la dispersion real de `cents` entre realizaciones independientes: cociente en [0,8; 1,25].
    // Mutante `f = 1` ⇒ ~0,48.
    constexpr int rate = 48000;
    constexpr double target = 110.0;
    constexpr int kRuns = 300;
    std::vector<double> cents, sig2;
    for (int r = 0; r < kRuns; ++r) {
        auto g = preparedGrids(rate, target);
        auto s = pureSine(detune(target, 2.0), rate, kWin + 7 * kHop, 0.05, 0.37 * r);
        addWhiteNoise(&s, 0.05, 7919u * (r + 1));
        g.process(s.data(), static_cast<int>(s.size()));
        const auto& f = g.denseEarlyFit();
        ASSERT_EQ(f.n, 8);
        cents.push_back(f.cents);
        sig2.push_back(f.sigmaCents * f.sigmaCents);
    }
    double mean = 0.0;
    for (double c : cents) mean += c;
    mean /= kRuns;
    double var = 0.0;
    for (double c : cents) var += (c - mean) * (c - mean);
    var /= (kRuns - 1);
    double rep = 0.0;
    for (double s : sig2) rep += s;
    rep /= kRuns;
    const double ratio = std::sqrt(rep / var);
    std::printf("  [REQ-058 1.2] σ reportada (rms) / dispersion real = %.3f (n = 8, %d corridas)\n", ratio, kRuns);
    RecordProperty("sigma_sobre_dispersion_milesimas", static_cast<int>(ratio * 1000.0));
    EXPECT_GE(ratio, 0.8);
    EXPECT_LE(ratio, 1.25);
}

// =============================================================================================
// D-1/D-2 — el motor entero
// =============================================================================================

void reportHonesty(const char* label, const Honesty& h) {
    std::printf("  [REQ-058] %-34s n=%5d  cobertura 2σ %.3f  mediana σ/|error| %.2f\n", label, h.n, h.coverage(),
                h.medianRatio());
}

TEST(EarlyReading, TwoPartialsCoveredAtTwoSigma) {
    // AC-058.4 (estrato k = 2, D29). Mutante «√Σσ² en vez de la propagacion del ajuste» ⇒ rojo.
    const Honesty h = stableHonesty(2, 2, 2);
    reportHonesty("estable, 2 parciales (k = 2)", h);
    RecordProperty("cobertura_k2_milesimas", static_cast<int>(h.coverage() * 1000.0));
    ASSERT_GT(h.n, 100);
    EXPECT_GE(h.coverage(), 0.95);
}

TEST(EarlyReading, StableThreePartialsCoveredAtTwoSigma) {
    // AC-058.4 (estrato k ≥ 3). Mutante «sin el piso de la σ propagada» ⇒ medido en prototipo
    // 0,981 → 0,792.
    for (int np : {3, 4}) {
        const Honesty h = stableHonesty(np, 3, 4);
        reportHonesty(np == 3 ? "estable, 3 parciales (k >= 3)" : "estable, 4 parciales (k >= 3)", h);
        RecordProperty(np == 3 ? "cobertura_3p_milesimas" : "cobertura_4p_milesimas",
                       static_cast<int>(h.coverage() * 1000.0));
        ASSERT_GT(h.n, 100);
        EXPECT_GE(h.coverage(), 0.95) << np << " parciales";
    }
}

TEST(EarlyReading, AStableNoteDoesNotInflateSigma) {
    // AC-058.5: sobre una nota estable (sintesis de 1 a 4 parciales) la mediana de σ/|error| de las
    // tempranas no supera 3. Mutante «deriva sin restar su σ» ⇒ infla.
    for (int np = 1; np <= 4; ++np) {
        const Honesty h = stableHonesty(np);
        char label[64];
        std::snprintf(label, sizeof label, "estable, %d parcial(es), todas", np);
        reportHonesty(label, h);
        RecordProperty("no_inflacion_" + std::to_string(np) + "p_centesimas", static_cast<int>(h.medianRatio() * 100.0));
        ASSERT_GT(h.n, 100);
        EXPECT_LE(h.medianRatio(), 3.0) << np << " parciales";
    }
}

TEST(EarlyReading, GlidesAreCoveredAtTwoSigma) {
    // AC-058.4 sobre los 24 glides de REQ-036, global y por fase de la nota (D21). Mutante «deriva = 0»
    // ⇒ medido en prototipo 0,983 → 0,045.
    Honesty all, byPhase[3];
    for (const GlideRun& g : glideRuns()) {
        const auto truth = glideTruth(g.x, g.tau);
        all.add(judgeEarly(g.pubs, truth));
        for (int ph = 0; ph < 3; ++ph) {
            std::vector<Pub> sel;
            for (const Pub& p : g.pubs)
                if (phaseOf(p.sec) == ph) sel.push_back(p);
            byPhase[ph].add(judgeEarly(sel, truth));
        }
    }
    reportHonesty("24 glides, tempranas", all);
    const char* names[3] = {"24 glides, (0, 0,25] s", "24 glides, (0,25, 1] s", "24 glides, > 1 s"};
    for (int ph = 0; ph < 3; ++ph) reportHonesty(names[ph], byPhase[ph]);
    RecordProperty("cobertura_glides_milesimas", static_cast<int>(all.coverage() * 1000.0));
    ASSERT_GT(all.n, 200);
    EXPECT_GE(all.coverage(), 0.95);
    for (int ph = 0; ph < 3; ++ph) {
        if (byPhase[ph].n < 20) continue;   // sin muestra suficiente no hay porcentaje que afirmar
        EXPECT_GE(byPhase[ph].coverage(), 0.95) << names[ph];
    }
}

TEST(States, NoConvergedBeforeTheVerdictOnGlides) {
    // AC-058.6 (D45): `CONVERGED` sale solo del veredicto; ninguna temprana se publica CONVERGED y
    // ninguna CONVERGED trae σ > 0,1. Sobre los glides y sobre la sintesis estable. Mutantes «estado
    // solo de σ» ⇒ 164 en glides y 450 en sintesis (prototipo).
    Honesty glides;
    for (const GlideRun& g : glideRuns()) glides.add(judgeEarly(g.pubs, glideTruth(g.x, g.tau)));
    EXPECT_EQ(glides.convergedEarly, 0);
    EXPECT_EQ(glides.convergedBigSigma, 0);
    EXPECT_GT(glides.n, 0) << "sin tempranas no hay nada que el estado pueda equivocar";
    for (int np = 1; np <= 4; ++np) {
        const Honesty h = stableHonesty(np);
        EXPECT_EQ(h.convergedEarly, 0) << np << " parciales";
        EXPECT_EQ(h.convergedBigSigma, 0) << np << " parciales";
    }
    // Y una temprana con σ ≤ 0,1 existe (es la que el mutante convertiria): la sintesis limpia la da.
    int earlySmall = 0;
    for (const auto& pubs : stableRuns(4).runs)
        for (const Pub& p : pubs)
            if (p.early && !std::isnan(p.cents) && p.sigma <= StrobeTracker::kConvergedUncertaintyCents) ++earlySmall;
    EXPECT_GT(earlySmall, 0) << "ninguna temprana con σ ≤ 0,1: el control positivo del mutante no existe";
}

TEST(Inharmonicity, NotMeasuredFromAnEarlyReading) {
    // D-2: B sale solo del camino del veredicto. Mutante «estimateFrom con temprana» ⇒ B publicado
    // sobre tempranas.
    int early = 0, bFromEarly = 0, bFromVerdict = 0;
    for (const auto& pubs : stableRuns(4).runs)
        for (const Pub& p : pubs) {
            if (std::isnan(p.cents)) continue;
            if (p.early) { ++early; if (p.bMeasured) ++bFromEarly; }
            else if (p.bMeasured) ++bFromVerdict;
        }
    EXPECT_GT(early, 0);
    EXPECT_EQ(bFromEarly, 0);
    EXPECT_GT(bFromVerdict, 0) << "control positivo: el veredicto si publica B";
}

TEST(EarlyReading, SigmaFallsUntilConvergedOnASteadyNote) {
    // AC-058.3: con la cuerda sonando sin quiebre, la σ publicada no crece (su mediana por tramo de
    // 0,5 s no sube) y termina en CONVERGED con σ ≤ 0,1.
    constexpr int rate = 48000;
    int checked = 0;
    for (double f0 : kGuitar) {
        auto sig = partialsWithPitchTrajectory(detune(f0, kStableDevCents), kStableB, oneOverNAmplitudes(4, 0.3), rate,
                                               4 * rate, [](double) { return 0.0; });
        addWhiteNoise(&sig, 0.001, 31u);
        const auto pubs = runEngine(sig, rate, f0);
        std::vector<double> bins[8];
        for (const Pub& p : pubs)
            if (!std::isnan(p.sigma)) bins[std::min(7, static_cast<int>(p.sec / 0.5))].push_back(p.sigma);
        // «No creciente» con una tolerancia ABSOLUTA de 0,001 c (la centesima parte del presupuesto de
        // CONVERGED): ya convergida, la σ del veredicto fluctua en 1e-5 c, y eso no es una tendencia.
        double prev = INFINITY;
        for (int b = 0; b < 8; ++b) {
            if (bins[b].empty()) continue;
            const double m = median(bins[b]);
            EXPECT_LE(m, prev + 1e-3) << f0 << " Hz, tramo " << b << ": la σ subio con la cuerda estable";
            prev = std::min(prev, m);
        }
        ASSERT_FALSE(pubs.empty());
        EXPECT_EQ(pubs.back().state, wma::analysis::kStateConverged) << f0 << " Hz";
        EXPECT_LE(pubs.back().sigma, StrobeTracker::kConvergedUncertaintyCents) << f0 << " Hz";
        ++checked;
    }
    EXPECT_EQ(checked, 6);
}

}  // namespace
}  // namespace wma_test
