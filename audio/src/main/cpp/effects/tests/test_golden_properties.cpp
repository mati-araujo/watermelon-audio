/**
 * WD-2.2 — la capa property-based: lo que tiene que valer para LOS 23 EFECTOS.
 *
 * QUE HACE DISTINTO A ESTO
 * ------------------------
 * Las otras capas de la suite golden miden un efecto por vez contra un valor
 * que la teoria fija. Esta barre el catalogo ENTERO y afirma tres propiedades
 * que no dependen de que hace cada efecto:
 *
 *   1. La salida es FINITA. Ni NaN ni infinito, con cualquier combinacion de
 *      parametros que el efecto acepte.
 *   2. La salida es ACOTADA. Un efecto puede amplificar; lo que no puede es
 *      divergir.
 *   3. `reset()` DEJA EL EFECTO COMO ESTABA. Procesar, resetear y volver a
 *      procesar la misma señal tiene que dar el mismo audio, muestra a muestra.
 *
 * La tercera es la que el requerimiento nombra como "la que habria cachado el
 * hueco de reset() de WD-3.2". `Effect::reset()` es virtual con default vacio:
 * un efecto con estado que no la sobrescriba compila perfecto, y su cola vieja
 * se filtra al contexto nuevo — que es exactamente el bug del residual del pad
 * que aparecia en el primer bloque de INPUT_FX.
 *
 * POR QUE PARAMETROS ALEATORIOS Y NO UNA LISTA
 * --------------------------------------------
 * Porque la lista la escribe la misma cabeza que escribio el efecto, y elige
 * los valores que sabe que andan. El barrido aleatorio pega en las esquinas
 * —Q al maximo con cutoff al minimo, feedback al tope con mix al tope— que es
 * donde vive la divergencia. Los efectos CLAMPEAN sus parametros, asi que
 * mandarles valores fuera de rango es legitimo: si uno no clampea y explota,
 * eso es el hallazgo, no un abuso del test.
 *
 * El generador tiene SEMILLA FIJA. Un test que falla una de cada veinte
 * corridas no es un test, es ruido: con semilla fija, un fallo se reproduce
 * exactamente y el caso se puede pegar en un test dedicado.
 */

#include "EffectCatalog.h"

#include "../Effect.h"
#include "../EffectRegistry.h"
#include "../EffectTypes.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

using wma::catalog::nameOf;

constexpr int kSampleRate = 48000;
constexpr int kBlock = 512;
constexpr int kBlocks = 8;

/// Cota de "no divergio". Deliberadamente holgada: hay efectos que amplifican a
/// proposito (distorsion con drive al tope, resonancia en auto-oscilacion). Lo
/// que esto separa no es fuerte de suave, es acotado de divergente.
constexpr float kSaneBound = 1000.0f;

/// Cantidad de ids de parametro que se barren. Es un SUPERCONJUNTO a proposito:
/// ningun efecto tiene mas, y los `switch` de setParam ignoran lo que no
/// conocen, asi que barrer de mas no rompe nada y cubre a los que tienen mas
/// parametros de los que uno recordaba.
constexpr int kMaxParamId = 16;

std::vector<float> noiseBlock(std::mt19937& rng, int frames, float amp) {
    std::uniform_real_distribution<float> d(-amp, amp);
    std::vector<float> b(static_cast<size_t>(frames) * 2);
    for (auto& s : b) s = d(rng);
    return b;
}

/// Barre parametros con valores que caen dentro y fuera de todo rango plausible.
void randomizeParams(Effect& fx, std::mt19937& rng) {
    std::uniform_real_distribution<float> d(-2.0f, 2.0f);
    std::uniform_real_distribution<float> scale(0.0f, 1.0f);
    for (int id = 0; id < kMaxParamId; ++id) {
        // Mezcla de escalas: los parametros de esta libreria van desde 0..1
        // (mix, feedback) hasta 20..20000 (frecuencias) pasando por dB
        // negativos (thresholds). Una sola escala solo probaria un tercio.
        const float pick = scale(rng);
        float v;
        if (pick < 0.34f) {
            v = d(rng);                    // 0..1 y un poco afuera
        } else if (pick < 0.67f) {
            v = d(rng) * 60.0f;            // dB
        } else {
            v = std::abs(d(rng)) * 12000.0f;  // Hz
        }
        fx.setParam(id, v);
    }
}

struct Sanity {
    bool finite = true;
    bool bounded = true;
    float worst = 0.0f;
    int badFrame = -1;
};

Sanity checkBuffer(const std::vector<float>& b) {
    Sanity s;
    for (size_t i = 0; i < b.size(); ++i) {
        const float v = b[i];
        if (!std::isfinite(v)) {
            s.finite = false;
            if (s.badFrame < 0) s.badFrame = static_cast<int>(i / 2);
            continue;
        }
        const float a = std::abs(v);
        if (a > s.worst) s.worst = a;
        if (a > kSaneBound && s.bounded) {
            s.bounded = false;
            if (s.badFrame < 0) s.badFrame = static_cast<int>(i / 2);
        }
    }
    return s;
}

/// Corre `kBlocks` bloques de ruido por el efecto y devuelve toda la salida.
std::vector<float> runNoise(Effect& fx, std::mt19937& rng, float amp) {
    std::vector<float> all;
    all.reserve(static_cast<size_t>(kBlock) * 2 * kBlocks);
    std::vector<float> out(static_cast<size_t>(kBlock) * 2, 0.0f);
    for (int b = 0; b < kBlocks; ++b) {
        std::vector<float> in = noiseBlock(rng, kBlock, amp);
        fx.process(in.data(), out.data(), kBlock);
        all.insert(all.end(), out.begin(), out.end());
    }
    return all;
}

/// Lee `reset-baseline.txt` y devuelve los nombres de efecto declarados.
/// Formato de linea: `NOMBRE | estado | nota`; `#` y vacias se ignoran.
std::set<std::string> readResetBaseline() {
    std::set<std::string> names;
    std::FILE* f = std::fopen(WMA_RESET_BASELINE, "rb");
    EXPECT_NE(f, nullptr) << "no pude abrir " << WMA_RESET_BASELINE;
    if (f == nullptr) return names;

    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string s(line);
        const size_t hash = s.find('#');
        if (hash != std::string::npos) s = s.substr(0, hash);
        const size_t bar = s.find('|');
        if (bar != std::string::npos) s = s.substr(0, bar);
        const size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        const size_t e = s.find_last_not_of(" \t\r\n");
        names.insert(s.substr(b, e - b + 1));
    }
    std::fclose(f);
    return names;
}

}  // namespace

// ===========================================================================
// 1 y 2 — finita y acotada, con parametros de todo el espacio
// ===========================================================================

TEST(EffectProperties, EveryEffectStaysFiniteAndBoundedUnderRandomParams) {
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    constexpr int kTrials = 24;

    for (int id = 0; id < EFFECT_TYPE_COUNT; ++id) {
        const auto type = static_cast<EffectType>(id);

        for (int trial = 0; trial < kTrials; ++trial) {
            // Semilla derivada del par (efecto, intento): reproducible, y un
            // fallo se re-corre solo con -R sobre este test.
            std::mt19937 rng(0x2D22u + static_cast<unsigned>(id) * 1009u +
                             static_cast<unsigned>(trial));

            std::unique_ptr<Effect> fx = registry.createEffect(type);
            ASSERT_NE(fx, nullptr) << nameOf(type) << " no esta registrado";
            fx->setSampleRate(kSampleRate);
            randomizeParams(*fx, rng);

            const std::vector<float> out = runNoise(*fx, rng, 0.8f);
            const Sanity s = checkBuffer(out);

            ASSERT_TRUE(s.finite)
                << nameOf(type) << " (intento " << trial << ") produjo NaN o "
                << "infinito en el frame " << s.badFrame;
            ASSERT_TRUE(s.bounded)
                << nameOf(type) << " (intento " << trial << ") divergio: pico "
                << s.worst << " en el frame " << s.badFrame
                << " (cota " << kSaneBound << ")";
        }
    }
}

TEST(EffectProperties, SilenceInGivesSilenceOutForEveryEffect) {
    // Un efecto que genera algo con la entrada en cero esta auto-oscilando o
    // leyendo estado sin inicializar. La excepcion legitima seria un generador,
    // y en este catalogo no hay ninguno: los 23 procesan lo que les entra.
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    for (int id = 0; id < EFFECT_TYPE_COUNT; ++id) {
        const auto type = static_cast<EffectType>(id);
        std::unique_ptr<Effect> fx = registry.createEffect(type);
        ASSERT_NE(fx, nullptr);
        fx->setSampleRate(kSampleRate);

        std::vector<float> in(static_cast<size_t>(kBlock) * 2, 0.0f);
        std::vector<float> out(static_cast<size_t>(kBlock) * 2, 0.0f);
        for (int b = 0; b < kBlocks; ++b) {
            fx->process(in.data(), out.data(), kBlock);
        }

        const Sanity s = checkBuffer(out);
        ASSERT_TRUE(s.finite) << nameOf(type) << ": silencio a la entrada y NaN a la salida";
        EXPECT_LT(s.worst, 1e-3f)
            << nameOf(type) << " genera " << s.worst << " con la entrada en cero";
    }
}

// ===========================================================================
// 3 — reset() deja el efecto como estaba
// ===========================================================================

TEST(EffectProperties, ResetMakesEveryEffectReproduceItsOutputExactly) {
    // ESTA ES LA QUE EL REQUERIMIENTO NOMBRA para el hueco de reset() de WD-3.2.
    //
    // El metodo: ensuciar un efecto con audio fuerte, `reset()`, y exigir que
    // produzca EL MISMO AUDIO que un efecto RECIEN CONSTRUIDO con los mismos
    // parametros. Bit a bit, no con tolerancia: no hay ninguna razon legitima
    // por la que un efecto reseteado difiera de uno nuevo ni en el ultimo bit.
    //
    // COMPARAR CONTRA UNO NUEVO, Y NO CONTRA SI MISMO DOS VECES, ES LO QUE HACE
    // QUE EL TEST SIRVA. La primera version comparaba dos pasadas del mismo
    // efecto, cada una precedida de un reset. Sonaba equivalente y NO lo era:
    // un mutante que borraba el limpiado de los buffers de `DelayEffect`
    // sobrevivia intacto. El motivo, medido: con el delay por defecto en 250 ms
    // (12.000 samples) y `writePos` vuelto a 0, la pasada 2 lee de una zona del
    // buffer que la pasada 1 nunca llego a escribir — las dos leen la misma
    // basura vieja y coinciden. Contra un efecto nuevo, esa zona son ceros y la
    // diferencia aparece en la primera muestra que sale del delay.
    //
    // Se ensucia ANTES del reset a proposito: un efecto que nunca proceso audio
    // no tiene estado que limpiar, y el test pasaria sin medir nada.
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    const std::set<std::string> baseline = readResetBaseline();
    std::set<std::string> failing;

    for (int id = 0; id < EFFECT_TYPE_COUNT; ++id) {
        const auto type = static_cast<EffectType>(id);

        std::unique_ptr<Effect> fx = registry.createEffect(type);
        ASSERT_NE(fx, nullptr);
        fx->setSampleRate(kSampleRate);

        // La señal de prueba, fija para las dos pasadas.
        std::mt19937 sig(0x5EEDu + static_cast<unsigned>(id));
        std::vector<std::vector<float>> probe;
        for (int b = 0; b < kBlocks; ++b) probe.push_back(noiseBlock(sig, kBlock, 0.5f));

        auto runProbe = [&](Effect& e) {
            std::vector<float> all;
            std::vector<float> out(static_cast<size_t>(kBlock) * 2, 0.0f);
            for (auto blk : probe) {
                e.process(blk.data(), out.data(), kBlock);
                all.insert(all.end(), out.begin(), out.end());
            }
            return all;
        };

        // La referencia: un efecto NUEVO, sin haber procesado nunca nada.
        std::unique_ptr<Effect> fresh = registry.createEffect(type);
        ASSERT_NE(fresh, nullptr);
        fresh->setSampleRate(kSampleRate);
        const std::vector<float> want = runProbe(*fresh);

        // Ensuciar: audio fuerte y distinto del probe, para dejar colas,
        // envolventes y lineas de delay cargadas.
        std::mt19937 dirt(0xD127u + static_cast<unsigned>(id));
        runNoise(*fx, dirt, 0.9f);
        fx->reset();
        const std::vector<float> got = runProbe(*fx);

        // Y la segunda pasada, para la otra mitad de la propiedad: reset() tiene
        // que ser REPETIBLE, no solo equivalente a construir.
        fx->reset();
        const std::vector<float> again = runProbe(*fx);

        ASSERT_EQ(want.size(), got.size());
        ASSERT_EQ(want.size(), again.size());

        // Se toma la UNION de las dos comparaciones porque cada una ve cosas que
        // la otra no. Medido, y en las dos direcciones: contra-nuevo es la unica
        // que caza a DISTORTION, y contra-si-mismo es la unica que caza a
        // BEAT_GRAIN. Quedarse con una sola perdia un efecto real.
        long diverged = -1;
        for (size_t i = 0; i < want.size(); ++i) {
            if (want[i] != got[i] || got[i] != again[i]) {
                diverged = static_cast<long>(i / 2);
                break;
            }
        }
        if (diverged >= 0) failing.insert(nameOf(type));
    }

    // --- el trinquete ------------------------------------------------------
    //
    // Deuda NUEVA: un efecto que falla y no esta declarado.
    for (const std::string& name : failing) {
        EXPECT_TRUE(baseline.count(name) > 0)
            << name << " no cumple el contrato de reset() y NO esta en "
            << "reset-baseline.txt.\n"
            << "  Procesar, resetear y volver a procesar la misma señal le da "
            << "audio distinto: arrastra estado a traves del reset.\n"
            << "  Effect::reset() tiene default vacio, asi que un efecto con "
            << "estado que no la sobrescriba compila perfecto y filtra su cola "
            << "vieja al contexto nuevo.\n"
            << "  Arreglalo, o —si es deuda consciente— declarala en el baseline "
            << "con que sobrevive y quien la saca.";
    }

    // Deuda PAGADA: declarada, pero ya no se reproduce. Sin esto el archivo se
    // queda mintiendo sobre la deuda, que es como un baseline deja de servir.
    for (const std::string& name : baseline) {
        EXPECT_TRUE(failing.count(name) > 0)
            << name << " esta en reset-baseline.txt pero YA CUMPLE el contrato "
            << "de reset().\n"
            << "  Si lo arreglaste, borra su linea del baseline: el trinquete "
            << "existe para que la deuda declarada sea la deuda real.";
    }

    // Y el numero, para que una mejora sea visible de un vistazo en el diff.
    EXPECT_EQ(failing.size(), baseline.size())
        << "fallan " << failing.size() << " efectos de " << EFFECT_TYPE_COUNT
        << "; el baseline declara " << baseline.size();
}
