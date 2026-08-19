#pragma once

/**
 * @file McLeodPitch.h
 * @brief Deteccion GRUESA de altura — que nota ES (REQ-001 S4).
 *
 * QUE PROBLEMA RESUELVE, Y POR QUE NO LO RESUELVE EL ESTIMADOR DE FASE
 * --------------------------------------------------------------------
 * `PhaseSlopeEstimator` mide con precision de milicents, pero **afina alrededor de un
 * objetivo: no lo busca**. Su rango de captura es `|Δf| < fs/2N`, o sea ±4,8 cents en C7.
 * Alguien tiene que decirle contra que medir, y ese alguien es esto: encontrar la altura
 * SIN saberla de antemano, con error < 50 cents — mil veces mas grosero, y suficiente para
 * elegir la nota.
 *
 * POR QUE MPM Y NO AUTOCORRELACION A SECAS
 * ----------------------------------------
 * El modo de falla que importa en un afinador de instrumentos de cuerda es el **error de
 * octava**, y la autocorrelacion cruda lo comete sistematicamente: su maximo global suele
 * caer en 2·τ cuando el fundamental es debil. En una bordona grave el fundamental puede
 * estar 20 dB por debajo del segundo parcial, asi que el caso patologico es tambien el caso
 * normal.
 *
 * El McLeod Pitch Method lo ataca en dos pasos:
 *
 *   1. **NSDF** —`2·r[τ] / m[τ]`— normaliza por la energia de la ventana desplazada, asi que
 *      los picos quedan en [-1, 1] y son comparables entre si. La autocorrelacion cruda
 *      decae con τ y eso solo ya sesga la eleccion.
 *   2. **Se elige el PRIMER pico por encima de `k · máximo`, no el maximo.** Ahi esta toda
 *      la defensa contra la octava: el pico en 2·τ puede ser mas alto, pero el de τ es el
 *      primero que supera el umbral. Bajar `k` a 1,0 convierte esto en "elegi el maximo" y
 *      reintroduce el error de octava — hay un test de mutacion que lo exige.
 *
 * SE DECIMA, Y ESO ES UNA DECISION DE COSTO CON UN NUMERO ATRAS
 * -------------------------------------------------------------
 * La NSDF cuesta O(W·τmax). A 48 kHz con τmax = fs/27,5 = 1745 y una ventana de 4096, son
 * ~7 millones de operaciones por deteccion — y a ritmo de tick eso no entra en el 5 % de CPU
 * del NFR-1.
 *
 * Se decima a ~24 kHz (factor entero, con pasabajos antialias antes). Ahi τmax baja a ~873 y
 * la ventana a 2048: **~1,8 millones**, casi cuatro veces menos. Lo que se pierde es
 * resolucion en la zona aguda —en C7 el periodo son 11,5 muestras— y por eso la
 * interpolacion parabolica del pico no es un adorno: sin ella el error en C7 se iria por
 * encima del presupuesto de 50 cents.
 *
 * NO decimar mas de 2 es deliberado: a 12 kHz el periodo de C7 son 5,7 muestras y ni la
 * interpolacion salva el error.
 */

#include "BiquadFilter.h"

#include <cstdint>
#include <vector>

namespace wma::dsp {

class McLeodPitch {
public:
    /// Rango de busqueda, en Hz. A0 (27,5) a C7 (2093) es el rango del AC, con margen.
    static constexpr double kMinHz = 26.0;
    static constexpr double kMaxHz = 2200.0;

    /// Rate objetivo despues de decimar. Ver la nota del encabezado.
    static constexpr int kTargetRate = 24000;

    /// Ventana de analisis, en muestras DECIMADAS. 2048 a 24 kHz son 85 ms y 2,3 periodos
    /// de A0 — el minimo para que la NSDF tenga algo que correlacionar en la nota mas grave.
    static constexpr int kWindowFrames = 2048;

    /**
     * Umbral relativo de seleccion de pico. **Es el parametro que evita el error de octava.**
     *
     * 0,9 es el valor del paper y el que se midio acá: se elige el PRIMER maximo que supere
     * `0,9 · máximo`, no el mas alto. Subirlo a 1,0 equivale a "elegi el maximo" y trae de
     * vuelta la octava; bajarlo demasiado hace elegir picos espurios de baja correlacion.
     */
    static constexpr double kPeakThreshold = 0.9;

    /**
     * Claridad minima para declarar que HAY una nota.
     *
     * Es el valor de la NSDF en el pico elegido: 1 = periodico perfecto, 0 = ruido. Con ruido
     * blanco los picos quedan bien por debajo de esto, y por eso el detector **no inventa una
     * nota** cuando no hay ninguna (AC-001.4).
     */
    static constexpr double kMinClarity = 0.5;

    /// Piso de nivel, lineal. Igual que el resto del camino de analisis.
    static constexpr float kSilenceFloor = 0.001f;

    /// Reserva el estado. **Unico punto que asigna.**
    void prepare(int sampleRate);

    /// Deja el detector indistinguible de recien preparado. No borra la configuracion.
    void reset();

    /**
     * Consume audio mono. El tamaño del bloque es libre y no puede cambiar el resultado.
     * @return true si esta llamada completo una ventana y produjo una estimacion nueva.
     */
    bool process(const float* mono, int numFrames);

    /// Ultima altura detectada en Hz, o 0 si no hay ninguna.
    double frequencyHz() const noexcept { return mFrequencyHz; }

    /// Claridad del pico elegido, 0..1. Es la confianza que cruza la frontera.
    double clarity() const noexcept { return mClarity; }

    /// true si la ultima ventana produjo una altura creible.
    bool hasPitch() const noexcept { return mHasPitch; }

    /// Ventanas completas analizadas desde el ultimo reset. Lo leen los tests.
    int windowsAnalyzed() const noexcept { return mWindows; }

    /// Factor de decimacion efectivo. Lo lee el test de costo y el de resolucion.
    int decimation() const noexcept { return mDecimation; }

private:
    void analyzeWindow();
    /// NSDF en un lag concreto. Se evalua bajo demanda: la busqueda no recorre todos.
    double nsdfAt(int lag) const;

    int mSampleRate{0};
    int mDecimation{1};
    double mWorkingRate{0.0};

    /// Pasabajos antialias, ANTES de decimar. Sin el, todo lo que este por encima del nuevo
    /// Nyquist se pliega adentro de la banda y la NSDF correlaciona basura.
    BiquadFilter mAntiAlias;
    int mPhase{0};

    std::vector<float> mWindow;
    std::vector<double> mNsdf;
    /**
     * Candidatos de la ventana actual: array FIJO, no vector.
     *
     * Un vector local asignaria en cada ventana. Uno miembro con `reserve()` no asignaria en
     * la practica, pero el lint de RT no puede saberlo y habria que escribirle una excepcion
     * — y una excepcion documentada es peor que no necesitarla. Con paso proporcional el
     * barrido evalua ~12·ln(τmax/τmin) ≈ 60 puntos, asi que 128 sobra con el doble.
     */
    static constexpr int kMaxCandidates = 128;
    int mKeyLags[kMaxCandidates]{};
    int mKeyCount{0};
    int mFilled{0};

    int mMinLag{0};
    int mMaxLag{0};

    double mFrequencyHz{0.0};
    double mClarity{0.0};
    bool mHasPitch{false};
    int mWindows{0};
};

}  // namespace wma::dsp
