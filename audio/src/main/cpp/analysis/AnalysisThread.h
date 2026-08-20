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
#include "PhaseSlopeEstimator.h"
#include "StrobeTracker.h"
#include "InharmonicityEstimator.h"
#include "../dsp/McLeodPitch.h"

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
    ///
    /// `captureSampleRate` es una SEMILLA para el caso en que todavia no haya
    /// entrado un bloque: el rate que se publica sale del estampado del
    /// escritor (`AnalysisRing::setCaptureRate`), que viaja con las muestras y
    /// por eso sigue los cambios de configuracion en caliente.
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

    /**
     * @brief Las cuatro fases del strobe, para que S7 lea la inarmonicidad sin
     *        volver a analizar la señal (tarea 6.12).
     *
     * Lo consume el MISMO thread de analisis, que es quien lo escribe: no cruza
     * la frontera y por eso no necesita atomicos. Un consumidor de otro thread
     * tiene que ir por el snapshot.
     */
    const StrobeTracker& strobe() const noexcept { return mStrobe; }

    /// La inarmonicidad estimada de la cuerda que suena (S7).
    const InharmonicityEstimator& inharmonicity() const noexcept { return mInharmonicity; }

    /**
     * @brief La frecuencia contra la que se mide. 0 = ninguna.
     *
     * EL OBJETIVO LO PONE EL CONSUMIDOR, Y NO ES PROVISORIO
     * -----------------------------------------------------
     * El estimador de fase **afina alrededor de un objetivo, no lo busca**: su rango de
     * captura es de unos pocos cents en la zona aguda. Asi que alguien tiene que decirle
     * contra que medir, y hasta que exista la deteccion gruesa ese alguien es el consumidor
     * —que es exactamente lo que `ITuner` declara como obligacion del implementador.
     *
     * **Sin objetivo NO se inventa uno.** El snapshot sigue publicando NaN en cents y el
     * estado queda en "sin enganche": es honesto, y es distinto de publicar la altura de
     * cualquier cosa que este sonando.
     *
     * La llama el thread de control. Cambiarla **reinicia la integracion**: la fase acumulada
     * contra el objetivo viejo no dice nada del nuevo.
     */
    void setTargetHz(double hz) noexcept {
        mTargetHz.store(hz > 0.0 ? hz : 0.0, std::memory_order_release);
    }

    double targetHz() const noexcept { return mTargetHz.load(std::memory_order_acquire); }

    /**
     * Incertidumbre por debajo de la cual la lectura se declara **convergida**, en cents.
     *
     * 0,1 es el presupuesto del producto: por debajo de eso, la medicion ya no es lo que
     * limita. El numero esta acá y no disperso porque S6 lo va a mirar y S10 lo va a escribir
     * en el contrato de exactitud.
     */
    static constexpr double kConvergedUncertaintyCents = 0.1;

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
    void drainLoop();

    AnalysisRing& mRing;
    AnalysisSnapshot& mSnapshot;
    std::vector<float> mScratch;

    /// El tracker vive ACA y no en el thread de audio: integra fase a lo largo de segundos
    /// y hace regresion, nada de lo cual entra en un deadline de 2,7 ms.
    ///
    /// Desde S6 es el STROBE —fundamental + 3 armonicos, combinados por 1/σ²— y ya no un
    /// `PhaseSlopeEstimator` suelto. La lectura combinada no puede ser peor que la del
    /// fundamental solo (es la combinacion de minima varianza), asi que el cambio no puede
    /// empeorar lo que S4 publicaba: medido sobre 14 cuerdas, es estrictamente mejor.
    StrobeTracker mStrobe;

    /// Lee las 4 fases del strobe; no vuelve a analizar la señal (S7 · 7.9).
    InharmonicityEstimator mInharmonicity;

    /// Deteccion gruesa: encuentra la altura SIN objetivo. Corre en el mismo thread y no
    /// depende del estimador — de hecho es al reves: es quien puede darle un objetivo.
    wma::dsp::McLeodPitch mDetector;
    std::atomic<double> mTargetHz{0.0};
    /// Lo ultimo con lo que se configuro el estimador, para no re-prepararlo por tick:
    /// `prepare()` asigna y `setTarget()` reinicia la integracion.
    int mPreparedRate{0};
    double mAppliedTarget{0.0};

    std::thread mThread;
    std::atomic<bool> mRunning{false};
    std::atomic<uint64_t> mTicks{0};
    uint64_t mFramesAnalyzed{0};
};

}  // namespace wma::analysis
