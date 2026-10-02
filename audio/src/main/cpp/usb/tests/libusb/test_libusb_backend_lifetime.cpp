/**
 * test_libusb_backend_lifetime.cpp — MINI-042: el backend USB no se lee después de liberado.
 *
 * El `LibusbBackend` es el REAL, compilado para el host sobre el núcleo de libusb vendorizado y
 * el backend de SO falso de REQ-047 S1 (ver CMakeLists.txt de este directorio). Dos costuras,
 * declaradas como `friend` en producción y definidas SÓLO acá (D6):
 *
 *   - `BackendManagerTestAccess::adopt()` pone ese backend como `mUsbBackend`. El fake de SO no
 *     alcanza para que `initializeFromFileDescriptor()` complete; adoptarlo es lo que hace
 *     falta para que el acceso con alcance tenga algo que proteger.
 *   - `LibusbBackendTestAccess::recreateTransferManagerLikeStart()` destruye y recrea
 *     `mTransferManager` bajo `mMutex`, igual que `start()`. Un `start()` real necesita un
 *     device que acepte transferencias isócronas, y el fake no lo hace.
 *
 * Lo que NO prueba: la JNI. Las JNIEXPORT se compilan contra este mismo acceso (y el arnés
 * JNI de androidUnitTest las carga), pero en el host el `LibusbBackend` del arnés es un stub
 * vacío. Lo que se afirma acá es el CONTRATO que la JNI usa.
 *
 * Bajo ASan, el código previo a MINI-042 da heap-use-after-free en los dos primeros tests; bajo
 * TSan, carrera de datos sobre `mTransferManager` en el de estrés.
 */

#include <gtest/gtest.h>

#include "backends/BackendManager.h"
#include "backends/LibusbBackend.h"
#include "backends/UsbTransferStatsArray.h"
#include "tests/support/TestWait.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

using watermelon_audio::BackendManager;
using watermelon_audio::LibusbBackend;
using namespace std::chrono_literals;

namespace watermelon_audio {

struct BackendManagerTestAccess {
    static void adopt(BackendManager& manager, std::unique_ptr<IAudioBackend> backend) {
        std::lock_guard<std::mutex> op(manager.mOpMutex);
        std::lock_guard<std::mutex> lock(manager.mMutex);
        manager.mUsbBackend = std::move(backend);
        manager.mUsbBackendAvailable.store(true, std::memory_order_release);
    }
};

struct LibusbBackendTestAccess {
    /// Lo que hace `start()` con el transfer manager: teardown + make_unique, bajo mMutex.
    static void recreateTransferManagerLikeStart(LibusbBackend& backend) {
        std::lock_guard<std::mutex> lock(backend.mMutex);
        backend.teardownTransferManager();
        backend.mTransferManager =
            std::make_unique<usb::UsbTransferManager>(backend.mDeviceHandle, backend.mContext);
        seedStatsLocked(backend);
    }

    /// Un backend "con stream" a los ojos de los getters: device listo, captura elegida y
    /// stats distintas de cero, para que "sin dato" se distinga de "dato que dice cero".
    static void makeStreamLike(LibusbBackend& backend) {
        backend.mDeviceReady.store(true);
        backend.mSelectedCapture = usb::UsbStreamingInterface{};
        recreateTransferManagerLikeStart(backend);
    }

    /// Sostiene mMutex como un `start()` en curso. Lo suelta `release()`.
    static void holdLikeStart(LibusbBackend& backend) { backend.mMutex.lock(); }
    static void release(LibusbBackend& backend) { backend.mMutex.unlock(); }

    static bool profilerEnabled(LibusbBackend& backend) {
        std::lock_guard<std::mutex> lock(backend.mMutex);
        return backend.mTransferManager && backend.mTransferManager->getLatencyProfiler().isEnabled();
    }

private:
    static void seedStatsLocked(LibusbBackend& backend) {
        // getStatistics() sólo tiene versión const; el objeto no lo es, así que el cast es legal.
        auto& stats = const_cast<usb::TransferStatistics&>(backend.mTransferManager->getStatistics());
        stats.packetsSubmitted.store(5);
        stats.packetsCompleted.store(3);
        stats.currentLatencyMs.store(7.5f);
        stats.currentInputLatencyMs.store(3.25f);
    }
};

}  // namespace watermelon_audio

using watermelon_audio::BackendManagerTestAccess;
using watermelon_audio::LibusbBackendTestAccess;

namespace {

/// Avisa cuándo EMPIEZA su destrucción: lo que el test necesita ver es si el manager la
/// dispara mientras un acceso sigue adentro.
class ObservedLibusbBackend : public LibusbBackend {
public:
    explicit ObservedLibusbBackend(std::atomic<bool>& destroyed) : mDestroyed(destroyed) {}
    ~ObservedLibusbBackend() override { mDestroyed.store(true); }

private:
    std::atomic<bool>& mDestroyed;
};

/// La estimación por configuración que `getOutputLatencyMs()` da sin stream (256 / 48 kHz).
constexpr float kNoStreamOutputLatencyMs = 256.0f / 48000.0f * 1000.0f;

}  // namespace

// ---------------------------------------------------------------------------------------
// AC-042.1 — el acceso con alcance impide que fallbackToOboe libere el backend.
// Bug que atrapa: el puntero crudo que salía de getLibusbBackend() después de soltar el lock,
// y los reset() de fallbackToOboe sin lock. Antes de MINI-042: ASan heap-use-after-free en la
// lectura de adentro, y `destroyed` en true mientras la lectura sigue.
// ---------------------------------------------------------------------------------------
TEST(LibusbBackendLifetime, AC_042_1_FallbackWaitsForAReadInProgress) {
    std::atomic<bool> destroyed{false};
    BackendManager manager;
    BackendManagerTestAccess::adopt(manager, std::make_unique<ObservedLibusbBackend>(destroyed));
    // El tipo YA es OBOE: así el selectBackend de adentro de fallbackToOboe vuelve sin tomar
    // mMutex, y lo único que puede frenar al fallback es el DESENGANCHE. Sin esto, el test
    // quedaba serializado por el mMutex de selectBackend y no veía un desenganche sin lock
    // (review de MINI-042).
    ASSERT_TRUE(manager.selectBackend(watermelon_audio::BackendType::OBOE));

    std::atomic<bool> inside{false};
    std::atomic<bool> releaseReader{false};
    std::atomic<bool> destroyedWhileInside{true};

    std::thread reader([&] {
        manager.withLibusbBackend([&](LibusbBackend* backend) {
            ASSERT_NE(backend, nullptr);
            inside.store(true);
            ASSERT_TRUE(wma_test::waitUntil([&] { return releaseReader.load(); }, 10000ms));
            destroyedWhileInside.store(destroyed.load());
            // Lo que leía la JNI. Con el backend liberado, ASan lo reporta acá.
            (void)backend->getTransferStatsSnapshot();
            (void)backend->getCapabilities();
            (void)backend->isUsbDeviceReady();
        });
    });

    ASSERT_TRUE(wma_test::waitUntil([&] { return inside.load(); }));
    std::atomic<bool> fallbackReturned{false};
    std::thread fallback([&] {
        manager.fallbackToOboe();
        fallbackReturned.store(true);
    });

    // Ausencia: con la lectura adentro, el fallback NO puede llegar a destruir.
    wma_test::sleepFixed(50ms);
    EXPECT_FALSE(destroyed.load()) << "fallbackToOboe destruyó el backend con una lectura en curso";
    EXPECT_FALSE(fallbackReturned.load());

    releaseReader.store(true);
    reader.join();
    fallback.join();

    EXPECT_FALSE(destroyedWhileInside.load());
    EXPECT_TRUE(destroyed.load()) << "el fallback tiene que destruir el backend al terminar la lectura";
    EXPECT_TRUE(manager.withLibusbBackend([](LibusbBackend* b) { return b == nullptr; }))
        << "después del fallback nadie puede volver a encontrar el backend";
}

// ---------------------------------------------------------------------------------------
// AC-042.1 — estrés: lectores entrando y saliendo mientras fallbackToOboe desengancha. El test
// de arriba fija UNA lectura adentro; éste cubre la que llega DESPUÉS de que el fallback cambió
// de backend y antes de que desenganche. Bajo TSan, un desenganche sin mMutex sale como carrera
// sobre mUsbBackend; bajo ASan, como uso de memoria liberada.
// ---------------------------------------------------------------------------------------
TEST(LibusbBackendLifetime, AC_042_1_FallbackRacesReadersComingAndGoing) {
    BackendManager manager;
    constexpr int kRounds = 100;
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};

    std::thread reader([&] {
        while (!stop.load()) {
            manager.withLibusbBackend([](LibusbBackend* backend) {
                if (backend) {
                    (void)backend->getTransferStatsSnapshot();
                    (void)backend->isUsbDeviceReady();
                }
            });
            reads.fetch_add(1);
        }
    });

    ASSERT_TRUE(wma_test::waitUntil([&] { return reads.load() > 0; }));
    for (int i = 0; i < kRounds; ++i) {
        BackendManagerTestAccess::adopt(manager, std::make_unique<LibusbBackend>());
        const int before = reads.load();
        // Que el lector lo vea al menos una vez antes de sacarlo.
        ASSERT_TRUE(wma_test::waitUntil([&] { return reads.load() > before; }));
        manager.fallbackToOboe();
    }
    stop.store(true);
    reader.join();

    EXPECT_TRUE(manager.withLibusbBackend([](LibusbBackend* b) { return b == nullptr; }));
}

// ---------------------------------------------------------------------------------------
// AC-042.1 + D5 — una operación de ciclo de vida (start/stop de la JNI) tampoco puede quedar
// adentro de un backend destruido: fallbackToOboe y initializeUsbBackend esperan por mOpMutex.
// Bug que atrapa: un destructor que toma sólo mMutex (initializeUsbBackend sin mOpMutex) y
// libera el backend mientras la JNI lo está arrancando.
// ---------------------------------------------------------------------------------------
class LifecycleVersusDestroyer : public ::testing::TestWithParam<int> {};

TEST_P(LifecycleVersusDestroyer, AC_042_1_DestroyerWaitsForALifecycleOperation) {
    std::atomic<bool> destroyed{false};
    BackendManager manager;
    BackendManagerTestAccess::adopt(manager, std::make_unique<ObservedLibusbBackend>(destroyed));

    std::atomic<bool> inside{false};
    std::atomic<bool> releaseOp{false};
    std::atomic<bool> destroyedWhileInside{true};

    std::thread op([&] {
        manager.withLibusbBackendLifecycle([&](LibusbBackend* backend) {
            ASSERT_NE(backend, nullptr);
            inside.store(true);
            ASSERT_TRUE(wma_test::waitUntil([&] { return releaseOp.load(); }, 10000ms));
            destroyedWhileInside.store(destroyed.load());
            backend->stop();  // lo que hacen nativeStopUsbStreaming / nativeCloseUsbDevice
        });
    });

    ASSERT_TRUE(wma_test::waitUntil([&] { return inside.load(); }));
    std::thread destroyer([&] {
        if (GetParam() == 0) {
            manager.fallbackToOboe();
        } else {
            // En el host no puede crear un backend nuevo: destruye el viejo y devuelve false.
            (void)manager.initializeUsbBackend(-1, "/dev/null");
        }
    });

    wma_test::sleepFixed(50ms);  // ausencia: no puede destruir con la operación adentro
    EXPECT_FALSE(destroyed.load());

    releaseOp.store(true);
    op.join();
    destroyer.join();

    EXPECT_FALSE(destroyedWhileInside.load());
    EXPECT_TRUE(destroyed.load());
}

INSTANTIATE_TEST_SUITE_P(FallbackAndInitialize, LifecycleVersusDestroyer, ::testing::Values(0, 1),
                         [](const ::testing::TestParamInfo<int>& info) {
                             return info.param == 0 ? std::string("fallbackToOboe")
                                                    : std::string("initializeUsbBackend");
                         });

// ---------------------------------------------------------------------------------------
// D5 — una operación de ciclo de vida NO bloquea a los lectores (Main). Bug que atrapa: correr
// start()/stop() bajo mMutex, que congela el health-check de Main durante todo el arranque.
// ---------------------------------------------------------------------------------------
TEST(LibusbBackendLifetime, D5_ReadersAreNotBlockedByALifecycleOperation) {
    BackendManager manager;
    auto owned = std::make_unique<LibusbBackend>();
    BackendManagerTestAccess::adopt(manager, std::move(owned));

    std::atomic<bool> inside{false};
    std::atomic<bool> releaseOp{false};
    std::thread op([&] {
        manager.withLibusbBackendLifecycle([&](LibusbBackend*) {
            inside.store(true);
            ASSERT_TRUE(wma_test::waitUntil([&] { return releaseOp.load(); }, 10000ms));
        });
    });
    ASSERT_TRUE(wma_test::waitUntil([&] { return inside.load(); }));

    std::atomic<bool> readerDone{false};
    std::thread reader([&] {
        manager.withLibusbBackend([](LibusbBackend* backend) { (void)backend->isUsbDeviceReady(); });
        (void)manager.isRunning();
        readerDone.store(true);
    });

    EXPECT_TRUE(wma_test::waitUntil([&] { return readerDone.load(); }))
        << "un lector quedó esperando a una operación de ciclo de vida";

    releaseOp.store(true);
    op.join();
    reader.join();
}

// ---------------------------------------------------------------------------------------
// AC-042.2 — con start()/stop() sosteniendo el mutex del backend, los getters devuelven "sin
// dato", sin leer el objeto y sin bloquear. Bugs que atrapa: el getter sin lock (devuelve el
// dato de un transfer manager que start() está recreando) y el getter con lock() (cuelga a
// Main durante todo el arranque).
// ---------------------------------------------------------------------------------------
TEST(LibusbBackendLifetime, AC_042_2_GettersSayNoDataWhileStartHoldsTheBackend) {
    LibusbBackend backend;
    LibusbBackendTestAccess::makeStreamLike(backend);

    // Control positivo: sin nadie adentro, los getters SÍ ven el stream.
    {
        const auto snap = backend.getTransferStatsSnapshot();
        ASSERT_TRUE(snap.has_value());
        EXPECT_EQ(snap->packetsSubmitted, 5u);
        EXPECT_EQ(snap->packetsCompleted, 3u);
        EXPECT_FLOAT_EQ(backend.getOutputLatencyMs(), 7.5f);
        EXPECT_FLOAT_EQ(backend.getInputLatencyMs(), 3.25f);
        EXPECT_FLOAT_EQ(backend.getStreamInfo().outputLatencyMs, 7.5f);
        EXPECT_TRUE(backend.setProfilingEnabled(true));
        EXPECT_TRUE(LibusbBackendTestAccess::profilerEnabled(backend));
        EXPECT_TRUE(backend.resetProfilingStats());
    }

    LibusbBackendTestAccess::holdLikeStart(backend);

    struct Seen {
        bool snapshot = true;
        float out = 0, in = -1, streamOut = -1;
        bool profilerApplied = true, resetApplied = true;
        int eventLoop = 0;
        double profilingHealth = -1;
        uint64_t profilingTransfers = 1;
        int jitter = -1, floorMs = -1, bufferMs = -1;
        std::size_t inFlight = 1;
        float ceiling = -1;
    } seen;
    std::atomic<bool> done{false};
    std::thread reader([&] {
        seen.snapshot = backend.getTransferStatsSnapshot().has_value();
        seen.out = backend.getOutputLatencyMs();
        seen.in = backend.getInputLatencyMs();
        seen.streamOut = backend.getStreamInfo().outputLatencyMs;
        seen.profilerApplied = backend.setProfilingEnabled(false);
        seen.resetApplied = backend.resetProfilingStats();
        seen.eventLoop = backend.getEventLoopSchedResult();
        const auto profiling = backend.getProfilingStats();
        seen.profilingHealth = profiling.healthScore;
        seen.profilingTransfers = profiling.outputTransfers.totalTransfers;
        seen.jitter = backend.getJitterBudgetMs();
        seen.floorMs = backend.getConvergedFloorMs();
        seen.inFlight = backend.getOutputInFlightDepthPackets();
        seen.ceiling = backend.getDeclaredOutputLatencyCeilingMs();
        seen.bufferMs = backend.getCurrentBufferMs();
        done.store(true);
    });

    const bool returned = wma_test::waitUntil([&] { return done.load(); });
    LibusbBackendTestAccess::release(backend);  // si algún getter bloqueó, recién ahora vuelve
    reader.join();

    ASSERT_TRUE(returned) << "un getter bloqueó detrás del mutex de start()";
    EXPECT_FALSE(seen.snapshot) << "las stats se leyeron con start() adentro";
    EXPECT_FLOAT_EQ(seen.out, kNoStreamOutputLatencyMs);
    EXPECT_FLOAT_EQ(seen.in, 0.0f);
    EXPECT_FLOAT_EQ(seen.streamOut, 0.0f);
    EXPECT_FALSE(seen.profilerApplied);
    EXPECT_FALSE(seen.resetApplied);
    EXPECT_EQ(seen.eventLoop, -1);
    // "Sin dato" = lo mismo que sin stream (review de MINI-042, M3).
    const watermelon_audio::usb::UsbProfilingStats noStream{};
    EXPECT_DOUBLE_EQ(seen.profilingHealth, noStream.healthScore);
    EXPECT_EQ(seen.profilingTransfers, noStream.outputTransfers.totalTransfers);
    EXPECT_EQ(seen.jitter, 0);
    EXPECT_EQ(seen.floorMs, 0);
    EXPECT_EQ(seen.inFlight, 0u);
    EXPECT_FLOAT_EQ(seen.ceiling, 0.0f);
    EXPECT_EQ(seen.bufferMs, 100);
    EXPECT_TRUE(LibusbBackendTestAccess::profilerEnabled(backend))
        << "un pedido que dijo 'no aplicado' no puede haberse aplicado";
}

// ---------------------------------------------------------------------------------------
// AC-042.2 — estrés: lectores contra la recreación de start(). Bajo TSan, cualquier getter que
// lea mTransferManager sin el try_lock sale como carrera; bajo ASan, como uso de memoria liberada.
// ---------------------------------------------------------------------------------------
TEST(LibusbBackendLifetime, AC_042_2_GettersRaceStartRecreatingTheTransferManager) {
    LibusbBackend backend;
    LibusbBackendTestAccess::makeStreamLike(backend);

    constexpr int kRecreations = 300;
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};

    std::thread reader([&] {
        while (!stop.load()) {
            (void)backend.getTransferStatsSnapshot();
            (void)backend.getProfilingStats();
            (void)backend.getOutputLatencyMs();
            (void)backend.getInputLatencyMs();
            (void)backend.getStreamInfo();
            (void)backend.getEventLoopSchedResult();
            (void)backend.getJitterBudgetMs();
            (void)backend.getConvergedFloorMs();
            (void)backend.getOutputInFlightDepthPackets();
            (void)backend.getDeclaredOutputLatencyCeilingMs();
            (void)backend.getCurrentBufferMs();
            (void)backend.setProfilingEnabled(true);
            (void)backend.resetProfilingStats();
            reads.fetch_add(1);
        }
    });

    // Que el lector haya entrado antes de empezar, para que la ventana exista.
    ASSERT_TRUE(wma_test::waitUntil([&] { return reads.load() > 0; }));
    for (int i = 0; i < kRecreations; ++i) {
        LibusbBackendTestAccess::recreateTransferManagerLikeStart(backend);
    }
    stop.store(true);
    reader.join();

    EXPECT_GT(reads.load(), 0);
}

// ---------------------------------------------------------------------------------------
// I2 del review — el layout de los 21 floats de nativeGetUsbTransferStats. La JNIEXPORT
// sólo llama a esta función; en el host el arnés JNI no tiene stats que traducir.
// Bug que atrapa: dos índices cruzados ([0]/[1], [19]/[20]...), que el runner de REQ-050
// leería como pérdida de paquetes o como otro techo de latencia.
// ---------------------------------------------------------------------------------------
TEST(UsbTransferStatsArray, EveryIndexCarriesItsOwnField) {
    LibusbBackend::TransferStatsSnapshot snap;
    snap.packetsSubmitted = 101;
    snap.packetsCompleted = 102;
    snap.packetsErrors = 103;
    snap.underruns = 104;
    snap.overruns = 105;
    snap.currentLatencyMs = 10.0f;
    snap.avgLatencyMs = 107.0f;
    snap.ringBufferLevel = 109;
    snap.ringBufferFillPct = 110.0f;
    snap.currentSampleRateHz = 113.0f;
    snap.driftPpm = 114.0f;
    snap.feedbackEffectiveFramesPerPacket = 115.0f;
    snap.feedbackPacketsReceived = 116;
    snap.feedbackPacketsInvalid = 117;
    snap.activeClockSourceId = 118;
    snap.outputInFlightDepthPackets = 119;
    snap.declaredOutputLatencyCeilingMs = 120.0f;

    float out[watermelon_audio::kUsbTransferStatsArraySize];
    watermelon_audio::usbTransferStatsToArray(snap, out);

    const float expected[watermelon_audio::kUsbTransferStatsArraySize] = {
        101, 102, 103, 104, 105, 10.0f, 107, 8.0f, 15.0f, 109, 110, 3840.0f,
        102.0f * 192.0f, 113, 114, 115, 116, 117, 118, 119, 120,
    };
    for (int i = 0; i < watermelon_audio::kUsbTransferStatsArraySize; ++i) {
        EXPECT_FLOAT_EQ(out[i], expected[i]) << "indice " << i;
    }
}

TEST(UsbTransferStatsArray, NoDataIsAllZeros) {
    float out[watermelon_audio::kUsbTransferStatsArraySize];
    for (float& v : out) v = 42.0f;
    watermelon_audio::usbTransferStatsToArray(std::nullopt, out);
    for (int i = 0; i < watermelon_audio::kUsbTransferStatsArraySize; ++i) {
        EXPECT_FLOAT_EQ(out[i], 0.0f) << "indice " << i;
    }
}
