/**
 * test_capture_quiesce.cpp — la compuerta del thread de CAPTURA (REQ-012 S1).
 *
 * QUE FALTABA, Y COMO SE VERIFICO QUE FALTABA
 * -------------------------------------------
 * El motor ya sabe drenar UN thread RT: `AudioEngine::spinForCallbackDrain()`
 * espera a `mActiveCallbacks`, y sobre eso se apoyan el `ReconfigureQuiesce` de
 * REQ-006.1 y el retiro de nodo de WD-1.3. Pero `mActiveCallbacks` lo mueven
 * `AudioEngine::onAudioReady` y los backends — o sea el camino de SALIDA — y
 * `InputNode::processInputBlock` NO lo toca. La captura de Android es un
 * SEGUNDO thread RT, con su propio stream de Oboe y su propio DSP, y ningun
 * drenaje del motor lo cubre.
 *
 * Eso importa porque `InputNode::prepare()` hace `resize()` de los dos rings y
 * de los dos buffers de trabajo. Llamarlo con el thread de captura adentro de
 * `processCapturedBlock()` es un use-after-free — es exactamente la razon por la
 * que `setCaptureSampleRate()` hoy es "un `store` atomico y NADA MAS", con la
 * consecuencia (el DSP de entrada corriendo con coeficientes de otro rate)
 * declarada en su KDoc y postergada a "un item propio". Este es ese item.
 *
 * ESTA ETAPA NO RE-PREPARA NADA. Entrega la compuerta sola, sin consumidor: si
 * el mecanismo esta mal, el rojo apunta al mecanismo y no a un cambio de
 * coeficientes corriendo encima.
 *
 * POR QUE HACEN FALTA LOS GANCHOS
 * -------------------------------
 * Dos, y ninguno de los que ya existen sirve:
 *
 *   - `gInputNodeForceStreamRunning` (REQ-009 S3) SI se reusa: sin el,
 *     `processInputBlock` se va por el primer `if` en host, porque
 *     `mInputStreamRunning` solo lo escribe `startInputStream()` y sin Oboe ese
 *     metodo devuelve false antes de tocarlo.
 *   - `gInputNodeHoldInCallback` (WD-1.3) NO sirve: vive en
 *     `isMonitoringEnabled()`, que es el primer metodo del camino de SALIDA.
 *     Retiene al thread de audio, no al de captura. Hace falta una compuerta
 *     DENTRO de `processInputBlock`, y esa la agrega esta etapa.
 *
 * La ventana que hay que volver determinista dura microsegundos: el codigo
 * buggeado de WD-1.3 sobrevivio 40 retiros por corrida en 15 corridas. Por eso
 * aca no hay iteraciones — hay una compuerta.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "../../nodes/InputNode.h"
#include "../../tests/support/TestWait.h"

// Ver la nota de los ganchos en `InputNode.cpp`.
extern std::atomic<bool> gInputNodeForceStreamRunning;
extern std::atomic<bool> gInputNodeHoldInCapture;
extern std::atomic<bool> gInputNodeIsInCapture;

namespace {

constexpr int kRate        = 44100;
constexpr int kBlockFrames = 128;

/// El techo del quiesce en produccion. Se nombra una vez para que un test que
/// mide el veredicto y otro que mide el reporte no puedan discrepar.
constexpr auto kTecho = std::chrono::milliseconds(250);

/// Deja los ganchos globales apagados pase lo que pase: son del binario entero y
/// uno prendido contaminaria a los demas tests.
struct Ganchos {
    Ganchos() { gInputNodeForceStreamRunning.store(true, std::memory_order_release); }
    ~Ganchos() {
        gInputNodeHoldInCapture.store(false, std::memory_order_release);
        gInputNodeForceStreamRunning.store(false, std::memory_order_release);
    }
};

/// Un nodo listo para recibir bloques por el camino de Oboe.
struct Nodo {
    InputNode node;
    std::vector<float> bloque;

    Nodo() : bloque(static_cast<size_t>(kBlockFrames) * 2, 0.25f) {
        node.prepare(kRate, kBlockFrames);
    }

    bool unBloque() {
        return node.processInputBlock(bloque.data(), kBlockFrames, 2, 0);
    }
};

/// Suelta al thread de captura y lo espera. Se llama en todos los caminos de
/// salida, incluido el de un `ASSERT` que aborta el cuerpo del test: un thread
/// retenido para siempre cuelga el binario entero, y colgarse es peor que fallar.
struct Soltar {
    std::thread& t;
    ~Soltar() {
        gInputNodeHoldInCapture.store(false, std::memory_order_release);
        if (t.joinable()) t.join();
    }
};

}  // namespace

/**
 * AC-012.2 — EL TEST PRINCIPAL.
 *
 * Con el thread de captura retenido ADENTRO, el control no puede salir del
 * quiesce. Es la propiedad entera: si sale, `prepare()` correria su `resize()`
 * mientras el otro thread usa esos mismos buffers.
 *
 * El gancho retiene DESPUES de que el bloque ya se conto como en vuelo, y esa
 * ubicacion es parte de lo que se afirma: contar primero y consultar la
 * compuerta despues es lo que hace que un bloque que ya decidio entrar no pueda
 * quedar invisible para el que drena.
 */
TEST(CaptureQuiesce, ElControlNoSaleMientrasLaCapturaEstaAdentro) {
    Ganchos ganchos;
    Nodo n;

    gInputNodeHoldInCapture.store(true, std::memory_order_release);
    std::thread captura([&] { n.unBloque(); });
    Soltar soltar{captura};

    ASSERT_TRUE(wma_test::waitUntil(
        [] { return gInputNodeIsInCapture.load(std::memory_order_acquire); }))
        << "el thread de captura nunca llego a la compuerta";

    std::atomic<bool> salio{false};
    std::atomic<bool> drenoIndebidamente{false};
    std::thread control([&] {
        InputNode::CaptureQuiesce quiesce(n.node, kTecho);
        drenoIndebidamente.store(quiesce.drained(), std::memory_order_release);
        salio.store(true, std::memory_order_release);
    });

    // Espera de AUSENCIA: se afirma que algo NO pasa, y eso no se puede esperar
    // por condicion. `sleepFixed` y no un `sleep_for` crudo — ver TestWait.h.
    wma_test::sleepFixed(std::chrono::milliseconds(50));
    EXPECT_FALSE(salio.load(std::memory_order_acquire))
        << "el quiesce dio por drenado un thread de captura que sigue adentro";

    gInputNodeHoldInCapture.store(false, std::memory_order_release);
    control.join();

    EXPECT_TRUE(salio.load(std::memory_order_acquire));
    EXPECT_TRUE(drenoIndebidamente.load(std::memory_order_acquire))
        << "una vez que la captura salio, el quiesce tenia que confirmar el drenaje";
}

/**
 * EL GEMELO de "no calles de mas" (REQ-001, leccion del apagado).
 *
 * Sin este, una compuerta que NUNCA drena pasaria el test de arriba y seria
 * inutil: no mentir es trivial si no se dice nada. Aca no hay captura en vuelo,
 * asi que el quiesce tiene la obligacion de confirmar.
 */
TEST(CaptureQuiesce, SinCapturaEnVueloElQuiesceConfirmaElDrenaje) {
    Ganchos ganchos;
    Nodo n;

    InputNode::CaptureQuiesce quiesce(n.node, kTecho);
    EXPECT_TRUE(quiesce.drained())
        << "sin nadie adentro, el drenaje es inmediato y hay que afirmarlo";
}

/**
 * AC-012.5 — el quiesce que no puede confirmar lo DICE.
 *
 * El modo de falla inseguro seria el opuesto: devolver `drained()` en true por
 * timeout y dejar que el llamador re-prepare igual. `setInputNode()` ya resuelve
 * su caso analogo del lado seguro —"se filtra el nodo en vez de arriesgar un
 * UAF"— y esta es la misma eleccion.
 */
TEST(CaptureQuiesce, SiLaCapturaNoSaleATiempoElQuiesceLoDice) {
    Ganchos ganchos;
    Nodo n;

    gInputNodeHoldInCapture.store(true, std::memory_order_release);
    std::thread captura([&] { n.unBloque(); });
    Soltar soltar{captura};

    ASSERT_TRUE(wma_test::waitUntil(
        [] { return gInputNodeIsInCapture.load(std::memory_order_acquire); }));

    // Techo corto A PROPOSITO: lo que se prueba es el veredicto cuando el techo
    // se agota, no cuanto tarda. Que sea corto no lo vuelve dependiente del
    // reloj — el thread de captura esta retenido por una condicion, no por
    // tiempo, asi que NO puede salir dentro de ningun techo.
    InputNode::CaptureQuiesce quiesce(n.node, std::chrono::milliseconds(20));
    EXPECT_FALSE(quiesce.drained())
        << "no se pudo confirmar el drenaje y el quiesce dijo que si";
}

/**
 * Un bloque que llega con la compuerta cerrada no se procesa Y SE CUENTA.
 *
 * Las dos mitades importan. Que no se procese es la seguridad; que se cuente es
 * lo que impide que el audio descartado desaparezca en silencio — S3 convierte
 * esa cuenta en la costura que el afinador necesita para no integrar a traves
 * del hueco.
 *
 * Devuelve `true` a proposito: el bloque se consumio sin incidente. `false` en
 * este camino significa "no habia stream", que es otra cosa.
 */
TEST(CaptureQuiesce, UnBloqueConLaCompuertaCerradaNoSeProcesaYSeCuenta) {
    Ganchos ganchos;
    Nodo n;

    ASSERT_EQ(0u, n.node.capturedBlocksGated());

    InputNode::CaptureQuiesce quiesce(n.node, kTecho);
    ASSERT_TRUE(quiesce.drained());

    EXPECT_TRUE(n.unBloque());
    EXPECT_EQ(1u, n.node.capturedBlocksGated())
        << "el bloque descartado tiene que quedar contabilizado";
    EXPECT_EQ(0.0f, n.node.getInputLevelLinear(0))
        << "la compuerta estaba cerrada: el DSP de entrada no tenia que correr";
}

/**
 * Y con la compuerta ABIERTA de nuevo, el bloque vuelve a procesarse.
 *
 * El gemelo del de arriba: una compuerta que se cierra y no se abre pasaria
 * aquel test y romperia la captura para siempre. `CaptureQuiesce` es RAII, asi
 * que lo que se afirma es que su destructor de verdad reabre.
 */
TEST(CaptureQuiesce, AlSalirDelQuiesceLaCapturaVuelveAProcesar) {
    Ganchos ganchos;
    Nodo n;

    {
        InputNode::CaptureQuiesce quiesce(n.node, kTecho);
        ASSERT_TRUE(quiesce.drained());
        ASSERT_TRUE(n.unBloque());
    }

    ASSERT_EQ(1u, n.node.capturedBlocksGated());
    EXPECT_TRUE(n.unBloque());
    EXPECT_EQ(1u, n.node.capturedBlocksGated())
        << "la compuerta quedo cerrada despues de salir del quiesce";
    EXPECT_GT(n.node.getInputLevelLinear(0), 0.0f)
        << "el bloque tenia que haber pasado por el DSP de entrada";
}
