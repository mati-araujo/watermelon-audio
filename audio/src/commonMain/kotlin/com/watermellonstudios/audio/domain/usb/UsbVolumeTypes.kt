package com.watermellonstudios.audio.domain.usb

/**
 * USB volume control mode.
 */
enum class UsbVolumeMode {
    /** Using USB Feature Unit hardware control */
    HARDWARE,
    /** Software volume scaling (digital) */
    DIGITAL,
    /** No volume control available */
    NONE
}

/**
 * USB volume capabilities from native layer.
 */
data class UsbVolumeCapabilities(
    val hasOutputVolume: Boolean = false,
    val hasInputVolume: Boolean = false,
    val hasOutputMute: Boolean = false,
    val hasInputMute: Boolean = false,
    val outputVolumeMode: UsbVolumeMode = UsbVolumeMode.NONE,
    val inputVolumeMode: UsbVolumeMode = UsbVolumeMode.NONE,
    val outputMinDb: Float = -96f,
    val outputMaxDb: Float = 0f,
    val inputMinDb: Float = -96f,
    val inputMaxDb: Float = 0f
) {
    companion object {
        val NONE = UsbVolumeCapabilities()

        /**
         * Parse capabilities from native FloatArray.
         *
         * @param array FloatArray from AudioNativeBridge.getUsbVolumeCapabilities()
         * @return Parsed capabilities or NONE if null/invalid
         */
        fun fromNativeArray(array: FloatArray?): UsbVolumeCapabilities {
            if (array == null || array.size < 10) return NONE

            val hasOutputVolume = array[0] > 0.5f
            val hasInputVolume = array[1] > 0.5f
            val isHardwareOutput = array[4] > 0.5f
            val isHardwareInput = array[5] > 0.5f

            return UsbVolumeCapabilities(
                hasOutputVolume = hasOutputVolume,
                hasInputVolume = hasInputVolume,
                hasOutputMute = array[2] > 0.5f,
                hasInputMute = array[3] > 0.5f,
                outputVolumeMode = when {
                    !hasOutputVolume -> UsbVolumeMode.NONE
                    isHardwareOutput -> UsbVolumeMode.HARDWARE
                    else -> UsbVolumeMode.DIGITAL
                },
                inputVolumeMode = when {
                    !hasInputVolume -> UsbVolumeMode.NONE
                    isHardwareInput -> UsbVolumeMode.HARDWARE
                    else -> UsbVolumeMode.DIGITAL
                },
                outputMinDb = array[6],
                outputMaxDb = array[7],
                inputMinDb = array[8],
                inputMaxDb = array[9]
            )
        }
    }

    /**
     * Check if any volume control is available.
     */
    val hasAnyVolumeControl: Boolean
        get() = hasOutputVolume || hasInputVolume
}

/**
 * USB volume state for UI.
 */
data class UsbVolumeState(
    /** Output volume 0.0 - 1.0 */
    val outputVolume: Float = 1.0f,
    /** Input volume 0.0 - 1.0 */
    val inputVolume: Float = 1.0f,
    /** Output muted state */
    val outputMuted: Boolean = false,
    /** Input muted state */
    val inputMuted: Boolean = false,
    /** Volume capabilities from device */
    val capabilities: UsbVolumeCapabilities = UsbVolumeCapabilities.NONE
) {
    companion object {
        val DEFAULT = UsbVolumeState()
    }

    /**
     * Get output volume as percentage (0-100).
     */
    val outputVolumePercent: Int
        get() = (outputVolume * 100).toInt()

    /**
     * Get input volume as percentage (0-100).
     */
    val inputVolumePercent: Int
        get() = (inputVolume * 100).toInt()
}
