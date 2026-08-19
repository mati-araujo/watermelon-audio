#include "SplitBackend.h"

#include <algorithm>
#include <cstring>

namespace watermelon_audio {

SplitBackend::SplitBackend(IAudioBackend& inputSource, IAudioBackend& outputSink)
    : mInputSource(inputSource)
    , mOutputSink(outputSink)
    , mInputCallback(*this)
    , mOutputCallback(*this) {
    prepareBuffers();
}

SplitBackend::~SplitBackend() {
    stop();
}

BackendResult SplitBackend::start() {
    std::lock_guard<std::mutex> lock(mLifecycleMutex);

    if (mRunning.load(std::memory_order_acquire)) {
        return BackendResult::ERROR_ALREADY_RUNNING;
    }
    if (!mUserCallback) {
        return BackendResult::ERROR_NOT_INITIALIZED;
    }

    applyConfig();
    if (!validateEndpointContracts()) {
        return BackendResult::ERROR_INVALID_CONFIG;
    }

    prepareBuffers();
    if (mInputBridge) {
        mInputBridge->clear();
    }
    mDriftResampler.configure(
        static_cast<float>(mInputSource.getStreamInfo().sampleRate),
        static_cast<float>(mOutputSink.getStreamInfo().sampleRate));
    mBridgeUnderruns.store(0, std::memory_order_release);
    mBridgeOverruns.store(0, std::memory_order_release);

    mInputSource.setCallback(&mInputCallback);
    mOutputSink.setCallback(&mOutputCallback);

    BackendResult result = mInputSource.start();
    if (result != BackendResult::OK) {
        return result;
    }

    result = mOutputSink.start();
    if (result != BackendResult::OK) {
        mInputSource.stop();
        return result;
    }

    mRunning.store(true, std::memory_order_release);
    return BackendResult::OK;
}

void SplitBackend::stop() {
    std::lock_guard<std::mutex> lock(mLifecycleMutex);

    if (!mRunning.load(std::memory_order_acquire)) {
        return;
    }

    mOutputSink.stop();
    mInputSource.stop();
    mRunning.store(false, std::memory_order_release);
}

void SplitBackend::pause() {
    std::lock_guard<std::mutex> lock(mLifecycleMutex);
    mOutputSink.pause();
    mInputSource.pause();
}

void SplitBackend::resume() {
    std::lock_guard<std::mutex> lock(mLifecycleMutex);
    mInputSource.resume();
    mOutputSink.resume();
}

void SplitBackend::setCallback(IAudioCallback* callback) {
    std::lock_guard<std::mutex> lock(mLifecycleMutex);
    mUserCallback = callback;
}

void SplitBackend::setSampleRate(int sampleRate) {
    std::lock_guard<std::mutex> lock(mLifecycleMutex);
    if (sampleRate > 0) {
        mSampleRate = sampleRate;
    }
}

void SplitBackend::setBufferSize(int framesPerBuffer) {
    std::lock_guard<std::mutex> lock(mLifecycleMutex);
    if (framesPerBuffer > 0) {
        mFramesPerBuffer = framesPerBuffer;
        if (!mRunning.load(std::memory_order_acquire)) {
            prepareBuffers();
        }
    }
}

void SplitBackend::setFullDuplexEnabled(bool enable) {
    // Sin mLifecycleMutex: start() lo retiene mientras arranca los dos
    // sub-backends, y este setter no puede quedarse esperando eso. Ver el
    // comentario equivalente en CoreAudioBackend.
    mFullDuplexEnabled.store(enable, std::memory_order_release);
}

StreamInfo SplitBackend::getStreamInfo() const {
    StreamInfo info = mOutputSink.getStreamInfo();
    const StreamInfo inputInfo = mInputSource.getStreamInfo();
    info.backendType = BackendType::SPLIT;
    info.isFullDuplex = true;
    info.inputLatencyMs = inputInfo.inputLatencyMs + inputInfo.outputLatencyMs;
    info.outputLatencyMs = mOutputSink.getOutputLatencyMs();
    info.deviceName = "Split";
    return info;
}

bool SplitBackend::isRunning() const {
    return mRunning.load(std::memory_order_acquire);
}

float SplitBackend::getOutputLatencyMs() const {
    return mOutputSink.getOutputLatencyMs();
}

float SplitBackend::getInputLatencyMs() const {
    return mInputSource.getInputLatencyMs() + mInputSource.getOutputLatencyMs();
}

bool SplitBackend::supportsPause() const {
    return mInputSource.supportsPause() && mOutputSink.supportsPause();
}

BackendEndpointCapabilities SplitBackend::getEndpointCapabilities() const {
    BackendEndpointCapabilities caps;
    caps.roles = BackendStreamRole::FULL_DUPLEX;
    caps.hasInputSourceContract = true;
    caps.callbackCarriesInput = true;
    caps.drivesUserCallback = true;
    return caps;
}

bool SplitBackend::validateEndpointContracts() const {
    const auto inputCaps = mInputSource.getEndpointCapabilities();
    const auto outputCaps = mOutputSink.getEndpointCapabilities();

    return hasBackendRole(inputCaps.roles, BackendStreamRole::INPUT_SOURCE) &&
           inputCaps.hasInputSourceContract &&
           hasBackendRole(outputCaps.roles, BackendStreamRole::OUTPUT_SINK) &&
           outputCaps.drivesUserCallback;
}

void SplitBackend::prepareBuffers() {
    const int safeFrames = std::max(mFramesPerBuffer, kDefaultFramesPerBuffer);
    const int safeRate = std::max(mSampleRate, kDefaultSampleRate);
    const size_t callbackSamples = static_cast<size_t>(safeFrames * kChannelCount);
    const size_t bridgeSamples = static_cast<size_t>(
        ((safeRate * kBridgeBufferMs) / 1000) * kChannelCount);

    mInputBridge = std::make_unique<LockFreeRingBuffer>(
        std::max(bridgeSamples, callbackSamples * 4 + 1));
    mUserInputBuffer.assign(callbackSamples, 0.0f);
    mResampledInputBuffer.assign(callbackSamples * 2, 0.0f);
}

void SplitBackend::applyConfig() {
    mInputSource.setSampleRate(mSampleRate);
    mInputSource.setBufferSize(mFramesPerBuffer);
    mInputSource.setFullDuplexEnabled(true);

    mOutputSink.setSampleRate(mSampleRate);
    mOutputSink.setBufferSize(mFramesPerBuffer);
    mOutputSink.setFullDuplexEnabled(false);
}

IAudioCallback::Result SplitBackend::handleInput(
    float* outputData,
    const float* inputData,
    int32_t numFrames) {

    if (outputData && numFrames > 0) {
        std::memset(outputData, 0, static_cast<size_t>(numFrames * kChannelCount) * sizeof(float));
    }

    if (!inputData || numFrames <= 0 || !mInputBridge) {
        return IAudioCallback::Result::CONTINUE;
    }

    const size_t inputSamples = static_cast<size_t>(numFrames * kChannelCount);
    if (mResampledInputBuffer.size() < inputSamples * 2) {
        mBridgeOverruns.fetch_add(1, std::memory_order_relaxed);
        return IAudioCallback::Result::CONTINUE;
    }

    const int producedFrames = mDriftResampler.process(
        inputData,
        numFrames,
        kChannelCount,
        mResampledInputBuffer.data(),
        static_cast<int>(mResampledInputBuffer.size() / kChannelCount));

    if (producedFrames <= 0) {
        return IAudioCallback::Result::CONTINUE;
    }

    const size_t producedSamples = static_cast<size_t>(producedFrames * kChannelCount);
    if (!mInputBridge->write(mResampledInputBuffer.data(), producedSamples)) {
        mBridgeOverruns.fetch_add(1, std::memory_order_relaxed);
    }

    return IAudioCallback::Result::CONTINUE;
}

IAudioCallback::Result SplitBackend::handleOutput(float* outputData, int32_t numFrames) {
    if (!mUserCallback || !outputData || numFrames <= 0) {
        return IAudioCallback::Result::CONTINUE;
    }

    const size_t samples = static_cast<size_t>(numFrames * kChannelCount);
    const float* inputPtr = nullptr;

    if (mInputBridge && mUserInputBuffer.size() >= samples) {
        if (!mInputBridge->read(mUserInputBuffer.data(), samples)) {
            mBridgeUnderruns.fetch_add(1, std::memory_order_relaxed);
        }
        inputPtr = mUserInputBuffer.data();
    }

    return mUserCallback->onAudioReady(outputData, inputPtr, numFrames);
}

void SplitBackend::forwardError(BackendError error) {
    if (mUserCallback) {
        mUserCallback->onBackendError(error);
    }
}

IAudioCallback::Result SplitBackend::InputCallback::onAudioReady(
    float* outputData,
    const float* inputData,
    int32_t numFrames) {
    return mParent.handleInput(outputData, inputData, numFrames);
}

void SplitBackend::InputCallback::onBackendError(BackendError error) {
    mParent.forwardError(error);
}

void SplitBackend::InputCallback::onStreamConfigChanged(const StreamInfo& newInfo) {
    // REQ-001 S1 (1.16). Esto era `(void)newInfo;` — la configuracion del stream
    // de CAPTURA se descartaba aca y nada aguas abajo podia enterarse de a que
    // rate estaba entrando el audio. Se reenvia por el hook de entrada, no por
    // `onStreamConfigChanged`, porque en un backend partido ese ya transporta el
    // rate de SALIDA y son dos numeros distintos.
    if (mParent.mUserCallback) {
        mParent.mUserCallback->onInputStreamConfigChanged(newInfo);
    }
}

IAudioCallback::Result SplitBackend::OutputCallback::onAudioReady(
    float* outputData,
    const float* inputData,
    int32_t numFrames) {
    (void)inputData;
    return mParent.handleOutput(outputData, numFrames);
}

void SplitBackend::OutputCallback::onBackendError(BackendError error) {
    mParent.forwardError(error);
}

void SplitBackend::OutputCallback::onStreamConfigChanged(const StreamInfo& newInfo) {
    if (mParent.mUserCallback) {
        mParent.mUserCallback->onStreamConfigChanged(newInfo);
    }
}

} // namespace watermelon_audio
