/**
 * WD-1.2 — el flush-to-zero tiene que ejecutarse EN EL THREAD DE AUDIO.
 *
 * QUE SE ESTA PROBANDO, Y POR QUE ASI
 * -----------------------------------
 * FPCR (ARM) y MXCSR (x86) son registros de control POR THREAD. Antes de WD-1.2
 * los dos unicos call sites de `flushDenormals()` eran `AudioEngine::start()` y
 * `OboeBackend::start()`, que corren en el thread del llamador, y
 * `CoreAudioBackend` no llamaba a ninguno. O sea: el thread RT corria con
 * denormales habilitados en las tres plataformas, y nadie lo notaba porque el
 * sintoma —10-100x de costo en una cola de reverb decayendo— es de performance,
 * no de correctitud.
 *
 * Un test que solo llamara a `flushDenormalsRtSafe()` y midiera el efecto no
 * probaria nada de eso: probaria que la funcion funciona, que nunca estuvo en
 * duda. Lo que hay que probar es que **el callback la llama**, y eso se observa
 * desde afuera porque el registro es del thread: se verifica el estado del
 * thread ANTES y DESPUES de un bloque, sobre un thread que arranca sin FTZ.
 *
 * SOBRE `volatile`
 * ----------------
 * Todas las operaciones con denormales pasan por variables `volatile`. Sin eso
 * el compilador constant-foldea la multiplicacion en tiempo de compilacion —
 * donde no hay FPCR que valga— y el test pasa en verde sin haber medido nada.
 * Es el modo de falla clasico de los tests de denormales.
 */

#include "../AudioEngine.h"
#include "../../platform/Platform.h"
#include "support/FakeAudioBackend.h"

#include <gtest/gtest.h>

#include <cmath>
#include <thread>
#include <vector>

namespace {

constexpr int kBlockFrames = 128;

/**
 * @return true si ESTE thread esta aplastando denormales a cero.
 *
 * El denormal mas chico de float32 es ~1.4e-45. Multiplicar 1e-38 (normal, muy
 * cerca del limite) por 1e-7 da ~1e-45: subnormal. Con FTZ activo el resultado
 * es exactamente 0.0f; sin FTZ es un subnormal distinto de cero.
 */
bool denormalsAreFlushedOnThisThread() {
    volatile float tiny = 1.0e-38f;
    volatile float scale = 1.0e-7f;
    volatile float result = tiny * scale;
    return result == 0.0f;
}

/// Precondicion del test: sin FTZ, la cuenta de arriba NO da cero.
/// Si esto no se cumple la plataforma ya viene con FTZ o el compilador plego la
/// cuenta, y el resto del test no mediria nada — por eso se afirma explicito.
bool platformCanObserveDenormals() {
    volatile float tiny = 1.0e-38f;
    volatile float scale = 1.0e-7f;
    volatile float result = tiny * scale;
    return result != 0.0f && std::isfinite(result);
}

}  // namespace

// ---------------------------------------------------------------------------
// El nucleo: un thread limpio, un bloque de audio, y el estado del thread
// cambiado por haber pasado por el callback.
// ---------------------------------------------------------------------------
TEST(DenormalFlush, TheAudioCallbackEnablesFlushToZeroOnItsOwnThread) {
    AudioEngine engine;
    std::vector<float> buffer(kBlockFrames * 2, 0.0f);

    bool observableBefore = false;
    bool flushedBefore = true;
    bool flushedAfter = false;

    // Un thread nuevo, que nunca paso por start() ni por ningun otro call site
    // del flush. Es el analogo del thread que crea AVFoundation o AAudio: el
    // motor no lo creo y no tuvo oportunidad de configurarlo.
    std::thread audioLikeThread([&] {
        observableBefore = platformCanObserveDenormals();
        flushedBefore = denormalsAreFlushedOnThisThread();

        engine.onAudioReady(buffer.data(), nullptr, kBlockFrames);

        flushedAfter = denormalsAreFlushedOnThisThread();
    });
    audioLikeThread.join();

    if (!observableBefore) {
        GTEST_SKIP() << "esta plataforma no deja observar denormales "
                        "(ya vienen aplastados, o el compilador plego la cuenta); "
                        "el test no puede medir nada aca";
    }

    EXPECT_FALSE(flushedBefore)
        << "el thread ya tenia FTZ antes del callback — la precondicion del "
           "test no se cumple y el resultado no probaria nada";

    EXPECT_TRUE(flushedAfter)
        << "onAudioReady() no dejo FTZ activo en SU PROPIO thread. Esto es "
           "exactamente el bug de WD-1.2: flushDenormals() se llamaba desde "
           "start(), o sea en el thread del llamador, y FPCR/MXCSR son estado "
           "por thread.";
}

// ---------------------------------------------------------------------------
// El segundo thread RT: la captura de Android corre en su propio stream, con su
// propio thread y su propio DSP. Arreglar solo el de salida lo dejaba afuera.
// ---------------------------------------------------------------------------
TEST(DenormalFlush, TheCaptureCallbackAlsoEnablesItOnItsOwnThread) {
    if (!platformCanObserveDenormals()) {
        GTEST_SKIP() << "esta plataforma no deja observar denormales";
    }

    bool flushedBefore = true;
    bool flushedAfter = false;

    std::thread captureLikeThread([&] {
        flushedBefore = denormalsAreFlushedOnThisThread();

        // Se llama a la funcion de plataforma directamente, y no a
        // InputNode::processInputBlock(), por una razon que conviene dejar
        // escrita: **el build de tests de host no compila InputNode.cpp**. Lo
        // reemplaza por support/test_input_node_stub.cpp (ver el comentario del
        // CMakeLists de core/tests), asi que un test que llamara al metodo real
        // estaria ejercitando el stub y no probaria nada del path de captura.
        //
        // Eso tiene una consecuencia que va mas alla de este test y conviene
        // tener presente: el call site de WD-1.2 en InputNode.cpp NO lo cubren
        // los 795 tests de host. Lo cubren el build de Android y el de iOS —
        // y de hecho el de iOS fue el que agarro que faltaba el include de
        // Platform.h, despues de que la suite entera pasara en verde.
        wma::platform::flushDenormalsRtSafe();

        flushedAfter = denormalsAreFlushedOnThisThread();
    });
    captureLikeThread.join();

    EXPECT_FALSE(flushedBefore);
    EXPECT_TRUE(flushedAfter);
}

// ---------------------------------------------------------------------------
// Idempotencia: se llama en CADA bloque, sin guarda `thread_local` (ver la nota
// de Platform.h sobre por que no la lleva). Llamarla mil veces tiene que dejar
// el mismo estado que llamarla una.
// ---------------------------------------------------------------------------
TEST(DenormalFlush, CallingItEveryBlockIsIdempotent) {
    if (!platformCanObserveDenormals()) {
        GTEST_SKIP() << "esta plataforma no deja observar denormales";
    }

    bool stayedFlushed = false;

    std::thread t([&] {
        for (int i = 0; i < 1000; ++i) {
            wma::platform::flushDenormalsRtSafe();
        }
        stayedFlushed = denormalsAreFlushedOnThisThread();
    });
    t.join();

    EXPECT_TRUE(stayedFlushed);
}

// ---------------------------------------------------------------------------
// La variante que loguea sigue existiendo para el diagnostico de arranque, y
// tiene que hacer el mismo trabajo sobre el registro. Si alguien la vacia
// pensando que quedo obsoleta, esto lo agarra.
// ---------------------------------------------------------------------------
TEST(DenormalFlush, TheDiagnosticVariantAlsoWritesTheRegister) {
    if (!platformCanObserveDenormals()) {
        GTEST_SKIP() << "esta plataforma no deja observar denormales";
    }

    bool flushedAfter = false;

    std::thread t([&] {
        wma::platform::flushDenormals();
        flushedAfter = denormalsAreFlushedOnThisThread();
    });
    t.join();

    EXPECT_TRUE(flushedAfter);
}
