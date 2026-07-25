#pragma once

/**
 * @file watermelon_audio_internal.h
 * @brief Internal definition of WmaEngine struct.
 *
 * This header is for internal use by the JNI bridge and C API implementation.
 * External consumers should only use watermelon_audio.h (opaque pointer).
 */

#include "watermelon_audio.h"
#include "../core/AudioEngine.h"
#include "../nodes/InputNode.h"
#include "../backends/BackendManager.h"

#include <memory>
#include <atomic>
#include <mutex>

struct WmaEngine {
    // BackendManager owned by this engine instance (Phase 0D)
    std::unique_ptr<watermelon_audio::BackendManager> backendManager;

    std::unique_ptr<AudioEngine> engine;
    std::shared_ptr<InputNode> inputNode;
    std::mutex inputNodeMutex;

    /* Mode system */
    std::atomic<int> currentMode{0};
    std::atomic<bool> modeTransitionInProgress{false};
    std::atomic<float> modeTransitionProgress{0.0f};
};

/**
 * Lazily create and prepare the engine's InputNode.
 *
 * Exposed (rather than being a static in watermelon_audio.cpp) so the JNI bridge
 * can share the SAME node the C API uses instead of building its own. Both used
 * to keep a separate InputNode and attach it to the same AudioEngine, which meant
 * every wma_input_* function operated on a node the shipping Android path never
 * touched — dead code in production, and iOS would have been its first real user.
 *
 * Thread-safe: takes the engine's inputNodeMutex.
 *
 * @return false if the node could not be created.
 */
bool wmaEnsureInputNode(WmaEngine* engine);
