/**
 * WD-2.3.3 / WD-3.5 — estabilidad numerica en los limites, y el clamp contra
 * Nyquist que AHORA SI EXISTE.
 *
 * QUE MIDE ESTA SUITE
 * -------------------
 * Que ningun efecto se escape del circulo unitario cuando el device negocia un
 * sample rate por debajo de 44,1 kHz. **32.000 Hz es un rate estandar de
 * Android** y 16.000 el de un headset Bluetooth SCO, que es exactamente el
 * hardware del camino de entrada: esto no es teorico.
 *
 * LA CONDICION, QUE NO ES "omega < pi"
 * ------------------------------------
 * Es `sin(omega) < 0`. El aliasing devuelve omega a semiciclos de seno positivo
 * —a 8.000 Hz un cutoff de 20 kHz da omega = 5 pi y sobrevive— asi que la
 * magnitud del angulo no alcanza para decidir; el signo del seno si. Con el
 * clamp puesto las dos formulaciones coinciden, y por eso los tests de abajo
 * pueden escribirse de la forma simple.
 *
 * LAS SIETE CAUSAS, QUE RESULTARON SER TRES
 * -----------------------------------------
 * `nyquist-baseline.txt` declaraba siete efectos y una sola causa verificada.
 * Diagnosticadas las otras seis, quedaron tres mecanismos:
 *
 *   1. FILTER — `setCutoff` acota contra la CONSTANTE de 20 kHz y
 *      `updateCoefficients` deriva omega contra el rate vigente.
 *
 *   2. VOCODER, HPF_DELAY, PLATE_REVERB y SHIMMER_REVERB — los CUATRO por la
 *      misma linea, y en ninguno se veia desde el efecto: los cuatro llaman a un
 *      setter de `BiquadFilter` que SI clampea, en el constructor, a 48 kHz, y
 *      despues cambian el rate. `setSampleRate()` recalculaba los coeficientes
 *      sin volver a aplicar `clampFrequency()`. **La deuda estaba en el
 *      primitivo que comparten**, igual que en WD-3.2 con `FDN::reset()`.
 *      El contrato del primitivo lo cubre `dsp/tests/test_biquad_filter.cpp`.
 *
 *   3. PHASER — su all-pass de primer orden no pasa por `BiquadFilter`: el
 *      coeficiente sale de (tan(wc) - 1) / (tan(wc) + 1) y |coef| pasa de 1 en
 *      cuanto modFreq supera fs/2.
 *
 * Los bordes se PREDIJERON antes de medirlos, y cayeron exactos: 24.000 para
 * VOCODER y HPF_DELAY (LPF de 12 kHz), 18.000 para PLATE (9 kHz), 21.000 para
 * SHIMMER (10,5 kHz) y 8.560 para PHASER (2 x 4.280, el tope del barrido del
 * LFO con el depth por defecto). Un borde que cae donde la hipotesis dijo es lo
 * que separa una causa localizada de una coincidencia.
 *
 * Y LA SEPTIMA NO ERA DE ACA
 * --------------------------
 * `SPRING_REVERB` diverge a los SIETE rates medidos, 44,1 / 48 / 96 incluidos,
 * y en el mismo tiempo. Aparecia solo a 8.000 Hz porque la ventana del test iba
 * en BLOQUES: 32 bloques de 512 son 2,0 s a 8 kHz y 0,34 s a 48 kHz, asi que el
 * unico rate que llegaba a la cota era el mas bajo. **Un baseline indexado por
 * sample rate atribuye al sample rate.** Vive en `test_spring_stability.cpp`.
 *
 * EL BASELINE SIGUE SIENDO UN TRINQUETE
 * -------------------------------------
 * `nyquist-baseline.txt` esta VACIO desde WD-3.5, y vacio hace mas trabajo que
 * lleno: falla si aparece deuda nueva. Misma semantica que `reset-baseline.txt`
 * y que `scripts/rt-safety-baseline.txt`.
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
 * Los casos medidos que TODAVIA reproducen.
 *
 * VACIO DESDE WD-3.5, y la lista sigue existiendo por lo mismo que
 * `reset-baseline.txt` sigue existiendo vacio: el trinquete tiene dos mitades, y
 * la que dice "deuda pagada" no puede funcionar si la estructura se borra al
 * quedarse sin entradas. La proxima entrada que aparezca aca viene con su rate,
 * su parametro y su repro MINIMO en bloques — no con un numero redondo.
 *
 * Lo que tenia, para que se entienda que se borro: diez casos sobre siete
 * efectos, de 2 bloques (FILTER, que rompia en el primero) a 32
 * (SPRING_REVERB). Los nueve de Nyquist se pagaron en WD-3.5; el decimo era
 * SPRING, que nunca fue de Nyquist.
 */
const std::vector<Case>& measuredCases() {
    static const std::vector<Case> kCases = {};
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

        // El omega que SALDRIA sin clamp, para que el mensaje de error diga
        // cuanto margen habia. Con el clamp puesto, el que se usa es otro.
        const double omegaUnclamped = 2.0 * wma::golden::kPi * 20000.0 / rate;
        const Verdict v = hammer(fx, 8, 512);

        // WD-3.5 dio vuelta la afirmacion. Antes decia "si sin(omega) > 0
        // entonces no diverge", que dejaba SIN AFIRMAR justo los rates donde
        // estaba la deuda — un condicional cuyo antecedente era falso
        // exactamente en los casos rotos. Ahora se afirma sin condicion, para
        // todos los rates, que es lo que el clamp hace cierto.
        EXPECT_FALSE(v.diverged)
            << "el filtro diverge a " << rate << " Hz con el cutoff pedido en "
            << "20 kHz (pico " << v.peak << ", finito=" << v.finite << ").\n"
            << "  Sin el clamp de WD-3.5, omega valdria " << (omegaUnclamped / wma::golden::kPi)
            << " pi con sin(omega) = " << std::sin(omegaUnclamped) << ": el alpha "
            << "del cookbook RBJ cambia de signo cuando ese seno es negativo y "
            << "los polos salen del circulo unitario.\n"
            << "  `FilterEffect::updateCoefficients()` dejo de acotar el cutoff "
            << "contra 0,45 * fs, y eso reabre las dos entradas de FILTER de "
            << "nyquist-baseline.txt.";
    }
}

TEST(NyquistLimits, TheRequestedCutoffIsRememberedButTheEffectiveOneStaysBelowNyquist) {
    // Este test estaba escrito al reves porque el defecto todavia existia: decia
    // "el tope es la constante de 20 kHz" y "ese tope supera a Nyquist", y
    // pedia explicitamente darlo vuelta al arreglarlo. Esto es el vuelta.
    //
    // Las dos mitades del contrato de WD-3.5 en este efecto, y la segunda es la
    // que decide el diseño:
    //
    //   1. La frecuencia EFECTIVA —la que arma los coeficientes— nunca pasa de
    //      0,45 * fs, a ningun rate.
    //   2. La PEDIDA se recuerda igual. El clamp se aplica a lo que se usa, no a
    //      lo que se guardo: acotar en su lugar dejaria la perilla degradada
    //      para siempre despues de que el device pase una vez por 16 kHz.
    //
    // Las dos se miden por comportamiento (el codo de -3 dB de la respuesta) y
    // no leyendo un miembro. `getParam` es la unica excepcion, y justamente
    // porque lo que ahi se afirma ES la interfaz.
    constexpr int kLowRate = 22050;

    FilterEffect fx;
    fx.setSampleRate(kLowRate);
    fx.setParam(0, 20000.0f);
    fx.setParam(1, 0.707f);
    fx.setParam(2, 0.0f);  // LPF

    // (1) El codo se planta en el techo, no en lo que se pidio.
    const double cornerLow =
        cornerFrequency(wma::golden::captureImpulseResponse(fx, 16384), kLowRate, false);
    EXPECT_LT(cornerLow, kLowRate * 0.5)
        << "con el cutoff pedido en 20 kHz y el rate en " << kLowRate << ", el "
        << "codo de -3 dB quedo en " << cornerLow << " Hz, POR ENCIMA de "
        << "Nyquist (" << (kLowRate * 0.5) << "). El clamp de "
        << "`updateCoefficients()` dejo de aplicarse.";

    // (2) Y la perilla no se toco: sigue diciendo lo que el usuario pidio.
    EXPECT_FLOAT_EQ(fx.getParam(0), 20000.0f)
        << "el cutoff pedido dejo de recordarse (quedo en " << fx.getParam(0)
        << " Hz). Si el clamp se volvio destructivo, un device que negocie "
        << "16 kHz una sola vez deja el filtro degradado para siempre, y el "
        << "usuario no tiene forma de recuperar lo que ya habia pedido.";

    // (3) La consecuencia observable de (2): volver a un rate alto RECUPERA el
    // filtro. Es lo unico que distingue "acoto lo que usa" de "acoto lo que
    // guarda", y sin esta parte las dos implementaciones pasan igual.
    fx.setSampleRate(96000);
    const double cornerHigh =
        cornerFrequency(wma::golden::captureImpulseResponse(fx, 16384), 96000, false);
    EXPECT_GT(cornerHigh, 18000.0)
        << "despues de volver a 96 kHz el codo quedo en " << cornerHigh
        << " Hz en vez de cerca de los 20.000 que se habian pedido. El filtro "
        << "no se recupero del paso por " << kLowRate << " Hz: el clamp esta "
        << "pisando el valor guardado.";
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
    // Antes de WD-3.5 este barrido corria a UN solo rate (22.050) porque los
    // otros los tapaban los siete efectos declarados. Pagada esa deuda, cubre
    // los cuatro rates bajos que importan: 8.000 y 16.000 son los de un headset
    // Bluetooth SCO, 22.050 la mitad de CD y 32.000 un rate estandar de Android.
    //
    // La ventana se queda corta A PROPOSITO, y ahora se sabe por que alcanza:
    // una divergencia por omega pasado de pi aparece en el PRIMER bloque —
    // medido, entre 0,03 y 0,06 s en los seis casos que habia. Lo que una
    // ventana corta NO ve es la otra clase de divergencia, la del lazo con
    // ganancia > 1, que tarda segundos y no depende del rate; esa la caza
    // `test_loop_stability.cpp`, que por eso mide en segundos y no en bloques.
    //
    // Lo que este test agrega y la tabla no puede: si alguien registra un efecto
    // NUEVO que calcula omega contra el rate sin acotar, aparece aca sin que
    // nadie se acuerde de agregarlo a ninguna lista.
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    const std::set<std::string> baseline = readBaseline(WMA_NYQUIST_BASELINE);

    for (int kRate : {8000, 16000, 22050, 32000}) {
    for (int id = 0; id < EFFECT_TYPE_COUNT; ++id) {
        const auto type = static_cast<EffectType>(id);
        if (baseline.count(nameOf(type)) > 0) continue;

        // SPRING_REVERB queda afuera de ESTE barrido y solo de este: su
        // divergencia no es de Nyquist —ocurre igual a 96 kHz— y meterla aca la
        // volveria a atribuir al sample rate, que es el error que este archivo
        // acaba de corregir. La exclusion no es un comentario: esta MEDIDA por
        // `LoopStability.EveryDeclaredCaseStillReproduces`, y el dia que WD-3.6
        // la pague, ese test se pone rojo.
        if (type == SPRING_REVERB) continue;

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
                    << "una CONSTANTE. El idioma que falta ya existe en seis "
                    << "lugares de la libreria: min(valor, 0,45 * fs) — y desde "
                    << "WD-3.5, tambien en `BiquadFilter::setSampleRate()`, que "
                    << "es el que cubre a cualquiera que configure sus filtros "
                    << "en el constructor.";
            }
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
