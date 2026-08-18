/**
 * WD-3.3 — la cadena de efectos no aplica ganancia propia.
 *
 * QUE SE ARREGLA
 * --------------
 * `EffectChain::processOneEffect` corria un auto-gain por efecto y por bloque:
 * escaneaba el pico del buffer y, si pasaba 1,5, multiplicaba TODO el bloque por
 * `1,5 / pico`. Sin ataque, sin release, sin suavizado en el borde — o sea un
 * limiter por bloque con ataque y release infinitos, que es la definicion del
 * artefacto que pretendia evitar.
 *
 * Como la ganancia es constante dentro del bloque y distinta en el siguiente, el
 * borde entre bloques queda con un ESCALON. Medido con un transitorio que
 * decae de 3,0 (bloques de 256 @ 48 kHz), las ganancias por bloque salieron
 * 0,521 → 0,616 → 0,727 → 0,859 → 1,0: no es "un click", son CUATRO, uno por
 * bloque mientras el transitorio esta arriba del techo. En el peor borde el
 * salto midio 0,231 contra 0,038 de la pendiente maxima de la propia onda: 6x
 * mas empinado que cualquier cosa que la senal haga por si misma.
 *
 * POR QUE ES ALCANZABLE, Y NO CON PERILLAS RARAS
 * ---------------------------------------------
 * El disparador dominante NO son los parametros: con una nota sola a 0,9 y
 * valores de fabrica, NINGUNO de los 23 efectos llega al techo. Es el NIVEL DE
 * ENTRADA de la cadena, que recibe el bus del sinte sumado. `VoicePool` reparte
 * headroom con `1/sqrt(n)`, asi que un acorde de n voces llega a `sqrt(n)` veces
 * la amplitud por voz — 4 voces son 2,0 y 8 son 2,83. En ese rango 20 de los 23
 * efectos disparaban el auto-gain en 14 a 37 de 40 bloques. O sea: no era un
 * artefacto de transitorio, era un escalon por bloque durante todo un pasaje
 * fuerte.
 *
 * DONDE VA LA PROTECCION
 * ----------------------
 * En `OutputStage`, UNA vez, al final: `LookaheadLimiter` (240 samples de
 * lookahead, con ataque y release) → `SoftClipper` → dither → hard limit. Un
 * limiter por efecto es el lugar equivocado para control de nivel.
 *
 * QUE NO SE SACO
 * --------------
 * El saneo de NaN/Inf, que sigue siendo la unica pasada extra del helper e
 * incrementa `mNonFiniteBlocks` (WD-1.1/WD-5.1). Y la proteccion del modo
 * FEEDBACK, que es propia de ese modo y no dependia del auto-gain: el buffer de
 * realimentacion pasa por `tanh` (acotado a ±1 por construccion) y tiene su
 * propio detector de energia desbocada.
 */

#include "../EffectChain.h"
#include "../EffectTypes.h"
#include "EffectCatalog.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

using wma::catalog::nameOf;

constexpr int kSampleRate = 48000;
constexpr int kBlock = 256;      // 5,33 ms @ 48 kHz
constexpr int kBlocks = 40;

/// El techo que aplicaba el auto-gain. No se usa como umbral de nada: se usa
/// para AFIRMAR que el montaje entra en el regimen donde el defecto vivia.
constexpr float kOldCeiling = 1.5f;

/**
 * Amplitudes de prueba, elegidas NO representables en binario A PROPOSITO.
 *
 * Con 0,5 / 2,0 / 4,0 un test de exactitud numerica es ciego por construccion:
 * las potencias de dos sobreviven redondeos que cualquier otro valor no
 * sobrevive. Ver la nota de WD-3.4 sobre el ulp que `0,5` escondio en
 * `ParameterSmoother`.
 */
constexpr float kLoudAmp = 2.6f;   // pico bien arriba del viejo techo
constexpr float kSoftAmp = 0.42f;  // pico bien abajo
constexpr float kScale   = 4.1f;   // factor de homogeneidad

/// Vecindario, en frames a cada lado, contra el que se compara la delta del
/// borde. +-8 frames a 48 kHz son 0,17 ms: mucho mas corto que cualquier cosa
/// que la onda haga, y mucho mas largo que un escalon de una muestra.
constexpr int kNeighbourhood = 8;

/// Corte entre las dos poblaciones medidas. Ver la nota del test.
constexpr double kBoundaryRatioLimit = 1.7;

const EffectType kAllEffects[] = {
    FILTER, REVERB, DELAY, VOCODER, DISTORTION, COMPRESSOR, CHORUS, PHASER,
    AMP_SIM, CABINET, DECIMATOR, DECI_HPF, AUTO_PAN, COMPLEX_TREM, RANDOM_RESO,
    HPF_DELAY, TAPE_ECHO, HALL_REVERB, RISER_REVERB, BEAT_GRAIN, SPRING_REVERB,
    PLATE_REVERB, SHIMMER_REVERB,
};

/// Nota de 220 Hz con ataque y decaimiento: lo que toca un usuario, no ruido
/// blanco. El ataque importa — es el transitorio el que cruza el techo.
std::vector<float> pluckedNote(float amp, int frames) {
    std::vector<float> b(static_cast<size_t>(frames) * 2);
    for (int n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / kSampleRate;
        const double env = amp * std::exp(-t / 0.25) * (1.0 - std::exp(-t / 0.002));
        const float s = static_cast<float>(env * std::sin(2.0 * M_PI * 220.0 * t));
        b[2 * n] = s;
        b[2 * n + 1] = s;
    }
    return b;
}

std::vector<float> runChain(EffectChain& chain, const std::vector<float>& in, int frames) {
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    for (int b = 0; b * kBlock < frames; ++b) {
        const int n = std::min(kBlock, frames - b * kBlock);
        chain.process(const_cast<float*>(in.data()) + static_cast<size_t>(b) * kBlock * 2,
                      out.data() + static_cast<size_t>(b) * kBlock * 2, n);
    }
    return out;
}

float peakOf(const std::vector<float>& b) {
    float p = 0.0f;
    for (float v : b) p = std::fmax(p, std::fabs(v));
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// Criterio 1 — `processOneEffect` no normaliza.
// ---------------------------------------------------------------------------

/**
 * La forma EXACTA de la propiedad: con un efecto configurado en identidad, la
 * cadena tiene que devolver la entrada bit a bit, por fuerte que venga.
 *
 * El instrumento es un DELAY con `wet = 0` y `feedback = 0`. Ahi
 * `DelayEffect::process` calcula `1,0f * input + 0,0f * delayed`, que es
 * `input` exacto en float — no aproximado. No se usa un efecto bypasseado:
 * `processOneEffect` sale temprano por el camino de bypass y NUNCA llegaria al
 * codigo que se esta midiendo.
 *
 * Antes de WD-3.3 esto fallaba con la salida clavada en 1,5.
 */
TEST(ChainGain, TheChainHandsBackExactlyWhatTheEffectProduced) {
    EffectChain chain;
    chain.setSampleRate(kSampleRate);
    ASSERT_TRUE(chain.addEffect(DELAY));
    chain.setParameter(0, 2, 0.0f);  // wet = 0  -> camino seco puro
    chain.setParameter(0, 1, 0.0f);  // feedback = 0

    const int frames = kBlock * kBlocks;
    const std::vector<float> in = pluckedNote(kLoudAmp, frames);

    // El montaje TIENE que entrar en el regimen del defecto. Sin esto, el test
    // podria pasar por no haber armado nunca la condicion que mide.
    ASSERT_GT(peakOf(in), kOldCeiling)
        << "el montaje no llega al regimen donde el auto-gain actuaba";

    const std::vector<float> out = runChain(chain, in, frames);

    for (size_t i = 0; i < in.size(); ++i) {
        ASSERT_EQ(in[i], out[i])
            << "la cadena reescalo la muestra " << i << " (frame " << i / 2 << "): "
            << in[i] << " -> " << out[i];
    }
}

/**
 * La misma propiedad pedida de otra forma, y sobre un efecto de verdad:
 * escalar la entrada por k tiene que escalar la salida por k. Un biquad es
 * lineal, asi que la homogeneidad es una propiedad del EFECTO; lo unico que
 * podia romperla era una ganancia dependiente del nivel puesta por la cadena.
 *
 * Los dos casos caen a proposito uno de cada lado del viejo techo.
 */
TEST(ChainGain, ScalingTheInputScalesTheOutputAcrossTheOldCeiling) {
    const int frames = kBlock * kBlocks;
    const std::vector<float> soft = pluckedNote(kSoftAmp, frames);
    std::vector<float> loud(soft.size());
    for (size_t i = 0; i < soft.size(); ++i) loud[i] = soft[i] * kScale;

    ASSERT_LT(peakOf(soft), kOldCeiling) << "el caso suave tiene que quedar bajo el techo";
    ASSERT_GT(peakOf(loud), kOldCeiling) << "el caso fuerte tiene que pasarlo";

    EffectChain a, b;
    a.setSampleRate(kSampleRate);
    b.setSampleRate(kSampleRate);
    ASSERT_TRUE(a.addEffect(FILTER));
    ASSERT_TRUE(b.addEffect(FILTER));

    const std::vector<float> outSoft = runChain(a, soft, frames);
    const std::vector<float> outLoud = runChain(b, loud, frames);

    const float ref = peakOf(outLoud);
    ASSERT_GT(ref, 0.0f);
    double worst = 0.0;
    for (size_t i = 0; i < outSoft.size(); ++i)
        worst = std::fmax(worst, std::fabs(outLoud[i] - outSoft[i] * kScale));

    // Tolerancia RELATIVA al pico, no absoluta: lo que se afirma es que no hay
    // ganancia extra, no que dos caminos de float den identico.
    EXPECT_LT(worst, ref * 1e-5) << "peor desvio " << worst << " sobre un pico de " << ref;
}

// ---------------------------------------------------------------------------
// El sintoma: el escalon en el borde de bloque. Pedido a LOS 23.
// ---------------------------------------------------------------------------

/**
 * Un borde de bloque no es un evento fisico: la senal no sabe donde el motor
 * corta el buffer. Asi que la onda no puede ser mas empinada ahi que en sus
 * muestras VECINAS. Eso es exactamente lo que un escalon de ganancia produce, y
 * es lo que se escucha como click.
 *
 * Se le pide a los 23 efectos registrados, no a los que descubrieron el
 * defecto: estar afuera del barrido es estar afuera del test.
 *
 * POR QUE LA COMPARACION ES LOCAL, Y NO CONTRA EL MAXIMO DE LA CORRIDA
 * -------------------------------------------------------------------
 * La primera version de este test comparaba la delta del borde contra el
 * maximo de todas las deltas interiores. Tenia DOS problemas, y los dos son del
 * instrumento, no del codigo:
 *
 *  1. Comparaba una sola delta contra el maximo de las otras 255. Superarlo por
 *     un 2 % no dice nada: es la chance de que el punto mas empinado de la onda
 *     caiga justo en el borde.
 *  2. Peor: tomaba el borde en L **y** R y el interior solo en L. Con AUTO_PAN,
 *     que separa los canales a proposito, eso solo daba 1,17 — un "hallazgo"
 *     que era enteramente mio.
 *
 * La medida local no tiene ninguno de los dos: compara la delta del borde
 * contra las deltas de sus ±8 frames vecinos, en el mismo canal. Y el test
 * lleva su propio CONTROL: la misma medida aplicada a una muestra INTERIOR
 * cualquiera. Si el instrumento estuviera sesgado, el control se movaria; medido,
 * queda en ~1,00 con y sin el defecto.
 *
 * EL CORTE NO ES UNA TOLERANCIA ELEGIDA
 * -------------------------------------
 * Medido sobre los 23, las dos poblaciones se separan solas y no hay nada en el
 * medio: con el auto-gain, 14 efectos entre **2,60 y 16,1** (el peor, VOCODER);
 * sin el, el maximo de los 23 es **1,011** (PHASER). El corte va en 1,7.
 */
TEST(ChainGain, NoBlockBoundaryIsSteeperThanItsOwnNeighbourhood) {
    const int frames = kBlock * kBlocks;
    const std::vector<float> in = pluckedNote(kLoudAmp, frames);
    ASSERT_GT(peakOf(in), kOldCeiling);

    int armed = 0;      // efectos cuya salida pasa el viejo techo
    int measured = 0;
    std::string worstName;
    double worstRatio = 0.0, worstControl = 0.0;

    for (EffectType t : kAllEffects) {
        EffectChain chain;
        chain.setSampleRate(kSampleRate);
        ASSERT_TRUE(chain.addEffect(t)) << nameOf(t);
        const std::vector<float> out = runChain(chain, in, frames);

        if (peakOf(out) > kOldCeiling) ++armed;

        // Razon de la delta en `k` contra la mayor delta de sus vecinas, en el
        // MISMO canal. Devuelve -1 si el vecindario esta mudo (no medible).
        auto neighbourRatio = [&out](size_t k) -> double {
            const double d = std::fabs(out[k] - out[k - 2]);
            double m = 0.0;
            for (int j = -kNeighbourhood; j <= kNeighbourhood; ++j) {
                if (j == 0) continue;
                const size_t kk = k + static_cast<size_t>(j) * 2;
                if (kk < 2 || kk >= out.size()) continue;
                m = std::fmax(m, std::fabs(out[kk] - out[kk - 2]));
            }
            return (m > 1e-9) ? d / m : -1.0;
        };

        double ratio = 0.0, control = 0.0;
        bool any = false;
        for (int b = 1; b < kBlocks; ++b) {
            const size_t k = static_cast<size_t>(b) * kBlock * 2;
            for (int ch = 0; ch < 2; ++ch) {
                const double r = neighbourRatio(k + static_cast<size_t>(ch));
                // CONTROL: la misma medida sobre una muestra que NO es borde.
                const double c = neighbourRatio(k + static_cast<size_t>(kBlock / 3) * 2
                                                  + static_cast<size_t>(ch));
                if (r >= 0.0) { ratio = std::fmax(ratio, r); any = true; }
                if (c >= 0.0) control = std::fmax(control, c);
            }
        }
        if (!any) continue;   // efecto mudo con esta entrada
        ++measured;
        if (ratio > worstRatio) { worstRatio = ratio; worstName = nameOf(t); }
        worstControl = std::fmax(worstControl, control);

        EXPECT_LE(ratio, kBoundaryRatioLimit)
            << nameOf(t) << ": la delta en un borde de bloque es " << ratio
            << "x la mayor de sus vecinas; el control interior dio " << control;
    }

    // El CONTROL valida el instrumento: si la misma medida aplicada adentro del
    // bloque ya diera alto, el test estaria midiendo su propio sesgo.
    EXPECT_LE(worstControl, kBoundaryRatioLimit)
        << "el instrumento acusa a una muestra INTERIOR (" << worstControl
        << "): la medida esta sesgada, no hay hallazgo";

    // Un dominio vacio da verde por AUSENCIA, no por correccion (WD-3.4).
    ASSERT_GE(measured, 20) << "el barrido midio muy pocos efectos";
    ASSERT_GE(armed, 10)
        << "ningun efecto supera el viejo techo: el montaje ya no ejercita "
           "el regimen donde el auto-gain actuaba";
    RecordProperty("worst_boundary_ratio", std::to_string(worstRatio));
    RecordProperty("worst_effect", worstName);
    RecordProperty("worst_interior_control", std::to_string(worstControl));
}

// ---------------------------------------------------------------------------
// Lo que NO se saca: la proteccion propia del modo FEEDBACK.
// ---------------------------------------------------------------------------

/**
 * El auto-gain era, de hecho, una cota dentro del lazo del modo FEEDBACK. Sacarlo
 * podria haber abierto ese lazo — pero no lo hace, y la razon esta en el codigo,
 * no en el auto-gain: `processFeedback` pasa el buffer de realimentacion por
 * `tanh` (acotado a ±1 por construccion, para cualquier entrada) antes de
 * volver a mezclarlo, con `feedbackAmount` acotado a 0,95.
 *
 * Se mide con el peor caso disponible: realimentacion al maximo, entrada fuerte
 * sostenida, y bastante mas tiempo que un transitorio.
 */
TEST(ChainGain, TheFeedbackRoutingStaysBoundedWithoutTheAutoGain) {
    EffectChain chain;
    chain.setSampleRate(kSampleRate);
    ASSERT_TRUE(chain.addEffect(DELAY));
    ASSERT_TRUE(chain.addEffect(HALL_REVERB));
    chain.setRoutingMode(RoutingMode::FEEDBACK);
    chain.setFeedbackAmount(1.0f);   // se acota solo a 0,95

    const int frames = kBlock * 400;   // ~2,1 s
    std::vector<float> in(static_cast<size_t>(frames) * 2);
    for (int n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / kSampleRate;
        const float s = static_cast<float>(kLoudAmp * std::sin(2.0 * M_PI * 220.0 * t));
        in[2 * n] = in[2 * n + 1] = s;
    }
    const std::vector<float> out = runChain(chain, in, frames);

    for (size_t i = 0; i < out.size(); ++i) {
        ASSERT_TRUE(std::isfinite(out[i])) << "no finito en la muestra " << i;
    }
    // Cota floja a proposito: lo que se afirma es ACOTADO contra DIVERGENTE.
    EXPECT_LT(peakOf(out), 100.0f) << "el lazo de FEEDBACK crecio sin cota";
}

// ---------------------------------------------------------------------------
// Criterio 2 — el saneo de NaN/Inf se conserva.
// ---------------------------------------------------------------------------

/**
 * Lo que SI queda de las dos pasadas: el saneo. Un NaN que entre a la cadena no
 * puede salir por el otro lado — una vez que un NaN toca el estado de un IIR,
 * el efecto queda mudo para siempre, y la comparacion `> ceiling` del viejo
 * auto-gain nunca lo hubiera visto (toda comparacion con NaN es falsa).
 *
 * POR QUE ESTE TEST ES NUEVO Y NO VENIA CON EL CODIGO: no existia. Borrar el
 * bucle de saneo entero sobrevivia los 890 tests de la suite. El criterio 2
 * dice "se conserva", y hasta ahora eso era una promesa, no una propiedad.
 *
 * El contador `mNonFiniteBlocks` que el mismo criterio nombra NO se afirma aca:
 * es privado y **todavia no tiene ningun lector** en el arbol — exponerlo es
 * WD-5.1. Afirmar la mitad observable y decir cual falta es mas honesto que un
 * test que toque el privado para poder tildar el criterio.
 */
TEST(ChainGain, ANonFiniteSampleNeverLeavesTheChain) {
    EffectChain chain;
    chain.setSampleRate(kSampleRate);
    ASSERT_TRUE(chain.addEffect(FILTER));

    std::vector<float> in(static_cast<size_t>(kBlock) * 2, 0.0f);
    std::vector<float> out(static_cast<size_t>(kBlock) * 2, 0.0f);
    for (int n = 0; n < kBlock; ++n) {
        const double t = static_cast<double>(n) / kSampleRate;
        in[2 * n] = in[2 * n + 1] =
            static_cast<float>(0.37 * std::sin(2.0 * M_PI * 220.0 * t));
    }
    in[2 * 40]      = std::numeric_limits<float>::quiet_NaN();
    in[2 * 90 + 1]  = std::numeric_limits<float>::infinity();
    in[2 * 140]     = -std::numeric_limits<float>::infinity();

    chain.process(in.data(), out.data(), kBlock);

    for (size_t i = 0; i < out.size(); ++i) {
        ASSERT_TRUE(std::isfinite(out[i]))
            << "salio un valor no finito en la muestra " << i << " (" << out[i] << ")";
    }

    // 🔴 HALLAZGO AL PASAR, MEDIDO Y **NO ARREGLADO** — no es el mecanismo de
    // WD-3.3 y arreglarlo cambia comportamiento.
    //
    // El saneo limpia el BUFFER, no el ESTADO del efecto. El NaN ya entro a la
    // memoria del IIR, asi que el efecto sigue produciendo NaN para siempre; lo
    // unico que hace el saneo es convertir eso en CEROS. Medido con una nota de
    // 220 Hz y un solo sample envenenado en la entrada, 40 bloques despues:
    //
    //   - solo NaN            ->  3 de 23 mudos para siempre
    //                             (REVERB, VOCODER, AMP_SIM)
    //   - NaN + +Inf + -Inf   -> 11 de 23 mudos para siempre
    //
    // O sea que el saneo no solo no cura: ESCONDE. La salida queda finita, nadie
    // aguas abajo se entera, y el unico rastro es `mSilentOutputBlocks`, que
    // **tampoco tiene lector** en el arbol (igual que `mNonFiniteBlocks`).
    //
    // Por eso este test NO afirma que la cadena se recupere: hoy no lo hace, y
    // afirmarlo seria pedirle a WD-3.3 un arreglo que no le toca. Lo que si
    // afirma es lo que el criterio 2 dice que se conserva, que es lo de arriba.
}
