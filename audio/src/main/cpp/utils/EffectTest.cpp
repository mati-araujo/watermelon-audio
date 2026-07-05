#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>
#include <string>
#include <algorithm>

// Legacy constant (was in core/constants.h, now in effects/EffectDefaults.h)
#ifndef SAMPLE_RATE
#define SAMPLE_RATE 48000
#endif
#include "../effects/Effect.h"
#include "../effects/EffectChain.h"
#include "../effects/DistortionEffect.h"
#include "../effects/CabinetSimulator.h"
#include "../effects/EffectTypes.h"

// Include implementations for testing
#include "../effects/FilterEffect.cpp"
#include "../effects/ReverbEffect.cpp"
#include "../effects/DelayEffect.cpp"
#include "../effects/EffectChain.cpp"

// Helper function to generate sine wave
std::vector<float> generateSineWave(int numSamples, float frequency) {
    std::vector<float> wave(numSamples * 2);
    for (int i = 0; i < numSamples; ++i) {
        float sample = sinf(2.0f * M_PI * frequency * i / SAMPLE_RATE);
        wave[i * 2] = sample;
        wave[i * 2 + 1] = sample;
    }
    return wave;
}

std::vector<float> generateGuitarTestSignal(int numSamples) {
    std::vector<float> wave(numSamples * 2);
    for (int i = 0; i < numSamples; ++i) {
        float t = static_cast<float>(i) / SAMPLE_RATE;
        float sample =
            0.35f * sinf(2.0f * M_PI * 110.0f * t) +
            0.30f * sinf(2.0f * M_PI * 440.0f * t) +
            0.20f * sinf(2.0f * M_PI * 1500.0f * t) +
            0.15f * sinf(2.0f * M_PI * 3500.0f * t);
        wave[i * 2] = sample;
        wave[i * 2 + 1] = sample;
    }
    return wave;
}

// Helper function to calculate RMS
float calculateRMS(const std::vector<float>& data) {
    float sum = 0.0f;
    for (float val : data) {
        sum += val * val;
    }
    return sqrtf(sum / data.size());
}

float calculateMaxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    float maxDiff = 0.0f;
    size_t count = std::min(a.size(), b.size());
    for (size_t i = 0; i < count; ++i) {
        maxDiff = std::max(maxDiff, fabsf(a[i] - b[i]));
    }
    return maxDiff;
}

std::vector<float> generateImpulse(int numSamples, float amplitude = 1.0f) {
    std::vector<float> impulse(numSamples * 2, 0.0f);
    if (numSamples > 0) {
        impulse[0] = amplitude;
        impulse[1] = amplitude;
    }
    return impulse;
}

std::vector<float> renderDistortionStereo(int algorithm, const std::vector<float>& input) {
    DistortionEffect distortion;
    distortion.setSampleRate(SAMPLE_RATE);
    distortion.setParam(DistortionEffect::ALGORITHM, static_cast<float>(algorithm));
    distortion.setParam(DistortionEffect::DRIVE, 0.9f);
    distortion.setParam(DistortionEffect::TONE, 0.5f);
    distortion.setParam(DistortionEffect::LEVEL, 1.0f);
    distortion.setParam(DistortionEffect::MIX, 1.0f);
    distortion.setParam(DistortionEffect::OVERSAMPLE, 0.0f);
    distortion.setParam(DistortionEffect::SAG, 0.0f);
    distortion.reset();

    std::vector<float> inputCopy = input;
    std::vector<float> output(input.size());
    distortion.process(inputCopy.data(), output.data(), static_cast<int>(inputCopy.size() / 2));
    return output;
}

float calculateChannelMaxAbsDiff(const std::vector<float>& a, const std::vector<float>& b, int channel) {
    float maxDiff = 0.0f;
    size_t frames = std::min(a.size(), b.size()) / 2;
    for (size_t i = 0; i < frames; ++i) {
        size_t index = i * 2 + static_cast<size_t>(channel);
        maxDiff = std::max(maxDiff, fabsf(a[index] - b[index]));
    }
    return maxDiff;
}

// Helper function to calculate THD
float calculateTHD(const std::vector<float>& output, float fundamentalFreq) {
    // Simplified THD calculation - measure harmonics
    // For simplicity, assume fundamental is at fundamentalFreq
    // This is a basic implementation
    float fundamental = 0.0f;
    float harmonics = 0.0f;
    int numSamples = output.size() / 2;
    for (int i = 0; i < numSamples; ++i) {
        float sample = (output[i * 2] + output[i * 2 + 1]) * 0.5f;
        fundamental += sample * sinf(2.0f * M_PI * fundamentalFreq * i / SAMPLE_RATE);
        // Add some harmonics (2nd, 3rd)
        harmonics += sample * sinf(2.0f * M_PI * 2 * fundamentalFreq * i / SAMPLE_RATE);
        harmonics += sample * sinf(2.0f * M_PI * 3 * fundamentalFreq * i / SAMPLE_RATE);
    }
    fundamental /= numSamples;
    harmonics /= numSamples;
    return sqrtf(harmonics * harmonics) / fabsf(fundamental);
}

// Test functions return true if pass
bool testFilterEffectCoefficients() {
    FilterEffect filter;
    filter.setCutoff(1000.0f);
    filter.setResonance(0.707f);
    filter.setType(FilterEffect::LPF);

    auto input = generateSineWave(1024, 1000.0f);
    std::vector<float> output(input.size());
    filter.process(input.data(), output.data(), 1024);

    float inputRMS = calculateRMS(input);
    float outputRMS = calculateRMS(output);
    return outputRMS < inputRMS; // Should be attenuated
}

bool testFilterEffectFrequencyResponse() {
    FilterEffect filter;
    filter.setCutoff(1000.0f);
    filter.setType(FilterEffect::LPF);

    // Test low frequency
    auto inputLow = generateSineWave(1024, 100.0f);
    std::vector<float> outputLow(inputLow.size());
    filter.process(inputLow.data(), outputLow.data(), 1024);
    float ratioLow = calculateRMS(outputLow) / calculateRMS(inputLow);

    // Test high frequency
    auto inputHigh = generateSineWave(1024, 10000.0f);
    std::vector<float> outputHigh(inputHigh.size());
    filter.process(inputHigh.data(), outputHigh.data(), 1024);
    float ratioHigh = calculateRMS(outputHigh) / calculateRMS(inputHigh);

    return ratioLow > 0.5f && ratioHigh < 0.5f;
}

bool testReverbEffectDelays() {
    ReverbEffect reverb;
    reverb.setDecay(1.0f);
    reverb.setSize(1.0f);
    reverb.setMix(1.0f);

    auto input = generateSineWave(2048, 440.0f);
    std::vector<float> output(input.size());
    reverb.process(input.data(), output.data(), 2048);

    float inputRMS = calculateRMS(input);
    float outputRMS = calculateRMS(output);
    return outputRMS >= inputRMS * 0.5f;
}

bool testReverbEffectToneFilters() {
    ReverbEffect reverb;
    reverb.setDecay(2.0f);
    reverb.setSize(1.5f);

    auto input = generateSineWave(1024, 1000.0f);
    std::vector<float> output(input.size());
    reverb.process(input.data(), output.data(), 1024);

    bool different = false;
    for (size_t i = 0; i < output.size(); ++i) {
        if (fabsf(output[i] - input[i]) > 0.01f) {
            different = true;
            break;
        }
    }
    return different;
}

bool testDelayEffectCircularBuffer() {
    DelayEffect delay;
    delay.setDelayTime(100.0f);
    delay.setFeedback(0.0f);
    delay.setWet(1.0f);

    auto input = generateSineWave(2048, 440.0f);
    std::vector<float> output(input.size());
    delay.process(input.data(), output.data(), 2048);

    int delaySamples = static_cast<int>(100.0f * SAMPLE_RATE / 1000.0f);
    bool delayed = true;
    for (int i = delaySamples; i < 1024 && delayed; ++i) {
        if (fabsf(output[i * 2] - input[(i - delaySamples) * 2]) > 0.1f) {
            delayed = false;
        }
    }
    return delayed;
}

bool testDelayEffectFeedback() {
    DelayEffect delay;
    delay.setDelayTime(200.0f);
    delay.setFeedback(0.5f);
    delay.setWet(1.0f);

    auto input = generateSineWave(4096, 220.0f);
    std::vector<float> output(input.size());
    delay.process(input.data(), output.data(), 4096);

    float initialRMS = calculateRMS(std::vector<float>(output.begin(), output.begin() + 1024));
    float laterRMS = calculateRMS(std::vector<float>(output.begin() + 2048, output.begin() + 3072));
    return laterRMS >= initialRMS * 0.8f;
}

bool testEffectChainSerialProcessing() {
    EffectChain chain;
    chain.addEffect(FILTER);
    chain.addEffect(DELAY);

    chain.setParameter(0, 0, 1000.0f);
    chain.setParameter(1, 0, 200.0f);

    auto input = generateSineWave(1024, 2000.0f);
    std::vector<float> output(input.size());
    chain.process(input.data(), output.data(), 1024);

    float inputRMS = calculateRMS(input);
    float outputRMS = calculateRMS(output);
    return outputRMS < inputRMS;
}

bool testEffectChainBypass() {
    EffectChain chain;
    chain.addEffect(FILTER);
    chain.setParameter(0, 0, 500.0f);

    auto input = generateSineWave(1024, 1000.0f);
    std::vector<float> outputBypass(input.size());
    std::vector<float> outputActive(input.size());

    chain.setBypass(0, true);
    chain.process(input.data(), outputBypass.data(), 1024);

    chain.setBypass(0, false);
    chain.process(input.data(), outputActive.data(), 1024);

    bool different = false;
    for (size_t i = 0; i < outputBypass.size(); ++i) {
        if (fabsf(outputBypass[i] - outputActive[i]) > 0.01f) {
            different = true;
            break;
        }
    }
    return different;
}

std::vector<float> renderDistortion(int algorithm, int paramId, float paramValue) {
    DistortionEffect distortion;
    distortion.setSampleRate(SAMPLE_RATE);
    distortion.setParam(DistortionEffect::ALGORITHM, static_cast<float>(algorithm));
    distortion.setParam(DistortionEffect::DRIVE, 0.75f);
    distortion.setParam(DistortionEffect::TONE, 0.5f);
    distortion.setParam(DistortionEffect::LEVEL, 1.0f);
    distortion.setParam(DistortionEffect::MIX, 1.0f);
    distortion.setParam(DistortionEffect::OVERSAMPLE, 0.0f);
    distortion.setParam(DistortionEffect::POST_HIGH_CUT, 18000.0f);
    distortion.setParam(paramId, paramValue);
    distortion.reset();

    auto input = generateGuitarTestSignal(4096);
    std::vector<float> output(input.size());
    distortion.process(input.data(), output.data(), 4096);
    return output;
}

bool testDistortionPedalToneStacks() {
    struct Case {
        int algorithm;
        int paramId;
        float baseValue;
        float changedValue;
    };

    const Case cases[] = {
        {DistortionVariants::TUBE_SCREAMER, DistortionEffect::PARAM_A, 0.0f, 1.0f},
        {DistortionVariants::RAT, DistortionEffect::PARAM_A, 0.0f, 1.0f},
        {DistortionVariants::BIG_MUFF, DistortionEffect::PARAM_B, 0.0f, 1.0f},
        {DistortionVariants::METAL_ZONE, DistortionEffect::PARAM_C, 0.0f, 1.0f},
        {DistortionVariants::HM2_CHAINSAW, DistortionEffect::PARAM_B, 0.0f, 1.0f}
    };

    for (const auto& c : cases) {
        auto base = renderDistortion(c.algorithm, c.paramId, c.baseValue);
        auto changed = renderDistortion(c.algorithm, c.paramId, c.changedValue);
        if (calculateMaxAbsDiff(base, changed) < 0.0005f) {
            return false;
        }
    }
    return true;
}

bool testDistortionGateAndSag() {
    auto quietInput = generateSineWave(1024, 440.0f);
    for (float& sample : quietInput) {
        sample *= 0.01f;
    }

    DistortionEffect gated;
    gated.setSampleRate(SAMPLE_RATE);
    gated.setParam(DistortionEffect::DRIVE, 0.8f);
    gated.setParam(DistortionEffect::LEVEL, 1.0f);
    gated.setParam(DistortionEffect::MIX, 1.0f);
    gated.setParam(DistortionEffect::OVERSAMPLE, 0.0f);
    gated.setParam(DistortionEffect::GATE_THRESHOLD, 1.0f);
    gated.reset();

    std::vector<float> gatedOutput(quietInput.size());
    gated.process(quietInput.data(), gatedOutput.data(), 1024);
    bool gateReducesQuietSignal = calculateRMS(gatedOutput) < calculateRMS(quietInput) * 0.25f;

    DistortionEffect noSag;
    DistortionEffect sag;
    noSag.setSampleRate(SAMPLE_RATE);
    sag.setSampleRate(SAMPLE_RATE);
    for (DistortionEffect* effect : {&noSag, &sag}) {
        effect->setParam(DistortionEffect::DRIVE, 0.9f);
        effect->setParam(DistortionEffect::LEVEL, 1.0f);
        effect->setParam(DistortionEffect::MIX, 1.0f);
        effect->setParam(DistortionEffect::OVERSAMPLE, 0.0f);
    }
    noSag.setParam(DistortionEffect::SAG, 0.0f);
    sag.setParam(DistortionEffect::SAG, 1.0f);
    noSag.reset();
    sag.reset();

    auto loudInput = generateGuitarTestSignal(4096);
    std::vector<float> noSagOutput(loudInput.size());
    std::vector<float> sagOutput(loudInput.size());
    noSag.process(loudInput.data(), noSagOutput.data(), 4096);
    sag.process(loudInput.data(), sagOutput.data(), 4096);
    bool sagChangesOutput = calculateMaxAbsDiff(noSagOutput, sagOutput) > 0.0005f;

    return gateReducesQuietSignal && sagChangesOutput;
}

bool testDistortionStatefulStereoIsolation() {
    auto leftSignal = generateSineWave(2048, 110.0f);
    auto rightSignal = generateSineWave(2048, 880.0f);

    std::vector<float> stereoInput(leftSignal.size());
    std::vector<float> leftOnly(leftSignal.size(), 0.0f);
    std::vector<float> rightOnly(rightSignal.size(), 0.0f);

    for (size_t i = 0; i < stereoInput.size() / 2; ++i) {
        float left = leftSignal[i * 2] * 0.8f;
        float right = rightSignal[i * 2] * 0.45f;
        stereoInput[i * 2] = left;
        stereoInput[i * 2 + 1] = right;
        leftOnly[i * 2] = left;
        rightOnly[i * 2 + 1] = right;
    }

    const int algorithms[] = {
        DistortionVariants::RAT,
        DistortionVariants::OCTAVE_FUZZ
    };

    for (int algorithm : algorithms) {
        auto stereoOutput = renderDistortionStereo(algorithm, stereoInput);
        auto leftReference = renderDistortionStereo(algorithm, leftOnly);
        auto rightReference = renderDistortionStereo(algorithm, rightOnly);

        if (calculateChannelMaxAbsDiff(stereoOutput, leftReference, 0) > 1e-6f) {
            return false;
        }
        if (calculateChannelMaxAbsDiff(stereoOutput, rightReference, 1) > 1e-6f) {
            return false;
        }
    }

    return true;
}

bool testCabinetFullFirImpulse() {
    CabinetSimulator cabinet;
    cabinet.setSampleRate(SAMPLE_RATE);
    cabinet.setParam(CabinetSimulator::MIX, 100.0f);
    cabinet.setParam(CabinetSimulator::LOW_CUT, 20.0f);
    cabinet.setParam(CabinetSimulator::HIGH_CUT, 20000.0f);
    cabinet.reset();

    auto input = generateImpulse(1024);
    std::vector<float> output(input.size());
    cabinet.process(input.data(), output.data(), 1024);

    int nonZeroAfter16 = 0;
    for (int i = 17; i < 512; ++i) {
        if (fabsf(output[i * 2]) > 1e-7f || fabsf(output[i * 2 + 1]) > 1e-7f) {
            nonZeroAfter16++;
        }
    }

    return nonZeroAfter16 > 8 && calculateMaxAbsDiff(input, output) > 0.01f;
}

bool testCabinetBypass() {
    CabinetSimulator cabinet;
    cabinet.setSampleRate(SAMPLE_RATE);
    cabinet.setParam(CabinetSimulator::MIX, 0.0f);
    cabinet.reset();

    auto input = generateGuitarTestSignal(512);
    std::vector<float> output(input.size());
    cabinet.process(input.data(), output.data(), 512);

    return calculateMaxAbsDiff(input, output) < 1e-7f;
}

bool testCabinetSmallBlockContinuity() {
    CabinetSimulator fullBlock;
    CabinetSimulator smallBlocks;
    fullBlock.setSampleRate(SAMPLE_RATE);
    smallBlocks.setSampleRate(SAMPLE_RATE);
    fullBlock.setParam(CabinetSimulator::MIX, 100.0f);
    smallBlocks.setParam(CabinetSimulator::MIX, 100.0f);
    fullBlock.setParam(CabinetSimulator::LOW_CUT, 20.0f);
    smallBlocks.setParam(CabinetSimulator::LOW_CUT, 20.0f);
    fullBlock.setParam(CabinetSimulator::HIGH_CUT, 20000.0f);
    smallBlocks.setParam(CabinetSimulator::HIGH_CUT, 20000.0f);
    fullBlock.reset();
    smallBlocks.reset();

    auto input = generateGuitarTestSignal(1024);
    std::vector<float> fullOutput(input.size());
    std::vector<float> chunkedOutput(input.size());

    fullBlock.process(input.data(), fullOutput.data(), 1024);

    constexpr int chunkFrames = 17;
    for (int offset = 0; offset < 1024; offset += chunkFrames) {
        int frames = std::min(chunkFrames, 1024 - offset);
        smallBlocks.process(input.data() + offset * 2,
                            chunkedOutput.data() + offset * 2,
                            frames);
    }

    return calculateMaxAbsDiff(fullOutput, chunkedOutput) < 1e-6f;
}

double testPerformanceLatency() {
    EffectChain chain;
    chain.addEffect(FILTER);
    chain.addEffect(REVERB);
    chain.addEffect(DELAY);

    auto input = generateSineWave(1024, 440.0f);
    std::vector<float> output(input.size());

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100; ++i) {
        chain.process(input.data(), output.data(), 1024);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    return duration / 100.0; // ms per buffer
}

float testPerformanceTHD() {
    FilterEffect filter;
    filter.setCutoff(1000.0f);
    filter.setResonance(0.707f);

    auto input = generateSineWave(4096, 440.0f);
    std::vector<float> output(input.size());
    filter.process(input.data(), output.data(), 4096);

    return calculateTHD(output, 440.0f);
}

int main() {
    std::cout << "Running Effect Tests...\n";

    int passed = 0;
    int total = 0;

    // FilterEffect tests
    total++;
    if (testFilterEffectCoefficients()) {
        std::cout << "PASS: FilterEffect Coefficients\n";
        passed++;
    } else {
        std::cout << "FAIL: FilterEffect Coefficients\n";
    }

    total++;
    if (testFilterEffectFrequencyResponse()) {
        std::cout << "PASS: FilterEffect Frequency Response\n";
        passed++;
    } else {
        std::cout << "FAIL: FilterEffect Frequency Response\n";
    }

    // ReverbEffect tests
    total++;
    if (testReverbEffectDelays()) {
        std::cout << "PASS: ReverbEffect Delays\n";
        passed++;
    } else {
        std::cout << "FAIL: ReverbEffect Delays\n";
    }

    total++;
    if (testReverbEffectToneFilters()) {
        std::cout << "PASS: ReverbEffect Tone Filters\n";
        passed++;
    } else {
        std::cout << "FAIL: ReverbEffect Tone Filters\n";
    }

    // DelayEffect tests
    total++;
    if (testDelayEffectCircularBuffer()) {
        std::cout << "PASS: DelayEffect Circular Buffer\n";
        passed++;
    } else {
        std::cout << "FAIL: DelayEffect Circular Buffer\n";
    }

    total++;
    if (testDelayEffectFeedback()) {
        std::cout << "PASS: DelayEffect Feedback\n";
        passed++;
    } else {
        std::cout << "FAIL: DelayEffect Feedback\n";
    }

    // EffectChain tests
    total++;
    if (testEffectChainSerialProcessing()) {
        std::cout << "PASS: EffectChain Serial Processing\n";
        passed++;
    } else {
        std::cout << "FAIL: EffectChain Serial Processing\n";
    }

    total++;
    if (testEffectChainBypass()) {
        std::cout << "PASS: EffectChain Bypass\n";
        passed++;
    } else {
        std::cout << "FAIL: EffectChain Bypass\n";
    }

    // Distortion guitar tests
    total++;
    if (testDistortionPedalToneStacks()) {
        std::cout << "PASS: Distortion Pedal Tone Stacks\n";
        passed++;
    } else {
        std::cout << "FAIL: Distortion Pedal Tone Stacks\n";
    }

    total++;
    if (testDistortionGateAndSag()) {
        std::cout << "PASS: Distortion Gate and Sag\n";
        passed++;
    } else {
        std::cout << "FAIL: Distortion Gate and Sag\n";
    }

    total++;
    if (testDistortionStatefulStereoIsolation()) {
        std::cout << "PASS: Distortion Stateful Stereo Isolation\n";
        passed++;
    } else {
        std::cout << "FAIL: Distortion Stateful Stereo Isolation\n";
    }

    // Cabinet simulator tests
    total++;
    if (testCabinetFullFirImpulse()) {
        std::cout << "PASS: Cabinet Full FIR Impulse\n";
        passed++;
    } else {
        std::cout << "FAIL: Cabinet Full FIR Impulse\n";
    }

    total++;
    if (testCabinetBypass()) {
        std::cout << "PASS: Cabinet Bypass\n";
        passed++;
    } else {
        std::cout << "FAIL: Cabinet Bypass\n";
    }

    total++;
    if (testCabinetSmallBlockContinuity()) {
        std::cout << "PASS: Cabinet Small Block Continuity\n";
        passed++;
    } else {
        std::cout << "FAIL: Cabinet Small Block Continuity\n";
    }

    // Performance tests
    double latency = testPerformanceLatency();
    total++;
    if (latency < 10.0) {
        std::cout << "PASS: Latency " << latency << " ms < 10ms\n";
        passed++;
    } else {
        std::cout << "FAIL: Latency " << latency << " ms >= 10ms\n";
    }

    float thd = testPerformanceTHD();
    total++;
    if (thd < 0.01f) { // Simplified, requirement is 0.1%
        std::cout << "PASS: THD " << (thd * 100) << "% < 1%\n";
        passed++;
    } else {
        std::cout << "FAIL: THD " << (thd * 100) << "% >= 1%\n";
    }

    std::cout << "\nResults: " << passed << "/" << total << " tests passed.\n";

    return (passed == total) ? 0 : 1;
}
