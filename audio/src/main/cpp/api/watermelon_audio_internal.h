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
#include "../analysis/AnalysisRing.h"
#include "../analysis/AnalysisSnapshot.h"
#include "../analysis/AnalysisThread.h"

#include <memory>
#include <atomic>
#include <mutex>

struct WmaEngine {
    // BackendManager owned by this engine instance (Phase 0D)
    std::unique_ptr<watermelon_audio::BackendManager> backendManager;

    std::unique_ptr<AudioEngine> engine;
    std::shared_ptr<InputNode> inputNode;
    std::mutex inputNodeMutex;

    /* Analisis / afinador (REQ-001 S1).
     *
     * El ring y el snapshot viven mientras vive el motor, y eso NO es descuido:
     * el thread de captura carga el puntero al ring de un atomico y despues
     * escribe. Si `wma_tuner_stop()` liberara el ring, un callback que ya cargo
     * el puntero escribiria en memoria muerta. Parar el afinador desengancha el
     * puntero (el escritor deja de recibir trabajo) pero no libera nada; lo
     * unico que arranca y para de verdad es el thread que drena.
     *
     * Se liberan en `wma_engine_destroy()`, DESPUES de parar el stream de
     * entrada, que es el unico momento en que se sabe que no queda un callback
     * de captura en vuelo. */
    std::unique_ptr<wma::analysis::AnalysisRing> analysisRing;
    std::unique_ptr<wma::analysis::AnalysisSnapshot> analysisSnapshot;
    std::unique_ptr<wma::analysis::AnalysisThread> analysisThread;
    std::mutex analysisMutex;

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

/**
 * Lazily create the engine's analysis seam: ring, snapshot and drain thread.
 *
 * Exposed for the same reason as wmaEnsureInputNode(): the JNI bridge has to
 * share the SAME ring the C API uses. Two rings would mean the capture thread
 * feeds one of them and the reader polls the other — a tuner that never moves,
 * which is exactly the class of defect that made every wma_input_* function
 * operate on a node the Android path never touched.
 *
 * Does NOT start the thread and does NOT attach the ring to the input node:
 * that is wma_tuner_start()'s job, so that an engine nobody tunes with pays
 * nothing on the capture thread.
 *
 * Thread-safe: takes the engine's analysisMutex.
 *
 * @return false if the seam could not be created.
 */
bool wmaEnsureAnalysis(WmaEngine* engine);
