/**
 * BackendManager.cpp
 *
 * Implementation of the backend manager singleton.
 */

#include "BackendManager.h"
#include "PlatformBackends.h"
#include "SplitBackend.h"
#include "../platform/Logger.h"

#define LOG_TAG "BackendManager"
#undef LOGI
#undef LOGW
#undef LOGE
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, LOG_TAG, __VA_ARGS__)

namespace watermelon_audio {

// =============================================================================
// Global Instance (Phase 0D: settable, no longer static-local only)
// =============================================================================

static std::atomic<BackendManager*> g_globalInstance{nullptr};

BackendManager& BackendManager::getInstance() {
    BackendManager* instance = g_globalInstance.load(std::memory_order_acquire);
    if (instance) return *instance;

    // Fallback: create a default static instance (backward compat for JNI code
    // that calls getInstance() before WmaEngine is created)
    static BackendManager defaultInstance;
    return defaultInstance;
}

void BackendManager::setGlobalInstance(BackendManager* instance) {
    g_globalInstance.store(instance, std::memory_order_release);
}

BackendManager::BackendManager() {
    LOGI("BackendManager initialized");

    // The platform decides what this is; the manager only sees IAudioBackend.
    mSystemBackend = createSystemAudioBackend();
    if (!mSystemBackend) {
        LOGW("No built-in audio backend on this platform — selectBackend(OBOE) will fail");
    }

    // The USB backend is created on demand, when a device is handed to us.
}

BackendManager::~BackendManager() {
    // El orden importa y es la diferencia entre esto y el use-after-free que ya
    // tiene anotado `stopWithFade`: aquel **detacha** un thread que captura
    // `this` y el destructor no tiene handle sobre él.
    //
    //   1. cortar, para que un worker en su segunda pasada no reabra un stream
    //      sobre un manager que se está muriendo;
    //   2. joinear, que es lo que garantiza que nadie use `this` después;
    //   3. recién ahí stop(), porque un worker vivo podría estar arrancándolo.
    //
    // Dos cosas que NO hay que hacer acá, cada una un deadlock:
    //
    //   - tomar mReopenMutex para joinear: el worker necesita ese mismo mutex
    //     para limpiar su flag antes de salir. No hace falta tomarlo — destruir
    //     un objeto mientras otro thread lo usa ya es UB, así que el destructor
    //     es el único que puede tocar el handle;
    //   - joinear con mMutex tomado: el worker lo pide adentro de stop()/start().
    mShuttingDown.store(true, std::memory_order_release);
    if (mReopenThread.joinable()) {
        mReopenThread.join();
    }

    stop();
    LOGI("BackendManager destroyed");
}

// =============================================================================
// Backend Selection
// =============================================================================

bool BackendManager::selectBackend(BackendType type) {
    std::lock_guard<std::mutex> lock(mMutex);

    BackendType oldType = mCurrentType.load(std::memory_order_acquire);

    if (oldType == type) {
        LOGI("Backend already selected: %s", backendTypeToString(type));
        return true;
    }

    // Check if engine was running
    mWasRunning = (mActiveBackend != nullptr && mActiveBackend->isRunning());

    // Stop current backend if running
    if (mWasRunning && mActiveBackend) {
        LOGI("Stopping current backend before switch");
        mActiveBackend->stop();
    }

    // Select new backend
    IAudioBackend* newBackend = nullptr;

    switch (type) {
        case BackendType::OBOE:
            if (!mSystemBackend) {
                LOGE("No built-in audio backend available on this platform");
                return false;
            }
            newBackend = mSystemBackend.get();
            break;

        case BackendType::LIBUSB:
            if (mUsbBackend && mUsbBackendAvailable.load()) {
                newBackend = mUsbBackend.get();
            } else if (mSystemBackend) {
                LOGW("USB backend not available, falling back to the built-in backend");
                newBackend = mSystemBackend.get();
                type = BackendType::OBOE;
            } else {
                LOGE("USB backend not available and no built-in backend to fall back to");
                return false;
            }
            break;

        case BackendType::SPLIT:
            if (mSplitBackend) {
                newBackend = mSplitBackend.get();
            } else {
                LOGW("Split backend not configured");
                return false;
            }
            break;

        case BackendType::NONE:
            newBackend = nullptr;
            break;

        default:
            LOGE("Unknown backend type: %d", static_cast<int>(type));
            return false;
    }

    // Apply configuration to new backend
    if (newBackend) {
        applyConfigToBackend(newBackend);
    }

    mActiveBackend = newBackend;
    mCurrentType.store(type, std::memory_order_release);

    LOGI("Backend selected: %s", backendTypeToString(type));

    publishBackendState();

    // Notify listeners
    notifyBackendChanged(oldType, type);

    // Restart if was running
    if (mWasRunning && mActiveBackend) {
        LOGI("Restarting backend after switch");
        BackendResult result = mActiveBackend->start();
        publishBackendState();
        if (result != BackendResult::OK) {
            LOGE("Failed to restart backend: %s", backendResultToString(result));
            return false;
        }
    }

    return true;
}

IAudioBackend* BackendManager::getCurrentBackend() {
    std::lock_guard<std::mutex> lock(mMutex);
    return mActiveBackend;
}

// =============================================================================
// Callback Management
// =============================================================================

void BackendManager::setCallback(IAudioCallback* callback) {
    std::lock_guard<std::mutex> lock(mMutex);

    mCallback = callback;

    // Apply to current backend
    if (mActiveBackend) {
        mActiveBackend->setCallback(callback);
    }
}

// =============================================================================
// Lifecycle Management
// =============================================================================

BackendResult BackendManager::start() {
    std::lock_guard<std::mutex> lock(mMutex);

    if (!mActiveBackend) {
        LOGE("No backend selected");
        return BackendResult::ERROR_NOT_INITIALIZED;
    }

    if (!mCallback) {
        LOGE("No callback set");
        return BackendResult::ERROR_NOT_INITIALIZED;
    }

    // Ensure callback is set
    mActiveBackend->setCallback(mCallback);

    BackendResult result = mActiveBackend->start();

    if (result != BackendResult::OK) {
        LOGE("Failed to start backend: %s", backendResultToString(result));
    } else {
        LOGI("Backend started: %s", backendTypeToString(mCurrentType.load()));
    }

    publishBackendState();
    return result;
}

void BackendManager::stop() {
    std::lock_guard<std::mutex> lock(mMutex);

    if (mActiveBackend && mActiveBackend->isRunning()) {
        mActiveBackend->stop();
        LOGI("Backend stopped");
    }

    publishBackendState();
}

bool BackendManager::isRunning() const {
    std::unique_lock<std::mutex> lock(mMutex, std::try_to_lock);
    if (lock.owns_lock()) {
        const bool running = mActiveBackend && mActiveBackend->isRunning();
        mRunningMirror.store(running, std::memory_order_release);
        return running;
    }
    return mRunningMirror.load(std::memory_order_acquire);
}

StreamInfo BackendManager::getStreamInfo() const {
    std::unique_lock<std::mutex> lock(mMutex, std::try_to_lock);
    if (lock.owns_lock()) {
        StreamInfo info = mActiveBackend ? mActiveBackend->getStreamInfo() : StreamInfo{};
        std::lock_guard<std::mutex> mirror(mStreamInfoMutex);
        mStreamInfoMirror = info;
        return info;
    }
    std::lock_guard<std::mutex> mirror(mStreamInfoMutex);
    return mStreamInfoMirror;
}

void BackendManager::publishBackendState() {
    // Con mMutex tomado. Se lee del backend una sola vez y se reparte a los tres
    // espejos, así no pueden contarse historias distintas entre sí.
    const bool running = mActiveBackend && mActiveBackend->isRunning();
    StreamInfo info = mActiveBackend ? mActiveBackend->getStreamInfo() : StreamInfo{};

    {
        std::lock_guard<std::mutex> lock(mStreamInfoMutex);
        mStreamInfoMirror = info;
    }
    mCaptureLiveMirror.store(running && info.isFullDuplex, std::memory_order_release);
    mRunningMirror.store(running, std::memory_order_release);
}

// =============================================================================
// Configuration
// =============================================================================

void BackendManager::setSampleRate(int sampleRate) {
    std::lock_guard<std::mutex> lock(mMutex);

    mSampleRate = sampleRate;

    if (mActiveBackend) {
        mActiveBackend->setSampleRate(sampleRate);
    }
}

void BackendManager::setBufferSize(int framesPerBuffer) {
    std::lock_guard<std::mutex> lock(mMutex);

    mBufferSize = framesPerBuffer;

    if (mActiveBackend) {
        mActiveBackend->setBufferSize(framesPerBuffer);
    }
}

void BackendManager::setFullDuplexEnabled(bool enable) {
    // The mode requester never restarts a running stream: a mode change must not
    // punch an audible gap into playback.
    requestCapture(CaptureRequester::MODE, enable, /*allowRestart=*/false);
}

bool BackendManager::isCaptureLive() const {
    std::unique_lock<std::mutex> lock(mMutex, std::try_to_lock);
    if (lock.owns_lock()) {
        const bool live = mActiveBackend && mActiveBackend->isRunning() &&
                          mActiveBackend->getStreamInfo().isFullDuplex;
        mCaptureLiveMirror.store(live, std::memory_order_release);
        return live;
    }
    return mCaptureLiveMirror.load(std::memory_order_acquire);
}

BackendManager::CaptureOutcome BackendManager::requestCapture(CaptureRequester who,
                                                              bool want,
                                                              bool allowRestart) {
    // Every path that does NOT need a reopen returns from inside this block, so
    // reaching the code after it *is* the decision to reopen.
    {
        std::lock_guard<std::mutex> lock(mMutex);

        if (who == CaptureRequester::MODE) {
            mCaptureRequestedByMode = want;
        } else {
            mCaptureRequestedByInputNode = want;
        }

        const bool effective = mCaptureRequestedByMode || mCaptureRequestedByInputNode;
        mFullDuplexEnabled = effective;

        if (!mActiveBackend) {
            return CaptureOutcome::NOT_LIVE;
        }

        // Always push the request: it is what the backend reads at its next
        // start(), and applyConfigToBackend() replays it if the backend is
        // recreated. Backends that can honor a change live (CoreAudio flips
        // delivery of an already-attached capture stream) do it inside here.
        mActiveBackend->setFullDuplexEnabled(effective);

        if (!mActiveBackend->isRunning()) {
            return CaptureOutcome::NOT_LIVE;
        }

        const bool live = mActiveBackend->getStreamInfo().isFullDuplex;

        if (live == effective) {
            return live ? CaptureOutcome::LIVE : CaptureOutcome::NOT_LIVE;
        }

        if (!effective) {
            // Asked to stop capturing and the stream still carries input. Not
            // worth a restart: the backend has already stopped delivering it, so
            // the only cost is some capture work nobody consumes. Trading that
            // for an audible gap would be a bad deal.
            return CaptureOutcome::NOT_LIVE;
        }

        if (!allowRestart) {
            LOGW("Capture requested on a running stream that has none — takes "
                 "effect on the next start()");
            return CaptureOutcome::NOT_LIVE;
        }

        // Autorizado a reabrir. La generación sube ACÁ, bajo mMutex, porque es
        // lo que le dice a un worker ya corriendo que llegó un pedido nuevo
        // después de que él leyera el flag. Ver mCaptureRestartGeneration.
        ++mCaptureRestartGeneration;
    }

    if (mShuttingDown.load(std::memory_order_acquire)) {
        return CaptureOutcome::NOT_LIVE;
    }

    // Agendar FUERA de mMutex y en un thread propio. El reopen entero —stop()
    // esperando a que drenen los callbacks de RT, start() hablando por IPC con
    // el servidor de audio— se lo come el worker; este thread vuelve ya.
    std::lock_guard<std::mutex> schedule(mReopenMutex);

    if (mReopenInFlight) {
        // Ya hay uno corriendo y la generación que acabamos de subir es
        // justamente cómo se entera. No se agenda un segundo.
        return CaptureOutcome::PENDING;
    }

    // Cosechar el worker anterior, que ya terminó (mReopenInFlight es false).
    // Sin esto, std::thread::operator= sobre un thread joinable llama a
    // std::terminate. Es seguro joinear con mReopenMutex tomado: el worker
    // suelta este mutex ANTES de salir y no lo vuelve a necesitar.
    if (mReopenThread.joinable()) {
        mReopenThread.join();
    }

    mReopenInFlight = true;
    mReopenThread = std::thread([this] { runCaptureReopen(); });

    return CaptureOutcome::PENDING;
}

bool BackendManager::isCaptureRequestPending() const {
    std::lock_guard<std::mutex> lock(mReopenMutex);
    return mReopenInFlight;
}

void BackendManager::waitForCaptureRequest() {
    std::unique_lock<std::mutex> lock(mReopenMutex);
    mReopenDone.wait(lock, [this] { return !mReopenInFlight; });
}

void BackendManager::runCaptureReopen() {
    // Tope de pasadas. Cada pasada es un corte audible, así que esto no es una
    // formalidad: es lo que impide que dos botones peleándose dejen al usuario
    // con el audio entrecortado. Si se agota, se dice en el log — un tope que
    // recorta en silencio se lee como "convergió".
    constexpr int kMaxPasses = 3;

    for (int pass = 1; !mShuttingDown.load(std::memory_order_acquire); ++pass) {
        uint64_t generationBefore;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            generationBefore = mCaptureRestartGeneration;
        }

        reopenOnce();

        uint64_t generationAfter;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            generationAfter = mCaptureRestartGeneration;
        }

        // Nadie pidió nada nuevo mientras reabríamos: lo que quedó es la
        // respuesta, sea captura viva o micrófono denegado. NO se reintenta por
        // "no quedó viva" — un permiso denegado no cambia por insistir, y cada
        // reintento sería otro corte.
        if (generationAfter == generationBefore) {
            break;
        }

        if (pass >= kMaxPasses) {
            LOGW("Capture reopen gave up after %d passes — a newer request "
                 "arrived each time; the last one may not be honored",
                 pass);
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mReopenMutex);
        mReopenInFlight = false;
    }
    mReopenDone.notify_all();
}

void BackendManager::reopenOnce() {
    LOGI("Reopening the stream to add a capture path");
    stop();

    if (start() == BackendResult::OK) {
        if (!isCaptureLive()) {
            LOGW("Stream reopened but capture is still not live — most likely "
                 "microphone access was denied");
        }
        return;
    }

    // The reopen failed with capture on. Falling back to no capture is the only
    // way out that leaves the user with audio instead of silence.
    LOGE("Failed to reopen with capture — retrying without it");
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCaptureRequestedByInputNode = false;
        mFullDuplexEnabled = mCaptureRequestedByMode;
        if (mActiveBackend) {
            mActiveBackend->setFullDuplexEnabled(mFullDuplexEnabled);
        }
    }
    if (start() != BackendResult::OK) {
        LOGE("Fallback start failed too — the stream is down");
    }
}

void BackendManager::setLatencyProfile(usb::UsbLatencyProfile profile) {
    std::lock_guard<std::mutex> lock(mMutex);

    mLatencyProfile = profile;

    // Apply immediately to the existing USB backend (takes effect at its next
    // start). Persisted in mLatencyProfile so applyConfigToBackend re-applies it
    // if the backend is later recreated — same lifecycle as the streaming mode.
    if (mUsbBackend) {
        mUsbBackend->setUsbLatencyProfile(profile);
    }
}

// =============================================================================
// USB Support
// =============================================================================

bool BackendManager::initializeUsbBackend(int fd, const char* usbfsPath) {
    std::lock_guard<std::mutex> lock(mMutex);

    LOGI("Initializing USB backend: fd=%d, path=%s", fd, usbfsPath);

    // Clean up existing USB backend if any
    if (mSplitBackend) {
        mSplitBackend->stop();
        mSplitBackend.reset();
        if (mCurrentType.load(std::memory_order_acquire) == BackendType::SPLIT) {
            mActiveBackend = nullptr;
            mCurrentType.store(BackendType::NONE, std::memory_order_release);
        }
    }
    if (mUsbBackend) {
        mUsbBackend->stop();
        mUsbBackend.reset();
    }

    mUsbBackend = createUsbAudioBackend();
    if (!mUsbBackend) {
        LOGE("USB audio is not supported on this platform");
        mUsbBackendAvailable.store(false, std::memory_order_release);
        return false;
    }

    // Initialize with file descriptor from Android
    if (!mUsbBackend->initializeFromFileDescriptor(fd, usbfsPath)) {
        LOGE("Failed to initialize USB backend");
        mUsbBackend.reset();
        mUsbBackendAvailable.store(false, std::memory_order_release);
        return false;
    }

    // Set up the error callback for automatic fallback on disconnect
    mUsbBackend->setErrorCallback([this](BackendError error, const char* message) {
        LOGW("USB error callback: %s", message);
        if (error == BackendError::DEVICE_DISCONNECTED) {
            LOGI("Device disconnected detected, triggering automatic fallback");
            // Don't call fallbackToOboe() directly from callback to avoid deadlock
            // Instead, post to a handler or use a flag
            // For now, we'll notify the error callback which can be handled by JNI
            notifyError(BackendError::DEVICE_DISCONNECTED);
        }
    });

    // Apply current configuration
    applyConfigToBackend(mUsbBackend.get());

    mUsbBackendAvailable.store(true, std::memory_order_release);
    LOGI("USB backend initialized successfully");

    return true;
}

bool BackendManager::createSplitBackend(BackendType inputType, BackendType outputType) {
    std::lock_guard<std::mutex> lock(mMutex);

    IAudioBackend* input = resolveBackendForSplit(inputType);
    IAudioBackend* output = resolveBackendForSplit(outputType);

    if (!input || !output || input == output) {
        LOGW("Cannot create Split backend: invalid endpoints input=%d output=%d",
             static_cast<int>(inputType), static_cast<int>(outputType));
        return false;
    }

    const bool wasRunning = (mActiveBackend != nullptr && mActiveBackend->isRunning());
    if (wasRunning && mActiveBackend) {
        mActiveBackend->stop();
    }

    mSplitBackend = std::make_unique<SplitBackend>(*input, *output);
    applyConfigToBackend(mSplitBackend.get());

    if (wasRunning) {
        mActiveBackend = mSplitBackend.get();
        mCurrentType.store(BackendType::SPLIT, std::memory_order_release);
        BackendResult result = mActiveBackend->start();
        if (result != BackendResult::OK) {
            LOGE("Failed to start Split backend: %s", backendResultToString(result));
            mSplitBackend.reset();
            mActiveBackend = nullptr;
            mCurrentType.store(BackendType::NONE, std::memory_order_release);
            return false;
        }
    }

    LOGI("Split backend configured: input=%s output=%s",
         backendTypeToString(inputType), backendTypeToString(outputType));
    return true;
}

void BackendManager::fallbackToOboe() {
    LOGI("Falling back to Oboe backend");

    mUsbBackendAvailable.store(false, std::memory_order_release);

    // Switch to Oboe
    selectBackend(BackendType::OBOE);

    // Clean up LibUSB backend
    if (mSplitBackend) {
        mSplitBackend->stop();
        mSplitBackend.reset();
    }
    if (mUsbBackend) {
        mUsbBackend->stop();
        mUsbBackend.reset();
    }
}

LibusbBackend* BackendManager::getLibusbBackend() {
    std::lock_guard<std::mutex> lock(mMutex);
    return asLibusbBackend(mUsbBackend.get());
}

// =============================================================================
// Internal Helpers
// =============================================================================

void BackendManager::notifyBackendChanged(BackendType oldType, BackendType newType) {
    if (mOnBackendChanged) {
        // Call outside lock to prevent deadlock
        auto callback = mOnBackendChanged;
        // Note: We're inside the lock here, be careful with callback
        // In production, consider posting to a queue
        callback(oldType, newType);
    }
}

void BackendManager::notifyError(BackendError error) {
    if (mOnError) {
        mOnError(error);
    }
}

void BackendManager::applyConfigToBackend(IAudioBackend* backend) {
    if (!backend) return;

    if (mCallback) {
        backend->setCallback(mCallback);
    }
    if (mSampleRate > 0) {
        backend->setSampleRate(mSampleRate);
    }
    if (mBufferSize > 0) {
        backend->setBufferSize(mBufferSize);
    }
    backend->setFullDuplexEnabled(mFullDuplexEnabled);

    // USB latency profile (Fase 1). Re-applied here so a freshly created or
    // reactivated USB backend picks up the persisted profile. Pushed
    // unconditionally: the interface default is a no-op, so backends without
    // USB latency knobs ignore it and no type test is needed.
    backend->setUsbLatencyProfile(mLatencyProfile);
}

IAudioBackend* BackendManager::resolveBackendForSplit(BackendType type) const {
    switch (type) {
        case BackendType::OBOE:
            return mSystemBackend.get();
        case BackendType::LIBUSB:
            return (mUsbBackend && mUsbBackendAvailable.load(std::memory_order_acquire))
                ? mUsbBackend.get()
                : nullptr;
        default:
            return nullptr;
    }
}

} // namespace watermelon_audio
