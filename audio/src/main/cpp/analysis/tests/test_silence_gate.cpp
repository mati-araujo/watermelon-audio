/**
 * REQ-014 S1 — la compuerta de ausencia de señal.
 *
 * Cubre AC-014.1 (declarar ausencia cuando no hay nada que afinar), AC-014.2
 * (el gemelo: no comprar esa ausencia con sensibilidad) y AC-014.5 (el estado y
 * el valor no pueden contradecirse en un mismo snapshot).
 *
 * 🔴 POR QUE ESTOS TESTS NO MIRAN UN NIVEL, Y ESTA MEDIDO QUE NO PUEDEN
 * ---------------------------------------------------------------------
 * La reproduccion de S1 midio que **no existe** un piso absoluto que cumpla
 * AC-014.1 y AC-014.2 a la vez:
 *
 *   - para declarar ausencia con el ruido de habitacion reportado desde
 *     hardware, el piso tendria que estar POR ENCIMA de rms 0,0070;
 *   - para no romper una cuerda limpia que HOY se mide bien en una habitacion
 *     silenciosa, tendria que estar POR DEBAJO de rms 0,0016 (medido:
 *     `CONVERGED` 50 de 50 a ese nivel).
 *
 * Estan 4,4x separados en la direccion imposible. Por eso la compuerta mira la
 * EVIDENCIA TONAL —la misma con la que ya se decide publicar el `cents`— y por
 * eso `TheQuietRoomKeepsMeasuring` es parte del contrato y no un extra: es el
 * test que un piso absoluto no puede pasar.
 *
 * 🔴 Y POR ESO SE PRUEBAN CUATRO RUIDOS, NO UNO
 * ----------------------------------------------
 * El ruido BLANCO es el estimulo mas facil de rechazar para la deteccion
 * gruesa. Medido en la reproduccion: blanco da claridad 0,065 y **el zumbido de
 * red da 0,994 con la gruesa enganchada a 50 Hz**. Una compuerta que mire
 * "hay altura" sin mirar CUAL pasa el caso blanco y deja vivo el eterno
 * `MEASURING` en la habitacion con zumbido, que es la mitad de las habitaciones
 * reales.
 */

#include "tests/support/TestWait.h"
#include "../AnalysisRing.h"
#include "../AnalysisSnapshot.h"
#include "../AnalysisThread.h"
#include "../FastModeTracker.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

using namespace wma::analysis;

constexpr int kRate   = 44100;
constexpr int kFrames = 1024;
constexpr double kE4  = 329.6276;

/// El piso de ruido ambiente medido sobre hardware en la habitacion del
/// reporte. NO es una constante del motor: es un DATO del escenario, y esta acá
/// para que el test falle si alguien lo cambia creyendo que es un umbral.
constexpr double kReportedRoomNoiseRms = 0.0070;

// --- generadores -------------------------------------------------------------

/// Ruido 1/f. Una habitacion real no suena a ruido blanco: la energia se va a
/// las bajas, que es justo donde la deteccion gruesa se puede confundir.
struct Pink {
    unsigned st = 987654321u;
    double b0 = 0.0, b1 = 0.0, b2 = 0.0;
    double next() {
        st = st * 1664525u + 1013904223u;
        const double w = static_cast<double>(st >> 8) / 8388608.0 - 1.0;
        b0 = 0.99765 * b0 + w * 0.0990460;
        b1 = 0.96300 * b1 + w * 0.2965164;
        b2 = 0.57000 * b2 + w * 1.0526913;
        return (b0 + b1 + b2 + w * 0.1848) * 0.2;
    }
};

struct White {
    unsigned st = 4242u;
    double next() {
        st = st * 1664525u + 1013904223u;
        return static_cast<double>(st >> 8) / 8388608.0 - 1.0;
    }
};

/// Cuerda de 4 parciales armonicos. `startFrame` NO es opcional: generar cada
/// bloque desde fase 0 mete una discontinuidad artificial cada `kFrames`
/// muestras y el estimador mide ESO (ver el KDoc de `stringBlock` en
/// test_analysis_thread.cpp, donde costo un falso rojo).
inline double stringSample(double f0, long frame, double amp) {
    double s = 0.0;
    for (int n = 1; n <= 4; ++n) {
        s += (amp / n) * std::sin(2.0 * M_PI * f0 * n * static_cast<double>(frame) / kRate);
    }
    return s;
}

/// Escala el bloque para que quede al `rms` pedido. El escenario se especifica
/// por NIVEL —que es lo que el reporte midio— y no por amplitud de generador.
template <typename Sample>
std::vector<float> blockAtRms(Sample sample, long startFrame, double wantRms) {
    std::vector<double> raw(static_cast<size_t>(kFrames));
    double sq = 0.0;
    for (int i = 0; i < kFrames; ++i) {
        raw[static_cast<size_t>(i)] = sample(startFrame + i);
        sq += raw[static_cast<size_t>(i)] * raw[static_cast<size_t>(i)];
    }
    const double have = std::sqrt(sq / kFrames);
    const double k = have > 0.0 ? wantRms / have : 0.0;
    std::vector<float> b(static_cast<size_t>(kFrames) * 2);
    for (int i = 0; i < kFrames; ++i) {
        const float v = static_cast<float>(raw[static_cast<size_t>(i)] * k);
        b[static_cast<size_t>(i) * 2]     = v;
        b[static_cast<size_t>(i) * 2 + 1] = v;
    }
    return b;
}

std::vector<float> stringBlock(long startFrame, double amp) {
    std::vector<float> b(static_cast<size_t>(kFrames) * 2, 0.0f);
    for (int i = 0; i < kFrames; ++i) {
        const float v = static_cast<float>(stringSample(kE4, startFrame + i, amp));
        b[static_cast<size_t>(i) * 2]     = v;
        b[static_cast<size_t>(i) * 2 + 1] = v;
    }
    return b;
}

// --- observacion -------------------------------------------------------------

/**
 * Lo observado a lo largo de una fase, en AGREGADO.
 *
 * 🔴 Se cuentan TODOS los snapshots, no se mira el ultimo. El defecto de
 * AC-014.5 dura 3 ticks de 40: un test que afirme sobre el estado final no lo
 * ve, y quedaria verde con el defecto vivo.
 */
struct Observed {
    int samples = 0;
    int publishedCents = 0;
    int noSignal = 0;
    int contradictions = 0;   // AC-014.5: NO_SIGNAL con un cents numerico
    int transitions = 0;      // parpadeo del estado
    double lastDetectedHz = 0.0;
    int lastState = -1;
};

/// Un banco que arranca el analisis con un objetivo puesto y alimenta el ring
/// esperando LUGAR — nunca audio no contiguo, que es lo que fabricaria una
/// lectura bien formada y equivocada (REQ-005 S3).
class Bench {
public:
    Bench() {
        mRing.setCaptureRate(kRate);
        mThread.setTargetHz(kE4);
        mThread.start(kRate);
    }
    ~Bench() { mThread.stop(); }

    template <typename MakeBlock>
    Observed feed(MakeBlock make, int blocks) {
        Observed o;
        for (int k = 0; k < blocks; ++k) {
            const bool room = wma_test::waitUntil([&] {
                return mRing.availableFrames() + static_cast<uint32_t>(kFrames)
                       <= AnalysisRing::kCapacityFrames;
            }, std::chrono::seconds(5));
            if (!room) {
                ADD_FAILURE() << "el ring nunca tuvo lugar: la muestra salio corta";
                return o;
            }
            const auto blk = make(mCursor);
            mRing.writeStereo(blk.data(), kFrames);
            mCursor += kFrames;

            // Se espera a que el analisis CONSUMA este bloque, por condicion.
            // Si no llega dentro del techo el snapshot no se cuenta: contar una
            // observacion repetida inflaria los denominadores.
            const double before = mSeenFrames;
            if (!wma_test::waitUntil([&] { return analysedFrames() > before; },
                                     std::chrono::seconds(2))) {
                continue;
            }
            mSeenFrames = analysedFrames();

            float v[kSnapshotValueCount];
            if (!mSnap.read(v)) continue;
            const int st = static_cast<int>(v[kSnapState]);
            const bool numeric = !std::isnan(v[kSnapCents]);

            ++o.samples;
            if (numeric) ++o.publishedCents;
            if (st == kStateNoSignal) ++o.noSignal;
            if (st == kStateNoSignal && numeric) ++o.contradictions;
            if (o.lastState >= 0 && st != o.lastState) ++o.transitions;
            o.lastState = st;
            o.lastDetectedHz = static_cast<double>(v[kSnapDetectedHz]);
        }
        return o;
    }

    /// Lleva el motor a una lectura convergida sobre una cuerda a nivel de pua.
    /// Es la PRECONDICION de todo lo demas: sin haber medido primero, "dejar de
    /// publicar" seria trivial.
    void pluckUntilConverged() {
        const Observed o = feed([](long c) { return stringBlock(c, 0.5); }, 40);
        ASSERT_GT(o.publishedCents, 0) << "el banco no llego a medir: sin esa "
                                          "precondicion los tests de ausencia no prueban nada";
    }

    double analysedFrames() {
        float v[kSnapshotValueCount];
        return mSnap.read(v) ? static_cast<double>(v[kSnapFramesAnalyzed]) : 0.0;
    }

private:
    AnalysisRing mRing;
    AnalysisSnapshot mSnap;
    AnalysisThread mThread{mRing, mSnap};
    long mCursor = 0;
    double mSeenFrames = -1.0;
};

// ---------------------------------------------------------------------------
// AC-014.1 — la ausencia se declara
// ---------------------------------------------------------------------------

/**
 * Los cuatro modelos de ruido, al nivel medido sobre hardware. En los cuatro el
 * motor tiene que decir `kStateNoSignal`: el usuario dejo de tocar.
 *
 * Antes de REQ-014 los cuatro publicaban `kStateMeasuring` para siempre — "ya
 * casi", en una habitacion vacia.
 */
class RoomNoise : public ::testing::TestWithParam<int> {};

TEST_P(RoomNoise, TheEngineDeclaresAbsenceWhenOnlyRoomNoiseRemains) {
    Bench b;
    b.pluckUntilConverged();

    Pink pink;
    White white;
    const int kind = GetParam();
    auto sample = [&](long t) -> double {
        const double ph = 2.0 * M_PI * 50.0 * static_cast<double>(t) / kRate;
        switch (kind) {
            case 0: return white.next();
            case 1: return pink.next();
            case 2: return pink.next()
                         + 0.5 * (std::sin(ph) + 0.5 * std::sin(2 * ph) + 0.33 * std::sin(3 * ph));
            default: return 0.2 * pink.next()
                         + std::sin(ph) + 0.6 * std::sin(2 * ph) + 0.4 * std::sin(3 * ph);
        }
    };

    const Observed o = b.feed([&](long c) {
        return blockAtRms(sample, c, kReportedRoomNoiseRms);
    }, 60);

    ASSERT_GE(o.samples, 40) << "muestra corta: el veredicto no se puede emitir";

    // La cola de la pua todavia vive en la ventana del estimador durante los
    // primeros ticks. Lo que se afirma es el REGIMEN, no el instante: la mayoria
    // amplia de las observaciones tiene que declarar ausencia.
    EXPECT_GT(o.noSignal, o.samples * 3 / 4)
        << "solo " << o.noSignal << " de " << o.samples
        << " snapshots declararon ausencia con la habitacion vacia";
    EXPECT_EQ(o.lastState, kStateNoSignal)
        << "el estado final no es ausencia (gruesa: " << o.lastDetectedHz << " Hz)";
}

INSTANTIATE_TEST_SUITE_P(FourNoiseModels, RoomNoise, ::testing::Values(0, 1, 2, 3),
                         [](const ::testing::TestParamInfo<int>& i) {
                             switch (i.param) {
                                 case 0:  return std::string("White");
                                 case 1:  return std::string("Pink");
                                 case 2:  return std::string("PinkPlusMainsHum");
                                 default: return std::string("MainsHumDominant");
                             }
                         });

TEST(SilenceGate, DigitalSilenceIsDeclaredAbsent) {
    Bench b;
    b.pluckUntilConverged();
    const Observed o = b.feed([](long) {
        return std::vector<float>(static_cast<size_t>(kFrames) * 2, 0.0f);
    }, 40);
    ASSERT_GE(o.samples, 30);
    EXPECT_EQ(o.lastState, kStateNoSignal);
}

// ---------------------------------------------------------------------------
// AC-014.2 — el gemelo. Sin esto, apagar el motor entero pasa la etapa.
// ---------------------------------------------------------------------------

/**
 * 🔴 ESTE ES EL TEST QUE HACE QUE LOS DE ARRIBA SIGNIFIQUEN ALGO.
 *
 * Callar siempre satisface cualquier test de "no publiques basura". El unico
 * modo de que "declara ausencia" sea una afirmacion es exigir, con la misma
 * fuerza, que NO la declare cuando todavia hay una cuerda sonando.
 *
 * El punto esta MEDIDO: cuerda a amp 0,010 sobre ruido rosa al piso reportado
 * da rms 0,010237 con la gruesa en 332,0 Hz, y el motor publicaba `CONVERGED`
 * 40 de 40 antes de este REQ. Si el arreglo lo apaga, el arreglo esta mal.
 */
TEST(SilenceGate, ADecayingStringOverRoomNoiseKeepsMeasuring) {
    Bench b;
    b.pluckUntilConverged();

    Pink pink;
    const Observed o = b.feed([&](long c) {
        auto blk = blockAtRms([&](long) { return pink.next(); }, c, kReportedRoomNoiseRms);
        const auto str = stringBlock(c, 0.010);
        for (size_t i = 0; i < blk.size(); ++i) blk[i] += str[i];
        return blk;
    }, 40);

    ASSERT_GE(o.samples, 30);
    EXPECT_EQ(o.noSignal, 0)
        << "el motor declaro ausencia " << o.noSignal << " veces con la cuerda todavia sonando"
        << " (gruesa: " << o.lastDetectedHz << " Hz)";
    EXPECT_GT(o.publishedCents, o.samples * 3 / 4)
        << "la ausencia se pago con sensibilidad: solo " << o.publishedCents
        << " de " << o.samples << " snapshots publicaron una lectura";
}

/**
 * El segundo gemelo, y el que un piso ABSOLUTO no puede pasar.
 *
 * En una habitacion silenciosa el motor mide bien una cuerda a rms 0,001649 —
 * medido, `CONVERGED` 50 de 50. Cualquier piso que declare ausencia sobre el
 * ruido de 0,0070 del reporte apagaria esto. Que este test y
 * `TheEngineDeclaresAbsenceWhenOnlyRoomNoiseRemains` pasen JUNTOS es la prueba
 * de que la compuerta no es de nivel.
 */
TEST(SilenceGate, TheQuietRoomKeepsMeasuringWellBelowTheReportedNoiseFloor) {
    Bench b;
    b.pluckUntilConverged();

    const Observed o = b.feed([](long c) { return stringBlock(c, 0.002); }, 40);

    ASSERT_GE(o.samples, 30);
    // El nivel de este estimulo esta POR DEBAJO del ruido de la habitacion del
    // reporte: si el veredicto saliera de un umbral de nivel, este test y el de
    // la habitacion vacia no podrian estar los dos en verde.
    EXPECT_EQ(o.noSignal, 0)
        << "el motor apago una cuerda audible en una habitacion silenciosa:"
           " la compuerta volvio a ser de nivel";
    EXPECT_GT(o.publishedCents, o.samples * 3 / 4);
}

// ---------------------------------------------------------------------------
// AC-014.5 — el estado y el valor no pueden contradecirse
// ---------------------------------------------------------------------------

/**
 * Determinista, 5 de 5 corridas antes del arreglo: tras converger, con silencio
 * digital, 3 de 40 snapshots publicaban `kStateNoSignal` **con un cents
 * numerico**, uno de ellos como `CONVERGED`.
 *
 * No era una carrera: `haveReading` no miraba el `rms` y el `rms` no tocaba el
 * valor, asi que eran dos compuertas con ventanas distintas —el bloque contra
 * los 4096 frames del estimador— que se pisaban en la transicion.
 */
TEST(SilenceGate, NoSnapshotEverDeclaresAbsenceAndPublishesANumber) {
    Bench b;
    b.pluckUntilConverged();
    const Observed o = b.feed([](long) {
        return std::vector<float>(static_cast<size_t>(kFrames) * 2, 0.0f);
    }, 40);

    ASSERT_GE(o.samples, 30);
    EXPECT_EQ(o.contradictions, 0)
        << o.contradictions << " snapshots dijeron 'sin señal' y publicaron un numero";
}

/// El mismo invariante sobre la transicion mas dificil: la cuerda que se apaga
/// gradualmente, donde las dos compuertas se cruzan.
TEST(SilenceGate, TheInvariantHoldsAcrossAGradualDecay) {
    Bench b;
    b.pluckUntilConverged();

    int contradictions = 0;
    for (double amp : {0.2, 0.08, 0.03, 0.012, 0.005, 0.002, 0.0008, 0.0}) {
        const Observed o = b.feed([amp](long c) { return stringBlock(c, amp); }, 12);
        contradictions += o.contradictions;
    }
    EXPECT_EQ(contradictions, 0)
        << contradictions << " snapshots se contradijeron durante el decaimiento";
}

}  // namespace
