#include "AnalysisThread.h"

#include <chrono>
#include <cmath>
#include <limits>

namespace wma::analysis {

namespace {
/// Piso por debajo del cual se reporta "sin senal". Lineal, ~-60 dBFS.
constexpr float kSilenceFloor = 0.001f;
/// Cada cuanto vuelve a mirar el ring cuando no habia nada. 5 ms es holgado
/// contra los ~170 ms que el ring aguanta antes de pisar.
constexpr auto kIdleNap = std::chrono::milliseconds(5);
}  // namespace

void AnalysisThread::start(int captureSampleRate) {
    if (mRunning.exchange(true, std::memory_order_acq_rel)) {
        return;   // ya estaba corriendo
    }
    // El rate que llega aca es la SEMILLA: describe lo que se sabia al
    // arrancar. La fuente viva es el estampado del escritor, que viaja con las
    // muestras (ver AnalysisRing::setCaptureRate). Sembrar el ring en vez de
    // guardar una copia propia deja UNA sola fuente de verdad.
    if (captureSampleRate > 0 && mRing.captureRate() <= 0) {
        mRing.setCaptureRate(captureSampleRate);
    }
    mThread = std::thread([this] { drainLoop(); });
}

void AnalysisThread::stop() {
    if (!mRunning.exchange(false, std::memory_order_acq_rel)) {
        if (mThread.joinable()) mThread.join();
        return;
    }
    if (mThread.joinable()) mThread.join();
}

void AnalysisThread::drainLoop() {
    const float nan = std::numeric_limits<float>::quiet_NaN();

    while (mRunning.load(std::memory_order_acquire)) {
        // --- la configuracion se mira ANTES de drenar ------------------------
        //
        // El orden no es cosmetico: al cambiar el objetivo hay que descartar lo
        // que quedo en el ring, y eso sólo sirve si se hace antes de leerlo.
        const int rate = mRing.captureRate();

        // --- candidatos y enganche a mano, del thread de control -------------
        //
        // El lazo NO toma `mCandidateMutex` en el caso normal: mira una bandera
        // atomica y solo entra al lock cuando de verdad cambiaron, que es una vez
        // por instrumento y no una vez por tick.
        if (mCandidatesDirty.exchange(false, std::memory_order_acq_rel)) {
            std::lock_guard<std::mutex> lock(mCandidateMutex);
            mFastMode.setCandidates(mPendingCandidates, mPendingCount);
        }
        const int wantLock = mPendingLock.exchange(-2, std::memory_order_acq_rel);
        if (wantLock != -2) mFastMode.lockTo(wantLock);

        // --- cambio de fuente: nada de lo integrado sobrevive (S8) -----------
        //
        // Se descarta el ring ANTES de leerlo, porque lo que quedo adentro es de
        // la fuente vieja. Y se reinicia todo lo que integra: un strobe que
        // siguiera acumulando fase entre dos señales distintas publicaria un
        // numero perfectamente formado que no mide nada.
        if (mSourceChanged.exchange(false, std::memory_order_acq_rel)) {
            mRing.skipToNewest();
            mStrobe.reset();
            mDetector.reset();
            mFastMode.reset();
            mInharmonicity.reset();
            mIntonation.reset();
            // `mAppliedTarget` vuelve a 0 para que el objetivo se re-aplique: si
            // no, el strobe recien reseteado se quedaria sin objetivo y el modo
            // no volveria a medir nunca.
            mAppliedTarget = 0.0;
        }

        double target = mTargetHz.load(std::memory_order_acquire);

        if (rate > 0 && rate != mPreparedRate) {
            // EL RATE MEDIDO, NO 48000. Preparar el estimador con un rate
            // asumido escala todo lo que mida: a 32 kHz son +702 cents. Es el
            // mismo defecto que las tareas 1.16-1.19 sacaron del camino, y este
            // es el ultimo lugar donde se podia volver a perder — justo al
            // usarlo.
            mStrobe.prepare(rate);
            mDetector.prepare(rate);
            mPreparedRate = rate;
            mAppliedTarget = 0.0;      // `prepare()` reinicia: hay que re-aplicar
        }
        if (target != mAppliedTarget && mPreparedRate > 0) {
            mStrobe.setTarget(target);
            mAppliedTarget = target;
            // Lo que quedo en el ring es de la cuerda ANTERIOR. Ver
            // AnalysisRing::skipToNewest().
            mRing.skipToNewest();
        }
        const bool measuring = mPreparedRate > 0 && mAppliedTarget > 0.0;

        const int got = mRing.read(mScratch.data(), kDrainFrames);
        mTicks.fetch_add(1, std::memory_order_relaxed);

        // --- REQ-009 S2 · LA GUARDA: el Δ de frames pisados, no el acumulado ---
        //
        // Si el ring desbordo, lo que acabo de leer NO es contiguo con lo que le
        // di al estimador la vuelta pasada: entre los dos trozos falta un pedazo
        // de señal que nadie vio. Integrar fase a traves de ese hueco da una
        // pendiente que mezcla dos mediciones — y sale PLAUSIBLE, con σ chica,
        // que es el hallazgo entero de REQ-009: medido, el motor publicaba
        // CONVERGIDO con la lectura a 1,04 cents del valor real (10x el
        // presupuesto) y σ en 0,024, muy por debajo del umbral de 0,1.
        //
        // 🔴 EL Δ, Y NO `droppedFrames() > 0`. El contador es acumulado y
        // monotono: con el acumulado, el primer desborde apagaria CONVERGIDO
        // para el resto de la sesion — el fallo que AC-009.2 prohibe
        // explicitamente. El Δ dice "entre la vuelta pasada y esta se perdio
        // señal", que es la pregunta que corresponde.
        //
        // 🔴 SE MUESTREA DESPUES DE `read()`, Y NO ES UN DETALLE DE ORDEN: ES LA
        // MITAD QUE HACE FUNCIONAR LA GUARDA. Los DOS `mDropped.bump()` de
        // `AnalysisRing` estan adentro de `read()` — el escritor no toca ese
        // contador NUNCA (pisa lo viejo y sigue; es su contrato). O sea que el
        // desborde lo descubre y lo cuenta el LECTOR, en la misma llamada que
        // devuelve el bloque que viene despues del hueco. Muestrear antes de
        // `read()` no es "un tick de corrimiento": es preguntar por un dato que
        // todavia no existe, y garantiza alimentar el salto y desmentirlo recien
        // en la vuelta siguiente — cuando la lectura equivocada ya se publico.
        //
        // Medido, 20 corridas de cada variante, peor error entre las lecturas
        // que el motor declaro CONVERGIDAS mientras el ring desbordaba:
        //     muestreo DESPUES (esto)  ->  3,8e-6 cents
        //     muestreo ANTES           ->  0,1875 cents  (4x el presupuesto)
        // Lo cubre `ABurstOverrunIsNeverPublishedAsConverged`, que fuerza el
        // caso en vez de esperar a que aparezca: la variante de "antes" cruzaba
        // el presupuesto en 1 de 20 corridas, o sea que un test que solo mire
        // desbordes sostenidos la deja pasar 19 veces de 20.
        const uint64_t dropped = mRing.droppedFrames();
        const uint64_t droppedDelta = dropped - mLastDroppedFrames;
        mLastDroppedFrames = dropped;
        if (droppedDelta > 0) {
            // El thread DETECTA; el estimador se HACE CARGO. Este lado es el
            // unico que ve el ring; el otro es el unico que sabe cuando su
            // integracion vuelve a ser confiable, porque es el que tiene la
            // ventana.
            //
            // El bloque de esta vuelta SI se alimenta, y eso no es descuido: el
            // ring nunca entrega una copia desgarrada —la re-chequea, la cuenta
            // en `mTorn` y devuelve 0—, asi que lo que llega aca es contiguo por
            // adentro. Lo unico roto era su union con el bloque anterior, y de
            // eso se encarga el reinicio. (Se probo tambien descartarlo: mueve
            // el peor error de 9,5e-6 a 3,8e-6 cents, o sea nada frente a un
            // presupuesto de 0,1, y ningun test lo puede matar. Codigo que no se
            // puede verificar y no cambia el resultado no se queda.)
            mStrobe.noteInputDiscontinuity();
        }

        if (got <= 0) {
            std::this_thread::sleep_for(kIdleNap);
            continue;
        }

        double sumSq = 0.0;
        for (int i = 0; i < got; ++i) {
            const double v = mScratch[static_cast<size_t>(i)];
            sumSq += v * v;
        }
        const float rms = static_cast<float>(std::sqrt(sumSq / got));
        mFramesAnalyzed += static_cast<uint64_t>(got);

        // Se lee POR TICK, no una vez: es lo unico que hace que un cambio de
        // rate en caliente aparezca en el snapshot siguiente.
        // `prepare()` asigna y `setTarget()` reinicia la integracion, asi que
        // llamarlos por tick tiraria la medicion antes de que converja: por eso
        // arriba se comparan contra lo ultimo aplicado.
        // 🔴 LA GRUESA VA PRIMERO, Y EL ORDEN ES PARTE DEL ARREGLO (REQ-003).
        //
        // El strobe necesita un control INDEPENDIENTE DE LA FASE para saber que
        // parciales estan dentro de su dominio de captura (AC-003.7), y ese
        // control es esta deteccion. Corriendola despues, el strobe combinaria
        // con el control del tick ANTERIOR — que es justo lo que no sirve
        // cuando el objetivo acaba de cambiar.
        //
        // No agrega analisis: la gruesa ya corria siempre. Agrega una
        // comparacion, que es lo que el no-funcional de la spec permite.
        if (mPreparedRate > 0) {
            mDetector.process(mScratch.data(), got);
        }
        if (measuring) {
            mStrobe.setCoarseFrequencyHz(
                mDetector.hasPitch() ? mDetector.frequencyHz() : 0.0);
            mStrobe.process(mScratch.data(), got);
        }

        // --- el modo rapido elige el objetivo, si hay candidatos --------------
        //
        // Aca se cierra el hueco que quedaba desde S4: el motor publicaba que
        // nota suena, pero convertir eso en "la cuerda que el musico quiso" y
        // empujarla como objetivo lo tenia que hacer el consumidor. Con la lista
        // de cuerdas puesta, lo hace el motor.
        if (mPreparedRate > 0 && mFastMode.candidateCount() > 0) {
            mFastMode.update(mDetector.hasPitch() ? mDetector.frequencyHz() : 0.0,
                             mDetector.clarity());
            const double picked = mFastMode.lockedTargetHz();
            if (picked > 0.0 && picked != mAppliedTarget) {
                mStrobe.setTarget(picked);
                mAppliedTarget = picked;
                mRing.skipToNewest();
            }
        }

        float values[kSnapshotValueCount];
        values[kSnapCaptureSampleRate] = static_cast<float>(rate);
        values[kSnapLevelRms]          = rms;
        values[kSnapFramesAnalyzed]    = static_cast<float>(mFramesAnalyzed);
        // 🔴 SE RELEE, NO SE REUSA `dropped`. Publicar la muestra que juzgo la
        // guarda parecia mas coherente y **oculta justamente el tick que
        // importa**: en la variante rota —muestrear antes de `read()`— la
        // muestra vale 0 en la vuelta que se come el hueco, asi que el snapshot
        // negaria el desborde que el ring acababa de contar. Un test que espere
        // "hasta que el contador suba" se saltearia esa vuelta y veria la
        // siguiente, ya recuperada. Medido: con la muestra reusada, el mutante
        // del orden sobrevivia; releyendo, muere.
        values[kSnapDroppedFrames]     = static_cast<float>(mRing.droppedFrames());

        // REQ-003 AC-003.8 — sin control no se publica lectura fina.
        //
        // `domainVerified()` es false cuando la deteccion gruesa no tiene nota:
        // ahi NO se puede saber si los parciales estan en su dominio, y publicar
        // sin verificar es exactamente por donde reentra el defecto que este REQ
        // cierra. Un afinador que dice "no se" es utilizable; uno que publica
        // +25,7 cuando la cuerda esta 100 cents abajo, no.
        //
        // El costo esta acotado y es el correcto: la gruesa mide 0,21 cents peor
        // caso sobre A0-C7, asi que "no tiene nota" significa que tampoco hay
        // señal utilizable para el strobe.
        const bool haveReading = measuring && mStrobe.hasSignal() &&
                                 mStrobe.hasMeasurement() && mStrobe.domainVerified();

        if (haveReading) {
            values[kSnapCents]       = static_cast<float>(mStrobe.cents());
            values[kSnapPhaseAngle]  = static_cast<float>(mStrobe.phaseAngle());
            values[kSnapUncertainty] = static_cast<float>(mStrobe.uncertaintyCents());
        } else {
            // NaN, no cero. `0.0` cents es un valor PLAUSIBLE —afinado exacto— y
            // un consumidor lo mostraria como medicion. Sin objetivo, o antes de
            // que la integracion tenga de donde sacar una pendiente, la ausencia
            // tiene que ser inconfundible.
            values[kSnapCents]       = nan;
            values[kSnapPhaseAngle]  = nan;
            values[kSnapUncertainty] = nan;
        }

        // El estado dice EN QUE PUNTO esta la medicion, y los cuatro casos son
        // distintos para el usuario: "sin señal" pide revisar el cable, "sin
        // enganche" pide elegir una cuerda o tocar mas limpio, "midiendo" es un
        // spinner y no un error.
        int state;
        if (rms < kSilenceFloor) {
            state = kStateNoSignal;
        } else if (!measuring) {
            state = kStateNoLock;          // hay señal, pero nadie dijo contra que medir
        } else if (!haveReading) {
            state = kStateMeasuring;       // integrando, todavia sin pendiente
        } else {
            state = mStrobe.uncertaintyCents() <= kConvergedUncertaintyCents
                        ? kStateConverged
                        : kStateMeasuring;
        }
        values[kSnapState] = static_cast<float>(state);

        // REQ-003 S2 (AC-003.4) — hasta donde vale la lectura fina, EN CENTS.
        //
        // Sale del propio estimador (`usableRangeCents()` -> el dominio del
        // parcial 1) y no de una constante replicada acá: si S1 mueve la guarda,
        // esto la sigue sola. Que las dos etapas no puedan divergir es
        // exactamente lo que verifica
        // `ThePublishedRangePredictsWhereTheFineReadingExists`.
        //
        // NaN sin objetivo, no 0: un rango de cero es plausible —"nunca confíes"—
        // y dice algo distinto de "no hay contra qué medir".
        values[kSnapUsableRangeCents] =
            measuring ? static_cast<float>(mStrobe.usableRangeCents()) : nan;

        values[kSnapDetectedHz] = mDetector.hasPitch()
                                      ? static_cast<float>(mDetector.frequencyHz())
                                      : 0.0f;
        values[kSnapDetectionClarity] = static_cast<float>(mDetector.clarity());

        // La inarmonicidad se lee de lo que el strobe YA calculo: cuatro fases
        // que discrepan entre si son, literalmente, la rigidez de la cuerda.
        const bool haveB = haveReading && mInharmonicity.estimateFrom(mStrobe);
        values[kSnapInharmonicityB] = haveB ? static_cast<float>(mInharmonicity.b()) : nan;
        values[kSnapInharmonicityMeasured] = haveB ? 1.0f : 0.0f;

        values[kSnapLockedString]  = static_cast<float>(mFastMode.lockedIndex());
        values[kSnapFastModeState] = static_cast<float>(mFastMode.state());

        // REQ-009 S2 (AC-009.3) — por que este estado NO es "todavia no".
        //
        // Sale del estimador y no de `droppedDelta`, y la diferencia no es de
        // estilo: `droppedDelta` describe UN TICK, y lo que el consumidor
        // necesita saber es si LA LECTURA QUE ESTA MIRANDO arrastra un hueco.
        // Eso lo sabe el que tiene la ventana — se levanta con el hueco y se
        // baja sola cuando la integracion vuelve a tener una medicion propia.
        values[kSnapInputDiscontinuity] =
            mStrobe.sawInputDiscontinuity() ? 1.0f : 0.0f;

        mSnapshot.publish(values);
    }
}

}  // namespace wma::analysis
