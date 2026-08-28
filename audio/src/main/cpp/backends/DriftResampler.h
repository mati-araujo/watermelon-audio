/**
 * DriftResampler.h
 *
 * Small linear resampler for SplitBackend clock reconciliation.
 *
 * 🔴 NO tiene perilla de deriva en ppm, y NO es un olvido (MINI-011). Tuvo una
 * —`setDriftCorrection(float ppm)`— que jamas llamo nadie en produccion, y se
 * borro al medir por que: **la deriva de reloj ya se corrige, y por otro lado**.
 * `ClockController::getAdjustedFrameCount()` la absorbe cambiando cuantos frames
 * van por paquete USB (lo llama `UsbTransferManager.cpp` en el camino real), asi
 * que los frames que llegan aca ya vienen rate-matched. Alimentarle el ppm de
 * `ClockController::getDriftPpm()` —que vive bajo un encabezado que dice
 * "Monitoring / Debug"— corregiria DOS VECES la misma deriva: agregaria error.
 *
 * Lo que si corrige esta clase es el desajuste NOMINAL de rates (44100 in contra
 * 48000 out), y eso entra por `configure(sourceRate, targetRate)`.
 */

#pragma once

#include <array>
#include <cstdint>

namespace watermelon_audio {

class DriftResampler {
public:
    DriftResampler(float sourceRateHz = 48000.0f, float targetRateHz = 48000.0f);

    void configure(float sourceRateHz, float targetRateHz);
    void reset();

    int process(
        const float* input,
        int inFrames,
        int channels,
        float* output,
        int outCapacityFrames);

    float ratio() const { return mStep; }

private:
    static constexpr int kMaxChannels = 8;

    float mSourceRate = 48000.0f;
    float mTargetRate = 48000.0f;
    float mStep = 1.0f;
    double mFractionalPos = 0.0;
    std::array<float, kMaxChannels> mHistory{};

    void updateStep();
};

} // namespace watermelon_audio
