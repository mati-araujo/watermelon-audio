#pragma once

#include "../core/graph/AudioNode.h"
#include "../dsp/LockFreeRingBuffer.h"
#include "../dsp/NoiseGate.h"
#include "../dsp/LevelMeter.h"
#include "../dsp/DCBlocker.h"
#include "../analysis/AnalysisRing.h"
#include "../platform/RtCounter.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

/**
 * @enum InputSource
 * @brief Audio input source types
 */
enum class InputSource {
    MIC = 0,        ///< Built-in microphone
    LINE_IN = 1,    ///< Line input (3.5mm jack)
    USB_DAC = 2,    ///< USB audio device
    BLUETOOTH = 3   ///< Bluetooth audio input
};

/**
 * @class InputNode
 * @brief Audio input node for capturing audio from microphone, line-in, or USB
 *
 * This node captures audio from the device's input and makes it available
 * in the audio graph. It includes:
 * - Input gain control
 * - DC blocking
 * - Noise gate (optional)
 * - Level metering
 *
 * The input stream runs on a separate thread and communicates with the
 * output processing thread via a lock-free ring buffer.
 *
 * The capture backend (Oboe on Android) is kept out of this header on purpose:
 * every backend detail lives behind the opaque adapter below, so including this
 * file does not drag a platform audio API into the translation unit. That is
 * what allows the core to be compiled for platforms without Oboe (WA-2.0).
 */
class InputNode : public AudioNode {
public:
    InputNode();
    ~InputNode() override;

    // AudioNode interface
    NodeType getType() const override { return NodeType::INPUT; }
    const char* getName() const override { return "Input"; }

    void prepare(int sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(AudioBuffer& inputBuffer, int numFrames) override;

    /// El backend no sabe decir cuantos xruns lleva. Ver `processInputBlock`.
    static constexpr int32_t kCaptureXRunsUnknown = -1;

    // Entry point for the platform capture callback: takes one interleaved
    // block straight from the device and runs the input DSP chain on it.
    // Returns false when the capture stream must stop.
    //
    // Called on the input audio thread — the implementation is allocation- and
    // lock-free, and the channel count is passed in (instead of being queried
    // from a stream object) so no backend type leaks into this header.
    //
    // `xRunCount` es el ACUMULADO de xruns del stream de captura, tal cual lo
    // reporta el backend, o `kCaptureXRunsUnknown` si no lo sabe. REQ-009 S3
    // (3.2b): en Android el afinador NO pasa por el ring del backend —
    // `wma_tuner_start` hace que este nodo abra su PROPIO stream de Oboe—, asi
    // que los overrun/underrun que cuenta `OboeBackend` pertenecen a un ring
    // que el afinador nunca toca. La fuente correcta de este camino es el xrun
    // de ESTE stream, y por eso entra por parametro: el que lo observa es el
    // mismo callback que despues escribe el `AnalysisRing`.
    //
    // 🔴 NO TIENE DEFAULT A PROPOSITO. Un `= kCaptureXRunsUnknown` dejaria que
    // un llamador nuevo apague el eje de captura entero sin escribir una linea
    // que se pueda revisar en un diff.
    bool processInputBlock(float* audioData, int numFrames, int channelCount,
                           int32_t xRunCount);

private:
    /**
     * @brief El DSP de entrada, COMPARTIDO por los dos caminos de captura.
     *
     * `stereo` es estereo intercalado y se procesa IN PLACE: ganancia de
     * entrada, DC blocker, noise gate, medidor, y las escrituras al ring de
     * captura y al de monitoreo.
     *
     * POR QUE EXISTE (2026-08-18)
     * ---------------------------
     * Esto estaba escrito DOS VECES —una en `processInputBlock` (el camino de
     * Oboe) y otra en `feedExternalInput` (el de USB y el unico que el host y
     * iOS pueden manejar)— y las dos copias ya habian driftado en tres puntos:
     * el flush de denormales y la conversion mono->estereo existian solo en la
     * primera, y el desborde del ring de monitoreo contaba en contadores
     * distintos.
     *
     * Ese tercer punto es la unica diferencia que se conserva, y a proposito:
     * saber CUAL camino descarto es informacion, no ruido. Por eso el contador
     * entra por parametro en vez de estar cableado adentro.
     *
     * Importa mas alla de la prolijidad: `processInputBlock` tiene UN solo
     * llamador en todo el arbol, el adaptador de Oboe, adentro de
     * `#if WMA_HAS_OBOE` — o sea que en host y en iOS es inalcanzable. Mientras
     * el DSP estuvo duplicado, ningun test de host podia tocar el codigo que
     * corre el camino del microfono. Ahora los dos caminos corren ESTE metodo,
     * asi que `core/tests/test_input_node_dsp.cpp` los cubre a los dos.
     *
     * RT-safe: sin asignar, sin loguear, sin locks.
     */
    void processCapturedBlock(float* stereo, int numFrames,
                              wma::RtCounter& monitorOverflowCounter);

    /**
     * @brief Recorta `numFrames` a lo que entra en los buffers de trabajo.
     *
     * Devuelve el valor recortado (0 si no entra nada) y cuenta el recorte.
     *
     * NO ES COSMETICO. Antes cada camino se defendia por su cuenta y los dos lo
     * hacian mal:
     *
     *   - `feedExternalInput` calculaba `numSamples` ANTES de recortar y lo
     *     dejaba `const`, asi que el recorte bajaba `numFrames` pero el
     *     `std::copy` seguia escribiendo el largo original. Medido con ASan
     *     bajo `-DNDEBUG`: `container-overflow`, WRITE de 8192 bytes. En debug
     *     no se veia porque un `assert` disparaba antes — y `assert` desaparece
     *     justo en el build que shippea.
     *   - `processInputBlock` solo tenia el `assert`, sin recorte ninguno.
     *
     * Un solo lugar que recorte, y que devuelva el valor que TODO lo de abajo
     * usa, es lo que hace que las dos mitades no puedan volver a discrepar.
     */
    int clampToWorkBuffers(int numFrames);

    /**
     * @brief Convierte el acumulado de xruns del stream en una COSTURA (REQ-009 S3).
     *
     * Lo llama `processInputBlock` ANTES de escribir el bloque en el ring del
     * afinador, y eso importa: `AnalysisRing::reportCaptureDiscontinuity()`
     * estampa la posicion de escritura del momento, o sea la frontera entre lo
     * que ya entro y lo que esta por entrar. Que es exactamente donde esta el
     * hueco — el xrun ocurrio entre el bloque anterior y este.
     *
     * 🔑 **Lo llama el mismo thread que escribe el ring**, y esa es la unica
     * razon por la que puede estampar posicion. En el camino de iOS/USB el
     * overrun lo detecta OTRO thread y por eso ahi hace falta otro mecanismo
     * (bandera pendiente). Ver el doc de la etapa, hallazgo D.
     *
     * REGLAS, y las tres estan medidas o razonadas:
     *
     *   - `kCaptureXRunsUnknown` (o cualquier negativo) NO reporta. Un backend
     *     que no sabe contar no puede afirmar que hubo hueco NI que no lo hubo;
     *     inventar un 0 seria decir "todo bien" sin haberlo mirado.
     *   - La PRIMERA observacion siembra y no reporta. Un stream puede arrancar
     *     con el contador ya distinto de cero y no hay audio anterior con el
     *     que ese hueco pudiera ser discontinuo.
     *   - Despues, CUALQUIER cambio reporta — no solo un aumento. Un stream
     *     reabierto vuelve el contador a cero, y eso tambien es una costura.
     *     Comparar con `>` dejaria pasar justo ese caso.
     */
    void noteCaptureXRuns(int32_t xRunCount) noexcept;

public:

    // Stream management
    bool startInputStream();
    void stopInputStream();
    bool isInputStreamRunning() const;

    // Configuration
    void setInputSource(InputSource source);
    InputSource getInputSource() const;

    void setInputGain(float gainDb);
    float getInputGain() const;

    void setNoiseGateEnabled(bool enabled);
    bool isNoiseGateEnabled() const;
    void setNoiseGateThreshold(float thresholdDb);

    // Monitoring
    float getInputLevel(int channel) const;       // -120 to 0 dB
    float getInputLevelLinear(int channel) const; // 0 to 1
    bool isClipping() const;
    bool isNoiseGateOpen() const;

    // Latency info
    int64_t getInputLatencyFrames() const;
    float getInputLatencyMs() const;
    void updateLatency();  // Call periodically to refresh latency

    // Sample rate (returns the actual sample rate of the input stream)
    /**
     * @brief El rate al que corre la CAPTURA. 0 = nadie lo dijo todavia.
     *
     * REEMPLAZA A `getStreamSampleRate()`, QUE MENTIA POR NOMBRE. Aquel
     * devolvia `mSampleRate` —el rate con el que se PREPARO el nodo— asi que un
     * nodo recien construido ya respondia 48000, el default de `AudioNode`, sin
     * que existiera stream ninguno. Y como el unico `prepare()` que el motor le
     * hace es el `prepare(48000, 4096)` LITERAL de `wmaEnsureInputNode`, el
     * camino de captura reportaba 48000 corriera a lo que corriera.
     *
     * Para el afinador de REQ-001 eso no es cosmetico: la conversion de
     * muestras a Hz usa este numero, y creerle 48000 a una captura de 44,1 kHz
     * escala TODAS las frecuencias por 1,0884. Ver
     * `core/tests/test_capture_sample_rate.cpp`.
     *
     * **Cero significa desconocido, y por eso no es 48000.** Un default
     * plausible es peor que la ausencia: el consumidor no puede distinguirlo de
     * una medicion.
     */
    int getCaptureSampleRate() const noexcept {
        return mCaptureSampleRate.load(std::memory_order_acquire);
    }

    /**
     * @brief Publica el rate de la captura. Thread de control.
     *
     * Es un `store` atomico y NADA MAS, a proposito. Lo tentador seria llamar a
     * `prepare()` para que el DSP de entrada se re-configure — y seria un
     * use-after-free: `prepare()` hace `resize()` de los dos rings y de los dos
     * buffers de trabajo, y el thread de captura puede estar adentro de
     * `processCapturedBlock()` usandolos. Es exactamente la clase de defecto
     * que WD-1.3 saco del retiro del nodo.
     *
     * ⚠️ Consecuencia CONOCIDA y no resuelta aca: el DC blocker, el noise gate
     * y el medidor se quedan con los coeficientes del rate con el que se
     * prepararon. Sus constantes de tiempo quedan corridas en la misma
     * proporcion (8,8 % entre 48 y 44,1 kHz). Es un defecto real y MENOR que el
     * de frecuencia —afecta tiempos de ataque, no la altura medida— y
     * arreglarlo pide re-preparar el nodo con el protocolo de publicacion y
     * retiro de WD-1.3, que es un item propio.
     */
    /**
     * @brief Conecta (o desconecta con `nullptr`) el ring del afinador.
     *
     * REQ-001 S1, tarea 1.11. El thread de captura le deja cada bloque ya
     * procesado; el analisis lo drena desde su propio thread.
     *
     * ⚠️ **El ring tiene que sobrevivir al nodo.** Aca solo se guarda un
     * puntero: publicarlo es un store atomico, pero DESTRUIR el ring mientras
     * el thread de captura esta adentro de `processCapturedBlock()` es un
     * use-after-free. El dueño del ring lo retira con la misma barrera con la
     * que el motor retira el nodo (WD-1.3): poner `nullptr`, esperar a que no
     * queden callbacks en vuelo, y recien entonces destruir.
     */
    /**
     * @brief La CAPTURA perdio continuidad: avisale al afinador (REQ-009 S3).
     *
     * La llama el BACKEND —o quien lo represente— cuando tiro audio de entrada:
     * `OboeBackend` cuando su ring de captura desborda (overrun) o se queda
     * corto (underrun), `CoreAudioBackend` en los mismos dos puntos. Los dos ya
     * saben; hasta REQ-009 S3 esa noticia se perdia acá.
     *
     * POR QUE PASA POR EL NODO Y NO VA DERECHO AL RING. Porque el nodo es quien
     * sabe si hay un afinador escuchando: `mAnalysisRing` puede ser nullptr, y
     * el backend no tiene por que enterarse de que existe un afinador. Es la
     * misma direccion de dependencia que el resto de `analysis/`.
     *
     * RT-safe: un `fetch_add` relajado sobre un atomico, o nada si no hay ring.
     * La llaman los dos threads de audio.
     */
    void reportCaptureDiscontinuity(uint64_t framesAhead = 0) noexcept {
        if (auto* ring = mAnalysisRing.load(std::memory_order_acquire)) {
            ring->reportCaptureDiscontinuity(framesAhead);
        }
    }

    void setAnalysisRing(wma::analysis::AnalysisRing* ring) noexcept {
        mAnalysisRing.store(ring, std::memory_order_release);
    }

    void setCaptureSampleRate(int sampleRate) noexcept {
        mCaptureSampleRate.store(sampleRate > 0 ? sampleRate : 0,
                                 std::memory_order_release);
    }

    // Monitoring (pass-through to output)
    void setMonitoringEnabled(bool enabled);
    bool isMonitoringEnabled() const;
    void setMonitoringVolume(float volume);  // 0.0 to 1.0
    float getMonitoringVolume() const;

    // Get monitoring samples for output mixing (thread-safe)
    // Returns number of frames actually read
    int getMonitoringSamples(float* outputBuffer, int numFrames);

    // FIX PHASE 7.2: Feed external audio input (from USB backend)
    // This allows USB audio input to be processed through the same pipeline
    // as native microphone input (INPUT_FX mode, vocoder, etc.)
    void feedExternalInput(const float* inputData, int numFrames);

private:
    bool createInputStream();
    void closeInputStream();

private:
    // Opaque handle to the capture-backend adapter defined in InputNode.cpp.
    // It owns both the backend callback inheritance and the stream handle, so
    // no backend type has to appear here. void* plus a custom deleter is used
    // because unique_ptr cannot delete an incomplete type; the deleter is
    // defined in the .cpp where the adapter is complete.
    struct BackendAdapterDeleter { void operator()(void* p) const; };
    std::unique_ptr<void, BackendAdapterDeleter> mBackendAdapter;

    std::atomic<bool> mInputStreamRunning{false};

    // Ring buffer for input data (1 second capacity)
    // Note: Buffer size affects memory, not latency. Latency is determined by
    // ISO transfer timing and DSP processing rate.
    static constexpr size_t RING_BUFFER_SECONDS = 1;
    LockFreeRingBuffer mRingBuffer;

    // Source configuration
    std::atomic<InputSource> mInputSource{InputSource::MIC};
    // BUGFIX: Default +12dB gain to compensate for low sensitivity of mobile microphones
    std::atomic<float> mInputGainDb{12.0f};
    std::atomic<float> mInputGainLinear{3.981f};  // 10^(12/20) ≈ 3.981

    // DSP processing
    StereoDCBlocker mDCBlocker;
    NoiseGate mNoiseGate;
    // Off by default: a -60 dB gate after +12 dB input gain cuts audible
    // guitar decay tails. Opt-in from the UI (InputTestScreen / Gain Staging).
    /// El ring del afinador, o nullptr. Ver setAnalysisRing().
    std::atomic<wma::analysis::AnalysisRing*> mAnalysisRing{nullptr};

    /// 0 = desconocido. Ver getCaptureSampleRate().
    std::atomic<int> mCaptureSampleRate{0};

    /// Ultimo acumulado de xruns visto en el stream de captura. -1 = todavia
    /// ninguno. Lo escribe el thread de captura; es atomico porque el de
    /// control lo puede leer al arrancar o parar el stream. Ver noteCaptureXRuns().
    std::atomic<int64_t> mLastCaptureXRuns{-1};

    std::atomic<bool> mNoiseGateEnabled{false};
    LevelMeter mLevelMeter;

    // Temporary buffer for processing (pre-allocated in prepare())
    std::vector<float> mTempBuffer;

    // Temporary buffer for monitoring volume scaling (pre-allocated in prepare())
    std::vector<float> mMonitorTempBuffer;

    // Latency tracking
    std::atomic<int64_t> mInputLatencyFrames{0};

    // Monitoring
    std::atomic<bool> mMonitoringEnabled{false};
    std::atomic<float> mMonitoringVolume{0.7f};  // Default 70% to avoid feedback

    // Separate ring buffer for monitoring output (so input processing doesn't affect monitoring)
    LockFreeRingBuffer mMonitoringBuffer;

    // ========== DIAGNOSTICO RT (WD-1.1) ==========
    //
    // Este archivo definia LOGI/LOGW/LOGE SIN condicional de NDEBUG, asi que
    // sus trece logs del path de captura llegaban a release. Y son justo los
    // que peor se comportan: un overflow de monitoring se repite en cada
    // bloque mientras dure, o sea que el log realimentaba el problema con un
    // syscall cada 2,7 ms sobre un thread que ya no llegaba a tiempo.
    wma::RtCounter mMonitorOverflowBlocks;  ///< no habia lugar en el ring de monitoring
    wma::RtCounter mMonitorPartialReads;    ///< se leyo menos de lo pedido
    wma::RtCounter mMonitorReadFailures;    ///< la lectura del ring fallo
    wma::RtCounter mUsbFeedDrops;           ///< bloque de USB descartado, ring lleno
    wma::RtCounter mFeedClampedBlocks;      ///< numFrames recortado al temp buffer

    // REQ-012 S1 — la compuerta del thread de captura. Ver `CaptureQuiesce`.
    std::atomic<int>  mCaptureInFlight{0};      ///< bloques de captura adentro AHORA
    std::atomic<bool> mCaptureGateClosed{false};///< el control pidio que no entre nadie
    wma::RtCounter    mCaptureGatedBlocks;      ///< bloques que la encontraron cerrada

public:
    uint64_t getMonitorOverflowBlocks() const { return mMonitorOverflowBlocks.get(); }
    uint64_t getMonitorPartialReads() const { return mMonitorPartialReads.get(); }
    uint64_t getMonitorReadFailures() const { return mMonitorReadFailures.get(); }
    uint64_t getUsbFeedDrops() const { return mUsbFeedDrops.get(); }
    uint64_t getFeedClampedBlocks() const { return mFeedClampedBlocks.get(); }

    /// Bloques de captura descartados por encontrar la compuerta cerrada (REQ-012 S1).
    ///
    /// No es diagnostico decorativo: cada uno de estos es audio que NO entro, o
    /// sea una discontinuidad. S3 la convierte en la costura que el afinador
    /// necesita para no integrar a traves del hueco.
    uint64_t capturedBlocksGated() const { return mCaptureGatedBlocks.get(); }

    /**
     * @brief Re-prepara el DSP de entrada para `sampleRate`. Thread de CONTROL.
     *
     * Es lo que faltaba para que el camino de captura siga al rate real en vez de
     * quedarse con el que tenia cuando se construyo el nodo. Devuelve si se
     * re-preparo: **`false` significa que NO se toco nada** porque no se pudo
     * confirmar el drenaje, y el llamador tiene que poder distinguirlo.
     *
     * DELEGA EN `prepare()`, y esa es una decision de diseño, no una comodidad.
     * La alternativa —una lista propia de "lo que hay que re-configurar"— es
     * exactamente la que REQ-006.2 encontro drifteada un piso mas arriba:
     * `onStreamConfigChanged` mantenia tres entradas contra las doce de
     * `configureComponentsWithSampleRate`, y el sintoma fue un dispatcher que no
     * se enteraba de los cambios de rate. Delegando **no hay dos listas que
     * puedan diferir**, que es mas fuerte que un trinquete que las compare.
     *
     * 🔴 **DRENA UN SOLO ESCRITOR.** Ver el KDoc de `CaptureQuiesce`: el otro es
     * `feedExternalInput()`, que corre en el thread de SALIDA. Quien llame a esto
     * con el nodo publicado en `AudioEngine::mInputNodeRt` tiene que haberlo
     * retirado primero — publicar `nullptr`, drenar los callbacks, re-preparar y
     * republicar— o el `resize()` corre contra el camino de USB.
     *
     * TIRA EL AUDIO EN VUELO. `prepare()` hace `resize()` de los dos rings, asi que
     * lo que todavia no se consumio se descarta. Los coeficientes del DSP se
     * recalculan para el rate nuevo, pero su ESTADO —el envolvente del medidor, la
     * apertura del gate— NO se reinicia: `prepare()` nunca lo hizo. Eso es una
     * DISCONTINUIDAD para el que este integrando: REQ-012.3 la convierte en la
     * costura que el afinador necesita para no integrar a traves del hueco.
     */
    bool reconfigureForRate(int sampleRate, std::chrono::milliseconds timeout);

    /**
     * @brief El quiesce del thread de CAPTURA. RAII: cierra, drena, y reabre al salir.
     *
     * POR QUE NO ALCANZA EL QUE YA HAY. `AudioEngine::spinForCallbackDrain()`
     * espera a `mActiveCallbacks`, que mueven `onAudioReady` y los backends — el
     * camino de SALIDA. `processInputBlock` no lo toca. La captura de Android es
     * un SEGUNDO thread RT y ningun drenaje del motor lo cubre, asi que
     * `prepare()` —que hace `resize()` de los dos rings y de los dos buffers de
     * trabajo— no tiene hoy forma segura de correr con la captura viva.
     *
     * EL PROTOCOLO, y su unica parte sutil:
     *
     *   control                          captura
     *   -------                          -------
     *   1. cerrar la compuerta           a. contarse como EN VUELO
     *   2. esperar a que no quede         b. consultar la compuerta
     *      nadie en vuelo                 c. si esta cerrada, salir sin tocar nada
     *
     * **`a` va antes que `b`, y ese orden es la propiedad entera.** Al reves
     * queda una ventana: un bloque que consulto la compuerta y la vio abierta,
     * pero todavia no se conto, es invisible para el que drena — el control
     * veria cero, saldria a re-preparar, y el `resize()` correria bajo los pies
     * de ese bloque. Contarse primero convierte "decidi entrar" en algo que el
     * otro thread puede ver.
     *
     * 🔴 EL ORDEN ES ARGUMENTADO, NO PROBADO POR UN TEST, y conviene saberlo antes
     * de tocarlo. REQ-012.2 persiguio el mutante que lo invierte —consultar antes de
     * contarse— con el consumidor real y bajo ASan: 200 reconfiguraciones contra 1
     * thread, 4000 contra 3, y con pausas para que la captura entrara de verdad. **No
     * murio en ninguna.** La ventana son unas pocas instrucciones y para que haya UAF
     * el control tiene que completar el quiesce entero y el `resize()` adentro de
     * ellas. Asi que este orden lo sostiene el razonamiento de arriba y nada mas: si
     * alguien lo invierte, la suite va a quedar verde.
     *
     * COSTO EN EL CAMINO NORMAL: un `fetch_add` y una carga relajada por bloque.
     * Es el segundo thread RT y valen las mismas reglas que el de salida — nada
     * de logging, allocation ni locks que bloqueen aca adentro.
     *
     * 🔴 **ESTO CUBRE UN ESCRITOR, Y EL NODO TIENE DOS.** `processInputBlock` es
     * el camino de captura de Android y corre en el thread del stream de entrada
     * de Oboe — ese es el que esta compuerta drena. Pero `feedExternalInput()`
     * —el camino de USB, y el del vocoder y MIX— entra al MISMO
     * `processCapturedBlock()` desde `AudioEngine::onAudioReady`, o sea desde el
     * thread de SALIDA. A ese lo cubre `mActiveCallbacks` y ninguna otra cosa.
     *
     * Quien vaya a re-preparar el nodo tiene que drenar **los dos**: un
     * `CaptureQuiesce` solo deja el camino de USB corriendo contra el `resize()`.
     * Es el mismo use-after-free, por la puerta menos transitada.
     *
     * DRENAR NO ES UN EXITO GARANTIZADO. Si no se puede confirmar dentro del
     * techo, `drained()` devuelve false y **el llamador no debe re-preparar**.
     * Es la misma eleccion que `AudioEngine::setInputNode()` ya hace del lado
     * seguro: antes filtrar un nodo que arriesgar un use-after-free.
     */
    class CaptureQuiesce {
    public:
        CaptureQuiesce(InputNode& node, std::chrono::milliseconds timeout);
        ~CaptureQuiesce();

        CaptureQuiesce(const CaptureQuiesce&) = delete;
        CaptureQuiesce& operator=(const CaptureQuiesce&) = delete;

        /// Si se pudo confirmar que no queda ningun bloque de captura en vuelo.
        bool drained() const noexcept { return mDrained; }

    private:
        InputNode& mNode;
        bool mDrained;
    };
};
