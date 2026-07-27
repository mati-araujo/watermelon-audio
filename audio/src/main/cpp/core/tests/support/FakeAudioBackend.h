#pragma once

/**
 * FakeAudioBackend.h — TEST DOUBLE, host test build only.
 *
 * A controllable IAudioBackend so the core suite can drive the BackendManager
 * path (USB today, CoreAudio on iOS tomorrow) without a device. It is handed to
 * BackendManager through the platform registration point — see
 * test_platform_backends.cpp for why that works and what it replaces.
 *
 * The one behaviour worth calling out: the *requested* sample rate (what the
 * engine pushes down via setSampleRate) and the *negotiated* sample rate (what
 * getStreamInfo reports) are separate knobs. Real devices coerce — a 48000
 * request can come back as 44100 — and telling those two numbers apart is
 * exactly what AudioEngine::currentSampleRate() has to get right.
 */

#include "backends/IAudioBackend.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace wma_test {

class FakeAudioBackend : public watermelon_audio::IAudioBackend {
public:
    // ---- IAudioBackend ----------------------------------------------------

    watermelon_audio::BackendResult start() override {
        // Freno opcional. Existe porque el reopen de la captura pasó a correr en
        // un thread propio, y "no bloquea al llamador" no se puede afirmar contra
        // un start() instantáneo: el worker terminaría antes de que el test mire.
        // Con esto el test decide cuándo destrabar y la carrera desaparece.
        {
            std::unique_lock<std::mutex> lock(mStartGateMutex);
            mStartEntered = true;
            mStartEnteredCv.notify_all();
            mStartGateCv.wait(lock, [this] { return !mStartBlocked; });
        }

        ++mStartCount;
        if (mStartResult != watermelon_audio::BackendResult::OK) {
            return mStartResult;
        }
        // Capture is decided HERE, not when it was requested — the same as every
        // real backend (OboeBackend.cpp:63 reads its flag in start(); CoreAudio
        // attaches its sink node while opening). A fake that honored the request
        // the moment it arrived would make the reopen logic untestable, because
        // the case that needs a reopen would never occur.
        mInfo.isFullDuplex =
            mFullDuplexRequested.load(std::memory_order_acquire) &&
            mCaptureAvailable.load(std::memory_order_acquire);
        mRunning.store(true, std::memory_order_release);
        return watermelon_audio::BackendResult::OK;
    }

    void stop() override {
        mRunning.store(false, std::memory_order_release);
        // No stream, no capture.
        mInfo.isFullDuplex = false;
    }
    void pause() override { mPaused = true; }
    void resume() override { mPaused = false; }

    void setCallback(watermelon_audio::IAudioCallback* callback) override {
        mCallback = callback;
    }

    // Recorded, deliberately NOT applied to the reported stream info: a real
    // device is free to ignore the request, and the tests rely on that gap.
    void setSampleRate(int sampleRate) override { mRequestedSampleRate = sampleRate; }

    void setBufferSize(int framesPerBuffer) override {
        mInfo.framesPerBuffer = framesPerBuffer;
    }

    void setFullDuplexEnabled(bool enable) override {
        mFullDuplexRequested.store(enable, std::memory_order_release);
    }

    watermelon_audio::StreamInfo getStreamInfo() const override { return mInfo; }

    bool isRunning() const override { return mRunning.load(std::memory_order_acquire); }

    float getOutputLatencyMs() const override { return mInfo.outputLatencyMs; }
    float getInputLatencyMs() const override { return mInfo.inputLatencyMs; }

    watermelon_audio::BackendType getType() const override {
        return watermelon_audio::BackendType::OBOE;
    }

    bool supportsFullDuplex() const override { return true; }

    // ---- Test knobs -------------------------------------------------------

    /// The rate the "device" settled on, reported through getStreamInfo().
    void setNegotiatedSampleRate(int sampleRate) { mInfo.sampleRate = sampleRate; }

    /// The rate the engine asked for, as recorded by setSampleRate().
    int requestedSampleRate() const { return mRequestedSampleRate; }

    /// Make start() fail, so the manager never reports isRunning().
    void setStartResult(watermelon_audio::BackendResult result) { mStartResult = result; }

    /**
     * Whether the "device" will actually grant capture when asked.
     *
     * False models a denied microphone or a machine with no input: the request
     * is accepted, the stream opens, and capture still never goes live. That gap
     * is the whole reason isCaptureLive() exists separately from the request.
     */
    void setCaptureAvailable(bool available) {
        mCaptureAvailable.store(available, std::memory_order_release);
    }

    // ---- Freno de start(), para los tests del reopen asincrónico -------------

    /// A partir de acá, todo start() se queda esperando en releaseStart().
    void blockStart() {
        std::lock_guard<std::mutex> lock(mStartGateMutex);
        mStartBlocked = true;
        mStartEntered = false;
    }

    /// Espera a que un start() haya llegado de verdad al freno.
    void waitUntilStartEntered() {
        std::unique_lock<std::mutex> lock(mStartGateMutex);
        mStartEnteredCv.wait(lock, [this] { return mStartEntered; });
    }

    void releaseStart() {
        {
            std::lock_guard<std::mutex> lock(mStartGateMutex);
            mStartBlocked = false;
        }
        mStartGateCv.notify_all();
    }

    /// The capture request currently pending for the next start().
    bool fullDuplexRequested() const {
        return mFullDuplexRequested.load(std::memory_order_acquire);
    }

    /// How many times start() has been called — a reopen shows up as +1.
    int startCount() const { return mStartCount; }

    watermelon_audio::IAudioCallback* callback() const { return mCallback; }
    bool isPaused() const { return mPaused; }

private:
    watermelon_audio::StreamInfo mInfo{};
    watermelon_audio::IAudioCallback* mCallback = nullptr;
    watermelon_audio::BackendResult mStartResult = watermelon_audio::BackendResult::OK;
    std::atomic<bool> mRunning{false};

    // Freno de start(). mutable no hace falta: sólo lo tocan métodos no-const.
    std::mutex mStartGateMutex;
    std::condition_variable mStartGateCv;
    std::condition_variable mStartEnteredCv;
    bool mStartBlocked = false;
    bool mStartEntered = false;
    bool mPaused = false;
    int mRequestedSampleRate = 0;
    // Atomic: con el reopen asincronico, requestCapture() lo escribe desde el
    // thread del llamador mientras el worker lo lee adentro de start().
    std::atomic<bool> mFullDuplexRequested{false};
    // Atomic: los tests del reopen asincronico lo mueven desde otro thread
    // mientras el worker esta adentro de start(). TSan lo agarro.
    std::atomic<bool> mCaptureAvailable{true};
    int mStartCount = 0;
};

/**
 * The FakeAudioBackend created by the most recent BackendManager construction,
 * or nullptr if none has been built yet. Owned by that manager — valid only
 * while it lives.
 */
FakeAudioBackend* lastCreatedSystemBackend();

/// Forget the pointer above. Call before constructing a manager you intend to
/// query, so a stale one can never be mistaken for the fresh one.
void resetLastCreatedSystemBackend();

}  // namespace wma_test
