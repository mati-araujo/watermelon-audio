package com.watermellonstudios.audio.domain.usb

import com.watermellonstudios.audio.domain.AudioBackendType

/**
 * USB Audio events for reactive streams.
 */

/**
 * Events emitted by UsbAudioManager.
 */
sealed class UsbDeviceEvent {
    /**
     * A new USB Audio device was connected.
     */
    data class DeviceConnected(val device: UsbAudioDevice) : UsbDeviceEvent()

    /**
     * A USB Audio device was disconnected.
     */
    data class DeviceDisconnected(val deviceId: String) : UsbDeviceEvent()

    /**
     * USB permission was granted for a device.
     */
    data class PermissionGranted(val device: UsbAudioDevice) : UsbDeviceEvent()

    /**
     * USB permission was denied for a device.
     */
    data class PermissionDenied(val deviceId: String) : UsbDeviceEvent()

    /**
     * Device is now streaming audio.
     */
    data class StreamingStarted(val device: UsbAudioDevice) : UsbDeviceEvent()

    /**
     * Device stopped streaming audio.
     */
    data class StreamingStopped(val deviceId: String) : UsbDeviceEvent()

    /**
     * An error occurred with a USB device.
     */
    data class Error(val deviceId: String?, val error: UsbAudioError) : UsbDeviceEvent()

    // ==================== Compatibility Events ====================

    /**
     * A compatible USB Audio device was detected (should auto-connect).
     * Emitted when a device in the allowlist is attached, or any device in DEBUG mode.
     */
    data class CompatibleDeviceDetected(
        val device: UsbAudioDevice,
        val compatibility: UsbCompatibilityResult
    ) : UsbDeviceEvent()

    /**
     * An incompatible USB Audio device was detected.
     * Only emitted in RELEASE builds for devices not in the allowlist.
     */
    data class IncompatibleDeviceDetected(
        val device: UsbAudioDevice,
        val reason: String
    ) : UsbDeviceEvent()

    /**
     * Backend was automatically switched to USB.
     * Emitted after a compatible device is connected and backend changed.
     */
    data class BackendAutoSwitched(
        val device: UsbAudioDevice,
        val previousBackend: AudioBackendType
    ) : UsbDeviceEvent()

    /**
     * Audio was automatically switched from USB to built-in (Oboe) backend.
     * Emitted when USB device disconnection is detected during streaming
     * and the system falls back to built-in audio to maintain playback.
     */
    data object FallbackToBuiltInAudio : UsbDeviceEvent()

    // ==================== Permission Guidance Events ====================

    /**
     * A previously trusted device needs permission again.
     * This happens when Android doesn't remember the "Always use" preference.
     *
     * The UI should show a hint to the user to check "Always use for this device"
     * in the system permission dialog to avoid repeated permission requests.
     */
    data class TrustedDeviceNeedsReauthorization(
        val device: UsbAudioDevice
    ) : UsbDeviceEvent()
}

/**
 * USB Audio error types.
 */
enum class UsbAudioError(val code: Int, val message: String) {
    NONE(0, "No error"),
    DEVICE_NOT_FOUND(1, "Device not found"),
    PERMISSION_DENIED(2, "USB permission denied"),
    UNSUPPORTED_FORMAT(3, "Audio format not supported"),
    DESCRIPTOR_PARSE_ERROR(4, "Failed to parse USB descriptors"),
    TRANSFER_ERROR(5, "USB transfer error"),
    TIMEOUT(6, "USB operation timed out"),
    DEVICE_DISCONNECTED(7, "Device was disconnected"),
    LIBUSB_ERROR(8, "libusb internal error"),
    INTERNAL_ERROR(9, "Internal error"),
    NO_AUDIO_INTERFACE(10, "Device has no audio interface"),
    ALREADY_CONNECTED(11, "Device is already connected"),
    NOT_CONNECTED(12, "Device is not connected"),
    STREAMING_ERROR(13, "Streaming error"),
    INITIALIZATION_FAILED(14, "Failed to initialize USB device");

    companion object {
        fun fromCode(code: Int): UsbAudioError = entries.find { it.code == code } ?: INTERNAL_ERROR
    }
}

/**
 * Result type for USB operations.
 */
sealed class UsbResult<out T> {
    data class Success<T>(val value: T) : UsbResult<T>()
    data class Failure(val error: UsbAudioError, val message: String? = null) : UsbResult<Nothing>()

    inline fun <R> map(transform: (T) -> R): UsbResult<R> = when (this) {
        is Success -> Success(transform(value))
        is Failure -> this
    }

    inline fun onSuccess(action: (T) -> Unit): UsbResult<T> {
        if (this is Success) action(value)
        return this
    }

    inline fun onFailure(action: (UsbAudioError, String?) -> Unit): UsbResult<T> {
        if (this is Failure) action(error, message)
        return this
    }

    fun getOrNull(): T? = (this as? Success)?.value

    fun getOrThrow(): T = when (this) {
        is Success -> value
        is Failure -> throw UsbAudioException(error, message)
    }
}

/**
 * Exception for USB Audio errors.
 */
class UsbAudioException(
    val error: UsbAudioError,
    override val message: String? = error.message
) : Exception(message)
