/**
 * REQ-014 S3 — la marca de discontinuidad deja de perderse (AC-014.4).
 *
 * 🔴 POR QUE ESTOS TESTS NO MIRAN DURANTE, Y ESA ES LA PARTE QUE IMPORTA
 * ----------------------------------------------------------------------
 * El flag vivo `kSnapInputDiscontinuity` sube con el hueco y **baja solo**
 * cuando la integracion se recupera. Verlo depende de cuantas veces el
 * consumidor alcanzo a mirar entre una publicacion y la siguiente — y con la
 * maquina cargada eso son 2 o 3 publicaciones, asi que verlo es un volado.
 *
 * MINI-006 intento arreglarlo DESDE EL TEST tres veces y las tres midieron
 * peor: la premisa sobre alimentacion truncada quedo refutada (`trunc=0` en 50
 * corridas), esperar por condicion es imposible (al terminar la alimentacion el
 * ring se vacia, esperar no produce publicaciones nuevas: 3 rojas de 12 SIN
 * carga), y el umbral por conteo no discrimina (sana minima 3, rota 2).
 *
 * Por eso el arreglo es de producto y estos tests miran **solo al principio y
 * al final**: es la forma que reproduce "el consumidor no estaba mirando", y es
 * la que no puede volverse escamosa, porque no depende de atrapar un instante.
 */

#include "tests/support/TestWait.h"
#include "../AnalysisRing.h"
#include "../AnalysisSnapshot.h"
#include "../AnalysisThread.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

using namespace wma::analysis;

constexpr int kRate   = 44100;
constexpr int kFrames = 1024;
constexpr double kTarget = 329.6276;

std::vector<float> stringBlock(long startFrame) {
    std::vector<float> b(static_cast<size_t>(kFrames) * 2, 0.0f);
    for (int i = 0; i < kFrames; ++i) {
        double s = 0.0;
        const double t = static_cast<double>(startFrame + i);
        for (int n = 1; n <= 4; ++n) {
            s += (0.5 / n) * std::sin(2.0 * M_PI * kTarget * n * t / kRate);
        }
        b[static_cast<size_t>(i) * 2]     = static_cast<float>(s);
        b[static_cast<size_t>(i) * 2 + 1] = static_cast<float>(s);
    }
    return b;
}

/// Banco con la misma disciplina que el resto: alimenta esperando LUGAR, para
/// no fabricar audio no contiguo sin querer (REQ-005 S3).
class Bench {
public:
    Bench() {
        mThread.setTargetHz(kTarget);
        mThread.start(kRate);
    }
    ~Bench() { mThread.stop(); }

    /// Alimenta `blocks` bloques SIN pisar nada. Devuelve false si nunca hubo lugar.
    bool feedClean(int blocks) {
        for (int k = 0; k < blocks; ++k) {
            if (!wma_test::waitUntil([&] {
                    return mRing.availableFrames() + static_cast<uint32_t>(kFrames)
                           <= AnalysisRing::kCapacityFrames;
                }, std::chrono::seconds(5))) {
                return false;
            }
            const auto blk = stringBlock(mWritten * kFrames);
            mRing.writeStereo(blk.data(), kFrames);
            ++mWritten;
            waitForOneMoreTick();
        }
        return true;
    }

    /// UNA rafaga de mas de un ring entero, sin esperar lugar: el desborde que
    /// rompe la continuidad. Es la misma tecnica que usa REQ-009.
    bool burstOverrun() {
        if (!wma_test::waitUntil([&] { return mRing.availableFrames() == 0; },
                                 std::chrono::seconds(5))) {
            return false;
        }
        const int burst = 2 * AnalysisRing::kCapacityFrames / kFrames;
        for (int i = 0; i < burst; ++i) {
            const auto blk = stringBlock(mWritten * kFrames);
            mRing.writeStereo(blk.data(), kFrames);
            ++mWritten;
        }
        return true;
    }

    /**
     * Desborde SOSTENIDO: `rounds` rafagas seguidas, cada una de mas de un ring
     * entero, dejando que el analisis de una vuelta entre medio.
     *
     * 🔴 ES DISTINTO DE `burstOverrun()`, Y LA DIFERENCIA LA ENCONTRO UN MUTANTE.
     * Una sola rafaga produce `droppedDelta > 0` en UNA sola vuelta del lazo,
     * asi que "contar por evento" y "contar por tick" dan lo MISMO y el mutante
     * que saca el chequeo de flanco sobrevive. Con varias vueltas seguidas los
     * dos se separan: por evento suma 1, por tick suma una por vuelta.
     *
     * Entre rondas NO hay audio limpio, asi que el estimador nunca recupera una
     * medicion propia y la marca no baja: es un unico hueco, sostenido.
     */
    bool sustainedOverrun(int rounds) {
        if (!wma_test::waitUntil([&] { return mRing.availableFrames() == 0; },
                                 std::chrono::seconds(5))) {
            return false;
        }
        const int burst = 2 * AnalysisRing::kCapacityFrames / kFrames;
        for (int r = 0; r < rounds; ++r) {
            for (int i = 0; i < burst; ++i) {
                const auto blk = stringBlock(mWritten * kFrames);
                mRing.writeStereo(blk.data(), kFrames);
                ++mWritten;
            }
            waitForOneMoreTick();
        }
        return true;
    }

    double droppedFrames() {
        float o[kSnapshotValueCount];
        return mSnap.read(o) ? static_cast<double>(o[kSnapDroppedFrames]) : -1.0;
    }

    bool convergeOnCleanAudio() {
        return wma_test::waitUntil([&] {
            for (int i = 0; i < 4; ++i) {
                if (mRing.availableFrames() + static_cast<uint32_t>(kFrames)
                        > AnalysisRing::kCapacityFrames) {
                    break;
                }
                const auto blk = stringBlock(mWritten * kFrames);
                mRing.writeStereo(blk.data(), kFrames);
                ++mWritten;
            }
            float o[kSnapshotValueCount];
            return mSnap.read(o) && static_cast<int>(o[kSnapState]) == kStateConverged;
        }, std::chrono::seconds(10));
    }

    double discontinuityCount() {
        float o[kSnapshotValueCount];
        return mSnap.read(o) ? static_cast<double>(o[kSnapDiscontinuityCount]) : -1.0;
    }
    double liveMark() {
        float o[kSnapshotValueCount];
        return mSnap.read(o) ? static_cast<double>(o[kSnapInputDiscontinuity]) : -1.0;
    }
    void waitForOneMoreTick() {
        const double before = analysed();
        wma_test::waitUntil([&] { return analysed() > before; }, std::chrono::seconds(2));
    }
    double analysed() {
        float o[kSnapshotValueCount];
        return mSnap.read(o) ? static_cast<double>(o[kSnapFramesAnalyzed]) : 0.0;
    }

private:
    AnalysisRing mRing;
    AnalysisSnapshot mSnap;
    AnalysisThread mThread{mRing, mSnap};
    long mWritten = 0;
};

// ---------------------------------------------------------------------------
// AC-014.4
// ---------------------------------------------------------------------------

/**
 * El corazon de la etapa: el hueco pasa **mientras nadie mira**, y despues se
 * puede saber que paso.
 *
 * No hay un solo sondeo entre el `antes` y el `despues`. Si esto pasa, pasa
 * siempre: no depende de atrapar la publicacion contaminada.
 */
TEST(DiscontinuityCount, ABreakThatHappenedWhileNobodyWasLookingIsStillReportable) {
    Bench b;
    ASSERT_TRUE(b.convergeOnCleanAudio())
        << "premisa rota: el motor nunca convergio con audio limpio";

    const double before = b.discontinuityCount();
    ASSERT_GE(before, 0.0) << "el snapshot no publico el contador";

    ASSERT_TRUE(b.burstOverrun()) << "premisa rota: no se pudo provocar el desborde";
    // Se deja correr el analisis SIN MIRAR: exactamente el escenario del AC.
    ASSERT_TRUE(b.feedClean(8)) << "premisa rota: no hubo lugar para el tramo de recuperacion";

    const double after = b.discontinuityCount();
    EXPECT_GT(after, before)
        << "la captura perdio continuidad y el consumidor no lo puede saber despues: "
           "antes=" << before << " despues=" << after;
}

/**
 * 🔴 EL GEMELO. Un contador que incrementa siempre pasa el test de arriba.
 */
TEST(DiscontinuityCount, WithoutABreakTheCounterDoesNotMove) {
    Bench b;
    ASSERT_TRUE(b.convergeOnCleanAudio());
    const double before = b.discontinuityCount();
    ASSERT_GE(before, 0.0);
    ASSERT_TRUE(b.feedClean(20)) << "premisa rota: no hubo lugar para el tramo limpio";
    EXPECT_EQ(b.discontinuityCount(), before)
        << "el contador se movio sin que hubiera un solo hueco";
}

/**
 * Cuenta EVENTOS, no vueltas del lazo. Un desborde sostenido es UN hueco: si
 * contara por tick, el numero no significaria nada y el consumidor no podria
 * distinguir "se rompio una vez" de "el lazo dio muchas vueltas".
 */
TEST(DiscontinuityCount, ASustainedOverrunCountsOnce) {
    constexpr int kRounds = 6;
    Bench b;
    ASSERT_TRUE(b.convergeOnCleanAudio());
    const double before = b.discontinuityCount();
    ASSERT_GE(before, 0.0);
    const double droppedBefore = b.droppedFrames();

    ASSERT_TRUE(b.sustainedOverrun(kRounds));

    // 🔴 PREMISA, Y NO ES DECORATIVA: sin ella el test pasa por VACIO si el
    // desborde ocurrio en una sola vuelta del lazo, y entonces no distingue
    // "por evento" de "por tick" — que es justo lo que viene a afirmar.
    // Cada ronda escribe 2 rings enteros, asi que pisar mucho mas de un ring
    // prueba que hubo varias vueltas con desborde.
    const double droppedAfter = b.droppedFrames();
    ASSERT_GT(droppedAfter - droppedBefore,
              static_cast<double>(AnalysisRing::kCapacityFrames) * 2.0)
        << "premisa rota: el desborde no fue sostenido, asi que este test no puede "
           "distinguir contar por evento de contar por tick";

    ASSERT_TRUE(b.feedClean(8));
    EXPECT_EQ(b.discontinuityCount(), before + 1.0)
        << "un desborde SOSTENIDO de " << kRounds << " rondas conto "
        << (b.discontinuityCount() - before) << " veces: se esta contando por tick";
}

/**
 * 🔴 EL FLAG VIVO NO PUEDE HABERSE VUELTO UN LATCH POR LA PUERTA DE ATRAS.
 *
 * `kSnapInputDiscontinuity` significa *"la lectura que estas mirando arrastra
 * un hueco"* (AC-009.3) y **tiene que bajar** cuando la integracion se
 * recupera. Un contador acumulado al lado no puede cambiar eso: si el flag se
 * quedara arriba, la guarda de AC-009.2 se trabaria y el motor no volveria a
 * declarar convergido nunca — que es exactamente el defecto que el KDoc de
 * `kSnapDroppedFrames` describe.
 */
TEST(DiscontinuityCount, TheLiveMarkStillComesBackDownWhileTheCounterStaysUp) {
    Bench b;
    ASSERT_TRUE(b.convergeOnCleanAudio());
    const double before = b.discontinuityCount();

    ASSERT_TRUE(b.burstOverrun());
    // Recuperacion larga: el estimador tiene que volver a tener medicion propia.
    ASSERT_TRUE(b.feedClean(40));

    EXPECT_EQ(b.liveMark(), 0.0)
        << "el flag vivo se quedo arriba tras la recuperacion: se volvio un latch, "
           "y con eso la guarda de AC-009.2 no vuelve a declarar convergido nunca";
    EXPECT_GT(b.discontinuityCount(), before)
        << "el contador tiene que seguir arriba: es la memoria de que paso";
}

}  // namespace
