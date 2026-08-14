/**
 * test_input_node_stub.cpp — TEST DOUBLE, host test build only.
 *
 * A link-time stand-in for nodes/InputNode.cpp, which cannot be compiled on the
 * host: its capture adapter inherits oboe::AudioStreamDataCallback and Oboe is
 * Android-only. Without these definitions the core test binary fails to link on
 * the ~2 dozen InputNode symbols that AudioEngine references.
 *
 * Almost NOT a behavioural double: every method here is the cheapest thing that
 * satisfies its signature, with ONE exception, called out below. If a test needs
 * real input behaviour beyond that exception, it must extend this double
 * deliberately rather than trust these bodies.
 *
 * THE EXCEPTION — the monitoring ring is real (feedExternalInput /
 * getMonitoringSamples). It had to become real because with getMonitoringSamples
 * returning 0 the sum in AudioEngine::handleMixMonitoring() never runs, so
 * nothing about MIX-mode monitoring is observable from the host suite at all —
 * and that blind spot is what let a shipped defect (master volume not reaching
 * the monitored input) live undetected. The two bodies carry the least behaviour
 * that makes the sum happen.
 *
 * Deliberately NOT modelled, so a future test does not read a level off this and
 * believe it: input gain, the noise gate, the DC blocker, and the partial-read
 * bookkeeping of the production node. Monitoring volume IS applied, because it
 * is the one gain a MIX test needs to hold at a known value to read a ratio off
 * the output buffer.
 *
 * The header is Oboe-free by design (WA-2.0), so the class definition below is
 * the production one; only the implementation is swapped.
 */

#include "nodes/InputNode.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

// WD-1.3 — SEGUNDA extension deliberada del doble (la primera es el ring de
// monitoring, documentado arriba). El contrato de WD-1.3 es "el InputNode se
// destruye en el thread de CONTROL, nunca en el de audio", y eso no se puede
// observar desde afuera del destructor. Estas dos variables lo hacen
// observable, y nada mas: no cambian ningun comportamiento.
std::atomic<int> gInputNodeDtorCount{0};
std::atomic<std::thread::id> gInputNodeDtorThread{};

// Y una COMPUERTA, para volver determinista la ventana de la carrera.
//
// El bug de WD-1.3 necesita que el thread de audio este ADENTRO del callback,
// con el nodo en uso, justo cuando el thread de control lo retira. Esa ventana
// dura microsegundos y bombear callbacks a ciegas no la pega: se probo con 40
// retiros por corrida y 15 corridas, y el codigo BUGGEADO paso siempre. Un test
// que no distingue la version rota de la arreglada no prueba nada.
//
// isMonitoringEnabled() es el primer metodo que el callback llama sobre el
// nodo — y, con el codigo viejo, se llamaba DESPUES de copiar el shared_ptr.
// Bloquear aca deja al callback atrapado exactamente en el estado que importa.
std::atomic<bool> gInputNodeHoldInCallback{false};
std::atomic<bool> gInputNodeIsInCallback{false};

void InputNode::BackendAdapterDeleter::operator()(void* p) const {
    // The stub never creates an adapter, so there is nothing to delete. Deleting
    // a void* would be undefined behaviour anyway.
    (void)p;
}

InputNode::InputNode()
    : mRingBuffer(48000 * 2 * RING_BUFFER_SECONDS)
    , mMonitoringBuffer(48000 * 2 * RING_BUFFER_SECONDS) {
    mNumInputChannels = 0;
    mNumOutputChannels = 2;
}

InputNode::~InputNode() {
    gInputNodeDtorThread.store(std::this_thread::get_id(), std::memory_order_release);
    gInputNodeDtorCount.fetch_add(1, std::memory_order_release);
}

void InputNode::prepare(int sampleRate, int maxBlockSize) {
    AudioNode::prepare(sampleRate, maxBlockSize);
}

void InputNode::reset() {
    AudioNode::reset();
}

void InputNode::process(AudioBuffer& inputBuffer, int numFrames) {
    (void)inputBuffer;
    (void)numFrames;
}

bool InputNode::processInputBlock(float* audioData, int numFrames, int channelCount) {
    (void)audioData;
    (void)numFrames;
    (void)channelCount;
    return true;
}

bool InputNode::startInputStream() { return false; }
void InputNode::stopInputStream() {}
bool InputNode::isInputStreamRunning() const { return false; }

void InputNode::setInputSource(InputSource source) {
    mInputSource.store(source, std::memory_order_release);
}

InputSource InputNode::getInputSource() const {
    return mInputSource.load(std::memory_order_acquire);
}

void InputNode::setInputGain(float gainDb) {
    mInputGainDb.store(gainDb, std::memory_order_release);
}

float InputNode::getInputGain() const {
    return mInputGainDb.load(std::memory_order_acquire);
}

void InputNode::setNoiseGateEnabled(bool enabled) {
    mNoiseGateEnabled.store(enabled, std::memory_order_release);
}

bool InputNode::isNoiseGateEnabled() const {
    return mNoiseGateEnabled.load(std::memory_order_acquire);
}

void InputNode::setNoiseGateThreshold(float thresholdDb) { (void)thresholdDb; }

float InputNode::getInputLevel(int channel) const {
    (void)channel;
    return -120.0f;
}

float InputNode::getInputLevelLinear(int channel) const {
    (void)channel;
    return 0.0f;
}

bool InputNode::isClipping() const { return false; }
bool InputNode::isNoiseGateOpen() const { return false; }

int64_t InputNode::getInputLatencyFrames() const {
    return mInputLatencyFrames.load(std::memory_order_acquire);
}

float InputNode::getInputLatencyMs() const { return 0.0f; }
void InputNode::updateLatency() {}

void InputNode::setMonitoringEnabled(bool enabled) {
    mMonitoringEnabled.store(enabled, std::memory_order_release);
}

bool InputNode::isMonitoringEnabled() const {
    if (gInputNodeHoldInCallback.load(std::memory_order_acquire)) {
        gInputNodeIsInCallback.store(true, std::memory_order_release);
        while (gInputNodeHoldInCallback.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        gInputNodeIsInCallback.store(false, std::memory_order_release);
    }
    return mMonitoringEnabled.load(std::memory_order_acquire);
}

void InputNode::setMonitoringVolume(float volume) {
    mMonitoringVolume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_release);
}

float InputNode::getMonitoringVolume() const {
    return mMonitoringVolume.load(std::memory_order_acquire);
}

int InputNode::getMonitoringSamples(float* outputBuffer, int numFrames) {
    if (outputBuffer == nullptr || numFrames <= 0) return 0;

    const size_t requested = static_cast<size_t>(numFrames) * 2;

    if (!mMonitoringEnabled.load(std::memory_order_acquire)) {
        std::fill_n(outputBuffer, requested, 0.0f);
        return 0;
    }

    // Whole stereo frames only: handing back an odd sample count would put the
    // caller's channels out of phase for the rest of the block.
    size_t toRead = std::min(mMonitoringBuffer.availableToRead(), requested);
    toRead -= toRead % 2;

    if (toRead == 0 || !mMonitoringBuffer.read(outputBuffer, toRead)) {
        std::fill_n(outputBuffer, requested, 0.0f);
        return 0;
    }

    std::fill_n(outputBuffer + toRead, requested - toRead, 0.0f);
    return static_cast<int>(toRead / 2);
}

void InputNode::feedExternalInput(const float* inputData, int numFrames) {
    if (inputData == nullptr || numFrames <= 0) return;
    if (!mMonitoringEnabled.load(std::memory_order_acquire)) return;

    const size_t needed = static_cast<size_t>(numFrames) * 2;
    if (mMonitoringBuffer.availableToWrite() < needed) return;

    const float monitorVolume = mMonitoringVolume.load(std::memory_order_acquire);
    if (monitorVolume == 1.0f) {
        mMonitoringBuffer.write(inputData, needed);
        return;
    }

    // A heap buffer on what production treats as the audio thread. Allowed here
    // and nowhere else: this file never ships, and sizing a member from prepare()
    // would make the double carry lifecycle state it otherwise does not have.
    std::vector<float> scaled(needed);
    for (size_t i = 0; i < needed; ++i) {
        scaled[i] = inputData[i] * monitorVolume;
    }
    mMonitoringBuffer.write(scaled.data(), needed);
}

bool InputNode::createInputStream() { return false; }
void InputNode::closeInputStream() {}
