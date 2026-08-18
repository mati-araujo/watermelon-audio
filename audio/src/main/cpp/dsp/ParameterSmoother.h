#ifndef PARAMETER_SMOOTHER_H
#define PARAMETER_SMOOTHER_H

#include <cmath>
#include <atomic>

/**
 * @file ParameterSmoother.h
 * @brief One-pole smoothing filter for audio parameters
 *
 * Prevents audible clicks and artifacts when parameters change abruptly.
 * Useful for cutoff frequency, resonance, and other continuous parameters.
 *
 * Thread Safety:
 * - process() can be called from the audio thread (lock-free)
 * - setCoefficient(), setSmoothingTime(), reset() can be called from UI thread
 * - Uses atomic operations with relaxed memory ordering for performance
 */

/**
 * @class ParameterSmoother
 * @brief Single-pole smoothing filter for parameter interpolation
 *
 * Uses exponential smoothing to gradually transition between parameter values.
 * The coefficient determines the smoothing amount (closer to 1.0 = more smoothing).
 *
 * Thread-safe: Uses atomic<float> for cross-thread access.
 */
class ParameterSmoother {
public:
    /**
     * @brief Constructor with optional smoothing coefficient
     * @param coeff Smoothing coefficient [0.0, 1.0). Default: 0.99
     *              Higher values = smoother but slower response
     *              Typical values: 0.95-0.999
     */
    explicit ParameterSmoother(float coeff = 0.99f)
        : mCurrent(0.0f), mCoefficient(clampCoefficient(coeff)) {
    }

    /**
     * @brief Process a target value and return smoothed output
     * @param target Target value to reach
     * @return Smoothed value
     *
     * RT-SAFE: Uses relaxed memory ordering for minimal overhead.
     * Can be called from audio thread without blocking.
     */
    /// ⚠️ EN REPOSO ESTE CAMINO NO ES EXACTO, y se deja asi a proposito.
    /// `c*x + (1-c)*x` vale `x` en los reales y no siempre en float: medido, gcc
    /// del CI devuelve `x` + 1 ulp para 0,7 y 0,35, y Apple clang devuelve `x`.
    /// `processBlock()` lo evita con una salida temprana; aca NO se hace lo
    /// mismo porque este es el camino POR MUESTRA de los efectos y cambiarlo
    /// altera su salida bit a bit en las plataformas donde hoy oscila — o sea
    /// que rompe los golden `.f32`, que se re-capturan CONSCIENTEMENTE y en su
    /// propia tanda (misma regla que WD-3.3). Medido: con la salida temprana
    /// agregada aca, los 886 tests pasan en macOS; falta la evidencia de Linux.
    inline float process(float target) {
        float current = mCurrent.load(std::memory_order_relaxed);
        float coeff = mCoefficient.load(std::memory_order_relaxed);
        float newCurrent = coeff * current + (1.0f - coeff) * target;
        mCurrent.store(newCurrent, std::memory_order_relaxed);
        return newCurrent;
    }

    /**
     * @brief Advance the smoother by `frames` samples in a single call
     * @param target Target value to reach (constant over the whole block)
     * @param frames Number of samples the block covers
     * @return Smoothed value after those `frames` samples
     *
     * RT-SAFE: one powf per call, no allocation, no locks.
     *
     * POR QUE ESTO EXISTE, Y QUE DEFECTO CIERRA
     * -----------------------------------------
     * `setSmoothingTime()` calcula el coeficiente para llamadas POR MUESTRA.
     * Quien lee un parametro UNA VEZ POR BLOQUE y llama a `process()` avanza el
     * smoother una sola muestra por bloque, asi que el tiempo de suavizado real
     * queda multiplicado por el tamaño del bloque: los 5 ms declarados por
     * `SynthEngine` eran **2,56 s** con bloques de 512, y el numero cambiaba con
     * el bloque.
     *
     * Eso no era un detalle cosmetico. En Karplus-Strong dejaba `brightness` en
     * ~0,02 durante el primer medio segundo (en vez de 0,5), el filtro del lazo
     * aportaba 7,5 muestras de retardo de grupo en vez de 0,9, y la cuerda
     * sonaba **118 cents baja** a 440 Hz sobre 44,1 kHz. El sintoma dependia del
     * TAMAÑO DE BLOQUE — 1,3 muestras con bloques de 16, 7,6 con bloques de 1024
     * — que es la firma de este defecto y no la de un retardo del lazo.
     *
     * `coeff^frames` es EXACTO, no una aproximacion: aplicar n veces
     * `c*x + (1-c)*t` con `t` constante da `c^n*x + (1-c^n)*t`. O sea que esto
     * entrega el mismo valor que suavizar por muestra, muestreado en el borde
     * del bloque, y el resultado deja de depender del tamaño del bloque.
     */
    inline float processBlock(float target, int frames) {
        if (frames <= 0) {
            return mCurrent.load(std::memory_order_relaxed);
        }
        float current = mCurrent.load(std::memory_order_relaxed);
        // YA LLEGO: n aplicaciones de `c*x + (1-c)*x` dejan `x` donde estaba, asi
        // que no hay nada que calcular. Salir aca no es una optimizacion
        // oportunista: es lo unico que mantiene el resultado EXACTO.
        //
        // La identidad `d*x + (1-d)*x == x` vale en los reales y NO en float, y
        // como `d = powf(coeff, frames)` depende del tamaño del bloque, el
        // ultimo bit del resultado terminaba dependiendo de el. Medido: con
        // `target` 0,3 a 44,1 kHz, n=512 daba 0x3e99999b contra 0x3e99999a del
        // resto; con 0,1 a 48 kHz salian tres valores distintos para cuatro
        // tamaños. Un ulp alcanza — FMEngine lo amplifica por su lazo de
        // realimentacion y GranularEngine por la planificacion de granos, y los
        // dos dejaban de sonar igual segun el bloque que negociara el device.
        //
        // Karplus-Strong no lo mostraba porque sus defaults son 0,5: potencia de
        // dos, donde la identidad SI es exacta en float. Un test que solo lo
        // mirara a el habria dado verde.
        //
        // OJO CON EL ORDEN: esta salida va ANTES de delegar `frames == 1` en
        // `process()`, y no es cosmetico. `process()` recalcula
        // `c*x + (1-c)*x`, que en reposo tampoco es exacto en float — y el
        // resultado depende del COMPILADOR: en Apple clang/arm64 da `x` exacto
        // y en gcc/x86 del CI da `x` + 1 ulp. Delegar el caso de un frame hacia
        // ahi hacia que `processBlock` fuera exacto para todo n MENOS n=1, y
        // sólo en Linux. El gate local daba verde y el CI rojo.
        if (current == target) {
            return current;
        }
        if (frames == 1) {
            return process(target);
        }
        float coeff = mCoefficient.load(std::memory_order_relaxed);
        float decay = std::pow(coeff, static_cast<float>(frames));
        float newCurrent = decay * current + (1.0f - decay) * target;
        mCurrent.store(newCurrent, std::memory_order_relaxed);
        return newCurrent;
    }

    /**
     * @brief Reset the smoother to a specific value (no smoothing)
     * @param value Value to reset to
     *
     * Thread-safe: Can be called from any thread.
     */
    inline void reset(float value) {
        mCurrent.store(value, std::memory_order_relaxed);
    }

    /**
     * @brief Set the smoothing coefficient
     * @param coeff New coefficient [0.0, 1.0)
     *
     * Thread-safe: Can be called from UI thread.
     */
    inline void setCoefficient(float coeff) {
        mCoefficient.store(clampCoefficient(coeff), std::memory_order_relaxed);
    }

    /**
     * @brief Set the smoothing time in milliseconds
     * @param timeMs Desired smoothing time in milliseconds
     * @param sampleRate Sample rate in Hz
     *
     * Calculates the coefficient to achieve approximately the desired
     * smoothing time (time to reach ~63% of the target value).
     *
     * Thread-safe: Can be called from UI thread.
     */
    inline void setSmoothingTime(float timeMs, float sampleRate) {
        // Calculate coefficient for desired time constant
        // tau = -1 / (sampleRate * ln(coefficient))
        // coefficient = exp(-1 / (tau * sampleRate))
        float tau = timeMs / 1000.0f;  // Convert to seconds
        float coeff = expf(-1.0f / (tau * sampleRate));
        mCoefficient.store(clampCoefficient(coeff), std::memory_order_relaxed);
    }

    /**
     * @brief Get the current smoothed value
     * @return Current value
     *
     * Thread-safe: Can be called from any thread.
     */
    inline float getCurrent() const {
        return mCurrent.load(std::memory_order_relaxed);
    }

private:
    /**
     * @brief Clamp coefficient to valid range [0.0, 0.999]
     */
    static inline float clampCoefficient(float coeff) {
        if (coeff < 0.0f) return 0.0f;
        if (coeff >= 1.0f) return 0.999f;
        return coeff;
    }

    std::atomic<float> mCurrent;      ///< Current smoothed value (atomic for RT-safety)
    std::atomic<float> mCoefficient;  ///< Smoothing coefficient (atomic for RT-safety)
};

#endif // PARAMETER_SMOOTHER_H
