/**
 * test_c_api_start_without_select.cpp
 *
 * Los dos bugs que encontró el harness de WA-5.5 al apretar "capturar" por
 * primera vez, en el simulador de iOS. Los diez comandos del gate estaban en
 * verde cuando aparecieron.
 *
 * ## Por qué se pueden reproducir acá y no se vieron antes
 *
 * `mUseBackendManager` arranca en `false` en Android —camino Oboe directo, el
 * que shippea— y en **`true` fuera de Android**, que incluye iOS **y este target
 * de host**. O sea que el camino roto siempre estuvo al alcance de la suite; lo
 * que lo tapaba es que `CApiFixture::startAt()` llama `wma_select_backend(1)`
 * **a mano** antes de arrancar. El fixture venía compensando, sin querer,
 * exactamente el paso que falta en producción.
 *
 * Por eso estos tests **no usan `startAt()`**: arrancan el motor como lo arranca
 * un consumidor de verdad —`wma_engine_create()` y `wma_engine_start()`, nada
 * más— que es lo único que reproduce el bug.
 *
 * ## Los dos bugs
 *
 *   1. **Nadie selecciona un backend.** `BackendManager` construye el backend de
 *      sistema en su constructor, pero `selectBackend()` sólo lo llaman
 *      `wma_select_backend()` y `AudioEngineImpl.setAudioBackend()`, los dos a
 *      pedido del consumidor. Sin eso `mActiveBackend` es null y `start()`
 *      devuelve `ERROR_NOT_INITIALIZED` para siempre: **el motor no puede abrir
 *      un stream**.
 *
 *   2. **Un start fallido deja el motor diciendo que corre.** `start()`
 *      transiciona a `Running` *antes* de llamar al backend —a propósito: es el
 *      fix PHASE 7.1 contra una carrera con el thread DSP, y no hay que
 *      deshacerlo—. Lo que estaba mal es el rollback: intentaba `Running →
 *      Stopped`, que la tabla de transiciones rechaza porque desde `Running` la
 *      única salida es `Stopping`. La transición se descartaba y el motor
 *      quedaba en `Running` sin stream.
 */

#include "support/CApiFixture.h"

#include <gtest/gtest.h>

namespace wma_test {
namespace {

/// `wma_get_engine_state`: 0=Stopped, 1=Starting, 2=Running, 3=Stopping.
constexpr int kStopped = 0;
constexpr int kRunning = 2;

using CApiStartWithoutSelectTest = CApiFixture;

// ===========================================================================
// Bug 1 — arrancar sin haber seleccionado un backend
// ===========================================================================

TEST_F(CApiStartWithoutSelectTest, TheEngineStartsWithoutAnyoneSelectingABackend) {
    // Cómo arranca un consumidor: crear y arrancar. Nadie llama a
    // wma_select_backend(), porque nada en la API pública obliga a hacerlo —
    // AudioEngine.start() de Kotlin no lo hace, y es la única puerta que tiene.
    ASSERT_EQ(wma_engine_start(mWma, 0), WMA_OK)
        << "el motor no pudo abrir un stream sin que el consumidor eligiera un "
           "backend a mano. Ese era el sintoma en iOS: 'BackendManager: No "
           "backend selected'";

    EXPECT_EQ(wma_get_engine_state(mWma), kRunning);
    EXPECT_EQ(mBackend->startCount(), 1) << "el backend de sistema tiene que haber arrancado";
}

TEST_F(CApiStartWithoutSelectTest, TheDefaultSelectionLandsOnTheSystemBackend) {
    ASSERT_EQ(wma_engine_start(mWma, 0), WMA_OK);

    // 1 == BackendType::OBOE, que fuera de Android es el slot del backend de
    // sistema (CoreAudio en iOS, el fake acá). No es NONE: si quedara en NONE,
    // un caller que pregunte qué backend tiene se llevaría "ninguno" sobre un
    // motor que está sonando.
    EXPECT_EQ(wma_get_backend_type(), 1);
}

TEST_F(CApiStartWithoutSelectTest, SelectingByHandBeforeStartingStillStartsExactlyOnce) {
    // Ojo con lo que este test NO prueba — se llamaba
    // "AnExplicitSelectionIsNotOverriddenByTheDefault" y era mentira. Lo
    // destapó un mutante: cambiar el guard `== NONE` por `true` **no lo hacía
    // fallar**. El motivo es que acá se selecciona el MISMO backend que elegiría
    // el default, y `selectBackend()` sale temprano cuando el tipo no cambia, así
    // que las dos versiones hacen exactamente lo mismo.
    //
    // Lo que sí cubre: que seleccionar a mano y después arrancar no termine
    // arrancando el backend dos veces.
    ASSERT_TRUE(wma_select_backend(1));
    ASSERT_EQ(wma_engine_start(mWma, 0), WMA_OK);

    EXPECT_EQ(wma_get_backend_type(), 1);
    EXPECT_EQ(mBackend->startCount(), 1);
}

/*
 * NO SE PUEDE CUBRIR ACÁ, y el guard igual es load-bearing.
 *
 * "El default no pisa una selección explícita" no es observable en el host: el
 * único backend seleccionable es el de sistema (LIBUSB cae a él por el fallback
 * silencioso, SPLIT falla sin uno construido), así que **el único estado no-NONE
 * alcanzable es el mismo valor que pondría el default**. Ningún test de esta
 * suite puede distinguir `== NONE` de `true`.
 *
 * Donde sí muerde es en Android con USB: ahí el consumidor llama
 * `setAudioBackend(LIBUSB)` —que además prende el BackendManager— y con el guard
 * en `true` el siguiente `start()` volvería a seleccionar el backend de sistema,
 * **degradando USB a Oboe en silencio**. Es exactamente la clase de bug que este
 * repo viene persiguiendo, y la suite de host no lo puede ver porque no tiene
 * backend USB.
 *
 * Se deja dicho en vez de escribir una assertion que pasa por casualidad.
 */

// ===========================================================================
// Bug 2 — el estado tiene que seguir a la realidad
// ===========================================================================

TEST_F(CApiStartWithoutSelectTest, AFailedStartLeavesTheEngineStoppedNotRunning) {
    // El backend falla al arrancar: es lo que pasaba en iOS con el manager sin
    // backend, y lo que pasa en la vida real cuando el device esta ocupado.
    mBackend->setStartResult(watermelon_audio::BackendResult::ERROR_DEVICE_NOT_FOUND);

    EXPECT_NE(wma_engine_start(mWma, 0), WMA_OK) << "el start tiene que fallar";

    EXPECT_EQ(wma_get_engine_state(mWma), kStopped)
        << "el motor reportaba RUNNING sobre un start fallido, sin stream "
           "abierto. Es la misma familia que los hallazgos de WA-2.6: un valor "
           "que se informa bien mientras la realidad es otra";
}

TEST_F(CApiStartWithoutSelectTest, TheEngineCanBeStartedAgainAfterAFailedStart) {
    // La consecuencia práctica de quedar clavado en Running: el siguiente start
    // veía "ya está corriendo" y no hacía nada, así que el motor no se
    // recuperaba nunca de un fallo transitorio.
    mBackend->setStartResult(watermelon_audio::BackendResult::ERROR_DEVICE_NOT_FOUND);
    ASSERT_NE(wma_engine_start(mWma, 0), WMA_OK);
    ASSERT_EQ(wma_get_engine_state(mWma), kStopped);

    mBackend->setStartResult(watermelon_audio::BackendResult::OK);
    EXPECT_EQ(wma_engine_start(mWma, 0), WMA_OK)
        << "un fallo transitorio no puede dejar el motor inservible";
    EXPECT_EQ(wma_get_engine_state(mWma), kRunning);
}

TEST_F(CApiStartWithoutSelectTest, AFailedStartDoesNotLeaveTheFadeRunning) {
    // start() arranca el fade ANTES de tocar el backend, para que los primeros
    // callbacks vean una rampa válida. Si el arranque no prosperó esa rampa no
    // la consume nadie, y dejarla viva hace que un consumidor vea `isFading`
    // sobre un motor detenido: la misma clase de mentira que el estado.
    //
    // Un fade de 500 ms para que la rampa siga viva si nadie la cancela — con
    // fade 0 el test pasaría por el motivo equivocado.
    mBackend->setStartResult(watermelon_audio::BackendResult::ERROR_DEVICE_NOT_FOUND);
    ASSERT_NE(wma_engine_start(mWma, 500), WMA_OK);

    EXPECT_EQ(wma_get_engine_state(mWma), kStopped);
    EXPECT_FALSE(wma_is_fading(mWma))
        << "el fade quedó corriendo sobre un motor que nunca arrancó";
}

}  // namespace
}  // namespace wma_test
