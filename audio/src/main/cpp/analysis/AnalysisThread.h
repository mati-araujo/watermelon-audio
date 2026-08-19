#pragma once

/**
 * @file AnalysisThread.h
 * @brief El thread que drena el ring de captura y publica el snapshot (REQ-001 S1).
 *
 * POR QUE VIVE EN `analysis/` Y NO EN `core/`
 * -------------------------------------------
 * La spec de la etapa dejaba la decision abierta. Va aca porque **el motor no
 * deberia saber que existe un afinador**: la dependencia va en una sola
 * direccion, de `core/` hacia `analysis/`, y asi el afinador se puede sacar,
 * reemplazar o testear sin tocar el motor.
 *
 * POR QUE UN THREAD PROPIO Y NO EL DE AUDIO
 * -----------------------------------------
 * El estimador de S2 integra fase a lo largo de segundos y hace regresion: nada
 * de eso entra en un deadline de 2,7 ms, y meterlo ahi seria exactamente el
 * error que el programa WD paso meses sacando del callback. El thread de
 * captura solo escribe al ring —lock-free, sin asignar— y sigue.
 *
 * ESTE THREAD NO PUEDE BLOQUEAR AL DE CAPTURA, Y NO TIENE COMO
 * -----------------------------------------------------------
 * Lo unico que comparte con el es el `AnalysisRing`, donde el escritor jamas
 * espera a nadie (pisa lo viejo y sigue), y el `AnalysisSnapshot`, que el thread
 * de captura ni toca. No hay un solo lock entre los dos.
 */

#include "AnalysisRing.h"
#include "AnalysisSnapshot.h"

#include <atomic>
#include <thread>
#include <vector>

namespace wma::analysis {

class AnalysisThread {
public:
    /// Frames que intenta drenar por vuelta. Un cuarto del ring: deja margen
    /// para que el escritor no lo alcance mientras copia.
    static constexpr int kDrainFrames = AnalysisRing::kCapacityFrames / 4;

    AnalysisThread(AnalysisRing& ring, AnalysisSnapshot& snapshot)
        : mRing(ring), mSnapshot(snapshot), mScratch(kDrainFrames, 0.0f) {}

    ~AnalysisThread() { stop(); }

    AnalysisThread(const AnalysisThread&) = delete;
    AnalysisThread& operator=(const AnalysisThread&) = delete;

    /// Arranca. Idempotente: llamarlo dos veces no crea dos threads.
    void start(int captureSampleRate);

    /// Para y JUNTA el thread. Idempotente, y llamable desde el destructor.
    /// No es RT — la llama el thread de control.
    void stop();

    bool isRunning() const noexcept {
        return mRunning.load(std::memory_order_acquire);
    }

    /// Vueltas completas del lazo. Lo lee el test para saber que arranco de
    /// verdad, en vez de dormir un rato y suponer.
    uint64_t ticks() const noexcept { return mTicks.load(std::memory_order_relaxed); }

private:
    /**
     * El lazo. Se llama `drainLoop` y NO `run`, y el nombre es load-bearing:
     * `check-rt-safety.py` sigue solo las llamadas que resuelven a UNA
     * definicion, asi que un segundo `::run` en el arbol vuelve AMBIGUA la
     * llamada a `TrackStorage::run` y el walker deja de seguirla. Medido: con
     * este metodo llamado `run`, el grafo RT perdio DOS funciones del looper
     * —`TrackStorage::run` y `ChunkedAudioBuffer::contiguousRun`, que cuelga de
     * ella— y el lint siguio en verde. Cobertura perdida en silencio.
     *
     * Es el mismo mecanismo que hizo que `SpectrumAnalyzer` cegara a
     * `VocoderBank::analyze` durante meses, sacado a la luz al borrarlo en la
     * tanda anterior de esta misma etapa. Ahi lo causaba codigo muerto; aca lo
     * habria causado codigo nuevo.
     *
     * Desde entonces eso ya no queda en silencio: `scripts/rt-coverage-baseline.txt`
     * declara que funciones alcanza el walker y el lint falla si el conjunto
     * cambia. Renombrar esto a `run` lo pone rojo — verificado con este mismo
     * archivo. El nombre sigue siendo load-bearing igual: el trinquete avisa,
     * no arregla.
     */
    void drainLoop(int captureSampleRate);

    AnalysisRing& mRing;
    AnalysisSnapshot& mSnapshot;
    std::vector<float> mScratch;

    std::thread mThread;
    std::atomic<bool> mRunning{false};
    std::atomic<uint64_t> mTicks{0};
    uint64_t mFramesAnalyzed{0};
};

}  // namespace wma::analysis
