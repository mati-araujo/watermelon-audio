#pragma once

/**
 * @file TunerInputDump.h
 * @brief El volcado a WAV de lo que el afinador VE (REQ-055 S0 · 0.1). Diagnostico, sin contrato.
 *
 * PARA QUE EXISTE
 * ---------------
 * Medir al afinador sobre grabaciones reales del telefono exige grabar EXACTAMENTE lo que entra al
 * analisis: el contenido del `AnalysisRing` (mono 0,5 * (L + R), despues del DSP de entrada, con el
 * rate que estampo el escritor). Una grabadora aparte mediria otra senal. Un corpus que miente
 * (otro rate, otra senal, un hueco con cara de sano) deja todo lo que se afine contra el mal sin
 * que nada se ponga rojo, y por eso el contrato central de este archivo es negativo: **un WAV con
 * huecos nunca tiene nombre de sano**.
 *
 * COMO ESTA HECHO
 * ---------------
 * Un cursor PROPIO sobre el ring, que mueve el hilo de analisis (el unico productor) sin tocar
 * `mRead`/`mDropped`/`mTorn` ni el scratch del analisis: el analisis queda bit a bit igual con y
 * sin toma. Lo copiado entra a una cola SPSC preasignada; un hilo escritor propio de la toma la
 * vacia hacia un `IDumpSink`. El productor NUNCA espera: sin lugar en la cola esos frames se pierden
 * y se cuentan.
 *
 * EL MAPA DE CORTES (0.6, D16/D25/D26). Una perdida o una costura que se puede UBICAR en el archivo
 * (ring pisado antes de copiar, cola llena, costura de captura) se anota como corte en el `wmad` y
 * la toma sigue siendo sana: quien la mide corta ahi. Lo que no se puede ubicar (tope de toma, error
 * del sink, reset del ring, cambio de fuente, reinicio del afinador, cambio de rate, mapa lleno) la
 * deja invalida. La coordenada de un corte es el frame del ARCHIVO: `mFramesQueued` en el momento
 * (igual al frame del archivo mientras el sink no falla; si falla, la toma ya es invalida).
 *
 * Nucleo puro: ni `fopen` ni disco aca. El borde de E/S es `IDumpSink` (`WavFileDumpSink` en disco,
 * un sink en memoria en los tests).
 *
 * HILOS
 * -----
 *   control   `beginTake`, `endTake` (con el productor ya desarmado), los contadores.
 *   analisis  `armAtRing`, `captureFromRing`, `noteInputDiscontinuity`: el unico productor.
 *   escritor  el hilo de la toma; solo toca la cola, el sink y sus contadores.
 */

#include "AnalysisRing.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace wma::analysis {

/// El borde de E/S. Lo implementan `WavFileDumpSink` (disco) y los tests (memoria).
class IDumpSink {
public:
    virtual ~IDumpSink() = default;
    /// Abre el destino temporal de `path` (el de disco: `<path>.part`) y reserva `headerBytes`.
    virtual bool openPart(const std::string& path, size_t headerBytes) = 0;
    /// Agrega `count` floats. false = error de E/S.
    virtual bool appendSamples(const float* samples, size_t count) = 0;
    /// Reescribe el header en el offset 0, cierra y nombra: valid => `path`; !valid =>
    /// `invalidDumpPathFor(path)`. Devuelve el path final, o "" si fallo.
    virtual std::string finishPart(const uint8_t* header, size_t headerBytes, bool valid) = 0;
    /// Donde quedo lo escrito si `finishPart` no pudo nombrarlo: el de disco, `<path>.part`. ""
    /// si el sink no deja nada en disco (memoria) o nunca abrio. Valido despues de `finishPart`.
    virtual std::string partPath() const = 0;
    /// ABORTA la toma (bug B de REQ-055 S0): cierra y BORRA el temporal que `openPart` creo, y solo
    /// ese. Nunca borra un archivo que el sink no creo: si en `partPath()` hay otro (lo reemplazaron
    /// durante la toma), no se toca. true si en disco no quedo nada en ese nombre; false si algo
    /// quedo (el borrado fallo, o el nombre lo ocupa un archivo ajeno). PRECONDICION: nadie mas
    /// escribe al sink (el escritor de la toma ya se junto).
    virtual bool abortPart() = 0;
};

/// "x.wav" -> "x.invalid.wav"; si no termina en ".wav": path + ".invalid.wav".
std::string invalidDumpPathFor(const std::string& path);

/// true si `path` termina en ".wav" (minusculas, la extension que nombra el volcado). La C API
/// rechaza cualquier otro (hallazgo 1 del security-auditor): el volcado solo crea archivos `.wav`,
/// nunca uno con el nombre que la app le pase.
bool hasWavExtension(const std::string& path);

/// Un CORTE de la toma (REQ-055 S0 · 0.6, D16/D25): una perdida o una costura UBICADA en el archivo.
/// La coordenada es el frame DEL ARCHIVO (el indice en `data`), la que cortan oraculo e instrumento.
///   kind 1 (perdida): `lostFrames` frames que faltan JUSTO ANTES de `fileFrame`; `fileSpan` = 0.
///   kind 2 (costura de captura, D25): el salto cae en algun frame de [fileFrame, fileFrame +
///           fileSpan]; cuantos frames se perdieron no se sabe (`lostFrames` = 0 = desconocido).
struct InputDumpCut {
    uint64_t fileFrame = 0;
    uint64_t lostFrames = 0;
    uint32_t fileSpan = 0;
    uint32_t kind = 0;
};
inline constexpr uint32_t kInputDumpCutLoss = 1;
inline constexpr uint32_t kInputDumpCutSeam = 2;
/// Capacidad FIJA del mapa (preasignado: el productor no asigna). El corte 257 ya no se puede
/// ubicar: va a `unlocated*` y la toma queda invalida.
inline constexpr uint32_t kInputDumpCutCapacity = 256;

struct InputDumpStats {
    uint64_t framesWritten = 0;    ///< frames que el sink acepto
    uint64_t framesLost = 0;       ///< TOTAL: ubicados (suma de `lostFrames` de los cortes) +
                                   ///< `unlocatedFrames`
    uint64_t discontinuities = 0;  ///< TOTAL: avisos de costura vistos + `unlocatedDiscontinuities`
    int32_t sampleRate = 0;        ///< el estampado al llegar el primer bloque; 0 = nunca llego audio
    int32_t inputSource = -1;      ///< la que paso el control al arrancar la toma
    /// unlocatedFrames == 0 && unlocatedDiscontinuities == 0 && sampleRate > 0 && framesWritten > 0
    /// && el sink no fallo (abrir/escribir/cerrar). Una toma con cortes UBICADOS es valida: el
    /// mapa los declara y quien la mide no mide a traves de ellos.
    bool valid = false;
    /// Perdidas que no se pueden ubicar en el archivo: tope de toma, error del sink, el mapa lleno.
    uint64_t unlocatedFrames = 0;
    /// Discontinuidades que no se pueden ubicar: reset del ring, cambio de fuente, reinicio del
    /// afinador con la toma viva (D26), cambio de rate, rate <= 0, una costura con el mapa lleno.
    uint64_t unlocatedDiscontinuities = 0;
    /// El mapa, ordenado por `fileFrame` (estable). Solo valen los primeros `cutCount`.
    uint32_t cutCount = 0;
    InputDumpCut cuts[kInputDumpCutCapacity] = {};
};

/// Cuantos valores lleva el resumen de una toma en la C API (`WMA_TUNER_DUMP_STATS_VALUES`): los
/// campos de `InputDumpStats`, en ese orden, mas `finalized` (el sink devolvio un path final).
inline constexpr int kInputDumpStatsValues = 7;

/// Header WAV fijo: RIFF/WAVE + fmt (18 bytes: formato 3 IEEE float, 1 canal, rate, byteRate =
/// rate * 4, blockAlign 4, 32 bits, cbSize 0) + fact (4: frames) + wmad + data (framesWritten * 4).
/// Todo little-endian.
///
/// `wmad` v2 (REQ-055 S0 · 0.6), cuerpo de `kInputDumpWmadBodyBytes`, CAPACIDAD FIJA: los 40 bytes
/// de v1 en sus offsets (u32 version = 2, u64 framesWritten, u64 framesLost, u64 discontinuities,
/// i32 sampleRate, i32 inputSource, u32 valid; lost y discontinuities siguen siendo TOTALES) + u64
/// unlocatedFrames, u64 unlocatedDiscontinuities, u32 cutCount, u32 cutCapacity (= 256) + 256 x
/// {u64 fileFrame, u64 lostFrames, u32 fileSpan, u32 kind} (los que pasan de cutCount, en cero).
///
/// Los tamanos de RIFF, fact y data son de 32 bits, como manda el formato. Que nunca den la vuelta
/// lo garantiza `kMaxTakeFrames` (una toma no escribe mas que eso), no la esperanza de que la toma
/// sea corta. PRECONDICION: `s.framesWritten <= kMaxTakeFrames`.
inline constexpr uint32_t kInputDumpWmadVersion = 2;
inline constexpr size_t kInputDumpCutBytes = 24;
inline constexpr size_t kInputDumpWmadBodyBytes = 64 + kInputDumpCutBytes * kInputDumpCutCapacity;
/// RIFF/WAVE (12) + fmt (8 + 18) + fact (8 + 4) + wmad (8 + cuerpo) + cabecera de data (8).
inline constexpr size_t kInputDumpHeaderBytes = 12 + 26 + 12 + 8 + kInputDumpWmadBodyBytes + 8;
static_assert(kInputDumpWmadBodyBytes == 6208, "wmad v2: 40 de v1 + 24 + 256 cortes de 24");
static_assert(kInputDumpWmadBodyBytes % 2 == 0, "un chunk RIFF de largo impar lleva relleno");
static_assert(kInputDumpHeaderBytes == 6274, "el header del volcado v2");
void writeInputDumpHeader(uint8_t* out /* kInputDumpHeaderBytes */, const InputDumpStats& s);

/// TOPE DE FRAMES POR TOMA (hallazgo 4 del security-auditor): 2^28 = ~93 min a 48 kHz, 1 GiB de
/// datos. Holgado para el protocolo de medicion (una guitarra, ~10 min) y lejos del limite de 32 bits
/// del RIFF (4 GiB): con el tope, el header no puede dar la vuelta aunque la app nunca llame a stop.
/// Pasado el tope, cada frame cuenta como perdido y la toma cierra invalida (`.invalid.wav`).
inline constexpr uint64_t kMaxTakeFrames = uint64_t{1} << 28;
static_assert(kInputDumpHeaderBytes - 8 + kMaxTakeFrames * sizeof(float) <= UINT32_MAX,
              "el tope de frames tiene que caber en el tamano de 32 bits del RIFF (y del data)");
static_assert(kMaxTakeFrames <= UINT32_MAX, "el tope tiene que caber en el fact de 32 bits");

class TunerInputDump {
public:
    /// ~5,4 s a 48 kHz, 1 MiB: absorbe las pausas de la flash sin perder.
    static constexpr size_t kDefaultQueueFrames = size_t{1} << 18;

    /// Preasigna la cola (potencia de dos; se redondea hacia arriba) y el scratch. Es lo unico que
    /// asigna: `captureFromRing` corre en el hilo de analisis y no puede.
    ///
    /// `maxTakeFrames` es el tope por toma; se inyecta (como el sink) para que un test lo cruce sin
    /// escribir 1 GiB. Solo puede BAJAR: un valor mayor que `kMaxTakeFrames` se recorta a el, asi que
    /// ningun llamador consigue un header que de la vuelta.
    explicit TunerInputDump(size_t queueFrames = kDefaultQueueFrames,
                            uint64_t maxTakeFrames = kMaxTakeFrames);
    /// Si quedo una toma viva: la cierra (`endTake`) y junta el escritor.
    ~TunerInputDump();

    TunerInputDump(const TunerInputDump&) = delete;
    TunerInputDump& operator=(const TunerInputDump&) = delete;

    // ---- thread de control ----

    /// Empieza una toma: resetea contadores, `sink->openPart(path, kInputDumpHeaderBytes)`, arranca
    /// el hilo escritor. NO arma la captura: eso lo aplica `AnalysisThread` por su buzon. false si
    /// ya hay una toma activa (sin abrir el sink: abrir el mismo `.part` truncaria la viva), si
    /// `sink` es null, si `path` es vacio, o si `openPart` falla (sin hilo vivo en ese caso).
    ///
    /// Un false DESPUES de abrir el sink (no se pudo crear el hilo escritor) aborta: el temporal
    /// se borra con `IDumpSink::abortPart` y en disco no queda nada de la toma (bug B). Si el borrado
    /// no pudo, `abortResidue()` dice que quedo.
    bool beginTake(std::unique_ptr<IDumpSink> sink, const std::string& path, int inputSource);

    /// ABORTA la toma en vez de cerrarla (bug B de REQ-055 S0): la usa quien devuelve false tras un
    /// `beginTake` exitoso (el armado no se aplico a tiempo), para que un start fallido no deje un
    /// `.invalid.wav` que nadie reporta y que, con el chequeo de no-pisar, bloquea ese nombre.
    /// PRECONDICION, igual que `endTake`: el productor ya esta desarmado. El ORDEN es lo que hace
    /// seguro el borrado: primero se para y se junta el escritor (nadie escribe el temporal), y
    /// recien despues `IDumpSink::abortPart` lo borra. true si en disco no quedo nada de la toma;
    /// false si algo quedo (`abortResidue()`). Sin toma activa: true, sin tocar nada.
    bool abortTake();

    /// Que quedo en disco del ultimo aborto que no pudo borrar su temporal; "" si el ultimo aborto
    /// (o ninguno) no dejo nada. Para que quien aborto lo deje constando (log, fuera del RT).
    const std::string& abortResidue() const noexcept { return mAbortResidue; }

    /// Cierra la toma. PRECONDICION: el productor ya esta desarmado (`AnalysisThread` aplico el
    /// desarme). El escritor vacia la cola, se junta, se parchea el header y se nombra el archivo.
    /// Sin toma activa devuelve stats vacias con valid = false y finalPath "".
    ///
    /// `finalPath`: el nombre final ("" si el sink no pudo nombrar). `leftPath`: el archivo que
    /// REALMENTE quedo en disco de esta toma (bug A de REQ-055 S0): el nombre final si se pudo
    /// nombrar, si no `IDumpSink::partPath()`. Es la verdad que la C API le devuelve al puente; el
    /// puente no la reconstruye con el path que le pidieron.
    InputDumpStats endTake(std::string* finalPath = nullptr, std::string* leftPath = nullptr);

    bool takeActive() const noexcept { return mActive.load(std::memory_order_acquire); }

    // ---- hilo de analisis (unico productor) ----

    /// Fija el cursor en la posicion de escritura actual del ring (y la costura vista como base:
    /// una anterior al cursor no le pertenece; una DELANTE del cursor si, y se cuenta al armar).
    /// Lo llama `AnalysisThread` al aplicar el armado.
    void armAtRing(const AnalysisRing& ring) noexcept;

    /// Copia [cursor, writePosition) del ring a la cola, en trozos, con `copyForInputDump`. Anota:
    /// - el retroceso de posiciones (cursor > writePosition: reset del ring): NO ubicada;
    /// - lo pisado en el ring antes de copiar y lo que no entro en la cola (`tryPush` es todo o
    ///   nada; el cursor sigue): corte de PERDIDA en el frame del archivo donde falta;
    /// - la costura de captura (D25): el ring guarda solo la mas lejana y avisa asincrono, asi que
    ///   entre dos lecturas puede haber una oculta. Se anota el TRAMO [lo, f(S)], `lo` = el frame
    ///   del archivo en la lectura ANTERIOR de la costura (todo aviso posterior vale >= el cursor de
    ///   entonces). Una S por delante del cursor queda pendiente y la copia se TOPA en S (cambia el
    ///   troceo, no los bits) para cerrar el tramo exacto; una perdida que salta S lo cierra ahi;
    ///   pendiente al cerrar la toma, `endTake` lo cierra en `framesWritten`;
    /// - el rate (el primero > 0 queda en la toma; uno distinto despues, o frames con rate <= 0:
    ///   NO ubicada).
    /// Lo que pase del tope por toma (`maxTakeFrames` encolados) se pierde: NO ubicada. Un corte que
    /// no entra en el mapa (lleno) tambien. NUNCA espera ni asigna.
    void captureFromRing(const AnalysisRing& ring, int captureRate) noexcept;

    /// Anota una discontinuidad NO UBICADA (cambio de fuente, el afinador parado y vuelto a
    /// arrancar con la toma viva, D26): deja la toma invalida. Solo incrementa un atomico; la llaman
    /// el hilo de analisis y el control (`AnalysisThread::start`).
    void noteInputDiscontinuity() noexcept {
        mUnlocatedDiscontinuities.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- contadores observables (cualquier hilo, atomicos) ----

    /// Frames que entraron a la cola en esta toma.
    uint64_t framesQueued() const noexcept { return mFramesQueued.load(std::memory_order_relaxed); }
    /// Mismo significado que `InputDumpStats::framesLost` (el TOTAL), hasta ahora.
    uint64_t framesLost() const noexcept {
        return mLostByProducer.load(std::memory_order_relaxed) +
               mLostByWriter.load(std::memory_order_relaxed);
    }
    /// Frames que el sink acepto hasta ahora.
    uint64_t framesWritten() const noexcept { return mFramesWritten.load(std::memory_order_relaxed); }

private:
    /// Trozo de copia desde el ring: el mismo bloque maximo que el escritor de produccion.
    static constexpr size_t kChunkFrames = 4096;
    /// Frames por llamada a `appendSamples`: escrituras a flash de buen tamano.
    static constexpr size_t kWriterBatchFrames = 8192;
    /// Siesta del escritor con la cola vacia. Holgada contra los ~5,4 s de cola.
    static constexpr int kWriterNapMs = 2;
    /// Tope de reintentos por llamada tras una copia desgarrada: el productor no puede quedar
    /// girando si algo mal configurado pisara el ring mas rapido de lo que se lo copia.
    static constexpr int kMaxCopyRounds = 64;

    bool tryPushFrames(const float* src, size_t n) noexcept;
    void writerLoop();

    // ---- el mapa de cortes (solo el hilo de analisis, salvo `endTake` tras el desarme) ----

    /// Corte de PERDIDA de `lost` frames en el frame del archivo actual (`mFramesQueued`). Se
    /// fusiona con la ultima perdida si cae en el mismo frame; sin lugar en el mapa, NO ubicada.
    void noteDumpLocatedLoss(uint64_t lost) noexcept;
    /// Cierra el tramo de la costura pendiente en el frame del archivo `hi`.
    void closeDumpSeamSpan(uint64_t hi) noexcept;
    /// Anota el tramo de costura [lo, hi]: se fusiona con el ultimo tramo de costura si lo toca; sin
    /// lugar en el mapa, NO ubicada.
    void noteDumpSeamSpan(uint64_t lo, uint64_t hi) noexcept;
    /// Mira la costura del ring (antes y despues de copiar) y abre/extiende/cierra el tramo.
    void readDumpSeam(const AnalysisRing& ring) noexcept;

    /// Tope de frames encolados por toma (<= kMaxTakeFrames). Fijo desde el constructor.
    const uint64_t mMaxTakeFrames;

    // Cola SPSC de floats (1 frame = 1 float). Productor: hilo de analisis; consumidor: escritor.
    std::vector<float> mQueue;
    size_t mQueueMask;
    std::atomic<uint64_t> mHead{0};   ///< la escribe el productor
    std::atomic<uint64_t> mTail{0};   ///< la escribe el escritor, DESPUES de que el sink termino
    std::vector<float> mChunk;        ///< scratch del productor, preasignado

    // Estado del productor: no atomico a proposito, lo toca un solo hilo.
    uint64_t mCursor = 0;
    uint64_t mLastSeam = 0;
    int mLastRate = 0;
    /// El mapa, preasignado (es un arreglo del objeto): el productor lo llena sin asignar.
    InputDumpCut mCuts[kInputDumpCutCapacity] = {};
    uint32_t mCutCount = 0;
    /// Indice en `mCuts` del ultimo tramo de costura y de la ultima perdida; -1 = ninguno.
    int mLastSeamCut = -1;
    int mLastLossCut = -1;
    /// Frame del archivo en la lectura anterior de la costura (el `lo` de un tramo nuevo).
    uint64_t mSeamReadFrame = 0;
    /// Costura delante del cursor (posicion del ring) y el `lo` de su tramo.
    bool mSeamPending = false;
    uint64_t mPendingSeam = 0;
    uint64_t mPendingSeamLo = 0;
    /// Avisos de costura vistos (va al total `discontinuities`).
    uint64_t mSeamNotices = 0;

    std::atomic<bool> mActive{false};
    std::atomic<bool> mStopWriter{false};
    std::atomic<bool> mSinkFailed{false};
    std::atomic<uint64_t> mFramesQueued{0};
    std::atomic<uint64_t> mFramesWritten{0};
    std::atomic<uint64_t> mLostByProducer{0};
    std::atomic<uint64_t> mLostByWriter{0};
    /// La parte de `mLostByProducer` que no se pudo ubicar (tope de toma, mapa lleno).
    std::atomic<uint64_t> mUnlocatedByProducer{0};
    std::atomic<uint64_t> mUnlocatedDiscontinuities{0};
    std::atomic<int> mSampleRate{0};

    // De la toma: los fija `beginTake` antes de arrancar el escritor.
    std::unique_ptr<IDumpSink> mSink;
    std::string mPath;
    int mInputSource = -1;
    std::thread mWriter;
    /// Lo que dejo en disco el ultimo aborto que no pudo borrar ("" si nada). Solo el control.
    std::string mAbortResidue;

    /// Borra el temporal del sink (ya sin escritor) y lo suelta; anota el residuo si quedo algo.
    bool abortSink();
};

}  // namespace wma::analysis
