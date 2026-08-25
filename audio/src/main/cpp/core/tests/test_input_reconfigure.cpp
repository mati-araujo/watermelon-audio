/**
 * test_input_reconfigure.cpp — el DSP de entrada sigue al rate (REQ-012 S2).
 *
 * QUE SE ARREGLA
 * --------------
 * `InputNode::prepare()` es lo UNICO que configura el DSP de entrada y dimensiona
 * los rings, y hasta esta etapa corria una sola vez, con el `(48000, 4096)` literal
 * de `wmaEnsureInputNode`. El rate real se PUBLICABA —`setCaptureSampleRate()`, que
 * es "un `store` atomico y NADA MAS"— pero nadie re-preparaba nada. A 96 kHz eso
 * deja el noise gate y el medidor con constantes de tiempo 2x corridas, y rings que
 * guardan medio segundo donde su contrato dice uno.
 *
 * COMO SE AFIRMA, Y POR QUE ASI
 * -----------------------------
 * Por EQUIVALENCIA, no por umbral: un nodo preparado directo a R tiene que
 * comportarse igual que uno preparado a otro rate y RE-preparado a R. Eso no
 * necesita defender ningun numero magico — la referencia es el propio motor
 * haciendo lo correcto — y falla ruidosamente si el re-preparado toca de menos.
 *
 * Es la misma forma fuerte que el repo ya usa cuando exige bit-exactitud entre
 * tamaños de bloque en vez de "parecido dentro de una tolerancia".
 *
 * 🔴 LOS RATES NO SON 48000 NI 4096. 48000 es la constante DEL BUG: usarla como
 * fixture dejaria pasar un re-preparado que no hace nada.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <chrono>
#include <thread>
#include <vector>

#include "../../nodes/InputNode.h"
#include "../AudioEngine.h"

// Ver la nota de los ganchos en `InputNode.cpp`.
extern std::atomic<bool> gInputNodeForceStreamRunning;
extern std::atomic<bool> gInputNodeHoldInCapture;
extern std::atomic<bool> gInputNodeIsInCapture;

namespace {

constexpr int kRateViejo  = 44100;   ///< con el que nace el nodo
constexpr int kRateNuevo  = 88200;   ///< al que pasa a correr la captura
constexpr int kBlockFrames = 256;
constexpr auto kTecho = std::chrono::milliseconds(250);

/// Alimenta bloques por el camino de USB y devuelve el nivel que marca el medidor.
float nivelTrasBloques(InputNode& node, int bloques) {
    std::vector<float> bloque(static_cast<size_t>(kBlockFrames) * 2, 0.5f);
    for (int i = 0; i < bloques; ++i) {
        node.feedExternalInput(bloque.data(), kBlockFrames);
    }
    return node.getInputLevelLinear(0);
}

/// Cuantos bloques entran en el ring antes de que se pise uno. Es la capacidad
/// observada desde afuera, sin agregarle un getter al nodo.
///
/// 🔴 El contador es `getUsbFeedDrops()` y NO `getMonitorOverflowBlocks()`: la vía de
/// `feedExternalInput` le pasa `mUsbFeedDrops` a `processCapturedBlock`. Con el otro
/// contador este helper devolvia SIEMPRE el tope del guard —el ring nunca "desbordaba"
/// segun un contador que esa vía no toca— y el test pasaba con el arreglo apagado.
/// Lo destapo un mutante de apagado que no mato a este test; medido, no razonado.
int bloquesHastaDesbordar(InputNode& node) {
    node.setMonitoringEnabled(true);
    std::vector<float> bloque(static_cast<size_t>(kBlockFrames) * 2, 0.25f);
    const uint64_t desbordesAlEmpezar = node.getUsbFeedDrops();
    int n = 0;
    while (node.getUsbFeedDrops() == desbordesAlEmpezar && n < 100000) {
        node.feedExternalInput(bloque.data(), kBlockFrames);
        ++n;
    }
    return n;
}

}  // namespace

/**
 * AC-012.1 — el DSP de entrada queda configurado para el rate NUEVO.
 *
 * Se mide el COMPORTAMIENTO del medidor (cuanto sube tras N bloques), no el valor
 * de un getter: una config sin lector pasa cuatro tests, que es como WA-1.2 dejo
 * pasar un recorte de `maxEffects` que nadie leia.
 */
TEST(InputReconfigure, ElMedidorQuedaConLasConstantesDelRateNuevo) {
    InputNode reconfigurado;
    reconfigurado.prepare(kRateViejo, kBlockFrames);
    ASSERT_TRUE(reconfigurado.reconfigureForRate(kRateNuevo, kTecho));

    InputNode referencia;
    referencia.prepare(kRateNuevo, kBlockFrames);

    const float nReconf = nivelTrasBloques(reconfigurado, 20);
    const float nRef    = nivelTrasBloques(referencia, 20);

    EXPECT_FLOAT_EQ(nRef, nReconf)
        << "el nodo re-preparado a " << kRateNuevo << " no se comporta como uno preparado "
           "directo a ese rate: sus constantes de tiempo siguen siendo las del rate viejo.";
}

/**
 * AC-012.3 — los rings se dimensionan al rate NUEVO.
 *
 * El contrato de `RING_BUFFER_SECONDS` esta en SEGUNDOS. Un ring dimensionado para
 * 44,1 kHz y corriendo a 88,2 guarda la MITAD del tiempo que promete, y eso es menos
 * tolerancia a un consumidor atrasado — o sea mas drops.
 */
TEST(InputReconfigure, ElRingSeRedimensionaAlRateNuevo) {
    InputNode reconfigurado;
    reconfigurado.prepare(kRateViejo, kBlockFrames);
    ASSERT_TRUE(reconfigurado.reconfigureForRate(kRateNuevo, kTecho));

    InputNode referencia;
    referencia.prepare(kRateNuevo, kBlockFrames);

    EXPECT_EQ(bloquesHastaDesbordar(referencia), bloquesHastaDesbordar(reconfigurado))
        << "el ring del nodo re-preparado no mide lo mismo que el de uno preparado directo: "
           "quedo dimensionado para el rate viejo.";
}

/**
 * Re-preparar al MISMO rate no falla ni se queja.
 *
 * 🔴 NO SE AFIRMA QUE "HAGA ALGO", Y NO ES PEREZA — ESTA MEDIDO. Se escribio primero
 * el test contrario (que re-preparar al mismo rate vaciara el ring) y el mutante que
 * agrega la guarda `if (sampleRate == mSampleRate) return true;` **sobrevivio**:
 * `prepare()` con el mismo tamaño hace un `resize()` que no mueve nada, asi que con
 * guarda y sin guarda el nodo queda observablemente igual. Es un mutante EQUIVALENTE
 * en todo lo que se puede alcanzar desde host, y un test que pretenda matarlo seria
 * teatro.
 *
 * Igual la guarda NO se escribe, y la razon esta un piso mas abajo: `createInputStream()`
 * hace `mSampleRate = actualSampleRate` **sin re-preparar** (`InputNode.cpp`), asi que
 * `mSampleRate` puede estar diciendo un rate que el DSP no tiene. Una guarda que
 * comparara contra ese campo se creeria al dia justo cuando no lo esta. Por eso
 * `reconfigureForRate()` no consulta `mSampleRate` para NADA.
 */
TEST(InputReconfigure, ReconfigurarAlMismoRateEsLegal) {
    InputNode node;
    node.prepare(kRateNuevo, kBlockFrames);
    EXPECT_TRUE(node.reconfigureForRate(kRateNuevo, kTecho));
}

/**
 * AC-012.5 heredado de S1 — sin drenaje confirmado NO se re-prepara.
 *
 * Es la mitad que hace segura a la otra: `prepare()` hace `resize()` de los rings y
 * de los buffers de trabajo. Correrlo sin haber confirmado que la captura salio es
 * el use-after-free que este REQ existe para no cometer.
 */
TEST(InputReconfigure, SinDrenajeConfirmadoNoSeRePrepara) {
    InputNode node;
    node.prepare(kRateViejo, kBlockFrames);

    gInputNodeForceStreamRunning.store(true, std::memory_order_release);
    gInputNodeHoldInCapture.store(true, std::memory_order_release);
    std::vector<float> bloque(static_cast<size_t>(kBlockFrames) * 2, 0.25f);
    std::thread captura([&] { node.processInputBlock(bloque.data(), kBlockFrames, 2, 0); });

    while (!gInputNodeIsInCapture.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const bool hizo = node.reconfigureForRate(kRateNuevo, std::chrono::milliseconds(20));

    gInputNodeHoldInCapture.store(false, std::memory_order_release);
    captura.join();
    gInputNodeForceStreamRunning.store(false, std::memory_order_release);

    EXPECT_FALSE(hizo)
        << "se re-preparo el nodo con el thread de captura adentro: eso es un resize() "
           "de los rings bajo los pies del que los esta usando.";
}

/**
 * TAREA 2.8 — la deuda de S1, y COMO TERMINO.
 *
 * S1 dejo vivo un mutante: invertir el orden de la compuerta —consultarla ANTES de
 * contarse en vuelo—. Alla no habia consumidor que hiciera `resize()`, asi que la
 * carrera no tenia bajo que manifestarse. Aca si lo hay, y se lo persiguio en serio.
 *
 * 🔴 **NO MURIO, Y ESO QUEDA DICHO ACA EN VEZ DE DISIMULADO.** Lo que se probo, con
 * el mutante aplicado y bajo ASan:
 *
 *   - 200 reconfiguraciones contra 1 thread de captura ......... sobrevive
 *   - 4000 reconfiguraciones contra 3 threads .................. sobrevive
 *   - con pausas para que la captura entre de verdad, x3 ....... sobrevive
 *
 * Y se midio POR QUE, que es lo que vuelve util al resultado: en el bucle apretado
 * **6.132.786 de 6.135.301 bloques encontraban la compuerta cerrada**. Con pausas
 * entran ~2500 de verdad, y aun asi nada. La ventana del mutante son unas pocas
 * instrucciones, y para que haya use-after-free el thread de control tiene que
 * completar el quiesce ENTERO y el `resize()` adentro de ellas.
 *
 * **El orden del protocolo es una propiedad ARGUMENTADA, no probada por un test**, y
 * asi esta declarado tambien en el KDoc de `CaptureQuiesce`. Un test que pretendiera
 * matarlo con mas iteraciones seria teatro: ya se midio que no escala.
 *
 * LO QUE ESTE TEST SI AFIRMA, y por eso se queda: el camino concurrente completo
 * —captura entrando mientras el control reconfigura y redimensiona— no rompe bajo
 * ASan ni TSan. Eso agarra un UAF grosero, que es mas de lo que habia antes.
 */
TEST(InputReconfigure, LaCapturaNuncaCorreContraElResize) {
    InputNode node;
    node.prepare(kRateViejo, kBlockFrames);

    gInputNodeForceStreamRunning.store(true, std::memory_order_release);
    std::atomic<bool> seguir{true};

    std::thread captura([&] {
        std::vector<float> bloque(static_cast<size_t>(kBlockFrames) * 2, 0.25f);
        while (seguir.load(std::memory_order_acquire)) {
            node.processInputBlock(bloque.data(), kBlockFrames, 2, 0);
        }
    });

    // Alternar el rate obliga a que cada vuelta redimensione de verdad: `resize()` al
    // mismo tamaño no mueve memoria, y entonces no habria nada contra que correr.
    for (int i = 0; i < 200; ++i) {
        node.reconfigureForRate(i % 2 == 0 ? kRateNuevo : kRateViejo, kTecho);
        // WAIT-OK: estimulo. La duracion ES el experimento — sin esta pausa el bucle
        // deja la compuerta cerrada casi todo el tiempo y la captura no llega a
        // entrar: medido, 6.132.786 bloques gateados sobre 6.135.301. Un test que no
        // deja entrar a la captura no esta probando concurrencia con la captura.
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    seguir.store(false, std::memory_order_release);
    captura.join();
    gInputNodeForceStreamRunning.store(false, std::memory_order_release);

    EXPECT_GT(node.capturedBlocksGated(), 0u)
        << "premisa: la compuerta tiene que haber rechazado bloques, o este test no "
           "ejercito ninguna reconfiguracion concurrente";
}

/**
 * EL SEGUNDO ESCRITOR — el que se olvida (REQ-012 S2, tarea 2.6).
 *
 * `InputNode::reconfigureForRate()` drena el thread de CAPTURA. Pero el nodo tiene
 * OTRO escritor RT: `feedExternalInput()` —USB, vocoder y MIX— llega al mismo
 * `processCapturedBlock()` desde `AudioEngine::onAudioReady`, o sea desde el thread
 * de SALIDA, y a ese solo lo cubre `mActiveCallbacks`. Drenar uno solo deja el
 * `resize()` corriendo contra el camino de USB: el mismo UAF, por la puerta menos
 * transitada. Salio de la auto-revision del diff de S1, no del plan.
 *
 * 🔴 SE ENTRA POR `onAudioReady`, Y NO ES CEREMONIA — la primera version de este
 * test llamaba a `node->feedExternalInput()` directo desde un thread propio, y
 * **TSan la volteo con una carrera legitima**: ese thread no incrementa
 * `mActiveCallbacks`, asi que `waitForCallbackDrain()` no tenia a quien esperar y el
 * `resize()` corria contra el `DCBlocker`. La carrera era del ATAJO DEL TEST: en
 * produccion `feedExternalInput` corre adentro del `callbackGuard` de `onAudioReady`
 * (`AudioEngine.cpp`), o sea contado.
 *
 * La leccion, que vale mas que el test: **un test que se saltea el registro del
 * llamador no puede verificar un drenaje que se apoya justo en ese registro.**
 * Habria "probado" el drenaje simulando el unico caso que el drenaje no cubre.
 */
TEST(InputReconfigure, ElCaminoDeUsbTampocoCorreContraElResize) {
    AudioEngine engine;
    engine.setOscillatorEnabled(true);   // sin esto onAudioReady no alimenta el nodo

    auto node = std::make_shared<InputNode>();
    node->prepare(kRateViejo, kBlockFrames);
    engine.setInputNode(node);

    std::atomic<bool> seguir{true};
    std::atomic<long> vueltas{0};

    std::thread salida([&] {
        std::vector<float> in(static_cast<size_t>(kBlockFrames) * 2, 0.25f);
        std::vector<float> out(static_cast<size_t>(kBlockFrames) * 2, 0.0f);
        while (seguir.load(std::memory_order_acquire)) {
            // El camino REAL: por aca pasa el callbackGuard que cuenta este thread
            // en `mActiveCallbacks`, que es lo que el drenaje espera.
            engine.onAudioReady(out.data(), in.data(), kBlockFrames);
            vueltas.fetch_add(1, std::memory_order_relaxed);
        }
    });

    int hechas = 0;
    for (int i = 0; i < 100; ++i) {
        if (engine.reconfigureInputNodeForRate(i % 2 == 0 ? kRateNuevo : kRateViejo)) ++hechas;
    }

    seguir.store(false, std::memory_order_release);
    salida.join();
    engine.setInputNode(nullptr);

    EXPECT_GT(hechas, 0) << "no se completo ninguna reconfiguracion: el test no ejercito nada";
    EXPECT_GT(vueltas.load(), 0L) << "el callback de salida no llego a correr";
}
