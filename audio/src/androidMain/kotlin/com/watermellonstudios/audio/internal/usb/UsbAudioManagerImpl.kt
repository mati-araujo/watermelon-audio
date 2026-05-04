package com.watermellonstudios.audio.internal.usb

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.PowerManager
import android.util.Log
import com.watermellonstudios.audio.BuildConfig
import com.watermellonstudios.audio.api.IUsbAudioManager
import com.watermellonstudios.audio.domain.usb.*
import com.watermellonstudios.audio.domain.usb.UsbDeviceCompatibility
import com.watermellonstudios.audio.internal.bridge.AudioNativeBridge
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.*
import kotlin.coroutines.resume

/**
 * Implementation of [IUsbAudioManager] using Android's USB Host API.
 *
 * Handles device discovery, permission requests, and connection management.
 */
internal class UsbAudioManagerImpl(
    private val context: Context,
    private val scope: CoroutineScope = CoroutineScope(Dispatchers.Main + SupervisorJob())
) : IUsbAudioManager {

    // Native bridge instance for USB audio operations
    private val nativeBridge = AudioNativeBridge.getInstance()

    companion object {
        private const val TAG = "UsbAudioManager"
        private const val ACTION_USB_PERMISSION = "com.watermellonstudios.audio.USB_PERMISSION"

        // USB Audio Class codes
        private const val USB_CLASS_AUDIO = 1
        private const val USB_SUBCLASS_AUDIOCONTROL = 1
        private const val USB_SUBCLASS_AUDIOSTREAMING = 2
    }

    // Android USB Manager
    private val usbManager: UsbManager by lazy {
        context.getSystemService(Context.USB_SERVICE) as UsbManager
    }

    // State flows
    private val _connectedDevices = MutableStateFlow<List<UsbAudioDevice>>(emptyList())
    override val connectedDevices: StateFlow<List<UsbAudioDevice>> = _connectedDevices.asStateFlow()

    private val _selectedDevice = MutableStateFlow<UsbAudioDevice?>(null)
    override val selectedDevice: StateFlow<UsbAudioDevice?> = _selectedDevice.asStateFlow()

    private val _connectionState = MutableStateFlow(UsbConnectionState.DISCONNECTED)
    override val connectionState: StateFlow<UsbConnectionState> = _connectionState.asStateFlow()

    private val _currentCapabilitySnapshot = MutableStateFlow<UsbCapabilitySnapshot?>(null)
    override val currentCapabilitySnapshot: StateFlow<UsbCapabilitySnapshot?> =
        _currentCapabilitySnapshot.asStateFlow()

    private val _deviceEvents = MutableSharedFlow<UsbDeviceEvent>(
        replay = 0,
        extraBufferCapacity = 16
    )
    override val deviceEvents: SharedFlow<UsbDeviceEvent> = _deviceEvents.asSharedFlow()

    // Current connection
    private var currentConnection: android.hardware.usb.UsbDeviceConnection? = null
    private var currentFileDescriptor: Int = -1
    private var currentUsbfsPath: String? = null
    private var isMonitoring = false

    // Health check configuration
    private var healthCheckJob: Job? = null
    private var healthCheckEnabled = true
    private val healthCheckIntervalMs = 1000L  // Check every second
    private var lastHealthyTime = 0L
    private var consecutiveUnhealthyChecks = 0
    private val maxUnhealthyChecksBeforeFallback = 3  // 3 seconds of unhealthy = fallback

    // Permission request continuation
    private var permissionContinuation: CancellableContinuation<Boolean>? = null
    private var pendingPermissionDevice: UsbDevice? = null

    // Trusted devices repository for remembering user-approved devices
    private val trustedDevicesRepository by lazy {
        TrustedUsbDevicesRepository(context)
    }

    // Volume repository for persisting volume settings per device
    private val volumeRepository by lazy {
        UsbVolumeRepository(context)
    }

    // Volume state flow
    private val _volumeState = MutableStateFlow(UsbVolumeState.DEFAULT)
    override val volumeState: StateFlow<UsbVolumeState> = _volumeState.asStateFlow()

    // Wake lock to prevent CPU from sleeping during USB audio streaming
    private var wakeLock: PowerManager.WakeLock? = null
    private val powerManager: PowerManager by lazy {
        context.getSystemService(Context.POWER_SERVICE) as PowerManager
    }

    // Broadcast receiver for USB events
    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            Log.d(TAG, "BroadcastReceiver.onReceive: action=${intent.action}")

            when (intent.action) {
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    val device = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
                    } else {
                        @Suppress("DEPRECATION")
                        intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                    }
                    device?.let { handleDeviceAttached(it) }
                }

                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    val device = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
                    } else {
                        @Suppress("DEPRECATION")
                        intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                    }
                    device?.let { handleDeviceDetached(it) }
                }

                ACTION_USB_PERMISSION -> {
                    Log.i(TAG, "ACTION_USB_PERMISSION received!")
                    Log.i(TAG, "  Intent extras: ${intent.extras?.keySet()?.joinToString()}")

                    val device = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
                    } else {
                        @Suppress("DEPRECATION")
                        intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                    }
                    val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)

                    Log.i(TAG, "  device: ${device?.productName} (id=${device?.deviceId})")
                    Log.i(TAG, "  granted: $granted")
                    Log.i(TAG, "  pendingPermissionDevice: ${pendingPermissionDevice?.productName} (id=${pendingPermissionDevice?.deviceId})")
                    Log.i(TAG, "  permissionContinuation is null: ${permissionContinuation == null}")

                    handlePermissionResult(device, granted)
                }

                else -> {
                    Log.d(TAG, "Unknown action received: ${intent.action}")
                }
            }
        }
    }

    // ==================== Device Discovery ====================

    override fun getConnectedDevices(): List<UsbAudioDevice> = _connectedDevices.value

    override suspend fun refreshDevices() {
        withContext(Dispatchers.IO) {
            val audioDevices = scanForAudioDevices()
            _connectedDevices.value = audioDevices
            Log.d(TAG, "Found ${audioDevices.size} USB Audio devices")

            // Cold-start auto-connect: when the app opens with a DAC already
            // plugged in, the runtime BroadcastReceiver never fires (it only
            // catches new attachments). Without this loop, the user sees the
            // app fall back to Oboe even though the DAC is right there with
            // permission persisted from a previous session. Re-enter the
            // standard handleDeviceAttached() flow for each pre-existing
            // device that we already have permission for.
            if (autoConnectEnabled && _selectedDevice.value == null) {
                usbManager.deviceList.values.forEach { usbDevice ->
                    if (isAudioDevice(usbDevice) && usbManager.hasPermission(usbDevice)) {
                        Log.i(TAG, "Cold-start: device ${usbDevice.productName} already " +
                                  "attached with persisted permission, triggering auto-connect")
                        handleDeviceAttached(usbDevice)
                    }
                }
            }
        }
    }

    override fun isUsbAudioSupported(): Boolean {
        return context.packageManager.hasSystemFeature("android.hardware.usb.host")
    }

    private fun scanForAudioDevices(): List<UsbAudioDevice> {
        val devices = mutableListOf<UsbAudioDevice>()

        usbManager.deviceList.values.forEach { usbDevice ->
            if (isAudioDevice(usbDevice)) {
                devices.add(usbDevice.toUsbAudioDevice())
            }
        }

        return devices
    }

    private fun isAudioDevice(device: UsbDevice): Boolean {
        // Check device class
        if (device.deviceClass == USB_CLASS_AUDIO) {
            return true
        }

        // Check interface classes
        for (i in 0 until device.interfaceCount) {
            val iface = device.getInterface(i)
            if (iface.interfaceClass == USB_CLASS_AUDIO &&
                (iface.interfaceSubclass == USB_SUBCLASS_AUDIOCONTROL ||
                        iface.interfaceSubclass == USB_SUBCLASS_AUDIOSTREAMING)
            ) {
                return true
            }
        }

        return false
    }

    // ==================== Device Selection ====================

    override suspend fun connectDevice(device: UsbAudioDevice): UsbResult<Unit> {
        Log.d(TAG, "Connecting to device: ${device.displayName}")

        // Find the actual USB device
        val usbDevice = findUsbDevice(device.deviceId)
            ?: return UsbResult.Failure(UsbAudioError.DEVICE_NOT_FOUND)

        // Check if device is in our trusted list
        val isTrusted = trustedDevicesRepository.isDeviceTrusted(device.vendorId, device.productId)
        if (isTrusted) {
            Log.i(TAG, "Device ${device.displayName} is in trusted list")
        }

        // Check/request permission
        if (!usbManager.hasPermission(usbDevice)) {
            _connectionState.value = UsbConnectionState.PERMISSION_REQUESTED

            if (isTrusted) {
                // Device was previously approved by user - notify UI to show guidance
                Log.i(TAG, "Trusted device requires permission again. User should check 'Always use NoisyPad for this device'")
                _deviceEvents.emit(UsbDeviceEvent.TrustedDeviceNeedsReauthorization(device))
            }

            val granted = requestPermissionSuspend(usbDevice)
            if (!granted) {
                _connectionState.value = UsbConnectionState.PERMISSION_DENIED
                _deviceEvents.emit(UsbDeviceEvent.PermissionDenied(device.deviceId))
                return UsbResult.Failure(UsbAudioError.PERMISSION_DENIED)
            }

            // Permission granted - add to trusted devices list
            trustedDevicesRepository.addTrustedDevice(
                vendorId = device.vendorId,
                productId = device.productId,
                deviceName = device.displayName
            )
            Log.i(TAG, "Device ${device.displayName} added to trusted devices")
        }

        _connectionState.value = UsbConnectionState.PERMISSION_GRANTED
        _deviceEvents.emit(UsbDeviceEvent.PermissionGranted(device))

        // Open connection
        return try {
            _connectionState.value = UsbConnectionState.CONNECTING

            val connection = usbManager.openDevice(usbDevice)
                ?: return UsbResult.Failure(
                    UsbAudioError.INTERNAL_ERROR,
                    "Failed to open USB device"
                )

            currentConnection = connection
            currentFileDescriptor = connection.fileDescriptor
            currentUsbfsPath = usbDevice.deviceName

            _selectedDevice.value = device
            _connectionState.value = UsbConnectionState.CONNECTED

            Log.i(TAG, "Connected to ${device.displayName}, FD=$currentFileDescriptor")

            // Initialize native side with file descriptor
            val nativeInitSuccess = initializeNativeUsb(currentFileDescriptor, currentUsbfsPath ?: "")
            if (!nativeInitSuccess) {
                // Close connection and report error
                connection.close()
                currentConnection = null
                currentFileDescriptor = -1
                currentUsbfsPath = null
                _selectedDevice.value = null
                _connectionState.value = UsbConnectionState.ERROR
                _deviceEvents.emit(
                    UsbDeviceEvent.Error(device.deviceId, UsbAudioError.INITIALIZATION_FAILED)
                )
                return UsbResult.Failure(
                    UsbAudioError.INITIALIZATION_FAILED,
                    "Failed to initialize native USB device"
                )
            }

            _deviceEvents.emit(UsbDeviceEvent.DeviceConnected(device))
            UsbResult.Success(Unit)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to connect: ${e.message}", e)
            _connectionState.value = UsbConnectionState.ERROR
            _deviceEvents.emit(
                UsbDeviceEvent.Error(
                    device.deviceId,
                    UsbAudioError.INTERNAL_ERROR
                )
            )
            UsbResult.Failure(UsbAudioError.INTERNAL_ERROR, e.message)
        }
    }

    override suspend fun disconnectDevice() {
        val device = _selectedDevice.value
        Log.d(TAG, "Disconnecting from device: ${device?.displayName}")

        try {
            // Release wake lock first
            releaseWakeLock()

            // Close native side
            closeNativeUsb()

            // Close connection
            currentConnection?.close()
            currentConnection = null
            currentFileDescriptor = -1
            currentUsbfsPath = null

            _selectedDevice.value = null
            _currentCapabilitySnapshot.value = null
            _selectedAltsetting = null
            _connectionState.value = UsbConnectionState.DISCONNECTED

            device?.let {
                _deviceEvents.emit(UsbDeviceEvent.StreamingStopped(it.deviceId))
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error during disconnect: ${e.message}", e)
            // Ensure wake lock is released even on error
            releaseWakeLock()
        }
    }

    override fun getPreferredDevice(): UsbAudioDevice? {
        return _connectedDevices.value.firstOrNull()
    }

    private fun findUsbDevice(deviceId: String): UsbDevice? {
        return usbManager.deviceList.values.find { it.deviceId.toString() == deviceId }
    }

    // ==================== Permissions ====================

    override fun hasPermission(device: UsbAudioDevice): Boolean {
        val usbDevice = findUsbDevice(device.deviceId) ?: return false
        return usbManager.hasPermission(usbDevice)
    }

    override suspend fun requestPermission(device: UsbAudioDevice) {
        val usbDevice = findUsbDevice(device.deviceId) ?: return
        requestPermissionSuspend(usbDevice)
    }

    private suspend fun requestPermissionSuspend(usbDevice: UsbDevice): Boolean {
        Log.i(TAG, "requestPermissionSuspend: Requesting permission for ${usbDevice.productName}")

        // BUG FIX: Prevent concurrent permission requests (double dialog issue)
        // If there's already a pending permission request, wait for it or skip
        if (permissionContinuation != null) {
            Log.w(TAG, "Permission request already in progress for ${pendingPermissionDevice?.productName}")
            // If requesting for the same device, wait for the existing request
            if (pendingPermissionDevice?.deviceId == usbDevice.deviceId) {
                Log.i(TAG, "Same device - waiting for existing permission request")
                // Return true to allow caller to proceed (permission will be checked again)
                return usbManager.hasPermission(usbDevice)
            } else {
                Log.w(TAG, "Different device - denying concurrent request")
                return false
            }
        }

        return suspendCancellableCoroutine { cont ->
            permissionContinuation = cont
            pendingPermissionDevice = usbDevice

            // IMPORTANT: Must use FLAG_MUTABLE because the system USB service needs to
            // add EXTRA_PERMISSION_GRANTED to the intent when sending the result.
            // FLAG_IMMUTABLE would prevent this modification and break permission handling.
            val intent = Intent(ACTION_USB_PERMISSION).apply {
                setPackage(context.packageName)
            }

            val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
            } else {
                PendingIntent.FLAG_UPDATE_CURRENT
            }

            val permissionIntent = PendingIntent.getBroadcast(
                context,
                0,
                intent,
                flags
            )

            Log.i(TAG, "  Action: $ACTION_USB_PERMISSION")
            Log.i(TAG, "  Package: ${context.packageName}")
            Log.i(TAG, "  Flags: $flags (FLAG_MUTABLE=${PendingIntent.FLAG_MUTABLE}, FLAG_UPDATE_CURRENT=${PendingIntent.FLAG_UPDATE_CURRENT})")
            Log.i(TAG, "  Device: ${usbDevice.productName} (id=${usbDevice.deviceId})")
            Log.i(TAG, "  isMonitoring: $isMonitoring")

            usbManager.requestPermission(usbDevice, permissionIntent)
            Log.i(TAG, "Permission dialog should now appear - waiting for user response...")

            cont.invokeOnCancellation {
                Log.w(TAG, "Permission request cancelled")
                permissionContinuation = null
                pendingPermissionDevice = null
            }
        }
    }

    private fun handlePermissionResult(device: UsbDevice?, granted: Boolean) {
        Log.d(TAG, "handlePermissionResult called: device=${device?.productName}, granted=$granted")

        // Handle case where device is null but we have a pending request
        if (device == null) {
            Log.w(TAG, "Permission result received with null device")
            // If we have a pending continuation, resume it with the granted value anyway
            // Some devices/Android versions may not include the device in the result
            if (permissionContinuation != null && pendingPermissionDevice != null) {
                Log.i(TAG, "Resuming continuation despite null device (granted=$granted)")
                permissionContinuation?.resume(granted)
                permissionContinuation = null
                pendingPermissionDevice = null
            }
            return
        }

        // Verify this is for our pending device
        if (device.deviceId != pendingPermissionDevice?.deviceId) {
            Log.w(TAG, "Permission result device mismatch: received=${device.deviceId}, pending=${pendingPermissionDevice?.deviceId}")
            return
        }

        Log.i(TAG, "Permission result for ${device.productName}: $granted")

        permissionContinuation?.resume(granted)
        permissionContinuation = null
        pendingPermissionDevice = null
    }

    // ==================== Device Capabilities ====================

    override suspend fun getDeviceCapabilities(device: UsbAudioDevice): UsbResult<UsbAudioCapabilities> {
        val usbDevice = findUsbDevice(device.deviceId)
            ?: return UsbResult.Failure(UsbAudioError.DEVICE_NOT_FOUND)

        if (!usbManager.hasPermission(usbDevice)) {
            return UsbResult.Failure(UsbAudioError.PERMISSION_DENIED)
        }

        return try {
            val capabilities = parseCapabilities(usbDevice)
            UsbResult.Success(capabilities)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get capabilities: ${e.message}", e)
            UsbResult.Failure(UsbAudioError.DESCRIPTOR_PARSE_ERROR, e.message)
        }
    }

    /**
     * Current capability snapshot from the native parser.
     * Populated when parseCapabilities() successfully decodes the snapshot.
     */
    private var _currentSnapshot: UsbCapabilitySnapshot?
        get() = _currentCapabilitySnapshot.value
        set(value) {
            _currentCapabilitySnapshot.value = value
        }

    /**
     * Parse capabilities by trying the native snapshot first, falling back
     * to basic interface enumeration if native is not yet initialized.
     */
    private fun parseCapabilities(device: UsbDevice): UsbAudioCapabilities {
        // Try the native snapshot path (requires native device to be initialized)
        val raw = nativeBridge.getUsbCapabilitySnapshot()
        if (raw != null) {
            try {
                val snapshot = UsbSnapshotCodec.decode(raw)
                _currentSnapshot = snapshot
                Log.i(TAG, "Decoded native snapshot: UAC${snapshot.uacVersion}, " +
                    "${snapshot.playbackAltsettings.size} playback, " +
                    "${snapshot.captureAltsettings.size} capture altsettings")

                val syncMode = snapshot.playbackAltsettings.firstOrNull()?.syncType
                    ?: UsbSyncMode.UNKNOWN

                return UsbAudioCapabilities(
                    supportedSampleRates = snapshot.effectiveOutputSampleRates
                        .ifEmpty { listOf(44100, 48000, 96000) },
                    supportedBitDepths = snapshot.effectiveOutputBitDepths
                        .ifEmpty { listOf(16, 24) },
                    maxChannelsOutput = snapshot.playbackAltsettings.maxOfOrNull { alt ->
                        alt.formats.maxOfOrNull { it.channels } ?: 0
                    } ?: 0,
                    maxChannelsInput = snapshot.captureAltsettings.maxOfOrNull { alt ->
                        alt.formats.maxOfOrNull { it.channels } ?: 0
                    } ?: 0,
                    syncMode = syncMode,
                    supportsFullDuplex = snapshot.isFullDuplex,
                    uacVersion = snapshot.uacVersion,
                )
            } catch (e: Exception) {
                Log.e(TAG, "Snapshot decode failed, falling back to basic parse", e)
            }
        }

        // Fallback: enumerate Android USB interfaces (same as old parseBasicCapabilities)
        return parseBasicCapabilitiesFallback(device)
    }

    private fun parseBasicCapabilitiesFallback(device: UsbDevice): UsbAudioCapabilities {
        var hasOutput = false
        var hasInput = false

        for (i in 0 until device.interfaceCount) {
            val iface = device.getInterface(i)
            if (iface.interfaceClass == USB_CLASS_AUDIO &&
                iface.interfaceSubclass == USB_SUBCLASS_AUDIOSTREAMING
            ) {
                for (j in 0 until iface.endpointCount) {
                    val endpoint = iface.getEndpoint(j)
                    if (endpoint.type == android.hardware.usb.UsbConstants.USB_ENDPOINT_XFER_ISOC) {
                        if (endpoint.direction == android.hardware.usb.UsbConstants.USB_DIR_OUT) {
                            hasOutput = true
                        } else {
                            hasInput = true
                        }
                    }
                }
            }
        }

        return UsbAudioCapabilities(
            supportedSampleRates = listOf(44100, 48000, 96000),
            supportedBitDepths = listOf(16, 24),
            maxChannelsOutput = if (hasOutput) 2 else 0,
            maxChannelsInput = if (hasInput) 2 else 0,
            syncMode = UsbSyncMode.ADAPTIVE,
            supportsFullDuplex = hasOutput && hasInput
        )
    }

    override fun getFileDescriptor(): Int = currentFileDescriptor

    override fun getUsbfsPath(): String? = currentUsbfsPath

    // ==================== Discovery (Stage 2) ====================

    /**
     * Returns the current capability snapshot. Always queries the native
     * side (rather than relying on the cached `_currentSnapshot` populated
     * by getDeviceCapabilities()), because callers like MainViewModel may
     * invoke this from the DeviceConnected event handler before any explicit
     * getDeviceCapabilities() call has happened. Updates the cache as a
     * side effect so subsequent reads have it.
     */
    override fun getCurrentCapabilitySnapshot(): UsbCapabilitySnapshot? {
        val raw = nativeBridge.getUsbCapabilitySnapshot() ?: return _currentSnapshot
        return try {
            val snapshot = UsbSnapshotCodec.decode(raw)
            _currentSnapshot = snapshot
            snapshot
        } catch (e: Exception) {
            Log.e(TAG, "getCurrentCapabilitySnapshot: decode failed", e)
            _currentSnapshot
        }
    }

    override fun rankPlaybackAltsettings(preference: StreamPreference): List<ScoredAltsetting> {
        val snapshot = getCurrentCapabilitySnapshot() ?: return emptyList()
        return snapshot.playbackAltsettings.flatMap { alt ->
            alt.formats.mapIndexedNotNull { index, format ->
                if (!passesHardConstraints(snapshot, alt, format, preference)) {
                    null
                } else {
                    val score = scoreAltsetting(alt, format, preference)
                    ScoredAltsetting(
                        altsetting = alt,
                        format = format,
                        formatIndex = index,
                        score = score,
                        recommendation = recommendationFor(alt, format, score)
                    )
                }
            }
        }.sortedWith(
            compareByDescending<ScoredAltsetting> { it.score }
                .thenBy { it.altsetting.alternateSetting }
                .thenBy { it.formatIndex }
        )
    }

    override suspend fun selectAltsetting(
        interfaceNumber: Int,
        alternateSetting: Int,
        formatIndex: Int,
    ): UsbResult<Unit> {
        if (formatIndex < 0) {
            return UsbResult.Failure(UsbAudioError.UNSUPPORTED_FORMAT, "formatIndex must be >= 0")
        }
        val snapshot = getCurrentCapabilitySnapshot()
            ?: return UsbResult.Failure(UsbAudioError.DESCRIPTOR_PARSE_ERROR, "No capability snapshot available")
        val alt = snapshot.playbackAltsettings.firstOrNull {
            it.interfaceNumber == interfaceNumber && it.alternateSetting == alternateSetting
        } ?: return UsbResult.Failure(
            UsbAudioError.UNSUPPORTED_FORMAT,
            "Playback IF$interfaceNumber Alt$alternateSetting not found"
        )
        if (formatIndex !in alt.formats.indices) {
            return UsbResult.Failure(
                UsbAudioError.UNSUPPORTED_FORMAT,
                "Format index $formatIndex not found for IF$interfaceNumber Alt$alternateSetting"
            )
        }

        return withContext(Dispatchers.IO) {
            val applied = nativeBridge.selectUsbAltsetting(interfaceNumber, alternateSetting, formatIndex)
            if (applied) {
                _selectedAltsetting = SelectedAltsetting(interfaceNumber, alternateSetting, formatIndex)
                UsbResult.Success(Unit)
            } else {
                UsbResult.Failure(
                    UsbAudioError.UNSUPPORTED_FORMAT,
                    "Native backend rejected IF$interfaceNumber Alt$alternateSetting format $formatIndex"
                )
            }
        }
    }

    override fun setStreamPreference(preference: StreamPreference) {
        Log.i(TAG, "Stream preference set: profile=${preference.profile}, " +
            "rate=${preference.preferredSampleRate}, minCh=${preference.minChannels}")
        _currentStreamPreference = preference
    }

    private var _currentStreamPreference: StreamPreference? = null
    private var _selectedAltsetting: SelectedAltsetting? = null

    private data class SelectedAltsetting(
        val interfaceNumber: Int,
        val alternateSetting: Int,
        val formatIndex: Int,
    )

    private fun passesHardConstraints(
        snapshot: UsbCapabilitySnapshot,
        alt: AltsettingInfo,
        format: AudioFormatInfo,
        preference: StreamPreference,
    ): Boolean {
        if (format.channels <= 0 || format.bitResolution <= 0) return false
        if (format.channels < preference.minChannels) return false
        if (preference.requireFeedback && !alt.hasFeedbackEndpoint) return false
        if (snapshot.uacVersion != 2 &&
            preference.preferredSampleRate > 0 &&
            format.sampleRates.isNotEmpty() &&
            preference.preferredSampleRate !in format.sampleRates
        ) {
            return false
        }
        return true
    }

    private fun scoreAltsetting(
        alt: AltsettingInfo,
        format: AudioFormatInfo,
        preference: StreamPreference,
    ): Double {
        val weights = when (preference.profile) {
            StreamPreference.Profile.LOWEST_LATENCY -> Weights(bitDepth = 0.3, channels = 0.5, sync = 0.5, feedback = 0.0)
            StreamPreference.Profile.HIGHEST_FIDELITY -> Weights(bitDepth = 1.5, channels = 0.8, sync = 0.8, feedback = 0.3)
            else -> Weights(bitDepth = 1.0, channels = 0.5, sync = 1.0, feedback = 0.3)
        }
        return weights.bitDepth * normalize(format.bitResolution.toDouble(), 16.0, 32.0) +
            weights.channels * normalize(format.channels.toDouble(), 1.0, 8.0) +
            weights.sync * syncScore(alt.syncType) +
            if (alt.hasFeedbackEndpoint) weights.feedback else 0.0
    }

    private data class Weights(
        val bitDepth: Double,
        val channels: Double,
        val sync: Double,
        val feedback: Double,
    )

    private fun normalize(value: Double, low: Double, high: Double): Double {
        if (high <= low) return 0.0
        return ((value - low) / (high - low)).coerceIn(0.0, 1.0)
    }

    private fun syncScore(syncType: UsbSyncMode): Double = when (syncType) {
        UsbSyncMode.ASYNCHRONOUS -> 1.0
        UsbSyncMode.ADAPTIVE -> 0.5
        UsbSyncMode.SYNCHRONOUS -> 0.25
        UsbSyncMode.UNKNOWN -> 0.0
    }

    private fun recommendationFor(
        alt: AltsettingInfo,
        format: AudioFormatInfo,
        score: Double,
    ): String = buildString {
        append("${format.channels}ch/${format.bitResolution}bit")
        append(" ")
        append(alt.syncType.displayName)
        if (alt.hasFeedbackEndpoint) append(" feedback")
        append(" score=")
        append((score * 100.0).toInt() / 100.0)
    }

    // ==================== Lifecycle ====================

    override fun startMonitoring() {
        if (isMonitoring) return

        Log.i(TAG, "Starting USB device monitoring")

        val filter = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
            addAction(ACTION_USB_PERMISSION)
        }

        // IMPORTANT: Must use RECEIVER_EXPORTED to receive USB permission results
        // from the system UsbService. The permission broadcast comes from the system,
        // not from our app, so RECEIVER_NOT_EXPORTED would block it.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(usbReceiver, filter, Context.RECEIVER_EXPORTED)
            Log.i(TAG, "BroadcastReceiver registered with RECEIVER_EXPORTED flag (Android 13+)")
        } else {
            context.registerReceiver(usbReceiver, filter)
            Log.i(TAG, "BroadcastReceiver registered (pre-Android 13)")
        }

        Log.i(TAG, "Listening for actions: ATTACHED, DETACHED, $ACTION_USB_PERMISSION")

        isMonitoring = true

        // Initial scan
        scope.launch {
            refreshDevices()
        }
    }

    override fun stopMonitoring() {
        if (!isMonitoring) return

        Log.d(TAG, "Stopping USB device monitoring")

        try {
            context.unregisterReceiver(usbReceiver)
        } catch (e: IllegalArgumentException) {
            // Receiver not registered
        }

        isMonitoring = false
    }

    override fun release() {
        Log.d(TAG, "Releasing UsbAudioManager")

        stopMonitoring()

        scope.launch {
            disconnectDevice()
        }

        scope.cancel()
    }

    // ==================== Event Handlers ====================

    /**
     * Auto-connect configuration.
     * When enabled, compatible devices will be automatically connected.
     */
    private var autoConnectEnabled = true

    /**
     * Enable or disable automatic connection to compatible devices.
     */
    override fun setAutoConnectEnabled(enabled: Boolean) {
        autoConnectEnabled = enabled
        Log.d(TAG, "Auto-connect ${if (enabled) "enabled" else "disabled"}")
    }

    /**
     * Check if automatic connection is enabled.
     */
    override fun isAutoConnectEnabled(): Boolean = autoConnectEnabled

    private fun handleDeviceAttached(device: UsbDevice) {
        if (!isAudioDevice(device)) return

        Log.i(TAG, "USB Audio device attached: ${device.productName}")
        Log.i(TAG, "  VID=0x${String.format("%04X", device.vendorId)}, PID=0x${String.format("%04X", device.productId)}")
        Log.i(TAG, "  Manufacturer: ${device.manufacturerName}")
        Log.i(TAG, "  Product: ${device.productName}")
        Log.i(TAG, "  Device ID: ${device.deviceId}")

        scope.launch {
            val audioDevice = device.toUsbAudioDevice()

            // Check device compatibility
            val compatibility = UsbDeviceCompatibility.checkCompatibility(audioDevice)
            Log.i(TAG, "Compatibility check: ${compatibility.status} - ${compatibility.reason}")

            // Debug: Check current state
            if (BuildConfig.DEBUG) {
                Log.i("USB_DEBUG", "RECONNECT: autoConnect=$autoConnectEnabled, selected=${_selectedDevice.value?.displayName}, state=${_connectionState.value}, hasPermission=${usbManager.hasPermission(device)}")
            }

            // Update connected devices list
            _connectedDevices.update { current ->
                if (current.none { it.deviceId == audioDevice.deviceId }) {
                    current + audioDevice
                } else {
                    current
                }
            }

            // Emit appropriate event based on compatibility
            when {
                compatibility.isAllowed -> {
                    Log.i(TAG, "Compatible device detected, auto-connect: $autoConnectEnabled")
                    Log.i(TAG, "  selectedDevice before auto-connect: ${_selectedDevice.value?.displayName}")
                    Log.i(TAG, "  isDeviceReady before auto-connect: ${nativeBridge.isUsbDeviceInitialized()}")

                    // Auto-connect if enabled and not already connected.
                    //
                    // IMPORTANT: only auto-connect from the broadcast path if
                    // permission is ALREADY granted. Otherwise we race the
                    // activity intent path: the runtime BroadcastReceiver fires
                    // first, calls connectDevice() → requestPermission(), and
                    // the user sees a redundant permission dialog. The activity
                    // intent (USB_DEVICE_ATTACHED) is delivered shortly after
                    // with implicit permission from Android, and its handler
                    // (MainActivity.handleUsbIntent → handleUsbDeviceFromIntent)
                    // calls connectDevice() where hasPermission is already true.
                    if (autoConnectEnabled && _selectedDevice.value == null && usbManager.hasPermission(device)) {
                        Log.i(TAG, "Auto-connecting to ${audioDevice.displayName} (permission already granted)")
                        val result = connectDevice(audioDevice)
                        Log.i(TAG, "Auto-connect result: $result")
                        Log.i(TAG, "  isDeviceReady after auto-connect: ${nativeBridge.isUsbDeviceInitialized()}")

                        result.onSuccess {
                            Log.i(TAG, "Auto-connect successful, emitting CompatibleDeviceDetected")
                            // Emit AFTER auto-connect succeeds so device is ready
                            _deviceEvents.emit(UsbDeviceEvent.CompatibleDeviceDetected(audioDevice, compatibility))
                        }.onFailure { error, msg ->
                            Log.e(TAG, "Auto-connect failed: $error - $msg")
                            // Still emit event so UI knows device was detected
                            _deviceEvents.emit(UsbDeviceEvent.CompatibleDeviceDetected(audioDevice, compatibility))
                        }
                    } else if (autoConnectEnabled && _selectedDevice.value == null) {
                        // Permission not yet granted — defer to the activity
                        // intent path which will get implicit permission from
                        // Android when the app is launched via the USB intent.
                        Log.i(TAG, "Skipping auto-connect from broadcast: no permission yet for " +
                                  "${audioDevice.displayName}. Activity intent path will handle.")
                        _deviceEvents.emit(UsbDeviceEvent.CompatibleDeviceDetected(audioDevice, compatibility))
                    } else {
                        // Not auto-connecting, emit event immediately
                        Log.w(TAG, "Skipping auto-connect: autoConnectEnabled=$autoConnectEnabled, selectedDevice=${_selectedDevice.value?.displayName}")
                        Log.w(TAG, "  isDeviceReady (skip path): ${nativeBridge.isUsbDeviceInitialized()}")
                        _deviceEvents.emit(UsbDeviceEvent.CompatibleDeviceDetected(audioDevice, compatibility))
                    }
                }
                else -> {
                    Log.w(TAG, "Incompatible device: ${compatibility.reason}")
                    _deviceEvents.emit(UsbDeviceEvent.IncompatibleDeviceDetected(audioDevice, compatibility.reason))
                }
            }

            // Always emit connected event for device list updates
            Log.i(TAG, "Emitting DeviceConnected event")
            _deviceEvents.emit(UsbDeviceEvent.DeviceConnected(audioDevice))
        }
    }

    private fun handleDeviceDetached(device: UsbDevice) {
        val deviceId = device.deviceId.toString()
        Log.i(TAG, "USB Audio device detached: ${device.productName}")
        if (BuildConfig.DEBUG) {
            Log.i("USB_DEBUG", "DETACH: deviceId=$deviceId, selected=${_selectedDevice.value?.displayName} (id: ${_selectedDevice.value?.deviceId})")
        }

        scope.launch {
            // If this was our connected device, disconnect
            if (_selectedDevice.value?.deviceId == deviceId) {
                disconnectDevice()
                if (BuildConfig.DEBUG) {
                    Log.i("USB_DEBUG", "After disconnect: selectedDevice=${_selectedDevice.value}, state=${_connectionState.value}")
                }
            }

            _connectedDevices.update { current ->
                current.filter { it.deviceId != deviceId }
            }
            _deviceEvents.emit(UsbDeviceEvent.DeviceDisconnected(deviceId))
        }
    }

    // ==================== Native Integration ====================

    private fun initializeNativeUsb(fileDescriptor: Int, usbfsPath: String): Boolean {
        Log.d(TAG, "Initialize native USB: fd=$fileDescriptor, path=$usbfsPath")

        return try {
            val success = nativeBridge.initializeUsbDevice(fileDescriptor, usbfsPath)
            if (success) {
                Log.i(TAG, "Native USB device initialized successfully")

                // Parse descriptors to get full capabilities
                val capsArray = nativeBridge.parseUsbDescriptors()
                if (capsArray != null) {
                    Log.d(TAG, "USB Capabilities parsed: ${capsArray.contentToString()}")
                }
            } else {
                Log.e(TAG, "Failed to initialize native USB device")
            }
            success
        } catch (e: Exception) {
            Log.e(TAG, "Exception initializing native USB: ${e.message}", e)
            false
        }
    }

    private fun closeNativeUsb() {
        Log.d(TAG, "Close native USB")
        try {
            // Stop streaming if active
            if (nativeBridge.isUsbDeviceInitialized()) {
                nativeBridge.stopUsbStreaming()
                nativeBridge.closeUsbDevice()
                Log.i(TAG, "Native USB device closed")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Exception closing native USB: ${e.message}", e)
        }
    }

    // ==================== Streaming Control ====================

    /**
     * Start USB audio streaming with the current device.
     * Must be called after connectDevice() succeeds.
     *
     * @param sampleRate Sample rate in Hz (44100, 48000, 96000)
     * @param channels Number of channels (1, 2)
     * @param bitDepth Bit depth (16, 24, 32)
     * @return true if streaming started successfully
     */
    override suspend fun startStreaming(
        sampleRate: Int,
        channels: Int,
        bitDepth: Int
    ): UsbResult<Unit> {
        return startStreaming(sampleRate, channels, bitDepth, UsbStreamingMode.PLAYBACK_ONLY)
    }

    /**
     * Start USB audio streaming with full-duplex support.
     * Must be called after connectDevice() succeeds.
     *
     * @param sampleRate Sample rate in Hz (44100, 48000, 96000)
     * @param channels Number of channels (1, 2)
     * @param bitDepth Bit depth (16, 24, 32)
     * @param streamingMode Streaming mode (playback only, capture only, or full duplex)
     * @return true if streaming started successfully
     */
    override suspend fun startStreaming(
        sampleRate: Int,
        channels: Int,
        bitDepth: Int,
        streamingMode: UsbStreamingMode
    ): UsbResult<Unit> {
        if (_connectionState.value != UsbConnectionState.CONNECTED) {
            return UsbResult.Failure(UsbAudioError.NOT_CONNECTED)
        }

        // Validate streaming mode against device capabilities
        if (streamingMode == UsbStreamingMode.FULL_DUPLEX && !supportsFullDuplex()) {
            return UsbResult.Failure(
                UsbAudioError.UNSUPPORTED_FORMAT,
                "Device does not support full-duplex"
            )
        }

        if (streamingMode == UsbStreamingMode.CAPTURE_ONLY && !hasCapture()) {
            return UsbResult.Failure(
                UsbAudioError.UNSUPPORTED_FORMAT,
                "Device does not support audio capture"
            )
        }

        return withContext(Dispatchers.IO) {
            try {
                val preference = (_currentStreamPreference ?: StreamPreference())
                    .copy(preferredSampleRate = sampleRate)
                nativeBridge.setUsbStreamPreference(preference)
                _selectedAltsetting?.let {
                    nativeBridge.selectUsbAltsetting(
                        it.interfaceNumber,
                        it.alternateSetting,
                        it.formatIndex
                    )
                }

                val success = nativeBridge.startUsbStreamingWithMode(
                    sampleRate,
                    channels,
                    bitDepth,
                    streamingMode.id
                )
                if (success) {
                    // Acquire wake lock to prevent CPU from sleeping during streaming
                    acquireWakeLock()

                    _connectionState.value = UsbConnectionState.STREAMING
                    _selectedDevice.value?.let { device ->
                        _deviceEvents.emit(UsbDeviceEvent.StreamingStarted(device))

                        // Restore persisted volume settings for this device
                        restorePersistedVolume(device)
                    }

                    // Start health checks to monitor for device disconnection
                    startHealthChecks()

                    Log.i(TAG, "USB streaming started: ${sampleRate}Hz, ${channels}ch, ${bitDepth}bit, mode=${streamingMode.displayName}")
                    UsbResult.Success(Unit)
                } else {
                    Log.e(TAG, "Failed to start USB streaming")
                    UsbResult.Failure(UsbAudioError.STREAMING_ERROR, "Failed to start streaming")
                }
            } catch (e: Exception) {
                Log.e(TAG, "Exception starting USB streaming: ${e.message}", e)
                UsbResult.Failure(UsbAudioError.INTERNAL_ERROR, e.message)
            }
        }
    }

    /**
     * Stop USB audio streaming.
     */
    override fun stopStreaming() {
        if (_connectionState.value == UsbConnectionState.STREAMING) {
            try {
                // Stop health checks first
                stopHealthChecks()

                nativeBridge.stopUsbStreaming()

                // Release wake lock
                releaseWakeLock()

                _connectionState.value = UsbConnectionState.CONNECTED
                _selectedDevice.value?.let { device ->
                    scope.launch {
                        _deviceEvents.emit(UsbDeviceEvent.StreamingStopped(device.deviceId))
                    }
                }
                Log.i(TAG, "USB streaming stopped")
            } catch (e: Exception) {
                Log.e(TAG, "Exception stopping USB streaming: ${e.message}", e)
                // Ensure health checks and wake lock are cleaned up even on error
                stopHealthChecks()
                releaseWakeLock()
            }
        }
    }

    /**
     * Get current USB transfer statistics.
     * @return Statistics or null if not streaming
     */
    override fun getTransferStats(): UsbTransferStats? {
        if (_connectionState.value != UsbConnectionState.STREAMING) {
            return null
        }

        return try {
            val statsArray = nativeBridge.getUsbTransferStats()

            // Also get adaptive buffer stats if available
            val adaptiveStats = nativeBridge.getAdaptiveBufferStats()
            val currentBufferMs = adaptiveStats?.getOrNull(0)?.toInt()
                ?: nativeBridge.getCurrentUsbBufferMs()
            val healthScore = adaptiveStats?.getOrNull(6) ?: 100f
            val bufferAdjustments = adaptiveStats?.getOrNull(8)?.toInt() ?: 0

            statsArray?.let { arr ->
                // Extended stats array format (13 elements):
                // [0] packetsSubmitted, [1] packetsCompleted, [2] packetsErrors
                // [3] underruns, [4] overruns
                // [5] currentLatencyMs, [6] avgLatencyMs, [7] minLatencyMs, [8] maxLatencyMs
                // [9] ringBufferLevel, [10] ringBufferFillPct, [11] ringBufferCapacity
                // [12] bytesTransferred
                if (arr.size >= 13) {
                    UsbTransferStats(
                        packetsSubmitted = arr[0].toLong(),
                        packetsCompleted = arr[1].toLong(),
                        packetsErrors = arr[2].toLong(),
                        bytesTransferred = arr[12].toLong(),
                        underruns = arr[3].toLong(),
                        overruns = arr[4].toLong(),
                        currentLatencyMs = arr[5].toDouble(),
                        avgLatencyMs = arr[6].toDouble(),
                        minLatencyMs = arr[7].toDouble(),
                        maxLatencyMs = arr[8].toDouble(),
                        ringBufferLevel = arr[9].toInt(),
                        ringBufferFillPct = arr[10],
                        ringBufferCapacity = arr[11].toInt(),
                        bufferMs = currentBufferMs,
                        healthScore = healthScore,
                        bufferAdjustments = bufferAdjustments
                    )
                } else {
                    // Fallback for old format (5 elements)
                    UsbTransferStats(
                        packetsCompleted = arr[0].toLong(),
                        bytesTransferred = arr[1].toLong(),
                        underruns = arr[2].toLong(),
                        overruns = arr[3].toLong(),
                        avgLatencyMs = arr.getOrElse(4) { 0f }.toDouble(),
                        bufferMs = currentBufferMs,
                        healthScore = healthScore,
                        bufferAdjustments = bufferAdjustments
                    )
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Exception getting transfer stats: ${e.message}", e)
            null
        }
    }

    /**
     * Check if USB device is ready for streaming.
     */
    override fun isDeviceReady(): Boolean = nativeBridge.isUsbDeviceInitialized()

    /**
     * Check if the connected device supports full-duplex.
     */
    override fun supportsFullDuplex(): Boolean {
        return try {
            nativeBridge.usbDeviceSupportsFullDuplex()
        } catch (e: Exception) {
            Log.e(TAG, "Exception checking full-duplex support: ${e.message}", e)
            false
        }
    }

    /**
     * Check if the connected device has capture capability.
     */
    override fun hasCapture(): Boolean {
        return try {
            nativeBridge.usbDeviceHasCapture()
        } catch (e: Exception) {
            Log.e(TAG, "Exception checking capture support: ${e.message}", e)
            false
        }
    }

    /**
     * Get the UAC version of the connected device.
     */
    override fun getUacVersion(): Int {
        return try {
            nativeBridge.getUsbDeviceUacVersion()
        } catch (e: Exception) {
            Log.e(TAG, "Exception getting UAC version: ${e.message}", e)
            0
        }
    }

    // ==================== Health Check System ====================

    /**
     * Start periodic health checks when USB streaming begins.
     * Monitors for device disconnection and triggers fallback to Oboe if needed.
     */
    private fun startHealthChecks() {
        if (!healthCheckEnabled) return

        healthCheckJob?.cancel()
        consecutiveUnhealthyChecks = 0
        lastHealthyTime = System.currentTimeMillis()

        healthCheckJob = scope.launch {
            Log.d(TAG, "Health check started")
            while (isActive && _connectionState.value == UsbConnectionState.STREAMING) {
                delay(healthCheckIntervalMs)

                try {
                    val isDisconnected = nativeBridge.isUsbDeviceDisconnected()
                    val healthStatus = nativeBridge.getUsbHealthStatus()

                    if (isDisconnected) {
                        consecutiveUnhealthyChecks++
                        Log.w(TAG, "Health check: USB appears disconnected ($consecutiveUnhealthyChecks/$maxUnhealthyChecksBeforeFallback)")

                        if (consecutiveUnhealthyChecks >= maxUnhealthyChecksBeforeFallback) {
                            Log.e(TAG, "Health check: USB disconnected, triggering fallback")
                            handleUsbDisconnectedDuringStreaming()
                            break
                        }
                    } else {
                        // Device is healthy
                        if (consecutiveUnhealthyChecks > 0) {
                            Log.i(TAG, "Health check: USB recovered after $consecutiveUnhealthyChecks unhealthy checks")
                        }
                        consecutiveUnhealthyChecks = 0
                        lastHealthyTime = System.currentTimeMillis()
                    }

                    // Log periodic health status
                    healthStatus?.let { status ->
                        if (status[1] > 0) {  // errors
                            Log.d(TAG, "Health check: isDisconnected=${status[0]}, errors=${status[1]}")
                        }
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Health check error: ${e.message}")
                    consecutiveUnhealthyChecks++
                    if (consecutiveUnhealthyChecks >= maxUnhealthyChecksBeforeFallback) {
                        handleUsbDisconnectedDuringStreaming()
                        break
                    }
                }
            }
            Log.d(TAG, "Health check stopped")
        }
    }

    /**
     * Stop health checks when streaming stops.
     */
    private fun stopHealthChecks() {
        healthCheckJob?.cancel()
        healthCheckJob = null
        consecutiveUnhealthyChecks = 0
    }

    /**
     * Handle USB disconnection detected during streaming.
     * Triggers fallback to Oboe and notifies listeners.
     */
    private fun handleUsbDisconnectedDuringStreaming() {
        Log.w(TAG, "Handling USB disconnection during streaming")

        val device = _selectedDevice.value

        scope.launch {
            try {
                // Trigger native fallback to Oboe
                nativeBridge.fallbackToOboeBackend()

                // Update state
                _connectionState.value = UsbConnectionState.ERROR
                releaseWakeLock()

                // Emit event
                device?.let {
                    _deviceEvents.emit(
                        UsbDeviceEvent.Error(
                            it.deviceId,
                            UsbAudioError.DEVICE_DISCONNECTED
                        )
                    )
                    _deviceEvents.emit(UsbDeviceEvent.StreamingStopped(it.deviceId))
                }

                // Also emit a special event for auto-fallback
                _deviceEvents.emit(UsbDeviceEvent.FallbackToBuiltInAudio)

                Log.i(TAG, "Fallback to built-in audio completed")
            } catch (e: Exception) {
                Log.e(TAG, "Error during fallback: ${e.message}", e)
            }
        }
    }

    /**
     * Enable or disable health checks.
     * @param enabled true to enable periodic health monitoring
     */
    fun setHealthCheckEnabled(enabled: Boolean) {
        healthCheckEnabled = enabled
        if (!enabled) {
            stopHealthChecks()
        }
        Log.d(TAG, "Health check ${if (enabled) "enabled" else "disabled"}")
    }

    // ==================== Wake Lock Management ====================

    /**
     * Acquire a partial wake lock to prevent CPU from sleeping during USB audio streaming.
     * This ensures continuous audio playback even when the screen is off.
     */
    private fun acquireWakeLock() {
        try {
            if (wakeLock == null) {
                wakeLock = powerManager.newWakeLock(
                    PowerManager.PARTIAL_WAKE_LOCK,
                    "NoisyPad::UsbAudioStreaming"
                )
            }

            wakeLock?.let { lock ->
                if (!lock.isHeld) {
                    // Acquire with 30-minute timeout for safety
                    // The lock will be released when streaming stops
                    lock.acquire(30 * 60 * 1000L)
                    Log.i(TAG, "Wake lock acquired for USB audio streaming")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to acquire wake lock: ${e.message}", e)
        }
    }

    /**
     * Release the wake lock when USB audio streaming stops.
     */
    private fun releaseWakeLock() {
        try {
            wakeLock?.let { lock ->
                if (lock.isHeld) {
                    lock.release()
                    Log.i(TAG, "Wake lock released")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to release wake lock: ${e.message}", e)
        }
    }

    // ==================== Volume Control ====================

    /**
     * Get volume capabilities from native layer.
     */
    override fun getVolumeCapabilities(): UsbVolumeCapabilities {
        return try {
            val capsArray = nativeBridge.getUsbVolumeCapabilities()
            UsbVolumeCapabilities.fromNativeArray(capsArray)
        } catch (e: Exception) {
            Log.e(TAG, "Exception getting volume capabilities: ${e.message}", e)
            UsbVolumeCapabilities.NONE
        }
    }

    /**
     * Set output volume.
     */
    override suspend fun setOutputVolume(volume: Float) {
        val clampedVolume = volume.coerceIn(0f, 1f)

        // Apply via native
        try {
            nativeBridge.setUsbOutputVolume(clampedVolume)
        } catch (e: Exception) {
            Log.e(TAG, "Exception setting output volume: ${e.message}", e)
        }

        // Persist per device
        val device = _selectedDevice.value
        if (device != null) {
            volumeRepository.saveOutputVolume(device.vendorId, device.productId, clampedVolume)
        }

        // Update state
        updateVolumeState()
    }

    /**
     * Get current output volume.
     */
    override fun getOutputVolume(): Float {
        return try {
            nativeBridge.getUsbOutputVolume()
        } catch (e: Exception) {
            Log.e(TAG, "Exception getting output volume: ${e.message}", e)
            1.0f
        }
    }

    /**
     * Set input volume.
     */
    override suspend fun setInputVolume(volume: Float) {
        val clampedVolume = volume.coerceIn(0f, 1f)

        // Apply via native
        try {
            nativeBridge.setUsbInputVolume(clampedVolume)
        } catch (e: Exception) {
            Log.e(TAG, "Exception setting input volume: ${e.message}", e)
        }

        // Persist per device
        val device = _selectedDevice.value
        if (device != null) {
            volumeRepository.saveInputVolume(device.vendorId, device.productId, clampedVolume)
        }

        // Update state
        updateVolumeState()
    }

    /**
     * Get current input volume.
     */
    override fun getInputVolume(): Float {
        return try {
            nativeBridge.getUsbInputVolume()
        } catch (e: Exception) {
            Log.e(TAG, "Exception getting input volume: ${e.message}", e)
            1.0f
        }
    }

    /**
     * Set output mute state.
     */
    override suspend fun setOutputMuted(muted: Boolean) {
        try {
            nativeBridge.setUsbOutputMute(muted)
        } catch (e: Exception) {
            Log.e(TAG, "Exception setting output mute: ${e.message}", e)
        }

        // Persist per device
        val device = _selectedDevice.value
        if (device != null) {
            volumeRepository.saveOutputMuted(device.vendorId, device.productId, muted)
        }

        // Update state
        updateVolumeState()
    }

    /**
     * Check if output is muted.
     */
    override fun isOutputMuted(): Boolean {
        return try {
            nativeBridge.isUsbOutputMuted()
        } catch (e: Exception) {
            Log.e(TAG, "Exception getting output mute state: ${e.message}", e)
            false
        }
    }

    /**
     * Set input mute state.
     */
    override suspend fun setInputMuted(muted: Boolean) {
        try {
            nativeBridge.setUsbInputMute(muted)
        } catch (e: Exception) {
            Log.e(TAG, "Exception setting input mute: ${e.message}", e)
        }

        // Persist per device
        val device = _selectedDevice.value
        if (device != null) {
            volumeRepository.saveInputMuted(device.vendorId, device.productId, muted)
        }

        // Update state
        updateVolumeState()
    }

    /**
     * Check if input is muted.
     */
    override fun isInputMuted(): Boolean {
        return try {
            nativeBridge.isUsbInputMuted()
        } catch (e: Exception) {
            Log.e(TAG, "Exception getting input mute state: ${e.message}", e)
            false
        }
    }

    /**
     * Adjust output volume by delta.
     */
    override suspend fun adjustOutputVolume(delta: Float): Float {
        val currentVolume = getOutputVolume()
        val newVolume = (currentVolume + delta).coerceIn(0f, 1f)
        setOutputVolume(newVolume)
        return newVolume
    }

    /**
     * Toggle output mute.
     */
    override suspend fun toggleOutputMute(): Boolean {
        val currentMuted = isOutputMuted()
        val newMuted = !currentMuted
        setOutputMuted(newMuted)
        return newMuted
    }

    /**
     * Check if we should intercept hardware volume buttons.
     */
    override fun shouldInterceptVolumeButtons(): Boolean {
        return _connectionState.value == UsbConnectionState.STREAMING
    }

    /**
     * Observe volume changes for current device.
     */
    override fun observeVolumeChanges(): Flow<UsbVolumeState> {
        return _volumeState.asStateFlow()
    }

    /**
     * Update volume state from native layer.
     */
    private fun updateVolumeState() {
        val capabilities = getVolumeCapabilities()
        val outputVolume = getOutputVolume()
        val inputVolume = getInputVolume()
        val outputMuted = isOutputMuted()
        val inputMuted = isInputMuted()

        _volumeState.value = UsbVolumeState(
            outputVolume = outputVolume,
            inputVolume = inputVolume,
            outputMuted = outputMuted,
            inputMuted = inputMuted,
            capabilities = capabilities
        )
    }

    /**
     * Restore persisted volume settings when device connects.
     */
    private suspend fun restorePersistedVolume(device: UsbAudioDevice) {
        try {
            val persisted = volumeRepository.getVolumeForDevice(device.vendorId, device.productId)
            Log.d(TAG, "Restoring persisted volume for ${device.displayName}: output=${persisted.outputVolume}, input=${persisted.inputVolume}")

            // Apply persisted volumes via native
            nativeBridge.setUsbOutputVolume(persisted.outputVolume)
            nativeBridge.setUsbInputVolume(persisted.inputVolume)
            nativeBridge.setUsbOutputMute(persisted.outputMuted)
            nativeBridge.setUsbInputMute(persisted.inputMuted)

            // Update state
            updateVolumeState()
        } catch (e: Exception) {
            Log.e(TAG, "Exception restoring persisted volume: ${e.message}", e)
        }
    }

    // ==================== Extension Functions ====================

    private fun UsbDevice.toUsbAudioDevice(): UsbAudioDevice {
        // serialNumber requires USB permission, so we need to safely access it
        val serial = try {
            this.serialNumber
        } catch (e: SecurityException) {
            // No permission yet - this is expected during initial scan
            null
        }

        if (BuildConfig.DEBUG) {
            Log.i("USB_DEBUG", "USB DEVICE: id=${this.deviceId}, VID=0x${String.format("%04X", this.vendorId)}, PID=0x${String.format("%04X", this.productId)}, product=${this.productName}, manufacturer=${this.manufacturerName}, interfaces=${this.interfaceCount}")
        }

        // Warn if VID/PID are zero - this prevents "Always use" option in permission dialog
        if (this.vendorId == 0 || this.productId == 0) {
            Log.w(TAG, "Device has zero VID/PID - 'Always use' option won't appear in permission dialog")
        }

        return UsbAudioDevice(
            deviceId = this.deviceId.toString(),
            vendorId = this.vendorId,
            productId = this.productId,
            deviceName = this.productName ?: "Unknown USB Audio Device",
            manufacturerName = this.manufacturerName,
            serialNumber = serial,
            capabilities = UsbAudioCapabilities.UNKNOWN
        )
    }
}
