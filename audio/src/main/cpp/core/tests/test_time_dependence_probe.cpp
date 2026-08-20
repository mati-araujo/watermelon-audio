/**
 * test_time_dependence_probe.cpp — los dos controles del instrumento de REQ-002.
 *
 * POR QUE UN INSTRUMENTO NECESITA SUS PROPIOS CONTROLES
 * -----------------------------------------------------
 * `scripts/check-time-dependence.sh` corre la suite con las esperas ciegas
 * colapsadas y reporta que tests cambian de veredicto. Un instrumento asi tiene
 * un modo de falla silencioso y muy feo: **si deja de colapsar las esperas, no
 * reporta nada y eso se lee como "no hay defectos"**. Verde por ceguera.
 *
 * Este repo ya se comio esa leccion dos veces —el arnes de mutacion cuyo parser
 * de fallos estaba roto y devolvia "sobrevive" para las seis guardas, y el lint
 * de RT cuya cobertura se encogia sola— y las dos veces la salida fue la misma:
 * un CONTROL POSITIVO, algo que TIENE que fallar. Si no falla, el roto es el
 * instrumento y no el codigo.
 *
 * Los dos tests de abajo pasan con la escala normal, asi que la suite no cambia
 * de color por tenerlos. Bajo `WMA_TEST_WAIT_SCALE=0` se separan, y esa
 * separacion es lo que el script verifica ANTES de creerle una sola palabra
 * sobre el resto de la suite.
 */

#include "tests/support/TestWait.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

namespace wma_test {
namespace {

/// Un trabajador que produce su efecto despues de un rato, como el worker de
/// eventos del looper: no hay forma de saber cuando, solo de mirar si ya paso.
class LateWorker {
public:
    void start(std::chrono::milliseconds after) {
        mThread = std::thread([this, after] {
            std::this_thread::sleep_for(after);
            mDone.store(true, std::memory_order_release);
        });
    }
    ~LateWorker() { if (mThread.joinable()) mThread.join(); }
    bool done() const { return mDone.load(std::memory_order_acquire); }

private:
    std::atomic<bool> mDone{false};
    std::thread mThread;
};

/**
 * CONTROL POSITIVO — **este test TIENE que fallar** con `WMA_TEST_WAIT_SCALE=0`.
 *
 * Sincroniza con una espera ciega y afirma despues, que es exactamente la clase
 * que REQ-002 persigue. Si el instrumento lo deja pasar, el instrumento no esta
 * colapsando nada y todo lo que reporte sobre la suite no vale.
 */
TEST(TimeDependenceProbe, ABlindWaitIsDetectedByTheInstrument) {
    LateWorker worker;
    worker.start(std::chrono::milliseconds(20));

    sleepFixed(std::chrono::milliseconds(400));

    EXPECT_TRUE(worker.done())
        << "CONTROL POSITIVO: con la escala normal esto pasa, y bajo\n"
           "WMA_TEST_WAIT_SCALE=0 tiene que FALLAR. Si estas leyendo esto con la\n"
           "escala en 0, el instrumento funciona. Si NO falla con la escala en 0,\n"
           "el roto es el instrumento — no le creas nada de lo que diga del resto\n"
           "de la suite.";
}

/**
 * CONTROL NEGATIVO — este test **no puede** fallar con `WMA_TEST_WAIT_SCALE=0`.
 *
 * Espera exactamente lo mismo, pero por la CONDICION. Un instrumento que lo
 * voltea no discrimina la clase: ensucia todo, y algo que tiñe todo de rojo no
 * senala nada.
 */
TEST(TimeDependenceProbe, AConditionWaitSurvivesTheInstrument) {
    LateWorker worker;
    worker.start(std::chrono::milliseconds(20));

    EXPECT_TRUE(waitUntil([&] { return worker.done(); }))
        << "CONTROL NEGATIVO: una espera por condicion no depende del reloj, asi\n"
           "que el instrumento no puede voltearla. Si fallo, el instrumento esta\n"
           "achicando tambien los TECHOS y dejo de discriminar.";
}

}  // namespace
}  // namespace wma_test
