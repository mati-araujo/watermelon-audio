#include "BiquadFilter.h"

BiquadFilter::BiquadFilter(float sampleRate)
    : mSampleRate(sampleRate) {
    // Initialize with lowpass at 1kHz
    setLowpass(1000.0f);
}

void BiquadFilter::setSampleRate(float sampleRate) {
    if (sampleRate <= 0.0f) {
        return;
    }

    mSampleRate = sampleRate;

    // WD-3.5 — el clamp contra Nyquist tiene que volver a aplicarse ACA.
    //
    // Antes de esto, `clampFrequency()` vivia solo en los setters, asi que
    // acotaba contra el rate VIGENTE EN ESE MOMENTO y despues nadie lo revisaba.
    // El idioma que eso rompe es el que usa medio repo: configurar los filtros
    // en el constructor (a 48 kHz) y llamar a `setSampleRate()` cuando el
    // backend negocia otro rate. Un LPF de 12 kHz configurado a 48 y llevado a
    // 16 quedaba con omega = 1,5 pi, sin(omega) < 0, el alpha del cookbook RBJ
    // negativo y los polos afuera del circulo unitario: NaN en el primer bloque.
    //
    // Era la causa de CUATRO de las siete entradas de nyquist-baseline.txt
    // —VOCODER, HPF_DELAY, PLATE_REVERB y SHIMMER_REVERB—, y de ninguna se veia
    // desde el efecto: los cuatro llaman a un setter que SI clampea. La deuda
    // estaba en el primitivo que comparten.
    //
    // Se re-clampea desde `mRequestedFrequency` y no desde `mFrequency` para que
    // bajar y volver a subir el rate RECUPERE lo que se habia pedido. Clampear
    // en su lugar seria destructivo: un device que pasa por 16 kHz dejaria el
    // filtro en 7.840 Hz para siempre.
    mFrequency = clampFrequency(mRequestedFrequency);
    updateCoefficients();
}

void BiquadFilter::setLowpass(float frequency, float Q) {
    mType = Type::LPF;
    mRequestedFrequency = frequency;
    mFrequency = clampFrequency(frequency);
    mQ = std::max(0.01f, Q);  // Prevent Q = 0
    mGainDb = 0.0f;
    updateCoefficients();
}

void BiquadFilter::setHighpass(float frequency, float Q) {
    mType = Type::HPF;
    mRequestedFrequency = frequency;
    mFrequency = clampFrequency(frequency);
    mQ = std::max(0.01f, Q);
    mGainDb = 0.0f;
    updateCoefficients();
}

void BiquadFilter::setBandpass(float frequency, float Q) {
    mType = Type::BPF;
    mRequestedFrequency = frequency;
    mFrequency = clampFrequency(frequency);
    mQ = std::max(0.01f, Q);
    mGainDb = 0.0f;
    updateCoefficients();
}

void BiquadFilter::setNotch(float frequency, float Q) {
    mType = Type::NOTCH;
    mRequestedFrequency = frequency;
    mFrequency = clampFrequency(frequency);
    mQ = std::max(0.01f, Q);
    mGainDb = 0.0f;
    updateCoefficients();
}

void BiquadFilter::setPeaking(float frequency, float Q, float gainDb) {
    mType = Type::PEAK;
    mRequestedFrequency = frequency;
    mFrequency = clampFrequency(frequency);
    mQ = std::max(0.01f, Q);
    mGainDb = std::clamp(gainDb, -40.0f, 40.0f);
    updateCoefficients();
}

void BiquadFilter::setLowShelf(float frequency, float Q, float gainDb) {
    mType = Type::LOW_SHELF;
    mRequestedFrequency = frequency;
    mFrequency = clampFrequency(frequency);
    mQ = std::max(0.01f, Q);
    mGainDb = std::clamp(gainDb, -40.0f, 40.0f);
    updateCoefficients();
}

void BiquadFilter::setHighShelf(float frequency, float Q, float gainDb) {
    mType = Type::HIGH_SHELF;
    mRequestedFrequency = frequency;
    mFrequency = clampFrequency(frequency);
    mQ = std::max(0.01f, Q);
    mGainDb = std::clamp(gainDb, -40.0f, 40.0f);
    updateCoefficients();
}

float BiquadFilter::process(float input) {
    // Load coefficients atomically
    float b0_val = b0.load(std::memory_order_acquire);
    float b1_val = b1.load(std::memory_order_acquire);
    float b2_val = b2.load(std::memory_order_acquire);
    float a1_val = a1.load(std::memory_order_acquire);
    float a2_val = a2.load(std::memory_order_acquire);

    // Direct Form II (transposed)
    float output = b0_val * input + z1;
    z1 = b1_val * input - a1_val * output + z2;
    z2 = b2_val * input - a2_val * output;

    // Denormal protection: flush very small values to zero
    // This prevents CPU slowdown and accumulation of floating point errors
    constexpr float DENORMAL_THRESHOLD = 1e-20f;
    if (std::abs(z1) < DENORMAL_THRESHOLD) z1 = 0.0f;
    if (std::abs(z2) < DENORMAL_THRESHOLD) z2 = 0.0f;

    return output;
}

void BiquadFilter::processBlock(const float* input, float* output, int numSamples) {
    // Load coefficients once for entire block (optimization)
    float b0_val = b0.load(std::memory_order_acquire);
    float b1_val = b1.load(std::memory_order_acquire);
    float b2_val = b2.load(std::memory_order_acquire);
    float a1_val = a1.load(std::memory_order_acquire);
    float a2_val = a2.load(std::memory_order_acquire);

    constexpr float DENORMAL_THRESHOLD = 1e-20f;

    for (int i = 0; i < numSamples; ++i) {
        // Direct Form II (transposed)
        output[i] = b0_val * input[i] + z1;
        z1 = b1_val * input[i] - a1_val * output[i] + z2;
        z2 = b2_val * input[i] - a2_val * output[i];

        // Denormal protection
        if (std::abs(z1) < DENORMAL_THRESHOLD) z1 = 0.0f;
        if (std::abs(z2) < DENORMAL_THRESHOLD) z2 = 0.0f;
    }
}

void BiquadFilter::reset() {
    z1 = 0.0f;
    z2 = 0.0f;
}

float BiquadFilter::getFrequencyResponse(float frequency) const {
    // Calculate magnitude response at given frequency
    // |H(e^jω)| where ω = 2πf/fs

    float omega = DSPMath::frequencyToRadians(frequency, mSampleRate);

    // Load coefficients
    float b0_val = b0.load(std::memory_order_relaxed);
    float b1_val = b1.load(std::memory_order_relaxed);
    float b2_val = b2.load(std::memory_order_relaxed);
    float a1_val = a1.load(std::memory_order_relaxed);
    float a2_val = a2.load(std::memory_order_relaxed);

    // Numerator: B(e^jω) = b0 + b1*e^(-jω) + b2*e^(-j2ω)
    float cos1 = std::cos(omega);
    float cos2 = std::cos(2.0f * omega);
    float sin1 = std::sin(omega);
    float sin2 = std::sin(2.0f * omega);

    float numReal = b0_val + b1_val * cos1 + b2_val * cos2;
    float numImag = -b1_val * sin1 - b2_val * sin2;
    float numMag = std::sqrt(numReal * numReal + numImag * numImag);

    // Denominator: A(e^jω) = 1 + a1*e^(-jω) + a2*e^(-j2ω)
    float denReal = 1.0f + a1_val * cos1 + a2_val * cos2;
    float denImag = -a1_val * sin1 - a2_val * sin2;
    float denMag = std::sqrt(denReal * denReal + denImag * denImag);

    // Magnitude response
    return (denMag > DSPMath::EPSILON) ? (numMag / denMag) : 0.0f;
}

void BiquadFilter::updateCoefficients() {
    // Implementation based on "Audio EQ Cookbook" by Robert Bristow-Johnson
    // http://shepazu.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html

    float omega = DSPMath::frequencyToRadians(mFrequency, mSampleRate);
    float sinOmega = std::sin(omega);
    float cosOmega = std::cos(omega);
    float alpha = sinOmega / (2.0f * mQ);
    float A = DSPMath::dbToLinear(mGainDb / 2.0f);  // sqrt of gain for shelves

    float b0_new, b1_new, b2_new, a0, a1_new, a2_new;

    switch (mType) {
        case Type::LPF:
            b0_new = (1.0f - cosOmega) / 2.0f;
            b1_new = 1.0f - cosOmega;
            b2_new = (1.0f - cosOmega) / 2.0f;
            a0 = 1.0f + alpha;
            a1_new = -2.0f * cosOmega;
            a2_new = 1.0f - alpha;
            break;

        case Type::HPF:
            b0_new = (1.0f + cosOmega) / 2.0f;
            b1_new = -(1.0f + cosOmega);
            b2_new = (1.0f + cosOmega) / 2.0f;
            a0 = 1.0f + alpha;
            a1_new = -2.0f * cosOmega;
            a2_new = 1.0f - alpha;
            break;

        case Type::BPF:
            b0_new = alpha;
            b1_new = 0.0f;
            b2_new = -alpha;
            a0 = 1.0f + alpha;
            a1_new = -2.0f * cosOmega;
            a2_new = 1.0f - alpha;
            break;

        case Type::NOTCH:
            b0_new = 1.0f;
            b1_new = -2.0f * cosOmega;
            b2_new = 1.0f;
            a0 = 1.0f + alpha;
            a1_new = -2.0f * cosOmega;
            a2_new = 1.0f - alpha;
            break;

        case Type::PEAK:
            b0_new = 1.0f + alpha * A;
            b1_new = -2.0f * cosOmega;
            b2_new = 1.0f - alpha * A;
            a0 = 1.0f + alpha / A;
            a1_new = -2.0f * cosOmega;
            a2_new = 1.0f - alpha / A;
            break;

        case Type::LOW_SHELF: {
            float beta = std::sqrt(A) / mQ;
            b0_new = A * ((A + 1.0f) - (A - 1.0f) * cosOmega + beta * sinOmega);
            b1_new = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosOmega);
            b2_new = A * ((A + 1.0f) - (A - 1.0f) * cosOmega - beta * sinOmega);
            a0 = (A + 1.0f) + (A - 1.0f) * cosOmega + beta * sinOmega;
            a1_new = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosOmega);
            a2_new = (A + 1.0f) + (A - 1.0f) * cosOmega - beta * sinOmega;
            break;
        }

        case Type::HIGH_SHELF: {
            float beta = std::sqrt(A) / mQ;
            b0_new = A * ((A + 1.0f) + (A - 1.0f) * cosOmega + beta * sinOmega);
            b1_new = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosOmega);
            b2_new = A * ((A + 1.0f) + (A - 1.0f) * cosOmega - beta * sinOmega);
            a0 = (A + 1.0f) - (A - 1.0f) * cosOmega + beta * sinOmega;
            a1_new = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosOmega);
            a2_new = (A + 1.0f) - (A - 1.0f) * cosOmega - beta * sinOmega;
            break;
        }

        default:
            // Should not reach here
            b0_new = 1.0f;
            b1_new = 0.0f;
            b2_new = 0.0f;
            a0 = 1.0f;
            a1_new = 0.0f;
            a2_new = 0.0f;
            break;
    }

    // Normalize by a0 and store atomically
    float a0_inv = 1.0f / a0;
    b0.store(b0_new * a0_inv, std::memory_order_release);
    b1.store(b1_new * a0_inv, std::memory_order_release);
    b2.store(b2_new * a0_inv, std::memory_order_release);
    a1.store(a1_new * a0_inv, std::memory_order_release);
    a2.store(a2_new * a0_inv, std::memory_order_release);
}

float BiquadFilter::clampFrequency(float freq) const {
    // Nyquist limit - leave some headroom
    float maxFreq = mSampleRate * 0.49f;
    return std::clamp(freq, 10.0f, maxFreq);
}
