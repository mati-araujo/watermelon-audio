#include "InharmonicityEstimator.h"

#include <algorithm>

namespace wma::analysis {

bool InharmonicityEstimator::estimateFrom(const StrobeTracker& strobe) noexcept {
    // `cents_n = C + K·n²`, con K = kCentsPerBPerNSquared · B. Se ajusta la recta
    // por minimos cuadrados PONDERADOS por 1/σ²: un parcial medido con σ diez
    // veces peor no puede pesar lo mismo, y S6 ya publica ese σ por parcial.
    double sw = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    int used = 0;

    for (int i = 0; i < kPartials; ++i) {
        if (!strobe.partialHasMeasurement(i)) continue;
        const double sigma = strobe.partialUncertaintyCents(i);
        if (!(sigma > 0.0) || !std::isfinite(sigma)) continue;
        const double cents = strobe.partialCents(i);
        if (!std::isfinite(cents)) continue;

        const double n = static_cast<double>(i + 1);
        const double x = n * n;
        const double w = 1.0 / (sigma * sigma);
        if (!std::isfinite(w)) continue;

        sw += w; sx += w * x; sy += w * cents;
        sxx += w * x * x; sxy += w * x * cents;
        ++used;
    }

    if (used < kMinPartialsForFit) { mMeasured = false; return false; }

    const double denom = sw * sxx - sx * sx;
    // Denominador cero = todos los x son el mismo, o sea que no hay palanca para
    // separar la pendiente de la ordenada. No es "B = 0": es "no se puede saber".
    if (!(std::abs(denom) > 1e-12) || !std::isfinite(denom)) {
        mMeasured = false;
        return false;
    }

    const double slope = (sw * sxy - sx * sy) / denom;
    const double b = slope / kCentsPerBPerNSquared;

    // Un B negativo no existe: la rigidez estira los parciales, nunca los junta.
    // Si el ajuste lo da, midio ruido — y devolver 0 seria peor que decir que no
    // se pudo, porque 0 es un valor PLAUSIBLE (cuerda ideal) y el consumidor lo
    // trataria como medicion.
    if (!std::isfinite(b) || b < 0.0) { mMeasured = false; return false; }

    mB = b;
    mMeasured = true;
    return true;
}

double InharmonicityEstimator::physicsB(const StringPhysics& p) noexcept {
    if (!(p.tensionN > 0.0) || !(p.scaleLengthM > 0.0)) return 0.0;
    constexpr double kPi = 3.14159265358979323846;
    const double d2 = p.coreDiameterM * p.coreDiameterM;
    return (kPi * kPi * kPi) * p.youngModulusPa * d2 * d2 /
           (64.0 * p.tensionN * p.scaleLengthM * p.scaleLengthM);
}

double InharmonicityEstimator::alignmentCents(double bLower, double bUpper,
                                              int pLower, int qUpper) noexcept {
    const double lower = 600.0 * std::log2(1.0 + static_cast<double>(pLower) * pLower * bLower);
    const double upper = 600.0 * std::log2(1.0 + static_cast<double>(qUpper) * qUpper * bUpper);
    return lower - upper;
}

void InharmonicityEstimator::matchingPartials(int semitones, int& pLower,
                                              int& qUpper) noexcept {
    // La razon justa mas cercana al intervalo temperado. Se listan los que
    // aparecen en las afinaciones de S3; cualquier otro cae en la octava, que es
    // el unico intervalo que siempre se puede chequear.
    switch (semitones) {
        case 3:  pLower = 6; qUpper = 5; return;   // tercera menor   6:5
        case 4:  pLower = 5; qUpper = 4; return;   // tercera mayor   5:4
        case 5:  pLower = 4; qUpper = 3; return;   // cuarta justa    4:3
        case 7:  pLower = 3; qUpper = 2; return;   // quinta justa    3:2
        case 12: pLower = 2; qUpper = 1; return;   // octava          2:1
        default: pLower = 2; qUpper = 1; return;
    }
}

void InharmonicityEstimator::perceptualCorrections(const double* freqHz, const double* bs,
                                                   int count, int reference,
                                                   double* outCents) noexcept {
    if (freqHz == nullptr || bs == nullptr || outCents == nullptr || count <= 0) return;
    const int ref = std::clamp(reference, 0, count - 1);

    outCents[ref] = 0.0;

    // Hacia arriba desde la referencia, y despues hacia abajo. La correccion de
    // una cuerda es la de su vecina MAS el desajuste del intervalo que las une:
    // por eso el offset es propiedad del conjunto y no de la cuerda.
    for (int i = ref + 1; i < count; ++i) {
        int p = 2, q = 1;
        const double ratio = (freqHz[i - 1] > 0.0) ? (freqHz[i] / freqHz[i - 1]) : 0.0;
        if (ratio > 0.0) {
            matchingPartials(static_cast<int>(std::lround(12.0 * std::log2(ratio))), p, q);
        }
        outCents[i] = outCents[i - 1] + alignmentCents(bs[i - 1], bs[i], p, q);
    }
    for (int i = ref - 1; i >= 0; --i) {
        int p = 2, q = 1;
        const double ratio = (freqHz[i] > 0.0) ? (freqHz[i + 1] / freqHz[i]) : 0.0;
        if (ratio > 0.0) {
            matchingPartials(static_cast<int>(std::lround(12.0 * std::log2(ratio))), p, q);
        }
        // El mismo desajuste, mirado desde la otra punta del intervalo.
        outCents[i] = outCents[i + 1] - alignmentCents(bs[i], bs[i + 1], p, q);
    }

    for (int i = 0; i < count; ++i) {
        outCents[i] = std::clamp(outCents[i], -kMaxCorrectionCents, kMaxCorrectionCents);
    }
}

}  // namespace wma::analysis
