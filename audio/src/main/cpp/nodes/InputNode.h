#pragma once

#include "../core/graph/AudioNode.h"
#include "../dsp/LockFreeRingBuffer.h"
#include "../dsp/NoiseGate.h"
#include "../dsp/LevelMeter.h"
#include "../dsp/DCBlocker.h"
#include "../platform/RtCounter.h"
#include <atomic>
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

    // Entry point for the platform capture callback: takes one interleaved
    // block straight from the device and runs the input DSP chain on it.
    // Returns false when the capture stream must stop.
    //
    // Called on the input audio thread — the implementation is allocation- and
    // lock-free, and the channel count is passed in (instead of being queried
    // from a stream object) so no backend type leaks into this header.
    bool processInputBlock(float* audioData, int numFrames, int channelCount);

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
    int getStreamSampleRate() const { return mSampleRate; }

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

public:
    uint64_t getMonitorOverflowBlocks() const { return mMonitorOverflowBlocks.get(); }
    uint64_t getMonitorPartialReads() const { return mMonitorPartialReads.get(); }
    uint64_t getMonitorReadFailures() const { return mMonitorReadFailures.get(); }
    uint64_t getUsbFeedDrops() const { return mUsbFeedDrops.get(); }
    uint64_t getFeedClampedBlocks() const { return mFeedClampedBlocks.get(); }
};
