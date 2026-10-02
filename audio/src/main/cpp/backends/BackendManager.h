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
#include <utility>

namespace watermelon_audio {

// Forward declarations. The manager never names a concrete backend beyond
// getLibusbBackend()'s return type — see PlatformBackends.h.
class LibusbBackend;
class SplitBackend;

/**
 * REQ-050 S2 (AC-050.3) — por qué no puede arrancar el streaming USB, con su causa.
 *
 * `nativeStartUsbStreamingWithMode` devolvía un `jboolean`, así que "sin motor", "sin
 * callback", "sin device" y "libusb no arrancó" llegaban a Kotlin como el mismo
 * STREAMING_ERROR genérico. Los valores 0..6 cruzan el JNI como `jint` y Kotlin los mapea
 * uno por uno (`UsbStreamStartStatus` en AudioNativeBridge.kt): **no se renumeran**.
 *
 * `PROCEED` no cruza: es "las precondiciones están, ahora arrancá", y el resultado final
 * lo da `LibusbBackend::start()` (OK o START_FAILED).
 */
enum class UsbStreamStartStatus : int {
    PROCEED = -1,
    OK = 0,
    NOT_INITIALIZED = 1,
    NO_ENGINE = 2,
    NO_BACKEND = 3,
    NO_CALLBACK = 4,
    INVALID_MODE = 5,
    START_FAILED = 6,
};

/** Los hechos que la JNIEXPORT junta antes de arrancar el streaming USB. */
struct UsbStreamStartFacts {
    bool alreadyStreaming = false;
    bool engineExists = false;
    bool deviceInitialized = false;
    bool backendPresent = false;
    bool modeValid = false;
    bool backendHasCallback = false;
};

/**
 * Decide qué causa nombrar. Pura y sin estado para poder afirmarla en la suite de host,
 * donde no hay backend libusb (test_usb_stream_start.cpp).
 *
 * El orden es el de las causas de raíz: sin motor no hay callback ni manager propio, así
 * que se nombra el motor antes que el device, y el device antes que el backend.
 * "Ya transmitiendo" va primero porque era el contrato previo: éxito sin tocar nada.
 */
inline UsbStreamStartStatus classifyUsbStreamStart(const UsbStreamStartFacts& f) noexcept {
    if (f.alreadyStreaming) return UsbStreamStartStatus::OK;
    if (!f.engineExists) return UsbStreamStartStatus::NO_ENGINE;
    if (!f.deviceInitialized) return UsbStreamStartStatus::NOT_INITIALIZED;
    if (!f.backendPresent) return UsbStreamStartStatus::NO_BACKEND;
    if (!f.modeValid) return UsbStreamStartStatus::INVALID_MODE;
    if (!f.backendHasCallback) return UsbStreamStartStatus::NO_CALLBACK;
    return UsbStreamStartStatus::PROCEED;
}

/**
 * REQ-050 S3 (AC-050.8, D17) — what a manual altsetting / clock source request means.
 *
 * The USB test runner renegotiates each row with `selectAltsetting`/`selectClockSource`,
 * and the backend keeps that selection for every later start. To hand the consumer back
 * the automatic choice, the two existing JNI entry points take a sentinel: exactly
 * (-1, -1, -1) for the altsetting and 0 for the clock source. Any other out-of-range value
 * is still a rejection, so a miscomputed negative index can never read as "automatic".
 * Pure, so the host suite can assert it (test_usb_selection_request.cpp).
 */
enum class UsbSelectionRequest : int {
    SELECT = 0,
    CLEAR = 1,
    REJECT = 2,
};

inline UsbSelectionRequest classifyAltsettingRequest(int interfaceNumber, int alternateSetting,
                                                     int formatIndex) noexcept {
    if (interfaceNumber == -1 && alternateSetting == -1 && formatIndex == -1) {
        return UsbSelectionRequest::CLEAR;
    }
    if (interfaceNumber < 0 || alternateSetting < 0 || formatIndex < 0) {
        return UsbSelectionRequest::REJECT;
    }
    return UsbSelectionRequest::SELECT;
}

inline UsbSelectionRequest classifyClockSourceRequest(int clockSourceId) noexcept {
    if (clockSourceId == 0) return UsbSelectionRequest::CLEAR;
    if (clockSourceId < 0 || clockSourceId > 255) return UsbSelectionRequest::REJECT;
    return UsbSelectionRequest::SELECT;
}

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
     * Stream info del backend activo, leída EN VIVO.
     *
     * @warning Toma `mMutex` y, anidado, el candado de stream info del backend: es un
     *          lector de CONTROL y **no se puede llamar desde el hilo de audio**. El
     *          nombre es único en el árbol a propósito (MINI-033): se llamaba
     *          `getStreamInfo()`, y como ese nombre tiene muchas definiciones el walker
     *          de `check-rt-safety.py` no seguía la llamada — el hilo RT de captura llegó
     *          a tomar estos dos mutex por bloque con el lint en verde. Renombrarla es lo
     *          que hace que reintroducir esa cadena salga ROJO.
     */
    StreamInfo activeStreamInfo() const;

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
     * ## Nada de esto bloquea al que llama
     *
     * Ni siquiera un segundo `wma_input_start()` / `wma_input_stop()` con una
     * reapertura en curso: anotar el pedido sólo necesita `mMutex`, que es corto
     * por construcción (ver `mOpMutex`). Los setters del backend que se tocan acá
     * tampoco bloquean — el flag de full-duplex es atómico en CoreAudio y en
     * Split justamente por esto.
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
     *
     * 🔴 **Devuelve lo que el pedido logro, y no es cosmetico** (REQ-045, D3): hasta
     * el 2026-09-28 descartaba el `CaptureOutcome` de [requestCapture] y era `void`,
     * asi que rio arriba —hasta la C API y Kotlin— pedir captura y que no pasara nada
     * era indistinguible de pedirla y que pasara.
     *
     * @return el mismo tri-estado de [requestCapture]: LIVE / NOT_LIVE cuando la
     *         respuesta se supo sin reabrir, PENDING cuando hay una reapertura en
     *         vuelo.
     */
    CaptureOutcome setFullDuplexEnabled(bool enable);

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
     * MINI-042 (M1, D5) — acceso al LibusbBackend CON ALCANCE, para lecturas y
     * configuraciones CORTAS.
     *
     * Corre `fn(LibusbBackend*)` con `mMutex` tomado; el puntero es nullptr si no
     * hay backend USB. Antes habia un `getLibusbBackend()` que devolvia el puntero
     * crudo DESPUES de soltar el lock, y `fallbackToOboe()` lo destruia mientras la
     * JNI lo leia: un SIGSEGV en el proceso del consumidor.
     *
     * El puntero no puede salir de `fn`. Y `fn`:
     *   - no puede volver a entrar al manager (mMutex no es recursivo);
     *   - no puede hacer nada lento: este es el lock que pregunta Main. Arrancar,
     *     parar o elegir altsetting/reloj (que toman el mutex del backend con
     *     lock(), y por eso esperan a un start() en curso) van por
     *     withLibusbBackendLifecycle().
     */
    template <typename Fn>
    decltype(auto) withLibusbBackend(Fn&& fn) {
        std::lock_guard<std::mutex> lock(mMutex);
        return std::forward<Fn>(fn)(usbBackendLocked());
    }

    /**
     * MINI-042 (M1, D5) — acceso al LibusbBackend para operaciones de CICLO DE
     * VIDA: start(), stop(), la seleccion de altsetting y de reloj.
     *
     * Corre `fn(LibusbBackend*)` con `mOpMutex` tomado y `mMutex` LIBRE: el lock
     * que se sostiene alrededor de la llamada lenta es el de operaciones, nunca el
     * de estado (ver mOpMutex). Asi un start() USB no le traba la mano a Main, que
     * sigue leyendo por withLibusbBackend(). El backend no se puede destruir
     * mientras `fn` corre porque todo camino que lo destruye —fallbackToOboe(),
     * initializeUsbBackend()— toma mOpMutex primero.
     *
     * `fn` no puede llamar a start()/stop()/selectBackend()/fallbackToOboe()/
     * initializeUsbBackend() del manager: todos toman mOpMutex, que no es
     * recursivo.
     */
    template <typename Fn>
    decltype(auto) withLibusbBackendLifecycle(Fn&& fn) {
        std::lock_guard<std::mutex> op(mOpMutex);
        LibusbBackend* backend = nullptr;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            backend = usbBackendLocked();
        }
        return std::forward<Fn>(fn)(backend);
    }

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
    /**
     * Serializa las operaciones de ciclo de vida —start, stop, selectBackend— y
     * es el único que se sostiene **alrededor de la llamada lenta al backend**.
     *
     * Existe porque `mMutex` no puede hacerlo. Abrir un stream habla por IPC con
     * el servidor de audio del sistema y puede tardar cientos de ms; retener ahí
     * el mutex que necesita cualquier lectura de estado congela a todo el que
     * pregunte, que en una app con UI es el main thread en cada frame. Con los
     * dos separados, una reapertura no le bloquea la mano a nadie: ni a los
     * lectores, ni a un `wma_input_stop()` que llegue en el medio.
     *
     * **Orden de lock, sin excepciones: mOpMutex → mMutex.** Nunca al revés, y
     * nunca `mMutex` tomado alrededor de algo que pueda bloquear.
     *
     * Lo único que sí espera acá es otra operación de ciclo de vida —cambiar de
     * backend a mitad de una reapertura, por ejemplo—, y eso es correcto: son
     * mutuamente excluyentes por naturaleza.
     */
    std::mutex mOpMutex;

    /// Estado. Secciones críticas cortas y **jamás** alrededor de una llamada
    /// que pueda bloquear.
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

    /// Atomic para poder consultarse **sin ningún lock** desde requestCapture();
    /// se escribe bajo mReopenMutex porque además es el predicado del condvar.
    std::atomic<bool> mReopenInFlight{false};

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

    /// Cuerpo del worker: pasadas hasta converger, con tope.
    void runCaptureReopen();

    /// Una pasada: stop + start, con el fallback a "sin captura" si falla.
    void reopenOnce();

    /// El LibusbBackend detras de mUsbBackend, o nullptr. Requiere mMutex tomado.
    LibusbBackend* usbBackendLocked() const;

    /// Cuerpo de selectBackend(). Requiere mOpMutex tomado (y mMutex LIBRE): lo
    /// usa fallbackToOboe() para cambiar de backend y desenganchar el USB sin
    /// soltar mOpMutex en el medio.
    bool selectBackendOpLocked(BackendType type);

    /// MINI-042 (D6): el reproductor de host adopta un LibusbBackend real como
    /// mUsbBackend. Definido sólo en el test.
    friend struct BackendManagerTestAccess;

    // Internal helpers
    void notifyBackendChanged(BackendType oldType, BackendType newType);
    void notifyError(BackendError error);
    void applyConfigToBackend(IAudioBackend* backend);
    IAudioBackend* resolveBackendForSplit(BackendType type) const;
};

} // namespace watermelon_audio
