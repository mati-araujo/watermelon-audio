package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.usb.*
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * Interface for managing USB Audio devices.
 *
 * Provides device discovery, permission handling, and connection management
 * for USB Audio Class devices.
 *
 * Usage:
 * ```kotlin
 * val usbManager = UsbAudioManagerFactory.create(context)
 *
 * // Observe connected devices
 * usbManager.connectedDevices.collect { devices ->
 *     println("Connected devices: $devices")
 * }
 *
 * // Connect to a device
 * usbManager.connectDevice(device).onSuccess {
 *     println("Connected to $device")
 * }
 * ```
 */
interface IUsbAudioManager {

    // ==================== State Flows ====================

    /**
     * Flow of currently connected USB Audio devices.
     * Emits whenever a device is connected or disconnected.
     */
    val connectedDevices: StateFlow<List<UsbAudioDevice>>

    /**
     * Flow of the currently selected/active device.
     * Null if no device is selected.
     */
    val selectedDevice: StateFlow<UsbAudioDevice?>

    /**
     * Flow of the current connection state.
     */
    val connectionState: StateFlow<UsbConnectionState>

    /**
     * Flow of USB device events (connect, disconnect, errors).
     */
    val deviceEvents: SharedFlow<UsbDeviceEvent>

    /**
     * Flow of USB clock/transfer health events emitted while streaming.
     */
    val healthEvents: SharedFlow<UsbHealthEvent>

    // ==================== Device Discovery ====================

    /**
     * Get list of currently connected USB Audio devices.
     * This is a snapshot; use [connectedDevices] flow for real-time updates.
     */
    fun getConnectedDevices(): List<UsbAudioDevice>

    /**
     * Refresh the list of connected devices.
     * Triggers a re-scan of USB devices.
     */
    suspend fun refreshDevices()

    /**
     * Check if USB Audio devices are supported on this device.
     * Returns false if USB Host mode is not available.
     */
    fun isUsbAudioSupported(): Boolean

    // ==================== Device Selection ====================

    /**
     * Request permission and connect to a USB Audio device.
     *
     * @param device The device to connect to
     * @return Result indicating success or failure
     */
    suspend fun connectDevice(device: UsbAudioDevice): UsbResult<Unit>

    /**
     * Disconnect from the currently connected device.
     */
    suspend fun disconnectDevice()

    /**
     * Get the preferred/default USB Audio device.
     * Returns the first available device or the last connected device.
     */
    fun getPreferredDevice(): UsbAudioDevice?

    // ==================== Permissions ====================

    /**
     * Check if we have permission to access a USB device.
     *
     * @param device The device to check
     * @return true if permission is granted
     */
    fun hasPermission(device: UsbAudioDevice): Boolean

    /**
     * Request permission to access a USB device.
     * The result will be emitted via [deviceEvents].
     *
     * @param device The device to request permission for
     */
    suspend fun requestPermission(device: UsbAudioDevice)

    // ==================== Device Capabilities ====================

    /**
     * Parse and get capabilities of a USB device.
     * Requires permission to be granted first.
     *
     * @param device The device to query
     * @return Result with capabilities or error
     */
    suspend fun getDeviceCapabilities(device: UsbAudioDevice): UsbResult<UsbAudioCapabilities>

    /**
     * Get the file descriptor for native access.
     * Only valid after [connectDevice] succeeds.
     *
     * @return File descriptor or -1 if not connected
     */
    fun getFileDescriptor(): Int

    /**
     * Get the USB filesystem path for native access.
     * Only valid after [connectDevice] succeeds.
     *
     * @return USB filesystem path or null if not connected
     */
    fun getUsbfsPath(): String?

    // ==================== Streaming ====================

    /**
     * Start USB audio streaming with the current device.
     * Must be called after [connectDevice] succeeds.
     *
     * @param sampleRate Sample rate in Hz (44100, 48000, 96000)
     * @param channels Number of channels (1, 2)
     * @param bitDepth Bit depth (16, 24, 32)
     * @return Result indicating success or failure
     */
    suspend fun startStreaming(
        sampleRate: Int = 48000,
        channels: Int = 2,
        bitDepth: Int = 24
    ): UsbResult<Unit>

    /**
     * Start USB audio streaming with full-duplex support.
     * Must be called after [connectDevice] succeeds.
     *
     * @param sampleRate Sample rate in Hz (44100, 48000, 96000)
     * @param channels Number of channels (1, 2)
     * @param bitDepth Bit depth (16, 24, 32)
     * @param streamingMode Streaming mode (playback only, capture only, or full duplex)
     * @return Result indicating success or failure
     */
    suspend fun startStreaming(
        sampleRate: Int = 48000,
        channels: Int = 2,
        bitDepth: Int = 24,
        streamingMode: UsbStreamingMode
    ): UsbResult<Unit>

    /**
     * Stop USB audio streaming.
     */
    fun stopStreaming()

    /**
     * Get current USB transfer statistics.
     * Only valid while streaming.
     *
     * @return Statistics or null if not streaming
     */
    fun getTransferStats(): UsbTransferStats?

    /**
     * Check if USB device is ready for streaming.
     * Returns true after [connectDevice] succeeds and native device is initialized.
     */
    fun isDeviceReady(): Boolean

    /**
     * Check if the connected device supports full-duplex (simultaneous playback and capture).
     *
     * @return true if full-duplex is supported
     */
    fun supportsFullDuplex(): Boolean

    /**
     * Check if the connected device has audio capture capability.
     *
     * @return true if capture is available
     */
    fun hasCapture(): Boolean

    /**
     * Get the UAC version of the connected device.
     *
     * @return UAC version (1 or 2), or 0 if not connected
     */
    fun getUacVersion(): Int

    // ==================== Discovery (Stage 2) ====================

    /**
     * Get the latest capability snapshot from the native USB descriptor parser.
     * Returns null if no device is connected or native parsing hasn't completed yet.
     */
    val currentCapabilitySnapshot: StateFlow<UsbCapabilitySnapshot?>

    /**
     * Backward-compatible getter for callers that are not Flow-based yet.
     * Prefer [currentCapabilitySnapshot] for reactive UI.
     */
    fun getCurrentCapabilitySnapshot(): UsbCapabilitySnapshot?

    /**
     * Rank playback altsettings against the provided preference.
     */
    fun rankPlaybackAltsettings(preference: StreamPreference): List<ScoredAltsetting>

    /**
     * Select a specific playback altsetting+format for the next startStreaming() call.
     */
    suspend fun selectAltsetting(
        interfaceNumber: Int,
        alternateSetting: Int,
        formatIndex: Int,
    ): UsbResult<Unit>

    /**
     * Select a UAC2 clock source for the next startStreaming() call.
     */
    suspend fun selectClockSource(clockSourceId: Int): UsbResult<Unit>

    /**
     * Set the stream preference used for altsetting selection.
     * Takes effect on the next startStreaming() call.
     */
    fun setStreamPreference(preference: StreamPreference)

    /**
     * Select the USB latency profile (Fase 1). Re-parametrizes the transfer
     * pipeline to trade buffering headroom for round-trip latency.
     *
     * Only valid while streaming is stopped — call before [startStreaming].
     * Returns failure if a stream is currently running.
     */
    fun setLatencyProfile(profile: UsbLatencyProfile): UsbResult<Unit>

    // ==================== Lifecycle ====================

    /**
     * Start monitoring for USB device connections.
     * Call this when the app starts or becomes active.
     */
    fun startMonitoring()

    /**
     * Stop monitoring for USB device connections.
     * Call this when the app is backgrounded or destroyed.
     */
    fun stopMonitoring()

    /**
     * Release all resources.
     * Call this when the manager is no longer needed.
     */
    fun release()

    // ==================== Auto-Connect ====================

    /**
     * Enable or disable automatic connection to compatible devices.
     * When enabled, compatible USB Audio devices will be connected automatically.
     *
     * @param enabled true to enable auto-connect
     */
    fun setAutoConnectEnabled(enabled: Boolean)

    /**
     * Check if automatic connection is enabled.
     *
     * @return true if auto-connect is enabled
     */
    fun isAutoConnectEnabled(): Boolean

    // ==================== Volume Control ====================

    /**
     * Flow of the current USB volume state.
     * Includes output/input volumes, mute states, and capabilities.
     */
    val volumeState: StateFlow<UsbVolumeState>

    /**
     * Get volume capabilities of the connected device.
     * Returns information about hardware vs digital volume control.
     *
     * @return Volume capabilities or NONE if not connected
     */
    fun getVolumeCapabilities(): UsbVolumeCapabilities

    /**
     * Set output volume for the connected USB device.
     * Volume is persisted per device (VID:PID).
     *
     * @param volume Volume level 0.0 to 1.0
     */
    suspend fun setOutputVolume(volume: Float)

    /**
     * Get current output volume.
     *
     * @return Current output volume 0.0 to 1.0
     */
    fun getOutputVolume(): Float

    /**
     * Set input volume for the connected USB device.
     * Volume is persisted per device (VID:PID).
     *
     * @param volume Volume level 0.0 to 1.0
     */
    suspend fun setInputVolume(volume: Float)

    /**
     * Get current input volume.
     *
     * @return Current input volume 0.0 to 1.0
     */
    fun getInputVolume(): Float

    /**
     * Set output mute state.
     *
     * @param muted true to mute, false to unmute
     */
    suspend fun setOutputMuted(muted: Boolean)

    /**
     * Check if output is muted.
     *
     * @return true if output is muted
     */
    fun isOutputMuted(): Boolean

    /**
     * Set input mute state.
     *
     * @param muted true to mute, false to unmute
     */
    suspend fun setInputMuted(muted: Boolean)

    /**
     * Check if input is muted.
     *
     * @return true if input is muted
     */
    fun isInputMuted(): Boolean

    /**
     * Adjust output volume by a delta.
     * Useful for hardware volume button handling.
     *
     * @param delta Volume delta (-1.0 to 1.0), positive to increase
     * @return New volume level after adjustment
     */
    suspend fun adjustOutputVolume(delta: Float): Float

    /**
     * Toggle output mute state.
     *
     * @return New mute state
     */
    suspend fun toggleOutputMute(): Boolean

    /**
     * Check if USB backend is currently active and should intercept volume buttons.
     *
     * @return true if USB audio is streaming and should handle hardware volume buttons
     */
    fun shouldInterceptVolumeButtons(): Boolean

    /**
     * Observe volume changes for the current device.
     * Emits when volume settings change for the connected device.
     *
     * @return Flow of volume state changes
     */
    fun observeVolumeChanges(): Flow<UsbVolumeState>
}
