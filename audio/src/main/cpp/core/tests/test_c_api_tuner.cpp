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

#include "tests/support/TestWait.h"
#include "support/CApiFixture.h"

#include "api/watermelon_audio.h"
#include "api/watermelon_audio_internal.h"
#include "analysis/AnalysisSnapshot.h"
#include "analysis/AnalysisThread.h"

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
using wma::analysis::kSnapLockedString;
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

/**
 * El fixture del afinador. Deriva en vez de ser un alias porque necesita UNA cosa
 * que `CApiFixture` no puede dar: esperar un snapshot **alimentando audio**.
 */
class TunerApiTest : public CApiFixture {
protected:
    /**
     * Espera a que haya un snapshot publicado, ALIMENTANDO AUDIO mientras espera.
     *
     * POR QUE `waitForSnapshot` NO ALCANZA, Y NO ES UN MATIZ
     * ------------------------------------------------------
     * `waitForSnapshot` sondea y duerme, pero no empuja nada al ring. Si la
     * integracion se reinicio —por ejemplo porque cambio la fuente de entrada— el
     * thread de analisis se queda **sin nada que analizar**, y entonces no hay
     * ningun snapshot que esperar: la espera se agota POR CONSTRUCCION, no por
     * lentitud. Ninguna cantidad de paciencia arregla eso.
     *
     * Fue el fallo de `4c7fdfb` y `b93dca8` en master, y es de OTRA CLASE que las
     * esperas ciegas de este REQ: comprimir el tiempo no lo reproduce (medido,
     * 0/10 en las dos escalas). Lo que lo reproduce es forzar el ORDEN — que el
     * ultimo cambio de fuente caiga despues del ultimo render — y con eso sale
     * **9/10** contra 0/10 sin la compuerta.
     */
    bool waitForSnapshotWhileFeeding(std::array<float, WMA_TUNER_SNAPSHOT_VALUES>& out,
                                     int maxBlocks = 400) {
        for (int i = 0; i < maxBlocks; ++i) {
            renderWithInput(1, kBlockFrames, 0.2f);
            if (wma_tuner_get_snapshot(mWma, out.data())) return true;
        }
        return false;
    }
};

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

/// Igual que `waitForSnapshot`, pero espera a que haya una MEDICION de altura —o sea, a
/// que `cents` deje de ser NaN. Sin esto un test leeria el primer snapshot publicado, que
/// sale antes de que la integracion tenga de donde sacar una pendiente.
bool waitForMeasurement(WmaEngine* e, std::array<float, WMA_TUNER_SNAPSHOT_VALUES>& out,
                        int timeoutMs = 3000) {
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (wma_tuner_get_snapshot(e, out.data()) && !std::isnan(out[kSnapCents])) return true;
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
 * Los tres campos de afinacion cruzan como NaN cuando NO HAY OBJETIVO, no como 0.
 *
 * ⚠️ Este test se llamaba `TheFieldsStageTwoWillFillCrossTheBoundaryAsNaN` y el nombre quedo
 * mintiendo: S2 ya esta cableada, asi que esos campos SI se llenan — cuando hay contra que
 * medir. Lo que los deja en NaN hoy es la ausencia de objetivo, que es una decision y no una
 * etapa pendiente. Un test cuyo nombre describe un estado que ya no existe manda al proximo
 * lector a buscar trabajo que ya esta hecho.
 *
 * Se verifica DESDE LA C API y no solo en el thread porque es aca donde el valor cruza a un
 * consumidor: `0.0` cents es "afinado exacto" y se dibujaria como una medicion.
 */
TEST_F(TunerApiTest, WithNoTargetTheTuningFieldsCrossTheBoundaryAsNaN) {
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
// 4.0 — EL CABLEADO: el afinador mide de verdad
// ---------------------------------------------------------------------------

/// Un seno de `hz` alimentado por el camino de captura, en bloques.
void feedTone(WmaEngine* engine, double hz, int rate, int blocks, int blockFrames);

/**
 * TAREA 4.0.1. Con objetivo empujado, el snapshot publica **cents reales**.
 *
 * Es el test que separa "las piezas existen" de "el producto mide". Antes de esta tarea,
 * S1 + S2 + S3 estaban cerradas y verdes y el snapshot devolvia NaN: el seam transportaba,
 * el estimador medía, y nadie los conectaba.
 */
TEST_F(TunerApiTest, WithATargetTheSnapshotPublishesRealCents) {
    startAt(kFirstRate, 0);
    negotiateCaptureRate(mWma, kFirstRate);

    // El objetivo ANTES de arrancar: es el orden natural de un consumidor.
    const double target = 110.0;
    ASSERT_TRUE(wma_tuner_set_target(mWma, static_cast<float>(target)));
    ASSERT_TRUE(wma_tuner_start(mWma));

    // Un tono un cent por encima del objetivo. 1 cent es DIEZ VECES el presupuesto,
    // asi que un estimador que devolviera 0 no pasaria.
    const double detuned = target * std::pow(2.0, 1.0 / 1200.0);
    feedTone(mWma, detuned, kFirstRate, 160, kBlockFrames);

    auto buf = sentinelBuffer();
    ASSERT_TRUE(waitForMeasurement(mWma, buf))
        << "con objetivo y señal, el snapshot sigue sin publicar altura";

    EXPECT_EQ(buf[kSnapDroppedFrames], 0.0f)
        << "el ring piso frames: la señal que vio el estimador tiene huecos y la medicion "
           "no significa nada. Es del ritmo de alimentacion del test, no del motor.";
    EXPECT_FALSE(std::isnan(buf[kSnapCents])) << "cents siguio en NaN con el cableado puesto";
    EXPECT_NEAR(buf[kSnapCents], 1.0f, 0.1f)
        << "midio " << buf[kSnapCents] << " cents contra 1,0 real";
    EXPECT_FALSE(std::isnan(buf[kSnapPhaseAngle]));
    EXPECT_FALSE(std::isnan(buf[kSnapUncertainty]));

    wma_tuner_stop(mWma);
}

/**
 * TAREA 4.0.2. **Sin** objetivo no se inventa uno: cents sigue en NaN y el estado dice
 * "sin enganche".
 *
 * Publicar la altura de lo que sea que este sonando seria una medicion que nadie pidio — y
 * peor, una que el usuario leeria como la de su cuerda.
 */
TEST_F(TunerApiTest, WithoutATargetItReportsNoLockInsteadOfGuessing) {
    startAt(kFirstRate, 0);
    negotiateCaptureRate(mWma, kFirstRate);
    ASSERT_TRUE(wma_tuner_start(mWma));            // sin set_target

    feedTone(mWma, 110.0, kFirstRate, 200, kBlockFrames);

    auto buf = sentinelBuffer();
    ASSERT_TRUE(waitForSnapshot(mWma, buf));
    EXPECT_TRUE(std::isnan(buf[kSnapCents]))
        << "sin objetivo publico " << buf[kSnapCents] << " cents: adivino";
    EXPECT_EQ(buf[kSnapState], static_cast<float>(wma::analysis::kStateNoLock));
    EXPECT_EQ(wma_tuner_get_target(mWma), 0.0f);

    wma_tuner_stop(mWma);
}

/**
 * TAREA 4.0.3. Cambiar el objetivo **reinicia la integracion**.
 *
 * Sin esto, la fase acumulada contra la cuerda anterior se mezcla con la nueva y la pendiente
 * resultante no describe a ninguna de las dos. Es el caso normal de un afinador: se pasa de
 * cuerda en cuerda.
 */
TEST_F(TunerApiTest, ChangingTheTargetRestartsTheIntegration) {
    startAt(kFirstRate, 0);
    negotiateCaptureRate(mWma, kFirstRate);
    ASSERT_TRUE(wma_tuner_set_target(mWma, 110.0f));
    ASSERT_TRUE(wma_tuner_start(mWma));

    feedTone(mWma, 110.0 * std::pow(2.0, 1.0 / 1200.0), kFirstRate, 160, kBlockFrames);
    auto first = sentinelBuffer();
    ASSERT_TRUE(waitForMeasurement(mWma, first));
    ASSERT_NEAR(first[kSnapCents], 1.0f, 0.1f);

    // Otra cuerda: A2 -> D3, y el tono nuevo esta 2 cents por encima de ESE objetivo.
    const double second = 146.832;
    ASSERT_TRUE(wma_tuner_set_target(mWma, static_cast<float>(second)));
    feedTone(mWma, second * std::pow(2.0, 2.0 / 1200.0), kFirstRate, 160, kBlockFrames);

    auto buf = sentinelBuffer();
    ASSERT_TRUE(waitForMeasurement(mWma, buf));
    EXPECT_NEAR(buf[kSnapCents], 2.0f, 0.1f)
        << "tras cambiar de objetivo midio " << buf[kSnapCents]
        << ": quedo fase de la nota anterior en la regresion";

    wma_tuner_stop(mWma);
}

/**
 * TAREA 4.0.4. El estimador se prepara con el rate **medido**, no con 48000.
 *
 * Es el ultimo lugar donde el rate de S1 se podia perder: llega vivo hasta el snapshot y se
 * usaria mal justo al medir. Con captura a 44,1 kHz y el estimador preparado a 48 k, la
 * lectura se corre **+146,7 cents** — mas de un semitono.
 */
TEST_F(TunerApiTest, TheEstimatorIsPreparedWithTheMeasuredRateNotWithAConstant) {
    startAt(kSecondRate, 0);                       // 32000: ni 48000 ni 44100
    negotiateCaptureRate(mWma, kSecondRate);

    const double target = 220.0;
    ASSERT_TRUE(wma_tuner_set_target(mWma, static_cast<float>(target)));
    ASSERT_TRUE(wma_tuner_start(mWma));

    feedTone(mWma, target * std::pow(2.0, 1.0 / 1200.0), kSecondRate, 160, kBlockFrames);

    auto buf = sentinelBuffer();
    ASSERT_TRUE(waitForMeasurement(mWma, buf));
    EXPECT_EQ(buf[kSnapCaptureSampleRate], static_cast<float>(kSecondRate));
    EXPECT_NEAR(buf[kSnapCents], 1.0f, 0.2f)
        << "con captura a " << kSecondRate << " midio " << buf[kSnapCents]
        << " cents contra 1,0 real: el estimador se preparo con otro rate";

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

    // 🔴 LA LINEA DE BASE SE TOMA DESPUES DE PARAR, Y ESO NO ES ESTILO.
    //
    // Este test tomaba `atStop` ANTES de `wma_tuner_stop()`, con el thread de
    // analisis todavia vivo — asi que entre esa lectura y el stop el drenador
    // seguia consumiendo y la base quedaba vieja. Medido bajo TSan, que ensancha
    // esa ventana: 2560 frames en la base contra 4608 despues. El test fallaba
    // acusando al motor de "seguir consumiendo con el afinador parado", cuando
    // lo que estaba mal era CUANDO se miraba.
    //
    // Despues de `stop()` la base es estable POR CONSTRUCCION: `AnalysisThread::
    // stop()` junta el thread, asi que cuando vuelve no queda nadie que pueda
    // publicar. La propiedad que se quiere afirmar no cambio; cambio el momento
    // desde el que se la mira.
    auto atStop = sentinelBuffer();
    ASSERT_TRUE(waitForSnapshot(mWma, atStop));   // hubo analisis de verdad
    wma_tuner_stop(mWma);

    // Y RECIEN ACA se toma la base, con el thread ya juntado.
    ASSERT_TRUE(wma_tuner_get_snapshot(mWma, atStop.data()));

    // El ring queda desenganchado: estos bloques no tienen a donde ir.
    for (int i = 0; i < 40; ++i) renderWithInput(1, kBlockFrames, 0.2f);
    // AUSENCIA, igual que en `test_analysis_thread`: con `stop()` joineando esto
    // vale por construccion, y la ventana existe para atrapar un `stop()` que
    // dejara al escritor enganchado al ring.
    wma_test::sleepFixed(std::chrono::milliseconds(20));

    auto later = sentinelBuffer();
    ASSERT_TRUE(wma_tuner_get_snapshot(mWma, later.data()));
    EXPECT_EQ(later[kSnapFramesAnalyzed], atStop[kSnapFramesAnalyzed])
        << "con el afinador parado el analisis sigue consumiendo captura";
}

/**
 * Alimenta un seno por el camino REAL de captura: `onAudioReady` con `inputData`, que es como
 * lo entrega un backend. La fase se acumula entre bloques —un salto de fase en el borde seria
 * un transitorio que el DSP de entrada veria como señal.
 *
 * 🔴 SE AUTORREGULA CONTRA EL DRENADOR, Y ESO NO ES OPCIONAL
 * ----------------------------------------------------------
 * En un device la captura llega EN TIEMPO REAL y el ring —8192 frames, ~170 ms— le sobra al
 * drenador. Un test que empuje todo lo rapido que puede el CPU invierte esa relacion: el ring
 * se llena, pisa lo mas viejo (que es su contrato: no bloquear jamas al escritor) y el
 * estimador recibe una señal CON HUECOS. La fase salta en cada hueco y la pendiente que sale
 * de ahi no mide nada.
 *
 * Medido antes de arreglarlo: 13,94 cents contra 1,0 real. El sintoma parecia del estimador y
 * era del ritmo de alimentacion.
 *
 * Por eso despues de cada tanda se ESPERA a que el analisis la haya consumido, en vez de
 * dormir un rato fijo: una compuerta contra el estado real aguanta una maquina cargada o una
 * corrida bajo sanitizers, y un sleep elegido a ojo no.
 */
void feedTone(WmaEngine* engine, double hz, int rate, int blocks, int blockFrames) {
    std::vector<float> out(static_cast<size_t>(blockFrames) * 2, 0.0f);
    std::vector<float> in(static_cast<size_t>(blockFrames) * 2, 0.0f);
    std::array<float, WMA_TUNER_SNAPSHOT_VALUES> probe{};
    double phase = 0.0;
    const double dp = 2.0 * M_PI * hz / static_cast<double>(rate);

    // Un cuarto del ring por tanda: deja al drenador tres cuartos de margen.
    const int blocksPerChunk = std::max(1, 2048 / blockFrames);
    long long fed = 0;

    // 🔴 LA BASE ES LO YA ANALIZADO, NO CERO. `framesAnalyzed` es ACUMULADO desde que
    // arranco el afinador, asi que comparar contra un contador local que empieza en cero
    // hace que la compuerta se satisfaga sola a partir de la segunda llamada — y entonces
    // esta funcion vuelve a inundar el ring sin que nada lo diga. Medido: el primer test
    // pasaba y el segundo media 20 cents contra 2 reales.
    float baseline = 0.0f;
    if (wma_tuner_get_snapshot(engine, probe.data())) baseline = probe[kSnapFramesAnalyzed];

    for (int b = 0; b < blocks; ++b) {
        for (int f = 0; f < blockFrames; ++f) {
            const float v = static_cast<float>(0.3 * std::sin(phase));
            phase += dp;
            if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
            in[static_cast<size_t>(f) * 2] = v;
            in[static_cast<size_t>(f) * 2 + 1] = v;
        }
        std::fill(out.begin(), out.end(), 0.0f);
        engine->engine->onAudioReady(out.data(), in.data(), blockFrames);
        fed += blockFrames;

        if ((b + 1) % blocksPerChunk == 0) {
            // Espera a que el analisis se ponga al dia, con techo para no colgar el test.
            const auto deadline = std::chrono::steady_clock::now()
                                  + std::chrono::milliseconds(500);
            while (std::chrono::steady_clock::now() < deadline) {
                if (wma_tuner_get_snapshot(engine, probe.data())
                    && probe[kSnapFramesAnalyzed] - baseline >= static_cast<float>(fed - 4096)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// REQ-001 S9 · 9.9 — el modo intonacion, desde la C API
// ---------------------------------------------------------------------------
/**
 * Lo que se verifica ACA y no en los tests del modo es lo que sólo la frontera
 * puede romper: que sin motor no explote, que un slot fuera de rango se rechace,
 * y que la diferencia cruce como NaN —y no como 0— cuando no hay resultado.
 */
TEST_F(TunerApiTest, IntonationRefusesToCaptureBeforeAnythingConverged) {
    startAt(kFirstRate, 0);
    negotiateCaptureRate(mWma, kFirstRate);
    ASSERT_TRUE(wma_tuner_start(mWma));

    EXPECT_EQ(wma_intonation_state(mWma), WMA_INTONATION_NEED_HARMONIC);
    EXPECT_FALSE(wma_intonation_capture(mWma, WMA_INTONATION_HARMONIC))
        << "capturo sin que el strobe hubiera convergido";

    // Un slot que no existe no puede pasar por una captura valida.
    EXPECT_FALSE(wma_intonation_capture(mWma, 7));
    EXPECT_FALSE(wma_intonation_capture(mWma, -1));

    const float diff = wma_intonation_difference_cents(mWma);
    EXPECT_TRUE(std::isnan(diff))
        << "sin resultado devolvio " << diff << "; 0 seria 'intonacion perfecta'";
}

TEST_F(TunerApiTest, IntonationSurvivesHavingNoEngineAtAll) {
    // La C API tiene que aguantar un puntero nulo sin romperse: es la frontera.
    EXPECT_FALSE(wma_intonation_capture(nullptr, WMA_INTONATION_HARMONIC));
    EXPECT_EQ(wma_intonation_state(nullptr), WMA_INTONATION_NEED_HARMONIC);
    EXPECT_TRUE(std::isnan(wma_intonation_difference_cents(nullptr)));
    wma_intonation_reset(nullptr);   // no debe explotar
}

// ===========================================================================
// REQ-001 S8 — fuentes de entrada
// ===========================================================================
/**
 * El modo de falla que esta seccion evita es SILENCIOSO: si el ring conserva
 * frames de la fuente vieja mientras el estimador sigue integrando, la lectura
 * sale de **mezclar dos señales** y no se ve como un error — se ve como un
 * numero perfectamente formado.
 */

/// 8.4 · AC-001.17 — la fuente activa es consultable y es la real.
TEST_F(TunerApiTest, TheActiveInputSourceIsQueryableAndMatchesWhatWasSet) {
    startAt(kFirstRate, 0);
    negotiateCaptureRate(mWma, kFirstRate);
    ASSERT_TRUE(wma_tuner_start(mWma));

    for (int src : {0, 1, 2, 0}) {
        wma_input_set_source(mWma, src);
        EXPECT_EQ(wma_input_get_source(mWma), src)
            << "se pidio la fuente " << src << " y reporta otra";
    }

    // Una fuente que no existe no puede cambiar la activa.
    wma_input_set_source(mWma, 0);
    wma_input_set_source(mWma, 99);
    EXPECT_EQ(wma_input_get_source(mWma), 0)
        << "una fuente invalida movio la activa";
}

/// 8.2 y 8.3 · AC-001.18 — conmutar tira lo integrado; nada se hereda.
TEST_F(TunerApiTest, SwitchingSourceThrowsAwayEverythingThatWasIntegrating) {
    startAt(kFirstRate, 0);
    negotiateCaptureRate(mWma, kFirstRate);
    ASSERT_TRUE(wma_tuner_start(mWma));
    const double target = 110.0;
    ASSERT_TRUE(wma_tuner_set_target(mWma, static_cast<float>(target)));

    // 🔴 EL OBSERVABLE ES LA MEDICION, Y COSTO DOS INTENTOS ENCONTRARLO.
    //
    // Primero afirme que el estado no quedara "convergido" alimentando silencio.
    // Sobrevivia al mutante que quita el reinicio entero: con silencio el estado
    // va a "sin señal" igual, se haya reiniciado algo o no.
    //
    // Despues use el ENGANCHE del modo rapido. Tampoco sirve, y por una razon
    // que es comportamiento correcto: el enganche se cae con el reinicio y se
    // vuelve a establecer en el mismo tick, porque el musico sigue afinando la
    // misma cuerda. Un testigo que se restaura solo no es un testigo.
    //
    // Lo que de verdad promete AC-001.18 es que **la integracion se tiro**: la
    // fase acumulada sobre el microfono no puede seguir contando cuando los
    // frames vienen de USB. Y eso se ve en que los cents vuelven a NaN — el
    // estimador quedo sin medicion y tiene que juntar ventanas de nuevo.
    // `feedTone` se autorregula contra `framesAnalyzed`: alimentar mas rapido que
    // el tiempo real le daria al estimador una señal CON HUECOS, porque el ring
    // pisa lo viejo por diseño.
    feedTone(mWma, target * std::pow(2.0, 1.0 / 1200.0), kFirstRate, 160, kBlockFrames);
    auto before = sentinelBuffer();
    ASSERT_TRUE(waitForMeasurement(mWma, before))
        << "no llego a producir una medicion antes de conmutar: el test no puede "
           "probar que se tira, porque nunca hubo nada";
    ASSERT_FALSE(std::isnan(before[kSnapCents]));

    wma_input_set_source(mWma, 2);          // a USB

    // 🔴 SE SIGUE ALIMENTANDO EL MISMO TONO, Y ESO ES LO QUE HACE DISCRIMINAR AL
    // TEST. Con silencio los cents darian NaN igual —`hasSignal()` seria falso—
    // aunque el estimador conservara su medicion: es el agujero de la primera
    // version. Con el tono puesto, la unica forma de que salga NaN es que la
    // integracion se haya TIRADO y el estimador tenga que juntar ventanas de nuevo.
    auto after = sentinelBuffer();
    bool wentBlank = false;
    for (int round = 0; round < 30 && !wentBlank; ++round) {
        feedTone(mWma, target * std::pow(2.0, 1.0 / 1200.0), kFirstRate, 2, kBlockFrames);
        // Se sondea un rato: el drenaje es asincrono, y leer justo despues de
        // empujar puede devolver el snapshot ANTERIOR al reinicio.
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
        while (std::chrono::steady_clock::now() < until) {
            if (wma_tuner_get_snapshot(mWma, after.data()) && std::isnan(after[kSnapCents])) {
                wentBlank = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    EXPECT_TRUE(wentBlank)
        << "siguio publicando una desviacion despues de conmutar de fuente: la "
           "fase acumulada sobre el microfono se esta mezclando con frames de USB, "
           "y eso no se ve como un error — se ve como un numero";
}

/**
 * 8.5 — conmutar MIENTRAS el thread de analisis integra no produce carrera.
 *
 * Con COMPUERTA y no con iteraciones: un test de concurrencia que solo repite
 * mucho no pega la ventana, y este repo ya se comio esa leccion. Los dos hilos
 * esperan la misma señal de largada, asi que el cambio de fuente cae adentro del
 * drenaje y no antes ni despues.
 */
TEST_F(TunerApiTest, SwitchingSourceWhileTheAnalysisIntegratesDoesNotRace) {
    startAt(kFirstRate, 0);
    negotiateCaptureRate(mWma, kFirstRate);
    ASSERT_TRUE(wma_tuner_start(mWma));
    ASSERT_TRUE(wma_tuner_set_target(mWma, 440.0f));

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};

    std::atomic<bool> renderDone{false};

    std::thread switcher([&] {
        while (!go.load(std::memory_order_acquire)) { /* compuerta */ }
        for (int i = 0; i < 200 && !stop.load(std::memory_order_acquire); ++i) {
            wma_input_set_source(mWma, i % 3);
        }
        // SEGUNDA COMPUERTA: el ULTIMO cambio de fuente cae DESPUES del ultimo
        // render, a proposito. Ese es el orden con el que esto murio en el CI, y
        // repetir mucho no lo pega: hay que forzarlo.
        while (!renderDone.load(std::memory_order_acquire)) { /* compuerta */ }
        wma_input_set_source(mWma, 1);
    });

    go.store(true, std::memory_order_release);
    for (int i = 0; i < 200; ++i) {
        renderWithInput(1, kBlockFrames, 0.2f);
        auto buf = sentinelBuffer();
        wma_tuner_get_snapshot(mWma, buf.data());   // el lector, en paralelo
    }
    stop.store(true, std::memory_order_release);
    renderDone.store(true, std::memory_order_release);
    switcher.join();

    // Lo que se afirma no es un valor: es que el motor sigue entero y coherente.
    //
    // 🔴 SE ESPERA ALIMENTANDO AUDIO. La version anterior usaba `waitForSnapshot`,
    // que sondea sin empujar nada al ring: con la fuente recien conmutada la
    // integracion arranca de cero y el thread de analisis no tiene de donde sacar
    // un snapshot, asi que la espera se agotaba SIN QUE HUBIERA NADA ROTO.
    auto buf = sentinelBuffer();
    ASSERT_TRUE(waitForSnapshotWhileFeeding(buf))
        << "el motor dejo de publicar despues de conmutar de fuente aun con audio "
           "entrando: eso ya no es el test esperando de gusto, es el afinador mudo";
    EXPECT_GE(buf[kSnapFramesAnalyzed], 0.0f);
    EXPECT_TRUE(wma_tuner_is_running(mWma));
}

/**
 * 8.7 — el objetivo se recomputa contra la tasa REAL del stream.
 *
 * Es el modo de falla mas grosero de la etapa y el mas facil de introducir:
 * asumir 48 k midiendo a 44,1 k corre la lectura **+147 cents**, un semitono y
 * medio. Un afinador que se equivoca por mas de un semitono no es un afinador
 * desafinado: es uno roto.
 */
TEST_F(TunerApiTest, ADifferentCaptureRateDoesNotShiftTheCalibration) {
    startAt(kFirstRate, 0);
    negotiateCaptureRate(mWma, 44100);
    ASSERT_TRUE(wma_tuner_start(mWma));
    for (int i = 0; i < 20; ++i) renderWithInput(1, kBlockFrames, 0.2f);

    auto buf = sentinelBuffer();
    ASSERT_TRUE(waitForSnapshot(mWma, buf));
    EXPECT_EQ(buf[kSnapCaptureSampleRate], 44100.0f)
        << "publico 48000 con el stream a 44100: la calibracion entera cuelga de "
           "este numero, y errarle son +147 cents";

    // Y en caliente: cambiar la tasa tiene que verse en el snapshot siguiente.
    negotiateCaptureRate(mWma, 48000);
    for (int i = 0; i < 20; ++i) renderWithInput(1, kBlockFrames, 0.2f);
    ASSERT_TRUE(waitForValue(mWma, kSnapCaptureSampleRate, 48000.0f, buf));
}
}  // namespace wma_test
