package com.watermellonstudios.audio.domain.usb

/**
 * Compatibility status for a USB Audio device.
 */
enum class UsbCompatibilityStatus {
    /** Device is in the allowlist and fully compatible */
    COMPATIBLE,
    /** Device is not in the allowlist but allowed in DEBUG */
    DEBUG_ONLY,
    /** Device is not compatible (RELEASE build, not in allowlist) */
    INCOMPATIBLE,
    /** Device is not a USB Audio device */
    NOT_AUDIO_DEVICE
}

/**
 * Result of USB device compatibility check.
 */
data class UsbCompatibilityResult(
    val status: UsbCompatibilityStatus,
    val deviceName: String? = null,
    val reason: String = ""
) {
    val isAllowed: Boolean get() = status == UsbCompatibilityStatus.COMPATIBLE ||
            status == UsbCompatibilityStatus.DEBUG_ONLY
}
