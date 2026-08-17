#ifndef WMA_PITCH_HARNESS_H
#define WMA_PITCH_HARNESS_H

/**
 * WD-2.3.2 — medir la FUNDAMENTAL de un engine, y poder confiar en la medida.
 *
 * POR QUE ESTE HEADER EXISTE
 * --------------------------
 * El criterio de WD-2.3.2 dice: "para un Karplus-Strong, la frecuencia
 * fundamental medida por FFT no se corre entre 44,1 / 48 / 96 kHz". Escribir ese
 * test es facil; escribir uno cuyo numero SIGNIFIQUE algo costo tres
 * estimadores descartados, y los tres fallaban de formas que un verde habria
 * tapado:
 *
 *   1. MAXIMO GLOBAL de la autocorrelacion sobre banda ancha.
 *      Error de octava. Medido, exacto: un seno de 880 Hz daba 440,367 Hz.
 *      La ACF de una señal periodica es maxima en TODOS los multiplos del
 *      periodo, asi que el empate lo rompe el ruido numerico.
 *
 *   2. MENOR LAG que llega al 90 % del maximo.
 *      Mata el error de octava y trae otro: el umbral se cruza en el FLANCO de
 *      subida, no en el pico. Sesgo medido de -6 a -9 cents sobre un seno puro,
 *      que por definicion tiene que dar exacto.
 *
 *   3. UMBRAL + SUBIR AL MAXIMO LOCAL.
 *      Exacto en señales sinteticas... y CLAVADO EN EL BORDE del rango con la
 *      salida real de Karplus-Strong, que es ruidosa y tiene re-excitaciones.
 *      Devolvia exactamente `4 x lo pedido`, o sea el limite del barrido.
 *
 * La que quedo: MAXIMO GLOBAL EN BANDA ESTRECHA. La ambiguedad de octava se
 * resuelve por construccion —en +-30 % no entra ninguna octava— y no hay
 * heuristica que sesgue el pico.
 *
 * LA REGLA QUE SALIO DE AHI, Y QUE EL HEADER HACE CUMPLIR
 * ------------------------------------------------------
 * **Una medida pegada al borde del rango no es una medida.** Las tres versiones
 * malas devolvian numeros perfectamente plausibles; la unica forma de notarlo
 * fue exigirle al estimador que primero reprodujera señales de frecuencia
 * CONOCIDA. Por eso `fundamentalHz()` devuelve NEGATIVO cuando el pico cae en el
 * limite del barrido, en vez de entregar el borde como si fuera un resultado, y
 * por eso `test_engine_pitch.cpp` valida el instrumento con un test propio antes
 * de apuntarlo al motor.
 *
 * Validado a 44,1 / 48 / 96 kHz sobre seno puro y sobre un pluck sintetico cuyo
 * SEGUNDO ARMONICO es mas fuerte que la fundamental: **error <= 0,03 cents**.
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace wma::pitch {

inline constexpr double kPi = 3.14159265358979323846;

/// Los tres rates del requerimiento.
inline constexpr int kRates[3] = {44100, 48000, 96000};

/// Ancho de la banda de busqueda, como factor. 1,3 no es arbitrario: la octava
/// esta en 2,0, asi que +-30 % la deja afuera por construccion y no hace falta
/// ninguna heuristica para desambiguarla.
inline constexpr double kBand = 1.3;

// ---------------------------------------------------------------------------
// Señales de referencia — para validar el instrumento, no el motor
// ---------------------------------------------------------------------------

inline std::vector<float> sine(double hz, int rate, double seconds) {
    const int n = static_cast<int>(seconds * rate);
    std::vector<float> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        x[static_cast<size_t>(i)] =
            static_cast<float>(std::sin(2.0 * kPi * hz * i / rate));
    }
    return x;
}

/**
 * Pluck sintetico con SEIS armonicos que decaen a distinta velocidad, y con el
 * SEGUNDO deliberadamente mas fuerte que la fundamental.
 *
 * Ese detalle es el que hace util la validacion: un estimador que buscara el
 * pico espectral devolveria 2*f y pasaria igual todos los tests de seno puro.
 */
inline std::vector<float> pluck(double hz, int rate, double seconds) {
    const int n = static_cast<int>(seconds * rate);
    std::vector<float> x(static_cast<size_t>(n), 0.0f);
    for (int h = 1; h <= 6; ++h) {
        const double f = hz * h;
        if (f > rate * 0.45) break;
        const double amp = (h == 2) ? 1.0 : 0.5 / h;
        const double decay = 2.0 * h;
        for (int i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / rate;
            x[static_cast<size_t>(i)] +=
                static_cast<float>(amp * std::exp(-decay * t) * std::sin(2.0 * kPi * f * t));
        }
    }
    return x;
}

// ---------------------------------------------------------------------------
// El estimador
// ---------------------------------------------------------------------------

/**
 * Fundamental por autocorrelacion NORMALIZADA, con interpolacion parabolica.
 *
 * @param from,len ventana de analisis. Saltear el ataque importa: la excitacion
 *                 de un pluck no es periodica y arrastra la estimacion.
 * @param expectedHz centro de la banda de busqueda (`expectedHz / kBand` a
 *                 `expectedHz * kBand`).
 * @return Hz, o un valor NEGATIVO si el pico cayo en el borde del barrido — que
 *         significa "el rango estaba mal elegido", no "la señal vale eso".
 *         El llamador tiene que tratarlo como fallo, nunca como medida.
 *
 * La normalizacion no es cosmetica: sin ella los lags cortos ganan siempre, por
 * tener mas terminos en la suma.
 */
inline double fundamentalHz(const std::vector<float>& x, size_t from, size_t len,
                            int rate, double expectedHz) {
    const double loHz = expectedHz / kBand;
    const double hiHz = expectedHz * kBand;
    const int minLag = static_cast<int>(rate / hiHz);
    const int maxLag = static_cast<int>(rate / loHz);
    if (from + len > x.size()) return -1.0;
    if (static_cast<size_t>(maxLag) * 2 > len) return -2.0;

    std::vector<double> r(static_cast<size_t>(maxLag) + 2, 0.0);
    const size_t count = len - static_cast<size_t>(maxLag);
    double best = -1e18;
    for (int lag = minLag; lag <= maxLag; ++lag) {
        double num = 0.0;
        double e0 = 0.0;
        double e1 = 0.0;
        for (size_t i = 0; i < count; ++i) {
            const double a = x[from + i];
            const double b = x[from + i + static_cast<size_t>(lag)];
            num += a * b;
            e0 += a * a;
            e1 += b * b;
        }
        const double den = std::sqrt(e0 * e1);
        const double v = den > 0.0 ? num / den : 0.0;
        r[static_cast<size_t>(lag)] = v;
        best = std::max(best, v);
    }

    int bestLag = -1;
    for (int lag = minLag; lag <= maxLag; ++lag) {
        if (r[static_cast<size_t>(lag)] >= best) { bestLag = lag; break; }
    }
    if (bestLag < 0) return -3.0;
    if (bestLag <= minLag || bestLag >= maxLag) {
        // Pegado al borde. Devolver el borde seria entregar el limite del
        // barrido disfrazado de medicion — que es exactamente el modo de falla
        // que costo dos estimadores descubrir.
        return -static_cast<double>(rate) / static_cast<double>(bestLag);
    }

    const double y0 = r[static_cast<size_t>(bestLag) - 1];
    const double y1 = r[static_cast<size_t>(bestLag)];
    const double y2 = r[static_cast<size_t>(bestLag) + 1];
    const double d = y0 - 2.0 * y1 + y2;
    const double delta = (d == 0.0) ? 0.0 : 0.5 * (y0 - y2) / d;
    return rate / (static_cast<double>(bestLag) + delta);
}

/// Diferencia en cents. La unidad en la que se juzga una afinacion: 100 cents
/// son un semitono, y un oido entrenado nota 5.
inline double cents(double measuredHz, double referenceHz) {
    return 1200.0 * std::log2(measuredHz / referenceHz);
}

}  // namespace wma::pitch

#endif  // WMA_PITCH_HARNESS_H
