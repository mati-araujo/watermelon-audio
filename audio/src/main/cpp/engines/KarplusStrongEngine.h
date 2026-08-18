#pragma once

#include "SynthEngine.h"
#include "../dsp/DelayLine.h"
#include <algorithm>  // std::clamp / std::min / std::max
#include <cmath>
#include <random>

/**
 * @class KarplusStrongEngine
 * @brief Physical modeling synthesis via Karplus-Strong algorithm
 *
 * Produces plucked string, percussion, and kalimba-like sounds.
 *
 * Algorithm:
 *   1. Excitation: short burst of noise/impulse (duration ≈ 1 period)
 *   2. Delay line: circular buffer, length = sampleRate / frequency
 *   3. Lowpass filter in feedback loop: one-pole LP for string damping
 *   4. Feedback coefficient controls decay time
 *   5. Output: delay line output × amplitude
 *
 * Parameters:
 *   0 - Brightness: LP filter cutoff (0=dark warm, 1=bright metallic)
 *   1 - Decay: feedback coefficient (0=short percussive, 1=long sustain)
 *   2 - Excitation: burst character (0=noise, 0.5=impulse, 1=swept)
 *
 * RT-Safety:
 *   - Delay line pre-allocated in prepare() for lowest frequency (20 Hz)
 *   - No allocations in process()
 *   - Random generator uses deterministic xorshift (no syscalls)
 */
class KarplusStrongEngine : public SynthEngine {
public:
    static constexpr int PARAM_BRIGHTNESS = 0;
    static constexpr int PARAM_DECAY = 1;
    static constexpr int PARAM_EXCITATION = 2;

    KarplusStrongEngine() {
        // Set defaults
        mParams[PARAM_BRIGHTNESS].store(0.5f, std::memory_order_relaxed);
        mParams[PARAM_DECAY].store(0.5f, std::memory_order_relaxed);
        mParams[PARAM_EXCITATION].store(0.3f, std::memory_order_relaxed);
    }

    void prepare(int32_t sampleRate, int32_t maxBlockSize) override {
        SynthEngine::prepare(sampleRate, maxBlockSize);

        // Pre-allocate delay line for lowest frequency (20 Hz = 50ms)
        float maxDelayMs = 1000.0f / 20.0f + 1.0f; // ~51ms
        mDelayLine = DelayLine(maxDelayMs, static_cast<float>(sampleRate));

        mFilterStateL = 0.0f;
        mFilterStateR = 0.0f;
        mPrevFrequency = 0.0f;
        mExcitationRemaining = 0;
        mRngState = 12345; // Deterministic seed
    }

    void reset() override {
        // Trigger new excitation on next process() call
        mNeedsExcitation.store(true, std::memory_order_release);
        mFilterStateL = 0.0f;
        mFilterStateR = 0.0f;
    }

    void process(float* buffer, int32_t numFrames,
                 float frequency, float amplitude) override {
        // Read parameters with smoothing (prevents zipper noise)
        const float brightness = smoothParam(PARAM_BRIGHTNESS, numFrames);
        const float decay = smoothParam(PARAM_DECAY, numFrames);
        const float excitation = smoothParam(PARAM_EXCITATION, numFrames);

        // Clamp frequency to valid range
        frequency = std::clamp(frequency, 20.0f, 20000.0f);

        // El periodo musical pedido, en muestras.
        const float periodSamples = static_cast<float>(mSampleRate) / frequency;

        // Feedback coefficient: maps decay [0,1] → [0.9, 0.999]
        const float feedbackCoeff = 0.9f + decay * 0.099f;

        // LP filter coefficient: brightness [0,1] → [0.1, 0.95]
        // Lower = darker (more filtering), Higher = brighter
        const float lpCoeff = 0.1f + brightness * 0.85f;

        // COMPENSACION DEL RETARDO DEL LAZO
        // ---------------------------------
        // El lazo no es solo la linea de retardo: el filtro de un polo tambien
        // retarda. Si no se descuenta, el lazo entero mide mas que `fs/f` y la
        // cuerda suena BAJA, tanto mas cuanto mas aguda la nota y tanto menos
        // cuanto mas alto el sample rate — que ademas rompe el criterio de
        // invariancia de rate de WD-2.3.2.
        //
        // Lo que hay que descontar es el retardo de FASE del filtro a la
        // frecuencia que se pide, NO su retardo de grupo en DC. Los dos valen
        // `(1-a)/a` en el limite de frecuencia cero y por eso la cuenta de DC
        // parece suficiente con `brightness` en su default (0,905 contra 0,902
        // a 440 Hz, 0,1 cents). Deja de serlo con la cuerda OSCURA, donde el
        // polo pesa: con `brightness` en 0 el retardo de grupo dice 9,0
        // muestras y el de fase 8,06, y descontar 9 dejaba la nota **16 cents
        // ALTA** a 440 Hz — medido, no estimado.
        //
        // El interpolador lineal NO aporta nada que descontar: su retardo es
        // exactamente `delaySamples`. Medido por centroide de la respuesta al
        // impulso del lazo abierto, el unico sobrante es el del filtro.
        //
        // El piso de 2 muestras acota el lazo. Medido sobre todo el dominio de
        // (rate, frecuencia, brightness): NUNCA muerde por debajo de Nyquist —
        // el minimo de `periodSamples - filterDelay` es exactamente 2,0, y se
        // toca en f = fs/2, porque el retardo de fase tiende a 0 cuando omega
        // tiende a pi. Solo actua ARRIBA de Nyquist, y ahi hay otra cosa mal:
        // `frequency` se acota contra la constante 20 kHz y no contra fs/2, que
        // es el mismo defecto que WD-3.5 arreglo en los efectos. En ese rincon
        // el engine ya sale mudo por una tercera razon (la rafaga de excitacion
        // dura `(int)periodSamples` muestras, o sea CERO), asi que sacar este
        // piso hoy no cambia nada observable — es un guard, no una correccion.
        const float omega = 2.0f * static_cast<float>(M_PI) / periodSamples;
        const float pole = 1.0f - lpCoeff;
        const float filterDelay =
            std::atan2(pole * std::sin(omega), 1.0f - pole * std::cos(omega)) / omega;
        const float loopDelay = std::max(2.0f, periodSamples - filterDelay);

        // Check if we need a new excitation burst
        bool needsExcitation = mNeedsExcitation.load(std::memory_order_acquire);

        // Retrigger on significant frequency change (user moved finger)
        if (mPrevFrequency > 0.0f && std::abs(frequency - mPrevFrequency) > mPrevFrequency * 0.05f) {
            needsExcitation = true;
        }
        mPrevFrequency = frequency;

        if (needsExcitation) {
            mNeedsExcitation.store(false, std::memory_order_release);
            startExcitation(periodSamples);
        }

        for (int32_t i = 0; i < numFrames; ++i) {
            // Read from delay line (fractional for pitch accuracy)
            float delayed = mDelayLine.readInterpolated(loopDelay, DelayLine::Interpolation::LINEAR);

            // One-pole lowpass filter (string damping)
            // y[n] = coeff * x[n] + (1-coeff) * y[n-1]
            mFilterStateL = lpCoeff * delayed + (1.0f - lpCoeff) * mFilterStateL;

            // Auto-retrigger: when energy in delay line drops below threshold.
            // This gives continuous sound while touching the XY pad (bowed
            // string feel).
            //
            // VA ADENTRO DEL LOOP DE MUESTRAS, y esa es la unica forma de que
            // "cada ~50 ms" sea cierto: cuando el contador vivia afuera contaba
            // BLOQUES, asi que el chequeo caia cada `mSampleRate/20` bloques —
            // 25,6 s con bloques de 512 a 44,1 kHz. La cuerda de 440 Hz se
            // apagaba a los ~0,4 s y no volvia nunca, y el periodo del defecto
            // escalaba con el tamaño del bloque.
            if (mExcitationRemaining <= 0) {
                mEnergyAccumulator += std::abs(mFilterStateL);
                mEnergySampleCount++;
                if (mEnergySampleCount >= mSampleRate / 20) { // Check every ~50ms
                    float avgEnergy = mEnergyAccumulator / static_cast<float>(mEnergySampleCount);
                    if (avgEnergy < 0.001f) {
                        startExcitation(periodSamples); // Signal too quiet, re-trigger
                    } else {
                        mEnergyAccumulator = 0.0f;
                        mEnergySampleCount = 0;
                    }
                }
            }

            // Generate excitation if in burst phase
            float exc = 0.0f;
            if (mExcitationRemaining > 0) {
                exc = generateExcitation(excitation);
                mExcitationRemaining--;
            }

            // Feedback: filtered delayed signal + excitation
            float feedback = exc + mFilterStateL * feedbackCoeff;

            // Soft-clip to prevent runaway feedback
            feedback = std::tanh(feedback);

            // Write to delay line
            mDelayLine.write(feedback);

            // Output (stereo, slight detuning for width)
            float sample = mFilterStateL * amplitude;

            // Sanitize output
            if (!std::isfinite(sample)) sample = 0.0f;

            buffer[i * 2]     = sample;
            buffer[i * 2 + 1] = sample;
        }
    }

    const char* getName() const override { return "Karplus-Strong"; }
    int getParameterCount() const override { return 3; }

    EngineParameterDef getParameterDef(int paramId) const override {
        switch (paramId) {
            case PARAM_BRIGHTNESS:
                return {"Brightness", "BRIGHT", 0.0f, 1.0f, 0.5f};
            case PARAM_DECAY:
                return {"Decay", "DECAY", 0.0f, 1.0f, 0.5f};
            case PARAM_EXCITATION:
                return {"Excitation", "EXCITE", 0.0f, 1.0f, 0.3f};
            default:
                return {"Unknown", "?", 0.0f, 1.0f, 0.0f};
        }
    }

private:
    DelayLine mDelayLine{50.0f, 48000.0f};  // Will be re-initialized in prepare()

    float mFilterStateL = 0.0f;
    float mFilterStateR = 0.0f;
    float mPrevFrequency = 0.0f;
    float mExcitationPhase = 0.0f;
    int mExcitationRemaining = 0;

    // Energy tracking for auto-retrigger
    float mEnergyAccumulator = 0.0f;
    int mEnergySampleCount = 0;

    std::atomic<bool> mNeedsExcitation{true};

    // Fast deterministic RNG (xorshift32) — RT-safe, no syscalls
    uint32_t mRngState = 12345;

    /// Arranca una rafaga de excitacion de ~un periodo y reinicia el medidor de
    /// energia. La longitud va en el periodo MUSICAL, no en el largo del lazo:
    /// el lazo lleva descontado el retardo del filtro y esa correccion no tiene
    /// nada que ver con cuanto tiene que durar el pluck.
    void startExcitation(float periodSamples) {
        mExcitationRemaining = static_cast<int>(periodSamples);
        mExcitationPhase = 0.0f;
        mEnergyAccumulator = 0.0f;
        mEnergySampleCount = 0;
    }

    float fastRandom() {
        mRngState ^= mRngState << 13;
        mRngState ^= mRngState >> 17;
        mRngState ^= mRngState << 5;
        // Convert to [-1, 1] range
        return static_cast<float>(static_cast<int32_t>(mRngState)) / static_cast<float>(INT32_MAX);
    }

    /**
     * @brief Generate excitation sample based on type
     * @param type 0=noise, 0.5=impulse, 1=swept frequency
     */
    float generateExcitation(float type) {
        if (type < 0.33f) {
            // Pure noise burst
            return fastRandom() * 0.8f;
        } else if (type < 0.66f) {
            // Impulse + noise mix (punchy pluck)
            float noise = fastRandom() * 0.3f;
            float impulse = (mExcitationRemaining > 2) ? 0.0f : 1.0f;
            return noise + impulse * 0.7f;
        } else {
            // Swept tone burst (metallic/bell character)
            mExcitationPhase += 0.1f + type * 0.4f;
            if (mExcitationPhase > static_cast<float>(M_PI) * 2.0f) {
                mExcitationPhase -= static_cast<float>(M_PI) * 2.0f;
            }
            float swept = std::sin(mExcitationPhase) * 0.6f;
            float noise = fastRandom() * 0.2f;
            return swept + noise;
        }
    }
};
