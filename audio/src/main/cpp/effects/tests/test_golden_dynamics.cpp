/**
 * WD-2.2 — curva de transferencia de la dinamica: compresor y limiter.
 *
 * QUE SE MIDE, Y POR QUE CON CONTINUA
 * -----------------------------------
 * La curva estatica de un compresor es una relacion dB de entrada -> dB de
 * salida que sus parametros determinan por completo: por debajo del threshold
 * es 1:1, por encima la pendiente es 1/ratio, y la rodilla suaviza el codo.
 * Todo eso son numeros exactos, no impresiones.
 *
 * Para aislarla hay que darle al detector un nivel CONSTANTE. Y aca eso
 * significa continua, no un seno: `CompressorEffect` detecta el PICO
 * INSTANTANEO del par estereo, sample a sample, sin promediar. Con un seno de
 * 1 kHz ese pico recorre todo el ciclo cuarenta y ocho veces por bloque, el
 * envelope persigue un objetivo que oscila, y lo que se mide termina siendo el
 * comportamiento del seguidor —attack, release, frecuencia— mezclado con la
 * curva. Con un escalon de continua el objetivo es constante, el envelope
 * converge, y queda la curva sola.
 *
 * No es una comodidad del test: es la unica forma de separar la curva estatica
 * del seguidor con ESTE detector. El comportamiento dinamico (attack/release)
 * es otra medicion, y va aparte.
 *
 * EL LIMITER
 * ----------
 * `LookaheadLimiter` no esta en `EffectRegistry` — vive en `OutputStage`, en el
 * bus master. Por eso no lo alcanza `test_effect_latency.cpp` ni el barrido
 * property-based, y por eso hay que medirlo aca a mano. Su contrato es una sola
 * cosa: que la salida no pase del techo. Y tiene 5 ms de lookahead, que son
 * latencia real sobre la señal directa.
 */

#include "GoldenHarness.h"

#include "../CompressorEffect.h"
#include "../LookaheadLimiter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

using namespace wma::golden;

/// Frames por escalon. Con release de 10 ms el envelope converge en ~5.500
/// samples; 9.600 (200 ms) deja margen de sobra y se mide en el ultimo frame.
constexpr int kStepFrames = 9600;

double dbToLin(double db) { return std::pow(10.0, db / 20.0); }

/**
 * Corre un escalon de continua al nivel pedido y devuelve el nivel de SALIDA en
 * dB, medido cuando el envelope ya convergio.
 */
double stepLevelDb(Effect& fx, double inputDb) {
    const float amp = static_cast<float>(dbToLin(inputDb));
    std::vector<float> in(static_cast<size_t>(kStepFrames) * 2, amp);
    std::vector<float> out(static_cast<size_t>(kStepFrames) * 2, 0.0f);
    fx.process(in.data(), out.data(), kStepFrames);
    return toDb(std::abs(static_cast<double>(out[static_cast<size_t>(kStepFrames) * 2 - 2])));
}

/// Un compresor con la curva bajo prueba y el seguidor lo mas rapido posible,
/// para que lo que se mida sea la curva y no el transitorio.
std::unique_ptr<CompressorEffect> makeComp(float thresholdDb, float ratio,
                                           float kneeDb, float makeupDb) {
    auto fx = std::make_unique<CompressorEffect>();
    fx->setSampleRate(kSampleRate);
    fx->setParam(CompressorEffect::THRESHOLD, thresholdDb);
    fx->setParam(CompressorEffect::RATIO, ratio);
    fx->setParam(CompressorEffect::KNEE, kneeDb);
    fx->setParam(CompressorEffect::MAKEUP_GAIN, makeupDb);
    fx->setParam(CompressorEffect::ATTACK, 0.1f);
    fx->setParam(CompressorEffect::RELEASE, 10.0f);
    return fx;
}

/// La curva teorica de rodilla dura.
double expectedHardKneeDb(double inputDb, double thresholdDb, double ratio,
                          double makeupDb) {
    const double compressed = (inputDb <= thresholdDb)
                                  ? inputDb
                                  : thresholdDb + (inputDb - thresholdDb) / ratio;
    return compressed + makeupDb;
}

}  // namespace

// ===========================================================================
// Compresor
// ===========================================================================

TEST(GoldenDynamics, CompressorFollowsItsHardKneeCurveAcrossTheStaircase) {
    // La afirmacion central: cada escalon sale donde threshold y ratio dicen.
    // Ningun test anterior podia decir esto — los que habia comprobaban que la
    // energia cambiara al mover un parametro, que es compatible con cualquier
    // ratio.
    constexpr double kThreshold = -20.0;

    for (double ratio : {2.0, 4.0, 8.0, 20.0}) {
        auto fx = makeComp(static_cast<float>(kThreshold), static_cast<float>(ratio),
                           0.0f, 0.0f);

        for (double inDb : {-50.0, -40.0, -30.0, -24.0, -12.0, -6.0, 0.0}) {
            const double got = stepLevelDb(*fx, inDb);
            const double want = expectedHardKneeDb(inDb, kThreshold, ratio, 0.0);

            EXPECT_NEAR(got, want, 0.1)
                << "ratio " << ratio << ":1, entrada " << inDb << " dBFS";
        }
    }
}

TEST(GoldenDynamics, CompressorIsTransparentBelowThreshold) {
    // Por debajo del threshold la ganancia es exactamente 1. Un compresor que
    // toque lo que esta abajo no es un compresor, es un gain stage.
    auto fx = makeComp(-20.0f, 8.0f, 0.0f, 0.0f);

    for (double inDb : {-60.0, -50.0, -40.0, -30.0, -25.0}) {
        EXPECT_NEAR(stepLevelDb(*fx, inDb), inDb, 0.05)
            << "entrada " << inDb << " dBFS, por debajo del threshold";
    }
}

TEST(GoldenDynamics, RatioOneIsUnityEvenAboveThreshold) {
    // 1:1 es la identidad. Es el caso que un error de signo en (1/ratio - 1)
    // rompe de inmediato, y ademas el que un usuario usa para comparar A/B.
    auto fx = makeComp(-20.0f, 1.0f, 0.0f, 0.0f);

    for (double inDb : {-30.0, -20.0, -10.0, 0.0}) {
        EXPECT_NEAR(stepLevelDb(*fx, inDb), inDb, 0.05) << "entrada " << inDb;
    }
}

TEST(GoldenDynamics, SoftKneeStartsCompressingBeforeTheThreshold) {
    // La rodilla blanda empieza a comprimir medio knee ANTES del threshold, y
    // en el threshold exacto ya bajo una cantidad que la formula determina:
    //   reduccion = (knee/2)^2 / (2*knee) * (1/ratio - 1)
    // Con knee = 12 dB y ratio 4 eso da -1,125 dB, contra 0 de la rodilla dura.
    // Es la diferencia que separa "hay rodilla" de "la rodilla hace algo".
    constexpr double kThreshold = -20.0;
    constexpr double kKnee = 12.0;
    constexpr double kRatio = 4.0;

    auto soft = makeComp(kThreshold, kRatio, kKnee, 0.0f);
    auto hard = makeComp(kThreshold, kRatio, 0.0f, 0.0f);

    const double kneeIn = kKnee / 2.0;
    const double expectedSoftAtThreshold =
        kThreshold + (kneeIn * kneeIn) / (2.0 * kKnee) * (1.0 / kRatio - 1.0);

    EXPECT_NEAR(stepLevelDb(*soft, kThreshold), expectedSoftAtThreshold, 0.1)
        << "en el threshold, la rodilla blanda ya tiene que estar comprimiendo";
    EXPECT_NEAR(stepLevelDb(*hard, kThreshold), kThreshold, 0.05)
        << "en el threshold, la rodilla dura todavia no comprime";

    // Y por debajo de threshold - knee/2 las dos coinciden: no hay compresion.
    const double below = kThreshold - kKnee / 2.0 - 3.0;
    EXPECT_NEAR(stepLevelDb(*soft, below), below, 0.05);
}

TEST(GoldenDynamics, MakeupGainShiftsTheWholeCurveByExactlyItsValue) {
    // El makeup es una ganancia de salida: corre la curva entera, no la dobla.
    constexpr double kThreshold = -20.0;
    constexpr double kRatio = 4.0;

    for (double makeup : {6.0, 12.0}) {
        auto fx = makeComp(kThreshold, kRatio, 0.0f, static_cast<float>(makeup));
        for (double inDb : {-40.0, -20.0, -6.0}) {
            EXPECT_NEAR(stepLevelDb(*fx, inDb),
                        expectedHardKneeDb(inDb, kThreshold, kRatio, makeup), 0.1)
                << "makeup " << makeup << " dB, entrada " << inDb;
        }
    }
}

// ===========================================================================
// Limiter
// ===========================================================================

TEST(GoldenDynamics, LimiterHoldsItsCeiling) {
    // El contrato entero del limiter en una linea: nada sale por encima del
    // techo. Se mide en regimen, despues de que el envelope engancho.
    for (float ceilingDb : {-0.5f, -3.0f, -6.0f, -12.0f}) {
        LookaheadLimiter lim;
        lim.setSampleRate(kSampleRate);
        lim.setThreshold(ceilingDb);

        // Continua bien por encima del techo, sostenida.
        const float amp = 0.99f;
        std::vector<float> in(static_cast<size_t>(kStepFrames) * 2, amp);
        std::vector<float> out(static_cast<size_t>(kStepFrames) * 2, 0.0f);
        lim.process(in.data(), out.data(), kStepFrames);

        // Se ignora el arranque: el lookahead y el attack necesitan su tiempo,
        // y medir ahi seria medir el transitorio, no el techo.
        const int settled = kStepFrames / 2;
        float peak = 0.0f;
        for (int i = settled; i < kStepFrames; ++i) {
            peak = std::max(peak, std::abs(out[static_cast<size_t>(i) * 2]));
        }

        const double ceilingLin = dbToLin(ceilingDb);
        EXPECT_LE(static_cast<double>(peak), ceilingLin * 1.02)
            << "el limiter dejo pasar " << toDb(peak) << " dBFS con el techo en "
            << ceilingDb << " dBFS";
    }
}

TEST(GoldenDynamics, LimiterDeclaresItsFiveMillisecondLookaheadAsLatency) {
    // WD-3.1 dice que la latencia declarada tiene que ser la que el procesador
    // TIENE. `LookaheadLimiter` retrasa la señal directa 5 ms —la salida es el
    // buffer demorado por la ganancia, no la entrada— y hasta esta tanda
    // devolvia el 0 del default de `Effect`.
    //
    // Hoy no rompe nada porque vive en el bus master, donde no hay ramas que
    // sumar; `test_effect_latency.cpp` ya deja escrito ese razonamiento. Pero es
    // una declaracion falsa, y el dia que alguien lo registre como efecto de
    // cadena la compensacion de WD-3.1 va a alinear contra un cero mentiroso.
    for (int sr : {44100, 48000, 96000}) {
        LookaheadLimiter lim;
        lim.setSampleRate(sr);

        const int expected = static_cast<int>(5.0f * sr / 1000.0f);
        EXPECT_EQ(lim.getLatencySamples(), expected)
            << "a " << sr << " Hz el lookahead son " << expected << " samples";

        // Y medido, no solo declarado: donde aparece la primera energia.
        const int frames = expected * 4;
        std::vector<float> in(static_cast<size_t>(frames) * 2, 0.0f);
        std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
        in[0] = 0.5f;
        in[1] = 0.5f;
        lim.process(in.data(), out.data(), frames);

        int onset = -1;
        for (int f = 0; f < frames; ++f) {
            if (std::abs(out[static_cast<size_t>(f) * 2]) > 1e-5f) {
                onset = f;
                break;
            }
        }
        EXPECT_EQ(onset, expected)
            << "a " << sr << " Hz la primera energia salio en el frame " << onset;
    }
}

TEST(GoldenDynamics, LimiterResetClearsItsLookaheadTail) {
    // El limiter guarda 5 ms de audio. Si `reset()` no los limpia, el primer
    // bloque del contexto nuevo recibe una cola del anterior — el mismo blip
    // que `OutputStage::reset()` nombra en su comentario.
    LookaheadLimiter lim;
    lim.setSampleRate(kSampleRate);

    const int frames = 2048;
    std::vector<float> loud(static_cast<size_t>(frames) * 2, 0.8f);
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    lim.process(loud.data(), out.data(), frames);

    lim.reset();

    std::vector<float> silence(static_cast<size_t>(frames) * 2, 0.0f);
    lim.process(silence.data(), out.data(), frames);

    for (size_t i = 0; i < out.size(); ++i) {
        ASSERT_EQ(out[i], 0.0f)
            << "quedo cola del bloque anterior en la muestra " << i;
    }
}
