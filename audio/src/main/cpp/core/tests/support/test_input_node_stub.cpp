/**
 * test_input_node_stub.cpp — TEST DOUBLE, host test build only.
 *
 * A link-time stand-in for nodes/InputNode.cpp, which cannot be compiled on the
 * host: its capture adapter inherits oboe::AudioStreamDataCallback and Oboe is
 * Android-only. Without these definitions the core test binary fails to link on
 * the ~2 dozen InputNode symbols that AudioEngine references.
 *
 * NOT a behavioural double. Every method here is the cheapest thing that
 * satisfies its signature. The suite in this directory never attaches an
 * InputNode to the engine, so none of this is reached — which is the point: if
 * a future test needs real input behaviour, it must build a proper fake rather
 * than trusting these bodies.
 *
 * The header is Oboe-free by design (WA-2.0), so the class definition below is
 * the production one; only the implementation is swapped.
 */

#include "nodes/InputNode.h"

#include <algorithm>

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

InputNode::~InputNode() = default;

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
    return mMonitoringEnabled.load(std::memory_order_acquire);
}

void InputNode::setMonitoringVolume(float volume) {
    mMonitoringVolume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_release);
}

float InputNode::getMonitoringVolume() const {
    return mMonitoringVolume.load(std::memory_order_acquire);
}

int InputNode::getMonitoringSamples(float* outputBuffer, int numFrames) {
    (void)outputBuffer;
    (void)numFrames;
    return 0;
}

void InputNode::feedExternalInput(const float* inputData, int numFrames) {
    (void)inputData;
    (void)numFrames;
}

bool InputNode::createInputStream() { return false; }
void InputNode::closeInputStream() {}
