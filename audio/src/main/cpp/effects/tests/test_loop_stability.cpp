/**
 * WD-3.5 (hallazgo) — ESTABILIDAD DE LAZO: la clase de divergencia que NO
 * depende del sample rate.
 *
 * POR QUE ESTE ARCHIVO EXISTE
 * ---------------------------
 * `nyquist-baseline.txt` declaraba SPRING_REVERB como "diverge a 8.000 Hz". Al
 * diagnosticarlo salio que diverge a los ocho rates probados —8.000, 11.025,
 * 16.000, 22.050, 32.000, 44.100, 48.000 y 96.000— y en el mismo tiempo: entre
 * 0,96 y 1,22 s hasta pasar la cota. No era un defecto de Nyquist.
 *
 * Aparecia solo en el rate mas bajo por un artefacto del INSTRUMENTO: la ventana
 * del test iba en BLOQUES, y 32 bloques de 512 muestras son 2,0 s a 8 kHz y
 * 0,34 s a 48 kHz. El unico rate cuya ventana llegaba a la cota era el mas bajo,
 * asi que el defecto se le atribuyo al unico eje que el test variaba.
 *
 * **Un baseline indexado por sample rate atribuye al sample rate.** Esa es la
 * leccion, y este archivo es la respuesta: mide la otra clase, con la ventana en
 * SEGUNDOS y a un rate ALTO, que es donde la clase de Nyquist no puede estar.
 *
 * EL INSTRUMENTO: RESPUESTA AL IMPULSO, NO SEÑAL SOSTENIDA
 * --------------------------------------------------------
 * Los barridos de este repo martillan con una cuadrada a Nyquist a fondo de
 * escala y declaran divergencia cuando el pico pasa de 1.000. Eso mezcla dos
 * cosas distintas: un lazo estable con ganancia alta llega a un estado
 * estacionario ELEVADO —finito— y uno inestable crece sin cota. La cota de 1.000
 * no las distingue; solo dice cual de las dos llego antes.
 *
 * Un impulso seguido de silencio si las distingue, y sin depender de ninguna
 * cota elegida: si la envolvente de la cola CRECE, el lazo tiene ganancia mayor
 * que 1. Se mide la razon media entre ventanas consecutivas en la segunda mitad
 * de la cola, que es donde ya no queda transitorio de la entrada.
 *
 * El instrumento esta validado contra comportamiento conocido: REVERB,
 * HALL_REVERB y PLATE_REVERB decaen con razones de 0,22 a 0,31 por ventana, y el
 * propio SPRING con el decay en 1,0 decae en 0,25. Un instrumento que declarara
 * inestable a un reverb sano no serviria para acusar a este.
 */

#include "EffectCatalog.h"

#include "../Effect.h"
#include "../EffectRegistry.h"
#include "../EffectTypes.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using wma::catalog::nameOf;

/// Rate ALTO a proposito: aca no puede haber un omega pasado de pi, asi que lo
/// que este test encuentre no es de `nyquist-baseline.txt` por construccion.
constexpr int kRate = 48000;
constexpr int kFrames = 512;
constexpr double kWindowSeconds = 0.5;
constexpr double kTotalSeconds = 8.0;

/// Por encima de esto se llama crecimiento. No es una tolerancia elegida: la
/// medicion separo dos poblaciones sin zona gris — los efectos sanos decaen
/// entre 0,22 y 0,31 por ventana, y el unico que crece lo hace a 3,15. Cualquier
/// corte entre 0,4 y 3 da el mismo resultado; se pone cerca de 1 porque 1 es
/// donde esta el significado (ganancia de lazo unitaria), no donde esta el hueco.
constexpr double kGrowthThreshold = 1.02;

/// Envolvente (pico por ventana) de la cola de un impulso.
/// Devuelve vacio si la cola se fue a no-finito: eso ya es la respuesta.
std::vector<double> impulseTailEnvelope(Effect& fx) {
    const int windowFrames = static_cast<int>(kWindowSeconds * kRate);
    const int blocks = static_cast<int>(kTotalSeconds * kRate / kFrames);

    std::vector<float> in(static_cast<size_t>(kFrames) * 2, 0.0f);
    std::vector<float> out(in.size(), 0.0f);

    std::vector<double> env;
    double peak = 0.0;
    int framesInWindow = 0;
    bool blewUp = false;

    for (int b = 0; b < blocks; ++b) {
        // Un solo impulso, en el primer bloque; silencio a partir del segundo.
        if (b == 0) { in[0] = 1.0f; in[1] = 1.0f; }
        else if (b == 1) { in[0] = 0.0f; in[1] = 0.0f; }

        fx.process(in.data(), out.data(), kFrames);
        for (float s : out) {
            if (!std::isfinite(s)) { blewUp = true; break; }
            peak = std::max(peak, static_cast<double>(std::abs(s)));
        }
        if (blewUp) break;

        framesInWindow += kFrames;
        if (framesInWindow >= windowFrames) {
            env.push_back(peak);
            peak = 0.0;
            framesInWindow = 0;
        }
    }
    if (blewUp) return {};
    return env;
}

/// Razon geometrica de crecimiento por ventana, medida de punta a punta de la
/// cola: de la ventana 1 (la primera despues del impulso directo) a la ultima.
/// Devuelve -1 solo si no hay cola que medir.
///
/// Se mide punta a punta y no promediando razones LOCALES por una razon que
/// destapo el propio arreglo de WD-3.6: cuando la cola decae hasta el silencio
/// numerico, todas las ventanas del final valen cero, cada razon local queda
/// indefinida, y el promedio de razones se vuelve "no medible" — que es
/// exactamente el veredicto que un efecto SANO no deberia recibir. Peor: el
/// barrido de deuda nueva saltea lo no medible, asi que el instrumento se
/// quedaba mudo justo sobre los efectos que mas rapido decaen.
///
/// Con el piso de abajo, una cola que se apaga da una razon cercana a cero —que
/// es decaimiento, y es lo que hay que afirmar— y una que crece la da mayor
/// que 1. El caso "no medible" queda reservado para lo que de verdad no tiene
/// cola.
double tailGrowthRatio(const std::vector<double>& env) {
    if (env.size() < 6) return -1.0;

    /// Piso de silencio. Por debajo de esto la cola ya se apago: lo que importa
    /// es que llego hasta aca, no cuanto mas baja.
    constexpr double kFloor = 1e-9;

    const double first = std::max(env[1], kFloor);
    const double last = std::max(env.back(), kFloor);
    if (env[1] <= kFloor) return -1.0;  // nunca hubo cola: nada que afirmar

    const double steps = static_cast<double>(env.size() - 2);
    return std::pow(last / first, 1.0 / steps);
}

/// La cola de un efecto con los parametros DE FABRICA. Que sea con los defaults
/// es la mitad del punto: un defecto que hace falta buscar con la perilla al
/// tope es una cosa, y uno que se come al que abre el efecto y no toca nada es
/// otra.
double factoryPresetGrowth(EffectRegistry& registry, EffectType type, bool& blewUp) {
    std::unique_ptr<Effect> fx = registry.createEffect(type);
    if (fx == nullptr) { blewUp = false; return -1.0; }
    fx->setSampleRate(kRate);

    const std::vector<double> env = impulseTailEnvelope(*fx);
    blewUp = env.empty();
    return blewUp ? 1e9 : tailGrowthRatio(env);
}

std::set<std::string> readBaseline(const char* path) {
    std::set<std::string> names;
    std::FILE* f = std::fopen(path, "rb");
    EXPECT_NE(f, nullptr) << "no pude abrir " << path;
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
// Primero el instrumento, contra comportamiento conocido.
// ===========================================================================

TEST(LoopStability, TheInstrumentSaysDecayForReverbsThatAreKnownToDecay) {
    // Sin esto, el test de abajo no puede acusar a nadie: un medidor que
    // declarara inestable a un reverb sano estaria midiendo su propio ruido.
    // Los tres estan verdes en toda la suite y los tres tienen cola larga, que
    // es el caso dificil para un detector de crecimiento.
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    for (EffectType type : {REVERB, HALL_REVERB, PLATE_REVERB, DELAY, TAPE_ECHO}) {
        bool blewUp = false;
        const double ratio = factoryPresetGrowth(registry, type, blewUp);
        EXPECT_FALSE(blewUp) << nameOf(type) << " se fue a no-finito";
        EXPECT_GT(ratio, 0.0) << nameOf(type) << ": la cola no se pudo medir";
        EXPECT_LT(ratio, 1.0)
            << nameOf(type) << " creceria segun este instrumento (razon "
            << ratio << " por ventana de " << kWindowSeconds << " s). Antes de "
            << "creerle, revisar el instrumento: estos cinco efectos son la "
            << "referencia de comportamiento sano de este archivo.";
    }
}

TEST(LoopStability, TheSpringStaysBoundedAcrossEveryCombinationOfItsKnobs) {
    // WD-3.6. El criterio pedia la ganancia por debajo de 1 **en todo el rango
    // de las perillas, no solo en los defaults**, y eso no es retorica: el
    // defecto original era estable en el tercio inferior del decay y por eso
    // sobrevivio a todos los barridos que probaban un solo punto.
    //
    // 126 combinaciones. El costo es de segundos porque la cola de un impulso
    // se mide en 8 s de audio y no hace falta señal sostenida.
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    for (float decay : {0.4f, 1.0f, 2.0f, 2.2f, 3.0f, 4.0f, 5.0f}) {
        for (float drip : {0.0f, 0.25f, 0.35f, 0.5f, 0.75f, 1.0f}) {
            for (float tension : {0.0f, 0.5f, 1.0f}) {
                std::unique_ptr<Effect> fx = registry.createEffect(SPRING_REVERB);
                fx->setSampleRate(kRate);
                fx->setParam(0, decay);
                fx->setParam(2, drip);
                fx->setParam(3, tension);

                const std::vector<double> env = impulseTailEnvelope(*fx);
                ASSERT_FALSE(env.empty())
                    << "la cola se fue a no-finito con decay " << decay
                    << ", drip " << drip << ", tension " << tension;

                const double ratio = tailGrowthRatio(env);
                EXPECT_LT(ratio, kGrowthThreshold)
                    << "la cola CRECE (razon " << ratio << ") con decay "
                    << decay << ", drip " << drip << ", tension " << tension
                    << ".\n  El presupuesto de ganancia de lazo de WD-3.6 dejo "
                    << "de acotar. `kTapSum` se deriva de los cuatro pesos, asi "
                    << "que no puede haber quedado stale: mira si aparecio un "
                    << "camino de realimentacion NUEVO que no descuenta del "
                    << "presupuesto — que es exactamente lo que hacia el drip "
                    << "antes de este requerimiento.";
            }
        }
    }
}

TEST(LoopStability, TheDecayKnobStillHasRangeAfterBeingBounded) {
    // La otra mitad, y la que hace que el arreglo sea un arreglo: acotar la
    // ganancia de lazo tiene un fix trivial —bajar el feedback hasta que no
    // explote— que pasa el test de arriba y **destruye el efecto**. Aplanar la
    // mitad superior del recorrido es la version suave del mismo error.
    //
    // Se mide el RT60 y se exige que crezca con la perilla y que tenga rango
    // real. Los numeros salen de la medicion, no de un deseo: 1,0 s en el
    // minimo y 3,0 s en el maximo.
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    auto rt60 = [&](float decay) {
        std::unique_ptr<Effect> fx = registry.createEffect(SPRING_REVERB);
        fx->setSampleRate(kRate);
        fx->setParam(0, decay);
        const std::vector<double> env = impulseTailEnvelope(*fx);
        if (env.size() < 3 || env[1] <= 0.0) return -1.0;
        for (size_t i = 2; i < env.size(); ++i) {
            if (env[i] > 0.0 && 20.0 * std::log10(env[i] / env[1]) <= -60.0) {
                return static_cast<double>(i) * kWindowSeconds;
            }
        }
        return -1.0;  // no llego a -60 dB en la ventana: cola demasiado larga
    };

    const double atMin = rt60(0.4f);
    const double atDefault = rt60(2.2f);
    const double atMax = rt60(5.0f);

    EXPECT_GT(atMin, 0.0) << "el decay minimo no deja cola medible";

    // Se miden TRES puntos y no dos a proposito. Con solo el minimo y el tope,
    // un clamp aplicado al final —en vez de un presupuesto repartido— pasa
    // igual: aplana la mitad SUPERIOR del recorrido y deja el extremo inferior
    // intacto, asi que la comparacion min-contra-max sigue dando bien. El punto
    // del medio es el que lo caza.
    EXPECT_GT(atDefault, atMin)
        << "el RT60 con el decay por defecto (" << atDefault << " s) no supera "
        << "al del minimo (" << atMin << " s).";
    EXPECT_GT(atMax, atDefault)
        << "el RT60 en el tope del decay (" << atMax << " s) no supera al del "
        << "defecto (" << atDefault << " s): la mitad de arriba del recorrido "
        << "quedo aplanada.\n"
        << "  Casi siempre significa que la ganancia se acoto con un clamp al "
        << "final en vez de repartiendo un presupuesto — el clamp satura, y "
        << "todo lo que este por encima del techo suena igual.";
    EXPECT_GE(atMax, 2.5)
        << "el RT60 maximo bajo a " << atMax << " s. El presupuesto de ganancia "
        << "se apreto de mas y el spring perdio su cola larga, que es la razon "
        << "por la que alguien elige un spring.";

    // Y el RANGO util, que es lo ultimo que separa una perilla de un adorno.
    //
    // Esta afirmacion se agrego por un mutante que SOBREVIVIO: acotar con un
    // clamp al final, escalando el total, es estable y ademas conserva la
    // monotonia — o sea que no es un defecto, es un DISEÑO ALTERNATIVO valido, y
    // por eso ninguna de las afirmaciones de arriba podia matarlo. Lo unico que
    // lo separa del elegido es esto: sobre el recorrido completo mueve el RT60
    // de 2,50 a 3,50 s (un factor de 1,4) contra 1,00 a 3,00 del presupuesto
    // repartido (un factor de 3,0).
    //
    // El corte va en 2,0 y no en 3,0 justamente para no atar el test a UNA
    // implementacion: lo que se defiende es que una perilla con recorrido 0,4 a
    // 5,0 —un factor de 12,5— no puede mover la cola apenas un 40 %.
    EXPECT_GE(atMax / atMin, 2.0)
        << "el recorrido completo del decay mueve el RT60 de " << atMin << " s a "
        << atMax << " s, un factor de " << (atMax / atMin) << ". La perilla "
        << "quedo casi inerte: su rango nominal es 0,4 a 5,0, un factor de 12,5.";
}

// ===========================================================================
// El trinquete: los 23 efectos, con los parametros de fabrica.
// ===========================================================================

TEST(LoopStability, NoEffectGrowsWithoutBoundOnItsFactoryPreset) {
    // La mitad "deuda nueva". Un efecto cuyo lazo se pasa de 1 no se nota en
    // ningun otro test de esta suite: los barridos de Nyquist tienen ventanas de
    // decimas de segundo, los golden miden bloques cortos, y el auto-gain de
    // `EffectChain` tapa el sintoma en el motor completo.
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    const std::set<std::string> baseline = readBaseline(WMA_LOOP_STABILITY_BASELINE);
    std::set<std::string> growing;

    for (int id = 0; id < EFFECT_TYPE_COUNT; ++id) {
        const auto type = static_cast<EffectType>(id);
        bool blewUp = false;
        const double ratio = factoryPresetGrowth(registry, type, blewUp);
        if (ratio < 0.0) continue;  // cola inmedible (efecto sin cola): no dice nada

        const bool grows = blewUp || ratio > kGrowthThreshold;
        if (grows) growing.insert(nameOf(type));

        if (baseline.count(nameOf(type)) == 0) {
            EXPECT_FALSE(grows)
                << nameOf(type) << " CRECE sin cota con sus parametros de "
                << "fabrica: la cola de un impulso se multiplica por " << ratio
                << " cada " << kWindowSeconds << " s a " << kRate << " Hz"
                << (blewUp ? " (y se fue a no-finito)" : "") << ", y no esta "
                << "declarado en loop-stability-baseline.txt.\n"
                << "  Esto NO es un defecto de sample rate: se mide a 48 kHz. Es "
                << "un lazo de realimentacion cuya ganancia paso de 1.\n"
                << "  Sintoma de usuario: el efecto satura y despues ENMUDECE, "
                << "cuando su estado interno llega a no-finito y el scrub de "
                << "salida lo convierte en silencio permanente.";
        }
    }
}

TEST(LoopStability, EveryDeclaredCaseStillReproduces) {
    // La mitad "deuda pagada", que es la que evita que este archivo se vuelva
    // una lista de excepciones que nadie borra.
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    const std::set<std::string> baseline = readBaseline(WMA_LOOP_STABILITY_BASELINE);
    std::set<std::string> reproduced;

    for (int id = 0; id < EFFECT_TYPE_COUNT; ++id) {
        const auto type = static_cast<EffectType>(id);
        if (baseline.count(nameOf(type)) == 0) continue;

        bool blewUp = false;
        const double ratio = factoryPresetGrowth(registry, type, blewUp);
        const bool grows = blewUp || ratio > kGrowthThreshold;

        EXPECT_TRUE(grows)
            << nameOf(type) << " YA NO crece con sus parametros de fabrica "
            << "(razon " << ratio << " por ventana).\n"
            << "  Si lo arreglaste: sacalo de loop-stability-baseline.txt, y "
            << "acordate de que este arreglo CAMBIA EL SONIDO del efecto — los "
            << "golden de WD-2.2 hay que recapturarlos a proposito, con "
            << "`scripts/regen-golden.sh`, y revisar el diff.\n"
            << "  Si no lo arreglaste, alguien cambio el efecto y este repro "
            << "dejo de apuntar al defecto: hay que volver a medirlo, no "
            << "borrarlo.";
        if (grows) reproduced.insert(nameOf(type));
    }

    EXPECT_EQ(reproduced, baseline)
        << "lo que reproduce y loop-stability-baseline.txt no coinciden.";
}
