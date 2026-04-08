package com.watermellonstudios.audio.domain.usb

import com.watermellonstudios.audio.BuildConfig

/**
 * USB Audio Device Compatibility Manager.
 *
 * Manages the list of compatible USB Audio devices and provides
 * compatibility checking based on build type.
 *
 * - DEBUG builds: All USB Audio Class devices are allowed
 * - RELEASE builds: Only devices in the allowlist are allowed
 *
 * To add a new device:
 * 1. Connect the device and check logs for VID/PID
 * 2. Add entry to [COMPATIBLE_DEVICES] with appropriate info
 * 3. Test thoroughly before release
 */
object UsbDeviceCompatibility {

    /**
     * Represents a compatible USB Audio device.
     */
    data class CompatibleDevice(
        val vendorId: Int,
        val productId: Int,
        val displayName: String,
        val manufacturer: String,
        val notes: String = "",
        val minUacVersion: Int = 1,
        val testedSampleRates: List<Int> = listOf(48000),
        val testedBitDepths: List<Int> = listOf(16, 24),
        val knownIssues: List<String> = emptyList()
    ) {
        val vidPid: String get() = String.format("%04X:%04X", vendorId, productId)
    }

    // Type aliases for backwards compatibility — actual types in commonMain UsbCompatibilityTypes.kt
    @Suppress("unused")
    @Deprecated("Use UsbCompatibilityStatus directly", ReplaceWith("UsbCompatibilityStatus"))
    typealias CompatibilityStatus = UsbCompatibilityStatus
    @Suppress("unused")
    @Deprecated("Use UsbCompatibilityResult directly", ReplaceWith("UsbCompatibilityResult"))
    typealias CompatibilityResult = UsbCompatibilityResult

    // ==================== COMPATIBLE DEVICES ALLOWLIST ====================
    // Add tested devices here with their VID:PID

    /**
     * List of tested and compatible USB Audio devices.
     *
     * To get VID/PID of a device, connect it and check logcat for:
     * "USB Device: VID=0x..., PID=0x..."
     */
    private val COMPATIBLE_DEVICES = listOf(
        // === DACs/Amplifiers ===

        CompatibleDevice(
            vendorId = 0x31B2,
            productId = 0x0011,
            displayName = "GHW USB AUDIO",
            manufacturer = "GHW Micro",
            notes = "USB Audio DAC - UAC 1.0",
            testedSampleRates = listOf(44100, 48000),
            testedBitDepths = listOf(16)
        ),

        // UC02 DAC (common cheap USB DAC)
        CompatibleDevice(
            vendorId = 0x0D8C,  // C-Media Electronics
            productId = 0x0014,
            displayName = "UC02 USB Audio",
            manufacturer = "C-Media",
            notes = "Common USB Audio DAC - UAC 1.0, Adaptive",
            testedSampleRates = listOf(44100, 48000),
            testedBitDepths = listOf(16)
        ),

        // Generic C-Media USB Audio
        CompatibleDevice(
            vendorId = 0x0D8C,
            productId = 0x0012,
            displayName = "C-Media USB Audio",
            manufacturer = "C-Media",
            notes = "Generic C-Media USB Audio",
            testedSampleRates = listOf(44100, 48000),
            testedBitDepths = listOf(16, 24)
        ),

        // Focusrite Scarlett Solo (popular audio interface)
        CompatibleDevice(
            vendorId = 0x1235,
            productId = 0x8211,
            displayName = "Scarlett Solo 3rd Gen",
            manufacturer = "Focusrite",
            notes = "Professional audio interface - UAC 2.0",
            minUacVersion = 2,
            testedSampleRates = listOf(44100, 48000, 96000),
            testedBitDepths = listOf(24)
        ),

        // Behringer UMC series
        CompatibleDevice(
            vendorId = 0x1397,
            productId = 0x0507,
            displayName = "UMC204HD",
            manufacturer = "Behringer",
            notes = "Audio interface - UAC 2.0",
            minUacVersion = 2,
            testedSampleRates = listOf(44100, 48000, 96000),
            testedBitDepths = listOf(24)
        ),

        // Creative Sound Blaster Play!
        CompatibleDevice(
            vendorId = 0x041E,
            productId = 0x30D3,
            displayName = "Sound Blaster Play! 3",
            manufacturer = "Creative",
            notes = "USB DAC",
            testedSampleRates = listOf(44100, 48000),
            testedBitDepths = listOf(16, 24)
        ),

        // FiiO USB DACs
        CompatibleDevice(
            vendorId = 0x2972,
            productId = 0x0047,
            displayName = "FiiO BTR5",
            manufacturer = "FiiO",
            notes = "Portable DAC/Amp",
            testedSampleRates = listOf(44100, 48000, 96000),
            testedBitDepths = listOf(16, 24, 32)
        ),

        // Apple USB-C to 3.5mm adapter
        CompatibleDevice(
            vendorId = 0x05AC,
            productId = 0x110A,
            displayName = "Apple USB-C Audio Adapter",
            manufacturer = "Apple",
            notes = "USB-C to 3.5mm adapter",
            testedSampleRates = listOf(44100, 48000),
            testedBitDepths = listOf(24)
        ),

        // Google USB-C to 3.5mm adapter
        CompatibleDevice(
            vendorId = 0x18D1,
            productId = 0x5034,
            displayName = "Google USB-C Audio Adapter",
            manufacturer = "Google",
            notes = "USB-C to 3.5mm adapter",
            testedSampleRates = listOf(48000),
            testedBitDepths = listOf(24)
        )
    )

    // Indexed by VID:PID for fast lookup
    private val deviceMap: Map<String, CompatibleDevice> by lazy {
        COMPATIBLE_DEVICES.associateBy { it.vidPid }
    }

    // ==================== PUBLIC API ====================

    /**
     * Check if a device is compatible based on VID/PID.
     *
     * @param vendorId USB Vendor ID
     * @param productId USB Product ID
     * @param isAudioDevice Whether the device is a USB Audio Class device
     * @return Compatibility result with status and details
     */
    fun checkCompatibility(
        vendorId: Int,
        productId: Int,
        isAudioDevice: Boolean = true
    ): UsbCompatibilityResult {
        if (!isAudioDevice) {
            return UsbCompatibilityResult(
                status = UsbCompatibilityStatus.NOT_AUDIO_DEVICE,
                reason = "Device is not a USB Audio Class device"
            )
        }

        val vidPid = String.format("%04X:%04X", vendorId, productId)
        val knownDevice = deviceMap[vidPid]

        return when {
            knownDevice != null -> {
                UsbCompatibilityResult(
                    status = UsbCompatibilityStatus.COMPATIBLE,
                    deviceName = knownDevice.displayName,
                    reason = "Device is in compatibility list"
                )
            }
            BuildConfig.DEBUG -> {
                UsbCompatibilityResult(
                    status = UsbCompatibilityStatus.DEBUG_ONLY,
                    reason = "Device allowed in DEBUG build (VID:PID = $vidPid)"
                )
            }
            else -> {
                UsbCompatibilityResult(
                    status = UsbCompatibilityStatus.INCOMPATIBLE,
                    reason = "Device not in compatibility list (VID:PID = $vidPid). " +
                            "Contact support to request adding this device."
                )
            }
        }
    }

    /**
     * Check if a UsbAudioDevice is compatible.
     */
    fun checkCompatibility(device: UsbAudioDevice): UsbCompatibilityResult {
        return checkCompatibility(
            vendorId = device.vendorId,
            productId = device.productId,
            isAudioDevice = true
        )
    }

    /**
     * Get the compatible device info if available.
     */
    fun getCompatibleDevice(vendorId: Int, productId: Int): CompatibleDevice? {
        val vidPid = String.format("%04X:%04X", vendorId, productId)
        return deviceMap[vidPid]
    }

    /**
     * Get all compatible devices.
     */
    fun getAllCompatibleDevices(): List<CompatibleDevice> = COMPATIBLE_DEVICES.toList()

    /**
     * Check if we're in debug mode (all devices allowed).
     */
    fun isDebugMode(): Boolean = BuildConfig.DEBUG

    /**
     * Get the recommended sample rate for a device.
     */
    fun getRecommendedSampleRate(vendorId: Int, productId: Int): Int {
        val device = getCompatibleDevice(vendorId, productId)
        return device?.testedSampleRates?.maxOrNull() ?: 48000
    }

    /**
     * Get the recommended bit depth for a device.
     */
    fun getRecommendedBitDepth(vendorId: Int, productId: Int): Int {
        val device = getCompatibleDevice(vendorId, productId)
        return device?.testedBitDepths?.maxOrNull() ?: 16
    }
}
