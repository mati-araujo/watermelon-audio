/**
 * test_reconfigure_seam.cpp — reconfigurar es una COSTURA (REQ-012 S3).
 *
 * QUE PASA CUANDO SE RE-PREPARA
 * -----------------------------
 * Dos cosas, y las dos rompen la continuidad de lo que el afinador venia
 * integrando:
 *
 *   1. Mientras dura el quiesce, los bloques de captura se DESCARTAN — es lo que
 *      cuenta `capturedBlocksGated()` desde REQ-012.1. Eso es un hueco.
 *   2. El rate cambia, asi que las muestras de despues tienen otra escala
 *      temporal que las de antes.
 *
 * Integrar a traves de eso y publicar CONVERGIDO es el modo de falla inseguro que
 * REQ-009 persiguio cuatro veces. Por eso la reconfiguracion avisa.
 *
 * 🔴 QUIEN PUEDE ESTAMPAR LA COSTURA, Y POR QUE ACA SI
 * ---------------------------------------------------
 * `AnalysisRing::reportCaptureDiscontinuity()` documenta una invariante fuerte:
 *
 *   > el load-then-store no es atomico y no hace falta que lo sea: en cada
 *   > topologia hay UN SOLO thread que avisa, y es el que escribe el ring.
 *
 * La reconfiguracion la dispara el thread de CONTROL, que NO es el que escribe.
 * La invariante no vale por si sola — **la restituye el drenaje**: adentro del
 * `CaptureQuiesce` el thread de captura esta afuera y confirmado, y el otro
 * escritor (`feedExternalInput`, desde el thread de salida) es precondicion
 * declarada de `reconfigureForRate()`, que `AudioEngine::reconfigureInputNodeForRate()`
 * garantiza retirando el nodo. Con los dos escritores quietos, el control es
 * momentaneamente el unico, que es exactamente lo que la invariante pide.
 *
 * Por eso NO hace falta la bandera pendiente de iOS/USB: alla el problema es que
 * el que detecta no puede parar al que escribe. Aca si lo paro.
 *
 * 🔴 Y POR ESO EL RATE NO SE ESTAMPA DESDE ACA. `processCapturedBlock` ya hace
 * `ring->setCaptureRate(...)` en CADA bloque, o sea que el rate nuevo lo publica el
 * escritor junto con las muestras que describe. Estamparlo tambien desde el control
 * seria meter un segundo escritor de ese campo — romper la invariante en el mismo
 * gesto de respetarla.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "../../nodes/InputNode.h"
#include "../../analysis/AnalysisRing.h"

#include <atomic>
#include <thread>

// Ver la nota de los ganchos en `InputNode.cpp`.
extern std::atomic<bool> gInputNodeForceStreamRunning;
extern std::atomic<bool> gInputNodeHoldInCapture;
extern std::atomic<bool> gInputNodeIsInCapture;

namespace {

constexpr int kRateViejo   = 44100;
constexpr int kRateNuevo   = 88200;
constexpr int kBlockFrames = 256;
constexpr auto kTecho = std::chrono::milliseconds(250);

using wma::analysis::AnalysisRing;

/// Nodo con el ring del afinador enganchado, alimentable por el camino de USB.
struct Nodo {
    AnalysisRing ring;
    InputNode node;
    std::vector<float> bloque;

    Nodo() : bloque(static_cast<size_t>(kBlockFrames) * 2, 0.25f) {
        node.prepare(kRateViejo, kBlockFrames);
        node.setCaptureSampleRate(kRateViejo);
        node.setAnalysisRing(&ring);
    }
    ~Nodo() { node.setAnalysisRing(nullptr); }

    void alimentar(int bloques) {
        for (int i = 0; i < bloques; ++i) node.feedExternalInput(bloque.data(), kBlockFrames);
    }
};

}  // namespace

/**
 * AC-012.4 — reconfigurar deja una costura, y en la frontera correcta.
 *
 * La posicion tiene que ser la de escritura DEL MOMENTO: ahi esta el hueco, entre
 * lo que ya entro con el rate viejo y lo que va a entrar con el nuevo. Se afirma
 * contra `readPosition()` avanzado a mano, que es el mismo sistema de coordenadas
 * en el que el lector compara su guarda.
 */
TEST(ReconfigureSeam, ReconfigurarEstampaUnaCosturaEnLaFrontera) {
    Nodo n;
    n.alimentar(4);
    const uint64_t escritosAntes = static_cast<uint64_t>(4) * kBlockFrames;

    ASSERT_EQ(0u, n.ring.captureSeamPosition()) << "premisa: sin daño no hay costura";

    ASSERT_TRUE(n.node.reconfigureForRate(kRateNuevo, kTecho));

    EXPECT_EQ(escritosAntes, n.ring.captureSeamPosition())
        << "la costura no quedo en la frontera entre lo viejo y lo nuevo: el lector "
           "descartaria audio sano, o peor, integraria a traves del hueco.";
}

/**
 * EL GEMELO: sin reconfigurar, no hay costura.
 *
 * Sin esto, avisar SIEMPRE pasaria el test de arriba y dejaria al afinador
 * descartando audio sano para siempre — la aguja nunca convergeria. Es la misma
 * forma que REQ-009 exige para toda guarda: el que no calla de mas.
 */
TEST(ReconfigureSeam, SinReconfigurarNoSeInventaUnaCostura) {
    Nodo n;
    n.alimentar(8);
    EXPECT_EQ(0u, n.ring.captureSeamPosition())
        << "aparecio una costura sin que nadie reconfigurara: una marca siempre "
           "prendida no distingue nada.";
}

/**
 * Un drenaje que NO se confirma no estampa costura.
 *
 * Es coherente con que tampoco re-prepara: si no se toco nada, no hubo hueco, y
 * avisar uno seria hacer descartar audio bueno por una reconfiguracion que no pasó.
 */
TEST(ReconfigureSeam, SiNoSeRePreparoTampocoHayCostura) {
    Nodo n;
    n.alimentar(4);
    ASSERT_FALSE(n.node.reconfigureForRate(-1, kTecho)) << "premisa: un rate invalido no re-prepara";
    EXPECT_EQ(0u, n.ring.captureSeamPosition());
}

/**
 * Y TAMPOCO SI EL DRENAJE NO SE PUDO CONFIRMAR — el caso que importa.
 *
 * 🔴 ESTE TEST LO PIDIO UN MUTANTE. El de arriba usa un rate invalido, que sale
 * ANTES de abrir el quiesce, asi que dejaba sin cubrir el otro camino de "no se
 * re-preparo": el drenaje que se agota. Un mutante que avisaba la costura tambien
 * en ese return SOBREVIVIA los cinco tests.
 *
 * Y el caso no es cosmetico: si el drenaje fallo, el nodo quedo intacto —mismo rate,
 * mismos rings, ni un bloque descartado—, o sea que NO hubo hueco. Avisar uno haria
 * que el afinador descarte audio sano y se quede sin converger, por una
 * reconfiguracion que nunca ocurrio.
 */
TEST(ReconfigureSeam, UnDrenajeFallidoNoEstampaCostura) {
    Nodo n;
    n.alimentar(4);

    gInputNodeForceStreamRunning.store(true, std::memory_order_release);
    gInputNodeHoldInCapture.store(true, std::memory_order_release);
    std::vector<float> bloque(static_cast<size_t>(kBlockFrames) * 2, 0.25f);
    std::thread captura([&] { n.node.processInputBlock(bloque.data(), kBlockFrames, 2, 0); });

    while (!gInputNodeIsInCapture.load(std::memory_order_acquire)) std::this_thread::yield();

    const bool hizo = n.node.reconfigureForRate(kRateNuevo, std::chrono::milliseconds(20));

    gInputNodeHoldInCapture.store(false, std::memory_order_release);
    captura.join();
    gInputNodeForceStreamRunning.store(false, std::memory_order_release);

    ASSERT_FALSE(hizo) << "premisa: con la captura adentro no se puede re-preparar";
    EXPECT_EQ(0u, n.ring.captureSeamPosition())
        << "se aviso una costura por una reconfiguracion que no ocurrio: el afinador "
           "va a descartar audio sano y no converger.";
}

/**
 * GANA LA MAS LEJANA, no la ultima.
 *
 * Es la regla que `reportCaptureDiscontinuity()` ya implementa con un maximo, y
 * este test la fija DESDE la reconfiguracion: un aviso de reconfiguracion no puede
 * retirar una costura de xrun ya anunciada mas adelante — el salto sigue estando
 * alla, y borrarlo deja al lector convergiendo a traves de el.
 */
TEST(ReconfigureSeam, LaReconfiguracionNoRetiraUnaCosturaMasLejana) {
    Nodo n;
    n.alimentar(2);

    // Un overrun avisado con la cola por delante: la costura queda lejos.
    const uint64_t lejos = static_cast<uint64_t>(2) * kBlockFrames + 100000;
    n.ring.reportCaptureDiscontinuity(100000);
    ASSERT_EQ(lejos, n.ring.captureSeamPosition()) << "premisa: la costura lejana quedo puesta";

    ASSERT_TRUE(n.node.reconfigureForRate(kRateNuevo, kTecho));

    EXPECT_EQ(lejos, n.ring.captureSeamPosition())
        << "la reconfiguracion piso una costura mas lejana. El salto de aquel overrun "
           "sigue estando adelante: borrarlo deja al lector integrando a traves.";
}

/**
 * El rate nuevo lo publica EL ESCRITOR, con las muestras que describe.
 *
 * `processCapturedBlock` hace `ring->setCaptureRate(...)` en cada bloque. Si el
 * control tambien lo estampara al reconfigurar, el ring diria el rate nuevo sobre
 * muestras que todavia son del viejo — un rate describiendo audio que no le
 * corresponde, que es el defecto que las tareas 1.16-1.18 sacaron del motor.
 */
TEST(ReconfigureSeam, ElRateNuevoLlegaConElPrimerBloqueNuevoYNoAntes) {
    Nodo n;
    n.alimentar(2);
    ASSERT_EQ(kRateViejo, n.ring.captureRate()) << "premisa: el escritor estampo el rate viejo";

    ASSERT_TRUE(n.node.reconfigureForRate(kRateNuevo, kTecho));

    EXPECT_EQ(kRateViejo, n.ring.captureRate())
        << "el control estampo el rate nuevo sobre muestras que son del viejo";

    n.alimentar(1);
    EXPECT_EQ(kRateNuevo, n.ring.captureRate())
        << "el primer bloque despues de reconfigurar tenia que traer el rate nuevo";
}
