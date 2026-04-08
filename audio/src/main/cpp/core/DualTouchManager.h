#pragma once

#include <atomic>
#include <vector>
#include <algorithm>
#include <cmath>
#include "../dsp/SIMDUtils.h"
#include "../platform/Logger.h"

// Logging macros local to this header (header-only, no LOG_TAG from .cpp)
#define DTM_LOGI(...) wma::logMessage(wma::LogLevel::INFO, "DualTouchMgr", __VA_ARGS__)
#define DTM_LOGE(...) wma::logMessage(wma::LogLevel::ERROR, "DualTouchMgr", __VA_ARGS__)

/**
 * @brief Modos de mezcla para dual touch
 */
enum class DualTouchMixMode {
    SUM = 0,        // Suma simple (puede saturar)
    AVERAGE = 1,    // Promedio (mantiene nivel) - DEFAULT
    MAX = 2,        // Máximo de ambas señales
    CROSSFADE = 3,  // Crossfade basado en distancia
    RING = 4,       // Multiplicación (ring modulation)
    AMPLITUDE_BALANCED = 5  // Balance automático de amplitud
};

/**
 * @brief Snapshot of all dual-touch state for use in the audio callback.
 *
 * Avoids multiple atomic loads by batch-reading everything in one call.
 */
struct TouchState {
    float x1, y1, freq1, amp1, pressure1;
    float x2, y2, freq2, amp2, pressure2;
    float distance, angle;
    bool active;
    int secondaryOscIndex;
    DualTouchMixMode mixMode;
};

/**
 * @class DualTouchManager
 * @brief Owns all dual-touch atomic parameters, buffers, and mixing logic.
 *
 * Extracted from AudioEngine (Phase 1E) to reduce its member count.
 * Header-only, RT-safe mixing path (no allocations after construction).
 */
class DualTouchManager {
public:
    DualTouchManager() {
        try {
            mTouch1Buffer.resize(8192);  // 4096 frames * 2 channels
            mTouch2Buffer.resize(8192);
            DTM_LOGI("DualTouchManager: buffers allocated successfully");
        } catch (const std::bad_alloc& e) {
            DTM_LOGE("DualTouchManager: failed to allocate buffers: %s", e.what());
        }
    }

    // ========== ENABLED STATE ==========

    void setEnabled(bool enabled) {
        bool wasEnabled = mDualTouchMode.load(std::memory_order_acquire);
        mDualTouchMode.store(enabled, std::memory_order_release);

        if (enabled && !wasEnabled) {
            DTM_LOGI("Dual touch mode ENABLED (UNIFIED: using same oscillator for both touches)");
            clearBuffers();
        } else if (!enabled && wasEnabled) {
            DTM_LOGI("Dual touch mode DISABLED");
            // Clear stale dual-touch state to prevent glitches when
            // returning to single-touch mode.
            mTouch1Amp.store(0.0f, std::memory_order_release);
            mTouch2Amp.store(0.0f, std::memory_order_release);
            mTouch1Freq.store(0.0f, std::memory_order_release);
            mTouch2Freq.store(0.0f, std::memory_order_release);
        }
    }

    bool isEnabled() const {
        return mDualTouchMode.load(std::memory_order_acquire);
    }

    // ========== PARAMETER UPDATE ==========

    void update(float x1, float y1, float freq1, float amp1, float pressure1,
                float x2, float y2, float freq2, float amp2, float pressure2,
                float distance, float angle) {
        mTouch1X.store(x1, std::memory_order_release);
        mTouch1Y.store(y1, std::memory_order_release);
        mTouch1Freq.store(freq1, std::memory_order_release);
        mTouch1Amp.store(amp1, std::memory_order_release);
        mTouch1Pressure.store(pressure1, std::memory_order_release);

        mTouch2X.store(x2, std::memory_order_release);
        mTouch2Y.store(y2, std::memory_order_release);
        mTouch2Freq.store(freq2, std::memory_order_release);
        mTouch2Amp.store(amp2, std::memory_order_release);
        mTouch2Pressure.store(pressure2, std::memory_order_release);

        mTouchDistance.store(distance, std::memory_order_release);
        mTouchAngle.store(angle, std::memory_order_release);
    }

    // ========== MIX MODE ==========

    void setMixMode(DualTouchMixMode mode) {
        mDualTouchMixMode.store(mode, std::memory_order_release);
        DTM_LOGI("Dual touch mix mode set to: %d", static_cast<int>(mode));
    }

    DualTouchMixMode getMixMode() const {
        return mDualTouchMixMode.load(std::memory_order_acquire);
    }

    // ========== SECONDARY OSCILLATOR ==========

    void setSecondaryOscillatorType(int typeId) {
        mSecondaryOscillatorIndex.store(typeId, std::memory_order_release);
        DTM_LOGI("Secondary oscillator type set to: %d", typeId);
    }

    int getSecondaryOscillatorType() const {
        return mSecondaryOscillatorIndex.load(std::memory_order_acquire);
    }

    // ========== SNAPSHOT (batch atomic read) ==========

    /**
     * @brief Read all touch parameters in one call for use in the audio callback.
     * Reduces the number of scattered atomic loads in the hot path.
     */
    TouchState snapshot() const {
        TouchState s;
        s.active = mDualTouchMode.load(std::memory_order_acquire);
        s.x1 = mTouch1X.load(std::memory_order_acquire);
        s.y1 = mTouch1Y.load(std::memory_order_acquire);
        s.freq1 = mTouch1Freq.load(std::memory_order_acquire);
        s.amp1 = mTouch1Amp.load(std::memory_order_acquire);
        s.pressure1 = mTouch1Pressure.load(std::memory_order_acquire);
        s.x2 = mTouch2X.load(std::memory_order_acquire);
        s.y2 = mTouch2Y.load(std::memory_order_acquire);
        s.freq2 = mTouch2Freq.load(std::memory_order_acquire);
        s.amp2 = mTouch2Amp.load(std::memory_order_acquire);
        s.pressure2 = mTouch2Pressure.load(std::memory_order_acquire);
        s.distance = mTouchDistance.load(std::memory_order_acquire);
        s.angle = mTouchAngle.load(std::memory_order_acquire);
        s.secondaryOscIndex = mSecondaryOscillatorIndex.load(std::memory_order_acquire);
        s.mixMode = mDualTouchMixMode.load(std::memory_order_acquire);
        return s;
    }

    // ========== MIXING ==========

    /**
     * @brief Mix two dual-touch audio buffers according to the current mix mode.
     * @param buffer1 Touch 1 audio (interleaved stereo)
     * @param buffer2 Touch 2 audio (interleaved stereo)
     * @param output Output buffer (interleaved stereo)
     * @param numFrames Number of stereo frames
     * @param ts TouchState snapshot (must be obtained via snapshot() before calling)
     *
     * RT-safe: No allocations, no locks.
     */
    void mixSignals(const float* buffer1, const float* buffer2,
                    float* output, int32_t numFrames,
                    const TouchState& ts) const {
        switch (ts.mixMode) {
            case DualTouchMixMode::SUM:
            case DualTouchMixMode::AVERAGE:
                simd::addStereoBuffers(output, buffer1, buffer2, numFrames, true);
                break;

            case DualTouchMixMode::MAX:
            {
                int32_t totalSamples = numFrames * 2;
                for (int32_t i = 0; i < totalSamples; ++i) {
                    float absMax = std::max(std::abs(buffer1[i]), std::abs(buffer2[i]));
                    float sign = (buffer1[i] + buffer2[i] >= 0.0f) ? 1.0f : -1.0f;
                    output[i] = absMax * sign;
                }
            }
            break;

            case DualTouchMixMode::CROSSFADE:
            {
                float crossfade = std::clamp(ts.distance, 0.0f, 1.0f);
                simd::mixStereoBuffers(output, buffer1, buffer2,
                                       1.0f - crossfade, crossfade, numFrames);
            }
            break;

            case DualTouchMixMode::RING:
            {
                int32_t totalSamples = numFrames * 2;
                for (int32_t i = 0; i < totalSamples; ++i) {
                    output[i] = buffer1[i] * buffer2[i] * 0.5f;
                }
            }
            break;

            case DualTouchMixMode::AMPLITUDE_BALANCED:
            {
                float totalAmp = ts.amp1 + ts.amp2;
                if (totalAmp > 0.001f) {
                    float weight1 = ts.amp1 / totalAmp;
                    float weight2 = ts.amp2 / totalAmp;
                    simd::mixStereoBuffers(output, buffer1, buffer2,
                                           weight1, weight2, numFrames);
                } else {
                    simd::addStereoBuffers(output, buffer1, buffer2, numFrames, true);
                }
            }
            break;

            default:
                simd::addStereoBuffers(output, buffer1, buffer2, numFrames, true);
                break;
        }
    }

    // ========== BUFFER ACCESS ==========

    float* getTouch1Buffer() { return mTouch1Buffer.data(); }
    float* getTouch2Buffer() { return mTouch2Buffer.data(); }

    bool hasBuffers() const {
        return !mTouch1Buffer.empty() && !mTouch2Buffer.empty();
    }

    void clearBuffers() {
        if (!mTouch1Buffer.empty()) std::fill(mTouch1Buffer.begin(), mTouch1Buffer.end(), 0.0f);
        if (!mTouch2Buffer.empty()) std::fill(mTouch2Buffer.begin(), mTouch2Buffer.end(), 0.0f);
        DTM_LOGI("Dual touch buffers cleared (size: %zu)", mTouch1Buffer.size());
    }

private:
    // Modo dual touch activo
    std::atomic<bool> mDualTouchMode{false};

    // Parámetros de touch 1
    std::atomic<float> mTouch1X{0.0f};
    std::atomic<float> mTouch1Y{0.0f};
    std::atomic<float> mTouch1Freq{440.0f};
    std::atomic<float> mTouch1Amp{0.0f};
    std::atomic<float> mTouch1Pressure{0.0f};

    // Parámetros de touch 2
    std::atomic<float> mTouch2X{0.0f};
    std::atomic<float> mTouch2Y{0.0f};
    std::atomic<float> mTouch2Freq{440.0f};
    std::atomic<float> mTouch2Amp{0.0f};
    std::atomic<float> mTouch2Pressure{0.0f};

    // Parámetros de interacción
    std::atomic<float> mTouchDistance{0.0f};
    std::atomic<float> mTouchAngle{0.0f};

    // Oscilador secundario (para touch 2)
    std::atomic<int> mSecondaryOscillatorIndex{1};

    // Modo de mezcla
    std::atomic<DualTouchMixMode> mDualTouchMixMode{DualTouchMixMode::AVERAGE};

    // Buffers pre-alocados para dual touch (RT-safe)
    std::vector<float> mTouch1Buffer;
    std::vector<float> mTouch2Buffer;
};
