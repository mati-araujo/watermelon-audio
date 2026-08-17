/**
 * WD-2.3.3 — estabilidad numerica en los limites, y el clamp contra Nyquist
 * que HOY NO EXISTE (WD-3.5).
 *
 * LO QUE ESTA SUITE MIDE, Y LA CORRECCION QUE TRAE
 * -----------------------------------------------
 * El requerimiento de WD-3.5 describe asi el escenario de falla: "cutoff a
 * 20 kHz en un device a 44,1 kHz... la salida auto-oscila o produce NaN".
 *
 * **Eso no se reproduce.** Medido: a 44,1 kHz con el cutoff en el tope,
 * omega/pi = 0,907, sin(omega) = +0,288, y la salida se queda en 1,088 de pico,
 * finita y acotada. Los tres rates que nombra WD-2.3 —44,1 / 48 / 96— estan
 * limpios, y el barrido de parametros a los extremos en esos tres esta en
 * `test_rate_invariance.cpp`, verde.
 *
 * El defecto es REAL pero el disparador es otro, y es peor: **cualquier sample
 * rate por debajo de 40 kHz**. La condicion exacta es sin(omega) < 0, o sea
 * cutoff > fs/2, y ahi el alpha del cookbook RBJ se vuelve NEGATIVO, a0 = 1 +
 * alpha se acerca a cero o cambia de signo, y los polos salen del circulo
 * unitario. Barrido de rates con el cutoff en 20 kHz:
 *
 *      8.000 -> finito   (omega = 5,00 pi, sin = 0)
 *     11.025 -> NaN      (omega = 3,63 pi, sin = -0,92)
 *     16.000 -> finito   (omega = 2,50 pi, sin = +1,00)
 *     22.050 -> NaN      (omega = 1,81 pi, sin = -0,55)
 *     32.000 -> NaN      (omega = 1,25 pi, sin = -0,71)
 *     39.000 -> NaN      (omega = 1,03 pi, sin = -0,08)
 *     40.000 -> finito   (omega = 1,00 pi, sin = 0)
 *     44.100 -> finito   (omega = 0,91 pi, sin = +0,29)
 *
 * Los "finitos" de 8.000 y 16.000 no son suerte de diseño: son aliasing que
 * devuelve omega a un semiciclo con seno positivo. Un clamp los cubriria a los
 * dos igual, y sin el, el que la salida sobreviva depende de en que vuelta de
 * 2*pi cayo el cutoff — que no es una propiedad sobre la que se pueda razonar.
 *
 * **32.000 Hz es un sample rate estandar de Android**, y 16.000 el de un
 * headset Bluetooth SCO — que es exactamente el hardware del camino de entrada.
 * O sea que esto no es teorico: es alcanzable con el hardware que ya se
 * soporta, y por eso vale mas que la historia de los 44,1.
 *
 * EL IDIOMA QUE FALTA YA EXISTE CINCO VECES
 * -----------------------------------------
 * `BiquadFilter` (0,49 * fs), `StateVariableFilter` (0,49), `VocoderBank`
 * (0,45), `FDN` (0,45) y `Oversampler` (0,45) SI acotan. Lo que WD-3.5 tiene
 * que hacer no es inventar el clamp: es aplicarlo donde falta.
 *
 * Donde falta, medido con un grep de omega contra `mSampleRate` sobre effects/
 * y dsp/, son dos lugares: `FilterEffect::updateCoefficients()` —el unico con
 * repro— y los tres helpers de biquad de `AmpSimulator`, que usan frecuencias
 * fijas de tone stack (100–4.000 Hz) y necesitarian fs < 8.000 para romperse.
 *
 * OJO CON LEER ESA LISTA COMO UN DIAGNOSTICO COMPLETO. Siete efectos divergen
 * por debajo de 44,1 kHz y solo uno —FILTER— tiene la causa verificada por un
 * test propio. VOCODER esta entre los que fallan **aunque `VocoderBank` si
 * clampee**, y cuatro de los siete divergen con el parametro en CERO, que no es
 * el perfil de un omega pasado de pi. O sea que hay al menos una segunda causa
 * sin identificar, y ponerlos a todos bajo el mismo diagnostico seria inventar
 * lo que no se midio. `nyquist-baseline.txt` separa las dos columnas.
 *
 * EL BASELINE ES UN TRINQUETE
 * ---------------------------
 * `nyquist-baseline.txt` declara los efectos que hoy divergen por debajo de
 * 44,1 kHz. Falla si aparece deuda nueva Y TAMBIEN si una entrada declarada
 * deja de reproducirse — misma semantica que `reset-baseline.txt` y que
 * `scripts/rt-safety-baseline.txt`. El clamp de WD-3.5 retira las dos entradas
 * de FILTER (verificado por mutacion: aplicar `min(valor, 0,45 * fs)` las marca
 * a las dos como deuda pagada). Las otras cinco necesitan diagnostico antes de
 * que nadie pueda decir que las arreglo.
 */

#include "EffectCatalog.h"
#include "RateHarness.h"

#include "../Effect.h"
#include "../EffectRegistry.h"
#include "../EffectTypes.h"
#include "../FilterEffect.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace wma::rate;
using wma::catalog::nameOf;

/// La misma cota que usa `EffectProperties`: separa "amplifica" de "diverge".
constexpr float kSaneBound = 1000.0f;

struct Verdict {
    bool diverged = false;
    bool finite = true;
    float peak = 0.0f;
};

/**
 * Le mete al efecto una onda cuadrada a Nyquist, a fondo de escala.
 *
 * Por que Nyquist y no ruido: un polo que se escapo del circulo unitario esta
 * casi siempre EN la zona alta —es lo que produce omega > pi— y excitarlo ahi
 * hace que la divergencia aparezca en cientos de muestras en vez de en miles.
 * Con ruido de banda ancha el mismo defecto tarda un orden de magnitud mas, y
 * la diferencia es entre un test de cuatro segundos y uno de un minuto.
 */
Verdict hammer(Effect& fx, int blocks, int frames) {
    std::vector<float> in(static_cast<size_t>(frames) * 2);
    std::vector<float> out(in.size(), 0.0f);
    for (size_t i = 0; i < in.size(); ++i) in[i] = (i % 4 < 2) ? 0.95f : -0.95f;

    Verdict v;
    for (int b = 0; b < blocks; ++b) {
        fx.process(in.data(), out.data(), frames);
        for (float s : out) {
            if (!std::isfinite(s)) {
                v.finite = false;
                v.diverged = true;
            } else {
                v.peak = std::max(v.peak, std::abs(s));
                if (v.peak > kSaneBound) v.diverged = true;
            }
        }
        if (v.diverged) break;
    }
    return v;
}

/// Un caso concreto de la deuda: efecto, rate, y el parametro que lo dispara.
struct Case {
    EffectType type;
    int rate;
    int paramId;
    float value;
    int blocks;
    int frames;
    const char* why;
};

/**
 * Los casos MEDIDOS, con el repro minimo de cada uno.
 *
 * No es un barrido: es una tabla, y eso es deliberado. El barrido que descubrio
 * estos casos —4 rates bajos x 6 parametros x 7 valores, con el presupuesto de
 * muestras que los reverbs necesitan para que su divergencia se vea— tardo
 * **297 s** en el build de debug. La tabla corre en 70 ms y ademas DICE cual es
 * el caso, que es lo que le hace falta a quien vaya a arreglarlo.
 *
 * Cada `blocks`/`frames` es el repro MINIMO medido, no un numero redondo: por
 * eso van de 2 bloques (FILTER, que rompe en el primero) a 32 (SPRING_REVERB,
 * que necesita 25). Bajarlos no ahorra nada y subirlos tapa una mejora.
 *
 * El precio de la tabla es que no descubre deuda nueva en otro efecto. Eso lo
 * paga `NoNewDivergenceAppearsBelowFortyKilohertz`, que si barre el catalogo.
 */
const std::vector<Case>& measuredCases() {
    static const std::vector<Case> kCases = {
        {FILTER, 22050, 0, 20000.0f, 2, 512,
         "cutoff 20 kHz sobre 22.050: omega = 1,81 pi, alpha < 0, polos afuera"},
        {FILTER, 32000, 0, 20000.0f, 2, 512,
         "32 kHz es un rate estandar de Android: omega = 1,25 pi"},
        {VOCODER, 16000, 0, 0.0f, 2, 512, "banco de filtros; causa sin localizar"},
        {VOCODER, 22050, 0, 0.0f, 2, 512, "idem, al rate donde tambien cae FILTER"},
        {PHASER, 8000, 0, 0.0f, 2, 512, "cadena de all-pass; causa sin localizar"},
        {HPF_DELAY, 16000, 1, 0.0f, 4, 512,
         "HPF dentro de un lazo de realimentacion; causa sin localizar"},
        {HPF_DELAY, 22050, 1, 0.0f, 4, 512, "el mismo caso, al otro rate"},
        {SPRING_REVERB, 8000, 2, 1.0f, 32, 512,
         "el mas lento de todos: 25 bloques hasta pasar la cota"},
        {PLATE_REVERB, 16000, 1, 0.0f, 2, 512,
         "diverge sin llegar a NaN: medido, alcanza 5,1e37; causa sin localizar"},
        {SHIMMER_REVERB, 16000, 0, 0.0f, 2, 512,
         "comparte el FDN con plate y falla al mismo rate; causa sin localizar"},
    };
    return kCases;
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
// El mecanismo, aislado: sin(omega) es el que decide.
// ===========================================================================

TEST(NyquistLimits, TheFilterSurvivesExactlyWhileTheCutoffStaysBelowHalfTheRate) {
    // Este es el test que le da a WD-3.5 su criterio de aceptacion: no "no
    // explota", que es una consecuencia, sino la PROPIEDAD que la produce.
    //
    // Se mide contra `sin(omega)` y no contra `omega < pi` a proposito: el
    // aliasing devuelve omega a semiciclos de seno positivo (8.000 y 16.000 Hz
    // sobreviven con omega de 5 pi y 2,5 pi), asi que la condicion verdadera es
    // el signo del seno, no la magnitud del angulo. Un clamp contra fs/2 hace
    // que las dos coincidan, y entonces este test pasa a decir lo mismo escrito
    // de forma mas simple.
    const int rates[] = {8000, 11025, 16000, 22050, 24000, 32000, 37000,
                         39000, 40000, 41000, 44100, 48000, 96000};

    for (int rate : rates) {
        FilterEffect fx;
        fx.setSampleRate(rate);
        fx.setParam(0, 20000.0f);
        fx.setParam(1, 0.707f);

        const double omega = 2.0 * wma::golden::kPi * 20000.0 / rate;
        const bool alphaIsPositive = std::sin(omega) > 0.0;
        const Verdict v = hammer(fx, 8, 512);

        if (alphaIsPositive) {
            EXPECT_FALSE(v.diverged)
                << "a " << rate << " Hz el seno de omega es "
                << std::sin(omega) << " (positivo, alpha > 0) y aun asi el "
                << "filtro divergio: pico " << v.peak << ", finito=" << v.finite
                << ".\n  Si esto falla, la causa NO es la que documenta esta "
                << "suite y hay que volver a medir.";
        }
    }
}

TEST(NyquistLimits, TheCutoffIsClampedToTwentyKilohertzNoMatterTheRate) {
    // La causa raiz, escrita como propiedad de la interfaz y no del audio:
    // `setCutoff` acota contra una constante, no contra el rate vigente. Es UNA
    // linea de FilterEffect.cpp, y es toda la deuda de WD-3.5 en este efecto.
    //
    // Cuando WD-3.5 ponga `min(valor, 0,45 * fs)`, este test se pone rojo — y
    // eso es lo correcto: el que lo arregle tiene que venir aca y darlo vuelta.
    constexpr int kRate = 22050;
    FilterEffect fx;
    fx.setSampleRate(kRate);

    // Un valor absurdo para que lo que quede sea EL TOPE, no lo que se pidio.
    fx.setParam(0, 1.0e9f);
    const float ceiling = fx.getParam(0);

    EXPECT_FLOAT_EQ(ceiling, 20000.0f)
        << "el tope del cutoff dejo de ser la constante de 20 kHz (quedo en "
        << ceiling << " Hz). Si es WD-3.5, invertir este test: el tope tiene que "
        << "ser <= 0,45 * fs = " << (kRate * 0.45f) << " Hz, y "
        << "`nyquist-baseline.txt` tiene que quedar vacio.";

    // Y la consecuencia, leida del efecto y no de un literal del test: ese tope
    // esta POR ENCIMA de Nyquist, que es toda la deuda en una linea.
    EXPECT_GT(ceiling, kRate * 0.5f)
        << "el tope del cutoff (" << ceiling << " Hz) ya no supera a Nyquist a "
        << kRate << " Hz. Sea porque se puso el clamp o porque cambio el rango "
        << "de la perilla, el defecto que documenta esta suite dejo de existir "
        << "por este camino — hay que volver a medir antes de tocar nada.";
}

// ===========================================================================
// La deuda declarada, caso por caso.
// ===========================================================================

TEST(NyquistLimits, EveryDeclaredCaseStillReproduces) {
    // La mitad "deuda pagada" del trinquete, con el detalle que un nombre de
    // efecto no puede llevar: el rate, el parametro y el repro minimo.
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    std::set<std::string> reproduced;
    for (const Case& c : measuredCases()) {
        std::unique_ptr<Effect> fx = registry.createEffect(c.type);
        ASSERT_NE(fx, nullptr) << nameOf(c.type) << " no esta registrado";
        fx->setSampleRate(c.rate);
        fx->setParam(c.paramId, c.value);

        const Verdict v = hammer(*fx, c.blocks, c.frames);
        EXPECT_TRUE(v.diverged)
            << nameOf(c.type) << " a " << c.rate << " Hz con el parametro "
            << c.paramId << " en " << c.value << " YA NO diverge (pico "
            << v.peak << ").\n"
            << "  El caso declarado era: " << c.why << "\n"
            << "  Si lo arreglaste, sacalo de measuredCases() y de "
            << "nyquist-baseline.txt. Si no, alguien cambio el efecto y este "
            << "repro dejo de apuntar al defecto — hay que volver a medirlo, "
            << "no borrarlo.";
        if (v.diverged) reproduced.insert(nameOf(c.type));
    }

    const std::set<std::string> baseline = readBaseline(WMA_NYQUIST_BASELINE);
    EXPECT_EQ(reproduced, baseline)
        << "la tabla de casos medidos y nyquist-baseline.txt no coinciden. El "
        << "archivo es lo que se lee en un PR; la tabla es lo que se ejecuta. "
        << "Que difieran es exactamente como un baseline empieza a mentir.";
}

// ===========================================================================
// Y el detector de deuda NUEVA: el catalogo entero a un rate bajo.
// ===========================================================================

TEST(NyquistLimits, NoNewDivergenceAppearsBelowFortyKilohertz) {
    // Barrido acotado a proposito: un solo rate representativo (22.050, donde
    // dos de los efectos declarados ya fallan) y los tres primeros parametros.
    // Cubrir los cuatro rates y los ocho parametros cuesta mas de un minuto en
    // debug y encuentra lo mismo que la tabla de arriba ya declara.
    //
    // Lo que este test agrega es lo que la tabla no puede: si alguien registra
    // un efecto NUEVO que calcula omega contra el rate sin acotar, aparece aca
    // sin que nadie se acuerde de agregarlo a ninguna lista.
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    const std::set<std::string> baseline = readBaseline(WMA_NYQUIST_BASELINE);
    constexpr int kRate = 22050;

    for (int id = 0; id < EFFECT_TYPE_COUNT; ++id) {
        const auto type = static_cast<EffectType>(id);
        if (baseline.count(nameOf(type)) > 0) continue;

        for (int p = 0; p < 3; ++p) {
            for (float v : {0.0f, 20.0f, 20000.0f}) {
                std::unique_ptr<Effect> fx = registry.createEffect(type);
                ASSERT_NE(fx, nullptr);
                fx->setSampleRate(kRate);
                fx->setParam(p, v);

                const Verdict verdict = hammer(*fx, 16, 512);
                EXPECT_FALSE(verdict.diverged)
                    << nameOf(type) << " diverge a " << kRate << " Hz con el "
                    << "parametro " << p << " en " << v << " (pico "
                    << verdict.peak << ", finito=" << verdict.finite << ") y NO "
                    << "esta declarado en nyquist-baseline.txt.\n"
                    << "  Casi siempre es lo mismo: un omega calculado contra el "
                    << "sample rate a partir de una frecuencia acotada contra "
                    << "una CONSTANTE. El idioma que falta ya existe en cinco "
                    << "lugares de la libreria: min(valor, 0,45 * fs).";
            }
        }
    }
}

// El criterio de aceptacion de WD-2.3.3 tal como esta escrito —los 23 efectos
// con los parametros a los extremos, a 44,1 / 48 / 96— lo cubre
// `RateInvariance.EveryEffectStaysFiniteAndBoundedAtEveryRate`, que ademas
// randomiza los DIECISEIS ids de parametro en vez de los tres primeros. Repetir
// el barrido aca costaba 25 s y no agregaba un solo caso.
//
// Ese test es verde, y conviene leerlo por lo que dice: "no diverge EN ESTOS
// TRES RATES". Por que esa afirmacion es mas chica de lo que parece es
// exactamente lo que mide el resto de este archivo.
