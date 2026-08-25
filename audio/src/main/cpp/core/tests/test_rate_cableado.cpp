/**
 * test_rate_cableado.cpp — que el rate REAL dispare la reconfiguracion (REQ-012 S4).
 *
 * POR QUE ESTA ETAPA EXISTE ASI
 * -----------------------------
 * S1 entrego la compuerta, S2 el re-preparado y S3 la costura — y al terminar S3 un
 * `grep` sobre el arbol, excluyendo tests, mostro que **nadie llamaba a
 * `reconfigureInputNodeForRate()` en produccion**. O sea que en un telefono el DSP de
 * entrada seguia sin seguir al rate: el mecanismo estaba, pero desenchufado.
 *
 * Cerrar el REQ sin esto habria sido declarar resuelto un sintoma de usuario que nadie
 * toco, con la suite entera en verde.
 *
 * Estos tests son el cable, y se afirman de punta a punta: entra un cambio de config
 * por donde entra de verdad —los hooks de `IAudioBackend`— y se mira el DSP del nodo.
 */

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "../AudioEngine.h"
#include "../../nodes/InputNode.h"
#include "../../backends/IAudioBackend.h"

#include <atomic>
#include <thread>

// El gancho de WD-1.3: retiene al thread de SALIDA adentro del callback.
// `isMonitoringEnabled()` es el primer metodo que `onAudioReady` llama sobre el nodo.
extern std::atomic<bool> gInputNodeHoldInCallback;
extern std::atomic<bool> gInputNodeIsInCallback;

namespace {

constexpr int kProvisional = 48000;   // con el que nace el nodo
constexpr int kRateEntrada = 44100;
constexpr int kRateSalida  = 96000;
constexpr int kBlockFrames = 256;

watermelon_audio::StreamInfo info(int rate) {
    watermelon_audio::StreamInfo i{};
    i.sampleRate = rate;
    i.channelCount = 2;
    return i;
}

/// Cuantos bloques entran en el ring antes de pisarse: la capacidad observada, que
/// es proporcional al rate con el que el nodo fue PREPARADO. Es el mismo helper que
/// S2 valido — y con el contador correcto, `getUsbFeedDrops()`.
int capacidadObservada(InputNode& node) {
    node.setMonitoringEnabled(true);
    std::vector<float> bloque(static_cast<size_t>(kBlockFrames) * 2, 0.25f);
    const uint64_t ini = node.getUsbFeedDrops();
    int n = 0;
    while (node.getUsbFeedDrops() == ini && n < 100000) {
        node.feedExternalInput(bloque.data(), kBlockFrames);
        ++n;
    }
    return n;
}

std::shared_ptr<InputNode> nodoProvisional() {
    auto n = std::make_shared<InputNode>();
    n->prepare(kProvisional, kBlockFrames);   // como wmaEnsureInputNode
    return n;
}

}  // namespace

/**
 * AC-012.1 de punta a punta — un cambio de config de ENTRADA re-prepara el DSP.
 *
 * Se compara contra un nodo preparado directo a ese rate: equivalencia, no umbral.
 */
TEST(RateCableado, UnCambioDeConfigDeEntradaRePreparaElNodo) {
    AudioEngine engine;
    auto node = nodoProvisional();
    engine.setInputNode(node);

    engine.onInputStreamConfigChanged(info(kRateEntrada));

    InputNode referencia;
    referencia.prepare(kRateEntrada, kBlockFrames);

    EXPECT_EQ(capacidadObservada(referencia), capacidadObservada(*node))
        << "el aviso de config de entrada no re-preparo el nodo: el DSP sigue con los "
           "coeficientes del rate provisional y los rings dimensionados para el.";
    engine.setInputNode(nullptr);
}

/**
 * Y el de SALIDA tambien, cuando nadie informo el de entrada.
 *
 * Es el caso del backend NO partido, que es el normal: el unico rate que hay es el de
 * salida y describe a los dos lados.
 */
TEST(RateCableado, SinConfigDeEntradaElRateDeSalidaRePreparaIgual) {
    AudioEngine engine;
    auto node = nodoProvisional();
    engine.setInputNode(node);

    engine.onStreamConfigChanged(info(kRateSalida));

    InputNode referencia;
    referencia.prepare(kRateSalida, kBlockFrames);

    EXPECT_EQ(capacidadObservada(referencia), capacidadObservada(*node))
        << "sin config de entrada, el rate de salida es el unico que hay y tenia que "
           "re-preparar la captura.";
    engine.setInputNode(nullptr);
}

/**
 * EN BACKEND PARTIDO GANA LA ENTRADA, y no solo para publicar el rate.
 *
 * `mHasInputStreamConfig` ya protegia esa asimetria para el rate PUBLICADO. El
 * cableado tenia que respetarla igual: si despues de saber el rate de entrada llega
 * uno de salida distinto, re-preparar con el de salida dejaria el DSP describiendo un
 * stream que no es el que captura.
 */
TEST(RateCableado, EnBackendPartidoElRateDeEntradaGanaTambienParaRePreparar) {
    AudioEngine engine;
    auto node = nodoProvisional();
    engine.setInputNode(node);

    engine.onInputStreamConfigChanged(info(kRateEntrada));   // la entrada habla primero
    engine.onStreamConfigChanged(info(kRateSalida));         // y la salida dice otra cosa

    InputNode referencia;
    referencia.prepare(kRateEntrada, kBlockFrames);

    EXPECT_EQ(capacidadObservada(referencia), capacidadObservada(*node))
        << "el rate de SALIDA piso al de entrada: el DSP quedo preparado para un stream "
           "que no es el que captura.";
    engine.setInputNode(nullptr);
}

/**
 * Un nodo que se engancha DESPUES queda preparado, no solo avisado.
 *
 * Es el caso normal del afinador: el usuario lo abre cuando el motor ya lleva rato
 * corriendo, asi que ese nodo se perdio el aviso de config. `setInputNode` ya le
 * decia el rate; ahora ademas lo prepara — sin eso entraria al grafo con el DSP
 * configurado para el provisional.
 */
TEST(RateCableado, UnNodoEnganchadoDespuesQuedaPreparadoConElRateConocido) {
    AudioEngine engine;
    engine.onInputStreamConfigChanged(info(kRateEntrada));   // el motor ya sabe

    auto node = nodoProvisional();
    engine.setInputNode(node);                               // y recien ahora se engancha

    InputNode referencia;
    referencia.prepare(kRateEntrada, kBlockFrames);

    EXPECT_EQ(capacidadObservada(referencia), capacidadObservada(*node))
        << "el nodo entro al grafo con el DSP del rate provisional: se le aviso el rate "
           "pero no se lo preparo.";
    engine.setInputNode(nullptr);
}

/**
 * AC-012.6 — un nodo recien creado NO afirma un rate que nadie midio.
 *
 * Cero significa desconocido y por eso no es 48000: un default plausible es peor que
 * la ausencia, porque el consumidor no lo puede distinguir de una medicion. El
 * `prepare()` provisional le da tamaño a los buffers, no una respuesta.
 */
TEST(RateCableado, UnNodoReciennCreadoNoAfirmaUnRateMedido) {
    auto node = nodoProvisional();
    EXPECT_EQ(0, node->getCaptureSampleRate())
        << "el nodo dice saber a que rate corre la captura sin que nadie se lo haya dicho.";
}

/**
 * EL CAMINO QUE FALTABA: un nodo que YA SABE su rate igual se re-prepara al entrar.
 *
 * 🔴 ESTE TEST LO PIDIO EL INSTRUMENTO DEL CRITERIO DE MUERTE, no un mutante. Al
 * listar los sitios que publican el rate de captura y mirar cuales re-preparan,
 * quedaba uno sin cablear: `InputNode::startInputStream()`, que abre un stream de
 * Oboe PROPIO del nodo —no pasa por `IAudioBackend`— asi que
 * `onInputStreamConfigChanged` nunca se dispara para el. Es el camino del afinador
 * en Android, o sea el caso que motiva el REQ entero.
 *
 * En host no hay Oboe y ese metodo devuelve false antes de tocar nada, asi que lo
 * que se puede afirmar aca es la mitad que SI es alcanzable: un nodo que llega
 * sabiendo su rate —como queda tras arrancar ese stream— tiene que salir PREPARADO
 * para el, no solo enterado.
 *
 * La guarda vieja de `setInputNode` re-preparaba solo si el rate era DESCONOCIDO, asi
 * que este caso caia justo afuera: saber el rate y estar preparado para el son cosas
 * distintas.
 */
TEST(RateCableado, UnNodoQueYaSabeSuRateSaleDePublicarsePreparadoParaEl) {
    AudioEngine engine;

    auto node = nodoProvisional();
    node->setCaptureSampleRate(kRateEntrada);   // como lo deja `startInputStream()`
    ASSERT_EQ(kProvisional, 48000) << "premisa: el nodo nacio con el rate provisional";

    engine.setInputNode(node);

    InputNode referencia;
    referencia.prepare(kRateEntrada, kBlockFrames);

    EXPECT_EQ(capacidadObservada(referencia), capacidadObservada(*node))
        << "el nodo entro al grafo sabiendo su rate pero preparado para el provisional: "
           "saber y estar preparado no son lo mismo.";
    engine.setInputNode(nullptr);
}

/**
 * MINI-007 — el fallback cuando el nodo NO esta publicado en el motor.
 *
 * 🔴 ESTE TEST LO PIDIO UNA MEDICION EN DEVICE, no un mutante. En el Moto G42, con el
 * harness: arrancar la captura por el camino del AFINADOR dejaba DOS `InputNode
 * prepared` en el log —el provisional y el re-preparado— y por el de `wma_input_start`
 * dejaba UNO SOLO. La diferencia es que el primero publica el nodo con `setInputNode()`
 * antes de arrancar el stream y el segundo no, asi que el motor salia por "no tengo
 * nodo" sin re-preparar nada.
 *
 * Alli no se notaba —Oboe negocia 48000 y coincide con el provisional— pero con una
 * interfaz USB a 44,1 o 96 kHz el DSP se quedaba con los coeficientes viejos.
 *
 * Lo que se afirma aca es el contrato que hace legitimo al fallback: el motor tiene que
 * DISTINGUIR "no hay nodo publicado" de "no se pudo drenar". Sobre un bool indistinto,
 * el fallback habria re-preparado tambien sin drenaje confirmado.
 */
TEST(RateCableado, ElMotorDistingueNoTenerNodoDeNoPoderDrenar) {
    AudioEngine engine;

    EXPECT_EQ(AudioEngine::InputReconfigure::SinNodoPublicado,
              engine.reconfigureInputNodeForRate(kRateEntrada))
        << "sin nodo publicado el motor tiene que decirlo con su propio valor: es el "
           "UNICO caso en que el llamador puede caer al camino del nodo sin arriesgar "
           "un resize sin drenar.";

    auto node = nodoProvisional();
    engine.setInputNode(node);
    EXPECT_EQ(AudioEngine::InputReconfigure::Reconfigurado,
              engine.reconfigureInputNodeForRate(kRateEntrada))
        << "con el nodo publicado tiene que ir por el camino que drena los dos escritores";

    EXPECT_EQ(AudioEngine::InputReconfigure::RateInvalido,
              engine.reconfigureInputNodeForRate(0))
        << "un rate invalido no se puede confundir con no tener nodo";

    engine.setInputNode(nullptr);
}

/**
 * AC-3 — un drenaje fallido NO se puede confundir con "no hay nodo".
 *
 * 🔴 ESTE ES EL TEST QUE PROTEGE EL FALLBACK, y lo pidio un mutante: reportar
 * `SinNodoPublicado` cuando en realidad el drenaje se agoto **sobrevivia** todo lo
 * demas. Y no es un matiz de nomenclatura — el helper del C API cae al
 * `reconfigureForRate()` del nodo justo en `SinNodoPublicado`, asi que ese mutante
 * convierte el fallback en un atajo que re-prepara **sin haber drenado el camino de
 * salida**. Es el use-after-free de REQ-012.2, entrando por la puerta del arreglo.
 *
 * Se fuerza con el gancho de WD-1.3: un thread de salida retenido adentro del
 * callback deja `mActiveCallbacks` en 1, asi que `waitForCallbackDrain()` se agota.
 */
TEST(RateCableado, UnDrenajeAgotadoNoSeDisfrazaDeNodoAusente) {
    AudioEngine engine;
    engine.setOscillatorEnabled(true);
    auto node = nodoProvisional();
    engine.setInputNode(node);

    gInputNodeHoldInCallback.store(true, std::memory_order_release);
    std::atomic<bool> seguir{true};
    std::thread salida([&] {
        std::vector<float> in(static_cast<size_t>(kBlockFrames) * 2, 0.25f);
        std::vector<float> out(static_cast<size_t>(kBlockFrames) * 2, 0.0f);
        while (seguir.load(std::memory_order_acquire)) {
            engine.onAudioReady(out.data(), in.data(), kBlockFrames);
        }
    });

    // Suelta el gancho pase lo que pase: un thread retenido cuelga el binario entero.
    struct Soltar {
        std::atomic<bool>& seguir;
        std::thread& t;
        ~Soltar() {
            gInputNodeHoldInCallback.store(false, std::memory_order_release);
            seguir.store(false, std::memory_order_release);
            if (t.joinable()) t.join();
        }
    } soltar{seguir, salida};

    ASSERT_TRUE([] {
        for (int i = 0; i < 20000; ++i) {
            if (gInputNodeIsInCallback.load(std::memory_order_acquire)) return true;
            std::this_thread::yield();
        }
        return false;
    }()) << "premisa: el thread de salida nunca quedo retenido adentro del callback";

    EXPECT_EQ(AudioEngine::InputReconfigure::SinDrenaje,
              engine.reconfigureInputNodeForRate(kRateEntrada))
        << "con el callback de salida atascado el motor tiene que decir SIN DRENAJE. "
           "Decir 'no hay nodo' habilitaria el fallback del C API, que re-prepararia "
           "sin drenar — el use-after-free que REQ-012.2 existe para no cometer.";
}
