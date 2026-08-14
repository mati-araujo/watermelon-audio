#pragma once

/**
 * @file FadeController.h
 * @brief Manages audio fade-in/fade-out and pause state.
 *
 * Provides RT-safe fade ramp computation (processFadeBlock) and
 * SIMD-accelerated fade+volume application (applyFadeAndVolume).
 *
 * Phase 1E: Extracted from AudioEngine.
 */

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <climits>
#include <cstdint>
#include <algorithm>
#include "../dsp/SIMDUtils.h"

class FadeController {
public:
    FadeController() = default;

    ~FadeController() {
        cancel();
    }

    // Non-copyable
    FadeController(const FadeController&) = delete;
    FadeController& operator=(const FadeController&) = delete;

    // =========== RT-Safe Methods (audio thread) ===========

    /**
     * Compute fade start/end values for the current block.
     * RT-safe: only atomic loads + arithmetic.
     */
    void processFadeBlock(int32_t numFrames, float& fadeStart, float& fadeEnd) {
        float current = mCurrentFadeVolume.load(std::memory_order_acquire);
        float target = mTargetFadeVolume.load(std::memory_order_acquire);
        int remaining = mFadeRemainingFrames.load(std::memory_order_acquire);

        fadeStart = current;

        if (remaining <= 0 || numFrames <= 0) {
            fadeEnd = target;
            mCurrentFadeVolume.store(target, std::memory_order_release);
            return;
        }

        // Linear interpolation
        float delta = target - current;
        float step = delta * static_cast<float>(std::min(numFrames, remaining)) / static_cast<float>(remaining);
        fadeEnd = current + step;

        // Clamp
        fadeEnd = (target > current)
            ? std::min(fadeEnd, target)
            : std::max(fadeEnd, target);

        mCurrentFadeVolume.store(fadeEnd, std::memory_order_release);
        int newRemaining = remaining - numFrames;
        if (newRemaining < 0) newRemaining = 0;
        mFadeRemainingFrames.store(newRemaining, std::memory_order_release);
    }

    /**
     * Apply fade ramp + master volume in one SIMD pass.
     * RT-safe.
     */
    void applyFadeAndVolume(float* stereoData, int numFrames, float masterVolume) {
        float fadeStart, fadeEnd;
        processFadeBlock(numFrames, fadeStart, fadeEnd);

        float gainStart = fadeStart * masterVolume;
        float gainEnd = fadeEnd * masterVolume;
        simd::applyStereoGainRamp(stereoData, numFrames, gainStart, gainEnd);
    }

    // =========== Non-RT Methods (UI/lifecycle thread) ===========

    /**
     * Configure a fade ramp.
     * @param from Starting volume (0.0-1.0)
     * @param to Target volume (0.0-1.0)
     * @param sampleRate Current stream sample rate (for ms→frames conversion)
     * @param fadeTimeMs Fade duration in milliseconds
     */
    void startFade(float from, float to, int sampleRate, int fadeTimeMs) {
        int64_t fadeFramesLong = (static_cast<int64_t>(sampleRate) * fadeTimeMs) / 1000;
        int fadeFrames = static_cast<int>(std::min(fadeFramesLong, static_cast<int64_t>(INT_MAX)));

        // Un fade de largo CERO es instantaneo, y arranca ya en el destino.
        //
        // Antes arrancaba en `from` con `remaining == 0`, y processFadeBlock
        // resolvia eso devolviendo fadeStart=from, fadeEnd=to. O sea que el
        // bloque siguiente recibia igual una rampa lineal de `from` a `to` —
        // **de la duracion del bloque**. "Instantaneo" era en realidad "en un
        // bloque", y cuanto duraba eso dependia del tamano de buffer que
        // negociara el device: 1,3 ms con 64 frames, 21 ms con 1024, 93 ms con
        // 4096 a 44,1 kHz. El mismo llamado, tres comportamientos.
        //
        // Lo encontro el test de invariancia de bloque de WD-2.1 (era el unico
        // lugar donde divergian dos renders del mismo audio), pero el defecto no
        // es del camino offline: pega mas fuerte en el device, que es donde los
        // buffers son grandes.
        if (fadeFrames <= 0) {
            mCurrentFadeVolume.store(to, std::memory_order_release);
        } else {
            mCurrentFadeVolume.store(from, std::memory_order_release);
        }
        mTargetFadeVolume.store(to, std::memory_order_release);
        mFadeTotalFrames.store(fadeFrames, std::memory_order_release);
        mFadeRemainingFrames.store(fadeFrames, std::memory_order_release);
    }

    /**
     * Set paused state. When paused, master volume is zeroed.
     */
    void setPaused(bool paused) {
        mIsPaused.store(paused, std::memory_order_release);
    }

    /**
     * Start a fade-out with delayed pause (background thread).
     * After fadeTimeMs + 50ms, sets paused=true unless cancelled.
     */
    void fadeOutAndPause(int sampleRate, int fadeTimeMs) {
        cancel();
        startFade(1.0f, 0.0f, sampleRate, fadeTimeMs);

        mCancelFade.store(false, std::memory_order_release);
        auto thread = std::make_unique<std::thread>([this, fadeTimeMs]() {
            auto start = std::chrono::steady_clock::now();
            auto duration = std::chrono::milliseconds(fadeTimeMs + 50);

            while (!mCancelFade.load(std::memory_order_acquire)) {
                auto elapsed = std::chrono::steady_clock::now() - start;
                if (elapsed >= duration) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            if (!mCancelFade.load(std::memory_order_acquire)) {
                mIsPaused.store(true, std::memory_order_release);
            }
        });
        // mThreadMutex serialises every access to mFadeThread. cancel() can run
        // on the UI thread and on AudioEngine's stopWithFade worker at the same
        // time; without this the two race on the unique_ptr (caught by TSan).
        std::lock_guard<std::mutex> lock(mThreadMutex);
        mFadeThread = std::move(thread);
    }

    /**
     * Start a fade-in from pause.
     */
    void resumeWithFade(int sampleRate, int fadeTimeMs) {
        cancel();
        mIsPaused.store(false, std::memory_order_release);
        startFade(0.0f, 1.0f, sampleRate, fadeTimeMs);
    }

    /**
     * Cancel any pending fade thread.
     */
    void cancel() {
        mCancelFade.store(true, std::memory_order_release);
        // Take ownership of the thread under the lock, then join OUTSIDE it: two
        // concurrent cancel() callers each move-out at most once (the loser gets
        // nullptr and joins nothing), and we never hold the mutex across the
        // potentially long join.
        std::unique_ptr<std::thread> toJoin;
        {
            std::lock_guard<std::mutex> lock(mThreadMutex);
            toJoin = std::move(mFadeThread);
        }
        if (toJoin && toJoin->joinable()) {
            toJoin->join();
        }
    }

    // =========== Getters ===========

    bool isPaused() const { return mIsPaused.load(std::memory_order_acquire); }
    float getCurrentFadeVolume() const { return mCurrentFadeVolume.load(std::memory_order_acquire); }
    float getTargetFadeVolume() const { return mTargetFadeVolume.load(std::memory_order_acquire); }

    bool isFading() const {
        return mFadeRemainingFrames.load(std::memory_order_acquire) > 0;
    }

    float getFadeProgress() const {
        int remaining = mFadeRemainingFrames.load(std::memory_order_acquire);
        int total = mFadeTotalFrames.load(std::memory_order_acquire);
        if (total <= 0) return 1.0f;
        return 1.0f - (static_cast<float>(remaining) / static_cast<float>(total));
    }

private:
    std::atomic<float> mCurrentFadeVolume{1.0f};
    std::atomic<float> mTargetFadeVolume{1.0f};
    std::atomic<int> mFadeRemainingFrames{0};
    std::atomic<int> mFadeTotalFrames{0};
    std::atomic<bool> mIsPaused{false};
    std::unique_ptr<std::thread> mFadeThread;
    std::mutex mThreadMutex;  // guards mFadeThread lifecycle only — never held on the RT path
    std::atomic<bool> mCancelFade{false};
};
