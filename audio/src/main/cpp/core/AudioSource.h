#pragma once
#include <cstdint>
#include <atomic>
#include <cmath>

// Constante PI para cálculos
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class AudioSource {
public:
    virtual ~AudioSource() = default;

    // Configuración inicial
    virtual void setSampleRate(int32_t sampleRate) {
        mSampleRate = sampleRate;
    }

    // Parámetros en tiempo real (Thread-safe)
    void setParameters(float frequency, float amplitude) {
        mFrequency.store(frequency);
        mAmplitude.store(amplitude);
    }

    // Getters for current parameters (Thread-safe)
    float getFrequency() const {
        return mFrequency.load(std::memory_order_acquire);
    }

    float getAmplitude() const {
        return mAmplitude.load(std::memory_order_acquire);
    }

    // Reset phase to prevent clicks when restarting
    void resetPhase() {
        mPhase = 0.0f;
    }

    // Método puro que cada oscilador debe implementar
    virtual void render(float* audioData, int32_t numFrames) = 0;

protected:
    // Datos protegidos accesibles por los hijos
    std::atomic<float> mFrequency{440.0f};
    std::atomic<float> mAmplitude{0.0f};
    int32_t mSampleRate = 48000;
    float mPhase = 0.0f;

    // Utilidad para avanzar fase
    void advancePhase(int frames) {
        // Este cálculo lo hacen casi todos, pero lo dejamos flexible
        // Normalmente se hace per-sample, así que lo dejamos inline en render
    }
};