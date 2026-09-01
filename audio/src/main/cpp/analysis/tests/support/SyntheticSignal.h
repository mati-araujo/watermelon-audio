#pragma once

/**
 * SyntheticSignal.h — el corpus sintetico de REQ-001 S2 (tarea 2.9).
 *
 * POR QUE SINTETICO Y NO GRABACIONES
 * ----------------------------------
 * Porque acá hace falta conocer **f0 exacto por construccion**. Una grabacion de
 * una cuerda real tiene el pitch que tiene, y medirlo requeriria... el estimador
 * que se esta probando. Con señal sintetica el numero verdadero se conoce con la
 * precision del `double`, y el error medido es error del estimador y de nada mas.
 *
 * El corpus real (grabaciones, robustez) es de S10 y baja por
 * `scripts/fetch-corpus.sh`. Este archivo NO lo reemplaza: mide exactitud, no
 * robustez.
 *
 * LA FASE SE ACUMULA EN `double` Y ESO NO ES DETALLE
 * --------------------------------------------------
 * A 48 kHz, 3 s son 144 000 muestras. Acumular la fase en `float` mete un error
 * de redondeo que, sobre esa cantidad de pasos, es del orden del efecto que se
 * quiere medir: el generador estaria fabricando el desafinado que el estimador
 * despues "encuentra". El generador tiene que ser MAS exacto que lo que mide, no
 * igual de exacto.
 */

#include <cmath>
#include <cstdint>
#include <vector>

namespace wma_test {

/// Cuantos cents hay de `f` a `reference`. Positivo = `f` por ENCIMA, que es la
/// convencion del afinador (ver PhaseSlopeEstimator.h).
inline double centsBetween(double f, double reference) {
    return 1200.0 * std::log2(f / reference);
}

/// La frecuencia que esta a `cents` de `reference`, con el mismo signo.
inline double detune(double reference, double cents) {
    return reference * std::pow(2.0, cents / 1200.0);
}

/**
 * Seno puro de `hz`, amplitud `amp`, `numFrames` a `sampleRate`.
 *
 * La fase arranca en `phase0` y se puede pedir distinta de cero a proposito: un
 * estimador que dependiera de la fase inicial daria resultados distintos segun
 * donde el usuario empezo a tocar, y eso hay que poder provocarlo en un test.
 */
inline std::vector<float> pureSine(double hz, int sampleRate, int numFrames,
                                   double amp = 0.5, double phase0 = 0.0) {
    std::vector<float> out(static_cast<size_t>(numFrames));
    const double dp = 2.0 * M_PI * hz / static_cast<double>(sampleRate);
    double p = phase0;
    for (int i = 0; i < numFrames; ++i) {
        out[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(p));
        p += dp;
        if (p >= 2.0 * M_PI) p -= 2.0 * M_PI;   // acota el error de acumulacion
    }
    return out;
}

/**
 * Cuerda con parciales INARMONICOS: `f_n = n·f0·sqrt(1 + B·n²)`.
 *
 * `B` es el coeficiente de inarmonicidad real de una cuerda con rigidez. Valores
 * tipicos: 1e-5 para una prima de guitarra, 5e-4 para una bordona gruesa. Con
 * `B = 0` degenera en armonicos exactos, que tambien sirve como caso de control.
 *
 * Las amplitudes decaen como 1/n, que es lo suficientemente parecido a una
 * cuerda pulsada para que el test signifique algo, y lo suficientemente simple
 * para que el numero verdadero siga siendo exacto.
 */
inline std::vector<float> partialsWithAmplitudes(double f0, double B,
                                                 const std::vector<double>& amps,
                                                 int sampleRate, int numFrames,
                                                 double phase0 = 0.0) {
    std::vector<float> out(static_cast<size_t>(numFrames), 0.0f);
    for (size_t k = 0; k < amps.size(); ++k) {
        const int n = static_cast<int>(k) + 1;
        const double fn = n * f0 * std::sqrt(1.0 + B * n * n);
        if (fn >= 0.5 * sampleRate) break;               // nada por encima de Nyquist
        const double a = amps[k];
        if (a == 0.0) continue;
        const double dp = 2.0 * M_PI * fn / static_cast<double>(sampleRate);
        // El parcial n arranca en n·phase0: es lo que produce CORRER EL ORIGEN DE
        // TIEMPO de la señal entera, que es la eleccion arbitraria de quien graba.
        // REQ-027 se hizo visible sobre este eje, asi que el generador lo tiene.
        double p = phase0 * static_cast<double>(n);
        for (int i = 0; i < numFrames; ++i) {
            out[static_cast<size_t>(i)] += static_cast<float>(a * std::sin(p));
            p += dp;
            if (p >= 2.0 * M_PI) p -= 2.0 * M_PI;
        }
    }
    return out;
}

/**
 * El caso que justifica rastrear armonicos (REQ-001 S6 · 6.3): el fundamental
 * `dB` por debajo del SEGUNDO parcial, que es lo que pasa en una bordona grave.
 * Los parciales 2..n mantienen el decaimiento 1/n; solo el fundamental se hunde.
 */
inline std::vector<float> stringWithWeakFundamental(double f0, double B, int numPartials,
                                                    int sampleRate, int numFrames,
                                                    double dbBelowSecond, double amp = 0.5) {
    std::vector<double> amps;
    amps.reserve(static_cast<size_t>(numPartials));
    for (int n = 1; n <= numPartials; ++n) amps.push_back(amp / n);
    // El 2do parcial vale amp/2; el fundamental queda `dbBelowSecond` por debajo DE EL.
    amps[0] = (amp / 2.0) * std::pow(10.0, -dbBelowSecond / 20.0);
    return partialsWithAmplitudes(f0, B, amps, sampleRate, numFrames);
}

/**
 * Cuerda con parciales INARMONICOS, amplitudes 1/n. DELEGA en
 * `partialsWithAmplitudes` — dos generadores de la misma señal serian dos fuentes
 * de verdad, y la que se usa menos es la que se desincroniza.
 */
inline std::vector<float> inharmonicString(double f0, double B, int numPartials,
                                           int sampleRate, int numFrames,
                                           double amp = 0.5, double phase0 = 0.0) {
    std::vector<double> amps;
    amps.reserve(static_cast<size_t>(numPartials));
    for (int n = 1; n <= numPartials; ++n) amps.push_back(amp / n);
    return partialsWithAmplitudes(f0, B, amps, sampleRate, numFrames, phase0);
}

/**
 * REQ-027 S3 — cuerda que DECAE con los agudos apagandose ANTES (`tau_n = tau/n`).
 *
 * 🔴 `applyDecay` NO sirve para esto, y la diferencia es el test entero: aplica el
 * mismo decaimiento a toda la señal, asi que las amplitudes RELATIVAS de los
 * parciales no cambian nunca. Una cuerda real no hace eso — los parciales altos
 * se apagan primero — y por lo tanto `applyDecay` no puede ejercer el caso donde
 * el piso de energia de REQ-027 descartaria un parcial legitimo por debil.
 *
 * Es el INSTRUMENTO del criterio de muerte de REQ-027.
 */
inline std::vector<float> decayingString(double f0, double B, int numPartials,
                                         int sampleRate, int numFrames,
                                         double tauFundamental, double amp = 0.5) {
    std::vector<float> out(static_cast<size_t>(numFrames), 0.0f);
    for (int n = 1; n <= numPartials; ++n) {
        const double fn = n * f0 * std::sqrt(1.0 + B * n * n);
        if (fn >= 0.5 * sampleRate) break;
        const double tau = tauFundamental / static_cast<double>(n);
        const double dp = 2.0 * M_PI * fn / static_cast<double>(sampleRate);
        double ph = 0.0;
        for (int i = 0; i < numFrames; ++i) {
            const double t = static_cast<double>(i) / sampleRate;
            out[static_cast<size_t>(i)] +=
                static_cast<float>((amp / n) * std::exp(-t / tau) * std::sin(ph));
            ph += dp;
            if (ph >= 2.0 * M_PI) ph -= 2.0 * M_PI;
        }
    }
    return out;
}

/// Una cuerda del catalogo de instrumentos de S3.
struct CatalogString { const char* name; double hz; };

/**
 * Las 14 cuerdas del catalogo, en afinacion estandar. Valores de TABLA PUBLICADA,
 * no calculados con la formula de la implementacion: un test que computa lo
 * esperado con el mismo codigo que prueba, prueba que el codigo es igual a si
 * mismo.
 *
 * Vive ACA y no dentro de un test porque la usan varios (REQ-027 S3). Una tabla
 * duplicada entre archivos es una que se desincroniza.
 */
inline const std::vector<CatalogString>& catalogStrings() {
    static const std::vector<CatalogString> kStrings = {
        // Guitarra estandar
        {"guitarra E2", 82.407}, {"guitarra A2", 110.000}, {"guitarra D3", 146.832},
        {"guitarra G3", 195.998}, {"guitarra B3", 246.942}, {"guitarra E4", 329.628},
        // Bajo de 5 cuerdas — el B0 es el caso que justifica todo el diseño
        {"bajo B0", 30.868}, {"bajo E1", 41.203}, {"bajo A1", 55.000},
        {"bajo D2", 73.416}, {"bajo G2", 97.999},
        // Ukelele (reentrante) — la cuerda mas aguda del catalogo
        {"ukelele G4", 391.995}, {"ukelele C4", 261.626}, {"ukelele A4", 440.000},
    };
    return kStrings;
}

/// Decaimiento exponencial en el lugar, con `tau` en segundos. Modela que una
/// nota real se apaga — y por lo tanto que el gate de nivel se va a cruzar.
inline void applyDecay(std::vector<float>& sig, int sampleRate, double tau) {
    for (size_t i = 0; i < sig.size(); ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        sig[i] = static_cast<float>(sig[i] * std::exp(-t / tau));
    }
}

/**
 * Ruido blanco sumado a un SNR dado, en dB.
 *
 * El generador es un LCG propio y **no** `std::mt19937` con `random_device`: el
 * corpus tiene que ser REPRODUCIBLE bit a bit entre corridas y entre maquinas,
 * porque de él salen los golden. Este repo ya se comio un efecto que sorteaba su
 * LFO desde `random_device` y por eso no puede tener golden.
 */
inline void addNoiseAtSnr(std::vector<float>& sig, double snrDb, uint32_t seed = 12345u) {
    double sumSq = 0.0;
    for (float v : sig) sumSq += static_cast<double>(v) * v;
    const double sigRms = std::sqrt(sumSq / static_cast<double>(sig.size()));
    const double noiseRms = sigRms / std::pow(10.0, snrDb / 20.0);

    uint32_t state = seed;
    for (float& v : sig) {
        state = state * 1664525u + 1013904223u;
        // [-1, 1) uniforme; su RMS es 1/sqrt(3), de ahi el factor.
        const double u = (static_cast<double>(state) / 2147483648.0) - 1.0;
        v = static_cast<float>(v + noiseRms * std::sqrt(3.0) * u);
    }
}

}  // namespace wma_test
