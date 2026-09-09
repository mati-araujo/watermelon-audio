/**
 * TemporalOracle.h — REQ-038 S2: el oraculo que mide LA MISMA CANTIDAD que la gruesa.
 *
 * POR QUE HACEN FALTA DOS ORACULOS
 * --------------------------------
 * `PartialOracle.h` mide el pico ESPECTRAL de H1 con Goertzel. Es el oraculo correcto
 * para la lectura FINA, que es espectral: el strobe ajusta una serie estirada sobre las
 * frecuencias de los parciales.
 *
 * La deteccion GRUESA no mide eso. `McLeodPitch` es TEMPORAL: busca el mejor PERIODO de
 * la forma de onda por NSDF. Sobre una serie armonica exacta las dos cantidades coinciden
 * y la distincion no importa. Sobre una cuerda REAL no:
 *
 *   f_n = n · f0 · sqrt(1 + B·n²)
 *
 * los parciales altos no caen en `n·f0`, asi que **la señal no tiene un periodo exacto**.
 * "La altura" deja de ser un solo numero: el pico espectral de H1 dice una cosa y el mejor
 * periodo de la forma de onda dice otra, y NINGUNA de las dos esta mal.
 *
 * 🔴 MEDIDO EN REQ-038 S1 (2026-09-09), y es lo que parte ese REQ. Sobre la ventana
 * sostenida de `guitarra-acero_E2`:
 *
 *   | metodo                          | que mide                    | resultado |
 *   |---------------------------------|-----------------------------|-----------|
 *   | detector (NSDF)                 | periodo de la forma de onda | +6,32 c   |
 *   | autocorrelacion cruda (esto)    | periodo de la forma de onda | +6,71 c   |
 *   | oraculo espectral (Goertzel H1) | pico de H1                  |  0,00 c   |
 *
 * Dos metodos temporales INDEPENDIENTES coinciden entre si (r = 0,9875) y se apartan del
 * espectral. El detector no calculaba mal: se lo estaba comparando contra la cantidad
 * equivocada. Sobre el control `guitarra-jazz_E2` los tres coinciden (−0,09 / −0,20 / 0,00)
 * y la NSDF del pico vale 1,000 contra 0,986 en acero.
 *
 * POR QUE AUTOCORRELACION CRUDA Y NO NSDF
 * ---------------------------------------
 * 🔴 Un oraculo tiene que ser OTRO METODO que el sistema bajo prueba. Si esto usara NSDF
 * seria una copia fiel del detector, y una copia fiel es INVISIBLE para todo instrumento:
 * coincidiria siempre, incluido el dia que las dos esten mal por la misma razon. La
 * autocorrelacion normalizada cruda mide la misma CANTIDAD (el periodo) por otro CAMINO
 * (correlacion directa, sin la normalizacion de McLeod, sin decimacion, sin barrido de
 * candidatos ni refinamiento).
 *
 * LO QUE ESTE ORACULO NO PUEDE
 * ----------------------------
 * Necesita un nominal para saber donde buscar (±3 %, ~±52 cents). No es un detector: es un
 * REFINADOR de un valor que ya se conoce. Si el nominal esta a mas de medio semitono, esto
 * devuelve el periodo equivocado con `r` alto y no avisa — por eso el llamador le pasa el
 * `hz_verdadero` del manifiesto, que es dato del corpus y no del motor.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace wma_test::oracle {

/**
 * El paso de la grilla de lags, en muestras. Es PUBLICO porque fija la RESOLUCION del
 * oraculo: `1200·log2(1 + kLagStepSamples/lagIdeal)` cents. A 82,41 Hz y 44,1 kHz
 * (lagIdeal ≈ 535) son **0,16 c**, y un test que compare contra este oraculo tiene que
 * derivar su tolerancia de acá en vez de escribir una constante — que fue lo que fallo la
 * primera vez que se escribio este self-test.
 */
inline constexpr double kLagStepSamples = 0.05;

/// El paso del PRIMER barrido, antes del refinamiento. No afecta la resolucion: solo el costo.
inline constexpr double kCoarseLagStepSamples = 0.5;

/// La resolucion del oraculo en cents, para el nominal dado.
inline double resolutionCents(int sampleRate, double nominalHz) {
    if (sampleRate <= 0 || nominalHz <= 0.0) return 0.0;
    const double lagIdeal = sampleRate / nominalHz;
    return 1200.0 * std::log2(1.0 + kLagStepSamples / lagIdeal);
}

/// Lo que devuelve `measurePeriod`. `valid` en false = no habia señal suficiente.
struct TemporalReading {
    bool valid = false;
    double hz = 0.0;          ///< 1 / periodo, en Hz
    double lagSamples = 0.0;  ///< el periodo en muestras (fraccionario)
    double r = 0.0;           ///< la correlacion normalizada en ese lag: [0, 1]
};

/**
 * El periodo de la forma de onda por autocorrelacion normalizada, sobre una ventana que
 * arranca en `fromSec`.
 *
 * @param mono        señal mono (el llamador ya mezclo los canales)
 * @param sampleRate  Hz
 * @param nominalHz   donde buscar. Se barre `±spanFraction` alrededor de `sampleRate/nominalHz`
 * @param fromSec     desde donde mirar. Por default 1,5 s: despues del ataque, que es
 *                    transitorio y no tiene un periodo estable (REQ-036 lo midio: el ataque
 *                    de un bajo acustico arranca 27,7 cents grave y se asienta recien a 1 s)
 * @param windowLog2  TECHO del largo de la ventana, en potencias de dos. La ventana real es
 *                    todo lo que quede de nota desde `fromSec`, acotado por esto (18 = 5,9 s
 *                    a 44,1 kHz, o sea: toda la nota del corpus)
 */
inline TemporalReading measurePeriod(const std::vector<float>& mono, int sampleRate,
                                     double nominalHz, double fromSec = 1.5,
                                     int windowLog2 = 18, double spanFraction = 0.03) {
    TemporalReading out;
    if (sampleRate <= 0 || nominalHz <= 0.0 || mono.empty()) return out;

    const long long from = static_cast<long long>(fromSec * sampleRate);
    const double lagIdeal = sampleRate / nominalHz;
    /**
     * 🔴 LA VENTANA CUBRE EL TRAMO SOSTENIDO ENTERO, no un pedazo de su comienzo, y eso NO es
     * un detalle de precision: es lo que hace que este oraculo describa LA MISMA COSA que la
     * mediana de la gruesa con la que se lo compara.
     *
     * El periodo temporal DERIVA a lo largo de la nota, porque los parciales altos —que son
     * los que lo apartan de H1 en una cuerda inarmonica— decaen mas rapido que el
     * fundamental. Medido (2026-09-09, `guitarra-acero_D3`): una ventana de 372 ms desde
     * 1,0 s da −3,01 c contra la mediana de la gruesa, y desde 1,5 s da −0,99. Los dos
     * numeros son correctos y describen tramos distintos; el comparable con una MEDIANA
     * SOBRE TODA LA NOTA es el que promedia toda la nota.
     */
    long long N = 1LL << windowLog2;
    // La ventana de correlacion necesita `N` muestras mas el lag mas alto que se prueba, mas
    // una para la interpolacion lineal del lag fraccionario.
    const long long need = static_cast<long long>(lagIdeal * (1.0 + spanFraction)) + 2;
    if (from < 0) return out;

    /**
     * 🔴 SI NO ENTRA, SE ACHICA LA VENTANA — NUNCA SE ADELANTA `fromSec`.
     *
     * Las dos salidas parecen equivalentes y no lo son. Achicar la ventana mide LA MISMA
     * cantidad con menos muestras (peor relacion señal-ruido, mismo periodo). Arrancar antes
     * mide OTRA cantidad: el ataque de una nota pulsada es un transitorio de afinacion —
     * REQ-036 lo midio, un bajo acustico arranca 27,7 cents grave y se asienta recien a 1 s—
     * asi que una ventana que lo incluye promedia la altura del ataque con la de la nota.
     *
     * El piso NO es un numero de muestras sino de PERIODOS: 2048 muestras son ~20 ciclos a
     * 440 Hz y solo ~2 a 41 Hz, y con dos ciclos una autocorrelacion no dice nada. Se piden
     * `kMinPeriods` ciclos, que es lo que hace que el piso valga igual para toda la tesitura
     * del corpus (E1 a G4).
     */
    constexpr int kMinPeriods = 8;
    const long long floorN =
        std::max<long long>(2048, static_cast<long long>(kMinPeriods * lagIdeal) + 1);
    const long long disponible = static_cast<long long>(mono.size()) - from - need;
    if (disponible < floorN) return out;   // no hay nota sostenida: se dice, no se inventa
    N = std::min(N, disponible);

    /**
     * LA BUSQUEDA, EN DOS PASOS. Un barrido unico al paso fino cuesta ~1,2·lagIdeal
     * evaluaciones (642 a 82 Hz), y con una ventana que cubre la nota entera eso pone al
     * barrido del corpus en decenas de segundos. Grueso a 0,5 muestras y despues fino
     * alrededor del mejor cuesta ~94 y da el MISMO lag: la correlacion es suave y tiene un
     * solo maximo dentro de ±3 %, que es media nota. Lo verifica el self-test, que fija el
     * resultado contra la fisica (monotono en B, techo en el parcial mas agudo).
     */
    auto correlate = [&](double lag) {
        const long long L = static_cast<long long>(lag);
        const double frac = lag - static_cast<double>(L);
        double num = 0.0, e1 = 0.0, e2 = 0.0;
        for (long long i = 0; i < N; ++i) {
            const double a = mono[static_cast<size_t>(from + i)];
            const double b0 = mono[static_cast<size_t>(from + i + L)];
            const double b1 = mono[static_cast<size_t>(from + i + L + 1)];
            const double b = b0 + frac * (b1 - b0);  // el lag es fraccionario: se interpola
            num += a * b;
            e1 += a * a;
            e2 += b * b;
        }
        return num / std::sqrt(e1 * e2 + 1e-30);
    };

    const double lo = lagIdeal * (1.0 - spanFraction);
    const double hi = lagIdeal * (1.0 + spanFraction);
    double best = -1.0, bestLag = 0.0;
    for (double lag = lo; lag <= hi; lag += kCoarseLagStepSamples) {
        const double r = correlate(lag);
        if (r > best) { best = r; bestLag = lag; }
    }
    if (bestLag <= 0.0) return out;
    // Y el refinamiento, al paso que fija la resolucion declarada. Se abre 1,5 pasos gruesos
    // a cada lado para que el maximo fino no pueda quedar afuera del intervalo.
    const double refLo = std::max(lo, bestLag - 1.5 * kCoarseLagStepSamples);
    const double refHi = std::min(hi, bestLag + 1.5 * kCoarseLagStepSamples);
    for (double lag = refLo; lag <= refHi; lag += kLagStepSamples) {
        const double r = correlate(lag);
        if (r > best) { best = r; bestLag = lag; }
    }

    if (bestLag <= 0.0) return out;

    out.valid = true;
    out.lagSamples = bestLag;
    out.hz = sampleRate / bestLag;
    out.r = best;
    return out;
}

/// La misma medicion, sobre estereo intercalado como lo devuelve `wav::readWav`.
/// Mezcla `0,5·(L+R)`, que es la misma mezcla que ve el analisis (ver `CorpusSweep.h`).
inline TemporalReading measurePeriodInterleaved(const std::vector<float>& stereo, int channels,
                                                int sampleRate, double nominalHz,
                                                double fromSec = 1.5) {
    if (channels <= 0) return {};
    std::vector<float> mono;
    mono.reserve(stereo.size() / static_cast<size_t>(channels));
    for (size_t i = 0; i + static_cast<size_t>(channels) <= stereo.size();
         i += static_cast<size_t>(channels)) {
        double s = 0.0;
        for (int c = 0; c < channels; ++c) s += stereo[i + static_cast<size_t>(c)];
        mono.push_back(static_cast<float>(channels == 2 ? 0.5 * s : s / channels));
    }
    return measurePeriod(mono, sampleRate, nominalHz, fromSec);
}

}  // namespace wma_test::oracle
