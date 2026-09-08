/**
 * PartialOracle.h — REQ-035 S1. El oraculo POR PARCIAL, en C++, para que el test lo tenga a mano.
 *
 * ES EL MISMO METODO QUE `scripts/corpus-reference-pitch.py --partials 4`, y a proposito NO es el
 * del motor: pico espectral por Goertzel con ventana de Hann e interpolacion parabolica, sobre
 * tramos de 0,75 s. El motor mide con NSDF + pendiente de fase; si este oraculo y el strobe
 * discrepan, hay dos metodos contra uno, y el que se aparta es el que hay que mirar.
 *
 * POR QUE EXISTE EN C++ ADEMAS DEL SCRIPT. El manifiesto trae `hz_verdadero` (H1, medido por el
 * script desde 2,0 s), pero AC-035.1 pide comparar CADA parcial del strobe con el suyo y ADEMAS
 * en la MISMA ventana de tiempo en que el strobe leyo — la ultima lectura de `ukelele_C4` es a
 * 2,46 s y el manifiesto mide de 2,0 a 5,0. Escribir esas tablas en un archivo de datos seria una
 * segunda fuente de verdad que envejece; medirlas aca cuesta ~30 ms por archivo. Que los dos
 * oraculos coinciden lo AFIRMA el test: el H1 de este, desde 2,0 s, contra el del manifiesto.
 *
 * `cents[n]` es la desviacion del parcial n respecto de `n·hz1`, con hz1 el H1 medido en el mismo
 * tramo: para n = 1 vale 0 por construccion. `db[n]` es su nivel respecto de H1. Un parcial a
 * −40 dB no es un parcial: es ruido con pico, y la tabla lo dice para que nadie lo lea como dato.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace wma_test::oracle {

constexpr int kPartials = 4;

struct PartialReading {
    double hz[kPartials + 1] = {};      ///< indice 1..4; 0 no se usa
    double cents[kPartials + 1] = {};   ///< respecto de n·hz1 (n = 1: 0)
    double db[kPartials + 1] = {};      ///< respecto de H1
    bool valid = false;                 ///< false si el tramo no entra en la señal
};

inline double centsOf(double f, double ref) { return 1200.0 * std::log2(f / ref); }
inline double detuneHz(double ref, double c) { return ref * std::pow(2.0, c / 1200.0); }

/// Magnitud de Goertzel en `hz` sobre `x` (ya ventaneado).
inline double goertzel(const std::vector<double>& x, int sr, double hz) {
    const double w = 2.0 * M_PI * hz / sr;
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (double v : x) {
        const double s = v + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2));
}

/// Barre `center` ± `spanCents` de a `stepCents` y devuelve el pico con interpolacion parabolica.
inline void peakIn(const std::vector<double>& x, int sr, double center, double spanCents,
                   double stepCents, double* outHz, double* outMag) {
    std::vector<double> grid, mags;
    for (double c = -spanCents; c <= spanCents + 1e-9; c += stepCents) {
        grid.push_back(c);
        mags.push_back(goertzel(x, sr, detuneHz(center, c)));
    }
    size_t i = 0;
    for (size_t k = 1; k < mags.size(); ++k) if (mags[k] > mags[i]) i = k;
    double off = 0.0;
    if (i > 0 && i + 1 < mags.size()) {
        const double a = mags[i - 1], b = mags[i], cc = mags[i + 1];
        const double den = a - 2.0 * b + cc;
        off = den != 0.0 ? 0.5 * (a - cc) / den : 0.0;
    }
    *outHz = detuneHz(center, grid[i] + off * stepCents);
    *outMag = mags[i];
}

/**
 * Los parciales 1..4 en el tramo `[t0, t0 + winSec)` de `mono`, buscando H1 alrededor de
 * `nominalHz` (±100 c) y cada parcial n alrededor de `n·hz1` (±30 c, despues ±4 c de a 0,25).
 * Mismos numeros que el script.
 */
inline PartialReading measureAt(const std::vector<float>& mono, int sr, double nominalHz,
                                double t0, double winSec = 0.75) {
    PartialReading r;
    const int n = static_cast<int>(winSec * sr);
    const int a = static_cast<int>(t0 * sr);
    if (a < 0 || a + n > static_cast<int>(mono.size()) || nominalHz <= 0.0) return r;
    std::vector<double> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double hann = 0.5 * (1.0 - std::cos(2.0 * M_PI * (i + 0.5) / n));
        x[static_cast<size_t>(i)] = mono[static_cast<size_t>(a + i)] * hann;
    }
    double coarse = 0.0, mag = 0.0;
    peakIn(x, sr, nominalHz, 100.0, 10.0, &coarse, &mag);
    // El script barre ±12 c de a 0,25 alrededor del grueso; aca ±6, y da lo MISMO: el grueso de a
    // 10 c deja el pico a menos de 5 c, y los puntos de la grilla fina coinciden (centro ± k·0,25),
    // asi que el maximo y sus dos vecinos son los mismos. Cuesta la mitad, y el test lo corre bajo
    // sanitizer sobre 41 archivos.
    peakIn(x, sr, coarse, 6.0, 0.25, &r.hz[1], &mag);
    const double m1 = mag;
    r.cents[1] = 0.0;
    r.db[1] = 0.0;
    for (int k = 2; k <= kPartials; ++k) {
        if (k * r.hz[1] >= 0.5 * sr) break;
        double c = 0.0, m = 0.0;
        peakIn(x, sr, k * r.hz[1], 30.0, 3.0, &c, &m);
        peakIn(x, sr, c, 4.0, 0.25, &r.hz[k], &m);
        r.cents[k] = centsOf(r.hz[k], k * r.hz[1]);
        r.db[k] = 20.0 * std::log10(std::max(m, 1e-12) / std::max(m1, 1e-12));
    }
    r.valid = true;
    return r;
}

/// El tramo del manifiesto: mediana de hasta cuatro tramos de 0,75 s desde `from` (2,0 s).
inline PartialReading measureSustained(const std::vector<float>& mono, int sr, double nominalHz,
                                       double from = 2.0) {
    std::vector<PartialReading> per;
    for (int k = 0; k < 4; ++k) {
        const PartialReading r = measureAt(mono, sr, nominalHz, from + 0.75 * k);
        if (r.valid) per.push_back(r);
    }
    PartialReading out;
    if (per.empty()) return out;
    auto median = [&](auto get) {
        std::vector<double> v;
        for (const auto& r : per) v.push_back(get(r));
        std::sort(v.begin(), v.end());
        const size_t m = v.size();
        return m % 2 ? v[m / 2] : 0.5 * (v[m / 2 - 1] + v[m / 2]);
    };
    for (int k = 1; k <= kPartials; ++k) {
        out.hz[k] = median([&](const PartialReading& r) { return r.hz[k]; });
        out.cents[k] = median([&](const PartialReading& r) { return r.cents[k]; });
        out.db[k] = median([&](const PartialReading& r) { return r.db[k]; });
    }
    out.valid = true;
    return out;
}

}  // namespace wma_test::oracle
