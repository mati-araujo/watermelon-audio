#include "AnalysisThread.h"

#include <algorithm>
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

// ---------------------------------------------------------------------------
// SpectralSupportProbe (REQ-031 S1). El porqué entero está en el header.
// ---------------------------------------------------------------------------

SpectralSupportProbe::SpectralSupportProbe()
    : mRing(static_cast<size_t>(kWindowFrames), 0.0f),
      mHann(static_cast<size_t>(kWindowFrames), 0.0f) {
    static_assert((kWindowFrames & (kWindowFrames - 1)) == 0,
                  "el ring indexa con mascara: el largo tiene que ser potencia de dos");
    // Hann "periódica" centrada en (i + ½): sin ceros exactos en las puntas, que a este largo
    // no cambian la fuga y sí tiran un frame de cada lado. La ventana no depende del rate, así
    // que se calcula una sola vez.
    for (int i = 0; i < kWindowFrames; ++i) {
        mHann[static_cast<size_t>(i)] = static_cast<float>(
            0.5 * (1.0 - std::cos(2.0 * M_PI * (static_cast<double>(i) + 0.5) / kWindowFrames)));
    }
}

void SpectralSupportProbe::pushMono(const float* mono, int numFrames) noexcept {
    constexpr int kMask = kWindowFrames - 1;
    for (int i = 0; i < numFrames; ++i) {
        mRing[static_cast<size_t>(mWrite)] = mono[i];
        mWrite = (mWrite + 1) & kMask;
    }
    if (mFilled < kWindowFrames) {
        mFilled = numFrames >= kWindowFrames - mFilled ? kWindowFrames : mFilled + numFrames;
    }
}

double SpectralSupportProbe::magnitudeAt(int sampleRate, double hz) const noexcept {
    constexpr int kMask = kWindowFrames - 1;
    const double w = 2.0 * M_PI * hz / static_cast<double>(sampleRate);
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    // Del frame más VIEJO al más nuevo, para que la ventana de Hann pese la señal en el orden
    // en que sonó. `mWrite` apunta al slot que se va a pisar, o sea al más viejo.
    for (int k = 0; k < kWindowFrames; ++k) {
        const size_t idx = static_cast<size_t>((mWrite + k) & kMask);
        const double x = static_cast<double>(mRing[idx]) * mHann[static_cast<size_t>(k)];
        const double s = x + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    // La magnitud de Goertzel; el `max(0, ·)` sólo defiende el redondeo de un valor que por
    // construcción no es negativo.
    return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2));
}

double SpectralSupportProbe::supportDb(int sampleRate, double hz) const noexcept {
    const double none = -std::numeric_limits<double>::infinity();
    if (sampleRate <= 0 || hz <= 0.0) return none;
    double peak = 0.0, fundamental = 0.0, octave = 0.0;
    for (int k = 1; k <= kHarmonics; ++k) {
        const double f = hz * k;
        if (f >= 0.5 * sampleRate) break;   // por encima de Nyquist no hay parcial que medir
        const double m = magnitudeAt(sampleRate, f);
        if (k == 1) fundamental = m;
        if (k == 2) octave = m;
        if (m > peak) peak = m;
    }
    const double best = std::max(fundamental, octave);
    if (peak <= 0.0 || best <= 0.0) return none;
    return 20.0 * std::log10(best / peak);
}

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
    while (mRunning.load(std::memory_order_acquire)) {
        // Lo UNICO del thread que habia adentro del cuerpo era esta siesta. El
        // resto es trabajo puro sobre el estado del objeto, y por eso se puede
        // manejar desde afuera sin reimplementar nada (REQ-015 S1).
        if (drainOnce() == DrainOutcome::kRingEmpty) {
            std::this_thread::sleep_for(kIdleNap);
        }
    }
}

/**
 * UNA vuelta del analisis. La comparten el thread y el puerto offline.
 *
 * 🔴 EL PUERTO NO REIMPLEMENTA EL ANALISIS, Y ESA ES LA RAZON DE ESTA FUNCION.
 * Un camino offline paralelo mediria OTRO motor: su verde no diria nada del
 * producto, que es justo lo que AC-015.3 existe para impedir. Extraer el cuerpo
 * deja UNA sola definicion del analisis y dos maneras de empujarlo.
 *
 * Devuelve `kRingEmpty` cuando no habia nada que drenar, para que cada llamador
 * decida: el thread duerme, el puerto offline termina. Esa decision es lo unico
 * que los distingue.
 */
AnalysisThread::DrainOutcome AnalysisThread::drainOnce() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    {
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
            mAbsence.reset();
            mInharmonicity.reset();
            mIntonation.reset();
            // `mAppliedTarget` vuelve a 0 para que el objetivo se re-aplique: si
            // no, el strobe recien reseteado se quedaria sin objetivo y el modo
            // no volveria a medir nunca.
            mAppliedTarget = 0.0;
            mLastUserTarget = -1.0;   // -1 no es un objetivo posible: fuerza la re-aplicacion
            // `mSupportDb` NO se toca aca a proposito: la bandera se deriva de `hasPitch()`
            // al publicar, asi que sigue sola al detector reseteado. Un reset explicito aca
            // era una segunda defensa que ningun mutante podia matar (REQ-031.1).
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
            mLastUserTarget = -1.0;
        }
        // 🔴 LA CONDICION ES "CAMBIO EL PEDIDO", NO "DIFIERE DE LO APLICADO" (REQ-030).
        // `mAppliedTarget` tiene un segundo escritor —el modo rapido, mas abajo— asi que
        // preguntar por la diferencia hace que las dos ramas se pisen una vez por tick
        // apenas hay un reenganche, y cada vuelta descarta el ring. Medido: 27 y 26
        // re-aplicaciones en 5 s, y la lectura no convergia nunca.
        if (target != mLastUserTarget && mPreparedRate > 0) {
            mLastUserTarget = target;
            mTargetAppliedByUser.bump();
            mStrobe.setTarget(target);
            mAppliedTarget = target;
            // Lo que quedo en el ring es de la cuerda ANTERIOR. Ver
            // AnalysisRing::skipToNewest().
            //
            // Sin objetivo NO se descarta: no hay contra que integrar, y tirar el ring antes
            // de leerlo deja al analisis sin ver un solo frame — ni el rate de captura se
            // publicaria. Es un rojo MEDIDO, no una precaucion (AC-030.4).
            if (target > 0.0) mRing.skipToNewest();
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

        // --- REQ-009 S3 · el MISMO Δ, para el eje de CAPTURA -----------------
        //
        // El otro contador cuenta lo que se pisa en ESTE ring. Éste cuenta lo que
        // se perdió ANTES de llegar: el ring que cada backend tiene entre su
        // callback de entrada y el de salida tira audio bajo presión —overrun—
        // o entrega silencio de más —underrun—, y las dos cosas llegan acá como
        // un bloque perfectamente normal. Medido en S1: hasta 2,15 cents de
        // error, 21x el presupuesto, con `droppedFrames` en 0 y el motor
        // diciendo CONVERGIDO.
        //
        // 🔴 EL ORDEN NO SE HEREDA DEL DE ARRIBA, Y ESO ES DELIBERADO. Aquél se
        // muestrea DESPUÉS de `read()` porque sus dos `bump()` viven adentro de
        // `read()`. Éste lo bumpea OTRO thread (el de captura) en OTRO lugar, así
        // que ese argumento no aplica. Lo que sí vale es la regla del estampado:
        // el aviso viaja con las muestras, así que leerlo junto al bloque que
        // acaba de salir del ring lo mantiene describiendo a ESE bloque. Leerlo
        // antes del `read()` describiría al bloque anterior — un tick de atraso
        // sobre una integración que ya cruzó el salto.
        // La frontera se compara contra la posicion del LECTOR, no contra un
        // contador: lo que importa es cuando el lector CRUZA el hueco, no cuando
        // la noticia llega. Ver `AnalysisRing::reportCaptureDiscontinuity`.
        //
        // 🔴 SE DESCARTA HASTA HABER PASADO LA COSTURA MAS NUEVA, y esa forma es
        // deliberadamente CONSERVADORA. Se guarda una sola posicion, asi que con
        // varias costuras pendientes a la vez —con huecos cada 4 bloques entran
        // dos en el ring— las viejas se perderian si solo se reaccionara al
        // cruzar exactamente una. Medido con esa version ingenua: quedaban
        // lecturas CONVERGIDAS a 2,1 cents, o sea el defecto entero intacto.
        //
        // Manteniendo el descarte hasta pasar la ultima conocida, ninguna costura
        // se cuela: cuesta tirar tambien algunos bloques sanos ANTERIORES al
        // hueco, o sea latencia de aguja — la misma moneda con la que esta etapa
        // ya paga el "cualquier Δ > 0 reinicia" de S2, y por la misma razon: es
        // preferible a defender un numero elegido a ojo.
        const uint64_t seam = mRing.captureSeamPosition();
        // Si la costura RETROCEDE, el ring se reseteo y las posiciones volvieron
        // a cero: re-sincronizar es lo unico correcto. Sin esto, una costura
        // nueva —que ahora nace con un numero chico— quedaria por debajo de lo
        // ya manejado y NO dispararia nunca. Es la otra mitad del defecto que
        // `AnalysisRing::reset()` arregla de su lado.
        if (seam < mLastCaptureSeam) mLastCaptureSeam = seam;
        const bool crossedSeam = seam > mLastCaptureSeam;
        if (crossedSeam && mRing.readPosition() >= seam) mLastCaptureSeam = seam;

        if (droppedDelta > 0 || crossedSeam) {
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
            // Los DOS ejes entran por el mismo gancho, y no es pereza: para la
            // integración significan exactamente lo mismo —cruzó un salto— y el
            // consumidor tampoco los distingue (la marca es una sola). Tener dos
            // caminos para la misma consecuencia sería superficie sin uso, y dos
            // sitios donde equivocarse.
            // REQ-014 S3 (AC-014.4) — el contador sube por FLANCO, no por tick.
            //
            // La marca del estimador ya distingue "sigo roto" de "me rompi
            // recien": se levanta con el hueco y no vuelve a bajar hasta que la
            // integracion produce una medicion propia. Preguntarle a ella si ya
            // estaba arriba es lo que convierte un desborde SOSTENIDO —que
            // dispara este bloque varias vueltas seguidas— en UN evento.
            //
            // Contar por tick daria un numero que no significa nada: el
            // consumidor no podria distinguir "se rompio una vez" de "el lazo
            // dio muchas vueltas mientras estaba roto", que es la unica pregunta
            // que este contador existe para contestar.
            //
            // Va ACA, en el sitio de DETECCION, y no muestreando la marca al
            // publicar: sobre una costura el lazo hace `continue` antes de
            // publicar (mas abajo), asi que un flanco podria quedar sin publicar
            // y el muestreo al final lo perderia — el mismo modo de falla que
            // este contador viene a cerrar.
            if (!mStrobe.sawInputDiscontinuity()) ++mDiscontinuityCount;
            mStrobe.noteInputDiscontinuity();
        }

        // 🔴 EL BLOQUE DE ESTA VUELTA SE DESCARTA, Y SOLO POR EL EJE DE CAPTURA.
        //
        // Es la diferencia exacta con el eje del ring, y vale la pena decirla
        // porque en S2 se probó descartar y se SACÓ por injustificado: ahí el
        // `AnalysisRing` garantiza que nunca entrega una copia desgarrada —la
        // re-chequea, la cuenta en `mTorn` y devuelve 0—, así que lo que llegaba
        // era contiguo por adentro y alcanzaba con reiniciar.
        //
        // Acá NO hay tal garantía, porque la costura no está en la mecánica del
        // ring sino en el CONTENIDO: el escritor metió audio de los dos lados del
        // hueco en frames consecutivos, y el ring no tiene cómo saberlo. Un
        // drenaje de 2048 frames se lleva los dos lados en el mismo bloque, así
        // que reiniciar antes de alimentarlo deja entrar la costura igual.
        //
        // Medido: sin este descarte quedaban lecturas CONVERGIDAS a 0,446 cents
        // —4,5x el presupuesto— con σ en 0,098, apenas por debajo del umbral. Con
        // el descarte, ninguna. Cuesta un drenaje de latencia sobre una
        // integración que se acaba de tirar de todos modos.
        if (got <= 0) {
            return DrainOutcome::kRingEmpty;
        }

        // El descarte va DESPUES del chequeo de arriba, y el orden importa: con
        // una costura pendiente y el ring VACIO, saltar antes del `kIdleNap`
        // deja el lazo girando en caliente sobre un ring que no tiene nada — un
        // nucleo quemado mientras el afinador espera audio. Con datos, en cambio,
        // saltar sin dormir es lo correcto: la vuelta consumio frames, o sea que
        // avanza hacia la costura en vez de esperarla.
        if (crossedSeam) {
            return DrainOutcome::kSkipped;
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
            // El `bool` que devuelve NO se descarta: dice si esta pasada produjo un
            // veredicto NUEVO. La compuerta de ausencia lo necesita para no contar la
            // misma evidencia una vez por bloque leido (REQ-019.2).
            mFreshPitchVerdict = mDetector.process(mScratch.data(), got);

            // REQ-031 S1 — la sonda de soporte espectral come el MISMO bloque que el
            // detector, y se evalua SOLO cuando el detector produjo un veredicto nuevo: es
            // la altura de ese veredicto la que califica, sobre los frames que lo terminaron.
            // Entre veredictos la altura no cambia, asi que su soporte tampoco — y por eso la
            // bandera no puede parpadear mas rapido que el propio detector.
            mSupportProbe.pushMono(mScratch.data(), got);
            if (mFreshPitchVerdict) {
                if (!mDetector.hasPitch()) {
                    mSupportDb = std::numeric_limits<double>::quiet_NaN();
                } else if (mSupportProbe.isPrimed()) {
                    mSupportDb = mSupportProbe.supportDb(mPreparedRate, mDetector.frequencyHz());
                }
                // Con altura y la sonda todavia sin ventana entera (no pasa en la practica:
                // el detector necesita al menos una ventana propia, que es igual o mas larga)
                // se conserva el ultimo soporte, que es de la misma altura o de la vuelta
                // anterior: a lo sumo 46 ms de atraso, nunca un valor inventado.
            }
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
                mTargetAppliedByFastMode.bump();
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
        // --- REQ-014 S1 (AC-014.1) — ¿HAY ALGO QUE AFINAR? -------------------
        //
        // 🔴 LA AUSENCIA DE SEÑAL NO ES UNA PREGUNTA DE NIVEL, Y ESTA MEDIDO
        // QUE NO PUEDE SERLO. Se reprodujo el 2026-08-25 contra este mismo
        // lazo: para declarar ausencia con el ruido de habitacion que se
        // reporto desde hardware (rms 0,0070) el piso tendria que estar POR
        // ENCIMA de 0,0070; para no apagar una cuerda limpia que HOY se mide
        // bien en una habitacion silenciosa (rms 0,001649, convergida 50 de 50)
        // tendria que estar POR DEBAJO de 0,0016. Estan 4,4x separados en la
        // direccion imposible: **ninguna constante cumple las dos cosas**.
        // Mover `kSilenceFloor` es el arreglo que la medicion refuto.
        //
        // La evidencia que si discrimina es la que el VALOR ya usa: la
        // deteccion gruesa. Medida sobre siete casos, vale 0,98 / 0,90 / 0,72
        // de claridad midiendo una cuerda y 0,48 para abajo sobre ruido, y da
        // 0 Hz cuando no hay altura ninguna.
        //
        // Y "esta altura puede ser este objetivo" NO ESTRENA UN UMBRAL: es
        // `FastModeTracker::kLockCents`, que ya existe en el motor para
        // exactamente esa pregunta y ya trae su justificacion medida. Sin el,
        // el zumbido de red pasa: da una altura de 50 Hz con claridad 0,994 y
        // una compuerta que solo preguntara "hay altura" dejaria el
        // `kStateMeasuring` eterno vivo en la mitad de las habitaciones reales.
        //
        // El piso de nivel se queda, pero para lo unico que sabe contestar: el
        // silencio de verdad, donde no hay ni altura que evaluar.
        const bool detectorRan = mPreparedRate > 0;
        bool tunableSourcePresent = detectorRan && mDetector.hasPitch();
        if (tunableSourcePresent && measuring) {
            const double devCents =
                1200.0 * std::log2(mDetector.frequencyHz() / mAppliedTarget);
            tunableSourcePresent = std::isfinite(devCents) &&
                                   std::fabs(devCents) < FastModeTracker::kLockCents;
        }
        // Sin rate preparado el detector NO CORRIO, y no se puede afirmar
        // ausencia apoyandose en una evidencia que no se produjo: ahi queda el
        // nivel solo, que es el comportamiento anterior.
        // REQ-019 — LA RAMA TONAL NO LE CREE A UNA SOLA LECTURA.
        //
        // Antes esto era la expresion directa, y por eso un hueco transitorio del
        // detector grueso apagaba la aguja: MEDIDO en MINI-010, cuatro lecturas
        // seguidas sin altura sobre una cuerda audible (`hz=0`, `pisados=0`,
        // `discont=0`), 22 rojos de 120 con 10 procesos TSan concurrentes.
        //
        // La rama de NIVEL sigue siendo inmediata y la tonal espera N lecturas.
        // El reparto es lo que hace compatibles AC-019.2 y AC-019.4; el porque
        // esta entero en `AbsenceGate.h`.
        const bool nothingToTune =
            mAbsence.update(rms < kSilenceFloor, detectorRan, tunableSourcePresent,
                            mFreshPitchVerdict);

        // 🔴 AC-014.5 SE CUMPLE POR CONSTRUCCION, Y ESA ES LA PARTE QUE IMPORTA.
        //
        // Colgar el valor de la MISMA compuerta que el rotulo es lo que hace
        // imposible el snapshot que decia "sin señal" trayendo un numero. El
        // defecto era estructural, no una carrera: `haveReading` no miraba el
        // `rms` y el `rms` no tocaba el valor, asi que eran dos compuertas con
        // ventanas distintas —el bloque contra los 4096 frames del estimador—
        // que se pisaban en la transicion. Medido antes del arreglo: 3 de 40
        // snapshots, uno de ellos CONVERGED, y 5 corridas de 5.
        //
        // Replicar la condicion con un segundo `if` mas abajo volveria a dejar
        // dos compuertas que pueden divergir: es el mismo error con otra ropa.
        const bool haveReading = measuring && !nothingToTune && mStrobe.hasSignal() &&
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
        //
        // 🔴 Y por eso `kStateMeasuring` NO puede significar tambien "aca no hay
        // nada": un spinner eterno es la peor de las cuatro respuestas, porque
        // le promete al usuario que el numero esta por llegar. Esa era la mitad
        // de AC-014.1 que si reprodujo.
        int state;
        if (nothingToTune) {
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

        // REQ-031 S1 (AC-031.4) — la bandera COMPAÑERA de `kSnapDetectedHz`: si esa altura
        // tiene soporte en la señal. Se publica SIEMPRE, no solo al no converger.
        //
        // NaN cuando no hay altura: sin altura no hay nada que calificar, y un 0 se leeria
        // como "vi una altura y no le creo". Y se pregunta `hasPitch()` ACA, ademas de al
        // evaluar la sonda, porque el detector puede perder la altura SIN producir un
        // veredicto nuevo —`reset()` por cambio de fuente o de rate— y ahi `mSupportDb`
        // todavia es el de la altura vieja. La bandera sigue al valor que califica, no a la
        // ultima evaluacion. Es la UNICA defensa, a proposito: la version con resets
        // explicitos ademas de esto dejaba un mutante vivo (REQ-031.1, checklist 7).
        //
        // 🔴 UNA SOLA COMPUTACION. La compuerta del estado (S2) lee este mismo valor, no
        // rehace la pregunta: dos derivaciones podrian contradecirse —convergido con bandera
        // en 0—, que es lo que R-PITCH-37 prohibe.
        values[kSnapSpectralSupport] =
            (!mDetector.hasPitch() || std::isnan(mSupportDb))
                ? nan
                : (mSupportDb >= kSpectralSupportFloorDb ? 1.0f : 0.0f);

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

        // REQ-014 S3 (AC-014.4) — la memoria de lo que ya paso. Ver el KDoc de
        // `kSnapDiscontinuityCount`: el flag de arriba se puede PERDER, este no.
        values[kSnapDiscontinuityCount] = static_cast<float>(mDiscontinuityCount);

        mSnapshot.publish(values);
    }
    return DrainOutcome::kPublished;
}

}  // namespace wma::analysis
