#include "FilterEffect.h"
#include "EffectDefaults.h"
#include <atomic>
#include <cmath>
#include <algorithm>

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

FilterEffect::FilterEffect() {
    // Initialize smoothers with default values to avoid transients
    cutoffSmoother.reset(cutoff.load());
    resonanceSmoother.reset(resonance.load());
    // Initialize with default sample rate from constants
    mSampleRate = DEFAULT_SAMPLE_RATE;
    updateCoefficients();
}

void FilterEffect::setCutoff(float freq) {
    cutoff.store(std::clamp(freq, 20.0f, 20000.0f));
    updateCoefficients();
}

void FilterEffect::setResonance(float res) {
    // PHASE 4: Rango Q expandido para efectos profesionales
    res = std::clamp(res, MIN_Q, MAX_Q);
    resonance.store(res);

    // PHASE 4: Calcular compensación de ganancia para Q alto
    // Previene runaway levels en self-oscillation
    if (res > SELF_OSC_THRESHOLD) {
        // Reducir ganancia proporcionalmente al exceso de Q
        mGainCompensation = SELF_OSC_THRESHOLD / res;
    } else {
        mGainCompensation = 1.0f;
    }

    updateCoefficients();
}

void FilterEffect::setType(FilterType t) {
    type = t;
    updateCoefficients();
}

void FilterEffect::process(float* input, float* output, int numFrames) {
    // IMPROVED: Apply parameter smoothing to prevent clicks
    // Smooth parameters once per buffer (efficient approach)
    float targetCutoff = cutoff.load();
    float targetResonance = resonance.load();

    float smoothedCutoff = cutoffSmoother.process(targetCutoff);
    float smoothedResonance = resonanceSmoother.process(targetResonance);

    // Update coefficients if parameters have changed significantly
    if (std::abs(smoothedCutoff - cutoffSmoother.getCurrent()) > 0.5f ||
        std::abs(smoothedResonance - resonanceSmoother.getCurrent()) > 0.001f) {

        // Temporarily update values for coefficient calculation
        cutoff.store(smoothedCutoff);
        resonance.store(smoothedResonance);
        updateCoefficients();
    }

    // FIXED: Leer índice actual de coeficientes atómicamente (RT-safe)
    int idx = currentCoeffsIndex.load(std::memory_order_acquire);
    const Coefficients& c = coeffs[idx];

    // CRITICAL FIX: Denormal threshold para prevenir CPU slowdown (70-100x)
    constexpr float DENORMAL_THRESHOLD = 1e-20f;

    // PHASE 4: Cache gain compensation locally for RT thread
    const float gainComp = mGainCompensation;

    for (int i = 0; i < numFrames * 2; i += 2) {
        // left
        float x = input[i];
        float y = c.b0 * x + c.b1 * x1_l + c.b2 * x2_l - c.a1 * y1_l - c.a2 * y2_l;

        // CRITICAL FIX: Flush denormals to zero (previene slowdown)
        if (std::abs(y) < DENORMAL_THRESHOLD) y = 0.0f;

        // PHASE 4: Apply gain compensation for high Q
        output[i] = y * gainComp;
        x2_l = x1_l; x1_l = x;
        y2_l = y1_l; y1_l = y;

        // right
        x = input[i+1];
        y = c.b0 * x + c.b1 * x1_r + c.b2 * x2_r - c.a1 * y1_r - c.a2 * y2_r;

        // CRITICAL FIX: Flush denormals to zero (previene slowdown)
        if (std::abs(y) < DENORMAL_THRESHOLD) y = 0.0f;

        // PHASE 4: Apply gain compensation for high Q
        output[i+1] = y * gainComp;
        x2_r = x1_r; x1_r = x;
        y2_r = y1_r; y1_r = y;
    }
}

void FilterEffect::setParam(int paramId, float value) {
    switch(paramId) {
        case 0: setCutoff(value); break;
        case 1: setResonance(value); break;
        case 2: setType(static_cast<FilterType>(static_cast<int>(value))); break;
    }
}

float FilterEffect::getParam(int paramId) {
    switch(paramId) {
        case 0: return cutoff.load();
        case 1: return resonance.load();
        case 2: return static_cast<float>(type);
    }
    return 0.0f;
}

void FilterEffect::setSampleRate(int sampleRate) {
    mSampleRate = sampleRate;
    // Recalculate coefficients with new sample rate
    updateCoefficients();
}

void FilterEffect::updateCoefficients() {
    // FIXED: Escribir en el buffer NO-actual, luego hacer swap atómico
    int currentIdx = currentCoeffsIndex.load(std::memory_order_relaxed);
    int nextIdx = 1 - currentIdx;  // Alternar entre 0 y 1

    float c = cutoff.load();
    float r = resonance.load();
    // IMPROVED: Use dynamic sample rate instead of constant
    float omega = 2.0f * kPi * c / mSampleRate;
    float alpha = sinf(omega) / (2.0f * r);
    float cos_omega = cosf(omega);
    // Initialized to the value every handled branch assigns, so an unhandled
    // `type` can never divide the coefficients by an uninitialized a0 below.
    float a0 = 1.0f + alpha;

    Coefficients& next = coeffs[nextIdx];

    if (type == LPF) {
        next.b0 = (1.0f - cos_omega) / 2.0f;
        next.b1 = 1.0f - cos_omega;
        next.b2 = next.b0;
        a0 = 1.0f + alpha;
        next.a1 = -2.0f * cos_omega;
        next.a2 = 1.0f - alpha;
    } else if (type == HPF) {
        next.b0 = (1.0f + cos_omega) / 2.0f;
        next.b1 = -(1.0f + cos_omega);
        next.b2 = next.b0;
        a0 = 1.0f + alpha;
        next.a1 = -2.0f * cos_omega;
        next.a2 = 1.0f - alpha;
    } else if (type == BPF) {
        next.b0 = sinf(omega) / 2.0f;
        next.b1 = 0.0f;
        next.b2 = -sinf(omega) / 2.0f;
        a0 = 1.0f + alpha;
        next.a1 = -2.0f * cos_omega;
        next.a2 = 1.0f - alpha;
    }

    // normalizar
    next.b0 /= a0;
    next.b1 /= a0;
    next.b2 /= a0;
    next.a1 /= a0;
    next.a2 /= a0;

    // Atomic swap - el RT thread verá los nuevos coeficientes en el próximo callback
    currentCoeffsIndex.store(nextIdx, std::memory_order_release);
}
