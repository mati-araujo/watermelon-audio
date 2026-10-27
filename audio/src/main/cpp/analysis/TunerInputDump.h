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
 * vacia hacia un `IDumpSink`. El productor NUNCA espera: sin lugar en la cola esos frames se pierden,
 * se cuentan, y la toma queda invalida.
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
};

/// "x.wav" -> "x.invalid.wav"; si no termina en ".wav": path + ".invalid.wav".
std::string invalidDumpPathFor(const std::string& path);

/// true si `path` termina en ".wav" (minusculas, la extension que nombra el volcado). La C API
/// rechaza cualquier otro (hallazgo 1 del security-auditor): el volcado solo crea archivos `.wav`,
/// nunca uno con el nombre que la app le pase.
bool hasWavExtension(const std::string& path);

struct InputDumpStats {
    uint64_t framesWritten = 0;    ///< frames que el sink acepto
    uint64_t framesLost = 0;       ///< pisados en el ring antes de copiar + cola llena + error del sink
    uint64_t discontinuities = 0;  ///< costura de captura cruzada, retroceso de posiciones (reset del
                                   ///< ring), cambio de fuente, cambio de rate dentro de la toma
    int32_t sampleRate = 0;        ///< el estampado al llegar el primer bloque; 0 = nunca llego audio
    int32_t inputSource = -1;      ///< la que paso el control al arrancar la toma
    bool valid = false;            ///< framesLost == 0 && discontinuities == 0 && sampleRate > 0 &&
                                   ///< framesWritten > 0 && el sink no fallo (abrir/escribir/cerrar)
};

/// Cuantos valores lleva el resumen de una toma en la C API (`WMA_TUNER_DUMP_STATS_VALUES`): los
/// campos de `InputDumpStats`, en ese orden, mas `finalized` (el sink devolvio un path final).
inline constexpr int kInputDumpStatsValues = 7;

/// Header WAV fijo: RIFF/WAVE + fmt (18 bytes: formato 3 IEEE float, 1 canal, rate, byteRate =
/// rate * 4, blockAlign 4, 32 bits, cbSize 0) + fact (4: frames) + wmad (40 bytes: u32 version = 1,
/// u64 framesWritten, u64 framesLost, u64 discontinuities, i32 sampleRate, i32 inputSource,
/// u32 valid) + data (framesWritten * 4). Todo little-endian.
///
/// Los tamanos de RIFF, fact y data son de 32 bits, como manda el formato. Que nunca den la vuelta
/// lo garantiza `kMaxTakeFrames` (una toma no escribe mas que eso), no la esperanza de que la toma
/// sea corta. PRECONDICION: `s.framesWritten <= kMaxTakeFrames`.
inline constexpr size_t kInputDumpHeaderBytes = 106;
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
    bool beginTake(std::unique_ptr<IDumpSink> sink, const std::string& path, int inputSource);

    /// Cierra la toma. PRECONDICION: el productor ya esta desarmado (`AnalysisThread` aplico el
    /// desarme). El escritor vacia la cola, se junta, se parchea el header y se nombra el archivo.
    /// Sin toma activa devuelve stats vacias con valid = false y finalPath "".
    InputDumpStats endTake(std::string* finalPath = nullptr);

    bool takeActive() const noexcept { return mActive.load(std::memory_order_acquire); }

    // ---- hilo de analisis (unico productor) ----

    /// Fija el cursor en la posicion de escritura actual del ring (y la costura vista como base:
    /// una anterior al cursor no le pertenece; una DELANTE del cursor si, y se cuenta al armar).
    /// Lo llama `AnalysisThread` al aplicar el armado.
    void armAtRing(const AnalysisRing& ring) noexcept;

    /// Copia [cursor, writePosition) del ring a la cola, en trozos, con `copyForInputDump`. Anota:
    /// el retroceso de posiciones (cursor > writePosition: reset del ring), el cruce de
    /// `captureSeamPosition()` (toda costura mayor que la ultima vista cuenta EN CUANTO SE VE, sin
    /// esperar a cruzarla: invalida de mas a proposito) y el rate (el primero > 0
    /// queda en la toma; uno distinto despues es una discontinuidad). `tryPush` es todo o nada: sin
    /// lugar, esos frames se pierden y se cuentan, y el cursor sigue. Lo que pase del tope por toma
    /// (`maxTakeFrames` encolados) tambien se pierde y se cuenta. NUNCA espera ni asigna.
    void captureFromRing(const AnalysisRing& ring, int captureRate) noexcept;

    /// Anota una discontinuidad que el ring no ve (cambio de fuente, el afinador parado y vuelto a
    /// arrancar). Solo incrementa un atomico.
    void noteInputDiscontinuity() noexcept {
        mDiscontinuities.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- contadores observables (cualquier hilo, atomicos) ----

    /// Frames que entraron a la cola en esta toma.
    uint64_t framesQueued() const noexcept { return mFramesQueued.load(std::memory_order_relaxed); }
    /// Mismo significado que `InputDumpStats::framesLost`, hasta ahora.
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

    std::atomic<bool> mActive{false};
    std::atomic<bool> mStopWriter{false};
    std::atomic<bool> mSinkFailed{false};
    std::atomic<uint64_t> mFramesQueued{0};
    std::atomic<uint64_t> mFramesWritten{0};
    std::atomic<uint64_t> mLostByProducer{0};
    std::atomic<uint64_t> mLostByWriter{0};
    std::atomic<uint64_t> mDiscontinuities{0};
    std::atomic<int> mSampleRate{0};

    // De la toma: los fija `beginTake` antes de arrancar el escritor.
    std::unique_ptr<IDumpSink> mSink;
    std::string mPath;
    int mInputSource = -1;
    std::thread mWriter;
};

}  // namespace wma::analysis
