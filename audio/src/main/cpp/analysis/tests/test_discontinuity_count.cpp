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

    /**
     * Discontinuidad SOSTENIDA, y **determinista**: se estampa una costura de
     * captura `ticksAhead` vueltas ADELANTE del lector y se lo alimenta con
     * audio limpio hasta que la cruce.
     *
     * 🔴 POR QUE POR COSTURA Y NO POR DESBORDE, QUE ES LO QUE HABIA.
     * El desborde depende de GANARLE AL LECTOR, y eso es una carrera: la
     * primera version de este test dio rojo en el CI de macOS y **tenia razon el
     * CI** —afirmaba que un desborde sostenido cuenta UNA vez, y contaba dos,
     * porque entre rafaga y rafaga el estimador SI puede recuperar una medicion
     * (el desborde entrega audio de cuerda valido, solo que con huecos) y
     * entonces la rafaga siguiente es un hueco NUEVO de verdad. Contar 2 era
     * CORRECTO; el test estaba mal. Los dos intentos de arreglarlo apretando
     * umbrales dieron 14 de 60 y 3 de 60, siempre nombrando su propia premisa:
     * es la clase que este repo ya midio en MINI-006 —"no hay umbral que
     * separe"— y el arreglo no es un umbral mejor, es sacar la carrera.
     *
     * La costura no compite con nadie: `crossedSeam` se mantiene VERDADERO en
     * cada vuelta mientras `readPosition() < seam` (`AnalysisThread.cpp`), asi
     * que el hueco dura exactamente las vueltas que uno elige. Por evento suma
     * 1; por tick sumaria una por vuelta. Los dos ejes entran por el MISMO
     * gancho (`noteInputDiscontinuity`), asi que el mutante sigue expuesto.
     */
    bool sustainedSeam(int ticksAhead) {
        const uint64_t ahead =
            static_cast<uint64_t>(ticksAhead) * AnalysisThread::kDrainFrames;
        mRing.reportCaptureDiscontinuity(ahead);
        const uint64_t seam = mRing.captureSeamPosition();
        if (seam == 0) return false;
        // Alimenta hasta que el lector CRUCE la costura. Es una condicion sobre
        // el propio mecanismo, no una duracion ni una carrera.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (mRing.readPosition() < seam) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            if (mRing.availableFrames() + static_cast<uint32_t>(kFrames)
                    <= AnalysisRing::kCapacityFrames) {
                const auto blk = stringBlock(mWritten * kFrames);
                mRing.writeStereo(blk.data(), kFrames);
                ++mWritten;
            }
        }
        // 🔴 HAY QUE ESPERAR UNA PUBLICACION NUEVA, y no es un detalle.
        // Mientras la costura esta pendiente el lazo hace `continue` ANTES de
        // publicar (`AnalysisThread.cpp`), asi que durante todo el hueco el
        // snapshot se queda CONGELADO en lo de antes. Leer el contador apenas se
        // cruza la costura devuelve el valor viejo — medido: daba 0 y el test
        // acusaba al motor de no contar.
        return feedClean(3);
    }

    uint64_t readPosition() const { return mRing.readPosition(); }

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

    // Se provoca el hueco y se deja correr el analisis SIN MIRAR una sola vez:
    // exactamente el escenario del AC. `sustainedSeam` es determinista — no
    // depende de ganarle al lector, que es lo que hacia escamoso al desborde.
    ASSERT_TRUE(b.sustainedSeam(1)) << "premisa rota: no se pudo provocar el hueco";

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
TEST(DiscontinuityCount, ASustainedBreakCountsOnce) {
    // Vueltas que el hueco dura. Por evento suma 1; por tick sumaria ~kTicks.
    constexpr int kTicks = 20;
    Bench b;
    ASSERT_TRUE(b.convergeOnCleanAudio());
    const double before = b.discontinuityCount();
    ASSERT_GE(before, 0.0);

    ASSERT_TRUE(b.sustainedSeam(kTicks))
        << "premisa rota: el lector nunca cruzo la costura";

    // Que el hueco haya durado de verdad: si el lector la hubiera cruzado en una
    // sola vuelta, "por evento" y "por tick" darian lo mismo y este test no
    // distinguiria nada — que es como el mutante sobrevivio la primera vez.
    ASSERT_GE(b.readPosition(),
              static_cast<uint64_t>(kTicks) * AnalysisThread::kDrainFrames)
        << "premisa rota: el hueco no duro las vueltas que se pidieron";

    EXPECT_EQ(b.discontinuityCount(), before + 1.0)
        << "un hueco SOSTENIDO durante " << kTicks << " vueltas conto "
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

    ASSERT_TRUE(b.sustainedSeam(2)) << "premisa rota: no se pudo provocar el hueco";
    // Recuperacion larga: el estimador tiene que volver a tener medicion propia.
    ASSERT_TRUE(b.feedClean(40));

    EXPECT_EQ(b.liveMark(), 0.0)
        << "el flag vivo se quedo arriba tras la recuperacion: se volvio un latch, "
           "y con eso la guarda de AC-009.2 no vuelve a declarar convergido nunca";
    EXPECT_GT(b.discontinuityCount(), before)
        << "el contador tiene que seguir arriba: es la memoria de que paso";
}

}  // namespace
