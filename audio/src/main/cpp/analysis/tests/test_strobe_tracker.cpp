/**
 * test_strobe_tracker.cpp — REQ-001 S6. El modo que justifica el producto.
 *
 * Estos tests afirman el umbral del AC (±0,1 cent) y no uno acomodado a lo que
 * salga, igual que la suite de S2. Si dan rojo, el hallazgo es el rojo.
 *
 * POR QUE SE MIDE SOBRE CUERDAS Y NO SOBRE EL RANGO A0-C7
 * -------------------------------------------------------
 * El desenvuelto de S2 acota el rango de captura a |Δf| < fs/(2N) = 5,86 Hz a
 * 48 kHz, y el parcial n se desvia n VECES MAS en Hz para la misma desviacion en
 * cents. O sea que rastrear el 4to armonico estrecha el rango en cents a la
 * cuarta parte. Sobre cuerdas reales (la mas aguda es A4 = 440) con desajustes de
 * un par de cents eso sobra; sobre C7 no habria margen. El AC-001.3 esta escrito
 * sobre cuerdas, y por eso.
 */

#include "support/SyntheticSignal.h"

#include "StrobeTracker.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace wma_test {
namespace {

using wma::analysis::StrobeTracker;

constexpr int kRate = 48000;
constexpr int kThreeSeconds = 3 * kRate;

/// La tolerancia del AC-001.3.
constexpr double kToleranceCents = 0.1;

/// El desajuste de prueba: 1 cent, DIEZ VECES la tolerancia. Un desajuste por
/// debajo de la tolerancia lo pasaria un estimador que devuelva siempre 0.
constexpr double kProbeCents = 1.0;

struct Str { const char* name; double hz; };

/**
 * Las cuerdas de los instrumentos de S3, en afinacion estandar. Los valores son
 * de tabla publicada, NO calculados con la formula de la implementacion: un test
 * que computa lo esperado con el mismo codigo que prueba, prueba que el codigo es
 * igual a si mismo.
 */
const std::vector<Str>& strings() {
    static const std::vector<Str> kStrings = {
        // Guitarra estandar
        {"guitarra E2", 82.407}, {"guitarra A2", 110.000}, {"guitarra D3", 146.832},
        {"guitarra G3", 195.998}, {"guitarra B3", 246.942}, {"guitarra E4", 329.628},
        // Bajo de 5 cuerdas — el B0 es el caso que justifica todo el diseño
        {"bajo B0", 30.868}, {"bajo E1", 41.203}, {"bajo A1", 55.000},
        {"bajo D2", 73.416}, {"bajo G2", 97.999},
        // Ukelele (reentrante) — la cuerda mas aguda del catalogo
        {"ukelele G4", 391.995}, {"ukelele C4", 261.626}, {"ukelele A4", 440.000},
    };
    return kStrings;
}

void feed(StrobeTracker& t, const std::vector<float>& sig, int blockFrames = 512) {
    int i = 0;
    const int n = static_cast<int>(sig.size());
    while (i < n) {
        const int take = std::min(blockFrames, n - i);
        t.process(sig.data() + i, take);
        i += take;
    }
}

/// Una cuerda con 4 parciales armonicos, desafinada `cents` respecto de `f0`.
std::vector<float> string4(double f0, double cents, int frames, int rate = kRate) {
    return inharmonicString(detune(f0, cents), 0.0, 4, rate, frames);
}

StrobeTracker measure(double f0, double cents, int frames = kThreeSeconds) {
    StrobeTracker t;
    t.prepare(kRate);
    t.setTarget(f0);
    feed(t, string4(f0, cents, frames));
    return t;
}

/**
 * Serie de angulos muestreada cada `everyFrames`, ya DESENVUELTA: lo que interesa
 * de un strobe es cuanto giro, no en que cuadrante quedo.
 *
 * `firstMeasured` es el indice desde el que la serie significa algo. ANTES de que
 * cierre la primera ventana el angulo vale 0 por construccion, y al cerrarla
 * salta a la fase cruda del Goertzel —un valor arbitrario en (-π, π]—. Ese salto
 * es el ARRANQUE, no una discontinuidad del giro: medido, 1,35 rad en el poll 5
 * de 180. Un test de continuidad que lo incluya mide el arranque y no la
 * propiedad.
 */
struct AngleSeries {
    std::vector<double> unwrapped;
    size_t firstMeasured = 0;
};

AngleSeries unwrappedAngles(double f0, double cents, int frames,
                            int everyFrames) {
    StrobeTracker t;
    t.prepare(kRate);
    t.setTarget(f0);
    const auto sig = string4(f0, cents, frames);

    AngleSeries out;
    double unwrapped = 0.0;
    double last = 0.0;
    bool first = true;
    bool seenMeasurement = false;
    int i = 0;
    const int n = static_cast<int>(sig.size());
    while (i < n) {
        const int take = std::min(everyFrames, n - i);
        t.process(sig.data() + i, take);
        i += take;
        const double a = t.phaseAngle();
        if (first) { unwrapped = a; last = a; first = false; }
        else {
            double d = a - last;
            while (d > M_PI) d -= 2.0 * M_PI;
            while (d < -M_PI) d += 2.0 * M_PI;
            unwrapped += d;
            last = a;
        }
        out.unwrapped.push_back(unwrapped);
        if (!seenMeasurement && t.hasMeasurement()) {
            seenMeasurement = true;
            out.firstMeasured = out.unwrapped.size() - 1;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 6.1 · AC-001.3 — el numero del que cuelga el producto
// ---------------------------------------------------------------------------
TEST(StrobeTrackerTest, ItMeetsATenthOfACentOnEveryStringOfEveryInstrument) {
    for (const auto& s : strings()) {
        auto t = measure(s.hz, kProbeCents);

        ASSERT_TRUE(t.hasMeasurement()) << s.name << ": no produjo ninguna medicion";
        const double err = std::abs(t.cents() - kProbeCents);
        RecordProperty(std::string("error_cents_") + s.name, std::to_string(err));

        EXPECT_LT(err, kToleranceCents)
            << s.name << " (" << s.hz << " Hz): midio " << t.cents()
            << " cents contra " << kProbeCents << " reales — error " << err;
        EXPECT_TRUE(t.converged())
            << s.name << ": 3 s no alcanzaron para declarar convergencia (σ = "
            << t.uncertaintyCents() << ")";
    }
}

// ---------------------------------------------------------------------------
// 6.2 — la combinacion tiene que ser AL MENOS TAN BUENA como el fundamental solo
// ---------------------------------------------------------------------------
/**
 * Con ponderacion por 1/σ² esto es cierto por construccion —
 * σ²_comb = 1/Σ(1/σ²ᵢ) ≤ min σ²ᵢ— pero se mide igual: la propiedad que importa
 * es la del ERROR, y que la formula sea correcta no garantiza que este bien
 * implementada.
 */
TEST(StrobeTrackerTest, TheCombinedReadingIsNeverWorseThanTheFundamentalAlone) {
    for (const auto& s : strings()) {
        auto t = measure(s.hz, kProbeCents);
        ASSERT_TRUE(t.hasMeasurement()) << s.name;
        ASSERT_TRUE(t.partialHasMeasurement(0)) << s.name << ": el fundamental no midio";

        EXPECT_LE(t.uncertaintyCents(), t.partialUncertaintyCents(0) + 1e-12)
            << s.name << ": la σ combinada (" << t.uncertaintyCents()
            << ") es PEOR que la del fundamental solo ("
            << t.partialUncertaintyCents(0) << ")";

        const double errComb = std::abs(t.cents() - kProbeCents);
        const double errFund = std::abs(t.partialCents(0) - kProbeCents);
        RecordProperty(std::string("comb_vs_fund_") + s.name,
                       std::to_string(errComb) + " vs " + std::to_string(errFund));
        EXPECT_LT(errComb, kToleranceCents) << s.name;
    }
}

// ---------------------------------------------------------------------------
// 6.3 — la razon #1 de rastrear armonicos: el fundamental debil
// ---------------------------------------------------------------------------
TEST(StrobeTrackerTest, ItStillMeetsTheBudgetWithTheFundamentalTwentyDbDown) {
    // Las graves son donde esto pasa de verdad.
    for (const auto& s : {Str{"bajo B0", 30.868}, Str{"bajo E1", 41.203},
                          Str{"guitarra E2", 82.407}}) {
        StrobeTracker t;
        t.prepare(kRate);
        t.setTarget(s.hz);
        feed(t, stringWithWeakFundamental(detune(s.hz, kProbeCents), 0.0, 4,
                                          kRate, kThreeSeconds, 20.0));

        ASSERT_TRUE(t.hasMeasurement()) << s.name;
        const double err = std::abs(t.cents() - kProbeCents);
        RecordProperty(std::string("weak_fund_error_") + s.name, std::to_string(err));
        EXPECT_LT(err, kToleranceCents)
            << s.name << ": con el fundamental 20 dB abajo midio " << t.cents();
    }
}

// ---------------------------------------------------------------------------
// 6.3b — el fundamental AUSENTE: la prueba de que los armonicos hacen el trabajo
// ---------------------------------------------------------------------------
/**
 * 🔴 ESTE TEST EXISTE PORQUE 6.3 NO ALCANZABA.
 *
 * Con el fundamental 20 dB abajo, un tracker que apuntara sus CUATRO estimadores
 * al fundamental —en vez de a f0, 2f0, 3f0, 4f0— seguia pasando: 20 dB debilita
 * el fundamental pero no lo borra, asi que cuatro copias de una lectura mediocre
 * daban el numero igual. Medido con mutacion: apuntar los 4 parciales a f0
 * SOBREVIVIA a toda la suite, o sea que nada probaba que los armonicos se
 * estuvieran usando.
 *
 * El fundamental AUSENTE si lo prueba, y ademas es un caso real: es el
 * "fundamental faltante" de un bajo por un parlante chico, donde la altura que se
 * percibe sale enteramente de los parciales superiores.
 */
TEST(StrobeTrackerTest, ItMeasuresFromTheHarmonicsWhenTheFundamentalIsMissingEntirely) {
    for (const auto& s : {Str{"bajo B0", 30.868}, Str{"bajo E1", 41.203}}) {
        StrobeTracker t;
        t.prepare(kRate);
        t.setTarget(s.hz);
        // Parciales 2, 3 y 4 con su decaimiento 1/n; el fundamental en CERO.
        const auto sig = partialsWithAmplitudes(
            detune(s.hz, kProbeCents), 0.0,
            {0.0, 0.5 / 2.0, 0.5 / 3.0, 0.5 / 4.0}, kRate, kThreeSeconds);
        feed(t, sig);

        ASSERT_TRUE(t.hasMeasurement()) << s.name << ": sin fundamental no midio nada";
        for (int i = 0; i < StrobeTracker::kPartials; ++i) {
            RecordProperty(std::string("p") + std::to_string(i) + "_cents_" + s.name,
                           std::to_string(t.partialCents(i)));
            RecordProperty(std::string("p") + std::to_string(i) + "_sigma_" + s.name,
                           std::to_string(t.partialUncertaintyCents(i)));
        }
        const double err = std::abs(t.cents() - kProbeCents);
        RecordProperty(std::string("no_fund_error_") + s.name, std::to_string(err));
        EXPECT_LT(err, kToleranceCents)
            << s.name << ": sin fundamental midio " << t.cents()
            << " cents contra " << kProbeCents << " reales";
    }
}

// ---------------------------------------------------------------------------
// 6.3c — el caso OPUESTO: un tono puro, donde SOLO el fundamental tiene energia
// ---------------------------------------------------------------------------
/**
 * Un diapason, una flauta o una referencia electronica no tienen armonicos que
 * valgan: p1, p2 y p3 miran bins vacios y devuelven fuga. Medido a 110 Hz:
 * -35,5 / +14,2 / -7,1 cents, contra el +1,0000 del fundamental.
 *
 * O sea que la MEDIANA de los cuatro vale -3,07 — el "consenso" es de los tres
 * que no estan midiendo nada. Este test es el que fija que el descarte por
 * mediana no se pueda llevar puesto al unico parcial con señal, y es el reverso
 * exacto del caso del fundamental ausente: alla habia que descartar p0, aca hay
 * que conservarlo. Lo que los separa no es la mediana sino σ.
 */
TEST(StrobeTrackerTest, APureToneIsMeasuredFromItsFundamentalAloneWithoutBeingOutvoted) {
    for (const double hz : {110.000, 440.000}) {
        StrobeTracker t;
        t.prepare(kRate);
        t.setTarget(hz);
        feed(t, pureSine(detune(hz, kProbeCents), kRate, kThreeSeconds));

        ASSERT_TRUE(t.hasMeasurement()) << hz << " Hz: un tono puro no produjo medicion";
        const double err = std::abs(t.cents() - kProbeCents);
        RecordProperty(std::string("pure_tone_error_") + std::to_string((int)hz),
                       std::to_string(err));
        EXPECT_LT(err, kToleranceCents)
            << hz << " Hz: tono puro midio " << t.cents() << " contra "
            << kProbeCents << " reales — los parciales sin señal se impusieron";
    }
}

// ---------------------------------------------------------------------------
// 6.3d — el regimen DEGENERADO: un solo parcial con energia, en las 4 posiciones
// ---------------------------------------------------------------------------
/**
 * Generaliza 6.3b (sin fundamental) y 6.3c (tono puro) al caso completo: para
 * cada cuerda, cuatro señales donde SOLO el parcial k tiene energia. Los otros
 * tres devuelven fuga, asi que la MEDIANA la fijan los que no miden nada.
 *
 * Barrido medido: en las cuerdas medias-altas la desviacion del mejor parcial
 * respecto de la mediana no pasa de 12 cents, pero en las GRAVES —donde el rango
 * de captura es mas ancho— llega a **40,46 cents en E1 con solo el 4to parcial**.
 * El umbral de descarte es 50: 1,2x de margen. Lo que hace que ese margen fino no
 * decida nada es que al parcial mejor medido no se lo descarta nunca.
 *
 * Este test es el que quedaria en rojo si alguien tocara el umbral o esa regla.
 */
TEST(StrobeTrackerTest, ASinglePartialCarryingAllTheEnergyIsStillMeasuredCorrectly) {
    for (const auto& s : {Str{"bajo B0", 30.868}, Str{"bajo E1", 41.203},
                          Str{"guitarra E2", 82.407}, Str{"ukelele A4", 440.000}}) {
        for (int only = 0; only < StrobeTracker::kPartials; ++only) {
            std::vector<double> amps(StrobeTracker::kPartials, 0.0);
            amps[static_cast<size_t>(only)] = 0.4;

            StrobeTracker t;
            t.prepare(kRate);
            t.setTarget(s.hz);
            feed(t, partialsWithAmplitudes(detune(s.hz, kProbeCents), 0.0, amps,
                                           kRate, kThreeSeconds));

            ASSERT_TRUE(t.hasMeasurement())
                << s.name << " solo p" << only << ": no midio nada";
            const double err = std::abs(t.cents() - kProbeCents);
            RecordProperty(std::string("only_p") + std::to_string(only) + "_" + s.name,
                           std::to_string(err));
            EXPECT_LT(err, kToleranceCents)
                << s.name << " con energia SOLO en p" << only << ": midio "
                << t.cents() << " contra " << kProbeCents
                << " reales — los tres parciales de fuga se impusieron";
        }
    }
}

// ---------------------------------------------------------------------------
// 6.4 · AC-001.22 — el angulo es continuo entre polls
// ---------------------------------------------------------------------------
TEST(StrobeTrackerTest, ThePublishedAngleIsContinuousBetweenPollsAtSixtyHertz) {
    const int poll = kRate / 60;                     // 60 Hz de polling
    const auto series = unwrappedAngles(82.407, 2.0, kThreeSeconds, poll);
    const auto& a = series.unwrapped;
    ASSERT_GT(a.size(), 100u);
    ASSERT_GT(series.firstMeasured, 0u) << "nunca hubo medicion";

    // Desde la primera medicion en adelante: ahi si el angulo tiene que ser
    // continuo. El arranque se excluye a proposito y con su razon escrita.
    double maxJump = 0.0;
    size_t maxAt = 0;
    for (size_t i = series.firstMeasured + 1; i < a.size(); ++i) {
        const double d = std::abs(a[i] - a[i - 1]);
        if (d > maxJump) { maxJump = d; maxAt = i; }
    }
    RecordProperty("max_jump_rad", std::to_string(maxJump));
    RecordProperty("max_jump_at_poll", std::to_string(maxAt));
    RecordProperty("first_measured_poll", std::to_string(series.firstMeasured));

    // A 2 cents sobre E2 el avance real por ventana es ~0,05 rad. Un salto a
    // cero —el modo de falla que este test existe para atrapar— seria de hasta π.
    EXPECT_LT(maxJump, 0.5)
        << "el angulo salto " << maxJump << " rad en el poll " << maxAt
        << " (primera medicion en el " << series.firstMeasured << ")";
}

// ---------------------------------------------------------------------------
// 6.5 — el disco gira para el lado correcto
// ---------------------------------------------------------------------------
TEST(StrobeTrackerTest, TheAngleTurnsOneWayWhenSharpAndTheOtherWhenFlat) {
    const int window = 4096;
    const auto sharp = unwrappedAngles(110.000, +2.0, kThreeSeconds, window);
    const auto flat  = unwrappedAngles(110.000, -2.0, kThreeSeconds, window);

    const double turnSharp = sharp.unwrapped.back() - sharp.unwrapped.front();
    const double turnFlat  = flat.unwrapped.back()  - flat.unwrapped.front();
    RecordProperty("turn_sharp_rad", std::to_string(turnSharp));
    RecordProperty("turn_flat_rad", std::to_string(turnFlat));

    EXPECT_GT(std::abs(turnSharp), 1.0) << "sostenido: el disco practicamente no giro";
    EXPECT_GT(std::abs(turnFlat), 1.0) << "bemol: el disco practicamente no giro";

    // 🔴 EL SENTIDO SE EXIGE EN ABSOLUTO, NO SOLO QUE DIFIERAN.
    //
    // Este test decia `turnSharp * turnFlat < 0` —"giraron para lados
    // distintos"— y con eso INVERTIR EL SIGNO DE LOS DOS lo pasaba: el producto
    // no cambia. Medido con mutacion: `phaseAngle()` devolviendo el negativo
    // sobrevivia, y el doc de la etapa pedia explicitamente que ese mutante
    // muriera. Un disco que gira al reves manda a aflojar la cuerda que hay que
    // apretar, asi que "difieren" no es la propiedad: la propiedad es CUAL.
    //
    // La convencion es la del afinador, la misma que ya declara el golden de S2:
    // señal POR ENCIMA del objetivo ⇒ POSITIVO.
    EXPECT_GT(turnSharp, 0.0)
        << "una cuerda SOSTENIDA hizo girar el disco en negativo (" << turnSharp << ")";
    EXPECT_LT(turnFlat, 0.0)
        << "una cuerda BEMOL hizo girar el disco en positivo (" << turnFlat << ")";
}

// ---------------------------------------------------------------------------
// 6.6 — la velocidad de giro es proporcional a la desviacion
// ---------------------------------------------------------------------------
/**
 * Es la propiedad que hace que un strobe se lea de un vistazo: el doble de
 * desafinado gira al doble de rapido.
 */
TEST(StrobeTrackerTest, TheTurningSpeedIsProportionalToTheDeviationInCents) {
    const int window = 4096;
    const auto two  = unwrappedAngles(110.000, 2.0, kThreeSeconds, window);
    const auto four = unwrappedAngles(110.000, 4.0, kThreeSeconds, window);

    const double turnTwo  = std::abs(two.unwrapped.back()  - two.unwrapped.front());
    const double turnFour = std::abs(four.unwrapped.back() - four.unwrapped.front());
    ASSERT_GT(turnTwo, 1.0) << "el caso de 2 cents no giro lo suficiente para medir";

    const double ratio = turnFour / turnTwo;
    RecordProperty("speed_ratio_4_over_2", std::to_string(ratio));
    EXPECT_NEAR(ratio, 2.0, 0.15)
        << "4 cents giro " << ratio << "x lo que giro 2 cents, y deberia ser 2x";
}

// ---------------------------------------------------------------------------
// 6.7 · AC-001.8 — la incertidumbre decrece y declara convergencia
// ---------------------------------------------------------------------------
TEST(StrobeTrackerTest, TheUncertaintyDecreasesWhileIntegratingAndDeclaresConvergence) {
    StrobeTracker t;
    t.prepare(kRate);
    t.setTarget(146.832);
    const auto sig = string4(146.832, kProbeCents, 6 * kRate);

    std::vector<double> sigmas;
    int i = 0;
    const int n = static_cast<int>(sig.size());
    while (i < n) {
        const int take = std::min(4096, n - i);
        t.process(sig.data() + i, take);
        i += take;
        if (t.hasMeasurement()) sigmas.push_back(t.uncertaintyCents());
    }

    ASSERT_GE(sigmas.size(), 8u) << "no hubo suficientes mediciones para ver la tendencia";
    // No se exige monotonia estricta punto a punto —es un estimador estadistico y
    // una ventana puede subirla— sino que la tendencia sea a la baja y termine
    // por debajo del umbral escrito.
    EXPECT_LT(sigmas.back(), sigmas.front())
        << "la incertidumbre no bajo al integrar: " << sigmas.front()
        << " → " << sigmas.back();
    EXPECT_LE(t.uncertaintyCents(), StrobeTracker::kConvergedUncertaintyCents);
    EXPECT_TRUE(t.converged());
}

// ---------------------------------------------------------------------------
// 6.8 — al perder la señal el disco se FRENA, no se teletransporta
// ---------------------------------------------------------------------------
TEST(StrobeTrackerTest, LosingTheSignalFreezesTheAngleInsteadOfSnappingItToZero) {
    StrobeTracker t;
    t.prepare(kRate);
    t.setTarget(196.0);
    feed(t, string4(196.0, 3.0, 2 * kRate));

    ASSERT_TRUE(t.hasSignal());

    // 🔴 EL DRENAJE NO ES OPCIONAL, Y NO ES COMPLACENCIA DEL TEST.
    //
    // La ventana que estaba a medio llenar cuando corto la señal MEZCLA audio y
    // silencio, y su RMS puede seguir por encima del piso: esa ventana integra
    // una vez mas, legitimamente. Medido: sin drenar, el angulo iba de 0,7787 a
    // 0,9482 y el test acusaba de "salto" a un analisis correcto de la cola de
    // audio real. La propiedad que el AC promete es que el disco se frena cuando
    // la señal YA NO ESTA, no en el instante en que el llamador deja de empujar.
    const std::vector<float> silence(static_cast<size_t>(2 * kRate), 0.0f);
    feed(t, std::vector<float>(2 * 4096, 0.0f));   // dos ventanas de drenaje

    ASSERT_FALSE(t.hasSignal()) << "tras dos ventanas de silencio sigue diciendo que hay señal";
    const double frozen = t.phaseAngle();
    // Un angulo de cero no probaria nada: hay que haber girado.
    ASSERT_GT(std::abs(frozen), 1e-6) << "el disco no habia girado todavia";

    feed(t, silence);

    EXPECT_FALSE(t.hasSignal()) << "con silencio sigue diciendo que hay señal";
    EXPECT_DOUBLE_EQ(t.phaseAngle(), frozen)
        << "el angulo se movio durante el silencio: " << frozen
        << " → " << t.phaseAngle();
}

// ---------------------------------------------------------------------------
// 6.9 — reset()
// ---------------------------------------------------------------------------
TEST(StrobeTrackerTest, ResetMakesItIndistinguishableFromFreshlyPrepared) {
    StrobeTracker used;
    used.prepare(kRate);
    used.setTarget(110.000);
    feed(used, string4(110.000, 5.0, 2 * kRate));
    ASSERT_TRUE(used.hasMeasurement());
    used.reset();

    StrobeTracker fresh;
    fresh.prepare(kRate);
    fresh.setTarget(110.000);

    EXPECT_EQ(used.hasMeasurement(), fresh.hasMeasurement());
    EXPECT_EQ(used.hasSignal(), fresh.hasSignal());
    EXPECT_DOUBLE_EQ(used.phaseAngle(), fresh.phaseAngle());
    EXPECT_DOUBLE_EQ(used.targetHz(), fresh.targetHz())
        << "reset() se llevo el objetivo: quedaria vivo midiendo contra nada";

    // Y lo que de verdad importa: la MISMA señal tiene que dar la MISMA lectura.
    const auto sig = string4(110.000, 1.5, 2 * kRate);
    feed(used, sig);
    feed(fresh, sig);
    ASSERT_TRUE(fresh.hasMeasurement());
    EXPECT_DOUBLE_EQ(used.cents(), fresh.cents());
    EXPECT_DOUBLE_EQ(used.uncertaintyCents(), fresh.uncertaintyCents());
}

}  // namespace
}  // namespace wma_test
