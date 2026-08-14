/**
 * WD-1.3 — el InputNode se retira SIN que el thread de audio toque un refcount,
 * y sobre todo sin que su destructor corra ahi.
 *
 * QUE ESTABA MAL
 * --------------
 * El callback copiaba `std::shared_ptr<InputNode>` bajo `try_lock`. El try_lock
 * era defendible; la copia no —`IAudioBackend.h:186` la prohibe— y el costo del
 * refcount era lo de menos. Si el thread de UI llamaba `setInputNode(nullptr)`
 * y soltaba su referencia mientras el thread de audio tenia la ultima, **el
 * destructor corria en el thread de audio**: dos `LockFreeRingBuffer` de 96.000
 * floats, dos `std::vector` y el cierre del stream de captura, adentro de un
 * deadline de 2,7 ms.
 *
 * QUE SE PRUEBA, Y POR QUE HACE FALTA EL DOBLE
 * --------------------------------------------
 * "No corre en el thread de audio" no se observa desde afuera del destructor,
 * asi que `test_input_node_stub.cpp` registra en que thread corrio. Es la
 * segunda extension deliberada de ese doble y no cambia ningun comportamiento.
 *
 * El primer test es el contrato. El segundo es el que habria fallado antes del
 * arreglo de forma no-deterministica: mete el retiro EN CARRERA con un thread
 * que bombea callbacks, que es exactamente la ventana donde el shared_ptr del
 * callback podia quedarse con la ultima referencia.
 */

#include "../AudioEngine.h"
#include "../../nodes/InputNode.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

// Definidos en support/test_input_node_stub.cpp.
extern std::atomic<int> gInputNodeDtorCount;
extern std::atomic<std::thread::id> gInputNodeDtorThread;
extern std::atomic<bool> gInputNodeHoldInCallback;
extern std::atomic<bool> gInputNodeIsInCallback;

namespace {

constexpr int kBlockFrames = 128;

/// Bombea callbacks hasta que se le pide parar. Deja huecos minimos entre
/// bloques para que el contador de callbacks en vuelo llegue a cero — igual que
/// un stream real, que no vuelve a entrar hasta el proximo periodo.
class CallbackPump {
public:
    explicit CallbackPump(AudioEngine& engine, bool withUsbInput = false)
        : mEngine(engine)
        , mOutput(kBlockFrames * 2, 0.0f)
        , mInput(kBlockFrames * 2, 0.1f)
        , mWithInput(withUsbInput) {}

    void start() {
        mRunning.store(true);
        mThread = std::thread([this] {
            mThreadId = std::this_thread::get_id();
            while (mRunning.load(std::memory_order_acquire)) {
                mEngine.onAudioReady(mOutput.data(),
                                     mWithInput ? mInput.data() : nullptr,
                                     kBlockFrames);
                mBlocks.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        // No seguir hasta que el bombeo este realmente en marcha: si el retiro
        // corriera antes del primer callback, el test no probaria la carrera.
        while (mBlocks.load(std::memory_order_relaxed) < 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void stop() {
        mRunning.store(false, std::memory_order_release);
        if (mThread.joinable()) mThread.join();
    }

    ~CallbackPump() { stop(); }

    std::thread::id threadId() const { return mThreadId; }
    uint64_t blocks() const { return mBlocks.load(std::memory_order_relaxed); }

private:
    AudioEngine& mEngine;
    std::vector<float> mOutput;
    std::vector<float> mInput;
    bool mWithInput;
    std::atomic<bool> mRunning{false};
    std::atomic<uint64_t> mBlocks{0};
    std::thread mThread;
    std::thread::id mThreadId{};
};

}  // namespace

// ---------------------------------------------------------------------------
// El contrato.
// ---------------------------------------------------------------------------
TEST(InputNodeRetire, TheNodeIsDestroyedOnTheControlThreadNotTheAudioThread) {
    AudioEngine engine;
    CallbackPump pump(engine);

    const int dtorsBefore = gInputNodeDtorCount.load(std::memory_order_acquire);

    {
        auto node = std::make_shared<InputNode>();
        engine.setInputNode(node);
        // Soltar NUESTRA referencia: a partir de aca el motor es el unico dueno,
        // que es la condicion en la que el bug se manifestaba.
    }

    pump.start();

    // El retiro, con callbacks en vuelo.
    engine.setInputNode(nullptr);

    ASSERT_EQ(gInputNodeDtorCount.load(std::memory_order_acquire), dtorsBefore + 1)
        << "el nodo no se destruyo al retirarlo — el motor se quedo con una "
           "referencia, o el retiro no solto la vieja";

    EXPECT_EQ(gInputNodeDtorThread.load(std::memory_order_acquire),
              std::this_thread::get_id())
        << "el destructor del InputNode corrio en otro thread que el de control. "
           "Si ese otro es el del pump, es exactamente el bug de WD-1.3: dos "
           "ring buffers de 96.000 floats liberados adentro del callback.";

    EXPECT_NE(gInputNodeDtorThread.load(std::memory_order_acquire), pump.threadId());

    EXPECT_EQ(engine.getUndrainedInputNodeCount(), 0u)
        << "el drenaje vencio su timeout con el pump corriendo normalmente";

    pump.stop();
}

// ---------------------------------------------------------------------------
// La carrera, repetida. Es el test que ASan y TSan tienen que ver.
// ---------------------------------------------------------------------------
TEST(InputNodeRetire, SwappingTheNodeUnderALiveCallbackIsNotAUseAfterFree) {
    AudioEngine engine;
    CallbackPump pump(engine);
    pump.start();

    const int dtorsBefore = gInputNodeDtorCount.load(std::memory_order_acquire);
    constexpr int kSwaps = 40;

    for (int i = 0; i < kSwaps; ++i) {
        engine.setInputNode(std::make_shared<InputNode>());
    }
    engine.setInputNode(nullptr);

    pump.stop();

    // kSwaps nodos publicados y retirados, mas el ultimo que retira el nullptr.
    EXPECT_EQ(gInputNodeDtorCount.load(std::memory_order_acquire),
              dtorsBefore + kSwaps);
    EXPECT_EQ(engine.getUndrainedInputNodeCount(), 0u);
    EXPECT_GT(pump.blocks(), 0u) << "el pump no llego a correr; el test no probo la carrera";
}

// ---------------------------------------------------------------------------
// El fast-path de USB tambien cuenta como callback en vuelo.
//
// La barrera vivia adentro de processAudioBlock(), y el fast-path de USB
// retorna ANTES de llegar ahi: esos bloques quedaban invisibles para el
// drenaje. Un retiro durante ese path podia liberar el nodo con un callback
// adentro. Este test bombea CON inputData y el oscilador apagado, que es la
// condicion que toma esa rama.
// ---------------------------------------------------------------------------
TEST(InputNodeRetire, TheUsbFastPathCountsAsAnInFlightCallback) {
    AudioEngine engine;
    engine.setOscillatorEnabled(false);  // condicion del fast-path INPUT_FX

    CallbackPump pump(engine, /*withUsbInput=*/true);
    pump.start();

    const int dtorsBefore = gInputNodeDtorCount.load(std::memory_order_acquire);

    engine.setInputNode(std::make_shared<InputNode>());
    engine.setInputNode(nullptr);

    pump.stop();

    EXPECT_EQ(gInputNodeDtorCount.load(std::memory_order_acquire), dtorsBefore + 1);
    EXPECT_EQ(gInputNodeDtorThread.load(std::memory_order_acquire),
              std::this_thread::get_id());
    EXPECT_EQ(engine.getUndrainedInputNodeCount(), 0u);
}

// ---------------------------------------------------------------------------
// EL TEST QUE DISTINGUE.
//
// Los tres de arriba verifican el contrato nuevo, pero NO detectan el bug
// viejo: se probo restaurando el shared_ptr en el callback y quitando el
// drenaje, y pasaron las quince corridas. La ventana de la carrera dura
// microsegundos y bombear a ciegas no la pega.
//
// Este la vuelve determinista con la compuerta del doble: deja al callback
// atrapado ADENTRO, con el nodo en uso, y recien ahi retira desde otro thread.
//
//   con el codigo VIEJO  el callback tiene una copia del shared_ptr; el retiro
//                        suelta la del motor sin esperar nada y se va. Al
//                        liberar la compuerta, el callback suelta la ULTIMA
//                        referencia -> el destructor corre en el thread de
//                        audio. FALLA.
//   con el codigo NUEVO  el callback tiene un puntero crudo; el retiro se
//                        BLOQUEA en el drenaje hasta que soltemos la compuerta,
//                        y recien entonces destruye — en su propio thread. PASA.
// ---------------------------------------------------------------------------
TEST(InputNodeRetire, TheCallbackNeverEndsUpHoldingTheLastReference) {
    AudioEngine engine;
    engine.setInputNode(std::make_shared<InputNode>());

    CallbackPump pump(engine);
    pump.start();

    const int dtorsBefore = gInputNodeDtorCount.load(std::memory_order_acquire);

    // 1. Atrapar al callback adentro, con el nodo en uso.
    gInputNodeHoldInCallback.store(true, std::memory_order_release);
    while (!gInputNodeIsInCallback.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // 2. Retirar desde otro thread de control. Con el arreglo esto se queda
    //    esperando el drenaje; sin el, vuelve enseguida.
    std::atomic<bool> retireReturned{false};
    std::thread retirer([&] {
        engine.setInputNode(nullptr);
        retireReturned.store(true, std::memory_order_release);
    });

    // 3. Dejar que el retiro llegue a su punto de espera (o termine, si no espera).
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const bool returnedWhileCallbackWasInside =
        retireReturned.load(std::memory_order_acquire);

    // 4. Soltar la compuerta.
    gInputNodeHoldInCallback.store(false, std::memory_order_release);
    retirer.join();
    pump.stop();

    EXPECT_FALSE(returnedWhileCallbackWasInside)
        << "setInputNode() volvio con un callback todavia adentro usando el "
           "nodo. Sin esa espera, quien suelte la ultima referencia es el "
           "thread de audio.";

    ASSERT_EQ(gInputNodeDtorCount.load(std::memory_order_acquire), dtorsBefore + 1);

    EXPECT_NE(gInputNodeDtorThread.load(std::memory_order_acquire), pump.threadId())
        << "el destructor del InputNode corrio EN EL THREAD DE AUDIO: dos ring "
           "buffers de 96.000 floats y el cierre del stream de captura, adentro "
           "del deadline. Es el bug de WD-1.3, exactamente.";
}

// ---------------------------------------------------------------------------
// Sin callbacks corriendo el retiro tiene que ser inmediato, no esperar el
// timeout de 250 ms. Si esto tarda, la barrera esta esperando algo que nunca
// va a llegar — y ese costo lo paga el thread de UI en cada cambio de modo.
// ---------------------------------------------------------------------------
TEST(InputNodeRetire, RetiringWithNoCallbacksRunningDoesNotWaitForTheTimeout) {
    AudioEngine engine;
    engine.setInputNode(std::make_shared<InputNode>());

    const auto start = std::chrono::steady_clock::now();
    engine.setInputNode(nullptr);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 50)
        << "el retiro tardo mas de 50 ms sin ningun callback en vuelo: la "
           "barrera no esta viendo el contador en cero";
}
