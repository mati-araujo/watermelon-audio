/**
 * BackendManager.h
 *
 * Manages audio backend selection and lifecycle.
 *
 * Responsibilities:
 * - Create and manage backend instances (Oboe, LibUSB)
 * - Handle backend switching (USB connect/disconnect)
 * - Provide automatic fallback to Oboe if USB fails
 * - Thread-safe backend access
 *
 * Usage:
 *   auto& manager = BackendManager::getInstance();
 *   manager.setCallback(&myCallback);
 *   manager.selectBackend(BackendType::OBOE);
 *   manager.start();
 *
 * USB Flow:
 *   1. UsbAudioManager.kt detects USB device
 *   2. JNI calls initializeUsbBackend(fd, usbfsPath)
 *   3. BackendManager creates LibusbBackend (future)
 *   4. On disconnect, fallbackToOboe() is called
 */

#pragma once

#include "IAudioBackend.h"
#include "../usb/LatencyProfile.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <functional>

namespace watermelon_audio {

// Forward declarations. The manager never names a concrete backend beyond
// getLibusbBackend()'s return type — see PlatformBackends.h.
class LibusbBackend;
class SplitBackend;

/**
 * BackendManager
 *
 * Manager for audio backends. Constructible (Phase 0D: no longer singleton-only).
 * WmaEngine creates and owns its BackendManager instance.
 *
 * Thread Safety:
 * - getInstance(): Thread-safe, returns global instance
 * - selectBackend(): Thread-safe with mutex
 * - getCurrentBackend(): Returns pointer, caller must not store long-term
 * - start/stop: Thread-safe with mutex
 */
class BackendManager {
public:
    BackendManager();
    ~BackendManager();

    /**
     * Get the global instance (for legacy code that hasn't been migrated).
     * If setGlobalInstance() was called, returns that. Otherwise creates a default.
     */
    static BackendManager& getInstance();

    /**
     * Set the global instance pointer. Called by WmaEngine on creation.
     * Pass nullptr to clear (called on WmaEngine destruction).
     * Does NOT take ownership — caller must ensure lifetime.
     */
    static void setGlobalInstance(BackendManager* instance);

    // Prevent copy/move
    BackendManager(const BackendManager&) = delete;
    BackendManager& operator=(const BackendManager&) = delete;

    // =========================================================================
    // Backend Selection
    // =========================================================================

    /**
     * Select which backend to use.
     *
     * If the engine is running, it will be stopped before switching
     * and restarted with the new backend.
     *
     * @param type Backend type to use
     * @return true if backend was successfully selected
     */
    bool selectBackend(BackendType type);

    /**
     * Get the currently selected backend type.
     */
    BackendType getCurrentType() const {
        return mCurrentType.load(std::memory_order_acquire);
    }

    /**
     * Get the current backend instance.
     *
     * @return Pointer to current backend, or nullptr if none selected.
     * @warning Do not store this pointer - it may become invalid after backend switch.
     */
    IAudioBackend* getCurrentBackend();

    // =========================================================================
    // Callback Management
    // =========================================================================

    /**
     * Set the audio callback for all backends.
     *
     * This must be called before start().
     * The callback will be passed to whichever backend is active.
     *
     * @param callback Pointer to callback handler
     */
    void setCallback(IAudioCallback* callback);

    /**
     * Get the current callback.
     */
    IAudioCallback* getCallback() const { return mCallback; }

    // =========================================================================
    // Lifecycle Management
    // =========================================================================

    /**
     * Start the current backend.
     *
     * @return Result of start operation
     */
    BackendResult start();

    /**
     * Stop the current backend.
     */
    void stop();

    /**
     * Check if the current backend is running.
     */
    bool isRunning() const;

    /**
     * Get stream info from the current backend.
     */
    StreamInfo getStreamInfo() const;

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * Set sample rate for backends.
     * Must be called before start().
     */
    void setSampleRate(int sampleRate);

    /**
     * Set buffer size for backends.
     * Must be called before start().
     */
    void setBufferSize(int framesPerBuffer);

    /**
     * Who is asking for capture.
     *
     * Two independent callers want input, and they must not overwrite each
     * other: the mode system (INPUT_FX needs input) and an explicit
     * wma_input_start(). A single bool would make the last writer win — turning
     * the mode off would kill a capture the app had started on purpose. The
     * effective request is the OR of both bits.
     */
    enum class CaptureRequester {
        MODE,        ///< The mode system: setFullDuplexEnabled()
        INPUT_NODE,  ///< An explicit wma_input_start() / wma_input_stop()
    };

    /**
     * What a capture request achieved, as of the moment it returned.
     *
     * Three values and not a bool because a reopen no longer finishes before the
     * call does — see [requestCapture]. Collapsing PENDING into NOT_LIVE would
     * make "still opening" indistinguishable from "the user denied the
     * microphone", which is the one distinction the whole input path exists to
     * report.
     */
    enum class CaptureOutcome {
        LIVE,      ///< capture is delivering frames right now
        NOT_LIVE,  ///< it is not, and nothing is in flight to change that
        PENDING,   ///< a reopen is running; poll isCaptureLive()
    };

    /**
     * Register (or withdraw) one requester's need for captured input.
     *
     * @param who          which requester is speaking
     * @param want         whether that requester needs capture
     * @param allowRestart permission to restart a RUNNING stream in order to
     *                     honor the request. Every backend reads its full-duplex
     *                     flag at start() — Oboe at OboeBackend.cpp:63, CoreAudio
     *                     when it attaches the sink node — so a stream already
     *                     running cannot grow a capture path without reopening.
     *                     Restarting is audible, so it is opt-in: the mode path
     *                     passes false (it must never punch a gap into playback),
     *                     an explicit input-start passes true (the caller asked
     *                     for the microphone and a brief gap is the price).
     *
     * @return LIVE / NOT_LIVE when the answer was known without reopening.
     *         **PENDING when a reopen was scheduled**: the stream is being torn
     *         down and reopened on a worker thread, and the caller's thread
     *         returns immediately.
     *
     * ## Por qué el reopen no corre en el thread del llamador
     *
     * Reabrir un stream es caro y **puede colgarse**: `stop()` espera a que
     * drenen los callbacks de RT, y `start()` hace IPC al servidor de audio del
     * sistema. En iOS eso se midió colgando indefinidamente adentro de
     * `[AVAudioSession setActive:]`. El llamador de `wma_input_start()` es, en
     * cualquier app con UI, el **main thread**: bloquearlo ahí son cientos de ms
     * en el mejor caso y un watchdog kill en el peor.
     *
     * ## Residual conocido: pedir captura DURANTE un reopen sí bloquea
     *
     * El worker retiene `mMutex` toda la reapertura, y la primera parte de esta
     * función lo necesita para anotar el pedido. O sea que un segundo
     * `wma_input_start()` / `wma_input_stop()` mientras hay un reopen en curso se
     * bloquea hasta que termine.
     *
     * **Lo que NO bloquea, que es lo que la UI hace en cada frame:**
     * `isRunning()`, `getStreamInfo()` e `isCaptureLive()` usan `try_lock` y
     * caen al último valor publicado. Sin eso, mover el reopen a un thread no
     * habría servido de nada.
     *
     * Sacar también este residual pide que `start()`/`stop()` dejen de tener
     * `mMutex` tomado alrededor de la llamada al backend, que es una
     * reestructuración de la concurrencia del manager y va en su propio ticket.
     */
    CaptureOutcome requestCapture(CaptureRequester who, bool want, bool allowRestart);

    /** Whether a scheduled reopen is still running. */
    bool isCaptureRequestPending() const;

    /**
     * Block until any scheduled reopen has finished.
     *
     * **Nunca desde el thread de audio ni desde el de UI** — es justo el bloqueo
     * que [requestCapture] existe para no hacer. Está para los tests y para un
     * llamador que ya esté en un thread de fondo y prefiera esperar.
     */
    void waitForCaptureRequest();

    /**
     * Enable/disable full-duplex mode — the [CaptureRequester::MODE] requester.
     *
     * Applies at the next start(); never restarts a running stream. See
     * requestCapture() for why.
     */
    void setFullDuplexEnabled(bool enable);

    /**
     * Whether the active backend is actually delivering captured frames.
     *
     * Distinct from the request: capture can be asked for and not happen (no
     * microphone permission, no input device).
     */
    bool isCaptureLive() const;

    /**
     * Select the USB latency profile (Fase 1). Persisted on the manager so it
     * survives backend recreation and is re-applied to the LibusbBackend each
     * time it is (re)configured — same lifecycle as the streaming mode. Takes
     * effect on the next USB stream start.
     */
    void setLatencyProfile(usb::UsbLatencyProfile profile);

    // =========================================================================
    // USB Support (Future)
    // =========================================================================

    /**
     * Initialize USB backend from Android file descriptor.
     *
     * Called from JNI when a USB audio device is connected.
     * Will switch from Oboe to LibUSB backend automatically.
     *
     * @param fd         File descriptor from UsbDeviceConnection
     * @param usbfsPath  Path to usbfs device (e.g., "/dev/bus/usb/001/002")
     * @return true if USB backend was initialized successfully
     */
    bool initializeUsbBackend(int fd, const char* usbfsPath);

    /**
     * Create an internal split backend from existing managed backends.
     *
     * The split backend is opt-in and does not take ownership of the selected
     * endpoints. It is destroyed before either endpoint is reset.
     */
    bool createSplitBackend(BackendType inputType, BackendType outputType);

    /**
     * Fallback to Oboe backend.
     *
     * Called when USB device is disconnected or USB backend fails.
     * Will attempt to maintain audio continuity.
     */
    void fallbackToOboe();

    /**
     * Check if USB backend is available.
     *
     * @return true if USB backend was successfully initialized
     */
    bool isUsbBackendAvailable() const {
        return mUsbBackendAvailable.load(std::memory_order_acquire);
    }

    /**
     * Get direct access to LibusbBackend.
     *
     * Used for USB-specific operations like getting transfer stats.
     * Only valid after successful initializeUsbBackend().
     *
     * @return Pointer to LibusbBackend, or nullptr if not available.
     */
    LibusbBackend* getLibusbBackend();

    // =========================================================================
    // Event Callbacks
    // =========================================================================

    using BackendChangedCallback = std::function<void(BackendType oldType, BackendType newType)>;
    using ErrorCallback = std::function<void(BackendError error)>;

    /**
     * Set callback for backend changes.
     */
    void setOnBackendChanged(BackendChangedCallback callback) {
        std::lock_guard<std::mutex> lock(mMutex);
        mOnBackendChanged = std::move(callback);
    }

    /**
     * Set callback for backend errors.
     */
    void setOnError(ErrorCallback callback) {
        std::lock_guard<std::mutex> lock(mMutex);
        mOnError = std::move(callback);
    }

private:
    // Mutex for thread-safe operations
    mutable std::mutex mMutex;

    // Backend instances. Held as IAudioBackend so this header stays free of
    // Oboe and libusb: which implementations exist is decided once, in
    // PlatformBackends.cpp. Null where the platform provides none.
    //
    // "System" is the platform's built-in audio path — Oboe on Android,
    // CoreAudio on iOS (WA-2.4). BackendType::OBOE remains its public name
    // because that value is mirrored by the Kotlin enum and the JNI encoding.
    std::unique_ptr<IAudioBackend> mSystemBackend;
    std::unique_ptr<IAudioBackend> mUsbBackend;

    // SplitBackend is portable — it composes two IAudioBackends and pulls in no
    // platform SDK — so it is held by its concrete type.
    std::unique_ptr<SplitBackend> mSplitBackend;

    // Current active backend
    IAudioBackend* mActiveBackend = nullptr;
    std::atomic<BackendType> mCurrentType{BackendType::NONE};

    // Configuration
    IAudioCallback* mCallback = nullptr;
    int mSampleRate = 0;
    int mBufferSize = 0;
    // Effective capture request — the OR of the two requesters below. Kept as a
    // member (rather than recomputed) because applyConfigToBackend() replays it
    // onto a backend that was created or swapped later.
    bool mFullDuplexEnabled = false;
    bool mCaptureRequestedByMode = false;
    bool mCaptureRequestedByInputNode = false;
    usb::UsbLatencyProfile mLatencyProfile = usb::UsbLatencyProfile::SAFE;

    // USB state
    std::atomic<bool> mUsbBackendAvailable{false};

    // Event callbacks
    BackendChangedCallback mOnBackendChanged;
    ErrorCallback mOnError;

    // Was running before backend switch?
    bool mWasRunning = false;

    // ---- Reopen asincrónico de la captura -----------------------------------
    //
    // Mutex propio, deliberadamente separado de mMutex: el worker toma mMutex
    // para stop()/start(), así que compartirlo sería un deadlock inmediato.
    //
    // **Regla de orden: nunca tomar mMutex teniéndo mReopenMutex, ni al revés.**
    // Los dos se usan secuencialmente, nunca anidados.
    mutable std::mutex mReopenMutex;
    std::condition_variable mReopenDone;
    std::thread mReopenThread;
    bool mReopenInFlight = false;

    /**
     * Sube cada vez que una petición **autoriza un reopen** (INPUT_NODE + want +
     * allowRestart). El worker la mira antes y después de cada pasada: si cambió,
     * es que llegó un pedido mientras él ya estaba pasado del punto donde
     * `start()` lee el flag, y hay que dar otra vuelta.
     *
     * Sube **sólo** en ese caso, no en cualquier cambio de estado. Un retiro no
     * autoriza reabrir —eso metería el gap audible que el diseño evita— y un
     * micrófono denegado no cambia la generación, así que no se reintenta en
     * bucle contra un permiso que nunca va a llegar.
     *
     * Vive bajo mMutex.
     *
     * > [!NOTE]
     * > **Sin test determinista, y con el porqué.** Para ejercitar esta rama el
     * > pedido nuevo tiene que llegar con mReopenInFlight todavía en true, y un
     * > llamador que pide captura durante un reopen se queda bloqueado en mMutex
     * > hasta que el worker está por terminar (ver el residual de abajo). Casi
     * > siempre gana el worker y el pedido termina agendando uno nuevo, que
     * > converge por otro camino. La rama se queda igual: sin ella, el caso en
     * > que sí gana el pedido pierde la petición en silencio.
     */
    uint64_t mCaptureRestartGeneration = 0;

    /**
     * Cortado en el destructor **antes** de joinear. Sin esto, un worker en su
     * segunda pasada podría reabrir un stream sobre un manager que se está
     * destruyendo.
     */
    std::atomic<bool> mShuttingDown{false};

    // ---- Último estado conocido, para lecturas que no pueden bloquearse ------
    //
    // **El worker retiene mMutex durante TODO el reopen** — `BackendManager::start()`
    // lo toma y adentro llama al `start()` del backend, que en iOS habla por IPC con
    // el servidor de audio. Sin esto, mover el reopen a un thread propio no serviría
    // de nada: la UI pollea `getStreamInfo()` e `isRunning()` en cada frame y se
    // quedaría colgada en el mutex igual que antes, sólo que en otra llamada.
    //
    // Los tres lectores usan `try_lock`, y el orden importa:
    //
    //   - **mutex libre → respuesta en vivo**, leída del backend. Esto no es un
    //     detalle: un device puede renegociar el sample rate sin que el motor
    //     reinicie, y `currentSampleRate()` tiene que verlo. Un snapshot puro
    //     rompe esa invariante — la pinchó `FollowsTheBackendAcrossARenegotiation`.
    //   - **mutex tomado → hay un reopen en curso → último valor publicado.** Es
    //     lo mejor que existe en ese momento y además es fiel: durante la
    //     reapertura el stream está de verdad caído, y eso es lo que dice.
    //
    // Son `mutable` porque los lectores son const y refrescan de paso.
    mutable std::atomic<bool> mRunningMirror{false};
    mutable std::atomic<bool> mCaptureLiveMirror{false};

    /// Mutex propio y **jamás** tomado alrededor de una llamada al backend: lo único
    /// que protege es la copia del struct, que son nanosegundos.
    mutable std::mutex mStreamInfoMutex;
    mutable StreamInfo mStreamInfoMirror{};

    /// Refresca los tres espejos desde el backend activo. Con mMutex tomado.
    void publishBackendState();

    /// Cuerpo del worker: pasadas hasta converger, con tope.
    void runCaptureReopen();

    /// Una pasada: stop + start, con el fallback a "sin captura" si falla.
    void reopenOnce();

    // Internal helpers
    void notifyBackendChanged(BackendType oldType, BackendType newType);
    void notifyError(BackendError error);
    void applyConfigToBackend(IAudioBackend* backend);
    IAudioBackend* resolveBackendForSplit(BackendType type) const;
};

} // namespace watermelon_audio
