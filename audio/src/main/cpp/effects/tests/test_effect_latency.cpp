/**
 * WD-3.1 — la latencia que cada efecto DECLARA tiene que ser la que TIENE.
 *
 * QUÉ MIDE, Y POR QUÉ ASÍ
 * -----------------------
 * Un impulso a la entrada y se busca dónde aparece la PRIMERA energía a la
 * salida. Ese corrimiento es la latencia: cuánto tarda en salir lo que entró.
 *
 * No es el largo del eco de un delay, ni el pre-delay de un reverb, ni la cola
 * de una convolución. Todo eso sale ADEMÁS de la señal directa, no en lugar de
 * ella, y por eso un delay con mix 50% tiene latencia CERO aunque su eco esté
 * 250 ms después: el directo salió en el sample 0.
 *
 * UNA CORRECCIÓN A LA AUDITORÍA QUE ORIGINÓ ESTE REQUERIMIENTO
 * -----------------------------------------------------------
 * La auditoría del 2026-08-13 afirmó que poner "un limiter en una rama y un
 * filtro en la otra" en modo PARALLEL producía un peine de 240 muestras, y lo
 * dio como defecto alcanzable hoy. **Es falso, y este archivo es el que lo
 * comprueba.** `LookaheadLimiter` NO está registrado en `EffectRegistry`: vive
 * sólo en `OutputStage`, en el bus master, donde no hay ramas que sumar.
 *
 * Los otros dos candidatos tampoco aportan latencia entera:
 *
 *   - `Oversampler` (que usa `DistortionEffect`) filtra con biquads en cascada.
 *     Un IIR de fase mínima tiene retardo de grupo dependiente de la frecuencia,
 *     no una latencia entera compensable con un delay.
 *   - `CabinetSimulator` convoluciona en el dominio del tiempo, forma directa,
 *     causal. Un FIR causal no agrega latencia: la salida en n depende de la
 *     entrada en n. (Sería distinto con un FIR de fase lineal, donde habría que
 *     declarar (N−1)/2 — pero un IR de cabinet no lo es.)
 *
 * Así que **hoy los 23 efectos tienen latencia cero, y eso es correcto**. El
 * valor de este test no es encontrar un culpable hoy: es que las declaraciones
 * dejen de ser una promesa. El día que alguien agregue un efecto con lookahead
 * y declare 0 por descuido, esto se pone rojo — y ese es exactamente el día en
 * que los cinco modos de routing paralelo empiezan a peinar.
 */

#include "../Effect.h"
#include "../EffectChain.h"
#include "../EffectRegistry.h"
#include "../EffectTypes.h"
#include "../DeciHpfEffect.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
// 4096 y no más: es el bloque máximo que el motor soporta (los scratch de
// EffectChain se alocan a 8192 samples estéreo) y también el tope interno de
// VocoderEffect, que en debug assertea si se lo pasa. Medir el ONSET no
// necesita más: si la primera energía no salió en 4096 frames, el efecto no
// tiene "latencia", tiene otro problema.
constexpr int kFrames = 4096;

/// Umbral de "acá empezó a salir señal". Bien por encima del ruido numérico y
/// bien por debajo de cualquier salida audible.
constexpr float kOnset = 1.0e-5f;

const char* nameOf(EffectType t) {
    switch (t) {
        case FILTER: return "FILTER";
        case REVERB: return "REVERB";
        case DELAY: return "DELAY";
        case VOCODER: return "VOCODER";
        case DISTORTION: return "DISTORTION";
        case COMPRESSOR: return "COMPRESSOR";
        case CHORUS: return "CHORUS";
        case PHASER: return "PHASER";
        case AMP_SIM: return "AMP_SIM";
        case CABINET: return "CABINET";
        case DECIMATOR: return "DECIMATOR";
        case DECI_HPF: return "DECI_HPF";
        case AUTO_PAN: return "AUTO_PAN";
        case COMPLEX_TREM: return "COMPLEX_TREM";
        case RANDOM_RESO: return "RANDOM_RESO";
        case HPF_DELAY: return "HPF_DELAY";
        case TAPE_ECHO: return "TAPE_ECHO";
        case HALL_REVERB: return "HALL_REVERB";
        case RISER_REVERB: return "RISER_REVERB";
        case BEAT_GRAIN: return "BEAT_GRAIN";
        case SPRING_REVERB: return "SPRING_REVERB";
        case PLATE_REVERB: return "PLATE_REVERB";
        case SHIMMER_REVERB: return "SHIMMER_REVERB";
        default: return "?";
    }
}

/**
 * @return índice del primer frame con energía, o -1 si el efecto no produjo nada.
 *
 * El impulso va en el frame 0 de un único bloque largo: procesar de una evita
 * que un borde de bloque se confunda con el frente de onda.
 */
int measureOnsetFrame(Effect& effect) {
    std::vector<float> in(static_cast<size_t>(kFrames) * 2, 0.0f);
    std::vector<float> out(static_cast<size_t>(kFrames) * 2, 0.0f);
    in[0] = 1.0f;
    in[1] = 1.0f;

    effect.process(in.data(), out.data(), kFrames);

    for (int f = 0; f < kFrames; ++f) {
        const float mag = std::max(std::abs(out[f * 2]), std::abs(out[f * 2 + 1]));
        if (mag > kOnset) return f;
    }
    return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// El contrato: lo declarado == lo medido, para los 23 tipos registrados.
// ---------------------------------------------------------------------------
TEST(EffectLatency, EveryEffectDeclaresTheLatencyItActuallyHas) {
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    int measured = 0;
    int silent = 0;

    for (int id = 0; id < EFFECT_TYPE_COUNT; ++id) {
        const auto type = static_cast<EffectType>(id);
        std::unique_ptr<Effect> effect = registry.createEffect(type);
        ASSERT_NE(effect, nullptr) << nameOf(type) << " no está registrado";

        effect->setSampleRate(kSampleRate);

        const int declared = effect->getLatencySamples();
        EXPECT_GE(declared, 0) << nameOf(type) << ": una latencia negativa no existe";

        const int onset = measureOnsetFrame(*effect);
        if (onset < 0) {
            // Hay efectos que con sus defaults no producen salida a partir de un
            // impulso — el vocoder sin modulador es el caso claro. No se los
            // puede medir así, y forzar un número inventado sería peor que
            // decir que no se midió.
            ++silent;
            continue;
        }
        ++measured;

        EXPECT_EQ(onset, declared)
            << nameOf(type) << ": declara " << declared << " samples de latencia "
            << "pero la primera energía sale en el frame " << onset << ".\n"
            << "  Si onset > declarado: el efecto retrasa la señal directa y no lo "
            << "dice — en modo PARALLEL su rama va a sumarse desalineada contra "
            << "las otras, y eso es un filtro peine.\n"
            << "  Si onset < declarado: declara de más y la compensación va a "
            << "sobre-retrasar el resto de la cadena.";
    }

    EXPECT_GT(measured, 15) << "se midieron sólo " << measured << " efectos de "
                            << EFFECT_TYPE_COUNT << "; el test perdió cobertura "
                            << "(" << silent << " no produjeron salida)";
}

// ---------------------------------------------------------------------------
// WD-2.3.1 — el mismo contrato, a los TRES sample rates del requerimiento.
//
// El test de arriba mide a 48 kHz y nada más, y eso deja un hueco con nombre:
// **la latencia de un efecto no tiene por qué ser la misma cantidad de muestras
// a otro rate**. `DECI_HPF` es el caso vivo — su retardo es el paso de su
// sample-and-hold, `fs / target`, así que a 96 kHz declara el doble de muestras
// que a 48. Un `getLatencySamples()` que devolviera una constante coincidiría
// con lo medido a 48 kHz y mentiría en los otros dos, y hoy nada lo notaría.
//
// Lo que se afirma sigue siendo lo mismo —declarado == medido— sólo que ahora
// una vez por rate. Es la mitad de WD-2.3 que faltaba: el criterio 1.
// ---------------------------------------------------------------------------
TEST(EffectLatency, TheDeclaredLatencyHoldsAtEverySampleRate) {
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    constexpr int kRates[3] = {44100, 48000, 96000};
    int measured = 0;

    for (int rate : kRates) {
        for (int id = 0; id < EFFECT_TYPE_COUNT; ++id) {
            const auto type = static_cast<EffectType>(id);
            std::unique_ptr<Effect> effect = registry.createEffect(type);
            ASSERT_NE(effect, nullptr) << nameOf(type) << " no está registrado";

            effect->setSampleRate(rate);

            const int declared = effect->getLatencySamples();
            EXPECT_GE(declared, 0)
                << nameOf(type) << " a " << rate << " Hz: una latencia negativa no existe";

            const int onset = measureOnsetFrame(*effect);
            if (onset < 0) continue;  // sin salida con sus defaults; ya lo cuenta el test de arriba
            ++measured;

            EXPECT_EQ(onset, declared)
                << nameOf(type) << " a " << rate << " Hz: declara " << declared
                << " samples y la primera energía sale en el frame " << onset << ".\n"
                << "  Que a 48 kHz coincida no alcanza: la latencia se declara en "
                << "MUESTRAS y la cantidad de muestras que tarda un efecto puede "
                << "depender del rate. Este es el rate donde se rompió.";
        }
    }

    EXPECT_GT(measured, 45) << "se midieron sólo " << measured << " combinaciones "
                            << "efecto×rate de las " << (EFFECT_TYPE_COUNT * 3)
                            << " posibles; el test perdió cobertura";
}

// ---------------------------------------------------------------------------
// Y el caso que le da sentido al test de arriba: un efecto cuya latencia SÍ
// cambia con el rate. Sin al menos uno, aquel barrido no distingue una
// implementación correcta de una que devuelve una constante.
// ---------------------------------------------------------------------------
TEST(EffectLatency, DeciHpfDeclaresMoreSamplesAtAHigherRate) {
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    auto latencyAt = [&](int rate) {
        auto fx = registry.createEffect(DECI_HPF);
        fx->setSampleRate(rate);
        // Target fijo en Hz: el paso del sample-and-hold es fs/target, así que
        // con el target quieto la latencia tiene que escalar con el rate.
        fx->setParam(DeciHpfEffect::PARAM_SAMPLE_RATE, 1000.0f);
        return fx->getLatencySamples();
    };

    const int at48 = latencyAt(48000);
    const int at96 = latencyAt(96000);

    ASSERT_GT(at48, 0) << "DECI_HPF dejó de declarar latencia; el test perdió su caso";

    // Lo que escala es el PASO del hold, no la latencia: `getLatencySamples()`
    // devuelve `ceil(step) - 1`, porque el hold ya entrega su primer valor en el
    // frame 0 y sólo los `step - 1` siguientes salen con el valor viejo.
    //
    // Es un detalle de una unidad y aun así importa: la primera versión de este
    // test afirmó `at96 == at48 * 2`, midió 95 contra 94 y se puso roja. **El
    // defectuoso era el test.** 47 y 95 son exactamente 48-1 y 96-1, o sea el
    // comportamiento correcto — y afirmar el doble exacto habría obligado a
    // "arreglar" un efecto que no tenía nada.
    EXPECT_EQ(at96 + 1, (at48 + 1) * 2)
        << "DECI_HPF declara " << at48 << " samples a 48 kHz y " << at96
        << " a 96 (pasos de " << (at48 + 1) << " y " << (at96 + 1) << ").\n"
        << "  Su latencia es el paso del sample-and-hold menos uno, y el paso es "
        << "fs/target: con el target fijo en 1 kHz, duplicar el rate tiene que "
        << "duplicar el paso. Si devuelve lo mismo a los dos, "
        << "getLatencySamples() está ignorando el sample rate — y entonces "
        << "TheDeclaredLatencyHoldsAtEverySampleRate pasa a no probar nada.";
}

// ---------------------------------------------------------------------------
// Un delay NO tiene latencia. Es el malentendido más fácil de cometer al
// implementar getLatencySamples(), y el que rompería la compensación: declarar
// 250 ms haría que la cadena entera se retrase un cuarto de segundo.
// ---------------------------------------------------------------------------
TEST(EffectLatency, ADelayHasEchoesNotLatency) {
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    for (EffectType type : {DELAY, TAPE_ECHO, HPF_DELAY}) {
        auto effect = registry.createEffect(type);
        ASSERT_NE(effect, nullptr);
        effect->setSampleRate(kSampleRate);

        EXPECT_EQ(effect->getLatencySamples(), 0)
            << nameOf(type) << " declara latencia. El eco de un delay sale ADEMÁS "
            << "de la señal directa, no en lugar de ella: el directo está en el "
            << "sample 0 y la latencia es cero. Declarar el tiempo de delay acá "
            << "retrasaría toda la cadena por el largo del eco.";

        EXPECT_EQ(measureOnsetFrame(*effect), 0)
            << nameOf(type) << ": la señal directa no está en el sample 0";
    }
}

// ---------------------------------------------------------------------------
// La cadena suma lo que declaran sus efectos. Es lo que un host de DAW le pide
// al plugin, y lo que el reporte de latencia de WD-8.1 va a consumir.
// ---------------------------------------------------------------------------
TEST(EffectLatency, TheChainReportsTheSumOfItsEffects) {
    EffectChain chain;
    chain.setSampleRate(kSampleRate);

    EXPECT_EQ(chain.getLatencySamples(), 0) << "una cadena vacía no retrasa nada";

    ASSERT_TRUE(chain.addEffect(FILTER));
    ASSERT_TRUE(chain.addEffect(CHORUS));
    ASSERT_TRUE(chain.addEffect(COMPRESSOR));

    // Con todos los efectos actuales en cero, la suma es cero — y el test
    // igual vale: verifica que la cadena CONSULTA a sus efectos en vez de
    // devolver una constante. Si alguien devolviera 0 fijo, el día que aparezca
    // un efecto con latencia esto seguiría en verde y la compensación no se
    // aplicaría nunca.
    int expected = 0;
    for (size_t i = 0; i < chain.getNumEffects(); ++i) {
        expected += chain.getEffectLatencySamples(i);
    }
    EXPECT_EQ(chain.getLatencySamples(), expected);
}

// ===========================================================================
// LA COMPENSACIÓN (segunda mitad de WD-3.1)
//
// Los efectos reales tienen latencias de 0 a 3 samples con sus defaults, así
// que con ellos el peine no se ve. Para probar la compensación hace falta un
// efecto que SÍ retrase — y construirlo es legítimo: es exactamente lo que va a
// entrar el día que alguien agregue un limiter con lookahead, que es el caso
// para el que existe todo este mecanismo.
// ===========================================================================

namespace {

/// Retrasa la señal N samples y declara ese retraso. Nada más.
class DelayingEffect : public Effect {
public:
    explicit DelayingEffect(int latency) : mLatency(latency), mLine(static_cast<size_t>(latency) * 2 + 2, 0.0f) {}

    void process(float* input, float* output, int numFrames) override {
        if (mLatency <= 0) {
            std::copy(input, input + numFrames * 2, output);
            return;
        }
        const int cap = mLatency;
        for (int f = 0; f < numFrames; ++f) {
            const float l = input[f * 2], r = input[f * 2 + 1];
            output[f * 2]     = mLine[static_cast<size_t>(mPos) * 2];
            output[f * 2 + 1] = mLine[static_cast<size_t>(mPos) * 2 + 1];
            mLine[static_cast<size_t>(mPos) * 2]     = l;
            mLine[static_cast<size_t>(mPos) * 2 + 1] = r;
            mPos = (mPos + 1) % cap;
        }
    }
    void setParam(int, float) override {}
    float getParam(int) override { return 0.0f; }
    void setSampleRate(int) override {}
    void reset() override { std::fill(mLine.begin(), mLine.end(), 0.0f); mPos = 0; }
    int getLatencySamples() const override { return mLatency; }

private:
    int mLatency;
    std::vector<float> mLine;
    int mPos = 0;
};

/// Suma dos ramas —una retrasada `latency`, la otra no— con y sin alinear, y
/// devuelve la energía de cada resultado. Si la compensación funciona, la suma
/// alineada conserva la energía y la desalineada la pierde en los notches.
struct CombEnergy { double aligned; double unaligned; };

CombEnergy sumTwoBranches(int latency, int numFrames) {
    // Ruido determinista: banda ancha, que es donde un peine se nota.
    std::vector<float> in(static_cast<size_t>(numFrames) * 2);
    uint32_t seed = 12345u;
    for (auto& v : in) {
        seed = seed * 1664525u + 1013904223u;
        v = (static_cast<float>(seed >> 8) / 8388608.0f) - 1.0f;
    }

    DelayingEffect slow(latency);
    std::vector<float> slowOut(in.size(), 0.0f);
    slow.process(in.data(), slowOut.data(), numFrames);

    double unaligned = 0.0, aligned = 0.0;
    // Sin alinear: rama directa + rama retrasada.
    for (size_t i = 0; i < in.size(); ++i) {
        const double v = static_cast<double>(in[i]) + slowOut[i];
        unaligned += v * v;
    }
    // Alineada: la rama directa retrasada lo mismo → las dos en fase.
    DelayingEffect align(latency);
    std::vector<float> fastAligned(in.size(), 0.0f);
    align.process(in.data(), fastAligned.data(), numFrames);
    for (size_t i = 0; i < in.size(); ++i) {
        const double v = static_cast<double>(fastAligned[i]) + slowOut[i];
        aligned += v * v;
    }
    return {aligned, unaligned};
}

}  // namespace

// ---------------------------------------------------------------------------
// El peine existe, y es grande. Esto establece POR QUÉ hace falta compensar:
// sumar dos ramas desalineadas pierde energía en los notches.
// ---------------------------------------------------------------------------
TEST(EffectLatency, SummingMisalignedBranchesIsAComb) {
    // 479 samples: el peor caso real medido, DECI_HPF con reducción al máximo.
    const auto e = sumTwoBranches(479, 8192);

    ASSERT_GT(e.aligned, 0.0);
    const double ratio = e.unaligned / e.aligned;

    EXPECT_LT(ratio, 0.75)
        << "sumar dos ramas con 479 samples de desalineación dio " << ratio
        << " de la energía de la suma alineada. Si esto se acerca a 1, el "
        << "montaje del test no está produciendo el peine que la compensación "
        << "existe para evitar, y los tests de abajo no prueban nada.";
}

// ---------------------------------------------------------------------------
// Suma vs máximo. Con un solo efecto latente los dos dan el MISMO número, así
// que hacen falta DOS — si no, el test no distingue una implementación de la
// otra. (Se comprobó por mutación: con FILTER + DECI_HPF el mutante que siempre
// suma pasaba.)
// ---------------------------------------------------------------------------
TEST(EffectLatency, TheChainReportsTheSlowestBranchNotTheSumInParallelModes) {
    EffectChain chain;
    chain.setSampleRate(kSampleRate);
    ASSERT_TRUE(chain.addEffect(DECI_HPF));
    ASSERT_TRUE(chain.addEffect(DECI_HPF));

    const int one = chain.getEffectLatencySamples(0);
    ASSERT_GT(one, 0) << "DECI_HPF dejó de declarar latencia; el test perdió su caso";
    ASSERT_EQ(chain.getEffectLatencySamples(1), one);

    chain.setRoutingMode(RoutingMode::SERIAL);
    EXPECT_EQ(chain.getLatencySamples(), one * 2)
        << "en SERIAL la señal atraviesa los dos: se suman";

    chain.setRoutingMode(RoutingMode::PARALLEL);
    EXPECT_EQ(chain.getLatencySamples(), one)
        << "en PARALLEL las ramas se alinean contra la más lenta, así que la "
        << "cadena retrasa lo que retrasa esa — NO la suma. Si esto da " << (one * 2)
        << ", getLatencySamples() está sumando en un modo que no suma.";
}

// ---------------------------------------------------------------------------
// EL QUE PRUEBA QUE LA COMPENSACIÓN SE APLICA.
//
// Un impulso a una cadena en PARALLEL con dos ramas de latencia distinta:
//
//   con compensación   la rama rápida se retrasa hasta la lenta, así que la
//                      PRIMERA energía de la suma sale recién en `maxLatencia`.
//   sin compensación   la rama rápida sale en el sample 0 y la lenta después:
//                      dos frentes de onda, el primero en 0.
//
// El onset es el discriminador, y es directo de medir.
// ---------------------------------------------------------------------------
TEST(EffectLatency, TheFastBranchIsDelayedToMeetTheSlowOne) {
    EffectChain chain;
    chain.setSampleRate(kSampleRate);

    ASSERT_TRUE(chain.addEffect(FILTER));     // rama rápida: 0 samples
    ASSERT_TRUE(chain.addEffect(DECI_HPF));   // rama lenta

    // Bajar el target de sample rate sube la latencia del hold: step = fs/target.
    // Con 1000 Hz son 48 samples — suficiente para medirlo sin ambigüedad y muy
    // por debajo del tope de compensación (512).
    chain.setParameter(1, DeciHpfEffect::PARAM_SAMPLE_RATE, 1000.0f);
    chain.setRoutingMode(RoutingMode::PARALLEL);

    const int declared = chain.getLatencySamples();
    ASSERT_GT(declared, 8) << "la rama lenta no quedó suficientemente lenta para medir";

    std::vector<float> in(static_cast<size_t>(kFrames) * 2, 0.0f);
    std::vector<float> out(static_cast<size_t>(kFrames) * 2, 0.0f);
    in[0] = 1.0f;
    in[1] = 1.0f;

    // Dos pasadas: la primera deja los suavizadores de bypass asentados, para
    // que el frente de onda que se mide sea el del audio y no el de una rampa.
    chain.process(in.data(), out.data(), kFrames);
    std::fill(out.begin(), out.end(), 0.0f);
    std::fill(in.begin(), in.end(), 0.0f);
    in[0] = 1.0f;
    in[1] = 1.0f;
    chain.process(in.data(), out.data(), kFrames);

    int onset = -1;
    for (int f = 0; f < kFrames; ++f) {
        if (std::max(std::abs(out[f * 2]), std::abs(out[f * 2 + 1])) > kOnset) {
            onset = f;
            break;
        }
    }
    ASSERT_GE(onset, 0) << "la cadena no produjo salida; el test no midió nada";

    // La cota es `declared / 2` y no `declared`, y la razón importa:
    // DECI_HPF es un SAMPLE-AND-HOLD, o sea un sistema variante en el tiempo,
    // no LTI. Su retardo instantáneo depende de la fase en que esté su contador
    // cuando llega el impulso, y va de 0 a `step`. El valor declarado es la
    // COTA de ese rango, que es lo correcto para compensar — alinear contra el
    // peor caso nunca desalinea de más.
    //
    // Lo que este test discrimina es otra cosa, y es binaria: con compensación
    // la rama rápida sale recién cerca de `declared`; sin ella sale en el
    // sample 0. Un onset de 0 es la firma exacta del defecto.
    EXPECT_GT(onset, declared / 2)
        << "la primera energía salió en el frame " << onset << " y la cadena "
        << "declara " << declared << " samples.\n"
        << "Un onset cerca de 0 significa que la rama RÁPIDA llegó sin alinear: "
        << "se suma contra la lenta con hasta " << declared << " samples de "
        << "desfase, que es el filtro peine que accumulateBranch() existe para "
        << "evitar. Primer notch en " << (kSampleRate / (2.0 * declared)) << " Hz.";
}

// ---------------------------------------------------------------------------
// REQ-012 — el recorte de compensación deja de ser silencioso en SPLIT_2X2.
//
// `BranchDelay` retrasa hasta MAX_DELAY_FRAMES (512) y devuelve false cuando hay
// que recortar. Ese false se contaba en el camino de la compensacion por slot, pero
// SPLIT_2X2 llamaba a `mBranchDelays[..].process()` directo y lo DESCARTABA: el
// único modo que puede pedir la compensación más grande —sus ramas son rangos
// seriales, así que su latencia es una SUMA— era el único que podía alinear de
// menos sin decirlo.
// ---------------------------------------------------------------------------

namespace {

/** SPLIT_2X2 con la rama A pidiendo `targetHz` en sus dos DECI_HPF. */
uint64_t clampedBlocksForSplit(float targetHz) {
    EffectChain chain;
    chain.setSampleRate(kSampleRate);

    EXPECT_TRUE(chain.addEffect(DECI_HPF));   // rama A, slot 0
    EXPECT_TRUE(chain.addEffect(DECI_HPF));   // rama A, slot 1
    EXPECT_TRUE(chain.addEffect(FILTER));     // rama B, slot 2 — 0 samples
    EXPECT_TRUE(chain.addEffect(FILTER));     // rama B, slot 3 — 0 samples

    chain.setParameter(0, DeciHpfEffect::PARAM_SAMPLE_RATE, targetHz);
    chain.setParameter(1, DeciHpfEffect::PARAM_SAMPLE_RATE, targetHz);
    chain.setRoutingMode(RoutingMode::SPLIT_2X2);

    std::vector<float> in(static_cast<size_t>(kFrames) * 2, 0.1f);
    std::vector<float> out(static_cast<size_t>(kFrames) * 2, 0.0f);

    // Dos bloques: el primero se va en el crossfade de cambio de modo.
    chain.process(in.data(), out.data(), kFrames);
    chain.process(in.data(), out.data(), kFrames);

    return chain.latencyClampedBlocks();
}

}  // namespace

TEST(EffectLatency, SplitCountsTheBlocksItCouldNotFullyAlign) {
    // Rama A = 2 × ceil(48000/100) - 1 = 2 × 479 = 958 samples, contra una rama B
    // de 0: se piden 958 de retardo y entran 512. Eso es un recorte, y se cuenta.
    EXPECT_GT(clampedBlocksForSplit(100.0f), 0u)
        << "SPLIT_2X2 alineó de menos y no lo contó: el recorte volvió a ser silencioso.";

    // CONTROL: con 1000 Hz la rama A suma 2 × 47 = 94 samples, muy por debajo del
    // tope. Si esto también contara, el test estaría midiendo cualquier cosa.
    EXPECT_EQ(clampedBlocksForSplit(1000.0f), 0u)
        << "contó un recorte donde el retardo pedido (94) entra de sobra en 512.";
}
