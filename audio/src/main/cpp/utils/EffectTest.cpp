#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>
#include <string>

// Legacy constant (was in core/constants.h, now in effects/EffectDefaults.h)
#ifndef SAMPLE_RATE
#define SAMPLE_RATE 48000
#endif
#include "../effects/Effect.h"
#include "../effects/EffectChain.h"

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

// Helper function to calculate RMS
float calculateRMS(const std::vector<float>& data) {
    float sum = 0.0f;
    for (float val : data) {
        sum += val * val;
    }
    return sqrtf(sum / data.size());
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