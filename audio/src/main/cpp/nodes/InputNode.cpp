#include "InputNode.h"
#include "../platform/Logger.h"
#include "../platform/Platform.h"  // WD-1.2 — flushDenormalsRtSafe()

// Oboe is Android-only. The input *capture* path (opening a mic/line stream)
// lives entirely behind this guard; the rest of InputNode — DSP, gain, noise
// gate, level metering, the ring buffers — is portable and compiles everywhere.
// On iOS the stream methods are inert (no capture yet); a CoreAudio input
// adapter would slot in at this same seam. See AudioEngine.cpp for the same
// WMA_HAS_OBOE pattern.
#if defined(__ANDROID__)
#define WMA_HAS_OBOE 1
#include <oboe/Oboe.h>
#else
#define WMA_HAS_OBOE 0
#endif

#include <cmath>
#include <algorithm>
#include <cassert>
#include <cstring>

// ===========================================================================
// GANCHOS DE TEST (WMA_TEST_HOOKS)
// ===========================================================================
//
// Los define UNICAMENTE `core/tests/CMakeLists.txt`. En el binario que shippea
// —Android e iOS— `WMA_TEST_HOOKS` no esta definido y todo esto compila a nada:
// ni una variable, ni una rama.
//
// POR QUE VIVEN EN EL ARCHIVO DE PRODUCCION Y NO EN UN DOBLE
// ----------------------------------------------------------
// Hasta hoy la suite de host sustituia este archivo entero por
// `support/test_input_node_stub.cpp`, asi que `InputNode.cpp` **no lo compilaba
// nadie fuera de Android e iOS**. Eso ya se cobro un bug: una llamada a
// `wma::platform::flushDenormalsRtSafe()` sin el include de `Platform.h` paso
// los 795 tests en verde —normal, ASan y TSan— y la agarro recien el build de
// iOS. Y el doble no era inerte: se lo habia extendido dos veces para poder
// observar cosas que desde afuera no se ven.
//
// Lo que se observa aca es exactamente eso, y nada mas:
//
//   1. EN QUE THREAD CORRE EL DESTRUCTOR. El contrato de WD-1.3 es que el nodo
//      se destruye en el thread de CONTROL y nunca en el de audio, y eso no es
//      observable desde afuera del destructor: para cuando el test podria
//      mirar, el objeto ya no existe.
//
//   2. UNA COMPUERTA que vuelve determinista la ventana de la carrera. El bug
//      de WD-1.3 necesita que el thread de audio este ADENTRO del callback, con
//      el nodo en uso, justo cuando el de control lo retira. Esa ventana dura
//      microsegundos: se probo con 40 retiros por corrida y 15 corridas, y el
//      codigo BUGGEADO paso siempre. `isMonitoringEnabled()` es el primer
//      metodo que el callback llama sobre el nodo (`AudioEngine.cpp`, en
//      `onAudioReady`), asi que bloquear ahi lo deja atrapado exactamente en el
//      estado que importa.
//
// Ninguno de los dos cambia comportamiento: uno registra, el otro espera a que
// el test lo suelte.
#if defined(WMA_TEST_HOOKS)
#include <thread>

std::atomic<int> gInputNodeDtorCount{0};
std::atomic<std::thread::id> gInputNodeDtorThread{};
std::atomic<bool> gInputNodeHoldInCallback{false};
std::atomic<bool> gInputNodeIsInCallback{false};
#endif

#define LOG_TAG "InputNode"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

#if WMA_HAS_OBOE
// ========== OBOE ADAPTER (WA-2.0) ==========
// Concentrates every Oboe dependency of InputNode: the callback inheritance and
// the stream handle. InputNode only sees it through an opaque pointer, which is
// what keeps <oboe/Oboe.h> out of InputNode.h and lets the core be built for
// platforms that have no Oboe. A CoreAudio adapter would sit at this same seam.
class InputOboeAdapter : public oboe::AudioStreamDataCallback {
public:
    explicit InputOboeAdapter(InputNode* node) : mNode(node) {}

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream,
                                          void* audioData,
                                          int32_t numFrames) override {
        // RT path: one pointer hop plus getChannelCount(), which in Oboe is a
        // plain member read. No allocation, no locking, no indirect call setup.
        return mNode->processInputBlock(static_cast<float*>(audioData),
                                        numFrames,
                                        stream->getChannelCount())
                   ? oboe::DataCallbackResult::Continue
                   : oboe::DataCallbackResult::Stop;
    }

    // Owned here rather than in InputNode so the header stays Oboe-free.
    std::shared_ptr<oboe::AudioStream> stream;

private:
    InputNode* mNode;
};

void InputNode::BackendAdapterDeleter::operator()(void* p) const {
    delete static_cast<InputOboeAdapter*>(p);
}

namespace {

// The nested deleter type is private, so the adapter is reached by casting the
// raw void* rather than by a helper that would have to name that type.
InputOboeAdapter* asAdapter(void* p) {
    return static_cast<InputOboeAdapter*>(p);
}

oboe::InputPreset inputPresetForSource(InputSource source) {
    switch (source) {
        case InputSource::MIC:
            // VoicePerformance is optimized for low-latency music/voice
            return oboe::InputPreset::VoicePerformance;
        case InputSource::LINE_IN:
            // Unprocessed gives raw audio without AGC/noise suppression
            return oboe::InputPreset::Unprocessed;
        case InputSource::USB_DAC:
            return oboe::InputPreset::Unprocessed;
        case InputSource::BLUETOOTH:
            return oboe::InputPreset::Generic;
        default:
            return oboe::InputPreset::Generic;
    }
}

}  // namespace

#else  // !WMA_HAS_OBOE

// Without Oboe nothing populates mBackendAdapter, but the deleter is declared in
// the header and must still link.
void InputNode::BackendAdapterDeleter::operator()(void*) const {}

#endif  // WMA_HAS_OBOE

InputNode::InputNode()
    : mRingBuffer(48000 * 2 * RING_BUFFER_SECONDS)  // Initial size: 1 second stereo at 48kHz
    , mMonitoringBuffer(48000 * 2 * RING_BUFFER_SECONDS)  // Same size for monitoring
{
    mNumInputChannels = 0;  // No inputs from other nodes (this is a source)
    mNumOutputChannels = 2;
    // Los DOS buffers de trabajo, no uno. El constructor dimensionaba solo
    // `mTempBuffer` y dejaba `mMonitorTempBuffer` VACIO, asi que un nodo al que
    // se le da de comer antes de `prepare()` escribia en un buffer de tamano 0
    // en cuanto el volumen de monitoreo no fuera 1,0 — protegido nada mas que
    // por un `assert`, o sea por nada en release. Dimensionarlos juntos es lo
    // que hace que `clampToWorkBuffers()` mida algo coherente.
    mTempBuffer.resize(8192);
    mMonitorTempBuffer.resize(8192);
}

InputNode::~InputNode() {
    stopInputStream();
#if defined(WMA_TEST_HOOKS)
    // WD-1.3 — ver la nota de los ganchos arriba. Va DESPUES de stopInputStream()
    // para que el registro cubra la destruccion entera, no solo su comienzo.
    gInputNodeDtorThread.store(std::this_thread::get_id(), std::memory_order_release);
    gInputNodeDtorCount.fetch_add(1, std::memory_order_release);
#endif
}

void InputNode::prepare(int sampleRate, int maxBlockSize) {
    AudioNode::prepare(sampleRate, maxBlockSize);

    // Prepare DSP components
    mDCBlocker.prepare(sampleRate);
    mNoiseGate.prepare(sampleRate);
    mLevelMeter.prepare(sampleRate);

    // Resize ring buffers for 1 second of stereo audio
    mRingBuffer.resize(sampleRate * 2 * RING_BUFFER_SECONDS);
    mMonitoringBuffer.resize(sampleRate * 2 * RING_BUFFER_SECONDS);

    // Resize temp buffers
    mTempBuffer.resize(maxBlockSize * 2);
    mMonitorTempBuffer.resize(maxBlockSize * 2);

    LOGI("InputNode prepared: sampleRate=%d, maxBlockSize=%d", sampleRate, maxBlockSize);
}

void InputNode::reset() {
    AudioNode::reset();
    mRingBuffer.clear();
    mMonitoringBuffer.clear();
    mLevelMeter.reset();
    mNoiseGate.reset();
    mDCBlocker.reset();
}

bool InputNode::createInputStream() {
#if !WMA_HAS_OBOE
    // No capture backend on this platform yet (iOS input is future work). The
    // node still exists and processes; it just never has a live input stream.
    return false;
#else
    // The adapter is created on first use and then kept for the lifetime of the
    // node: Oboe holds a raw pointer to it while a stream is open, so it must
    // outlive every stream it is registered with.
    if (!mBackendAdapter) {
        mBackendAdapter.reset(new InputOboeAdapter(this));
    }
    auto* adapter = asAdapter(mBackendAdapter.get());

    oboe::AudioStreamBuilder builder;

    // Use Shared mode for better device routing compatibility
    // This allows proper switching when USB DAC or other devices are connected
    builder.setDirection(oboe::Direction::Input)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Shared)  // Changed from Exclusive for device routing
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(oboe::ChannelCount::Stereo)
            // Don't force sample rate - let system choose best rate for the device
            ->setDataCallback(adapter)
            ->setInputPreset(inputPresetForSource(mInputSource.load()));

    oboe::Result result = builder.openStream(adapter->stream);

    if (result != oboe::Result::OK) {
        LOGE("Failed to open input stream: %s", oboe::convertToText(result));
        return false;
    }

    // Update latency with actual measurement
    updateLatency();

    auto& stream = adapter->stream;
    auto framesPerBurst = stream->getFramesPerBurst();
    auto actualSampleRate = stream->getSampleRate();
    auto deviceId = stream->getDeviceId();
    auto sharingMode = stream->getSharingMode();

    // DEBUG: Detailed stream info for routing diagnostics
    LOGI("=== INPUT STREAM OPENED ===");
    LOGI("  Sample rate: %d Hz", actualSampleRate);
    LOGI("  Channel count: %d", stream->getChannelCount());
    LOGI("  Sharing mode: %s", sharingMode == oboe::SharingMode::Shared ? "Shared" : "Exclusive");
    LOGI("  Performance mode: %s",
         stream->getPerformanceMode() == oboe::PerformanceMode::LowLatency ? "LowLatency" :
         stream->getPerformanceMode() == oboe::PerformanceMode::PowerSaving ? "PowerSaving" : "None");
    LOGI("  Device ID: %d", deviceId);
    LOGI("  Frames per burst: %d", framesPerBurst);
    LOGI("  Buffer capacity: %d frames", stream->getBufferCapacityInFrames());
    LOGI("  Latency: %.1f ms", getInputLatencyMs());
    LOGI("===========================");

    // Store actual sample rate for reference
    mSampleRate = actualSampleRate;

    return true;
#endif  // WMA_HAS_OBOE
}

void InputNode::closeInputStream() {
#if WMA_HAS_OBOE
    auto* adapter = asAdapter(mBackendAdapter.get());
    if (adapter && adapter->stream) {
        adapter->stream->stop();
        adapter->stream->close();
        adapter->stream.reset();
        LOGI("Input stream closed");
    }
#endif
}

bool InputNode::startInputStream() {
#if !WMA_HAS_OBOE
    return false;  // No capture backend on this platform (see createInputStream).
#else
    if (mInputStreamRunning.load()) {
        LOGI("Input stream already running");
        return true;
    }

    auto* adapter = asAdapter(mBackendAdapter.get());
    if (!adapter || !adapter->stream) {
        if (!createInputStream()) {
            return false;
        }
        adapter = asAdapter(mBackendAdapter.get());
    }

    oboe::Result result = adapter->stream->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start input stream: %s", oboe::convertToText(result));
        return false;
    }

    mInputStreamRunning.store(true);
    LOGI("Input stream started");
    return true;
#endif  // WMA_HAS_OBOE
}

void InputNode::stopInputStream() {
    mInputStreamRunning.store(false);
    closeInputStream();
    mRingBuffer.clear();
    LOGI("Input stream stopped");
}

bool InputNode::isInputStreamRunning() const {
    return mInputStreamRunning.load();
}

int InputNode::clampToWorkBuffers(int numFrames) {
    if (numFrames <= 0) return 0;
    const size_t needed = static_cast<size_t>(numFrames) * 2;
    const size_t room = std::min(mTempBuffer.size(), mMonitorTempBuffer.size());
    if (needed <= room) return numFrames;
    mFeedClampedBlocks.bump();  // WD-1.1 — era un LOGW incondicional
    return static_cast<int>(room / 2);
}

void InputNode::processCapturedBlock(float* stereo, int numFrames,
                                     wma::RtCounter& monitorOverflowCounter) {
    const size_t numSamples = static_cast<size_t>(numFrames) * 2;

    // Ganancia de entrada
    const float gain = mInputGainLinear.load(std::memory_order_relaxed);
    if (std::abs(gain - 1.0f) > 0.001f) {
        for (size_t i = 0; i < numSamples; ++i) {
            stereo[i] *= gain;
        }
    }

    // DC blocking — el camino de entrada tiene el SUYO, distinto del que corre
    // sobre el bus del instrumento. Que sean dos no es redundancia: son dos
    // senales que llegan por caminos distintos.
    mDCBlocker.process(stereo, numFrames);

    if (mNoiseGateEnabled.load(std::memory_order_relaxed)) {
        mNoiseGate.process(stereo, numFrames);
    }

    // Medicion de nivel — publica atomicos que lee el thread de control
    // (getInputLevel / isNoiseGateOpen). WD-1.1: era ademas un log periodico.
    mLevelMeter.process(stereo, numFrames);

    // Ring de captura, para el grafo de audio.
    if (mRingBuffer.availableToWrite() >= numSamples) {
        mRingBuffer.write(stereo, numSamples);
    }

    // Ring de monitoreo.
    if (!mMonitoringEnabled.load(std::memory_order_relaxed)) {
        return;
    }
    const float monitorVolume = mMonitoringVolume.load(std::memory_order_relaxed);
    const float* toWrite = stereo;
    if (std::abs(monitorVolume - 1.0f) > 0.001f) {
        for (size_t i = 0; i < numSamples; ++i) {
            mMonitorTempBuffer[i] = stereo[i] * monitorVolume;
        }
        toWrite = mMonitorTempBuffer.data();
    }
    if (mMonitoringBuffer.availableToWrite() >= numSamples) {
        mMonitoringBuffer.write(toWrite, numSamples);
    } else {
        // WD-1.1 — era un LOGW, y los macros de este archivo NO son
        // condicionales: sobrevivia a release. Ademas un overflow de monitoring
        // se repite por bloque mientras dure, asi que el log realimentaba el
        // problema. El contador lo dice el llamador: cual camino descarto es
        // justo lo que hay que saber.
        monitorOverflowCounter.bump();
    }
}

bool InputNode::processInputBlock(float* audioData, int numFrames, int channelCount) {
    // WD-1.2 — este es el SEGUNDO thread RT del motor: en Android la captura
    // corre en su propio stream de Oboe, con su propio thread y su propio DSP.
    // FPCR/MXCSR son por thread, asi que setearlos en el thread de salida no
    // hace nada por este. `feedExternalInput` NO lo repite, y es correcto: a esa
    // la llama `AudioEngine::onAudioReady`, que ya flusheo en su propio thread.
    wma::platform::flushDenormalsRtSafe();

    if (!mInputStreamRunning.load()) {
        return false;
    }

    numFrames = clampToWorkBuffers(numFrames);
    if (numFrames <= 0) {
        return true;
    }

    float* processBuffer = audioData;

    // Mono llega como un canal y sale como dos. Solo pasa por aca: el camino de
    // USB recibe estereo intercalado del backend.
    if (channelCount == 1) {
        for (int i = numFrames - 1; i >= 0; --i) {
            mTempBuffer[static_cast<size_t>(i) * 2] = audioData[i];
            mTempBuffer[static_cast<size_t>(i) * 2 + 1] = audioData[i];
        }
        processBuffer = mTempBuffer.data();
    } else if (channelCount == 2) {
        std::copy(audioData, audioData + static_cast<size_t>(numFrames) * 2,
                  mTempBuffer.begin());
        processBuffer = mTempBuffer.data();
    }

    processCapturedBlock(processBuffer, numFrames, mMonitorOverflowBlocks);
    return true;
}

void InputNode::process(AudioBuffer& inputBuffer, int numFrames) {
    if (!isActive()) {
        mBuffer.clear();
        return;
    }

    // Read from ring buffer to our internal interleaved temp buffer
    bool success = mRingBuffer.read(mTempBuffer.data(), numFrames * 2);

    if (!success) {
        // Underrun: ring buffer already filled with silence
        mBuffer.clear();
        return;
    }

    float* data = mTempBuffer.data();

    // DC Blocking
    mDCBlocker.process(data, numFrames);

    // Noise Gate (if enabled)
    if (mNoiseGateEnabled.load(std::memory_order_relaxed)) {
        mNoiseGate.process(data, numFrames);
    }

    // Level metering
    mLevelMeter.process(data, numFrames);

    // Copy to output buffer (de-interleave)
    mBuffer.copyFromInterleaved(data, numFrames);
}

void InputNode::setInputSource(InputSource source) {
    if (mInputSource.load() == source) return;

    bool wasRunning = mInputStreamRunning.load();

    if (wasRunning) {
        stopInputStream();
    }

    mInputSource.store(source);

    if (wasRunning) {
        startInputStream();
    }

    LOGI("Input source changed to: %d", static_cast<int>(source));
}

InputSource InputNode::getInputSource() const {
    return mInputSource.load();
}

void InputNode::setInputGain(float gainDb) {
    mInputGainDb.store(gainDb, std::memory_order_relaxed);
    mInputGainLinear.store(std::pow(10.0f, gainDb / 20.0f), std::memory_order_relaxed);
}

float InputNode::getInputGain() const {
    return mInputGainDb.load(std::memory_order_relaxed);
}

void InputNode::setNoiseGateEnabled(bool enabled) {
    mNoiseGateEnabled.store(enabled, std::memory_order_relaxed);
}

bool InputNode::isNoiseGateEnabled() const {
    return mNoiseGateEnabled.load(std::memory_order_relaxed);
}

void InputNode::setNoiseGateThreshold(float thresholdDb) {
    mNoiseGate.setThreshold(thresholdDb);
}

float InputNode::getInputLevel(int channel) const {
    return mLevelMeter.getPeakDb(channel);
}

float InputNode::getInputLevelLinear(int channel) const {
    return (channel == 0) ? mLevelMeter.getPeakL() : mLevelMeter.getPeakR();
}

bool InputNode::isClipping() const {
    return mLevelMeter.isClipping();
}

bool InputNode::isNoiseGateOpen() const {
    return mNoiseGate.isOpen();
}

int64_t InputNode::getInputLatencyFrames() const {
    return mInputLatencyFrames.load();
}

float InputNode::getInputLatencyMs() const {
    if (mSampleRate <= 0) return 0.0f;
    return static_cast<float>(mInputLatencyFrames.load()) / mSampleRate * 1000.0f;
}

void InputNode::updateLatency() {
#if !WMA_HAS_OBOE
    mInputLatencyFrames.store(0);
    return;
#else
    auto* adapter = asAdapter(mBackendAdapter.get());
    if (!adapter || !adapter->stream) {
        mInputLatencyFrames.store(0);
        return;
    }
    auto& stream = adapter->stream;

    // Try to get actual latency from Oboe
    // calculateLatencyMillis() returns the estimated latency in milliseconds
    oboe::ResultWithValue<double> latencyResult = stream->calculateLatencyMillis();

    if (latencyResult.error() == oboe::Result::OK) {
        // Convert ms to frames
        double latencyMs = latencyResult.value();
        int64_t latencyFrames = static_cast<int64_t>(latencyMs * stream->getSampleRate() / 1000.0);
        mInputLatencyFrames.store(latencyFrames);
        LOGI("Input latency measured: %.1fms (%lld frames)", latencyMs, (long long)latencyFrames);
    } else {
        // Fallback to estimate: framesPerBurst * 2 (double buffering)
        auto framesPerBurst = stream->getFramesPerBurst();
        int64_t estimatedFrames = framesPerBurst * 2;
        mInputLatencyFrames.store(estimatedFrames);
        LOGI("Input latency estimated: %.1fms (%lld frames) - actual measurement unavailable",
             static_cast<float>(estimatedFrames) / stream->getSampleRate() * 1000.0f,
             (long long)estimatedFrames);
    }
#endif  // WMA_HAS_OBOE
}

// Monitoring functions
void InputNode::setMonitoringEnabled(bool enabled) {
    bool wasEnabled = mMonitoringEnabled.load();
    mMonitoringEnabled.store(enabled, std::memory_order_relaxed);

    if (enabled && !wasEnabled) {
        // Clear monitoring buffer when enabling to avoid stale data
        mMonitoringBuffer.clear();
        LOGI("Monitoring enabled");
    } else if (!enabled && wasEnabled) {
        LOGI("Monitoring disabled");
    }
}

bool InputNode::isMonitoringEnabled() const {
#if defined(WMA_TEST_HOOKS)
    // WD-1.3 — la compuerta. Ver la nota de los ganchos arriba.
    if (gInputNodeHoldInCallback.load(std::memory_order_acquire)) {
        gInputNodeIsInCallback.store(true, std::memory_order_release);
        while (gInputNodeHoldInCallback.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        gInputNodeIsInCallback.store(false, std::memory_order_release);
    }
#endif
    return mMonitoringEnabled.load(std::memory_order_relaxed);
}

void InputNode::setMonitoringVolume(float volume) {
    // Clamp to 0-1 range
    float clampedVolume = std::max(0.0f, std::min(1.0f, volume));
    mMonitoringVolume.store(clampedVolume, std::memory_order_relaxed);
}

float InputNode::getMonitoringVolume() const {
    return mMonitoringVolume.load(std::memory_order_relaxed);
}

int InputNode::getMonitoringSamples(float* outputBuffer, int numFrames) {
    if (!mMonitoringEnabled.load(std::memory_order_relaxed)) {
        // Monitoring disabled, fill with silence
        std::memset(outputBuffer, 0, numFrames * 2 * sizeof(float));
        return 0;
    }

    size_t samplesToRead = static_cast<size_t>(numFrames * 2);
    size_t available = mMonitoringBuffer.availableToRead();

    // DEBUG: Log read attempts periodically
    // WD-1.1 — log periodico borrado (macros no condicionales en este archivo).

    // If not enough data, read what's available instead of returning silence
    size_t actualSamplesToRead = samplesToRead;
    int actualFrames = numFrames;

    if (available < samplesToRead) {
        // Read what we have, fill rest with silence
        if (available >= 2) {  // At least 1 stereo frame
            actualSamplesToRead = (available / 2) * 2;  // Round down to stereo frames
            actualFrames = static_cast<int>(actualSamplesToRead / 2);

            // DEBUG: Log partial read
            mMonitorPartialReads.bump();  // WD-1.1 — era un LOGW incondicional
        } else {
            // Really nothing available
            std::memset(outputBuffer, 0, numFrames * 2 * sizeof(float));
            return 0;
        }
    }

    // Read from monitoring buffer
    bool success = mMonitoringBuffer.read(outputBuffer, actualSamplesToRead);
    if (!success) {
        std::memset(outputBuffer, 0, numFrames * 2 * sizeof(float));
        mMonitorReadFailures.bump();  // WD-1.1 — era un LOGE incondicional
        return 0;
    }

    // Fill remaining with silence if we did a partial read
    if (actualSamplesToRead < samplesToRead) {
        std::memset(outputBuffer + actualSamplesToRead, 0,
                   (samplesToRead - actualSamplesToRead) * sizeof(float));
    }

    // DEBUG: Log sample values occasionally
    // WD-1.1 — log periodico borrado.

    return actualFrames;
}

// FIX PHASE 7.2: Feed external audio input (from USB backend)
void InputNode::feedExternalInput(const float* inputData, int numFrames) {
    if (inputData == nullptr || numFrames <= 0) {
        return;
    }

    // El recorte va ANTES de la copia y su resultado lo usa TODO lo de abajo.
    // Antes no: `numSamples` se calculaba antes de recortar y quedaba `const`,
    // asi que el `std::copy` escribia el largo original en un buffer mas chico.
    // Ver la nota de clampToWorkBuffers() en el header.
    numFrames = clampToWorkBuffers(numFrames);
    if (numFrames <= 0) {
        return;
    }
    const size_t numSamples = static_cast<size_t>(numFrames) * 2;

    std::copy(inputData, inputData + numSamples, mTempBuffer.data());
    processCapturedBlock(mTempBuffer.data(), numFrames, mUsbFeedDrops);
}
