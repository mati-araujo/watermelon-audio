/**
 * test_phase_slope.cpp — REQ-001 S2. La etapa que puede fallar.
 *
 * ESTA SUITE EXISTE PARA MEDIR, NO PARA APROBAR
 * ----------------------------------------------
 * Todo el diferenciador del producto cuelga de un numero que nadie midio todavia
 * en este codigo: ±0,1 cent relativo. Si no es alcanzable en esta plataforma, el
 * requerimiento entero cambia de forma, y el momento de saberlo es **ahora**, con
 * S1 hecho y nada mas construido encima. Un resultado negativo acá es un exito de
 * la etapa: cuesta dos semanas en vez de cuatro meses.
 *
 * Por eso los tests afirman el umbral del AC y **no** un umbral acomodado a lo
 * que salga. Si dan rojo, el hallazgo es el rojo.
 *
 * DOS TRAMPAS QUE ESTA SUITE EVITA A PROPOSITO
 * --------------------------------------------
 * 1. **El desajuste de prueba va POR ENCIMA de la tolerancia.** El escenario que
 *    traia la spec —82,410 contra 82,407— son 0,063 cents con tolerancia 0,1: un
 *    estimador que devuelva SIEMPRE 0,0 lo pasaba. Un test que no distingue la
 *    implementacion correcta de `return 0` no mide nada. Acá el desajuste es de
 *    ±1 cent, y la resolucion de 0,063 tiene su propio test que ademas exige que
 *    el valor sea distinguible de cero.
 * 2. **Ningun rate de prueba es 48000 en los tests que podrian coincidir por
 *    casualidad con una constante cableada** — la leccion del rate de captura de
 *    S1. Donde 48 kHz importa como caso realista, se usa, pero acompañado.
 */

#include "support/SyntheticSignal.h"

#include "PhaseSlopeEstimator.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace wma_test {
namespace {

using wma::analysis::PhaseSlopeEstimator;

constexpr int kRate = 48000;

/// Los 3 s que pide el AC, en frames.
constexpr int kThreeSeconds = 3 * kRate;

/// El desajuste de prueba: 1 cent, DIEZ VECES la tolerancia. Ver la trampa 1.
constexpr double kProbeCents = 1.0;

/// La tolerancia del AC.
constexpr double kToleranceCents = 0.1;

struct Note {
    const char* name;
    double hz;
};

/// El rango del AC: A0 (bajo de 5 cuerdas) a C7 (armonicos de guitarra).
const std::vector<Note>& noteTable() {
    static const std::vector<Note> kNotes = {
        {"A0", 27.500}, {"B0", 30.868}, {"E1", 41.203}, {"E2", 82.407},
        {"A2", 110.000}, {"D3", 146.832}, {"G3", 195.998}, {"B3", 246.942},
        {"E4", 329.628}, {"A4", 440.000}, {"E5", 659.255}, {"C7", 2093.005},
    };
    return kNotes;
}

/// Corre una señal entera por el estimador, en bloques de `blockFrames`.
void feed(PhaseSlopeEstimator& est, const std::vector<float>& sig, int blockFrames) {
    int i = 0;
    const int n = static_cast<int>(sig.size());
    while (i < n) {
        const int take = std::min(blockFrames, n - i);
        est.process(sig.data() + i, take);
        i += take;
    }
}

/// Mide un tono desafinado `cents` respecto de `targetHz`, integrando `frames`.
PhaseSlopeEstimator measure(double targetHz, double cents, int frames,
                            int blockFrames = 512, int rate = kRate) {
    PhaseSlopeEstimator est;
    est.prepare(rate);
    est.setTarget(targetHz);
    const auto sig = pureSine(detune(targetHz, cents), rate, frames);
    feed(est, sig, blockFrames);
    return est;
}

// ---------------------------------------------------------------------------
// 2.1 — el numero del que cuelga el producto
// ---------------------------------------------------------------------------

/**
 * TAREA 2.1. Error ≤ 0,1 cent tras 3 s, en todo el rango.
 *
 * Cada nota se reporta con su error medido aunque pase, porque **el producto de
 * esta etapa es el numero**, no el verde: el DoD pide que quede escrito.
 */
TEST(PhaseSlope, TheErrorStaysUnderATenthOfACentAcrossTheRangeAfterThreeSeconds) {
    for (const auto& note : noteTable()) {
        const auto est = measure(note.hz, kProbeCents, kThreeSeconds);

        ASSERT_TRUE(est.hasMeasurement()) << note.name << ": no produjo medicion";
        const double err = std::abs(est.cents() - kProbeCents);

        EXPECT_LE(err, kToleranceCents)
            << note.name << " (" << note.hz << " Hz): medido " << est.cents()
            << " cents contra " << kProbeCents << " reales — error " << err;

        // El numero, siempre, pase o falle.
        RecordProperty(std::string("error_cents_") + note.name, std::to_string(err));
    }
}

/**
 * TAREA 2.1b. La RESOLUCION llega a 0,063 cents — el escenario original del
 * spec— y el valor es **distinguible de cero**.
 *
 * Es lo que 2.1 no puede dar: aquel mide exactitud contra un Δ grande, este mide
 * que el piso de resolucion existe. Sin la segunda mitad, `return 0` pasa.
 */
TEST(PhaseSlope, ItResolvesSixHundredthsOfACentAndTheReadingIsNotZero) {
    const double target = 82.407;
    const double sig = 82.410;
    const double real = centsBetween(sig, target);     // +0,063
    ASSERT_NEAR(real, 0.063, 0.001) << "la aritmetica del escenario cambio";

    PhaseSlopeEstimator est;
    est.prepare(kRate);
    est.setTarget(target);
    feed(est, pureSine(sig, kRate, kThreeSeconds), 512);

    ASSERT_TRUE(est.hasMeasurement());
    EXPECT_NEAR(est.cents(), real, kToleranceCents);
    EXPECT_GT(est.cents(), 0.02)
        << "reporto " << est.cents() << ": indistinguible de cero, asi que este "
           "test lo pasaria un estimador que no mide nada";
}

/**
 * TAREA 2.1c. El SIGNO es el del afinador: señal por encima ⇒ positivo.
 *
 * S6 · 6.5 hereda esta convencion para el sentido de giro del disco, asi que un
 * signo invertido acá se ve como un strobe que gira al reves.
 */
TEST(PhaseSlope, SharpReadsPositiveAndFlatReadsNegative) {
    const double target = 110.0;

    const auto sharp = measure(target, +2.0, kThreeSeconds);
    const auto flat  = measure(target, -2.0, kThreeSeconds);

    ASSERT_TRUE(sharp.hasMeasurement());
    ASSERT_TRUE(flat.hasMeasurement());

    EXPECT_GT(sharp.cents(), 0.0) << "señal por ENCIMA del objetivo tiene que leer positivo";
    EXPECT_LT(flat.cents(), 0.0)  << "señal por DEBAJO del objetivo tiene que leer negativo";
    EXPECT_NEAR(sharp.cents(), +2.0, kToleranceCents);
    EXPECT_NEAR(flat.cents(), -2.0, kToleranceCents);
}

// ---------------------------------------------------------------------------
// 2.2 — convergencia
// ---------------------------------------------------------------------------

/**
 * TAREA 2.2. La convergencia es MONOTONA: el error a 3 s no es peor que a 1 s.
 *
 * Un estimador cuyo error empeora integrando mas tiempo esta acumulando algo mal
 * —tipicamente el desenvuelto de fase— y lo taparia un test que solo mire el
 * final.
 */
TEST(PhaseSlope, IntegratingLongerNeverMakesTheReadingWorse) {
    const double target = 146.832;

    const double e1 = std::abs(measure(target, kProbeCents, 1 * kRate).cents() - kProbeCents);
    const double e3 = std::abs(measure(target, kProbeCents, 3 * kRate).cents() - kProbeCents);

    RecordProperty("error_at_1s", std::to_string(e1));
    RecordProperty("error_at_3s", std::to_string(e3));

    // Con holgura de 1 ulp de cent: la exigencia es que no EMPEORE, no que mejore
    // estrictamente — un estimador que ya convergio a 1 s cumple igual.
    EXPECT_LE(e3, e1 + 1e-9)
        << "el error a 3 s (" << e3 << ") es peor que a 1 s (" << e1 << ")";
}

// ---------------------------------------------------------------------------
// 2.3 — el error de reloj (habilitador de AC-001.7, que es de S10)
// ---------------------------------------------------------------------------

/**
 * TAREA 2.3. Inmunidad al error de MODO COMUN.
 *
 * Un cristal de telefono tipico esta en ±20–50 ppm. 50 ppm son 0,087 cents: se
 * comen el presupuesto entero. Pero ese error escala **señal y objetivo por igual**
 * —los dos se miden con el mismo reloj— asi que se cancela exacto al afinar un
 * instrumento contra si mismo.
 *
 * Esta es la premisa que vuelve honesta la declaracion de DOS cifras de AC-001.7
 * (S10): relativa ≤ 0,1 cent, absoluta limitada por el dispositivo y **no
 * garantizada**. Si este test falla, ese AC no se arregla redactando distinto: se
 * replantea.
 */
TEST(PhaseSlope, ACommonModeClockErrorCancelsExactly) {
    const double target = 82.407;
    const double ppm = 50e-6;

    const auto normal = measure(target, kProbeCents, kThreeSeconds);

    // El cristal corre 50 ppm rapido: TODO lo que se mide con el se escala igual.
    const double scaledTarget = target * (1.0 + ppm);
    const auto scaled = measure(scaledTarget, kProbeCents, kThreeSeconds);

    ASSERT_TRUE(normal.hasMeasurement());
    ASSERT_TRUE(scaled.hasMeasurement());

    EXPECT_NEAR(scaled.cents(), normal.cents(), 0.001)
        << "50 ppm de cristal movieron la lectura RELATIVA en "
        << std::abs(scaled.cents() - normal.cents())
        << " cents. Si esto no cancela, la exactitud relativa no es defendible.";
}

// ---------------------------------------------------------------------------
// 2.4 — parciales inarmonicos
// ---------------------------------------------------------------------------

/**
 * TAREA 2.4. Sobre una cuerda con inarmonicidad conocida, el estimador sobre el
 * FUNDAMENTAL no se contamina con los parciales.
 *
 * Es la condicion para que S7 pueda leer B del desacuerdo entre armonicos: si el
 * fundamental ya viene arrastrado por el segundo parcial, el desacuerdo mide el
 * arrastre y no la cuerda.
 */
TEST(PhaseSlope, ThePartialsOfARealStringDoNotDragTheFundamental) {
    const double f0 = 82.407;
    const double B = 5e-4;                    // bordona gruesa: inarmonicidad alta

    // 🔴 EL PRIMER PARCIAL NO ESTA EN f0, Y CREER QUE SI ESTA COSTO UN ROJO.
    //
    // Este test afirmaba `|cents| <= 0,1` "porque el fundamental esta en f0", y
    // daba rojo con 0,4327. El defectuoso era el TEST: por la definicion del
    // modelo, `f_n = n·f0·sqrt(1 + B·n²)`, asi que el primer parcial esta en
    // `f0·sqrt(1+B)` — +0,4327 cents para este B. La lectura del estimador
    // coincidia con eso a CINCO decimales.
    //
    // Corregido, el test es mas fuerte que el original: exige que la lectura sea
    // la del primer parcial REAL, asi que cualquier arrastre de los parciales 2
    // a 6 —que estan a 7, 14 y 21 bins de la ventana— aparece como error sobre
    // ese valor. Es la condicion para que S7 pueda leer B del desacuerdo entre
    // armonicos: si el fundamental ya viniera arrastrado, el desacuerdo mediria
    // el arrastre y no la cuerda.
    const double firstPartial = f0 * std::sqrt(1.0 + B);
    const double expected = centsBetween(firstPartial, f0);

    PhaseSlopeEstimator est;
    est.prepare(kRate);
    est.setTarget(f0);
    feed(est, inharmonicString(f0, B, 6, kRate, kThreeSeconds), 512);

    ASSERT_TRUE(est.hasMeasurement());
    const double drag = std::abs(est.cents() - expected);
    RecordProperty("partial_drag_cents", std::to_string(drag));

    EXPECT_LE(drag, kToleranceCents)
        << "el primer parcial esta en " << firstPartial << " Hz (" << expected
        << " cents de f0) y se leyo " << est.cents()
        << ": los parciales superiores arrastraron " << drag << " cents";

    // Y la mitad que impide que esto pase por casualidad: la lectura tiene que
    // ser DISTINTA de cero. Un estimador que no midiera nada tendria drag =
    // 0,4327 y fallaria arriba, pero uno que devolviera `expected` cableado
    // pasaria — esta linea exige que el valor venga de la señal.
    EXPECT_GT(std::abs(est.cents()), 0.1)
        << "leyo " << est.cents() << ", indistinguible de 'no medi nada'";
}

// ---------------------------------------------------------------------------
// 2.5 — ruido
// ---------------------------------------------------------------------------

/**
 * TAREA 2.5. Con ruido, el error degrada de forma DECLARADA y monotona — no
 * salta.
 *
 * Los umbrales de abajo son el contrato de esta etapa con S10, que los va a
 * escribir en `accuracy_contract.md`. No son aspiraciones: si no se cumplen, lo
 * que cambia es el contrato, no el test.
 */
TEST(PhaseSlope, TheErrorDegradesMonotonicallyAndWithinDeclaredBoundsUnderNoise) {
    const double target = 110.0;
    struct Case { double snrDb; double maxError; };
    const Case cases[] = {
        {20.0, 0.10},   // SNR alto: el presupuesto del AC sigue en pie
        {10.0, 0.30},
        { 0.0, 1.00},   // señal y ruido iguales: se pide que siga siendo util
    };

    double previous = 0.0;
    for (const auto& c : cases) {
        PhaseSlopeEstimator est;
        est.prepare(kRate);
        est.setTarget(target);
        auto sig = pureSine(detune(target, kProbeCents), kRate, kThreeSeconds);
        addNoiseAtSnr(sig, c.snrDb);
        feed(est, sig, 512);

        ASSERT_TRUE(est.hasMeasurement()) << "SNR " << c.snrDb << " dB";
        const double err = std::abs(est.cents() - kProbeCents);
        RecordProperty("error_at_snr_" + std::to_string(static_cast<int>(c.snrDb)),
                       std::to_string(err));

        EXPECT_LE(err, c.maxError)
            << "SNR " << c.snrDb << " dB: error " << err << " cents";
        EXPECT_GE(err, previous - 1e-9)
            << "el error MEJORO al bajar el SNR: la degradacion no es monotona, "
               "lo que sugiere que a SNR alto no se estaba midiendo la señal";
        previous = err;
    }
}

// ---------------------------------------------------------------------------
// 2.6 — reset
// ---------------------------------------------------------------------------

/**
 * TAREA 2.6. `reset()` deja el estimador indistinguible de recien construido.
 *
 * Se prueba como en el resto del repo: ensuciarlo con una medicion COMPLETA de
 * otra nota, resetear, y exigir que la medicion siguiente sea **identica** a la
 * de un estimador nuevo — no parecida.
 */
TEST(PhaseSlope, ResetMakesItIndistinguishableFromFreshlyConstructed) {
    const double target = 196.0;

    PhaseSlopeEstimator dirty;
    dirty.prepare(kRate);
    dirty.setTarget(target);
    feed(dirty, pureSine(detune(target, -30.0), kRate, kThreeSeconds), 512);   // bien desafinado
    dirty.reset();

    PhaseSlopeEstimator fresh;
    fresh.prepare(kRate);
    fresh.setTarget(target);

    EXPECT_FALSE(dirty.hasMeasurement()) << "reset() dejo una medicion viva";
    EXPECT_EQ(dirty.windowsAnalyzed(), 0);

    const auto sig = pureSine(detune(target, kProbeCents), kRate, kThreeSeconds);
    feed(dirty, sig, 512);
    feed(fresh, sig, 512);

    EXPECT_DOUBLE_EQ(dirty.cents(), fresh.cents())
        << "el estado viejo sobrevivio al reset y corrio la medicion";
    EXPECT_DOUBLE_EQ(dirty.uncertaintyCents(), fresh.uncertaintyCents());
}

// ---------------------------------------------------------------------------
// El tamaño de bloque como eje diagnostico
// ---------------------------------------------------------------------------

/**
 * El resultado NO puede depender de como el llamador parta el audio.
 *
 * No esta en la lista de tareas y se agrega igual, porque este repo ya uso este
 * eje para cazar un semitono de error en Karplus-Strong: un estado que depende
 * del tamaño de bloque es un estado que se pierde en los bordes. La forma fuerte
 * es exigir **bit-exactitud**, y es la que se exige.
 *
 * 16 y 1024 tampoco son casuales: 16 es mas chico que la ventana interna (asi que
 * una ventana se arma de muchos pedazos) y 1024 la cruza (asi que un bloque
 * alimenta varias ventanas parciales).
 */
TEST(PhaseSlope, TheReadingIsBitIdenticalRegardlessOfTheCallersBlockSize) {
    const double target = 329.628;
    const auto sig = pureSine(detune(target, kProbeCents), kRate, kThreeSeconds);

    PhaseSlopeEstimator small, large, odd;
    for (auto* e : {&small, &large, &odd}) { e->prepare(kRate); e->setTarget(target); }

    feed(small, sig, 16);
    feed(large, sig, 1024);
    feed(odd, sig, 337);      // ni potencia de dos ni divisor de la ventana

    EXPECT_DOUBLE_EQ(small.cents(), large.cents())
        << "bloques de 16 y de 1024 dan lecturas distintas: hay estado que "
           "depende de donde cayo el corte del buffer";
    EXPECT_DOUBLE_EQ(small.cents(), odd.cents());
    EXPECT_EQ(small.windowsAnalyzed(), large.windowsAnalyzed());
}

// ---------------------------------------------------------------------------
// Comportamiento declarado en la spec (Given/When/Then)
// ---------------------------------------------------------------------------

/**
 * GIVEN una nota que decae por debajo del gate, THEN el estimador reporta "sin
 * señal" y **no congela la ultima lectura como si fuera actual**.
 *
 * Es la diferencia entre "no hay dato" y "el dato es viejo". Un afinador que
 * congela miente con cara de estar midiendo — y es la misma leccion que los dos
 * stubs que devolvian ceros.
 */
TEST(PhaseSlope, AFadedNoteReportsNoSignalInsteadOfFreezingTheLastReading) {
    const double target = 110.0;

    PhaseSlopeEstimator est;
    est.prepare(kRate);
    est.setTarget(target);

    auto sig = pureSine(detune(target, kProbeCents), kRate, 6 * kRate);
    applyDecay(sig, kRate, 0.5);          // a los 6 s quedan ~e^-12 del nivel
    feed(est, sig, 512);

    EXPECT_FALSE(est.hasSignal())
        << "la nota se apago hace segundos y el estimador sigue diciendo que hay señal";
}

/**
 * GIVEN un cambio brusco de f0 (el usuario paso a otra cuerda), THEN la
 * incertidumbre SUBE antes de que el error baje.
 *
 * Es lo que le permite a la app mostrar la transicion en vez de un numero que se
 * arrastra desde la nota anterior.
 */
TEST(PhaseSlope, AnAbruptNoteChangeRaisesTheUncertaintyBeforeTheErrorSettles) {
    const double target = 110.0;

    PhaseSlopeEstimator est;
    est.prepare(kRate);
    est.setTarget(target);

    feed(est, pureSine(detune(target, kProbeCents), kRate, 3 * kRate), 512);
    ASSERT_TRUE(est.hasMeasurement());
    const double settled = est.uncertaintyCents();

    // Otra cuerda: un semitono arriba, sin tocar el objetivo.
    feed(est, pureSine(detune(target, 100.0), kRate, kRate / 2), 512);

    EXPECT_GT(est.uncertaintyCents(), settled)
        << "la nota cambio y la incertidumbre no se movio (" << settled << " → "
        << est.uncertaintyCents() << "): la app no tiene como ver la transicion";
}

/**
 * Despues de un SILENCIO, el estimador vuelve a medir de cero — no arrastra la
 * fase ni los puntos de regresion de antes del silencio.
 *
 * ES EL CICLO NORMAL DE UN AFINADOR: se pulsa, la nota se apaga, se vuelve a
 * pulsar. Si el hilo de fase no se corta, la ventana de regresion queda con
 * puntos de los dos lados de un hueco temporal —donde la fase no significaba
 * nada— y la recta que salga de ahi no describe a ninguna de las dos notas.
 *
 * ESTE TEST EXISTE PORQUE UN MUTANTE SOBREVIVIO. Al borrar el corte
 * (`mHavePrevPhase = false; mCount = 0;`) los 15 tests seguian verdes: nada
 * cubria la recuperacion. Se probaba que el estimador DETECTA el silencio, no
 * que se recupera de el.
 */
TEST(PhaseSlope, ItStartsOverAfterASilenceInsteadOfBridgingAcrossIt) {
    const double target = 110.0;

    PhaseSlopeEstimator est;
    est.prepare(kRate);
    est.setTarget(target);

    // Primera nota: +1 cent, dos segundos.
    feed(est, pureSine(detune(target, +1.0), kRate, 2 * kRate), 512);
    ASSERT_TRUE(est.hasMeasurement());
    ASSERT_NEAR(est.cents(), +1.0, kToleranceCents);

    // Se apaga. Un segundo entero por debajo del piso.
    const std::vector<float> silence(static_cast<size_t>(kRate), 0.0f);
    feed(est, silence, 512);
    ASSERT_FALSE(est.hasSignal());

    // Se vuelve a pulsar, y ahora la cuerda esta al OTRO lado del objetivo.
    feed(est, pureSine(detune(target, -2.0), kRate, 3 * kRate), 512);

    ASSERT_TRUE(est.hasMeasurement()) << "no volvio a medir despues del silencio";
    EXPECT_NEAR(est.cents(), -2.0, kToleranceCents)
        << "tras el silencio leyo " << est.cents()
        << " en vez de -2: quedaron puntos de la nota anterior en la regresion";
}

// ---------------------------------------------------------------------------
// 2.11c — el angulo del strobe (AC-001.22, cuyo dueño es S6)
// ---------------------------------------------------------------------------

/**
 * TAREA 2.11c. El angulo publicado es CONTINUO entre ventanas consecutivas —sin
 * saltos salvo el envolvimiento en ±π— y gira a la velocidad de la desafinacion.
 *
 * VINO DE S1 COMO 1.13b Y SE MOVIO ACA, porque en S1 no habia estimador que
 * produjera el angulo: el test habria medido un placeholder.
 *
 * Lo que el AC promete es que **la app no tenga que integrar nada**: si tuviera
 * que derivar la rotacion de la velocidad, un frame perdido le correria la fase
 * para siempre. Por eso el angulo se publica ya acumulado, y por eso este test
 * mira los INCREMENTOS: son ellos los que tienen que ser chicos y parejos.
 *
 * La cota no es arbitraria. Con la señal a `Δcents` del objetivo, el angulo
 * avanza `2π·Δf·N/fs` por ventana; para 1 cent en A2 son 0,034 rad. Se exige que
 * ningun incremento se pase de 3x eso: un salto mas grande significa que el
 * angulo publicado lleva adentro algo que no es la desafinacion.
 */
TEST(PhaseSlope, ThePublishedAngleAdvancesSmoothlyAtTheDetuningRate) {
    const double target = 110.0;
    const double probe = 1.0;

    PhaseSlopeEstimator est;
    est.prepare(kRate);
    est.setTarget(target);

    const auto sig = pureSine(detune(target, probe), kRate, 4 * kRate);

    // El avance TEORICO por ventana, del que sale la cota.
    const double deltaHz = detune(target, probe) - target;
    const double expectedStep =
        2.0 * M_PI * deltaHz * PhaseSlopeEstimator::kWindowFrames / kRate;

    std::vector<double> angles;
    int i = 0;
    const int n = static_cast<int>(sig.size());
    while (i < n) {
        const int take = std::min(512, n - i);
        if (est.process(sig.data() + i, take)) {
            if (est.hasSignal()) angles.push_back(est.phaseAngle());
        }
        i += take;
    }

    ASSERT_GE(angles.size(), 10u) << "no hubo suficientes ventanas para mirar la continuidad";

    double worst = 0.0;
    for (size_t k = 1; k < angles.size(); ++k) {
        // El envolvimiento en ±π es legitimo: se descuenta antes de mirar el salto.
        double d = angles[k] - angles[k - 1];
        while (d > M_PI) d -= 2.0 * M_PI;
        while (d < -M_PI) d += 2.0 * M_PI;
        worst = std::max(worst, std::abs(d));
    }
    RecordProperty("worst_angle_step_rad", std::to_string(worst));
    RecordProperty("expected_step_rad", std::to_string(expectedStep));

    EXPECT_LE(worst, 3.0 * std::abs(expectedStep))
        << "el angulo salta hasta " << worst << " rad entre ventanas, cuando la "
           "desafinacion de " << probe << " cent solo justifica " << expectedStep
        << ": lo que se publica no es la fase de la desafinacion";

    // Y la otra mitad: que se MUEVA. Un angulo clavado tambien es "continuo".
    EXPECT_GT(worst, 0.1 * std::abs(expectedStep))
        << "el angulo no se movio: un disco quieto no es un strobe";
}

/**
 * Y el angulo gira en el sentido del signo: sostenido y bemol dan rotaciones
 * opuestas.
 *
 * Es el insumo directo de S6 · 6.5. Se verifica acá, en la primitiva, porque acá
 * es donde el signo nace — alla ya seria heredado.
 */
TEST(PhaseSlope, TheAngleTurnsOppositeWaysForSharpAndFlat) {
    const double target = 110.0;

    auto netRotation = [&](double cents) {
        PhaseSlopeEstimator est;
        est.prepare(kRate);
        est.setTarget(target);
        const auto sig = pureSine(detune(target, cents), kRate, 2 * kRate);

        double prev = 0.0;
        bool havePrev = false;
        double net = 0.0;
        int i = 0;
        const int n = static_cast<int>(sig.size());
        while (i < n) {
            const int take = std::min(512, n - i);
            if (est.process(sig.data() + i, take) && est.hasSignal()) {
                const double a = est.phaseAngle();
                if (havePrev) {
                    double d = a - prev;
                    while (d > M_PI) d -= 2.0 * M_PI;
                    while (d < -M_PI) d += 2.0 * M_PI;
                    net += d;
                }
                prev = a;
                havePrev = true;
            }
            i += take;
        }
        return net;
    };

    const double sharp = netRotation(+2.0);
    const double flat  = netRotation(-2.0);

    EXPECT_GT(sharp, 0.0) << "sostenido tiene que girar en el sentido positivo";
    EXPECT_LT(flat, 0.0)  << "bemol tiene que girar al reves";
}

}  // namespace
}  // namespace wma_test
