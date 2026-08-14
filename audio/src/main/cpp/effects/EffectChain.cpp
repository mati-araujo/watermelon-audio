#include "EffectChain.h"
#include "EffectTypes.h"
#include "EffectRegistry.h"
// Individual effect includes moved to EffectRegistry.cpp (Phase 1F)
// VocoderEffect still needed for vocoder-specific methods (static_cast + param IDs)
#include "VocoderEffect.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <thread>
#include <chrono>
#include "../platform/Logger.h"

#define LOG_TAG "EffectChain"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

// ========== AUDIO_DIAG: Diagnostic logging for effect chain ==========
// Filter with: adb logcat -s AUDIO_DIAG
#ifndef NDEBUG
    static constexpr bool AUDIO_DIAG_ENABLED = true;
#else
    static constexpr bool AUDIO_DIAG_ENABLED = false;
#endif

#define AUDIO_DIAG_TAG "AUDIO_DIAG"
#define AUDIO_DIAG(...) do { \
    if (AUDIO_DIAG_ENABLED) wma::logMessage(wma::LogLevel::INFO, AUDIO_DIAG_TAG, __VA_ARGS__); \
} while(0)

EffectChain::EffectChain() {
    // Register all built-in effects (Phase 1F)
    registerBuiltinEffects(mRegistry);

    presets.resize(8);
    // Pre-alocar buffers para 4096 frames stereo (8192 samples)
    // Esto evita heap allocation en el callback RT
    // All buffers zero-initialized to prevent garbage on first use
    tempBuffer1.resize(8192, 0.0f);
    tempBuffer2.resize(8192, 0.0f);

    // Routing mode buffers (same size as ping-pong buffers)
    mBranchBufferA.resize(8192, 0.0f);
    mBranchBufferB.resize(8192, 0.0f);
    mMixBuffer.resize(8192, 0.0f);
    mFeedbackBuffer.resize(8192, 0.0f);
    mCrossfadeBuffer.resize(8192, 0.0f);

    // Crossfade duration: ~30ms at 48kHz = 1440 samples
    mCrossfadeSamples = static_cast<int>(48000.0f * 0.030f);

    // Initialize bypass smoothers (0.0 = active, 1.0 = bypassed)
    for (size_t i = 0; i < MAX_BYPASS_SLOTS; ++i) {
        mBypassSmooth[i].reset(0.0f);
        mBypassTarget[i].store(0.0f, std::memory_order_relaxed);
    }
    mGlobalBypassSmooth.reset(0.0f);
    mGlobalBypassTarget.store(0.0f, std::memory_order_relaxed);

    // Inicializar snapshot vacío
    mActiveSnapshot.store(&mSnapshot1, std::memory_order_release);

    LOGI("EffectChain constructed");
}

EffectChain::~EffectChain() {
    // RAII: Limpiar recursos
    std::lock_guard<std::mutex> lock(chainMutex);
    effects.clear();
    effectTypes.clear();
    bypassed.clear();

    LOGI("EffectChain destroyed");
}

void EffectChain::updateSnapshot() {
    // PRECONDICIÓN: chainMutex debe estar locked por el caller

    // Determinar cuál snapshot usar (el que NO está activo)
    bool usingSnapshot1 = mUsingSnapshot1.load(std::memory_order_acquire);
    EffectSnapshot* inactiveSnapshot = usingSnapshot1 ? &mSnapshot2 : &mSnapshot1;

    // Actualizar snapshot inactivo
    inactiveSnapshot->effects.clear();
    inactiveSnapshot->bypassed.clear();

    for (size_t i = 0; i < effects.size(); ++i) {
        inactiveSnapshot->effects.push_back(effects[i].get());
        inactiveSnapshot->bypassed.push_back(bypassed[i]);
    }

    inactiveSnapshot->size = effects.size();

    // WD-1.6 — publicar el indice del vocoder junto con el snapshot.
    // Se computa aca, bajo chainMutex, y es lo que deja a los cuatro setters
    // del vocoder leer sin tomar el lock desde el thread de audio.
    int vocoderIndex = -1;
    for (size_t i = 0; i < effectTypes.size(); ++i) {
        if (effectTypes[i] == VOCODER) {
            vocoderIndex = static_cast<int>(i);
            break;
        }
    }
    mVocoderIndex.store(vocoderIndex, std::memory_order_release);

    // Swap atómico del snapshot activo
    mActiveSnapshot.store(inactiveSnapshot, std::memory_order_release);
    mUsingSnapshot1.store(!usingSnapshot1, std::memory_order_release);

    LOGI("Snapshot updated: %zu effects", inactiveSnapshot->size);
}

bool EffectChain::addEffect(EffectType type) {
    std::lock_guard<std::mutex> lock(chainMutex);

    if (effects.size() >= MAX_EFFECTS) {
        LOGE("Cannot add effect: chain full (max %zu)", MAX_EFFECTS);
        return false;
    }

    // Create effect via registry (Phase 1F — replaces hardcoded switch)
    std::unique_ptr<Effect> effect = mRegistry.createEffect(type);
    if (!effect) {
        LOGE("Unknown or unregistered effect type: %d", type);
        return false;
    }

    // IMPROVED: Propagate current sample rate to the new effect
    effect->setSampleRate(mSampleRate);

    effects.push_back(std::move(effect));
    effectTypes.push_back(type);
    bypassed.push_back(false);

    // Initialize bypass smoother for new effect (fully active)
    size_t newIndex = effects.size() - 1;
    if (newIndex < MAX_BYPASS_SLOTS) {
        mBypassSmooth[newIndex].reset(0.0f);
        mBypassTarget[newIndex].store(0.0f, std::memory_order_relaxed);
    }

    // Actualizar snapshot para audio thread
    updateSnapshot();

    LOGI("Effect added: type=%d, total=%zu", type, effects.size());
    AUDIO_DIAG("addEffect: type=%d, sampleRate=%d, total=%zu", type, mSampleRate, effects.size());
    return true;
}

void EffectChain::removeEffect(size_t index) {
    std::unique_ptr<Effect> removedEffect;

    {
        std::lock_guard<std::mutex> lock(chainMutex);

        if (index >= effects.size()) {
            LOGE("Cannot remove effect: invalid index %zu (size=%zu)", index, effects.size());
            return;
        }

        // Mover el efecto a una variable local (para destrucción posterior)
        removedEffect = std::move(effects[index]);

        // Eliminar de las estructuras
        effects.erase(effects.begin() + index);
        effectTypes.erase(effectTypes.begin() + index);
        bypassed.erase(bypassed.begin() + index);

        // Actualizar snapshot INMEDIATAMENTE
        updateSnapshot();

        LOGI("Effect removed: index=%zu, remaining=%zu", index, effects.size());
    }

    // CRÍTICO: Esperar un breve momento para que el audio thread
    // termine de usar el efecto antes de destruirlo
    // Asumiendo ~10ms de latencia de audio, 20ms es seguro
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Ahora sí, destruir el efecto (al salir del scope)
    LOGI("Effect destroyed safely");
}

void EffectChain::clearAllEffects() {
    // Move ALL effects out of the chain under the lock, publish an empty
    // snapshot, then drop the lock and sleep ONCE to let the audio thread
    // finish any in-flight read of the previous snapshot before the
    // batched unique_ptrs are destroyed at scope exit.
    //
    // This is the batch equivalent of removeEffect() — same RT-safety
    // guarantees (audio thread reads atomic snapshot), but the 20ms grace
    // is paid once for N effects instead of N times.
    std::vector<std::unique_ptr<Effect>> removed;

    {
        std::lock_guard<std::mutex> lock(chainMutex);

        if (effects.empty()) {
            LOGI("clearAllEffects: chain already empty");
            return;
        }

        removed = std::move(effects);
        effects.clear();
        effectTypes.clear();
        bypassed.clear();

        // Reset bypass smoothers for the slots we just freed so that
        // re-adding effects later starts from a clean (fully active) state.
        for (size_t i = 0; i < MAX_BYPASS_SLOTS; ++i) {
            mBypassSmooth[i].reset(0.0f);
            mBypassTarget[i].store(0.0f, std::memory_order_relaxed);
        }

        // Publish empty snapshot ONCE.
        updateSnapshot();

        LOGI("clearAllEffects: %zu effects removed in batch", removed.size());
    }

    // Single 20ms grace period covers the entire batch. The audio thread
    // is lock-free reading the now-empty snapshot; the old per-effect
    // pointers stored in `removed` are still valid here. After the sleep
    // any in-flight callback has finished, and `removed` destructs at
    // scope exit, taking all old Effect instances with it.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    LOGI("clearAllEffects: batch destruction complete");
}

void EffectChain::reorderEffects(size_t from, size_t to) {
    std::lock_guard<std::mutex> lock(chainMutex);

    if (from >= effects.size() || to >= effects.size() || from == to) {
        LOGE("Invalid reorder: from=%zu, to=%zu, size=%zu", from, to, effects.size());
        return;
    }

    // Mover elementos
    auto effect = std::move(effects[from]);
    auto type = effectTypes[from];
    auto bypass = bypassed[from];

    effects.erase(effects.begin() + from);
    effectTypes.erase(effectTypes.begin() + from);
    bypassed.erase(bypassed.begin() + from);

    effects.insert(effects.begin() + to, std::move(effect));
    effectTypes.insert(effectTypes.begin() + to, type);
    bypassed.insert(bypassed.begin() + to, bypass);

    // Actualizar snapshot
    updateSnapshot();

    LOGI("Effects reordered: from=%zu to=%zu", from, to);
}

// ========== STATE RESET ==========

void EffectChain::reset() {
    // Zero-fill all chain-owned scratch and state buffers. These are
    // pre-sized in the constructor so no allocation happens here —
    // std::fill is a memset on contiguous float vectors.
    std::fill(tempBuffer1.begin(), tempBuffer1.end(), 0.0f);
    std::fill(tempBuffer2.begin(), tempBuffer2.end(), 0.0f);
    std::fill(mBranchBufferA.begin(), mBranchBufferA.end(), 0.0f);
    std::fill(mBranchBufferB.begin(), mBranchBufferB.end(), 0.0f);
    std::fill(mMixBuffer.begin(), mMixBuffer.end(), 0.0f);
    std::fill(mFeedbackBuffer.begin(), mFeedbackBuffer.end(), 0.0f);
    std::fill(mCrossfadeBuffer.begin(), mCrossfadeBuffer.end(), 0.0f);

    // Walk the currently active snapshot (read via atomic acquire — we
    // are on the audio thread which is the only reader, so race-free)
    // and call reset() on every effect. Effects whose state is purely
    // parameter-driven (no buffers) inherit the no-op default from
    // Effect::reset() and this is a cheap iteration.
    EffectSnapshot* snapshot = mActiveSnapshot.load(std::memory_order_acquire);
    if (snapshot) {
        for (size_t i = 0; i < snapshot->size; ++i) {
            if (snapshot->effects[i]) {
                snapshot->effects[i]->reset();
            }
        }
    }

    // Reset crossfade counter — a stale partial crossfade from before
    // the reset would leak the old routing mode's buffer contents.
    mCrossfadeCounter = 0;
    mFeedbackHighEnergyFrames = 0;

    // WD-1.1 — sin log. reset() lo despacha onAudioReady desde el thread de
    // audio (ver Effect.h): es RT, aunque no lo parezca por el nombre.
}

// ========== ROUTING MODE SETTERS ==========

void EffectChain::setRoutingMode(RoutingMode mode) {
    RoutingMode current = mRoutingMode.load(std::memory_order_relaxed);
    if (mode == current) return;
    mRoutingMode.store(mode, std::memory_order_relaxed);
    LOGI("Routing mode set to %d", static_cast<int>(mode));
}

void EffectChain::setParallelMix(float mix) {
    mParallelMix.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

void EffectChain::setFeedbackAmount(float amount) {
    mFeedbackAmount.store(std::clamp(amount, 0.0f, 0.95f), std::memory_order_relaxed);
}

// ========== SINGLE EFFECT PROCESSING HELPER ==========

void EffectChain::processOneEffect(Effect* effect, size_t slotIndex, bool isBypassed,
                                    const float* input, float* output, int numFrames) {
    const int totalSamples = numFrames * 2;

    // Read pre-advanced bypass level (smoothers are advanced once per frame in process())
    float bypassLevel = (slotIndex < MAX_BYPASS_SLOTS) ?
        mBypassSmooth[slotIndex].getCurrent() : (isBypassed ? 1.0f : 0.0f);

    if (bypassLevel > 0.999f) {
        // Fully bypassed: pass-through
        if (input != output) {
            std::copy(input, input + totalSamples, output);
        }
        return;
    }

    // Process effect (note: Effect::process takes non-const input, but doesn't modify it)
    effect->process(const_cast<float*>(input), output, numFrames);

    // Sanitize NaN/Inf to prevent silent propagation through the chain.
    // NaN comparisons always return false, so auto-gain won't catch them.
    bool hadBadSample = false;
    for (int s = 0; s < totalSamples; ++s) {
        if (!std::isfinite(output[s])) {
            output[s] = 0.0f;
            hadBadSample = true;
        }
    }
    if (hadBadSample) {
        // WD-1.1 — era un LOGE, y `LOGE` en este archivo NO es condicional:
        // sobrevivia a release. Peor, una tormenta de NaN dispara por bloque,
        // asi que el log realimentaba el problema con un syscall cada 2,7 ms.
        // El contador conserva el hecho; WD-5.1 lo expone.
        mNonFiniteBlocks.bump();
    }

    if (bypassLevel > 0.001f) {
        // Crossfade wet/dry during bypass transition
        for (int s = 0; s < totalSamples; ++s) {
            output[s] = output[s] * (1.0f - bypassLevel) + input[s] * bypassLevel;
        }
    }

    // Auto-gain compensation
    constexpr float GAIN_CEILING = 1.5f;
    float peak = 0.0f;
    for (int s = 0; s < totalSamples; ++s) {
        float absVal = std::abs(output[s]);
        if (absVal > peak) peak = absVal;
    }
    if (peak > GAIN_CEILING) {
        float gain = GAIN_CEILING / peak;
        for (int s = 0; s < totalSamples; ++s) {
            output[s] *= gain;
        }
    }
}

// ========== MAIN PROCESS ==========

void EffectChain::process(float* input, float* output, int numFrames) {
    // RT-SAFE: Completamente lock-free

    // Cargar snapshot activo atómicamente
    EffectSnapshot* snapshot = mActiveSnapshot.load(std::memory_order_acquire);

    if (snapshot == nullptr || snapshot->size == 0) {
        const int totalSamples = numFrames * 2;
        std::copy(input, input + totalSamples, output);
        return;
    }

    // Verificar si todos los efectos están completamente bypassed
    bool allBypassed = true;
    for (size_t i = 0; i < snapshot->size; ++i) {
        float bypassLevel = (i < MAX_BYPASS_SLOTS) ?
            mBypassSmooth[i].getCurrent() : (snapshot->bypassed[i] ? 1.0f : 0.0f);
        float bypassTarget = (i < MAX_BYPASS_SLOTS) ?
            mBypassTarget[i].load(std::memory_order_relaxed) : (snapshot->bypassed[i] ? 1.0f : 0.0f);
        if (bypassLevel < 0.999f || bypassTarget < 0.999f) {
            allBypassed = false;
            break;
        }
    }

    const int totalSamples = numFrames * 2;
    const float globalBypassTarget = mGlobalBypassTarget.load(std::memory_order_relaxed);
    const float globalBypassLevel = mGlobalBypassSmooth.process(globalBypassTarget);

    if (globalBypassLevel > 0.999f && globalBypassTarget > 0.999f) {
        std::copy(input, input + totalSamples, output);
        return;
    }

    if (allBypassed) {
        std::copy(input, input + totalSamples, output);
        return;
    }

    // Advance bypass smoothers exactly once per frame to prevent
    // double-advancement during crossfade (where processWithMode is called twice)
    for (size_t i = 0; i < snapshot->size && i < MAX_BYPASS_SLOTS; ++i) {
        float target = mBypassTarget[i].load(std::memory_order_relaxed);
        mBypassSmooth[i].process(target);
    }

    // Validar que no excedemos buffers pre-alocados
    if (totalSamples > static_cast<int>(tempBuffer1.size())) {
        mOverflowBlocks.bump();  // WD-1.1 — era un LOGE incondicional
        std::fill(output, output + totalSamples, 0.0f);
        return;
    }

    // Propagate BPM to effects if changed
    float currentBpm = mBpm.load(std::memory_order_relaxed);
    if (currentBpm != mLastBpm) {
        for (size_t i = 0; i < snapshot->size; ++i) {
            if (snapshot->effects[i]) {
                snapshot->effects[i]->setBpm(currentBpm);
            }
        }
        mLastBpm = currentBpm;
    }

    // Check for routing mode change → initiate crossfade
    RoutingMode requestedMode = mRoutingMode.load(std::memory_order_relaxed);
    if (requestedMode != mCurrentProcessingMode && mCrossfadeCounter <= 0) {
        mPendingRoutingMode = requestedMode;
        mCrossfadeCounter = mCrossfadeSamples;

        // Clear feedback buffer when leaving Feedback mode to prevent
        // stale audio artifacts if re-entering Feedback later
        if (mCurrentProcessingMode == RoutingMode::FEEDBACK) {
            std::fill(mFeedbackBuffer.begin(), mFeedbackBuffer.end(), 0.0f);
        }
    }

    if (mCrossfadeCounter > 0) {
        // Crossfade: process with both old and new mode, blend
        processWithMode(mCurrentProcessingMode, snapshot, input, mCrossfadeBuffer.data(), numFrames);
        processWithMode(mPendingRoutingMode, snapshot, input, output, numFrames);

        // Linear crossfade from old → new
        float progress = 1.0f - static_cast<float>(mCrossfadeCounter) / static_cast<float>(mCrossfadeSamples);
        for (int s = 0; s < totalSamples; ++s) {
            output[s] = mCrossfadeBuffer[s] * (1.0f - progress) + output[s] * progress;
        }

        mCrossfadeCounter -= numFrames;
        if (mCrossfadeCounter <= 0) {
            mCurrentProcessingMode = mPendingRoutingMode;
            mCrossfadeCounter = 0;
        }
    } else {
        processWithMode(mCurrentProcessingMode, snapshot, input, output, numFrames);
    }

    if (globalBypassLevel > 0.001f) {
        for (int s = 0; s < totalSamples; ++s) {
            output[s] = output[s] * (1.0f - globalBypassLevel) + input[s] * globalBypassLevel;
        }
    }

    // Silence detection: flag when input has signal but output is silent
    {
        float outputPeakCheck = 0.0f;
        float inputPeakCheck = 0.0f;
        int checkSamples = std::min(totalSamples, 64);
        for (int s = 0; s < checkSamples; ++s) {
            float ai = std::abs(input[s]);
            float ao = std::abs(output[s]);
            if (ai > inputPeakCheck) inputPeakCheck = ai;
            if (ao > outputPeakCheck) outputPeakCheck = ao;
        }
        if (inputPeakCheck > 0.001f && outputPeakCheck < 0.0001f) {
            mSilentOutputBlocks.bump();  // WD-1.1 — era un LOGE incondicional
        }
    }

    // WD-1.1 — el bloque AUDIO_DIAG periodico se borro. Compilaba a nada bajo
    // NDEBUG (AUDIO_DIAG_ENABLED es constexpr false), asi que no era el bug de
    // release — pero SI logueaba desde el thread de audio en debug, y ademas
    // llevaba un `static int` de funcion, o sea global de proceso: dos motores
    // se pisaban el contador (WD-1.5). Lo que medía —cuantos efectos activos,
    // peak de entrada y de salida— es exactamente lo que WD-5.1 expone sin
    // salir del thread.
}

// ========== ROUTING MODE DISPATCHER ==========

void EffectChain::processWithMode(RoutingMode mode, EffectSnapshot* snapshot,
                                   const float* input, float* output, int numFrames) {
    switch (mode) {
        case RoutingMode::SERIAL:
            processSerial(snapshot, input, output, numFrames);
            break;
        case RoutingMode::PARALLEL:
            processParallel(snapshot, input, output, numFrames);
            break;
        case RoutingMode::SPLIT_2X2:
            processSplit2x2(snapshot, input, output, numFrames);
            break;
        case RoutingMode::SERIAL_PARALLEL:
            processSerialParallel(snapshot, input, output, numFrames);
            break;
        case RoutingMode::PARALLEL_SERIAL:
            processParallelSerial(snapshot, input, output, numFrames);
            break;
        case RoutingMode::FEEDBACK:
            processFeedback(snapshot, input, output, numFrames);
            break;
    }
}

// ========== ROUTING STRATEGIES ==========

void EffectChain::processSerialRange(EffectSnapshot* snapshot, size_t startIdx, size_t endIdx,
                                      const float* input, float* output, int numFrames) {
    const int totalSamples = numFrames * 2;

    if (startIdx >= endIdx) {
        if (input != output) {
            std::copy(input, input + totalSamples, output);
        }
        return;
    }

    const float* currentInput = input;
    float* currentOutput = tempBuffer1.data();

    for (size_t i = startIdx; i < endIdx; ++i) {
        processOneEffect(snapshot->effects[i], i, snapshot->bypassed[i],
                         currentInput, currentOutput, numFrames);
        currentInput = currentOutput;
        currentOutput = (currentOutput == tempBuffer1.data()) ?
                        tempBuffer2.data() : tempBuffer1.data();
    }

    if (currentInput != output) {
        std::copy(currentInput, currentInput + totalSamples, output);
    }
}

void EffectChain::processSerial(EffectSnapshot* snapshot, const float* input,
                                 float* output, int numFrames) {
    processSerialRange(snapshot, 0, snapshot->size, input, output, numFrames);
}

void EffectChain::processParallel(EffectSnapshot* snapshot, const float* input,
                                   float* output, int numFrames) {
    const int totalSamples = numFrames * 2;

    // Clear accumulation buffer
    std::fill(mMixBuffer.data(), mMixBuffer.data() + totalSamples, 0.0f);

    int activeCount = 0;
    for (size_t i = 0; i < snapshot->size; ++i) {
        // Check if this effect is fully bypassed — skip it entirely
        // to avoid leaking dry signal into the parallel mix
        float bypassLevel = (i < MAX_BYPASS_SLOTS) ?
            mBypassSmooth[i].getCurrent() : (snapshot->bypassed[i] ? 1.0f : 0.0f);
        if (bypassLevel > 0.999f) {
            continue;
        }

        // Each effect receives the original dry input
        processOneEffect(snapshot->effects[i], i, snapshot->bypassed[i],
                         input, mBranchBufferA.data(), numFrames);

        activeCount++;

        // Accumulate
        for (int s = 0; s < totalSamples; ++s) {
            mMixBuffer[s] += mBranchBufferA[s];
        }
    }

    // Average by active count; if all bypassed, pass-through dry input
    if (activeCount == 0) {
        std::copy(input, input + totalSamples, output);
    } else if (activeCount > 1) {
        float invCount = 1.0f / static_cast<float>(activeCount);
        for (int s = 0; s < totalSamples; ++s) {
            output[s] = mMixBuffer[s] * invCount;
        }
    } else {
        std::copy(mMixBuffer.data(), mMixBuffer.data() + totalSamples, output);
    }
}

void EffectChain::processSplit2x2(EffectSnapshot* snapshot, const float* input,
                                   float* output, int numFrames) {
    const int totalSamples = numFrames * 2;
    const size_t n = snapshot->size;

    // Fallback for < 2 effects: serial
    if (n < 2) {
        processSerial(snapshot, input, output, numFrames);
        return;
    }

    // Fallback for 2 effects: parallel
    if (n == 2) {
        processParallel(snapshot, input, output, numFrames);
        return;
    }

    float mix = mParallelMix.load(std::memory_order_relaxed);

    // Proportional split: first half → branch A, second half → branch B
    size_t splitPoint = n / 2;

    // Branch A: effects[0..splitPoint-1] in serial
    processSerialRange(snapshot, 0, splitPoint, input, mBranchBufferA.data(), numFrames);

    // Branch B: effects[splitPoint..n-1] in serial
    processSerialRange(snapshot, splitPoint, n, input, mBranchBufferB.data(), numFrames);

    // Mix branches: A * (1-mix) + B * mix
    for (int s = 0; s < totalSamples; ++s) {
        output[s] = mBranchBufferA[s] * (1.0f - mix) + mBranchBufferB[s] * mix;
    }
}

void EffectChain::processSerialParallel(EffectSnapshot* snapshot, const float* input,
                                         float* output, int numFrames) {
    const int totalSamples = numFrames * 2;
    const size_t n = snapshot->size;

    // Fallback: < 3 effects → serial
    if (n < 3) {
        processSerial(snapshot, input, output, numFrames);
        return;
    }

    // Serial pre-process: effects[0] → effects[1] → mBranchBufferB
    // (using mBranchBufferB as serial output to keep tempBuffers free for processSerialRange)
    processSerialRange(snapshot, 0, 2, input, mBranchBufferB.data(), numFrames);
    const float* serialOut = mBranchBufferB.data();

    // Parallel part: effects[2..n-1] each receive serialOut, accumulate with equal gain
    std::fill(mMixBuffer.data(), mMixBuffer.data() + totalSamples, 0.0f);

    int activeCount = 0;
    for (size_t i = 2; i < n; ++i) {
        // Skip fully bypassed effects
        float bypassLevel = (i < MAX_BYPASS_SLOTS) ?
            mBypassSmooth[i].getCurrent() : (snapshot->bypassed[i] ? 1.0f : 0.0f);
        if (bypassLevel > 0.999f) continue;

        processOneEffect(snapshot->effects[i], i, snapshot->bypassed[i],
                         serialOut, mBranchBufferA.data(), numFrames);
        activeCount++;

        for (int s = 0; s < totalSamples; ++s) {
            mMixBuffer[s] += mBranchBufferA[s];
        }
    }

    // Average parallel outputs
    if (activeCount == 0) {
        std::copy(serialOut, serialOut + totalSamples, output);
    } else if (activeCount > 1) {
        float invCount = 1.0f / static_cast<float>(activeCount);
        for (int s = 0; s < totalSamples; ++s) {
            output[s] = mMixBuffer[s] * invCount;
        }
    } else {
        std::copy(mMixBuffer.data(), mMixBuffer.data() + totalSamples, output);
    }
}

void EffectChain::processParallelSerial(EffectSnapshot* snapshot, const float* input,
                                         float* output, int numFrames) {
    const int totalSamples = numFrames * 2;
    const size_t n = snapshot->size;

    // Fallback: < 3 effects → serial
    if (n < 3) {
        processSerial(snapshot, input, output, numFrames);
        return;
    }

    // Parallel part: effects[0..n-3] each receive input, accumulate with equal gain
    size_t parallelEnd = n - 2;

    std::fill(mMixBuffer.data(), mMixBuffer.data() + totalSamples, 0.0f);

    int activeCount = 0;
    for (size_t i = 0; i < parallelEnd; ++i) {
        // Skip fully bypassed effects
        float bypassLevel = (i < MAX_BYPASS_SLOTS) ?
            mBypassSmooth[i].getCurrent() : (snapshot->bypassed[i] ? 1.0f : 0.0f);
        if (bypassLevel > 0.999f) continue;

        processOneEffect(snapshot->effects[i], i, snapshot->bypassed[i],
                         input, mBranchBufferA.data(), numFrames);
        activeCount++;

        for (int s = 0; s < totalSamples; ++s) {
            mMixBuffer[s] += mBranchBufferA[s];
        }
    }

    // Average parallel outputs (or pass dry if all bypassed)
    if (activeCount == 0) {
        std::copy(input, input + totalSamples, mMixBuffer.data());
    } else if (activeCount > 1) {
        float invCount = 1.0f / static_cast<float>(activeCount);
        for (int s = 0; s < totalSamples; ++s) {
            mMixBuffer[s] *= invCount;
        }
    }

    // Serial post-process: effects[n-2] → effects[n-1] → output
    processSerialRange(snapshot, parallelEnd, n, mMixBuffer.data(), output, numFrames);
}

void EffectChain::processFeedback(EffectSnapshot* snapshot, const float* input,
                                   float* output, int numFrames) {
    const int totalSamples = numFrames * 2;
    const size_t n = snapshot->size;

    // Fallback: < 2 effects → serial
    if (n < 2) {
        processSerial(snapshot, input, output, numFrames);
        return;
    }

    float fbAmount = mFeedbackAmount.load(std::memory_order_relaxed);

    // Mix input with feedback from previous frame
    for (int s = 0; s < totalSamples; ++s) {
        mMixBuffer[s] = input[s] + mFeedbackBuffer[s] * fbAmount;
    }

    // Forward path: effects[0] → effects[1] → output
    processOneEffect(snapshot->effects[0], 0, snapshot->bypassed[0],
                     mMixBuffer.data(), tempBuffer1.data(), numFrames);

    // Determine last forward effect index (all except last = feedback)
    size_t feedbackEffectIndex = n - 1;
    size_t forwardEnd = feedbackEffectIndex;  // exclusive

    if (forwardEnd >= 2) {
        // Multiple forward effects: chain 0 → 1 → ... → (n-2)
        const float* fwdInput = tempBuffer1.data();
        float* fwdOutput = tempBuffer2.data();
        for (size_t i = 1; i < forwardEnd; ++i) {
            processOneEffect(snapshot->effects[i], i, snapshot->bypassed[i],
                             fwdInput, fwdOutput, numFrames);
            fwdInput = fwdOutput;
            fwdOutput = (fwdOutput == tempBuffer1.data()) ?
                        tempBuffer2.data() : tempBuffer1.data();
        }
        std::copy(fwdInput, fwdInput + totalSamples, output);
    } else {
        // Only 2 effects: effect[0] output goes straight to output
        std::copy(tempBuffer1.data(), tempBuffer1.data() + totalSamples, output);
    }

    // Feedback path: last effect processes output into feedback buffer
    processOneEffect(snapshot->effects[feedbackEffectIndex], feedbackEffectIndex,
                     snapshot->bypassed[feedbackEffectIndex],
                     output, mFeedbackBuffer.data(), numFrames);

    // Safety: soft clip (tanh) on feedback buffer
    for (int s = 0; s < totalSamples; ++s) {
        mFeedbackBuffer[s] = std::tanh(mFeedbackBuffer[s]);
    }

    // Safety: detect runaway energy (peak > 4.0 for 3 consecutive frames)
    float fbPeak = 0.0f;
    for (int s = 0; s < totalSamples; ++s) {
        float absVal = std::abs(mFeedbackBuffer[s]);
        if (absVal > fbPeak) fbPeak = absVal;
    }
    if (fbPeak > 4.0f) {
        mFeedbackHighEnergyFrames++;
        if (mFeedbackHighEnergyFrames >= 3) {
            // Emergency: reduce feedback buffer energy
            for (int s = 0; s < totalSamples; ++s) {
                mFeedbackBuffer[s] *= 0.5f;
            }
            mFeedbackHighEnergyFrames = 0;
        }
    } else {
        mFeedbackHighEnergyFrames = 0;
    }
}

void EffectChain::setBypass(size_t index, bool bypass) {
    std::lock_guard<std::mutex> lock(chainMutex);
    if (index < bypassed.size()) {
        bypassed[index] = bypass;

        // Set smooth bypass target (audio thread will crossfade)
        if (index < MAX_BYPASS_SLOTS) {
            mBypassTarget[index].store(bypass ? 1.0f : 0.0f, std::memory_order_relaxed);
        }

        LOGI("Effect %zu bypass set to %d", index, bypass);

        // Actualizar snapshot para reflejar cambio de bypass
        updateSnapshot();
    } else {
        LOGE("setBypass: invalid index %zu (size=%zu)", index, bypassed.size());
    }
}

bool EffectChain::getBypass(size_t index) const {
    std::lock_guard<std::mutex> lock(chainMutex);
    if (index < bypassed.size()) {
        return bypassed[index];
    }
    LOGE("getBypass: invalid index %zu (size=%zu)", index, bypassed.size());
    return false;
}

void EffectChain::setGlobalBypass(bool bypass) {
    mGlobalBypassTarget.store(bypass ? 1.0f : 0.0f, std::memory_order_relaxed);
    LOGI("Global effect bypass set to %d", bypass ? 1 : 0);
}

bool EffectChain::getGlobalBypass() const {
    return mGlobalBypassTarget.load(std::memory_order_relaxed) > 0.5f;
}

void EffectChain::setParameter(size_t index, int paramId, float value) {
    // Lock-free: read from atomic snapshot to avoid race with addEffect() reallocation.
    // Effect parameters themselves use atomics internally.
    EffectSnapshot* snapshot = mActiveSnapshot.load(std::memory_order_acquire);
    if (snapshot && index < snapshot->size) {
        snapshot->effects[index]->setParam(paramId, value);
        AUDIO_DIAG("setParameter: effectIndex=%zu, paramId=%d, value=%.4f", index, paramId, value);
    } else {
        LOGE("setParameter: invalid index %zu (snapshot size=%zu)",
             index, snapshot ? snapshot->size : 0);
    }
}

float EffectChain::getParameter(size_t index, int paramId) const {
    // Lock-free: read from atomic snapshot to avoid race with addEffect() reallocation.
    EffectSnapshot* snapshot = mActiveSnapshot.load(std::memory_order_acquire);
    if (snapshot && index < snapshot->size) {
        return snapshot->effects[index]->getParam(paramId);
    }
    LOGE("getParameter: invalid index %zu (snapshot size=%zu)",
         index, snapshot ? snapshot->size : 0);
    return 0.0f;
}

// NOTE: getNumParams is a free function used by savePreset/loadPreset.
// It now delegates to a static registry instance for backward compatibility.
// TODO(Phase 1F): Consider making this an EffectChain method that uses mRegistry.
static EffectRegistry& getGlobalRegistry() {
    static EffectRegistry reg;
    static bool initialized = false;
    if (!initialized) {
        registerBuiltinEffects(reg);
        initialized = true;
    }
    return reg;
}

int getNumParams(EffectType type) {
    return getGlobalRegistry().getNumParams(type);
}

void EffectChain::savePreset(size_t presetId, const std::string& name) {
    std::lock_guard<std::mutex> lock(chainMutex);

    if (presetId >= presets.size()) {
        LOGE("savePreset: invalid preset ID %zu", presetId);
        return;
    }

    Preset p;
    p.name = name;
    p.params.resize(effects.size());

    for (size_t i = 0; i < effects.size(); ++i) {
        int numParams = getNumParams(effectTypes[i]);
        p.params[i].resize(numParams);
        for (int j = 0; j < numParams; ++j) {
            p.params[i][j] = effects[i]->getParam(j);
        }
    }

    presets[presetId] = p;
    LOGI("Preset %zu saved: %s", presetId, name.c_str());
}

void EffectChain::loadPreset(size_t presetId) {
    std::lock_guard<std::mutex> lock(chainMutex);

    if (presetId >= presets.size() || presets[presetId].params.empty()) {
        LOGE("loadPreset: invalid or empty preset ID %zu", presetId);
        return;
    }

    // Cargar parámetros (assume effects match, for simplicity)
    for (size_t i = 0; i < std::min(effects.size(), presets[presetId].params.size()); ++i) {
        for (size_t j = 0; j < presets[presetId].params[i].size(); ++j) {
            effects[i]->setParam(j, presets[presetId].params[i][j]);
        }
    }

    LOGI("Preset %zu loaded", presetId);
}

float mapValue(float value, float min, float max) {
    return min + value * (max - min);
}

/**
 * @brief Maps a normalized value [0, 1] to a logarithmic frequency range
 * @param value Normalized value [0, 1]
 * @return Frequency in Hz (20 Hz to 20 kHz)
 *
 * Uses logarithmic mapping for more intuitive frequency control.
 * Formula: freq = minFreq * (maxFreq/minFreq)^value
 * Example: 0.0 -> 20 Hz, 0.5 -> ~632 Hz, 1.0 -> 20000 Hz
 */
float mapValueLogarithmic(float value, float minFreq, float maxFreq) {
    // Clamp input to [0, 1]
    value = std::clamp(value, 0.0f, 1.0f);
    // Logarithmic mapping: freq = min * (max/min)^value
    return minFreq * powf(maxFreq / minFreq, value);
}

void EffectChain::setAutomationParameter(size_t effectIndex, int paramId, float xyValue) {
    // RT-safe: validate via snapshot to avoid data race with removeEffect
    EffectSnapshot* snapshot = mActiveSnapshot.load(std::memory_order_acquire);
    if (!snapshot || effectIndex >= snapshot->size) {
        LOGE("setAutomationParameter: invalid effect index %zu", effectIndex);
        return;
    }

    EffectType type = effectTypes[effectIndex];
    float mappedValue;

    switch (type) {
        case FILTER:
            // IMPROVED: Use logarithmic mapping for cutoff for more intuitive control
            if (paramId == 0) mappedValue = mapValueLogarithmic(xyValue, 20.0f, 20000.0f); // cutoff
            else if (paramId == 1) mappedValue = mapValue(xyValue, 0.1f, 10.0f); // resonance
            else return;
            break;
        case REVERB:
            if (paramId == 0) mappedValue = mapValue(xyValue, 0.1f, 5.0f); // decay
            else if (paramId == 1) mappedValue = mapValue(xyValue, 0.5f, 2.0f); // size
            else if (paramId == 2) mappedValue = mapValue(xyValue, 0.0f, 1.0f); // mix
            else return;
            break;
        case DELAY:
            if (paramId == 0) mappedValue = mapValue(xyValue, 1.0f, 2000.0f); // delayTime
            else if (paramId == 1) mappedValue = mapValue(xyValue, 0.0f, 0.9f); // feedback
            else if (paramId == 2) mappedValue = mapValue(xyValue, 0.0f, 1.0f); // wet
            else return;
            break;
        case VOCODER:
            if (paramId == 0) mappedValue = mapValue(xyValue, 4.0f, 32.0f); // bandCount
            else if (paramId == 1) mappedValue = mapValue(xyValue, -24.0f, 24.0f); // formantShift
            else if (paramId == 4) mappedValue = mapValue(xyValue, 0.0f, 1.0f); // mix
            else return;
            break;
        case DISTORTION:
            if (paramId == 0) mappedValue = mapValue(xyValue, 0.0f, 1.0f); // drive
            else if (paramId == 1) mappedValue = mapValue(xyValue, 0.0f, 1.0f); // tone
            else if (paramId == 2) mappedValue = mapValue(xyValue, 0.0f, 1.0f); // mix
            else return;
            break;
        case DECIMATOR:
            if (paramId == 0) mappedValue = mapValue(xyValue, 1.0f, 24.0f);             // Bit Depth
            else if (paramId == 1) mappedValue = mapValueLogarithmic(xyValue, 100.0f, 48000.0f); // Sample Rate (log)
            else if (paramId == 2) mappedValue = mapValue(xyValue, 0.0f, 1.0f);          // Mix
            else return;
            break;
        case DECI_HPF:
            if (paramId == 0) mappedValue = mapValue(xyValue, 1.0f, 24.0f);              // Bit Depth (linear)
            else if (paramId == 1) mappedValue = mapValueLogarithmic(xyValue, 20.0f, 8000.0f); // HPF Cutoff (log)
            else if (paramId == 2) mappedValue = mapValueLogarithmic(xyValue, 100.0f, 48000.0f); // Sample Rate (log)
            else if (paramId == 3) mappedValue = mapValue(xyValue, 0.0f, 1.0f);           // Mix
            else return;
            break;
        case AUTO_PAN:
            if (paramId == 0) mappedValue = mapValue(xyValue, 0.1f, 20.0f);              // Rate (linear)
            else if (paramId == 1) mappedValue = mapValue(xyValue, 0.0f, 1.0f);           // Depth
            else if (paramId == 4) mappedValue = mapValue(xyValue, 0.0f, 1.0f);           // Mix
            else return;
            break;
        case COMPLEX_TREM:
            if (paramId == 0) mappedValue = mapValue(xyValue, 0.1f, 20.0f);              // Rate1 (linear)
            else if (paramId == 1) mappedValue = mapValue(xyValue, 0.1f, 20.0f);          // Rate2 (linear)
            else if (paramId == 2) mappedValue = mapValue(xyValue, 0.0f, 1.0f);           // Depth
            else return;
            break;
        case RANDOM_RESO:
            if (paramId == 0) mappedValue = mapValueLogarithmic(xyValue, 80.0f, 12000.0f); // CenterFreq (log)
            else if (paramId == 1) mappedValue = mapValue(xyValue, 0.5f, 30.0f);           // Resonance (linear)
            else if (paramId == 2) mappedValue = mapValue(xyValue, 0.1f, 20.0f);           // LFO Rate (linear)
            else return;
            break;
        case HPF_DELAY:
            if (paramId == 0) mappedValue = mapValueLogarithmic(xyValue, 20.0f, 8000.0f);  // HPF Cutoff (log)
            else if (paramId == 1) mappedValue = mapValue(xyValue, 10.0f, 2000.0f);         // Delay Time (linear)
            else if (paramId == 2) mappedValue = mapValue(xyValue, 0.0f, 0.95f);            // Feedback (linear)
            else if (paramId == 3) mappedValue = mapValue(xyValue, 0.0f, 1.0f);             // Mix
            else if (paramId == 7) mappedValue = mapValue(xyValue, 0.0f, 1.0f);             // Ducking
            else return;
            break;
        case TAPE_ECHO:
            if (paramId == 0) mappedValue = mapValue(xyValue, 50.0f, 2000.0f);              // Delay Time (linear)
            else if (paramId == 1) mappedValue = mapValue(xyValue, 0.0f, 0.95f);            // Feedback (linear)
            else if (paramId == 2) mappedValue = mapValue(xyValue, 0.0f, 1.0f);             // Wow/Flutter (linear)
            else if (paramId == 5) mappedValue = mapValue(xyValue, 0.0f, 1.0f);             // Mix
            else if (paramId == 9) mappedValue = mapValue(xyValue, 0.0f, 1.0f);             // Ducking
            else return;
            break;
        case HALL_REVERB:
            if (paramId == 0) mappedValue = mapValue(xyValue, 0.5f, 15.0f);                 // Decay Time (linear)
            else if (paramId == 1) mappedValue = mapValue(xyValue, 0.1f, 1.0f);             // Size (linear)
            else if (paramId == 3) mappedValue = mapValue(xyValue, 0.0f, 1.0f);             // Diffusion (linear)
            else return;
            break;
        case RISER_REVERB:
            if (paramId == 0) mappedValue = mapValue(xyValue, 100.0f, 3000.0f);             // Attack Time (linear)
            else if (paramId == 1) mappedValue = mapValue(xyValue, 0.5f, 10.0f);            // Decay (linear)
            else if (paramId == 3) mappedValue = mapValue(xyValue, 0.0f, 1.0f);             // Diffusion (linear)
            else return;
            break;
        case BEAT_GRAIN:
            if (paramId == 0) mappedValue = mapValue(xyValue, 1.0f, 200.0f);                // Grain Size (linear)
            else if (paramId == 1) mappedValue = mapValue(xyValue, 0.0f, 3.0f);             // Density (linear)
            else if (paramId == 5) mappedValue = mapValue(xyValue, 0.0f, 1.0f);             // Mix (linear)
            else return;
            break;
        case SPRING_REVERB:
            if (paramId == 0) mappedValue = mapValue(xyValue, 0.4f, 5.0f);
            else if (paramId == 1) mappedValue = mapValue(xyValue, 0.0f, 1.0f);
            else if (paramId == 4) mappedValue = mapValue(xyValue, 0.0f, 1.0f);
            else return;
            break;
        case PLATE_REVERB:
            if (paramId == 0) mappedValue = mapValue(xyValue, 0.5f, 8.0f);
            else if (paramId == 2) mappedValue = mapValue(xyValue, 0.0f, 1.0f);
            else if (paramId == 6) mappedValue = mapValue(xyValue, 0.0f, 1.0f);
            else return;
            break;
        case SHIMMER_REVERB:
            if (paramId == 0) mappedValue = mapValue(xyValue, 1.0f, 15.0f);
            else if (paramId == 3) mappedValue = mapValue(xyValue, 0.0f, 1.0f);
            else if (paramId == 6) mappedValue = mapValue(xyValue, 0.0f, 1.0f);
            else return;
            break;
        default: return;
    }

    snapshot->effects[effectIndex]->setParam(paramId, mappedValue);
}

// ========== XY MAPPING (Phase 4) ==========

/**
 * @brief Apply mapping curve transformation to a normalized position.
 * RT-safe: pure math, no allocations.
 */
static float applyMappingCurve(float position, MappingCurveType curve,
                                MappingPolarity polarity,
                                float mapMin, float mapMax, bool inverted) {
    position = std::clamp(position, 0.0f, 1.0f);

    // 1. Invert if needed
    if (inverted) position = 1.0f - position;

    // 2. Apply polarity
    float normalized;
    if (polarity == MappingPolarity::BIPOLAR) {
        normalized = (position - 0.5f) * 2.0f;  // -1 to +1
    } else {
        normalized = position;  // 0 to 1
    }

    // 3. Apply curve
    float curved;
    switch (curve) {
        case MappingCurveType::LINEAR:
            curved = normalized;
            break;
        case MappingCurveType::EXPONENTIAL:
            // Signed square: more resolution near center/zero
            curved = (polarity == MappingPolarity::BIPOLAR)
                ? std::copysign(normalized * normalized, normalized)
                : normalized * normalized;
            break;
        case MappingCurveType::LOGARITHMIC:
            // Signed sqrt: more resolution at extremes
            curved = (polarity == MappingPolarity::BIPOLAR)
                ? std::copysign(std::sqrt(std::abs(normalized)), normalized)
                : std::sqrt(std::max(normalized, 0.0f));
            break;
        case MappingCurveType::TOGGLE:
            // Snap to min or max
            curved = (polarity == MappingPolarity::BIPOLAR)
                ? (normalized >= 0.0f ? 1.0f : -1.0f)
                : (normalized >= 0.5f ? 1.0f : 0.0f);
            break;
        default:
            curved = normalized;
            break;
    }

    // 4. Map to custom range
    if (polarity == MappingPolarity::BIPOLAR) {
        float center = (mapMin + mapMax) * 0.5f;
        float range = (mapMax - mapMin) * 0.5f;
        return center + curved * range;
    }
    return mapMin + curved * (mapMax - mapMin);
}

AtomicMappingConfig* EffectChain::getMappingForAxis(int axis) {
    switch (axis) {
        case 0: return &mXMapping;
        case 1: return &mYMapping;
        case 2: return &mDepthMapping;
        default: return nullptr;
    }
}

const AtomicMappingConfig* EffectChain::getMappingForAxis(int axis) const {
    switch (axis) {
        case 0: return &mXMapping;
        case 1: return &mYMapping;
        case 2: return &mDepthMapping;
        default: return nullptr;
    }
}

void EffectChain::setMappingConfig(int axis, int effectIndex, int paramId,
                                    int curve, int polarity,
                                    float mapMin, float mapMax, bool inverted) {
    AtomicMappingConfig* config = getMappingForAxis(axis);
    if (!config) return;

    // Write all fields (order doesn't matter for individual atomics at ~config rate)
    config->paramId.store(paramId, std::memory_order_relaxed);
    config->curve.store(curve, std::memory_order_relaxed);
    config->polarity.store(polarity, std::memory_order_relaxed);
    config->mapMin.store(mapMin, std::memory_order_relaxed);
    config->mapMax.store(mapMax, std::memory_order_relaxed);
    config->inverted.store(inverted, std::memory_order_relaxed);
    // Write effectIndex LAST — this is the "enable" flag read first by applyAutomation
    config->effectIndex.store(effectIndex, std::memory_order_release);

    LOGI("MappingConfig set: axis=%d effect=%d param=%d curve=%d polarity=%d range=[%.2f,%.2f] inv=%d",
         axis, effectIndex, paramId, curve, polarity, mapMin, mapMax, inverted);
}

void EffectChain::clearMappingConfig(int axis) {
    AtomicMappingConfig* config = getMappingForAxis(axis);
    if (!config) return;

    config->effectIndex.store(-1, std::memory_order_release);
    LOGI("MappingConfig cleared: axis=%d", axis);
}

void EffectChain::applyAutomation(int axis, float normalizedValue) {
    const AtomicMappingConfig* config = getMappingForAxis(axis);
    if (!config) return;

    // Read effectIndex first (written last in setMappingConfig with release)
    int effectIdx = config->effectIndex.load(std::memory_order_acquire);
    if (effectIdx < 0) return;

    // RT-safe: use snapshot to validate effect still exists (avoids data race with removeEffect)
    EffectSnapshot* snapshot = mActiveSnapshot.load(std::memory_order_acquire);
    if (!snapshot || static_cast<size_t>(effectIdx) >= snapshot->size) return;

    int paramId = config->paramId.load(std::memory_order_relaxed);
    auto curveType = static_cast<MappingCurveType>(config->curve.load(std::memory_order_relaxed));
    auto polarityType = static_cast<MappingPolarity>(config->polarity.load(std::memory_order_relaxed));
    float mapMin = config->mapMin.load(std::memory_order_relaxed);
    float mapMax = config->mapMax.load(std::memory_order_relaxed);
    bool inverted = config->inverted.load(std::memory_order_relaxed);

    float mapped = applyMappingCurve(normalizedValue, curveType, polarityType,
                                      mapMin, mapMax, inverted);

    snapshot->effects[effectIdx]->setParam(paramId, mapped);
}

void EffectChain::setSampleRate(int sampleRate) {
    std::lock_guard<std::mutex> lock(chainMutex);
    mSampleRate = sampleRate;
    LOGI("Sample rate set to %d", sampleRate);

    // Configure bypass smoothers (~20ms crossfade)
    float sr = static_cast<float>(sampleRate);
    for (size_t i = 0; i < MAX_BYPASS_SLOTS; ++i) {
        mBypassSmooth[i].setSmoothingTime(20.0f, sr);
    }
    mGlobalBypassSmooth.setSmoothingTime(20.0f, sr);

    // Routing mode crossfade: ~30ms
    mCrossfadeSamples = static_cast<int>(sr * 0.030f);

    // IMPROVED: Propagate sample rate to all existing effects
    for (auto& effect : effects) {
        effect->setSampleRate(sampleRate);
    }
    LOGI("Sample rate propagated to %zu effects", effects.size());
    AUDIO_DIAG("setSampleRate: rate=%d, propagated to %zu effects", sampleRate, effects.size());
}

size_t EffectChain::getNumEffects() const {
    // Lock-free read (size es pequeño, lectura atómica implícita en la mayoría de plataformas)
    return effects.size();
}

EffectType EffectChain::getEffectType(size_t index) const {
    std::lock_guard<std::mutex> lock(chainMutex);
    if (index >= effectTypes.size()) {
        return EffectType::FILTER; // Default fallback
    }
    return effectTypes[index];
}

// ========== VOCODER-SPECIFIC METHODS ==========

// WD-1.6 — el vocoder tomaba el mutex de la cadena DESDE EL THREAD DE AUDIO.
//
// Esto lo encontro el lint de WD-1.1, no la auditoria. La cadena era:
//
//   AudioEngine::feedVocoderModulator()        [thread de audio, por bloque]
//     -> EffectChain::setVocoderModulatorBuffer()
//        -> findVocoderIndex()
//           -> std::lock_guard<std::mutex> lock(chainMutex)   <-- BLOQUEA
//
// `chainMutex` es el mismo que sostienen addEffect(), removeEffect() y
// clearAllEffects() — y este ultimo lo sostiene mientras construye y destruye
// efectos, o sea mientras aloca. Agregar un efecto con el vocoder corriendo
// ponia al thread de audio a esperar a un thread de UI que estaba en el
// allocator: inversion de prioridad de manual, y un dropout garantizado.
//
// Y ademas era una carrera: despues de soltar el lock, los cuatro callers
// indexaban `effects` —el vector PROPIO, no el snapshot— confiando en un
// comentario que decia "effects vector is stable once added". Es falso:
// removeEffect() hace effects.erase(), que desplaza y puede realocar.
//
// El arreglo publica el indice en un atomico, mantenido bajo el mutex por
// updateSnapshot() (que ya corre en el thread de control en cada cambio
// estructural), y lee el PUNTERO del snapshot activo, que es la vista que ya
// existia justamente para que el thread de audio no toque `effects`.

int EffectChain::findVocoderIndex() const {
    // Lock-free: el indice lo publica updateSnapshot() bajo chainMutex.
    return mVocoderIndex.load(std::memory_order_acquire);
}

Effect* EffectChain::vocoderFromSnapshot() const {
    const int index = mVocoderIndex.load(std::memory_order_acquire);
    if (index < 0) return nullptr;

    EffectSnapshot* snapshot = mActiveSnapshot.load(std::memory_order_acquire);
    if (snapshot == nullptr) return nullptr;
    if (static_cast<size_t>(index) >= snapshot->size) return nullptr;

    return snapshot->effects[index];
}

void EffectChain::setVocoderModulatorBuffer(const float* buffer, int numSamples) {
    if (buffer == nullptr || numSamples <= 0) {
        return;
    }
    // VocoderEffect::setModulatorBuffer es thread-safe (copia internamente).
    if (Effect* v = vocoderFromSnapshot()) {
        static_cast<VocoderEffect*>(v)->setModulatorBuffer(buffer, numSamples);
    }
}

void EffectChain::setVocoderCarrierFrequency(float frequency) {
    if (Effect* v = vocoderFromSnapshot()) {
        v->setParam(VocoderEffect::CARRIER_FREQ, frequency);
    }
}

void EffectChain::setVocoderCarrierSource(bool useInternalCarrier) {
    // 0 = input signal as carrier, 1 = internal oscillator as carrier
    if (Effect* v = vocoderFromSnapshot()) {
        v->setParam(VocoderEffect::CARRIER_SOURCE, useInternalCarrier ? 1.0f : 0.0f);
    }
}

void EffectChain::setVocoderModulatorSource(bool useExternalMod) {
    // 0 = self-vocoding, 1 = external modulator (mic)
    if (Effect* v = vocoderFromSnapshot()) {
        v->setParam(VocoderEffect::MOD_SOURCE, useExternalMod ? 1.0f : 0.0f);
    }
}
