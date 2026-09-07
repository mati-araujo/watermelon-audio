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
#include "AbsenceGate.h"
#include "../dsp/McLeodPitch.h"
#include "../platform/RtCounter.h"

#include <atomic>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace wma::analysis {

/**
 * @brief La sonda de SOPORTE ESPECTRAL (REQ-031 S1): ¿la altura detectada está en la señal?
 *
 * QUÉ PREGUNTA CONTESTA, Y POR QUÉ NO LA CONTESTA NADIE MÁS
 * ---------------------------------------------------------
 * El detector grueso encuentra un PERÍODO; el strobe integra fase contra el OBJETIVO. Ninguno de
 * los dos pregunta si la altura publicada tiene energía en la señal — y no pueden: el NSDF de un
 * submúltiplo es alto **por construcción** (un múltiplo del período también es un período), y el
 * strobe sigue los parciales del objetivo, que sin candidatos lo pone el consumidor y no coincide
 * con `detectedHz`. Por eso es una sonda propia, sobre la señal cruda.
 *
 * CÓMO MIDE
 * ---------
 * Goertzel en los 8 primeros armónicos de `detectedHz` sobre los últimos `kWindowFrames` frames
 * mono, con ventana de Hann. La respuesta es `max(X(f), X(2f))` relativo al más fuerte de los
 * ocho, en dB: en una cuerda real —aunque le falte el fundamental— el fundamental o su octava es
 * el pico; en un submúltiplo inventado no hay energía en ninguno de los dos.
 *
 * 🔴 LA VENTANA DE HANN NO ES ADORNO, Y ESTÁ MEDIDO (2026-09-07, tarea 1.1). Los números que
 * fijaron el umbral salían de un Goertzel sobre 0,5 s; el lazo drena 2048 frames (46 ms a 44,1
 * kHz). Sobre esos 2048 frames SIN ventana las poblaciones SE TOCAN: el falso convergido sube
 * hasta −15,2 dB y cruza el umbral de −25, porque la fuga de la ventana rectangular (H3 de la E4
 * está a ~40 bins) se mete en el bin del fundamental inventado. Y acumular a través de ticks
 * —el remedio que la spec anticipaba— NO lo arregla, porque la fuga no es varianza: con 8192
 * frames rectangulares el falso queda en −26,2, pegado al umbral. Con Hann sobre los MISMOS 2048
 * frames el peor aceptado es −1,8 y el mejor rechazado −61,3: 59,5 dB de separación, y el
 * barrido bloque a bloque no oscila (±0,7 dB). El umbral no se movió.
 *
 * El ring es propio y NO el bloque drenado: en vivo `drainOnce()` lee lo que haya en el ring, que
 * son unos milisegundos por vuelta, y un Goertzel sobre 200 frames a 110 Hz no mide nada. Se
 * evalúa cuando el detector produce un veredicto NUEVO, sobre los frames que lo terminaron.
 *
 * Lo toca SOLO el thread de análisis. No es RT y no necesita atómicos.
 */
class SpectralSupportProbe {
public:
    /// La ventana que decidió 1.1: el mismo largo que drena el lazo. En frames de captura,
    /// así que a 48 kHz son 43 ms y a 16 kHz 128 ms; el detector tiene la misma dependencia.
    static constexpr int kWindowFrames = AnalysisRing::kCapacityFrames / 4;
    /// Cuántos armónicos entran en la referencia. Es lo que se midió; más no separa mejor
    /// (el falso ya queda 60 dB abajo) y menos deja al pico fuera cuando H3 domina.
    static constexpr int kHarmonics = 8;

    SpectralSupportProbe();

    /// Copia `numFrames` frames mono al ring propio. Bloques de cualquier tamaño.
    void pushMono(const float* mono, int numFrames) noexcept;

    /// true cuando el ring ya dio una vuelta entera: antes no hay ventana que evaluar.
    bool isPrimed() const noexcept { return mFilled >= kWindowFrames; }

    /**
     * `max(X(hz), X(2·hz))` relativo al armónico más fuerte, en dB (≤ 0). Sólo tiene sentido
     * con `isPrimed()`. Devuelve −∞ si no hay energía en ningún armónico: sin energía, no hay
     * soporte.
     */
    double supportDb(int sampleRate, double hz) const noexcept;

private:
    /// Magnitud de Goertzel en `hz` sobre la ventana, ya con Hann.
    double magnitudeAt(int sampleRate, double hz) const noexcept;

    std::vector<float> mRing;
    std::vector<float> mHann;
    int mWrite{0};
    int mFilled{0};
};

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
     * @brief Cuantas veces se APLICO el objetivo, separado por quien lo pidio (REQ-030 S1).
     *
     * POR QUE HACE FALTA UN CONTADOR Y NO ALCANZA MIRAR EL DESENLACE
     * -------------------------------------------------------------
     * El objetivo tiene dos escritores —el consumidor por `setTargetHz()` y el modo rapido
     * cuando reengancha— y lo unico que un test podia ver hasta ahora era el estado final.
     * Eso NO alcanza: una lectura que no converge y una que converge tarde se parecen, y una
     * que converge por otra razon pasa igual. Lo que decide es **cuantas veces se re-aplico**,
     * porque cada re-aplicacion descarta el ring y le corta la integracion al estimador.
     *
     * 🔴 Y no habia con que verlo: `AnalysisRing::skipToNewest()` declara explicitamente que
     * **no cuenta como frames perdidos** —es una decision del lector, no un atraso—, asi que
     * `droppedFrames()` no lo delata, y el snapshot no lleva el dato.
     *
     * Un objetivo estable deja los dos quietos. Que sigan subiendo tick a tick es el defecto
     * de REQ-030: medido, 27 y 26 en 5 s contra 1 y 0 de un objetivo correcto de entrada.
     */
    uint64_t targetAppliedByUser() const noexcept { return mTargetAppliedByUser.get(); }
    uint64_t targetAppliedByFastMode() const noexcept { return mTargetAppliedByFastMode.get(); }

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

    /**
     * Por debajo de esto, la altura detectada NO tiene soporte espectral (REQ-031).
     *
     * Es el centro de la ventana que la spec midió sobre 0,5 s (peor aceptado −12,0, mejor
     * rechazado −37,5): no privilegia ninguno de los dos errores. Y quedó ADENTRO de la ventana
     * re-medida sobre la sonda real —2048 frames con Hann: −1,8 / −61,3—, con 23 dB de margen
     * hacia lo que se acepta y 36 hacia lo que se rechaza. Ver `SpectralSupportProbe`.
     *
     * 🔴 Si algún día una cuerda legítima cae por debajo, la salida NO es mover esto: es medir
     * qué tiene esa cuerda que la sonda no ve. Bajar el umbral para salvar un caso compra el
     * falso positivo de vuelta, y ése es el lado que este REQ existe para cerrar.
     */
    static constexpr double kSpectralSupportFloorDb = -25.0;

    /// Que paso en una vuelta de `drainOnce()`. `kRingEmpty` es la unica que el
    /// llamador tiene que tratar distinto: el thread duerme, el puerto termina.
    enum class DrainOutcome { kPublished, kSkipped, kRingEmpty };

    /**
     * @brief UNA vuelta del analisis, sin nada del thread adentro (REQ-015 S1).
     *
     * Existe para que el puerto de analisis offline empuje el MISMO analisis en
     * vez de reimplementarlo. Dos definiciones del analisis serian dos motores,
     * y el verde de uno no diria nada del otro — que es exactamente lo que
     * AC-015.3 existe para impedir.
     *
     * 🔴 UN SOLO CONDUCTOR A LA VEZ. O se arranca el thread con `start()`, o se
     * llama a esto desde afuera: **nunca las dos cosas**. Todo el estado que
     * toca (`mStrobe`, `mDetector`, `mFastMode`, los contadores) es no-atomico
     * a proposito porque hoy lo toca UN solo thread; dos conductores lo
     * convierten en una carrera. `isRunning()` dice si el thread ya es el
     * conductor.
     */
    DrainOutcome drainOnce();

private:
    /**
     * Cuantas veces la entrada perdio continuidad. Lo escribe y lo lee SOLO el
     * thread de analisis, asi que no necesita ser atomico: cruza la frontera
     * por el snapshot, que es el seam que S1 construyo justo para esto.
     */
    uint64_t mDiscontinuityCount{0};

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

    /// REQ-019 — la compuerta de ausencia, con su memoria. Vive aca y no como dos
    /// lineas sueltas en el lazo porque su defecto era invisible ahi: le creia a
    /// UNA lectura sin altura y apagaba la aguja sobre una cuerda audible.
    AbsenceGate mAbsence;

    /// Si la ultima pasada del detector produjo un veredicto NUEVO. Su ventana es
    /// NO SOLAPADA y mas larga que un bloque, asi que sin esto la compuerta contaria
    /// la misma evidencia varias veces (REQ-019.2).
    bool mFreshPitchVerdict = false;

    /// REQ-031 S1 — la sonda de soporte espectral. Se alimenta por tick y se evalua
    /// por veredicto del detector, sobre los frames que lo terminaron.
    SpectralSupportProbe mSupportProbe;

    /**
     * El soporte de la ULTIMA altura detectada, en dB (ver `SpectralSupportProbe`), o NaN
     * si el ultimo veredicto no trajo altura. Se actualiza SOLO con un veredicto nuevo y se
     * publica en cada tick: entre veredictos la altura no cambia, asi que su soporte tampoco.
     * Es lo que hace que la bandera no pueda parpadear mas rapido que el propio detector.
     */
    double mSupportDb{std::numeric_limits<double>::quiet_NaN()};
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

    /**
     * El ultimo objetivo que pidio EL CONSUMIDOR, que no es lo mismo que el aplicado.
     *
     * 🔴 Existe porque `mAppliedTarget` tiene DOS escritores —`setTargetHz()` y el modo
     * rapido cuando reengancha— y la rama que re-aplica preguntaba *"¿lo aplicado difiere de
     * lo pedido?"*. Esa pregunta y *"¿cambio lo que pide el consumidor?"* son la misma
     * mientras hay un solo escritor, y dejan de serlo en cuanto aparece el segundo: apenas el
     * modo rapido reengancha, difieren PARA SIEMPRE y las dos ramas se pisan una vez por
     * tick, descartando el ring cada vez (REQ-030).
     *
     * Arranca en 0 igual que `mTargetHz`, para que "todavia no pidieron nada" no dispare una
     * re-aplicacion en el primer tick. Los centinelas de re-aplicacion lo bajan a -1, que no
     * es un objetivo posible.
     */
    double mLastUserTarget{0.0};

    // REQ-030 S1 — miembros y no globales: un contador global de proceso hace que dos
    // instancias se pisen, que es la leccion WD-1.5 que `RtCounter.h` documenta. Cada test
    // construye su propio `AnalysisThread`, asi que arrancan en cero solos.
    wma::RtCounter mTargetAppliedByUser;
    wma::RtCounter mTargetAppliedByFastMode;

    std::thread mThread;
    std::atomic<bool> mRunning{false};
    std::atomic<uint64_t> mTicks{0};
    uint64_t mFramesAnalyzed{0};

    /**
     * REQ-009 S2. `mRing.droppedFrames()` tal como estaba en la vuelta anterior,
     * para poder preguntar el **Δ** en vez del acumulado.
     *
     * Lo toca SOLO `drainLoop()`, asi que no necesita ser atomico. Y es
     * `uint64_t` como el contador: restar dos snapshots de un contador que solo
     * sube no puede desbordar por abajo.
     */
    uint64_t mLastDroppedFrames{0};

    /**
     * REQ-009 S3. Lo mismo para el eje de CAPTURA: el acumulado que el backend
     * estampó en el ring, tal como estaba en la vuelta anterior.
     *
     * Va aparte de `mLastDroppedFrames` y no sumado, porque los dos contadores
     * los mueve gente distinta en momentos distintos — juntarlos haría imposible
     * decir cuál de los dos ejes disparó, que es lo primero que se pregunta al
     * depurar esto.
     */
    uint64_t mLastCaptureSeam{0};
};

}  // namespace wma::analysis
