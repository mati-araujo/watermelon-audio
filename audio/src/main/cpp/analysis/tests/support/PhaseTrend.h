/**
 * PhaseTrend.h — REQ-036 S1. EL ESTADISTICO DE TENDENCIA sobre la ventana de regresion de fase,
 * calculado DESDE AFUERA con la sonda `regressionPhaseAt`, y la simulacion de la compuerta que
 * S2 va a meter en produccion.
 *
 * POR QUE TENDENCIA Y NO RESIDUOS (decision 3 de la spec). Un glide de ataque deja la fase como un
 * palo de hockey; una recta ajustada a un palo de hockey deja residuos chicos, asi que σ no lo ve.
 * Pero las DOS MITADES de la ventana tienen pendientes distintas: la vieja arrastra el glide y la
 * nueva ya mide la nota asentada. Un vibrato tambien deja residuos estructurados —y un test de
 * residuos lo apagaria— pero sus dos mitades tienen la MISMA pendiente media: el vibrato es el
 * control negativo, y su tasa de disparo tiene que ser cero.
 *
 * EL ESTADISTICO tiene dos partes, y S1 mide si hacen falta las dos:
 *   · `tScore`: (b₂ − b₁) / √(se₁² + se₂²), la diferencia de pendientes entre mitades en unidades
 *     de su propio error estandar. Es adimensional: dice si la discrepancia es SIGNIFICATIVA
 *     contra el ruido de la propia ventana.
 *   · `deltaCents`: esa misma diferencia llevada a cents con la conversion del estimador. Dice si
 *     la discrepancia IMPORTA: sobre sintesis sin ruido `se` es de nivel numerico y `tScore` sale
 *     enorme para una cola de glide de 0,001 c que ningun contrato distingue.
 * La compuerta dispara si |tScore| > kT Y |deltaCents| > kDelta; los dos umbrales los elige la
 * evidencia de S1 (nota estable y vibrato: cero disparos; σ ciega al glide: todos).
 */
#pragma once

#include "PhaseSlopeEstimator.h"
#include "StrobeTracker.h"

#include <cmath>
#include <limits>

namespace wma_test::trend {

/// Menos de esto no hay dos mitades con residuos cada una (3 puntos por mitad, y una de sobra).
constexpr int kMinWindowsForTrend = 8;

struct Slope { double slope = 0.0; double stdErr = 0.0; bool ok = false; };

/// Regresion lineal de `y[i]` contra `i` sobre `[from, to)`. Error estandar de la pendiente a
/// partir de los residuos, igual que en `PhaseSlopeEstimator::closeWindow()`.
inline Slope fitSlope(const double* y, int from, int to) {
    Slope s;
    const int n = to - from;
    if (n < 3) return s;
    const double dn = n;
    double sumX = 0, sumY = 0, sumXX = 0, sumXY = 0;
    for (int i = 0; i < n; ++i) {
        const double x = i, v = y[from + i];
        sumX += x; sumY += v; sumXX += x * x; sumXY += x * v;
    }
    const double sxx = sumXX - sumX * sumX / dn;
    if (sxx <= 0.0) return s;
    s.slope = (sumXY - sumX * sumY / dn) / sxx;
    const double intercept = (sumY - s.slope * sumX) / dn;
    double sse = 0.0;
    for (int i = 0; i < n; ++i) {
        const double r = y[from + i] - (intercept + s.slope * i);
        sse += r * r;
    }
    s.stdErr = std::sqrt(sse / (dn - 2.0) / sxx);
    s.ok = true;
    return s;
}

/// Cents por unidad de pendiente (rad/ventana) para un parcial en `partialHz`: la misma
/// conversion que `closeWindow()` (Δf = slope·fs/(2π·N); cents ≈ Δf · 1200/(ln2 · f)).
inline double centsPerSlopeUnit(int sampleRate, double partialHz) {
    const double hzPerSlope = sampleRate / (2.0 * M_PI * wma::analysis::PhaseSlopeEstimator::kWindowFrames);
    return hzPerSlope * 1200.0 / (std::log(2.0) * partialHz);
}

/// La lectura de un parcial a partir de la pendiente de su regresion, con las MISMAS formulas que
/// `closeWindow()`: Δf = slope·fs/(2π·N); cents = 1200·log2((f+Δf)/f); σ por la derivada.
struct PartialReading { double cents = NAN; double sigma = NAN; bool ok = false; };
inline PartialReading readingFromSlope(const Slope& s, int sampleRate, double partialHz) {
    PartialReading r;
    if (!s.ok) return r;
    const double hzPerSlope = sampleRate / (2.0 * M_PI * wma::analysis::PhaseSlopeEstimator::kWindowFrames);
    r.cents = 1200.0 * std::log2((partialHz + s.slope * hzPerSlope) / partialHz);
    r.sigma = std::fabs(s.stdErr * hzPerSlope) * 1200.0 / (std::log(2.0) * partialHz);
    r.ok = true;
    return r;
}

struct Trend {
    bool evaluable = false;   ///< hubo ≥ kMinWindowsForTrend fases con dos ajustes validos
    double tScore = 0.0;      ///< (b₂ − b₁) / √(se₁² + se₂²)
    double deltaCents = 0.0;  ///< (b₂ − b₁) en cents del parcial: la DISCREPANCIA entre mitades
    /**
     * (b_entera − b₂) en cents: cuanto CORRE a la lectura publicada la mitad vieja respecto de lo
     * que dice la mitad nueva sola. Si la mitad nueva ya esta limpia, es EL SESGO de la lectura.
     * Medido en S1: Δ sobrestima el sesgo ~3× cuando el glide esta en el borde de la ventana (la
     * regresion entera le da poco peso al borde; la de media ventana, mas) y lo subestima cuando el
     * glide es lento respecto de la ventana. El sesgo directo es lo que el contrato acota.
     */
    double biasCents = 0.0;
    int windows = 0;
};

/// El estadistico sobre `phases[0..count)`: mitad vieja `[0, count/2)`, mitad nueva `[count/2, count)`.
inline Trend trendOver(const double* phases, int count, int sampleRate, double partialHz) {
    Trend t;
    t.windows = count;
    if (count < kMinWindowsForTrend) return t;
    const int mid = count / 2;
    const Slope a = fitSlope(phases, 0, mid);
    const Slope b = fitSlope(phases, mid, count);
    const Slope full = fitSlope(phases, 0, count);
    if (!a.ok || !b.ok || !full.ok) return t;
    const double diff = b.slope - a.slope;
    const double se = std::sqrt(a.stdErr * a.stdErr + b.stdErr * b.stdErr);
    t.tScore = se > 0.0 ? diff / se : (diff == 0.0 ? 0.0 : std::numeric_limits<double>::infinity());
    const double k = centsPerSlopeUnit(sampleRate, partialHz);
    t.deltaCents = diff * k;
    t.biasCents = (full.slope - b.slope) * k;
    t.evaluable = true;
    return t;
}

/// El estadistico del parcial `i` de un strobe, leido por la sonda.
inline Trend trendOfPartial(const wma::analysis::StrobeTracker& s, int i, int sampleRate) {
    const wma::analysis::PhaseSlopeEstimator& p = s.partialEstimator(i);
    const int n = p.regressionPhaseCount();
    double phases[wma::analysis::PhaseSlopeEstimator::kMaxWindows];
    for (int k = 0; k < n; ++k) phases[k] = p.regressionPhaseAt(k);
    return trendOver(phases, n, sampleRate, s.partialTargetHz(i));
}

/// La regla de la compuerta con un par de umbrales: el estadistico DISPARA si las DOS partes
/// superan el suyo.
/// Que magnitud se compara: Δ entre mitades, el sesgo, o la mayor de las dos (Δ ve la cola LENTA,
/// que la mitad nueva todavia arrastra; el sesgo ve el glide en el BORDE, al que Δ sobrestima).
enum class Magnitude { kDelta, kBias, kMax };
struct Threshold {
    double t;            ///< |T| tiene que superar esto
    double deltaCents;   ///< y la magnitud elegida esto
    int minWindows;      ///< y la ventana tener al menos estas fases para que el veredicto valga
    Magnitude magnitude = Magnitude::kDelta;
};
inline double magnitudeOf(const Trend& tr, const Threshold& th) {
    switch (th.magnitude) {
        case Magnitude::kBias: return tr.biasCents;
        case Magnitude::kMax: return std::fabs(tr.biasCents) > std::fabs(tr.deltaCents) ? tr.biasCents : tr.deltaCents;
        default: return tr.deltaCents;
    }
}
inline bool fires(const Trend& tr, const Threshold& th) {
    return tr.evaluable && std::fabs(tr.tScore) > th.t && std::fabs(magnitudeOf(tr, th)) > th.deltaCents;
}

/// Y el parcial se ADMITE solo si el estadistico se pudo evaluar sobre una ventana de al menos
/// `minWindows` fases y no disparo. Una ventana de menos de `kMinWindowsForTrend` fases no tiene
/// dos mitades que comparar, y un parcial que no se puede verificar no se admite: son las ventanas
/// 4..7, donde hoy el strobe ya declara CONVERGIDO. `minWindows` puede pedir mas que eso: sobre
/// las cuerdas mas graves el rizado de la imagen negativa deja a las mitades de una ventana corta
/// con pendientes distintas (medido: B0 y E1 a 44,1 kHz hasta n = 12), y eso no es un glide.
inline bool admits(const Trend& tr, const Threshold& th) {
    return tr.evaluable && tr.windows >= th.minWindows && !fires(tr, th);
}

/**
 * La lectura combinada que el strobe publicaria con el subconjunto `mask` de parciales
 * (bit i = parcial i), usando SUS funciones: `fitStretchedSeries` con k ≥ 2 (k = 2 propaga σ,
 * como en produccion) y el valor solo con k = 1. Con k = 0, sin medicion.
 *
 * Es la simulacion de la compuerta desde afuera: se parte del subconjunto que produccion ADMITIO
 * y se le sacan los parciales cuyo estadistico dispara.
 */
struct Combined {
    bool hasMeasurement = false; double cents = NAN; double sigma = NAN; int used = 0;
    bool loneHarmonicVetoed = false;   ///< la regla del armonico solo dejo la lectura sin medicion
};
/**
 * `loneHarmonicRule`: con UN solo parcial admitido que no es el fundamental, no hay lectura.
 *
 * 🔴 MEDIDO EN S1: el camino k = 1 publica los cents del parcial TAL CUAL, y sobre una cuerda
 * inarmonica (B = 1e-4) el parcial 4 esta +1,38 c estirado respecto del fundamental. Hoy k = 1 es
 * el tono puro (el fundamental solo, sin estiramiento). Con admision por parcial, los cuatro se
 * reincorporan en ventanas distintas y el primero en volver puede ser el 4: la lectura sale
 * CONVERGIDA a +1,4 c con σ 0,003. Con un parcial no hay serie que ajustar ni B que estimar: sin
 * el fundamental no hay a que referir la lectura.
 */
inline Combined combineFrom(const double* partialCents, const double* partialSigmas, int mask,
                            bool loneHarmonicRule = false) {
    using wma::analysis::StrobeTracker;
    Combined c;
    double cents[StrobeTracker::kPartials], sigmas[StrobeTracker::kPartials];
    int orders[StrobeTracker::kPartials];
    int k = 0;
    for (int i = 0; i < StrobeTracker::kPartials; ++i) {
        if (!(mask & (1 << i))) continue;
        cents[k] = partialCents[i]; sigmas[k] = partialSigmas[i]; orders[k] = i + 1; ++k;
    }
    c.used = k;
    if (k == 0) return c;
    if (loneHarmonicRule && k == 1 && orders[0] != 1) { c.loneHarmonicVetoed = true; return c; }
    if (k >= 2) {
        if (StrobeTracker::fitStretchedSeries(cents, sigmas, orders, k, &c.cents, &c.sigma)) {
            c.hasMeasurement = true;
            return c;
        }
    }
    double sw = 0.0, swv = 0.0;
    for (int i = 0; i < k; ++i) { const double w = 1.0 / (sigmas[i] * sigmas[i]); sw += w; swv += w * cents[i]; }
    if (sw > 0.0) { c.cents = swv / sw; c.sigma = std::sqrt(1.0 / sw); c.hasMeasurement = true; }
    return c;
}
inline Combined combine(const wma::analysis::StrobeTracker& s, int mask) {
    double pc[wma::analysis::StrobeTracker::kPartials], ps[wma::analysis::StrobeTracker::kPartials];
    for (int i = 0; i < wma::analysis::StrobeTracker::kPartials; ++i) {
        pc[i] = s.partialCents(i); ps[i] = s.partialUncertaintyCents(i);
    }
    return combineFrom(pc, ps, mask);
}

/// `mask` menos los parciales que `th` no admite.
inline int gated(const wma::analysis::StrobeTracker& s, int mask, int sampleRate, const Threshold& th) {
    int out = mask;
    for (int i = 0; i < wma::analysis::StrobeTracker::kPartials; ++i) {
        if (!(mask & (1 << i))) continue;
        if (!admits(trendOfPartial(s, i, sampleRate), th)) out &= ~(1 << i);
    }
    return out;
}

}  // namespace wma_test::trend
