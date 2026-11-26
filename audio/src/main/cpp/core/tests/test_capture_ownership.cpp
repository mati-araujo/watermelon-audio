/**
 * test_capture_ownership.cpp — REQ-054.1: quien es dueño de la captura del InputNode.
 *
 * Los tests salen de los criterios de aceptacion, no de la implementacion. El InputNode tiene UN
 * stream de captura propio (el de Oboe en Android) y lo piden dos clases de dueño:
 *
 *   Consumer  `wma_input_start/stop` (lo que llama `AudioInput`) y el modo INPUT_FX / MIX.
 *   Tuner     `wma_tuner_start/stop` (lo que llama `ITuner`).
 *
 * Un bit por dueño, no un conteo. La captura se cierra cuando el ULTIMO dueño la suelta, y nunca
 * por el stop de otro. Antes de REQ-054.1 `wma_tuner_stop` no la cerraba (el microfono quedaba
 * abierto con el afinador apagado) y `wma_input_stop` la cerraba SIEMPRE (apagaba al afinador
 * que seguia corriendo).
 *
 * EL INSTRUMENTO. El stream es el `FakeCaptureStream` de `support/` (apagado por defecto): cuenta
 * aperturas, cierres y violaciones (abrir abierto, arrancar cerrado, cerrar cerrado, destruirse
 * abierto), y `fakeCaptureStreamDeliver` hace de hilo RT. Cada test arranca con un `WmaEngine`
 * nuevo y la fabrica encendida; el fixture la apaga al salir para no contaminar al resto del
 * binario. Todos los tests afirman `violations == 0`.
 *
 * AC-054.3 necesita un backend que entregue la entrada por callback (LIBUSB). En el host el USB
 * real no se puede construir, asi que se adopta un `FakeAudioBackend` con tipo LIBUSB como
 * `mUsbBackend` y se lo selecciona; el test AFIRMA `getCurrentType() == LIBUSB` antes de medir
 * nada, o estaria midiendo el camino OBOE con nombre de USB.
 *
 * Mapa AC -> test (el ID tambien esta en cada nombre):
 *   AC-054.1  AC054_1_OnlyTheTunerOpensAndItsStopCloses
 *             AC054_1_TunerStopTwiceClosesOnceAndNeverFaults
 *             AC054_1_TunerStopWithoutStartTouchesNothing
 *             AC054_1_TwoTunerStartsAndOneStopClose            (bit, no conteo)
 *   AC-054.2  AC054_2_TunerStopKeepsTheConsumersCapture_TunerFirst
 *             AC054_2_TunerStopKeepsTheConsumersCapture_InputFirst
 *             AC054_2_TunerStopKeepsTheCaptureWhenTheModeIsAttached_InputFx / _Mix
 *             AC054_2_ASecondTunerStopDoesNotReleaseTheConsumer
 *             AC054_2_ConsumerStopKeepsTheTunersCapture_TunerFirst / _InputFirst   (mitad simetrica)
 *             AC054_2_InputStopWithoutInputStartKeepsTheTunersCapture              (suplencia de Tunio)
 *             AC054_2_TwoInputStartsAndOneStopClose            (bit, no conteo)
 *             AC054_2_ModeAndInputStartShareOneConsumerBit
 *             AC054_2_StartStopFromTwoControlThreadsLeavesNoOrphan_BothStopped / _ConsumerLeftRunning
 *   AC-054.3  AC054_3_ABackendThatDeliversInputNeverSeesAStreamOpenOrClose
 *   AC-054.4  AC054_4_ANewTunerAfterItsOwnStopReopensAndAnalysesAgain
 */

#include "tests/support/SnapshotRead.h"
#include "tests/support/TestWait.h"
#include "support/CApiFixture.h"
#include "support/FakeCaptureStream.h"

#include "api/watermelon_audio.h"
#include "api/watermelon_audio_internal.h"
#include "analysis/AnalysisSnapshot.h"
#include "analysis/AnalysisThread.h"
#include "backends/BackendManager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace watermelon_audio {

/// El `friend` que BackendManager declara para tests; definido SOLO aqui (mismo patron que
/// usb/tests/libusb/test_libusb_backend_lifetime.cpp, que es otro binario).
struct BackendManagerTestAccess {
    static void adopt(BackendManager& manager, std::unique_ptr<IAudioBackend> backend) {
        std::lock_guard<std::mutex> op(manager.mOpMutex);
        std::lock_guard<std::mutex> lock(manager.mMutex);
        manager.mUsbBackend = std::move(backend);
        manager.mUsbBackendAvailable.store(true, std::memory_order_release);
    }
};

}  // namespace watermelon_audio

namespace wma_test {
namespace {

using namespace std::chrono_literals;
using wma::analysis::kSnapFramesAnalyzed;

constexpr int kModeInputFx = 1;
constexpr int kModeMix = 2;

constexpr int kBlockFrames = 256;

struct CApiSnapshot {
    WmaEngine* e;
    bool read(float* out) const { return wma_tuner_get_snapshot(e, out); }
    bool hasData() const { return e->analysisSnapshot && e->analysisSnapshot->hasData(); }
};

class CaptureOwnership : public CApiFixture {
protected:
    void SetUp() override {
        // Fabrica encendida ANTES de crear el motor: el nodo crea su stream perezosamente, y un
        // test nuevo no debe heredar ni contadores ni un stream de otro.
        resetFakeCaptureStream();
        setFakeCaptureStreamAvailable(true);
        CApiFixture::SetUp();
    }

    void TearDown() override {
        CApiFixture::TearDown();       // destruye el motor: cierra lo que haya quedado abierto
        resetFakeCaptureStream();      // y apaga la fabrica para el resto del binario
    }

    /// Lo que TODO test afirma al final: el fake no vio un uso ilegal de su stream.
    void expectNoViolations() const {
        EXPECT_EQ(fakeCaptureStreamViolations(), 0)
            << "el stream de captura vio abrir abierto / arrancar cerrado / cerrar cerrado / "
               "destruirse abierto";
    }

    void expectOpen(int opens, int closes, const char* where) const {
        SCOPED_TRACE(where);
        EXPECT_TRUE(fakeCaptureStreamIsOpen()) << "la captura tenia que seguir abierta";
        EXPECT_EQ(fakeCaptureStreamOpens(), opens);
        EXPECT_EQ(fakeCaptureStreamCloses(), closes);
    }

    void expectClosed(int opens, int closes, const char* where) const {
        SCOPED_TRACE(where);
        EXPECT_FALSE(fakeCaptureStreamIsOpen()) << "la captura tenia que estar cerrada";
        EXPECT_EQ(fakeCaptureStreamOpens(), opens);
        EXPECT_EQ(fakeCaptureStreamCloses(), closes);
    }

    /// Modo adjunto (Consumer) + afinador: tuner_stop deja la captura, input_stop la cierra.
    void modeAttachedScenario(int mode) {
        wma_set_audio_mode(mWma, mode);
        expectOpen(1, 0, "el modo abre la captura");
        ASSERT_TRUE(wma_tuner_start(mWma));
        expectOpen(1, 0, "el afinador se suma sin reabrir");

        wma_tuner_stop(mWma);
        expectOpen(1, 0, "tras tuner_stop: la tiene el modo (Consumer)");

        wma_input_stop(mWma);
        expectClosed(1, 1, "tras input_stop: ultimo dueño, el modo no queda colgado");
        expectNoViolations();
    }

    /// Deja al manager en LIBUSB (el backend entrega la entrada): ver el comentario del archivo.
    void selectUsbBackend() {
        auto usb = std::make_unique<FakeAudioBackend>();
        usb->setType(watermelon_audio::BackendType::LIBUSB);
        auto& manager = watermelon_audio::BackendManager::getInstance();
        watermelon_audio::BackendManagerTestAccess::adopt(manager, std::move(usb));
        ASSERT_TRUE(manager.selectBackend(watermelon_audio::BackendType::LIBUSB));
        ASSERT_EQ(manager.getCurrentType(), watermelon_audio::BackendType::LIBUSB)
            << "el manager no quedo en LIBUSB: el test mediria el camino OBOE con nombre de USB";
    }

    double framesAnalyzed() const {
        return wma_test::readCoherentValueOr(CApiSnapshot{mWma}, kSnapFramesAnalyzed, 0.0);
    }

    /// Empuja audio por el "hilo RT" falso hasta que `framesAnalyzed` pase de `target`. Devuelve
    /// si algun bloque llego a entrar a la captura (stream abierto y arrancado).
    bool feedUntilAnalysedBeyond(double target, bool& everDelivered) {
        std::vector<float> block(static_cast<size_t>(kBlockFrames) * 2);
        double phase = 0.0;
        everDelivered = false;
        return waitUntil(
            [&] {
                for (int f = 0; f < kBlockFrames; ++f) {
                    const float v = static_cast<float>(0.2 * std::sin(phase));
                    phase += 2.0 * M_PI * 220.0 / 48000.0;
                    block[static_cast<size_t>(f) * 2] = v;
                    block[static_cast<size_t>(f) * 2 + 1] = v;
                }
                if (fakeCaptureStreamDeliver(block.data(), kBlockFrames)) everDelivered = true;
                return framesAnalyzed() > target;
            },
            5000ms);
    }
};

// ============================================================================
// AC-054.1 — sólo el afinador: start() abre, stop() cierra antes de devolver.
// ============================================================================

// Bug que atrapa: `wma_tuner_stop` no cierra la captura que abrio `wma_tuner_start` (el
// microfono queda abierto con el afinador apagado).
TEST_F(CaptureOwnership, AC054_1_OnlyTheTunerOpensAndItsStopCloses) {
    ASSERT_TRUE(wma_tuner_start(mWma));
    expectOpen(1, 0, "tras tuner_start");

    wma_tuner_stop(mWma);
    // Sin espera: "antes de devolver" es la afirmacion, y el cierre del fake es sincronico.
    expectClosed(1, 1, "tras tuner_stop");
    expectNoViolations();
}

// Bug que atrapa: un segundo `tuner_stop` cierra de nuevo (violacion "cerrar cerrado") o
// deja de cerrar el primero porque el bit se limpia antes de actuar.
TEST_F(CaptureOwnership, AC054_1_TunerStopTwiceClosesOnceAndNeverFaults) {
    ASSERT_TRUE(wma_tuner_start(mWma));
    wma_tuner_stop(mWma);
    wma_tuner_stop(mWma);
    expectClosed(1, 1, "tras dos tuner_stop");
    expectNoViolations();
}

// Bug que atrapa: un `tuner_stop` sin start previo "libera" algo que nunca abrio y revienta el
// stream (o abre uno para cerrarlo).
TEST_F(CaptureOwnership, AC054_1_TunerStopWithoutStartTouchesNothing) {
    wma_tuner_stop(mWma);                 // ni siquiera hay InputNode todavia
    expectClosed(0, 0, "tuner_stop sobre un motor virgen");

    ASSERT_TRUE(wma_tuner_start(mWma));
    wma_tuner_stop(mWma);
    wma_tuner_stop(mWma);
    expectClosed(1, 1, "ciclo normal + stop de mas");
    expectNoViolations();
}

// Bug que atrapa: el dueño "Tuner" implementado como CONTEO (dos starts, un stop => sigue abierta).
TEST_F(CaptureOwnership, AC054_1_TwoTunerStartsAndOneStopClose) {
    ASSERT_TRUE(wma_tuner_start(mWma));
    ASSERT_TRUE(wma_tuner_start(mWma));
    expectOpen(1, 0, "dos tuner_start abren UNA sola vez");

    wma_tuner_stop(mWma);
    expectClosed(1, 1, "un tuner_stop alcanza");
    expectNoViolations();
}

// ============================================================================
// AC-054.2 — con un consumidor, ITuner.stop() deja la captura; la cierra el último dueño.
// ============================================================================

// Bug que atrapa: `tuner_stop` cierra la captura que el consumidor (AudioInput) arranco antes.
TEST_F(CaptureOwnership, AC054_2_TunerStopKeepsTheConsumersCapture_InputFirst) {
    ASSERT_TRUE(wma_input_start(mWma));
    ASSERT_TRUE(wma_tuner_start(mWma));
    expectOpen(1, 0, "ambos arrancados");

    wma_tuner_stop(mWma);
    expectOpen(1, 0, "tras tuner_stop: la tiene el consumidor");

    wma_input_stop(mWma);
    expectClosed(1, 1, "tras input_stop: ultimo dueño");
    expectNoViolations();
}

// Bug que atrapa: el afinador arrancado PRIMERO y el consumidor despues => `tuner_stop` cierra
// igual (el orden de arranque decide el dueño), o `input_start` reabre una captura ya abierta.
TEST_F(CaptureOwnership, AC054_2_TunerStopKeepsTheConsumersCapture_TunerFirst) {
    ASSERT_TRUE(wma_tuner_start(mWma));
    ASSERT_TRUE(wma_input_start(mWma));
    expectOpen(1, 0, "ambos arrancados, sin reabrir");

    wma_tuner_stop(mWma);
    expectOpen(1, 0, "tras tuner_stop: la tiene el consumidor");

    wma_input_stop(mWma);
    expectClosed(1, 1, "tras input_stop: ultimo dueño");
    expectNoViolations();
}

// Bug que atrapa: el modo INPUT_FX/MIX adjunto cuenta como dueño PROPIO que nunca suelta (la
// captura quedaria abierta para siempre), o no cuenta (tuner_stop la cierra bajo el modo).
TEST_F(CaptureOwnership, AC054_2_TunerStopKeepsTheCaptureWhenTheModeIsAttached_InputFx) {
    modeAttachedScenario(kModeInputFx);
}

TEST_F(CaptureOwnership, AC054_2_TunerStopKeepsTheCaptureWhenTheModeIsAttached_Mix) {
    modeAttachedScenario(kModeMix);
}

// Bug que atrapa: Consumer implementado como dos dueños distintos (modo y wma_input_start): un
// solo `wma_input_stop` no alcanza y el microfono queda abierto.
TEST_F(CaptureOwnership, AC054_2_ModeAndInputStartShareOneConsumerBit) {
    wma_set_audio_mode(mWma, kModeInputFx);
    ASSERT_TRUE(wma_input_start(mWma));
    expectOpen(1, 0, "modo + input_start abren UNA vez");

    wma_input_stop(mWma);
    expectClosed(1, 1, "un input_stop suelta el Consumer entero");
    expectNoViolations();
}

// Bug que atrapa: el dueño "Consumer" implementado como CONTEO (dos starts, un stop => abierta).
TEST_F(CaptureOwnership, AC054_2_TwoInputStartsAndOneStopClose) {
    ASSERT_TRUE(wma_input_start(mWma));
    ASSERT_TRUE(wma_input_start(mWma));
    expectOpen(1, 0, "dos input_start abren una sola vez");

    wma_input_stop(mWma);
    expectClosed(1, 1, "un input_stop alcanza");
    expectNoViolations();
}

// Bug que atrapa: un segundo `tuner_stop` (idempotencia) le suelta al consumidor lo que no es
// del afinador (stop implementado como decremento de un conteo).
TEST_F(CaptureOwnership, AC054_2_ASecondTunerStopDoesNotReleaseTheConsumer) {
    ASSERT_TRUE(wma_input_start(mWma));
    ASSERT_TRUE(wma_tuner_start(mWma));
    wma_tuner_stop(mWma);
    wma_tuner_stop(mWma);
    expectOpen(1, 0, "dos tuner_stop con un consumidor vivo");

    wma_input_stop(mWma);
    expectClosed(1, 1, "el consumidor cierra al final");
    expectNoViolations();
}

// Bug que atrapa: `wma_input_start` que devuelve false deja el bit Consumer puesto; el
// consumidor no llamara `stop()`, y despues `tuner_stop` no cierra (el microfono queda abierto).
TEST_F(CaptureOwnership, AC054_2_AFailedInputStartLeavesNoConsumerOwner) {
    setFakeCaptureStreamOpenFails(true);
    ASSERT_FALSE(wma_input_start(mWma)) << "sin stream propio ni backend, el start informa fallo";
    expectClosed(0, 0, "input_start fallido");

    setFakeCaptureStreamOpenFails(false);
    ASSERT_TRUE(wma_tuner_start(mWma));
    expectOpen(1, 0, "el afinador abre");

    wma_tuner_stop(mWma);
    expectClosed(1, 1, "tuner_stop cierra: el Consumer fallido no es dueño");
    expectNoViolations();
}

// --- La mitad simetrica: AudioInput.stop() ya no apaga al afinador que sigue corriendo. -------

// Bug que atrapa: `wma_input_stop` cierra SIEMPRE y deja al afinador corriendo sobre una captura
// muerta (el afinador queda en silencio sin un solo error).
TEST_F(CaptureOwnership, AC054_2_ConsumerStopKeepsTheTunersCapture_TunerFirst) {
    ASSERT_TRUE(wma_tuner_start(mWma));
    ASSERT_TRUE(wma_input_start(mWma));

    wma_input_stop(mWma);
    expectOpen(1, 0, "tras input_stop: la tiene el afinador");
    const std::vector<float> block(static_cast<size_t>(kBlockFrames) * 2, 0.0f);
    EXPECT_TRUE(fakeCaptureStreamDeliver(block.data(), kBlockFrames))
        << "la captura del afinador tiene que seguir entregando audio";

    wma_tuner_stop(mWma);
    expectClosed(1, 1, "tras tuner_stop: ultimo dueño");
    expectNoViolations();
}

// Bug que atrapa: igual que el anterior con el consumidor arrancado primero (el orden de arranque
// no puede cambiar quien suelta que).
TEST_F(CaptureOwnership, AC054_2_ConsumerStopKeepsTheTunersCapture_InputFirst) {
    ASSERT_TRUE(wma_input_start(mWma));
    ASSERT_TRUE(wma_tuner_start(mWma));

    wma_input_stop(mWma);
    expectOpen(1, 0, "tras input_stop: la tiene el afinador");

    wma_tuner_stop(mWma);
    expectClosed(1, 1, "tras tuner_stop: ultimo dueño");
    expectNoViolations();
}

// Bug que atrapa: la suplencia de Tunio (`AudioInput.stop()` sin que nadie haya llamado
// `start()`) le cierra la captura al afinador que esta corriendo.
TEST_F(CaptureOwnership, AC054_2_InputStopWithoutInputStartKeepsTheTunersCapture) {
    ASSERT_TRUE(wma_tuner_start(mWma));

    wma_input_stop(mWma);        // sin wma_input_start previo
    expectOpen(1, 0, "input_stop sin dueño Consumer no toca la captura del afinador");

    wma_tuner_stop(mWma);
    expectClosed(1, 1, "tras tuner_stop");
    expectNoViolations();
}

// Bug que atrapa: el cambio de fuente (stop+start del stream) pierde a los dueños: el stream
// reabierto queda sin quien lo cierre, y `tuner_stop` lo deja abierto.
TEST_F(CaptureOwnership, AC054_1_ChangingTheInputSourceKeepsTheTunerAsOwner) {
    ASSERT_TRUE(wma_tuner_start(mWma));
    expectOpen(1, 0, "tuner_start");

    wma_input_set_source(mWma, 1);
    expectOpen(2, 1, "set_source reinicia el stream con el afinador vivo");

    wma_tuner_stop(mWma);
    expectClosed(2, 2, "tuner_stop cierra el stream reiniciado");
    expectNoViolations();
}

// ============================================================================
// AC-054.3 — un backend que entrega la entrada por callback: ningún stream propio.
// ============================================================================

// Bug que atrapa: el afinador abre (o cierra) el stream propio del nodo aun cuando el backend
// entrega la entrada por onAudioReady (LIBUSB), peleando con el stream USB.
TEST_F(CaptureOwnership, AC054_3_ABackendThatDeliversInputNeverSeesAStreamOpenOrClose) {
    auto usb = std::make_unique<FakeAudioBackend>();
    usb->setType(watermelon_audio::BackendType::LIBUSB);
    auto& manager = watermelon_audio::BackendManager::getInstance();
    watermelon_audio::BackendManagerTestAccess::adopt(manager, std::move(usb));
    ASSERT_TRUE(manager.selectBackend(watermelon_audio::BackendType::LIBUSB));
    ASSERT_EQ(manager.getCurrentType(), watermelon_audio::BackendType::LIBUSB)
        << "el manager no quedo en LIBUSB: este test mediria el camino OBOE con nombre de USB";

    ASSERT_TRUE(wma_tuner_start(mWma)) << "el afinador tiene que arrancar igual sobre USB";
    EXPECT_EQ(fakeCaptureStreamOpens(), 0) << "el afinador abrio un stream propio sobre USB";
    EXPECT_FALSE(fakeCaptureStreamIsOpen());

    wma_tuner_stop(mWma);
    EXPECT_EQ(fakeCaptureStreamOpens(), 0);
    EXPECT_EQ(fakeCaptureStreamCloses(), 0) << "el afinador cerro un stream que no abrio";
    expectNoViolations();
}

// Bug que atrapa: con USB, `input_stop` deja abierto el stream propio "para el afinador" (que
// sólo se registro) y lo cierra `tuner_stop`, que sobre USB no debe tocar ningun stream.
TEST_F(CaptureOwnership, AC054_3_UsbInputStopClosesTheOwnStreamEvenWithTheTunerRegistered) {
    selectUsbBackend();
    ASSERT_TRUE(wma_tuner_start(mWma));
    EXPECT_EQ(fakeCaptureStreamOpens(), 0);

    ASSERT_TRUE(wma_input_start(mWma)) << "input_start sobre USB sigue abriendo el stream propio";
    expectOpen(1, 0, "input_start abre");

    wma_input_stop(mWma);
    expectClosed(1, 1, "input_stop cierra lo que abrio el consumidor");

    wma_tuner_stop(mWma);
    expectClosed(1, 1, "tuner_stop sobre USB no abre ni cierra nada");
    expectNoViolations();
}

// Bug que atrapa: con USB, `tuner_stop` cierra el stream propio que abrio el consumidor (la
// regla "ultimo dueño" sin distinguir quien pidio el stream propio).
TEST_F(CaptureOwnership, AC054_3_UsbTunerStopKeepsTheConsumersOwnStream) {
    selectUsbBackend();
    ASSERT_TRUE(wma_input_start(mWma));
    ASSERT_TRUE(wma_tuner_start(mWma));
    expectOpen(1, 0, "consumidor + afinador registrado");

    wma_tuner_stop(mWma);
    expectOpen(1, 0, "tuner_stop: el stream es del consumidor");

    wma_input_stop(mWma);
    expectClosed(1, 1, "input_stop cierra");
    expectNoViolations();
}

// Bug que atrapa: borrar `closeOwnStreamForExternalDelivery()` de `wmaAttachInputForMode`: el
// modo entra con USB y el stream propio que ya corria sigue peleando con el del backend.
TEST_F(CaptureOwnership, AC054_3_EnteringAModeOnUsbClosesTheOwnStream) {
    ASSERT_TRUE(wma_input_start(mWma));
    expectOpen(1, 0, "input_start sobre el backend de sistema");

    selectUsbBackend();
    wma_set_audio_mode(mWma, kModeInputFx);
    expectClosed(1, 1, "el modo sobre USB cierra el stream propio");

    wma_input_stop(mWma);
    expectClosed(1, 1, "input_stop posterior no cierra ni abre nada mas");
    expectNoViolations();
}

// ============================================================================
// AC-054.4 — start() tras un stop() que cerró la captura la reabre y el análisis avanza.
// ============================================================================

// Bug que atrapa: tras cerrar la captura, el siguiente `tuner_start` cree que el stream sigue
// vivo (flag viejo) y no lo reabre, o lo reabre pero el analisis no vuelve a recibir audio.
TEST_F(CaptureOwnership, AC054_4_ANewTunerAfterItsOwnStopReopensAndAnalysesAgain) {
    // Ciclo de Tunio: al fondo, al frente, un ITuner nuevo.
    ASSERT_TRUE(wma_tuner_start(mWma));
    bool delivered = false;
    ASSERT_TRUE(feedUntilAnalysedBeyond(0.0, delivered)) << "la primera sesion no analizo nada";
    ASSERT_TRUE(delivered);

    wma_tuner_stop(mWma);
    EXPECT_FALSE(fakeCaptureStreamIsOpen()) << "el stop del afinador tenia que cerrar la captura";
    EXPECT_EQ(fakeCaptureStreamCloses(), 1);

    ASSERT_TRUE(wma_tuner_start(mWma));
    EXPECT_EQ(fakeCaptureStreamOpens(), 2) << "el segundo start tenia que REABRIR la captura";
    EXPECT_TRUE(fakeCaptureStreamIsOpen());

    // Mas que el ring entero (8192 frames) por encima de lo que ya habia: lo que el ring
    // retuvo de la primera sesion no alcanza, tiene que venir audio nuevo por la captura nueva.
    const double before = framesAnalyzed();
    ASSERT_TRUE(feedUntilAnalysedBeyond(before + 9000.0, delivered))
        << "el analisis no volvio a avanzar tras reabrir";
    EXPECT_TRUE(delivered) << "ningun bloque entro por la captura reabierta";

    wma_tuner_stop(mWma);
    expectClosed(2, 2, "fin");
    expectNoViolations();
}

// ============================================================================
// Concurrencia — dos hilos de control, sólo la C API, sin sleeps.
// ============================================================================

/// Si el test se cuelga (deadlock en el dueño), aborta con un mensaje en vez de dejar al runner
/// esperando el timeout de ctest. Se desarma con el destructor; espera por condicion, no duerme.
class HangWatchdog {
public:
    explicit HangWatchdog(std::chrono::seconds limit) {
        mThread = std::thread([this, limit] {
            std::unique_lock<std::mutex> lock(mMutex);
            if (!mCv.wait_for(lock, limit, [this] { return mDisarmed; })) {
                std::fprintf(stderr, "[CaptureOwnership] el test de concurrencia se colgo (>%llds)\n",
                             static_cast<long long>(limit.count()));
                std::abort();
            }
        });
    }
    ~HangWatchdog() {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mDisarmed = true;
        }
        mCv.notify_all();
        mThread.join();
    }

private:
    std::mutex mMutex;
    std::condition_variable mCv;
    bool mDisarmed = false;
    std::thread mThread;
};

constexpr int kRounds = 100;

/// Dos hilos de control que SOLO llaman la C API. El afinador hace `kRounds - 1` ciclos EN
/// PARALELO con el consumidor (que cicla mientras el afinador no avise que termino la tanda; asi
/// el solapamiento dura todo el test aunque sus ciclos sean mas baratos). Despues el consumidor
/// hace su cierre y SOLO ENTONCES el afinador hace su ultimo ciclo: el estado final lo decide el
/// ultimo en soltar, y tiene que ser el afinador para que un stop de otro no pueda taparle a
/// `tuner_stop` el cierre que no hizo. Sin sleeps: compuertas atomicas.
/// `consumerLeavesRunning`: el consumidor termina con `input_start` en pie (y el afinador, detenido).
void runTwoControlThreads(WmaEngine* e, bool consumerLeavesRunning) {
    HangWatchdog watchdog(120s);
    std::atomic<bool> go{false};
    std::atomic<bool> tunerBatchDone{false};
    std::atomic<bool> consumerDone{false};
    auto spinUntil = [](const std::atomic<bool>& flag) {
        while (!flag.load(std::memory_order_acquire)) std::this_thread::yield();
    };
    std::thread tuner([&] {
        spinUntil(go);
        for (int i = 0; i < kRounds - 1; ++i) {
            wma_tuner_start(e);
            wma_tuner_stop(e);
        }
        tunerBatchDone.store(true, std::memory_order_release);
        spinUntil(consumerDone);
        wma_tuner_start(e);
        wma_tuner_stop(e);
    });
    std::thread consumer([&] {
        spinUntil(go);
        do {
            wma_input_start(e);
            wma_input_stop(e);
        } while (!tunerBatchDone.load(std::memory_order_acquire));
        wma_input_start(e);
        if (!consumerLeavesRunning) wma_input_stop(e);
        consumerDone.store(true, std::memory_order_release);
    });
    go.store(true, std::memory_order_release);
    tuner.join();
    consumer.join();
}

// Bug que atrapa: start/stop de dos dueños sin serializar dejan la captura abierta sin dueño
// (o cierran con dueño), o abren dos veces / cierran dos veces (violaciones del fake).
TEST_F(CaptureOwnership, AC054_2_StartStopFromTwoControlThreadsLeavesNoOrphan_BothStopped) {
    runTwoControlThreads(mWma, /*consumerLeavesRunning=*/false);

    EXPECT_FALSE(fakeCaptureStreamIsOpen())
        << "los dos dueños estan detenidos y la captura quedo abierta sin dueño";
    EXPECT_EQ(fakeCaptureStreamOpens(), fakeCaptureStreamCloses())
        << "aperturas y cierres tienen que emparejar";
    expectNoViolations();
}

// Bug que atrapa: la carrera deja la captura CERRADA aunque un dueño (el consumidor, que termina
// arrancado) la tiene, porque el stop del afinador le gano al start del consumidor.
TEST_F(CaptureOwnership, AC054_2_StartStopFromTwoControlThreadsLeavesNoOrphan_ConsumerLeftRunning) {
    runTwoControlThreads(mWma, /*consumerLeavesRunning=*/true);

    EXPECT_TRUE(fakeCaptureStreamIsOpen())
        << "el consumidor termino arrancado y la captura quedo cerrada con dueño";
    EXPECT_EQ(fakeCaptureStreamOpens(), fakeCaptureStreamCloses() + 1)
        << "una apertura sin cerrar: la del consumidor";
    expectNoViolations();

    wma_input_stop(mWma);
    EXPECT_FALSE(fakeCaptureStreamIsOpen());
    expectNoViolations();
}

}  // namespace
}  // namespace wma_test
