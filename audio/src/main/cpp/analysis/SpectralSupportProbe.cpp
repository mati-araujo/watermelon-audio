#include "SpectralSupportProbe.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wma::analysis {

// ---------------------------------------------------------------------------
// SpectralSupportProbe (REQ-031 S1). El porqué entero está en el header.
// ---------------------------------------------------------------------------

SpectralSupportProbe::SpectralSupportProbe() : SpectralSupportProbe(kWindowFrames) {}

SpectralSupportProbe::SpectralSupportProbe(int windowFrames)
    : mWindowFrames(windowFrames > 0 ? windowFrames : kWindowFrames),
      mRing(static_cast<size_t>(mWindowFrames), 0.0f),
      mHann(static_cast<size_t>(mWindowFrames), 0.0f) {
    // Hann "periódica" centrada en (i + ½): sin ceros exactos en las puntas, que a este largo
    // no cambian la fuga y sí tiran un frame de cada lado. La ventana no depende del rate, así
    // que se calcula una sola vez.
    for (int i = 0; i < mWindowFrames; ++i) {
        mHann[static_cast<size_t>(i)] = static_cast<float>(
            0.5 * (1.0 - std::cos(2.0 * M_PI * (static_cast<double>(i) + 0.5) / mWindowFrames)));
    }
}

void SpectralSupportProbe::pushMono(const float* mono, int numFrames) noexcept {
    for (int i = 0; i < numFrames; ++i) {
        mRing[static_cast<size_t>(mWrite)] = mono[i];
        if (++mWrite >= mWindowFrames) mWrite = 0;
    }
    if (mFilled < mWindowFrames) {
        mFilled = numFrames >= mWindowFrames - mFilled ? mWindowFrames : mFilled + numFrames;
    }
}

double SpectralSupportProbe::magnitudeAt(int sampleRate, double hz) const noexcept {
    const double w = 2.0 * M_PI * hz / static_cast<double>(sampleRate);
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    // Del frame más VIEJO al más nuevo, para que la ventana de Hann pese la señal en el orden
    // en que sonó. `mWrite` apunta al slot que se va a pisar, o sea al más viejo.
    for (int k = 0; k < mWindowFrames; ++k) {
        int i = mWrite + k;
        if (i >= mWindowFrames) i -= mWindowFrames;
        const size_t idx = static_cast<size_t>(i);
        const double x = static_cast<double>(mRing[idx]) * mHann[static_cast<size_t>(k)];
        const double s = x + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    // La magnitud de Goertzel; el `max(0, ·)` sólo defiende el redondeo de un valor que por
    // construcción no es negativo.
    return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2));
}

double SpectralSupportProbe::supportDb(int sampleRate, double hz) const noexcept {
    const double none = -std::numeric_limits<double>::infinity();
    if (sampleRate <= 0 || hz <= 0.0) return none;
    double peak = 0.0, fundamental = 0.0, octave = 0.0;
    for (int k = 1; k <= kHarmonics; ++k) {
        const double f = hz * k;
        if (f >= 0.5 * sampleRate) break;   // por encima de Nyquist no hay parcial que medir
        const double m = magnitudeAt(sampleRate, f);
        if (k == 1) fundamental = m;
        if (k == 2) octave = m;
        if (m > peak) peak = m;
    }
    const double best = std::max(fundamental, octave);
    if (peak <= 0.0 || best <= 0.0) return none;
    return 20.0 * std::log10(best / peak);
}

}  // namespace wma::analysis
