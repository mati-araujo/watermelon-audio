/**
 * IAudioBackend.h
 *
 * Abstract interface for audio backends (Oboe, libusb, etc.)
 * This allows the AudioEngine to be agnostic about the audio I/O source.
 *
 * Design Goals:
 * - Backend-agnostic DSP processing
 * - Hot-swappable backends (USB connect/disconnect)
 * - RT-safe callback interface
 * - Unified error handling
 */

#pragma once

#include "../usb/LatencyProfile.h"

#include <cstdint>
#include <atomic>
#include <functional>
#include <string>

namespace watermelon_audio {

// =============================================================================
// Enums and Types
// =============================================================================

enum class BackendType {
    NONE = 0,
    OBOE = 1,
    LIBUSB = 2,
    SPLIT = 3,
    COREAUDIO = 4   // iOS/macOS, WA-2.4
};

enum class BackendResult {
    OK = 0,
    ERROR_DEVICE_NOT_FOUND,
    ERROR_PERMISSION_DENIED,
    ERROR_DEVICE_BUSY,
    ERROR_INVALID_CONFIG,
    ERROR_USB_INIT_FAILED,
    ERROR_STREAM_FAILED,
    ERROR_ALREADY_RUNNING,
    ERROR_NOT_INITIALIZED
};

enum class BackendError {
    NONE = 0,
    DEVICE_DISCONNECTED,
    UNDERRUN,
    OVERRUN,
    TRANSFER_ERROR,
    TIMEOUT,
    FATAL
};

/**
 * Asynchronous, backend-agnostic error notification.
 *
 * Exists so BackendManager can learn about device loss without knowing which
 * implementation reported it: a transport-specific channel (libusb error codes,
 * an Oboe stream error) is mapped to BackendError by the backend itself. The
 * message is for logging only and is not owned by the callee.
 *
 * Invoked off the audio callback — implementations must not call this from the
 * RT path.
 */
using BackendErrorCallback = std::function<void(BackendError error, const char* message)>;

enum class BackendStreamRole : uint32_t {
    NONE = 0,
    INPUT_SOURCE = 1u << 0,
    OUTPUT_SINK = 1u << 1,
    FULL_DUPLEX = (1u << 0) | (1u << 1)
};

inline BackendStreamRole operator|(BackendStreamRole lhs, BackendStreamRole rhs) {
    return static_cast<BackendStreamRole>(
        static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

inline bool hasBackendRole(BackendStreamRole roles, BackendStreamRole role) {
    return (static_cast<uint32_t>(roles) & static_cast<uint32_t>(role)) ==
           static_cast<uint32_t>(role);
}

struct BackendEndpointCapabilities {
    BackendStreamRole roles = BackendStreamRole::OUTPUT_SINK;

    // True only when this backend can provide captured frames through a
    // pull-style input source contract. This is deliberately stricter than
    // supportsFullDuplex(): Oboe can open an input stream today, but does not
    // yet bridge it into engine callbacks or an input-source adapter.
    bool hasInputSourceContract = false;

    // True when the backend's normal onAudioReady callback includes non-null
    // inputData in capture/full-duplex mode.
    bool callbackCarriesInput = false;

    // True when this backend is allowed to drive the user's DSP callback.
    // Split composition should set this true only on the output sink.
    bool drivesUserCallback = true;
};

enum class AudioFormat {
    FLOAT_32 = 0,   // Native format for DSP
    INT_16,         // Common USB format
    INT_24,         // Pro audio USB format
    INT_32
};

// =============================================================================
// Stream Configuration
// =============================================================================

struct StreamConfig {
    int sampleRate = 48000;
    int channelCount = 2;
    int framesPerBuffer = 256;
    AudioFormat format = AudioFormat::FLOAT_32;
    bool enableInput = false;   // Full-duplex capture
    bool enableOutput = true;   // Playback
};

struct StreamInfo {
    int sampleRate = 48000;
    int channelCount = 2;
    int framesPerBuffer = 256;
    AudioFormat format = AudioFormat::FLOAT_32;
    float outputLatencyMs = 0.0f;
    float inputLatencyMs = 0.0f;
    bool isFullDuplex = false;
    BackendType backendType = BackendType::NONE;

    // USB-specific info
    int usbVendorId = 0;
    int usbProductId = 0;
    std::string deviceName;
};

class IAudioInputSource {
public:
    virtual ~IAudioInputSource() = default;

    /**
     * Pull captured input frames into outputData.
     *
     * Returns the number of frames written. Implementations must be bounded
     * and must not allocate, lock, sleep, or log in a real-time pull path.
     */
    virtual int32_t readInput(float* outputData, int32_t maxFrames) = 0;
    virtual StreamInfo getInputStreamInfo() const = 0;
};

class IAudioOutputSink {
public:
    virtual ~IAudioOutputSink() = default;

    /**
     * Push output frames to the sink.
     *
     * Returns the number of frames consumed. Implementations must be bounded
     * and RT-safe when called from an audio render path.
     */
    virtual int32_t writeOutput(const float* inputData, int32_t frames) = 0;
    virtual StreamInfo getOutputStreamInfo() const = 0;
};

// =============================================================================
// Audio Callback Interface
// =============================================================================

/**
 * IAudioCallback
 *
 * Interface for receiving audio data from a backend.
 * The onAudioReady() method is called from a high-priority audio thread.
 *
 * CRITICAL: All implementations MUST be RT-safe:
 * - No memory allocations (new, malloc, std::vector::push_back)
 * - No locks that can contend
 * - No system calls (file I/O, logging, etc.)
 * - No std::shared_ptr (atomic refcount operations)
 * - Bounded execution time
 */
class IAudioCallback {
public:
    virtual ~IAudioCallback() = default;

    enum class Result {
        CONTINUE,   // Keep streaming
        STOP        // Stop the stream gracefully
    };

    /**
     * Called when audio data is ready for processing.
     *
     * @param outputData  Buffer to write output samples (interleaved stereo float).
     *                    MUST be filled with numFrames * channelCount samples.
     *                    Pre-filled with zeros if no prior data.
     *
     * @param inputData   Buffer containing input samples (interleaved stereo float).
     *                    nullptr if input is not enabled or not available.
     *                    Contains numFrames * channelCount samples.
     *
     * @param numFrames   Number of audio frames to process.
     *                    One frame = channelCount samples (e.g., 2 for stereo).
     *
     * @return Result::CONTINUE to keep streaming, Result::STOP to stop gracefully.
     *
     * Threading: Called from high-priority audio thread.
     *            Must complete within buffer duration (numFrames / sampleRate).
     */
    virtual Result onAudioReady(
        float* outputData,
        const float* inputData,
        int32_t numFrames
    ) = 0;

    /**
     * Called when an error occurs in the backend.
     *
     * This is NOT called from the RT audio thread - it's safe to:
     * - Log messages
     * - Allocate memory
     * - Update UI state
     *
     * @param error  The type of error that occurred.
     */
    virtual void onBackendError(BackendError error) = 0;

    /**
     * Called when the stream configuration changes.
     *
     * This can happen when:
     * - USB device is hot-plugged with different sample rate
     * - Backend switches (Oboe <-> libusb)
     * - Device routing changes
     *
     * NOT called from RT thread - safe to allocate/log.
     *
     * @param newInfo  The new stream configuration.
     */
    virtual void onStreamConfigChanged(const StreamInfo& newInfo) {}

    /**
     * @brief Como el de arriba, pero del stream de ENTRADA (REQ-001 S1, 1.16).
     *
     * Existe porque en un backend partido —`SplitBackend`, o sea entrada y
     * salida en streams distintos, que es para lo que existe `DriftResampler`—
     * los dos lados pueden negociar rates DISTINTOS, y `onStreamConfigChanged`
     * solo transporta el de salida.
     *
     * Antes de esto, `SplitBackend::InputCallback::onStreamConfigChanged` era
     * literalmente `(void)newInfo;`: la configuracion del stream de captura se
     * descartaba en el seam y nada aguas abajo podia enterarse. Para un afinador
     * conectado por interfaz USB —el caso principal— eso significa medir con el
     * rate equivocado.
     *
     * Default no-op: un backend duplex no lo llama, y ahi el rate de captura es
     * el mismo que el de salida.
     *
     * NO se llama desde el thread RT.
     */
    virtual void onInputStreamConfigChanged(const StreamInfo& newInfo) {}
};

// =============================================================================
// Audio Backend Interface
// =============================================================================

/**
 * IAudioBackend
 *
 * Abstract interface for audio I/O backends.
 *
 * Implementations:
 * - OboeBackend: Uses Google's Oboe library (AAudio/OpenSL ES)
 * - LibusbBackend: Direct USB Audio Class via libusb
 *
 * Lifecycle:
 * 1. Create backend instance
 * 2. Configure (setSampleRate, setBufferSize, etc.)
 * 3. setCallback()
 * 4. start()
 * 5. ... audio flows via callback ...
 * 6. stop()
 * 7. Destroy instance
 */
class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    // =========================================================================
    // Lifecycle Management
    // =========================================================================

    /**
     * Start the audio stream.
     *
     * Opens the audio device and begins calling the callback.
     * Must call setCallback() before start().
     *
     * @return BackendResult::OK on success, error code otherwise.
     */
    virtual BackendResult start() = 0;

    /**
     * Stop the audio stream.
     *
     * Stops callbacks and releases audio device resources.
     * May block briefly while flushing pending buffers.
     */
    virtual void stop() = 0;

    /**
     * Pause the audio stream (if supported).
     *
     * Callbacks stop but device remains open for quick resume.
     * Not all backends support pause - check with supportsPause().
     */
    virtual void pause() = 0;

    /**
     * Resume a paused stream.
     */
    virtual void resume() = 0;

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * Set the audio callback handler.
     *
     * MUST be called before start().
     * Can be called while stopped to change callback.
     *
     * @param callback  Pointer to callback handler. Must remain valid until stop().
     */
    virtual void setCallback(IAudioCallback* callback) = 0;

    /**
     * Subscribe to asynchronous backend errors (device loss, fatal transport
     * failures). Optional: backends that cannot fail asynchronously ignore it.
     *
     * Declared here rather than on the concrete backends so the manager can
     * arm the fallback path without naming an implementation.
     */
    virtual void setErrorCallback(BackendErrorCallback /*callback*/) {}

    /**
     * Set desired sample rate.
     *
     * Must be called before start().
     * Actual rate may differ - check getStreamInfo() after start().
     *
     * @param sampleRate  Desired sample rate in Hz (e.g., 44100, 48000, 96000)
     */
    virtual void setSampleRate(int sampleRate) = 0;

    /**
     * Set desired buffer size.
     *
     * Must be called before start().
     * Smaller = lower latency but higher CPU usage.
     * Actual size may differ - check getStreamInfo() after start().
     *
     * @param framesPerBuffer  Number of frames per callback (e.g., 128, 256, 512)
     */
    virtual void setBufferSize(int framesPerBuffer) = 0;

    /**
     * Enable or disable full-duplex mode.
     *
     * When enabled, inputData in onAudioReady() will contain captured audio.
     * Must be called before start().
     *
     * @param enable  true to enable input capture, false for output only.
     */
    virtual void setFullDuplexEnabled(bool enable) = 0;

    // =========================================================================
    // State Queries
    // =========================================================================

    /**
     * Get current stream information.
     *
     * Contains actual (not requested) values for sample rate, buffer size, etc.
     * Only valid after start() returns OK.
     *
     * @warning **Tiene que ser seguro llamarlo mientras otro thread está adentro
     *          de start() o stop().** No es una cortesía: `BackendManager` expone
     *          esto como lector en vivo y la UI lo pollea en cada frame, mientras
     *          una reapertura de captura corre en un worker propio. El manager
     *          NO puede protegerlo desde afuera — un lock alrededor congelaría el
     *          poller durante los cientos de ms que tarda abrir un stream, y un
     *          snapshot dejaría de reflejar una renegociación de sample rate.
     *          Sincronizar el estado propio es responsabilidad de cada
     *          implementación.
     *
     *          Escrito después de que el TSan del CI encontrara la carrera el
     *          2026-08-12: el estado quedaba sin sincronizar y sólo asomaba una
     *          vez cada nueve merges. La reproduce
     *          `CaptureRequestTest.ReadingStateWhileTheStreamIsBeingReopenedIsNotADataRace`.
     */
    virtual StreamInfo getStreamInfo() const = 0;

    /**
     * Check if the stream is currently running.
     *
     * @warning Mismo contrato que getStreamInfo(): seguro contra un start() o
     *          stop() concurrente.
     */
    virtual bool isRunning() const = 0;

    /**
     * Get current output latency in milliseconds.
     *
     * Only valid while running.
     */
    virtual float getOutputLatencyMs() const = 0;

    /**
     * Get current input latency in milliseconds.
     *
     * Only valid while running and full-duplex is enabled.
     */
    virtual float getInputLatencyMs() const = 0;

    /**
     * Get the total round-trip latency in milliseconds.
     *
     * inputLatency + outputLatency + processing overhead.
     */
    virtual float getRoundTripLatencyMs() const {
        return getInputLatencyMs() + getOutputLatencyMs();
    }

    /**
     * Get the backend type.
     */
    virtual BackendType getType() const = 0;

    /**
     * Check if this backend supports full-duplex (simultaneous input/output).
     */
    virtual bool supportsFullDuplex() const = 0;

    /**
     * Check if this backend supports pause/resume.
     */
    virtual bool supportsPause() const { return true; }

    /**
     * Return explicit endpoint roles for backend composition.
     *
     * This separates "can be used as an output sink" from "can provide real
     * input frames". SplitBackend must require hasInputSourceContract for its
     * input side and drivesUserCallback only on its output side.
     */
    virtual BackendEndpointCapabilities getEndpointCapabilities() const {
        BackendEndpointCapabilities caps;
        caps.roles = supportsFullDuplex()
            ? BackendStreamRole::FULL_DUPLEX
            : BackendStreamRole::OUTPUT_SINK;
        caps.hasInputSourceContract = false;
        caps.callbackCarriesInput = false;
        caps.drivesUserCallback = true;
        return caps;
    }

    // =========================================================================
    // USB-Specific (Optional)
    // =========================================================================

    /**
     * Initialize backend from an Android USB file descriptor.
     *
     * Only implemented by LibusbBackend.
     * Other backends return false.
     *
     * @param fd        File descriptor from UsbDeviceConnection.getFileDescriptor()
     * @param usbfsPath Path to the usbfs device (e.g., "/dev/bus/usb/001/002")
     *
     * @return true if initialization succeeded, false otherwise.
     */
    virtual bool initializeFromFileDescriptor(int fd, const char* usbfsPath) {
        return false;  // Default: not supported
    }

    /**
     * Get USB device information.
     *
     * Only valid for LibusbBackend.
     *
     * @param vendorId   Output: USB vendor ID
     * @param productId  Output: USB product ID
     *
     * @return true if this is a USB backend with device info available.
     */
    virtual bool getUsbDeviceInfo(int* vendorId, int* productId) const {
        return false;  // Default: not a USB backend
    }

    /**
     * Apply a USB latency profile.
     *
     * Only LibusbBackend acts on it; every other backend ignores it. Having the
     * no-op default here lets BackendManager persist and re-apply the profile
     * without a type test — the profile is manager state that must survive
     * backend recreation, so it is pushed on every (re)configuration.
     *
     * Takes effect on the next start().
     */
    virtual void setUsbLatencyProfile(usb::UsbLatencyProfile /*profile*/) {
        // Default: no USB latency knobs.
    }
};

// =============================================================================
// Utility Functions
// =============================================================================

inline const char* backendTypeToString(BackendType type) {
    switch (type) {
        case BackendType::NONE:   return "None";
        case BackendType::OBOE:   return "Oboe";
        case BackendType::LIBUSB: return "USB Audio";
        case BackendType::SPLIT:  return "Split";
        case BackendType::COREAUDIO: return "Core Audio";
        default:                  return "Unknown";
    }
}

inline const char* backendResultToString(BackendResult result) {
    switch (result) {
        case BackendResult::OK:                     return "OK";
        case BackendResult::ERROR_DEVICE_NOT_FOUND: return "Device not found";
        case BackendResult::ERROR_PERMISSION_DENIED:return "Permission denied";
        case BackendResult::ERROR_DEVICE_BUSY:      return "Device busy";
        case BackendResult::ERROR_INVALID_CONFIG:   return "Invalid configuration";
        case BackendResult::ERROR_USB_INIT_FAILED:  return "USB initialization failed";
        case BackendResult::ERROR_STREAM_FAILED:    return "Stream failed";
        case BackendResult::ERROR_ALREADY_RUNNING:  return "Already running";
        case BackendResult::ERROR_NOT_INITIALIZED:  return "Not initialized";
        default:                                    return "Unknown error";
    }
}

inline const char* backendErrorToString(BackendError error) {
    switch (error) {
        case BackendError::NONE:                return "No error";
        case BackendError::DEVICE_DISCONNECTED: return "Device disconnected";
        case BackendError::UNDERRUN:            return "Buffer underrun";
        case BackendError::OVERRUN:             return "Buffer overrun";
        case BackendError::TRANSFER_ERROR:      return "Transfer error";
        case BackendError::TIMEOUT:             return "Timeout";
        case BackendError::FATAL:               return "Fatal error";
        default:                                return "Unknown error";
    }
}

} // namespace watermelon_audio
