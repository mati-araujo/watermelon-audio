/**
 * WD-2.3.2 — el mismo resultado musical a 44,1 / 48 / 96 kHz.
 *
 * QUE AFIRMA ESTA SUITE
 * ---------------------
 * Que las perillas del motor estan en unidades FISICAS y no en muestras. Un
 * cutoff de 2 kHz tiene que caer en 2 kHz, un delay de 250 ms tiene que volver a
 * los 250 ms, y un LFO de 2 Hz tiene que dar dos vueltas por segundo — en el
 * device que negocio 44,1 igual que en el que negocio 96.
 *
 * Es la mitad de la Fase 2 que hacia falta para que WD-3.4 no se pueda degradar:
 * un componente preparado para un rate y corrido a otro se ve ACA, y en ningun
 * otro lado de la suite.
 *
 * POR QUE NO SE COMPARA LA CURVA |H(f)| ENTERA — Y POR QUE ESO SE MIDE
 * -------------------------------------------------------------------
 * Porque no es cierto que sea igual, y un test que lo exigiera estaria pidiendo
 * que el filtro deje de ser un biquad bilineal. La transformada bilineal
 * comprime el eje de frecuencia cerca de Nyquist, asi que la banda de rechazo
 * tiene otra forma a cada rate. Medido sobre un LPF a 1 kHz, 44,1 contra 96:
 *
 *      5 kHz  ->  0,58 dB        15 kHz  ->  7,76 dB
 *     10 kHz  ->  2,69 dB        19 kHz  -> 18,50 dB
 *
 * Lo invariante es el LANDMARK, no la curva. Y para que esa decision no quede
 * como una afirmacion de comentario, `TheStopbandShapeIsNotRateInvariantEither`
 * la MIDE: si algun dia alguien agrega prewarping por banda, ese test se pone
 * rojo y avisa que el landmark ya no es lo unico que se puede exigir.
 *
 * Es el mismo criterio que la exclusion de `RANDOM_RESO` en
 * test_golden_properties.cpp: una exclusion sin cobertura es un punto ciego.
 *
 * LA CUARTA MEDIDA, LA QUE ALCANZA A LOS 23
 * -----------------------------------------
 * Los landmarks solo existen donde hay un landmark: un reverb no tiene "cutoff".
 * Para el catalogo entero se mide el NIVEL de salida (RMS) ante el mismo seno.
 * Es una propiedad debil —dice que suena igual de fuerte, no que suene igual—
 * pero es la unica que aplica a los 23 y basta para cachar el defecto de la
 * clase de WD-3.4: cinco efectos entregan hasta 5,9 dB de diferencia segun el
 * rate, y estan declarados en `rate-invariance-baseline.txt`.
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
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace wma::rate;
using wma::catalog::nameOf;
using wma::golden::captureImpulseResponse;
using wma::golden::responseDbAt;

/// Ventana de captura de IR, en SEGUNDOS. 170 ms es lo que usa la suite golden
/// a 48 kHz; expresado en tiempo da la misma cola a los tres rates, que es la
/// condicion para que las tres DFT sean comparables.
constexpr double kIrSeconds = 0.17;

/// Tolerancia del landmark, como fraccion. Medido: la dispersion real va de
/// 0,0007 % a 0,095 % en todo el rango util. 0,5 % deja cinco veces de margen
/// sobre el peor caso y sigue siendo veinte veces mas fino que un semitono
/// (5,9 %), que es la unidad en la que un corrimiento se vuelve audible.
constexpr double kCornerTolerance = 0.005;

/// Tolerancia de nivel entre rates. Medido: 18 de los 23 quedan por debajo de
/// 0,25 dB y 16 por debajo de 0,02. Los cinco que se pasan lo hacen por entre
/// 0,94 y 5,87 dB — no hay nada en la zona gris, asi que el corte no es
/// arbitrario: separa dos poblaciones que la medicion ya dejo separadas.
constexpr double kLevelToleranceDb = 0.5;

/// Un `FilterEffect` configurado, al rate pedido. Los tres parametros van
/// SIEMPRE en el mismo orden porque `setCutoff` recalcula coeficientes: fijar el
/// rate al final dejaria los coeficientes del rate anterior.
std::vector<float> filterIr(int rate, int typeId, float cutoffHz, float q) {
    FilterEffect fx;
    fx.setSampleRate(rate);
    fx.setParam(2, static_cast<float>(typeId));
    fx.setParam(0, cutoffHz);
    fx.setParam(1, q);
    return captureImpulseResponse(fx, framesFor(kIrSeconds, rate), 512);
}

/**
 * Efectos cuyo nivel de salida NO se puede medir de forma reproducible.
 *
 * `RANDOM_RESO` sortea un LFO `RANDOM_SMOOTH`, y `LFO::randomFloat()` siembra su
 * generador con `std::random_device`: dos instancias recien construidas ya
 * difieren, sin que nada las haya tocado. Ya estaba excluido del test bit a bit
 * de WD-3.2 por lo mismo.
 *
 * 🔴 EL CRITERIO ES CONTRA LA TOLERANCIA DEL BARRIDO, NO CONTRA LA DIFERENCIA
 * ENTRE RATES. Un efecto queda fuera cuando su dispersion corrida-a-corrida es
 * comparable a los 0,5 dB que este barrido tolera — no cuando es comparable a su
 * propio movimiento entre rates.
 *
 * La distincion no es academica: `TAPE_ECHO` estuvo excluido de aca por el
 * criterio equivocado (REQ-005 S1). La premisa escrita era CIERTA —su dispersion
 * de 0,0515 dB se come los 0,0517 dB que separan sus rates— pero el barrido no
 * pide RESOLVER esa diferencia: pide que el nivel no se mueva mas de 0,5 dB, y
 * eso se contesta con 15,3 sigma de margen. Era un criterio de resolucion
 * aplicado a un test de umbral, y dejaba fuera del catalogo a un efecto que si
 * se podia medir. `RANDOM_RESO` se queda afuera con el mismo criterio nuevo, y
 * el numero es holgado: 16,0 dB contra 0,5.
 */
const std::set<std::string>& nonDeterministicLevel() {
    static const std::set<std::string> kNames = {"RANDOM_RESO"};
    return kNames;
}

/// Lee un baseline con formato `NOMBRE | ... | ...`; `#` y vacias se ignoran.
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
// 1 — HERTZ. El corte del filtro cae donde dice la perilla, a cualquier rate.
// ===========================================================================

TEST(RateInvariance, LowpassCornerIsInHertzAtEveryRate) {
    // El landmark: la frecuencia donde la respuesta cae 3 dB por debajo de su
    // pico. Se mide sobre la IR que `process()` produjo, no sobre los
    // coeficientes — un error en el calculo de coeficientes aparece identico de
    // los dos lados y dejaria el test verde (la leccion de WD-2.2).
    for (float fc : {50.0f, 100.0f, 500.0f, 2000.0f, 8000.0f, 18000.0f}) {
        double corner[3];
        for (int i = 0; i < 3; ++i) {
            corner[i] = cornerFrequency(filterIr(kRates[i], 0, fc, 0.707f), kRates[i], false);
        }

        EXPECT_LT(relativeSpread(corner), kCornerTolerance)
            << "un LPF con el cutoff en " << fc << " Hz corta en frecuencias "
            << "distintas segun el sample rate:\n"
            << "  44,1 kHz -> " << corner[0] << " Hz\n"
            << "  48 kHz   -> " << corner[1] << " Hz\n"
            << "  96 kHz   -> " << corner[2] << " Hz\n"
            << "El cutoff es una magnitud en HERTZ: si se corre con el rate, hay "
            << "un omega calculado contra un sample rate que no es el vigente.";
    }
}

TEST(RateInvariance, HighpassCornerIsInHertzWhileItIsFarFromNyquist) {
    // Hasta 2 kHz la dispersion medida es 0,0018 %. Mas arriba deja de valer, y
    // eso NO es un defecto: lo mide el test de abajo.
    for (float fc : {50.0f, 100.0f, 500.0f, 2000.0f}) {
        double corner[3];
        for (int i = 0; i < 3; ++i) {
            corner[i] = cornerFrequency(filterIr(kRates[i], 1, fc, 0.707f), kRates[i], true);
        }

        EXPECT_LT(relativeSpread(corner), kCornerTolerance)
            << "un HPF con el cutoff en " << fc << " Hz corta en " << corner[0]
            << " / " << corner[1] << " / " << corner[2] << " Hz segun el rate.";
    }
}

// ===========================================================================
// Las dos exclusiones, MEDIDAS. Sin estos dos tests, las decisiones de arriba
// —landmark en vez de curva, HPF solo hasta 2 kHz— serian afirmaciones de
// comentario, y un comentario no se pone rojo cuando deja de ser cierto.
// ===========================================================================

TEST(RateInvariance, NearNyquistTheCornerIsNotRateInvariantAndThatIsTheDesignMethod) {
    // Un HPF RBJ ubica su POLO en omega0, y el -3 dB coincide con omega0 solo
    // mientras omega0 << pi. A 18 kHz sobre 44,1 kHz, omega0/pi = 0,816: el
    // polo esta tan cerca de Nyquist que el -3 dB ya no cae donde se pidio, y
    // cae distinto a cada rate.
    //
    // Este test existe para que el limite de 2 kHz del test anterior sea un
    // hecho medido y no una precaucion. Si alguien agrega prewarping y esto se
    // pone verde, la accion correcta es SUBIR el limite del test de arriba —
    // que es mas fuerte— y borrar este.
    double corner[3];
    for (int i = 0; i < 3; ++i) {
        corner[i] = cornerFrequency(filterIr(kRates[i], 1, 18000.0f, 0.707f), kRates[i], true);
    }

    EXPECT_GT(relativeSpread(corner), 0.05)
        << "un HPF a 18 kHz dio el mismo corte a los tres rates ("
        << corner[0] << " / " << corner[1] << " / " << corner[2] << " Hz).\n"
        << "Medido al escribir este test, la dispersion era del 8,97 %: el "
        << "warping bilineal cerca de Nyquist. Si desaparecio, el diseño del "
        << "filtro cambio — y entonces HighpassCornerIsInHertz... puede y debe "
        << "cubrir tambien los cutoffs altos.";
}

TEST(RateInvariance, TheStopbandShapeIsNotRateInvariantEither) {
    // La razon por la que esta suite mide landmarks y no compara curvas.
    const std::vector<float> ir44 = filterIr(kRates[0], 0, 1000.0f, 0.707f);
    const std::vector<float> ir96 = filterIr(kRates[2], 0, 1000.0f, 0.707f);

    const double near = std::abs(responseDbAt(ir44, 5000.0, kRates[0]) -
                                 responseDbAt(ir96, 5000.0, kRates[2]));
    const double far = std::abs(responseDbAt(ir44, 19000.0, kRates[0]) -
                                responseDbAt(ir96, 19000.0, kRates[2]));

    EXPECT_LT(near, 1.5) << "cerca del codo la respuesta si coincide entre rates";
    EXPECT_GT(far, 10.0)
        << "a 19 kHz el LPF a 1 kHz dio la misma atenuacion a 44,1 y a 96 kHz "
        << "(delta " << far << " dB).\n"
        << "Medido: 18,50 dB. Ese hueco es el warping bilineal, y es la razon "
        << "por la que esta suite compara LANDMARKS y no curvas enteras. Si "
        << "desaparecio, comparar la curva completa paso a ser legitimo y esta "
        << "suite se puede volver mucho mas fuerte.";
}

// ===========================================================================
// 2 — MILISEGUNDOS. El eco vuelve cuando dice la perilla.
// ===========================================================================

TEST(RateInvariance, DelayEchoLandsAtTheSameMillisecondAtEveryRate) {
    // Es el defecto de WD-3.4 escrito en el dominio del tiempo: una linea de
    // retardo dimensionada en SAMPLES contra un rate equivocado suena a otro
    // tempo. Con 250 ms declarados y el device a 44,1, un delay preparado para
    // 48 volveria a los 272 ms — 9 % tarde, que a 120 BPM es media corchea.
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    for (EffectType type : {DELAY, HPF_DELAY}) {
        double ms[3];
        for (int i = 0; i < 3; ++i) {
            std::unique_ptr<Effect> fx = registry.createEffect(type);
            ASSERT_NE(fx, nullptr);
            fx->setSampleRate(kRates[i]);

            const std::vector<float> ir =
                captureImpulseResponse(*fx, framesFor(1.2, kRates[i]), 512);
            // Saltear 20 ms de directo: lo que se busca es el PRIMER ECO, no la
            // señal que salio en el sample 0.
            const int peak = peakFrameAfter(ir, framesFor(0.02, kRates[i]));
            ASSERT_GE(peak, 0) << nameOf(type) << " no produjo eco a " << rateName(kRates[i]);
            ms[i] = 1000.0 * peak / kRates[i];
        }

        EXPECT_LT(relativeSpread(ms), 0.002)
            << nameOf(type) << " devuelve el eco en " << ms[0] << " / " << ms[1]
            << " / " << ms[2] << " ms segun el rate.\n"
            << "El tiempo de delay es una magnitud en MILISEGUNDOS. Si cambia "
            << "con el rate, la linea se dimensiono en samples contra un rate "
            << "que no es el vigente — y el delay queda desafinado del tempo.";
    }
}

// ===========================================================================
// 3 — HERTZ LENTOS. El LFO da las mismas vueltas por segundo.
// ===========================================================================

TEST(RateInvariance, LfoCycleIsInHertzAtEveryRate) {
    // Solo los efectos que modulan AMPLITUD entran aca, y no por comodidad: el
    // instrumento cuenta cruces de la envolvente, asi que un LFO que modula
    // retardo (CHORUS) o fase (PHASER) no deja huella medible en ella. Medido:
    // con continua a la entrada, esos dos no completan ni un cruce en 4 s.
    // Cubrirlos necesita otro instrumento, y eso es otra tanda — decirlo es
    // mejor que fingir cobertura con un test que no puede fallar.
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    for (EffectType type : {AUTO_PAN, COMPLEX_TREM}) {
        constexpr double kSeconds = 4.0;
        double period[3];
        for (int i = 0; i < 3; ++i) {
            std::unique_ptr<Effect> fx = registry.createEffect(type);
            ASSERT_NE(fx, nullptr);
            fx->setSampleRate(kRates[i]);

            const std::vector<float> out =
                runBlocks(*fx, dcStereo(kSeconds, kRates[i], 0.5f));
            period[i] = modulationPeriodSeconds(out, kRates[i], kSeconds);
            ASSERT_GT(period[i], 0.0)
                << nameOf(type) << " no completo un ciclo de modulacion en "
                << kSeconds << " s a " << rateName(kRates[i]);
        }

        // 0,02 no es una tolerancia de verdad, y conviene saberlo: el
        // instrumento cuenta cruces enteros, asi que su resolucion es
        // 1/crossings — con los 16 cruces que da AUTO_PAN en 4 s, un cruce de
        // mas o de menos ya son 6,25 %. O sea que esto exige, en los hechos,
        // EXACTAMENTE la misma cuenta de cruces a los tres rates. Es estricto a
        // proposito: la medida es determinista (medido, 0,0000 de dispersion
        // entre corridas), asi que exigir igualdad no introduce intermitencia.
        EXPECT_LT(relativeSpread(period), 0.02)
            << nameOf(type) << " modula con periodo " << period[0] << " / "
            << period[1] << " / " << period[2] << " s segun el rate.\n"
            << "La velocidad de un LFO es una magnitud en HERTZ: si el ciclo "
            << "dura mas a 96 kHz, el incremento de fase esta en samples y no "
            << "en tiempo.";
    }
}

// ===========================================================================
// 4 — DECIBELES. Lo unico exigible a los 23, y el trinquete que lo sostiene.
// ===========================================================================

TEST(RateInvariance, EveryEffectDeliversTheSameLevelAtEveryRate) {
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    const std::set<std::string> baseline = readBaseline(WMA_RATE_BASELINE);
    std::set<std::string> failing;

    for (int id = 0; id < EFFECT_TYPE_COUNT; ++id) {
        const auto type = static_cast<EffectType>(id);
        if (nonDeterministicLevel().count(nameOf(type)) > 0) continue;

        double rms[3];
        for (int i = 0; i < 3; ++i) {
            std::unique_ptr<Effect> fx = registry.createEffect(type);
            ASSERT_NE(fx, nullptr) << nameOf(type) << " no esta registrado";
            fx->setSampleRate(kRates[i]);

            // Un seno de 440 Hz: en la banda donde todos los efectos hacen
            // algo, y lejos de Nyquist a los tres rates.
            const std::vector<float> out =
                runBlocks(*fx, sineStereo(440.0, 2.0, kRates[i], 0.5f));
            // Medio segundo de descarte: los smoothers de parametro de esta
            // libreria se asientan en decenas de ms, y el sobrante es margen.
            rms[i] = rmsLeft(out, kRates[i], 0.5);
            ASSERT_GT(rms[i], 0.0)
                << nameOf(type) << " produjo silencio o no-finitos a "
                << rateName(kRates[i]);
        }

        if (spreadDb(rms) > kLevelToleranceDb) failing.insert(nameOf(type));
    }

    // --- el trinquete, con la misma semantica que reset-baseline.txt --------
    for (const std::string& name : failing) {
        EXPECT_TRUE(baseline.count(name) > 0)
            << name << " entrega niveles distintos segun el sample rate y NO "
            << "esta en rate-invariance-baseline.txt.\n"
            << "  El mismo seno de 440 Hz sale con mas de " << kLevelToleranceDb
            << " dB de diferencia entre 44,1 y 96 kHz.\n"
            << "  Casi siempre significa lo mismo: hay estado interno "
            << "dimensionado en SAMPLES contra un rate fijo — una linea de "
            << "retardo, un coeficiente de filtro, un largo de IR.\n"
            << "  Arreglalo, o declaralo en el baseline con quien lo saca.";
    }

    for (const std::string& name : baseline) {
        EXPECT_TRUE(failing.count(name) > 0)
            << name << " esta en rate-invariance-baseline.txt pero YA entrega "
            << "el mismo nivel a los tres rates.\n"
            << "  Si lo arreglaste, borra su linea: el trinquete existe para "
            << "que la deuda declarada sea la deuda real.";
    }

    EXPECT_EQ(failing.size(), baseline.size())
        << "fallan " << failing.size() << " efectos; el baseline declara "
        << baseline.size();
}

// ===========================================================================
// 5 — Y lo que vale a cualquier rate: nada explota.
// ===========================================================================

TEST(RateInvariance, EveryEffectStaysFiniteAndBoundedAtEveryRate) {
    // `EffectProperties.EveryEffectStaysFiniteAndBoundedUnderRandomParams` hace
    // este barrido a 48 kHz. Esto lo repite a los tres rates del requerimiento,
    // que es donde WD-2.3.3 pone la vara.
    //
    // OJO CON LEER ESTE VERDE DE MAS: verde aca significa "a 44,1 / 48 / 96 no
    // explota", y nada mas. `test_nyquist_limits.cpp` mide que pasa POR DEBAJO
    // de 44,1 — y ahi si hay seis efectos que producen NaN.
    EffectRegistry registry;
    registerBuiltinEffects(registry);

    constexpr int kTrials = 8;
    constexpr int kBlock = 512;
    constexpr float kSaneBound = 1000.0f;

    for (int id = 0; id < EFFECT_TYPE_COUNT; ++id) {
        const auto type = static_cast<EffectType>(id);
        for (int i = 0; i < 3; ++i) {
            for (int trial = 0; trial < kTrials; ++trial) {
                std::mt19937 rng(0x2D23u + static_cast<unsigned>(id) * 1009u +
                                 static_cast<unsigned>(i) * 101u +
                                 static_cast<unsigned>(trial));
                std::uniform_real_distribution<float> pick(-2.0f, 2.0f);
                std::uniform_real_distribution<float> scale(0.0f, 1.0f);

                std::unique_ptr<Effect> fx = registry.createEffect(type);
                ASSERT_NE(fx, nullptr);
                fx->setSampleRate(kRates[i]);
                for (int p = 0; p < 16; ++p) {
                    const float s = scale(rng);
                    const float v = s < 0.34f ? pick(rng)
                                  : (s < 0.67f ? pick(rng) * 60.0f
                                               : std::abs(pick(rng)) * 12000.0f);
                    fx->setParam(p, v);
                }

                std::vector<float> in(static_cast<size_t>(kBlock) * 2);
                std::vector<float> out(in.size(), 0.0f);
                std::uniform_real_distribution<float> noise(-0.8f, 0.8f);
                float worst = 0.0f;
                bool finite = true;
                for (int b = 0; b < 8; ++b) {
                    for (auto& s : in) s = noise(rng);
                    fx->process(in.data(), out.data(), kBlock);
                    for (float v : out) {
                        if (!std::isfinite(v)) finite = false;
                        else worst = std::max(worst, std::abs(v));
                    }
                }

                ASSERT_TRUE(finite)
                    << nameOf(type) << " produjo NaN o infinito a "
                    << rateName(kRates[i]) << " (intento " << trial << ")";
                ASSERT_LT(worst, kSaneBound)
                    << nameOf(type) << " divergio a " << rateName(kRates[i])
                    << " (intento " << trial << "): pico " << worst;
            }
        }
    }
}
