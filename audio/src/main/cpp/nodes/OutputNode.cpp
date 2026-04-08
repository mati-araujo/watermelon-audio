#include "OutputNode.h"
#include <algorithm>
#include <cmath>

OutputNode::OutputNode()
    : mSoftClipper(SoftClipper::Type::TANH)
    , mDitherer(16)
    , mDCBlocker(0.995f) {}

void OutputNode::prepare(int sampleRate, int maxBlockSize) {
    AudioNode::prepare(sampleRate, maxBlockSize);

    // Pre-alocar buffer de salida final (interleaved estéreo)
    mFinalOutputBuffer.resize(maxBlockSize * 2, 0.0f);

    // Configurar coeficiente RMS para media exponencial
    // Tiempo de integración ~300ms para metering suave
    float rmsTimeMs = 300.0f;
    mRmsCoeff = std::exp(-1.0f / (rmsTimeMs * 0.001f * static_cast<float>(sampleRate)));
}

void OutputNode::reset() {
    AudioNode::reset();

    std::fill(mFinalOutputBuffer.begin(), mFinalOutputBuffer.end(), 0.0f);
    mLastNumFrames = 0;

    mDCBlocker.reset();
    mDitherer.reset();

    mPeakL.store(0.0f, std::memory_order_release);
    mPeakR.store(0.0f, std::memory_order_release);
    mRmsL.store(0.0f, std::memory_order_release);
    mRmsR.store(0.0f, std::memory_order_release);

    mFadeState.store(FadeState::NONE, std::memory_order_release);
    mFadePosition.store(1.0f, std::memory_order_release);
}

void OutputNode::process(AudioBuffer& inputBuffer, int numFrames) {
    // Convertir de formato por canales a interleaved
    const float* inLeft = inputBuffer.getReadPointer(0);
    const float* inRight = inputBuffer.getReadPointer(1);

    for (int i = 0; i < numFrames; ++i) {
        mFinalOutputBuffer[i * 2] = inLeft[i];
        mFinalOutputBuffer[i * 2 + 1] = inRight[i];
    }

    // 1. DC Blocking
    mDCBlocker.process(mFinalOutputBuffer.data(), numFrames);

    // 2. Aplicar master volume
    float volume = mMasterVolume.load(std::memory_order_acquire);
    bool muted = mMasterMute.load(std::memory_order_acquire);

    if (muted) {
        volume = 0.0f;
    }

    // 3. Procesar fade
    processFade(mFinalOutputBuffer.data(), numFrames);

    // Aplicar volumen
    for (int i = 0; i < numFrames * 2; ++i) {
        mFinalOutputBuffer[i] *= volume;
    }

    // 4. Soft clipping (protección)
    if (mLimiterEnabled.load(std::memory_order_acquire)) {
        mSoftClipper.processStereo(mFinalOutputBuffer.data(), numFrames);
    }

    // 5. Dithering (para conversión a int16)
    mDitherer.processStereo(mFinalOutputBuffer.data(), numFrames);

    // 6. Actualizar meters
    updateMeters(mFinalOutputBuffer.data(), numFrames);

    // Copiar también al buffer de salida del nodo (formato por canales)
    float* outLeft = mBuffer.getWritePointer(0);
    float* outRight = mBuffer.getWritePointer(1);

    for (int i = 0; i < numFrames; ++i) {
        outLeft[i] = mFinalOutputBuffer[i * 2];
        outRight[i] = mFinalOutputBuffer[i * 2 + 1];
    }

    mLastNumFrames = numFrames;
}

void OutputNode::processFade(float* buffer, int numFrames) {
    FadeState state = mFadeState.load(std::memory_order_acquire);
    if (state == FadeState::NONE) return;

    float position = mFadePosition.load(std::memory_order_acquire);
    float increment = mFadeIncrement.load(std::memory_order_acquire);
    int remaining = mFadeRemainingFrames.load(std::memory_order_acquire);

    for (int i = 0; i < numFrames && remaining > 0; ++i) {
        // Aplicar curva de fade suave (coseno)
        float fadeGain = 0.5f * (1.0f - std::cos(position * M_PI));

        buffer[i * 2] *= fadeGain;
        buffer[i * 2 + 1] *= fadeGain;

        position += increment;
        position = std::clamp(position, 0.0f, 1.0f);
        remaining--;
    }

    mFadePosition.store(position, std::memory_order_release);
    mFadeRemainingFrames.store(remaining, std::memory_order_release);

    // Verificar si el fade terminó
    if (remaining <= 0) {
        mFadeState.store(FadeState::NONE, std::memory_order_release);
        // Establecer posición final
        if (state == FadeState::FADING_IN) {
            mFadePosition.store(1.0f, std::memory_order_release);
        } else {
            mFadePosition.store(0.0f, std::memory_order_release);
        }
    }
}

void OutputNode::updateMeters(const float* buffer, int numFrames) {
    float peakL = 0.0f;
    float peakR = 0.0f;
    float sumSquaredL = 0.0f;
    float sumSquaredR = 0.0f;

    for (int i = 0; i < numFrames; ++i) {
        float sampleL = buffer[i * 2];
        float sampleR = buffer[i * 2 + 1];

        float absL = std::abs(sampleL);
        float absR = std::abs(sampleR);

        if (absL > peakL) peakL = absL;
        if (absR > peakR) peakR = absR;

        sumSquaredL += sampleL * sampleL;
        sumSquaredR += sampleR * sampleR;
    }

    // Peak con decay lento
    float currentPeakL = mPeakL.load(std::memory_order_acquire);
    float currentPeakR = mPeakR.load(std::memory_order_acquire);

    // Decay de ~20dB/segundo (approx)
    float peakDecay = std::pow(0.9995f, numFrames);
    currentPeakL *= peakDecay;
    currentPeakR *= peakDecay;

    if (peakL > currentPeakL) currentPeakL = peakL;
    if (peakR > currentPeakR) currentPeakR = peakR;

    mPeakL.store(currentPeakL, std::memory_order_release);
    mPeakR.store(currentPeakR, std::memory_order_release);

    // RMS con media exponencial
    float rmsL = std::sqrt(sumSquaredL / static_cast<float>(numFrames));
    float rmsR = std::sqrt(sumSquaredR / static_cast<float>(numFrames));

    float currentRmsL = mRmsL.load(std::memory_order_acquire);
    float currentRmsR = mRmsR.load(std::memory_order_acquire);

    currentRmsL = currentRmsL * mRmsCoeff + rmsL * (1.0f - mRmsCoeff);
    currentRmsR = currentRmsR * mRmsCoeff + rmsR * (1.0f - mRmsCoeff);

    mRmsL.store(currentRmsL, std::memory_order_release);
    mRmsR.store(currentRmsR, std::memory_order_release);
}

void OutputNode::setMasterVolume(float volume) {
    mMasterVolume.store(std::clamp(volume, 0.0f, 2.0f), std::memory_order_release);
}

float OutputNode::getMasterVolume() const {
    return mMasterVolume.load(std::memory_order_acquire);
}

void OutputNode::setMasterMute(bool mute) {
    mMasterMute.store(mute, std::memory_order_release);
}

bool OutputNode::isMasterMuted() const {
    return mMasterMute.load(std::memory_order_acquire);
}

void OutputNode::setLimiterEnabled(bool enabled) {
    mLimiterEnabled.store(enabled, std::memory_order_release);
}

bool OutputNode::isLimiterEnabled() const {
    return mLimiterEnabled.load(std::memory_order_acquire);
}

float OutputNode::getPeakLevel(int channel) const {
    if (channel == 0) return mPeakL.load(std::memory_order_acquire);
    if (channel == 1) return mPeakR.load(std::memory_order_acquire);
    return 0.0f;
}

float OutputNode::getRMSLevel(int channel) const {
    if (channel == 0) return mRmsL.load(std::memory_order_acquire);
    if (channel == 1) return mRmsR.load(std::memory_order_acquire);
    return 0.0f;
}

void OutputNode::startFadeIn(float durationMs) {
    if (durationMs <= 0.0f) {
        mFadePosition.store(1.0f, std::memory_order_release);
        mFadeState.store(FadeState::NONE, std::memory_order_release);
        return;
    }

    int totalFrames = static_cast<int>((durationMs / 1000.0f) * static_cast<float>(mSampleRate));
    float increment = 1.0f / static_cast<float>(totalFrames);

    mFadePosition.store(0.0f, std::memory_order_release);
    mFadeIncrement.store(increment, std::memory_order_release);
    mFadeTotalFrames.store(totalFrames, std::memory_order_release);
    mFadeRemainingFrames.store(totalFrames, std::memory_order_release);
    mFadeState.store(FadeState::FADING_IN, std::memory_order_release);
}

void OutputNode::startFadeOut(float durationMs) {
    if (durationMs <= 0.0f) {
        mFadePosition.store(0.0f, std::memory_order_release);
        mFadeState.store(FadeState::NONE, std::memory_order_release);
        return;
    }

    int totalFrames = static_cast<int>((durationMs / 1000.0f) * static_cast<float>(mSampleRate));
    float increment = -1.0f / static_cast<float>(totalFrames);

    mFadePosition.store(1.0f, std::memory_order_release);
    mFadeIncrement.store(increment, std::memory_order_release);
    mFadeTotalFrames.store(totalFrames, std::memory_order_release);
    mFadeRemainingFrames.store(totalFrames, std::memory_order_release);
    mFadeState.store(FadeState::FADING_OUT, std::memory_order_release);
}

bool OutputNode::isFading() const {
    return mFadeState.load(std::memory_order_acquire) != FadeState::NONE;
}

float OutputNode::getFadeProgress() const {
    int remaining = mFadeRemainingFrames.load(std::memory_order_acquire);
    int total = mFadeTotalFrames.load(std::memory_order_acquire);
    if (total <= 0) return 1.0f;
    return 1.0f - (static_cast<float>(remaining) / static_cast<float>(total));
}

const float* OutputNode::getFinalOutputBuffer() const {
    return mFinalOutputBuffer.data();
}

int OutputNode::getFinalOutputNumFrames() const {
    return mLastNumFrames;
}
