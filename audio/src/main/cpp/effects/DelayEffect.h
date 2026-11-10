#ifndef DELAY_EFFECT_H
#define DELAY_EFFECT_H

#include "Effect.h"
#include "../dsp/ParameterSmoother.h"
#include <atomic>
#include <vector>

class DelayEffect : public Effect {
public:
    /// Largo de cada linea (L y R), en muestras: el techo del tiempo, de 2000 ms hasta 96 kHz.
    static constexpr int kMaxDelaySamples = 96000 * 2;

    DelayEffect();

    void setDelayTime(float dt);
    void setFeedback(float fb);
    void setWet(float w);
    void setBpm(float b) override;
    void setNoteDivision(float nd);
    void setSync(bool s);

    void process(float* input, float* output, int numFrames) override;
    void setParam(int paramId, float value) override;
    float getParam(int paramId) override;
    void setSampleRate(int sampleRate) override;
    void reset() override;

    /// Clava el suavizado del tiempo en su objetivo SIN vaciar las lineas (reset() hace las dos).
    void snapDelayTimeToTarget();

private:
    int mSampleRate = 48000;

    std::atomic<float> delayTime{250.0f};
    std::atomic<float> feedback{0.4f};
    std::atomic<float> wet{0.3f};
    std::atomic<float> bpm{120.0f};
    std::atomic<float> noteDivision{4.0f};
    std::atomic<bool> sync{false};

    std::atomic<float> delaySamplesTarget{0.0f};
    ParameterSmoother delaySamplesSmoother;
    std::vector<float> bufferL;
    std::vector<float> bufferR;
    std::atomic<int> writePos{0};

    void updateDelaySamples();
};

#endif // DELAY_EFFECT_H
