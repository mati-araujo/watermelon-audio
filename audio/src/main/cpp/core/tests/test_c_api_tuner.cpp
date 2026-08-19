/**
 * test_c_api_tuner.cpp — la frontera del afinador (REQ-001 S1, tareas 1.6/1.13).
 *
 * Lo que se prueba aca no es el analisis: es el CRUCE. Del otro lado de esta
 * frontera hay un consumidor —iOS por la C API, Android por el JNI— que no puede
 * ver el ring, ni el thread, ni el snapshot; lo unico que ve es un `bool` y ocho
 * floats. Asi que lo que tiene que ser cierto es sobre eso.
 *
 * DOS COSAS QUE ESTE ARCHIVO EXISTE PARA IMPEDIR
 * -----------------------------------------------
 * 1. **Que un fallo se lea como una medicion.** Este repo ya shippeo dos stubs
 *    que devolvian un array de ceros: los ceros derrotaron los fallbacks elvis
 *    de sus propios callers, porque un array de ceros NO es dato ausente, es
 *    dato plausible. Por eso el contrato es "devuelve false y NO TOCA el
 *    buffer", y por eso los tests de abajo llenan el buffer con centinelas y
 *    exigen que sigan ahi.
 * 2. **Que el rate publicado sea uno asumido.** El motor tuvo `48000` cableado
 *    en el prepare del `InputNode`, y las tareas 1.16-1.18 lo sacaron del
 *    camino. Un snapshot que publique el rate que se sabia al ARRANCAR
 *    reintroduce el mismo defecto un piso mas arriba, porque el rate de captura
 *    cambia en caliente. Los tests de abajo lo cambian a mitad de sesion.
 *
 * NINGUN RATE DE PRUEBA ES 48000, y no es estilo: es la constante que estaba
 * cableada, asi que usarla como valor de prueba haria que un defecto de
 * propagacion pase por coincidencia.
 */

#include "support/CApiFixture.h"

#include "api/watermelon_audio.h"
#include "api/watermelon_audio_internal.h"
#include "analysis/AnalysisSnapshot.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <thread>

namespace wma_test {
namespace {

using wma::analysis::kSnapCaptureSampleRate;
using wma::analysis::kSnapDroppedFrames;
using wma::analysis::kSnapFramesAnalyzed;
using wma::analysis::kSnapLevelRms;
using wma::analysis::kSnapState;
using wma::analysis::kSnapCents;
using wma::analysis::kSnapPhaseAngle;
using wma::analysis::kSnapUncertainty;

/// Centinela para detectar escritura donde el contrato dice que no la hay.
///
/// NO es 0 ni 0,5: el cero es justo el valor que un stub escribiria, y 0,5 es
/// potencia de dos —representable exacto— asi que esconde defectos de float. Se
/// elige uno que no sea ninguna de las dos cosas y que ademas seria absurdo como
/// medicion.
constexpr float kSentinel = -37.317f;

constexpr int kBlockFrames = 256;

/// El rate "raro" con el que se arranca, y el que llega despues en caliente.
/// Ninguno es 48000; ver la nota de arriba.
constexpr int kFirstRate  = 44100;
constexpr int kSecondRate = 32000;

using TunerApiTest = CApiFixture;

std::array<float, WMA_TUNER_SNAPSHOT_VALUES> sentinelBuffer() {
    std::array<float, WMA_TUNER_SNAPSHOT_VALUES> buf{};
    buf.fill(kSentinel);
    return buf;
}

bool allSentinels(const std::array<float, WMA_TUNER_SNAPSHOT_VALUES>& buf) {
    for (float v : buf) {
        if (v != kSentinel) return false;
    }
    return true;
}

/**
 * Espera hasta que haya un snapshot publicado, o se rinde.
 *
 * Con techo y no con un sleep fijo: el thread de analisis duerme 5 ms cuando el
 * ring esta vacio, asi que un sleep "generoso" seria a la vez lento y frágil.
 * Devuelve false si nunca llego, para que el test falle con su propio mensaje.
 */
bool waitForSnapshot(WmaEngine* e, std::array<float, WMA_TUNER_SNAPSHOT_VALUES>& out,
                     int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (wma_tuner_get_snapshot(e, out.data())) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// Igual, pero espera a que un valor concreto llegue al numero pedido. Para el
/// rate en caliente: el snapshot ya existe, lo que cambia es su contenido.
bool waitForValue(WmaEngine* e, int index, float expected,
                  std::array<float, WMA_TUNER_SNAPSHOT_VALUES>& out,
                  int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (wma_tuner_get_snapshot(e, out.data()) && out[static_cast<size_t>(index)] == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// Empuja el rate negociado hasta el `InputNode`, por el mismo camino que un
/// device: el callback de cambio de configuracion de stream.
void negotiateCaptureRate(WmaEngine* e, int hz) {
    watermelon_audio::StreamInfo info{};
    info.sampleRate = hz;
    info.channelCount = 2;
    e->engine->onStreamConfigChanged(info);
}

// ---------------------------------------------------------------------------
// 1.6 — el motor sin seam de analisis
// ---------------------------------------------------------------------------

/**
 * TAREA 1.6. Un motor al que nadie le pidio afinar no tiene snapshot que dar, y
 * lo que hace con el buffer del llamador importa tanto como el `false`.
 */
TEST_F(TunerApiTest, ASnapshotWithoutATunerReturnsFalseAndLeavesTheBufferAlone) {
    auto buf = sentinelBuffer();

    EXPECT_FALSE(wma_tuner_get_snapshot(mWma, buf.data()))
        << "no hay seam de analisis: decir que si es prometer una medicion";
    EXPECT_TRUE(allSentinels(buf))
        << "escribio en el buffer del llamador. Ceros ahi no son 'dato ausente': "
           "son una medicion plausible que derrota el fallback del consumidor";
}

/**
 * El mismo contrato en el otro estado en que no hay dato: el afinador ARRANCO
 * pero todavia no publico nada. Es una ventana real —el thread duerme 5 ms
 * cuando el ring esta vacio— y sin este test un `read()` que devolviera ceros
 * antes del primer publish pasaria inadvertido.
 */
TEST_F(TunerApiTest, ASnapshotBeforeTheFirstPublishAlsoLeavesTheBufferAlone) {
    startAt(kFirstRate, 0);
    ASSERT_TRUE(wma_tuner_start(mWma));

    auto buf = sentinelBuffer();
    // Sin haber renderizado un solo bloque, no hubo captura y no hay publish.
    EXPECT_FALSE(wma_tuner_get_snapshot(mWma, buf.data()));
    EXPECT_TRUE(allSentinels(buf));

    wma_tuner_stop(mWma);
}

/// Un puntero nulo no puede tumbar el proceso del consumidor.
TEST_F(TunerApiTest, ANullBufferIsRejectedAndNotDereferenced) {
    startAt(kFirstRate, 0);
    ASSERT_TRUE(wma_tuner_start(mWma));
    EXPECT_FALSE(wma_tuner_get_snapshot(mWma, nullptr));
    EXPECT_FALSE(wma_tuner_get_snapshot(nullptr, nullptr));
    wma_tuner_stop(mWma);
}

// ---------------------------------------------------------------------------
// 1.13 — el cruce entrega lo que se midio
// ---------------------------------------------------------------------------

/**
 * El camino entero, por donde pasa de verdad: bloques con entrada por el
 * backend -> `InputNode::processCapturedBlock` -> ring -> thread -> snapshot ->
 * C API. Es el unico test que falla si cualquiera de esos seis eslabones se
 * desconecta.
 */
TEST_F(TunerApiTest, TheSnapshotCarriesWhatTheCapturePathActuallyMeasured) {
    startAt(kFirstRate, 0);
    negotiateCaptureRate(mWma, kFirstRate);
    ASSERT_TRUE(wma_tuner_start(mWma));

    // Un tono audible: 20 bloques son ~107 ms a 48 kHz, de sobra para varios
    // ticks del drenador.
    for (int i = 0; i < 20; ++i) renderWithInput(1, kBlockFrames, 0.2f);

    auto buf = sentinelBuffer();
    ASSERT_TRUE(waitForSnapshot(mWma, buf))
        << "el afinador corrio y entro audio, y no publico nada: el seam esta cortado";

    EXPECT_EQ(buf[kSnapCaptureSampleRate], static_cast<float>(kFirstRate))
        << "publico un rate que no es el que se negocio";
    EXPECT_GT(buf[kSnapLevelRms], 0.0f) << "entro un tono a 0,2 y midio silencio";
    EXPECT_GT(buf[kSnapFramesAnalyzed], 0.0f);
    EXPECT_EQ(buf[kSnapDroppedFrames], 0.0f)
        << "con el drenador al dia no deberia haber perdido frames";

    wma_tuner_stop(mWma);
}

/**
 * Los tres campos que llena S2 valen NaN, no 0.
 *
 * Se verifica DESDE LA C API y no solo en el thread porque es aca donde el valor
 * cruza a un consumidor: `0.0` cents es "afinado exacto" y se dibujaria como una
 * medicion. Este test es lo que impide que S2 arranque con un placeholder que
 * miente.
 */
TEST_F(TunerApiTest, TheFieldsStageTwoWillFillCrossTheBoundaryAsNaN) {
    startAt(kFirstRate, 0);
    negotiateCaptureRate(mWma, kFirstRate);
    ASSERT_TRUE(wma_tuner_start(mWma));
    for (int i = 0; i < 20; ++i) renderWithInput(1, kBlockFrames, 0.2f);

    auto buf = sentinelBuffer();
    ASSERT_TRUE(waitForSnapshot(mWma, buf));

    EXPECT_TRUE(std::isnan(buf[kSnapCents]));
    EXPECT_TRUE(std::isnan(buf[kSnapPhaseAngle]));
    EXPECT_TRUE(std::isnan(buf[kSnapUncertainty]));

    wma_tuner_stop(mWma);
}

/**
 * EL RATE SIGUE AL STREAM EN CALIENTE.
 *
 * Este es el test que separa "el rate llega" de "el rate llego una vez". El
 * afinador arranca con la sesion a 44 100, y a mitad de sesion entra una
 * configuracion a 32 000 —un auricular BT, una interfaz que se enchufa— SIN que
 * nadie reinicie el afinador. Un motor que publique el rate que capturo al
 * arrancar escala todo lo que mida por 44100/32000: **+702 cents**, medio tono
 * largo. El afinador diria que una cuerda afinada esta a mas de un semitono.
 */
TEST_F(TunerApiTest, ARateChangeMidSessionReachesTheSnapshotWithoutARestart) {
    startAt(kFirstRate, 0);
    negotiateCaptureRate(mWma, kFirstRate);
    ASSERT_TRUE(wma_tuner_start(mWma));
    for (int i = 0; i < 20; ++i) renderWithInput(1, kBlockFrames, 0.2f);

    auto buf = sentinelBuffer();
    ASSERT_TRUE(waitForValue(mWma, kSnapCaptureSampleRate,
                             static_cast<float>(kFirstRate), buf))
        << "no llego a publicar el rate inicial";

    // El device cambia de configuracion. El afinador NO se reinicia.
    negotiateCaptureRate(mWma, kSecondRate);
    for (int i = 0; i < 20; ++i) renderWithInput(1, kBlockFrames, 0.2f);

    ASSERT_TRUE(waitForValue(mWma, kSnapCaptureSampleRate,
                             static_cast<float>(kSecondRate), buf))
        << "el snapshot se quedo en " << buf[kSnapCaptureSampleRate]
        << " Hz con la captura a " << kSecondRate
        << ": todo lo que mida queda escalado por " << kFirstRate << "/" << kSecondRate;

    wma_tuner_stop(mWma);
}

// ---------------------------------------------------------------------------
// Ciclo de vida
// ---------------------------------------------------------------------------

/// Arrancar dos veces no arranca dos veces. Sin esto, un consumidor que llame
/// `start()` en cada `onResume` acumularia threads.
TEST_F(TunerApiTest, StartingTwiceIsIdempotent) {
    startAt(kFirstRate, 0);
    ASSERT_TRUE(wma_tuner_start(mWma));
    EXPECT_TRUE(wma_tuner_is_running(mWma));
    EXPECT_TRUE(wma_tuner_start(mWma)) << "el segundo start tiene que ser inofensivo";
    EXPECT_TRUE(wma_tuner_is_running(mWma));
    wma_tuner_stop(mWma);
    EXPECT_FALSE(wma_tuner_is_running(mWma));
}

/**
 * Parar deja de medir, pero NO borra lo ultimo medido.
 *
 * Es la diferencia entre "el afinador esta apagado" y "el afinador no existe", y
 * un consumidor la necesita: al soltar la cuerda, la UI sigue mostrando la
 * ultima lectura en vez de parpadear a vacio.
 */
TEST_F(TunerApiTest, StoppingKeepsTheLastSnapshotReadable) {
    startAt(kFirstRate, 0);
    negotiateCaptureRate(mWma, kFirstRate);
    ASSERT_TRUE(wma_tuner_start(mWma));
    for (int i = 0; i < 20; ++i) renderWithInput(1, kBlockFrames, 0.2f);

    auto before = sentinelBuffer();
    ASSERT_TRUE(waitForSnapshot(mWma, before));

    wma_tuner_stop(mWma);
    ASSERT_FALSE(wma_tuner_is_running(mWma));

    auto after = sentinelBuffer();
    ASSERT_TRUE(wma_tuner_get_snapshot(mWma, after.data()))
        << "parar no puede borrar la ultima medicion";
    EXPECT_EQ(after[kSnapCaptureSampleRate], before[kSnapCaptureSampleRate]);
}

/**
 * Y despues de parar, la captura deja de alimentar el ring.
 *
 * Se mide por el efecto observable: con el afinador parado, siguen entrando
 * bloques y `framesAnalyzed` NO se mueve. Sin esta mitad, un `stop()` que solo
 * juntara el thread y se olvidara de desenganchar el ring pasaria el test de
 * arriba igual.
 */
TEST_F(TunerApiTest, StoppingDetachesTheWriterFromTheRing) {
    startAt(kFirstRate, 0);
    negotiateCaptureRate(mWma, kFirstRate);
    ASSERT_TRUE(wma_tuner_start(mWma));
    for (int i = 0; i < 20; ++i) renderWithInput(1, kBlockFrames, 0.2f);

    auto atStop = sentinelBuffer();
    ASSERT_TRUE(waitForSnapshot(mWma, atStop));
    wma_tuner_stop(mWma);

    // El ring queda desenganchado: estos bloques no tienen a donde ir.
    for (int i = 0; i < 40; ++i) renderWithInput(1, kBlockFrames, 0.2f);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto later = sentinelBuffer();
    ASSERT_TRUE(wma_tuner_get_snapshot(mWma, later.data()));
    EXPECT_EQ(later[kSnapFramesAnalyzed], atStop[kSnapFramesAnalyzed])
        << "con el afinador parado el analisis sigue consumiendo captura";
}

}  // namespace
}  // namespace wma_test
