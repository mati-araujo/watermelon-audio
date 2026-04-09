#pragma once

#include "AudioMode.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>

// Forward declarations (global namespace - these are NOT in watermelon_audio namespace)
class OscillatorNode;
class InputNode;
class MixerNode;
class EffectChainNode;

/**
 * @file XYMapper.h
 * @brief Flexible XY controller mapping system.
 *
 * Part of Stage 3: Mode System implementation.
 *
 * Maps XY controller coordinates to audio parameters based on
 * the current mode configuration. Supports:
 * - Primary X/Y mappings
 * - Secondary mappings (for complex modes)
 * - Multiple curve types (linear, logarithmic, exponential, S-curve)
 * - Thread-safe parameter updates
 */

namespace watermelon_audio {

/**
 * @brief Callback type for custom parameter application.
 */
using ParameterCallback = std::function<void(XYTarget target, float value)>;

/**
 * @brief Secondary mapping for complex modes (forward declaration).
 */
struct SecondaryMappingData {
    XYTarget target = XYTarget::NONE;
    bool isXAxis = true;
    Range range = {0.0f, 1.0f};
    CurveType curve = CurveType::LINEAR;
    bool inverted = false;
};

/**
 * @brief RT-safe snapshot of XY mapper configuration.
 *
 * Used with double-buffering for lock-free reads from audio thread.
 */
struct XYConfigSnapshot {
    XYMappingConfig config;
    std::vector<SecondaryMappingData> secondaryMappings;
    bool hasCallback = false;  // Track if callback is set (callback itself not copied)
};

/**
 * @class XYMapper
 * @brief Maps XY coordinates to audio parameters.
 *
 * Thread Safety:
 * - processXY() can be called from any thread
 * - Configuration updates are mutex-protected
 * - Target node pointers should only be set from UI thread
 */
class XYMapper {
public:
    XYMapper();
    ~XYMapper() = default;

    // Non-copyable
    XYMapper(const XYMapper&) = delete;
    XYMapper& operator=(const XYMapper&) = delete;

    /**
     * @brief Set target nodes for parameter application.
     *
     * These should be set once during setup and not changed during audio processing.
     */
    void setOscillatorNode(OscillatorNode* node) { mOscillatorNode = node; }
    void setInputNode(InputNode* node) { mInputNode = node; }
    void setMixerNode(MixerNode* node) { mMixerNode = node; }
    void setEffectChainNode(EffectChainNode* node) { mEffectChainNode = node; }

    /**
     * @brief Set the XY mapping configuration.
     * @param config New mapping configuration
     *
     * Thread-safe: protected by mutex.
     */
    void setConfiguration(const XYMappingConfig& config);

    /**
     * @brief Get current mapping configuration.
     */
    XYMappingConfig getConfiguration() const;

    /**
     * @brief Process XY coordinates and apply to targets.
     * @param x X coordinate (0.0 to 1.0)
     * @param y Y coordinate (0.0 to 1.0)
     *
     * Thread-safe: can be called from UI or audio thread.
     */
    void processXY(float x, float y);

    /**
     * @brief Get last processed X value.
     */
    float getLastX() const { return mLastX.load(std::memory_order_acquire); }

    /**
     * @brief Get last processed Y value.
     */
    float getLastY() const { return mLastY.load(std::memory_order_acquire); }

    /**
     * @brief Get last mapped X value (after curve and range).
     */
    float getLastMappedX() const { return mLastMappedX.load(std::memory_order_acquire); }

    /**
     * @brief Get last mapped Y value (after curve and range).
     */
    float getLastMappedY() const { return mLastMappedY.load(std::memory_order_acquire); }

    /**
     * @brief Secondary mapping for complex modes.
     * Uses SecondaryMappingData for RT-safe snapshot compatibility.
     */
    using SecondaryMapping = SecondaryMappingData;

    /**
     * @brief Add a secondary mapping.
     */
    void addSecondaryMapping(const SecondaryMapping& mapping);

    /**
     * @brief Clear all secondary mappings.
     */
    void clearSecondaryMappings();

    /**
     * @brief Set custom callback for parameter application.
     *
     * If set, this callback is invoked for each target instead of
     * directly applying to nodes. Useful for custom routing.
     */
    void setParameterCallback(ParameterCallback callback);

    /**
     * @brief Clear custom callback (resume direct node application).
     */
    void clearParameterCallback();

private:
    /**
     * @brief Apply curve transformation to normalized value.
     */
    float applyCurve(float value, CurveType curve, bool inverted) const;

    /**
     * @brief Apply value to specific target.
     */
    void applyToTarget(XYTarget target, float value);

    // Configuration (mutex-protected for UI thread writes)
    XYMappingConfig mConfig;
    mutable std::mutex mConfigMutex;

    // ========== RT-Safe Snapshot System ==========
    // Double-buffered snapshots for lock-free audio thread access
    XYConfigSnapshot mConfigSnapshot1;
    XYConfigSnapshot mConfigSnapshot2;
    std::atomic<XYConfigSnapshot*> mActiveConfig{&mConfigSnapshot1};
    std::atomic<bool> mUsingConfigSnapshot1{true};

    /**
     * @brief Update the inactive snapshot and atomically swap.
     *
     * Called from UI thread after modifying configuration.
     * PRECONDITION: mConfigMutex must be locked by caller.
     */
    void updateConfigSnapshot();

    // Target nodes (set once, then stable)
    OscillatorNode* mOscillatorNode = nullptr;
    InputNode* mInputNode = nullptr;
    MixerNode* mMixerNode = nullptr;
    EffectChainNode* mEffectChainNode = nullptr;

    // Current values (atomic for thread-safe reads)
    std::atomic<float> mLastX{0.5f};
    std::atomic<float> mLastY{0.0f};
    std::atomic<float> mLastMappedX{0.5f};
    std::atomic<float> mLastMappedY{0.0f};

    // Secondary mappings
    std::vector<SecondaryMapping> mSecondaryMappings;
    mutable std::mutex mSecondaryMutex;

    // Optional custom callback
    ParameterCallback mParameterCallback;
    mutable std::mutex mCallbackMutex;
};

} // namespace watermelon_audio
