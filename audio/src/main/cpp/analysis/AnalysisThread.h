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
#include "IntonationMode.h"
#include "FastModeTracker.h"
#include "../dsp/McLeodPitch.h"

#include <atomic>
#include <mutex>
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
     * @brief El modo intonacion (S9). Lo maneja el THREAD DE CONTROL, no el lazo.
     *
     * Capturar es un acto del usuario ("ahora toca el armonico"), no algo que el
     * drenaje decida: por eso vive aca afuera y el lazo no lo toca. Y por eso
     * `captureIntonation()` lee el strobe bajo el mismo mutex con el que la C API
     * ya serializa lo demas.
     */
    bool captureIntonation(IntonationMode::Slot slot) noexcept {
        // 🔴 SE LEE EL SNAPSHOT PUBLICADO, NO `mStrobe`.
        //
        // `mStrobe` lo escribe el thread de analisis; esto corre en el de
        // control. La primera version preguntaba `mStrobe.converged()` y TSan
        // reporto la carrera en el primer gate. El `analysisMutex` de la C API no
        // la cubria: serializa a los llamadores de control entre si, y el thread
        // de analisis nunca lo toma.
        //
        // El snapshot es el seam que S1 construyo para exactamente esto, y ademas
        // da una garantia que leer los miembros sueltos no daria: los tres
        // valores salen del MISMO publish, asi que no se puede mezclar el estado
        // de un tick con los cents de otro.
        float values[kSnapshotValueCount];
        if (!mSnapshot.read(values)) return false;

        const bool converged =
            static_cast<int>(values[kSnapState]) == kStateConverged;
        return mIntonation.capture(slot, static_cast<double>(values[kSnapCents]),
                                   targetHz(), converged);
    }
    void resetIntonation() noexcept { mIntonation.reset(); }
    const IntonationMode& intonation() const noexcept { return mIntonation; }

    /**
     * @brief Las cuerdas del instrumento, EN ORDEN DE CUERDA (S5 · 5.12).
     *
     * Con candidatos puestos, el motor **elige el objetivo solo** desde la
     * deteccion gruesa de S4 — que es lo que faltaba para que el afinador
     * funcione sin que el consumidor empuje un objetivo a mano. Con la lista
     * vacia se vuelve al comportamiento anterior: manda `setTargetHz()`.
     *
     * Lo llama el thread de control. El lazo NO toma este mutex: levanta una
     * bandera atomica y copia una sola vez por tick.
     */
    void setCandidates(const double* hz, int count) noexcept {
        std::lock_guard<std::mutex> lock(mCandidateMutex);
        mPendingCount = 0;
        if (hz != nullptr) {
            for (int i = 0; i < count && i < FastModeTracker::kMaxCandidates; ++i) {
                if (hz[i] > 0.0) mPendingCandidates[mPendingCount++] = hz[i];
            }
        }
        mCandidatesDirty.store(true, std::memory_order_release);
    }

    /**
     * @brief La fuente de entrada cambio: TODO lo integrado deja de valer (S8).
     *
     * El modo de falla que esto evita es SILENCIOSO. Si el ring conserva frames
     * de la fuente vieja mientras el estimador sigue integrando, la lectura sale
     * de **mezclar dos señales**, con una fase que no significa nada — y no se ve
     * como un error, se ve como un numero.
     *
     * Lo llama el thread de control, y **no toca nada**: levanta una bandera y el
     * lazo hace el reinicio. Tocar el strobe desde aca es exactamente la carrera
     * que TSan encontro en S9.
     */
    void onSourceChanged() noexcept {
        mSourceChanged.store(true, std::memory_order_release);
    }

    /// Engancha a mano a una cuerda (el musico la elige). -1 suelta.
    void lockString(int index) noexcept {
        mPendingLock.store(index, std::memory_order_release);
    }

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

    /// S9. No lo toca `drainLoop`: lo maneja el thread de control.
    IntonationMode mIntonation;

    /// S5. Lo actualiza el lazo con la deteccion gruesa; los candidatos los pone
    /// el thread de control (protegidos por `mCandidateMutex`, que el lazo NO
    /// toma: copia una vez por tick a `mActiveCandidates`).
    FastModeTracker mFastMode;
    std::mutex mCandidateMutex;
    double mPendingCandidates[FastModeTracker::kMaxCandidates]{};
    int mPendingCount{0};
    std::atomic<bool> mCandidatesDirty{false};
    std::atomic<int> mPendingLock{-2};   // -2 = nada pedido

    /// S8. La pone el thread de control; la consume el lazo.
    std::atomic<bool> mSourceChanged{false};

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
