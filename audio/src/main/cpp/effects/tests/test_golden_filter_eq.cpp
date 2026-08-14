/**
 * WD-2.2 — primera tanda de la suite golden: FilterEffect y ParametricEQ.
 *
 * POR QUE ESTOS DOS PRIMERO
 * -------------------------
 * Porque son donde un error se puede AFIRMAR, no estimar. Un filtro tiene una
 * frecuencia de corte, una pendiente y una Q, y las tres son numeros que la
 * teoria fija de antemano: un LPF Butterworth vale exactamente -3,0103 dB en su
 * cutoff, y un peaking de +9 dB vale exactamente +9,00 dB en su centro. No hay
 * que decidir "cuanto es aceptable" — hay que decidir cuanto error numerico se
 * tolera alrededor de un valor que ya esta determinado.
 *
 * Comparar contra la RT60 de un reverb, en cambio, empieza con una discusion
 * sobre que es la RT60 de ESE reverb. Eso viene despues.
 *
 * LAS TRES CAPAS, Y QUE CACHA CADA UNA
 * ------------------------------------
 * 1. PROPIEDADES ANALITICAS — el cutoff esta donde dice, la pendiente es
 *    -12 dB/oct, la Q no esta invertida, el shelf llega a su ganancia. Afirman
 *    INTENCION DE DISEÑO. Son las unicas que pueden decir que el DSP esta MAL,
 *    en vez de solo que CAMBIO. Sobreviven a una recaptura de golden.
 *
 * 2. GOLDEN DE RESPUESTA (.resp) — la curva entera en 31 puntos, en texto.
 *    Cacha los cambios que ninguna propiedad nombro. Robusta entre plataformas
 *    porque |H(f)| no acumula error. El diff se lee en el PR.
 *
 * 3. GOLDEN DE IR (.f32) — muestra por muestra. La red mas fina y la mas
 *    fragil; ver la nota de deriva entre libms en GoldenHarness.h.
 *
 * Las tres miden el AUDIO que `process()` produjo. Ninguna le pregunta al
 * codigo por sus propios coeficientes.
 *
 * LO QUE ESTOS TESTS ENCONTRARON AL ESCRIBIRLOS
 * ---------------------------------------------
 * El smoothing de cutoff y resonancia de `FilterEffect` NUNCA SE EJECUTO. Ver
 * `FilterSmoothingIsDeadCode_CutoffJumpsInstantly` al final del archivo: no es
 * una sospecha de lectura, es una propiedad medida.
 */

#include "GoldenHarness.h"

#include "../FilterEffect.h"
#include "../ParametricEQ.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace {

using namespace wma::golden;

constexpr double kNyquist = kSampleRate / 2.0;

/// El valor exacto de un Butterworth (Q = 1/raiz(2)) en su propio cutoff.
/// No es una convencion: para el biquad RBJ, |H(w0)| = Q para LPF, HPF y BPF.
constexpr double kMinus3dB = -3.0103;

// --- constructores de preset -----------------------------------------------

std::unique_ptr<FilterEffect> makeFilter(FilterEffect::FilterType type, float cutoff, float q) {
    auto fx = std::make_unique<FilterEffect>();
    fx->setSampleRate(kSampleRate);
    fx->setType(type);
    fx->setCutoff(cutoff);
    fx->setResonance(q);
    return fx;
}

std::unique_ptr<ParametricEQ> makeEq() {
    auto fx = std::make_unique<ParametricEQ>();
    fx->setSampleRate(kSampleRate);
    return fx;
}

/// dB medidos en `freqHz` de un efecto recien construido.
double responseOf(Effect& fx, double freqHz) {
    const std::vector<float> ir = captureImpulseResponse(fx);
    return responseDbAt(ir, freqHz);
}

}  // namespace

// ===========================================================================
// CAPA 1 — propiedades analiticas: FilterEffect
// ===========================================================================

TEST(GoldenFilter, LpfIsUnityAtDcAndMinus3dBAtItsCutoff) {
    // Para el LPF de RBJ, B(z=1)/A(z=1) = 1 EXACTAMENTE: la ganancia en
    // continua no depende ni del cutoff ni de la Q. Si esto se mueve, se rompio
    // la normalizacion por a0.
    for (float fc : {200.0f, 1000.0f, 5000.0f}) {
        auto fx = makeFilter(FilterEffect::LPF, fc, 0.707f);
        const std::vector<float> ir = captureImpulseResponse(*fx);

        EXPECT_NEAR(responseDbAt(ir, 0.0), 0.0, 0.02) << "ganancia en DC, fc=" << fc;
        EXPECT_NEAR(responseDbAt(ir, fc), kMinus3dB, 0.05) << "en el cutoff, fc=" << fc;
    }
}

TEST(GoldenFilter, LpfRollsOffTwelveDbPerOctave) {
    // Un biquad es de segundo orden: -12 dB por octava. Si alguien lo degrada a
    // un one-pole daria -6, y una cascada daria -24. El rango de aceptacion
    // esta puesto para distinguir esos tres casos, no para ser estrecho.
    auto fx = makeFilter(FilterEffect::LPF, 500.0f, 0.707f);
    const std::vector<float> ir = captureImpulseResponse(*fx);

    const double at2k = responseDbAt(ir, 2000.0);
    const double at4k = responseDbAt(ir, 4000.0);
    const double slope = at4k - at2k;

    EXPECT_GT(slope, -13.5) << "pendiente medida " << slope << " dB/oct";
    EXPECT_LT(slope, -11.0) << "pendiente medida " << slope << " dB/oct";
}

TEST(GoldenFilter, HpfIsSilentAtDcAndMinus3dBAtItsCutoff) {
    // B(z=1) = 0 EXACTAMENTE para el HPF: b0 + b1 + b2 se cancela termino a
    // termino. Un HPF que deje pasar continua tiene los coeficientes mal.
    auto fx = makeFilter(FilterEffect::HPF, 1000.0f, 0.707f);
    const std::vector<float> ir = captureImpulseResponse(*fx);

    EXPECT_LT(responseDbAt(ir, 0.0), -70.0) << "el HPF esta dejando pasar continua";
    EXPECT_NEAR(responseDbAt(ir, 1000.0), kMinus3dB, 0.05);
    // Y sube 12 dB/oct hacia el cutoff, visto desde abajo.
    const double slope = responseDbAt(ir, 250.0) - responseDbAt(ir, 125.0);
    EXPECT_GT(slope, 11.0) << "pendiente medida " << slope;
    EXPECT_LT(slope, 13.5) << "pendiente medida " << slope;
}

TEST(GoldenFilter, BpfPeakGainEqualsQAndRejectsBothEnds) {
    // ESTA ES LA FORMA "constant skirt gain, peak gain = Q" del cookbook
    // (b0 = sin(w)/2), no la de pico unitario (b0 = alpha). La diferencia es
    // audible y silenciosa: con Q=4 el pico esta 12 dB arriba, no en 0 dB.
    // Confundir las dos formas es un error clasico y este test lo separa.
    for (float q : {0.707f, 2.0f, 4.0f}) {
        auto fx = makeFilter(FilterEffect::BPF, 1000.0f, q);
        const std::vector<float> ir = captureImpulseResponse(*fx);

        EXPECT_NEAR(responseDbAt(ir, 1000.0), 20.0 * std::log10(q), 0.05)
            << "el pico del BPF deberia valer Q = " << q;
        EXPECT_LT(responseDbAt(ir, 0.0), -70.0) << "el BPF pasa continua, Q=" << q;
        EXPECT_LT(responseDbAt(ir, kNyquist), -70.0) << "el BPF pasa Nyquist, Q=" << q;
    }
}

TEST(GoldenFilter, ResonancePeakEqualsQ_SoAnInvertedQFailsLoudly) {
    // |H(w0)| = Q para el LPF. Con Q=10 el pico son +20 dB; con la Q invertida
    // (1/Q) serian -20 dB. Cuarenta dB de distancia: ningun redondeo llega ahi.
    for (float q : {1.0f, 4.0f, 10.0f}) {
        auto fx = makeFilter(FilterEffect::LPF, 1000.0f, q);
        EXPECT_NEAR(responseOf(*fx, 1000.0), 20.0 * std::log10(q), 0.05)
            << "pico de resonancia con Q=" << q;
    }
}

TEST(GoldenFilter, GainCompensationPinsThePeakAboveTheSelfOscThreshold) {
    // Arriba de SELF_OSC_THRESHOLD el efecto multiplica por 15/Q, asi que el
    // pico deja de crecer y queda CLAVADO en 15 (+23,52 dB) para toda Q mayor.
    // Es una propiedad falsable y fuerte: si alguien saca la compensacion, Q=30
    // se va a +29,5 dB y esto se pone rojo.
    const double pinnedDb = 20.0 * std::log10(FilterEffect::SELF_OSC_THRESHOLD);

    for (float q : {20.0f, 25.0f, 30.0f}) {
        auto fx = makeFilter(FilterEffect::LPF, 1000.0f, q);
        EXPECT_NEAR(responseOf(*fx, 1000.0), pinnedDb, 0.05)
            << "la compensacion de ganancia no clavo el pico con Q=" << q;
    }

    // Por debajo del umbral NO se aplica: el pico sigue valiendo Q, y con Q=14
    // eso es un valor DISTINTO del clavado. (Con Q=15 exacto los dos coinciden
    // por continuidad, asi que ese punto no discriminaria nada.)
    auto below = makeFilter(FilterEffect::LPF, 1000.0f, 14.0f);
    EXPECT_NEAR(responseOf(*below, 1000.0), 20.0 * std::log10(14.0), 0.05);
    EXPECT_LT(20.0 * std::log10(14.0), pinnedDb);
}

// ===========================================================================
// CAPA 1 — propiedades analiticas: ParametricEQ
// ===========================================================================

TEST(GoldenEq, FlatEqIsExactlyTransparent) {
    // Con las tres ganancias en 0 dB, A = 1 y los coeficientes del numerador
    // quedan IDENTICOS a los del denominador: H(z) = 1 exacto, no aproximado.
    // Un EQ "plano" que coloree es el defecto mas facil de no notar y el mas
    // caro de arrastrar, porque afecta a todas las cadenas por igual.
    auto fx = makeEq();
    const std::vector<float> ir = captureImpulseResponse(*fx);

    EXPECT_NEAR(ir[0], 1.0f, 1e-6f) << "la IR de un EQ plano tiene que ser una delta";
    for (size_t n = 1; n < ir.size(); ++n) {
        ASSERT_LT(std::abs(ir[n]), 1e-6f)
            << "cola en la muestra " << n << " de un EQ que deberia ser transparente";
    }
    for (double f : responseGrid()) {
        EXPECT_NEAR(responseDbAt(ir, f), 0.0, 0.01) << "en " << f << " Hz";
    }
}

TEST(GoldenEq, MidBandReachesExactlyItsRequestedGain) {
    // Para el peaking de RBJ, |H(w0)| = A^2 = 10^(gainDb/20) EXACTAMENTE. Que
    // un +9 dB pedido de +9,0 dB medidos es la afirmacion que ningun test
    // anterior podia hacer: los que habia solo miraban que la energia cambiara.
    for (float gainDb : {-12.0f, -9.0f, -3.0f, 3.0f, 9.0f, 12.0f}) {
        auto fx = makeEq();
        fx->setMid(1000.0f, gainDb, 2.0f);
        const std::vector<float> ir = captureImpulseResponse(*fx);

        EXPECT_NEAR(responseDbAt(ir, 1000.0), gainDb, 0.05)
            << "la banda media no llego a " << gainDb << " dB";
        // Y lejos del centro no hace nada: una banda "parametrica" que mueva
        // los extremos no es parametrica.
        EXPECT_NEAR(responseDbAt(ir, 20.0), 0.0, 0.3) << "en 20 Hz con gain " << gainDb;
        EXPECT_NEAR(responseDbAt(ir, 20000.0), 0.0, 0.3) << "en 20 kHz con gain " << gainDb;
    }
}

TEST(GoldenEq, HigherMidQNarrowsTheBand_SoAnInvertedQFails) {
    // La Q invertida deja el pico intacto (|H(w0)| = A^2 no depende de Q) y solo
    // cambia el ANCHO. Por eso el test del pico no alcanza y hace falta este:
    // con +12 dB en 1 kHz, medir en 2 kHz y exigir que la banda se angoste
    // MONOTONAMENTE al subir la Q.
    double previous = 1e9;
    for (float q : {0.5f, 1.0f, 2.0f, 4.0f, 8.0f}) {
        auto fx = makeEq();
        fx->setMid(1000.0f, 12.0f, q);
        const double atOctaveAbove = responseOf(*fx, 2000.0);

        EXPECT_LT(atOctaveAbove, previous)
            << "con Q=" << q << " la banda no se angosto respecto de la Q anterior";
        previous = atOctaveAbove;
    }
    EXPECT_LT(previous, 3.0) << "con Q=8 una octava arriba ya casi no deberia quedar boost";
}

TEST(GoldenEq, LowShelfPlateausAtItsGainInDcAndVanishesUp) {
    // Ganancia exacta en continua = A^2, y unidad en Nyquist. Las dos son
    // identidades del shelf de RBJ, no aproximaciones.
    for (float gainDb : {-9.0f, 9.0f}) {
        auto fx = makeEq();
        fx->setLowShelf(100.0f, gainDb);
        const std::vector<float> ir = captureImpulseResponse(*fx);

        EXPECT_NEAR(responseDbAt(ir, 0.0), gainDb, 0.05) << "meseta del low shelf";
        EXPECT_NEAR(responseDbAt(ir, kNyquist), 0.0, 0.05) << "el low shelf toca Nyquist";
    }
}

TEST(GoldenEq, HighShelfPlateausAtItsGainInNyquistAndVanishesDown) {
    for (float gainDb : {-9.0f, 9.0f}) {
        auto fx = makeEq();
        fx->setHighShelf(8000.0f, gainDb);
        const std::vector<float> ir = captureImpulseResponse(*fx);

        EXPECT_NEAR(responseDbAt(ir, kNyquist), gainDb, 0.05) << "meseta del high shelf";
        EXPECT_NEAR(responseDbAt(ir, 0.0), 0.0, 0.05) << "el high shelf toca continua";
    }
}

TEST(GoldenEq, BypassingEveryBandIsBitExactPassthrough) {
    // El camino de bypass copia entrada a salida. "Bit exacto" y no "parecido":
    // si en algun momento se cuela una multiplicacion por 1.0f calculada, esto
    // sigue verde, pero si se cuela un filtro, no.
    auto fx = makeEq();
    fx->setLowShelf(100.0f, 9.0f);
    fx->setMid(1000.0f, -9.0f, 4.0f);
    fx->setHighShelf(8000.0f, 9.0f);
    fx->setBandBypass(ParametricEQ::LOW, true);
    fx->setBandBypass(ParametricEQ::MID, true);
    fx->setBandBypass(ParametricEQ::HIGH, true);

    std::vector<float> in = impulseStereo(256);
    for (int i = 0; i < 256; ++i) {
        in[static_cast<size_t>(i) * 2] = static_cast<float>(i % 7) * 0.1f - 0.3f;
        in[static_cast<size_t>(i) * 2 + 1] = static_cast<float>(i % 5) * 0.1f - 0.2f;
    }
    const std::vector<float> expected = in;
    std::vector<float> out(in.size(), 0.0f);
    fx->process(in.data(), out.data(), 256);

    for (size_t i = 0; i < out.size(); ++i) {
        ASSERT_FLOAT_EQ(out[i], expected[i]) << "en la muestra " << i;
    }
}

// ===========================================================================
// CAPA 2 y 3 — golden de respuesta (.resp) y de IR (.f32)
// ===========================================================================

namespace {

/// Tolerancia del golden de respuesta. |H(f)| es una funcion suave de los
/// coeficientes: no acumula error, asi que 0,02 dB alcanza entre libms y sigue
/// siendo mucho mas fino que cualquier cambio real de DSP.
constexpr double kRespTolDb = 0.02;

/// Tolerancia del golden muestra a muestra. Holgada A PROPOSITO: un IIR
/// amplifica a lo largo del decay la diferencia de 1 ULP que `sinf`/`cosf`
/// tienen entre glibc y libc++. Cualquier cambio real de DSP la supera por
/// varios ordenes de magnitud.
constexpr float kIrTol = 2e-5f;

void checkPreset(const std::string& name, Effect& fx) {
    const std::vector<float> full = captureImpulseResponse(fx, kResponseFrames);
    checkGoldenResponse(name, responseCurve(full), kRespTolDb);

    if (::testing::Test::HasFatalFailure()) return;

    const std::vector<float> head(full.begin(), full.begin() + kGoldenFrames);
    checkGoldenSamples(name, head, kIrTol);
}

}  // namespace

TEST(GoldenPresets, FilterLpf1kQ0707) {
    auto fx = makeFilter(FilterEffect::LPF, 1000.0f, 0.707f);
    checkPreset("filter_lpf_1k_q0707", *fx);
}

TEST(GoldenPresets, FilterHpf1kQ0707) {
    auto fx = makeFilter(FilterEffect::HPF, 1000.0f, 0.707f);
    checkPreset("filter_hpf_1k_q0707", *fx);
}

TEST(GoldenPresets, FilterBpf1kQ2) {
    auto fx = makeFilter(FilterEffect::BPF, 1000.0f, 2.0f);
    checkPreset("filter_bpf_1k_q2", *fx);
}

TEST(GoldenPresets, EqMid1kPlus9dBQ2) {
    auto fx = makeEq();
    fx->setMid(1000.0f, 9.0f, 2.0f);
    checkPreset("eq_mid_1k_p9db_q2", *fx);
}

TEST(GoldenPresets, EqLowShelf100Plus9dB) {
    auto fx = makeEq();
    fx->setLowShelf(100.0f, 9.0f);
    checkPreset("eq_lowshelf_100_p9db", *fx);
}

TEST(GoldenPresets, EqHighShelf8kMinus9dB) {
    auto fx = makeEq();
    fx->setHighShelf(8000.0f, -9.0f);
    checkPreset("eq_highshelf_8k_m9db", *fx);
}

// ===========================================================================
// Invariancia de tamaño de bloque
// ===========================================================================

TEST(GoldenFilter, ImpulseResponseIsIndependentOfBlockSize) {
    // El mismo defecto de familia que encontro WD-2.1 en el fade: un estado que
    // avanza POR BLOQUE en vez de por muestra hace que el sonido dependa del
    // tamaño de buffer del device — 64 frames en un telefono, 4096 en otro.
    //
    // Se compara BIT A BIT, no con tolerancia: para parametros fijos no hay
    // ninguna razon legitima por la que trocear la misma señal de otra forma
    // cambie una sola muestra.
    for (auto type : {FilterEffect::LPF, FilterEffect::HPF, FilterEffect::BPF}) {
        auto reference = makeFilter(type, 500.0f, 2.0f);
        const std::vector<float> want = captureImpulseResponse(*reference, 4096, 4096);

        for (int block : {1, 32, 512, 1024}) {
            auto fx = makeFilter(type, 500.0f, 2.0f);
            const std::vector<float> got = captureImpulseResponse(*fx, 4096, block);

            ASSERT_EQ(got.size(), want.size());
            for (size_t i = 0; i < got.size(); ++i) {
                ASSERT_EQ(got[i], want[i])
                    << "tipo " << static_cast<int>(type) << ", bloque de " << block
                    << " frames, muestra " << i;
            }
        }
    }
}

TEST(GoldenEq, ImpulseResponseIsIndependentOfBlockSize) {
    auto reference = makeEq();
    reference->setMid(1000.0f, 9.0f, 2.0f);
    const std::vector<float> want = captureImpulseResponse(*reference, 4096, 4096);

    for (int block : {1, 32, 512, 1024}) {
        auto fx = makeEq();
        fx->setMid(1000.0f, 9.0f, 2.0f);
        const std::vector<float> got = captureImpulseResponse(*fx, 4096, block);

        ASSERT_EQ(got.size(), want.size());
        for (size_t i = 0; i < got.size(); ++i) {
            ASSERT_EQ(got[i], want[i]) << "bloque de " << block << ", muestra " << i;
        }
    }
}

// ===========================================================================
// Caracterizacion: el smoothing de FilterEffect es codigo muerto
// ===========================================================================

/**
 * ESTE TEST DOCUMENTA UN DEFECTO, NO LO BENDICE.
 *
 * `FilterEffect::process()` dice suavizar cutoff y resonancia para evitar
 * clicks. No lo hace. La guarda es:
 *
 *     float smoothedCutoff = cutoffSmoother.process(targetCutoff);
 *     if (std::abs(smoothedCutoff - cutoffSmoother.getCurrent()) > 0.5f || ...)
 *
 * y `process()` ESCRIBE su resultado en el smoother antes de devolverlo, asi que
 * `getCurrent()` devuelve exactamente lo mismo que acaba de devolver `process()`.
 * La diferencia es cero identico, la guarda nunca se abre, y los coeficientes
 * nunca se recalculan desde el valor suavizado. Lo unico que aplica un cambio de
 * cutoff es `setCutoff()`, que llama a `updateCoefficients()` DE UNA.
 *
 * Consecuencia real: un barrido de cutoff desde el pad XY —la interaccion
 * central de NoisyPad— salta escalon por escalon en vez de deslizarse. Es
 * exactamente el zipper noise que el smoother decia prevenir.
 *
 * NO SE ARREGLA ACA, y a proposito: arreglarlo CAMBIA EL SONIDO, y cambiar el
 * sonido antes de que existan los golden es hacerlo a ciegas. Ese es el orden
 * que fija el programa WD y la razon por la que la Fase 2 va antes que la 3.
 *
 * CUANDO SE ARREGLE, ESTE TEST SE PONE ROJO. Eso no es una regresion: es la
 * señal de recapturar los golden conscientemente y de borrar este archivo de
 * caracterizacion.
 */
TEST(GoldenFilter, FilterSmoothingIsDeadCode_CutoffJumpsInstantly) {
    // (a) El estado del smoother no influye en la salida. `warm` proceso cien
    //     bloques de SILENCIO despues de pedir el cambio de cutoff — silencio
    //     deja el estado del filtro en cero exacto, pero habria hecho avanzar
    //     cien pasos a un smoother vivo. `cold` no proceso nada.
    auto warm = makeFilter(FilterEffect::LPF, 200.0f, 0.707f);
    std::vector<float> silence(512 * 2, 0.0f);
    std::vector<float> sink(512 * 2, 0.0f);
    for (int i = 0; i < 100; ++i) {
        warm->process(silence.data(), sink.data(), 512);
    }

    auto cold = makeFilter(FilterEffect::LPF, 200.0f, 0.707f);

    const std::vector<float> warmIr = captureImpulseResponse(*warm, 4096, 512);
    const std::vector<float> coldIr = captureImpulseResponse(*cold, 4096, 512);
    for (size_t i = 0; i < warmIr.size(); ++i) {
        ASSERT_EQ(warmIr[i], coldIr[i])
            << "muestra " << i << ": el smoother influyo en la salida — "
            << "si el smoothing ya funciona, este test cumplio su proposito "
            << "y hay que recapturar los golden y borrarlo";
    }

    // (b) Y la consecuencia audible: un salto de 5 kHz a 200 Hz vale ya
    //     completo en el PRIMER bloque. Un smoothing vivo de ~10 ms dejaria el
    //     corte muy por encima de 200 Hz durante los primeros 480 frames.
    auto jumped = makeFilter(FilterEffect::LPF, 5000.0f, 0.707f);
    jumped->process(silence.data(), sink.data(), 512);
    jumped->setCutoff(200.0f);

    EXPECT_NEAR(responseOf(*jumped, 200.0), kMinus3dB, 0.05)
        << "el corte no quedo en 200 Hz de inmediato";
}
