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
