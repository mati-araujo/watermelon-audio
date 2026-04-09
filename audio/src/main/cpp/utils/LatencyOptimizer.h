/**
 * LatencyOptimizer.h
 *
 * Automatic latency optimization for Oboe streams.
 *
 * This class tests different Oboe configurations to find the optimal
 * settings for the current device:
 * - Exclusive vs Shared mode
 * - Different buffer sizes
 * - Sample rate compatibility
 *
 * Usage:
 *   LatencyOptimizer optimizer;
 *   auto config = optimizer.findOptimalConfig();
 *   // Apply config to your AudioStreamBuilder
 */

#pragma once

#include <oboe/Oboe.h>
#include <vector>
#include <string>
#include <functional>
#include <chrono>
#include <thread>

namespace watermelon_audio {

// =============================================================================
// Configuration Options
// =============================================================================

struct StreamConfiguration {
    oboe::SharingMode sharingMode = oboe::SharingMode::Shared;
    oboe::PerformanceMode performanceMode = oboe::PerformanceMode::LowLatency;
    int32_t sampleRate = 0;          // 0 = auto
    int32_t framesPerBuffer = 0;     // 0 = auto
    int32_t channelCount = 2;
    oboe::AudioFormat format = oboe::AudioFormat::Float;

    // Results after testing
    bool isSupported = false;
    double measuredLatencyMs = -1.0;
    int32_t actualFramesPerBurst = 0;
    int32_t actualBufferCapacity = 0;
    int32_t actualSampleRate = 0;
    oboe::AudioApi actualApi = oboe::AudioApi::Unspecified;
    std::string errorMessage;

    std::string describe() const {
        std::string desc;
        desc += (sharingMode == oboe::SharingMode::Exclusive) ? "Exclusive" : "Shared";
        desc += ", ";
        desc += (performanceMode == oboe::PerformanceMode::LowLatency) ? "LowLatency" : "Normal";

        if (sampleRate > 0) {
            desc += ", " + std::to_string(sampleRate) + "Hz";
        }
        if (framesPerBuffer > 0) {
            desc += ", " + std::to_string(framesPerBuffer) + " frames";
        }

        return desc;
    }
};

struct OptimizationResult {
    StreamConfiguration bestConfig;
    std::vector<StreamConfiguration> testedConfigs;

    double baselineLatencyMs = -1.0;   // Latency with default config
    double optimizedLatencyMs = -1.0;  // Latency with best config
    double improvementMs = 0.0;        // baselineLatencyMs - optimizedLatencyMs
    double improvementPercent = 0.0;   // improvement / baseline * 100

    bool hasImprovement() const {
        return improvementMs > 0.5;  // At least 0.5ms improvement
    }

    std::string getSummary() const {
        std::string summary;
        summary += "Best configuration: " + bestConfig.describe() + "\n";
        summary += "Baseline latency: " + std::to_string(baselineLatencyMs) + " ms\n";
        summary += "Optimized latency: " + std::to_string(optimizedLatencyMs) + " ms\n";
        summary += "Improvement: " + std::to_string(improvementMs) + " ms";
        summary += " (" + std::to_string(improvementPercent) + "%)\n";
        return summary;
    }
};

// =============================================================================
// Progress Callback
// =============================================================================

using OptimizationProgressCallback = std::function<void(
    int currentStep,
    int totalSteps,
    const std::string& currentTest,
    double currentLatencyMs
)>;

// =============================================================================
// Latency Optimizer
// =============================================================================

class LatencyOptimizer {
public:
    /**
     * Test different configurations and find the optimal one.
     *
     * @param progressCallback  Optional callback for progress updates.
     * @return Optimization results with best configuration.
     */
    OptimizationResult findOptimalConfig(
        OptimizationProgressCallback progressCallback = nullptr
    ) {
        OptimizationResult result;

        // Build list of configurations to test
        auto configs = buildTestConfigurations();
        int totalSteps = static_cast<int>(configs.size());
        int currentStep = 0;

        // Test each configuration
        for (auto& config : configs) {
            currentStep++;

            if (progressCallback) {
                progressCallback(currentStep, totalSteps, config.describe(), -1.0);
            }

            testConfiguration(config);

            if (progressCallback && config.isSupported) {
                progressCallback(currentStep, totalSteps, config.describe(),
                                config.measuredLatencyMs);
            }

            result.testedConfigs.push_back(config);
        }

        // Find baseline (Shared, LowLatency, auto everything)
        for (const auto& config : result.testedConfigs) {
            if (config.sharingMode == oboe::SharingMode::Shared &&
                config.sampleRate == 0 &&
                config.framesPerBuffer == 0 &&
                config.isSupported) {
                result.baselineLatencyMs = config.measuredLatencyMs;
                break;
            }
        }

        // Find best configuration
        double bestLatency = 1e9;
        for (const auto& config : result.testedConfigs) {
            if (config.isSupported && config.measuredLatencyMs > 0) {
                if (config.measuredLatencyMs < bestLatency) {
                    bestLatency = config.measuredLatencyMs;
                    result.bestConfig = config;
                }
            }
        }

        result.optimizedLatencyMs = bestLatency;

        // Calculate improvement
        if (result.baselineLatencyMs > 0 && result.optimizedLatencyMs > 0) {
            result.improvementMs = result.baselineLatencyMs - result.optimizedLatencyMs;
            result.improvementPercent =
                (result.improvementMs / result.baselineLatencyMs) * 100.0;
        }

        return result;
    }

    /**
     * Quick test to check if Exclusive mode is available.
     */
    bool isExclusiveModeSupported() {
        StreamConfiguration config;
        config.sharingMode = oboe::SharingMode::Exclusive;
        config.performanceMode = oboe::PerformanceMode::LowLatency;

        testConfiguration(config);
        return config.isSupported;
    }

    /**
     * Get the minimum achievable latency with current device.
     */
    double getMinimumAchievableLatency() {
        StreamConfiguration config;
        config.sharingMode = oboe::SharingMode::Exclusive;
        config.performanceMode = oboe::PerformanceMode::LowLatency;
        config.framesPerBuffer = 64;  // Try minimum buffer

        testConfiguration(config);

        if (!config.isSupported) {
            // Fallback to Shared mode
            config.sharingMode = oboe::SharingMode::Shared;
            testConfiguration(config);
        }

        return config.isSupported ? config.measuredLatencyMs : -1.0;
    }

    /**
     * Test a specific configuration.
     */
    void testConfiguration(StreamConfiguration& config) {
        oboe::AudioStreamBuilder builder;

        builder.setDirection(oboe::Direction::Output)
            ->setPerformanceMode(config.performanceMode)
            ->setSharingMode(config.sharingMode)
            ->setFormat(config.format)
            ->setChannelCount(config.channelCount);

        if (config.sampleRate > 0) {
            builder.setSampleRate(config.sampleRate);
        }

        if (config.framesPerBuffer > 0) {
            builder.setFramesPerDataCallback(config.framesPerBuffer);
        }

        std::shared_ptr<oboe::AudioStream> stream;
        oboe::Result result = builder.openStream(stream);

        if (result != oboe::Result::OK) {
            config.isSupported = false;
            config.errorMessage = oboe::convertToText(result);
            return;
        }

        // Check if we got what we asked for
        bool gotExclusive = (stream->getSharingMode() == oboe::SharingMode::Exclusive);
        bool wantedExclusive = (config.sharingMode == oboe::SharingMode::Exclusive);

        if (wantedExclusive && !gotExclusive) {
            // Exclusive mode was requested but not granted
            config.isSupported = false;
            config.errorMessage = "Exclusive mode not available";
            stream->close();
            return;
        }

        // Start the stream briefly to get accurate latency measurement
        result = stream->requestStart();
        if (result != oboe::Result::OK) {
            config.isSupported = false;
            config.errorMessage = "Failed to start stream";
            stream->close();
            return;
        }

        // Wait for stream to stabilize
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Measure latency
        auto latencyResult = stream->calculateLatencyMillis();
        if (latencyResult) {
            config.measuredLatencyMs = latencyResult.value();
        } else {
            // Calculate from buffer info
            int sampleRate = stream->getSampleRate();
            int bufferCapacity = stream->getBufferCapacityInFrames();
            if (sampleRate > 0 && bufferCapacity > 0) {
                config.measuredLatencyMs =
                    (static_cast<double>(bufferCapacity) / sampleRate) * 1000.0;
            }
        }

        // Record actual values
        config.actualFramesPerBurst = stream->getFramesPerBurst();
        config.actualBufferCapacity = stream->getBufferCapacityInFrames();
        config.actualSampleRate = stream->getSampleRate();
        config.actualApi = stream->getAudioApi();
        config.isSupported = true;

        stream->requestStop();
        stream->close();
    }

    /**
     * Apply configuration to an AudioStreamBuilder.
     */
    static void applyConfiguration(
        oboe::AudioStreamBuilder& builder,
        const StreamConfiguration& config
    ) {
        builder.setSharingMode(config.sharingMode)
            ->setPerformanceMode(config.performanceMode)
            ->setFormat(config.format)
            ->setChannelCount(config.channelCount);

        if (config.sampleRate > 0) {
            builder.setSampleRate(config.sampleRate);
        }

        if (config.framesPerBuffer > 0) {
            builder.setFramesPerDataCallback(config.framesPerBuffer);
        }
    }

private:
    std::vector<StreamConfiguration> buildTestConfigurations() {
        std::vector<StreamConfiguration> configs;

        // Test combinations of sharing mode and buffer sizes
        std::vector<oboe::SharingMode> sharingModes = {
            oboe::SharingMode::Shared,
            oboe::SharingMode::Exclusive
        };

        // Buffer sizes to test (0 = auto)
        std::vector<int32_t> bufferSizes = {0, 64, 128, 192, 256, 512};

        for (auto sharingMode : sharingModes) {
            for (auto bufferSize : bufferSizes) {
                StreamConfiguration config;
                config.sharingMode = sharingMode;
                config.performanceMode = oboe::PerformanceMode::LowLatency;
                config.framesPerBuffer = bufferSize;
                configs.push_back(config);
            }
        }

        return configs;
    }
};

// =============================================================================
// Quick Latency Check
// =============================================================================

/**
 * QuickLatencyCheck
 *
 * Fast utility to get current latency without full optimization.
 */
class QuickLatencyCheck {
public:
    struct Result {
        double outputLatencyMs = -1.0;
        double inputLatencyMs = -1.0;
        double totalLatencyMs = -1.0;
        int sampleRate = 0;
        int framesPerBurst = 0;
        int bufferCapacity = 0;
        std::string apiName;
        std::string sharingModeName;
        bool isLowLatency = false;
    };

    /**
     * Get latency info from an existing stream.
     */
    static Result fromStream(oboe::AudioStream* stream) {
        Result result;

        if (!stream) {
            return result;
        }

        result.sampleRate = stream->getSampleRate();
        result.framesPerBurst = stream->getFramesPerBurst();
        result.bufferCapacity = stream->getBufferCapacityInFrames();

        result.apiName = (stream->getAudioApi() == oboe::AudioApi::AAudio)
            ? "AAudio" : "OpenSL ES";

        result.sharingModeName = (stream->getSharingMode() == oboe::SharingMode::Exclusive)
            ? "Exclusive" : "Shared";

        result.isLowLatency =
            (stream->getPerformanceMode() == oboe::PerformanceMode::LowLatency);

        auto latency = stream->calculateLatencyMillis();
        if (latency) {
            result.outputLatencyMs = latency.value();
        } else if (result.sampleRate > 0) {
            // Estimate from buffer
            result.outputLatencyMs =
                (static_cast<double>(result.bufferCapacity) / result.sampleRate) * 1000.0;
        }

        return result;
    }

    /**
     * Quick test with a new temporary stream.
     */
    static Result quickTest() {
        Result result;

        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Output)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Shared)
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(2);

        std::shared_ptr<oboe::AudioStream> stream;
        if (builder.openStream(stream) != oboe::Result::OK) {
            return result;
        }

        stream->requestStart();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        result = fromStream(stream.get());

        stream->requestStop();
        stream->close();

        return result;
    }

    /**
     * Format result as string for logging.
     */
    static std::string formatResult(const Result& result) {
        char buffer[512];
        snprintf(buffer, sizeof(buffer),
            "Latency: %.1f ms | API: %s | Mode: %s | "
            "Rate: %d Hz | Burst: %d | Buffer: %d | LowLatency: %s",
            result.outputLatencyMs,
            result.apiName.c_str(),
            result.sharingModeName.c_str(),
            result.sampleRate,
            result.framesPerBurst,
            result.bufferCapacity,
            result.isLowLatency ? "Yes" : "No"
        );
        return std::string(buffer);
    }
};

} // namespace watermelon_audio
