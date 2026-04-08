#pragma once

#include "DelayLine.h"
#include <vector>
#include <atomic>
#include <utility>

/**
 * EarlyReflections - Generador de primeras reflexiones para reverb profesional
 *
 * Las early reflections son las primeras reflexiones del sonido que llegan al oyente
 * después del sonido directo, pero antes del late reverb. Son cruciales para:
 * - Percepción del tamaño del espacio
 * - Sensación de distancia de la fuente
 * - Localización espacial
 * - Carácter del espacio (sala vs catedral)
 *
 * Algoritmo:
 * - Multi-tap delay con 12 reflexiones
 * - Tiempos inspirados en espacios reales (5-80ms)
 * - Allpass filters para difusión/densidad
 * - Control de diffusion (0 = discrete taps, 1 = dense cluster)
 *
 * RT-Safety:
 * - Todas las operaciones son lock-free
 * - No hay allocations en process()
 * - Buffers pre-allocados en constructor
 *
 * Referencias:
 * - "Room Reflection Modeling" - Schroeder, Manfred
 * - "Dattorro Reverb" - Jon Dattorro (1997)
 * - docs/reverb-professional-iteration.md
 *
 * @author NoisyPad Audio Team
 * @date 2025
 */
class EarlyReflections {
public:
    /**
     * Constructor.
     * @param sampleRate Sample rate en Hz
     */
    explicit EarlyReflections(int sampleRate);

    /**
     * Procesa un sample de entrada y retorna early reflections.
     *
     * @param input Sample mono de entrada
     * @return Sample mono con early reflections aplicadas
     */
    float process(float input);

    /**
     * Procesa un bloque de samples.
     *
     * @param input Buffer de entrada (mono)
     * @param output Buffer de salida (mono)
     * @param numSamples Número de samples a procesar
     */
    void processBlock(const float* input, float* output, int numSamples);

    /**
     * Establece el control de difusión.
     * 0.0 = Reflexiones discretas (taps individuales audibles)
     * 1.0 = Reflexiones densas (cluster difuso)
     *
     * @param amount Cantidad de difusión [0, 1]
     */
    void setDiffusion(float amount);

    /**
     * Obtiene el nivel de difusión actual.
     * @return Difusión [0, 1]
     */
    float getDiffusion() const { return mDiffusion.load(std::memory_order_relaxed); }

    /**
     * Establece el tamaño del espacio (escala los delay times).
     * 0.5 = Espacio pequeño
     * 1.0 = Espacio medio
     * 2.0 = Espacio grande
     *
     * @param size Factor de tamaño [0.5, 2.0]
     */
    void setSize(float size);

    /**
     * Cambia el sample rate.
     * Recalcula todos los delay times y reconfigura buffers.
     *
     * @param sampleRate Nuevo sample rate en Hz
     */
    void setSampleRate(int sampleRate);

    /**
     * Resetea todos los buffers internos a cero.
     * Útil para evitar artefactos al cambiar presets.
     */
    void reset();

private:
    // Sample rate
    int mSampleRate;

    // ===== EARLY REFLECTION TAPS =====
    // 12 reflexiones con tiempos y ganancias específicas
    // Inspirados en estudios de espacios reales
    static constexpr int NUM_TAPS = 12;

    // Tiempos base de delay en milisegundos (para size=1.0)
    // Basados en patrones de reflexión de una sala rectangular
    static constexpr float BASE_DELAY_TIMES_MS[NUM_TAPS] = {
        5.0f,   // Primera reflexión (pared lateral)
        8.9f,   // Pared opuesta
        12.3f,  // Piso
        15.8f,  // Techo
        19.2f,  // Esquina cercana
        23.7f,  // Segunda reflexión lateral
        28.4f,  // Pared trasera
        33.5f,  // Combinación piso-techo
        39.1f,  // Esquina lejana
        45.6f,  // Triple reflexión
        53.2f,  // Reflexión compleja 1
        62.3f   // Reflexión compleja 2
    };

    // Ganancias de cada tap (simulan absorción)
    static constexpr float TAP_GAINS[NUM_TAPS] = {
        0.841f,  // Primera reflexión fuerte
        0.707f,
        0.630f,
        0.561f,
        0.500f,
        0.445f,
        0.397f,
        0.354f,
        0.315f,
        0.281f,
        0.250f,
        0.223f   // Última reflexión más débil
    };

    // Delay line principal para early reflections
    DelayLine mDelayLine;

    // ===== DIFFUSION NETWORK =====
    // 4 allpass filters para crear difusión
    struct AllpassFilter {
        DelayLine delay;
        float gain;

        AllpassFilter(int maxDelaySamples, int sampleRate)
            : delay(static_cast<float>(maxDelaySamples) / sampleRate * 1000.0f, sampleRate),
              gain(0.7f) {}

        // Make movable for std::vector
        AllpassFilter(AllpassFilter&&) = default;
        AllpassFilter& operator=(AllpassFilter&&) = default;

        // Delete copy constructor/assignment (not needed, prevent accidental copies)
        AllpassFilter(const AllpassFilter&) = delete;
        AllpassFilter& operator=(const AllpassFilter&) = delete;

        float process(float input) {
            float delayed = delay.read(delay.getMaxDelaySamples() - 1);
            float output = -gain * input + delayed;
            delay.write(input + gain * output);
            return output;
        }

        void reset() {
            delay.clear();
        }
    };

    std::vector<AllpassFilter> mDiffusionAllpass;

    // ===== PARÁMETROS =====
    std::atomic<float> mDiffusion{0.7f};  // Cantidad de difusión [0, 1]
    std::atomic<float> mSize{1.0f};        // Factor de tamaño [0.5, 2.0]

    // ===== HELPERS =====
    void initializeDiffusionNetwork();
    float processSingleTap(int tapIndex, float delayTimeMs) const;
};
