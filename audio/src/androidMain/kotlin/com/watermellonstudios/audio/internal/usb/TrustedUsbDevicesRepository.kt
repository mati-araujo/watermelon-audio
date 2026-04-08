package com.watermellonstudios.audio.internal.usb

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.emptyPreferences
import androidx.datastore.preferences.core.stringSetPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import java.io.IOException

/**
 * DataStore instance for trusted USB devices.
 */
private val Context.trustedUsbDevicesDataStore: DataStore<Preferences> by preferencesDataStore(
    name = "trusted_usb_devices"
)

/**
 * Represents a trusted USB device identified by Vendor ID and Product ID.
 */
data class TrustedUsbDevice(
    val vendorId: Int,
    val productId: Int,
    val deviceName: String
) {
    /**
     * Unique key for this device (VID:PID format).
     */
    val key: String get() = "${vendorId}:${productId}"

    /**
     * Human-readable identifier.
     */
    val displayId: String get() = "VID:0x${vendorId.toString(16).uppercase()} PID:0x${productId.toString(16).uppercase()}"

    companion object {
        /**
         * Parse a device from its key string.
         */
        fun fromKey(key: String, deviceName: String = "Unknown"): TrustedUsbDevice? {
            val parts = key.split(":")
            if (parts.size != 2) return null
            val vid = parts[0].toIntOrNull() ?: return null
            val pid = parts[1].toIntOrNull() ?: return null
            return TrustedUsbDevice(vid, pid, deviceName)
        }
    }
}

/**
 * Repository for managing trusted USB audio devices.
 *
 * Trusted devices will automatically be granted permission without showing
 * the system permission dialog repeatedly.
 *
 * Usage:
 * ```
 * val repository = TrustedUsbDevicesRepository(context)
 *
 * // Check if device is trusted
 * if (repository.isDeviceTrusted(vendorId, productId)) {
 *     // Skip permission dialog
 * }
 *
 * // Add device after user grants permission
 * repository.addTrustedDevice(vendorId, productId, "My Audio Interface")
 * ```
 */
class TrustedUsbDevicesRepository(
    private val context: Context
) {
    private object PreferenceKeys {
        val TRUSTED_DEVICE_KEYS = stringSetPreferencesKey("trusted_device_keys")

        // Store device names separately for each VID:PID
        fun deviceNameKey(vidPid: String) = stringSetPreferencesKey("device_name_$vidPid")
    }

    /**
     * Flow of all trusted devices.
     */
    val trustedDevices: Flow<Set<TrustedUsbDevice>> = context.trustedUsbDevicesDataStore.data
        .catch { exception ->
            if (exception is IOException) {
                emit(emptyPreferences())
            } else {
                throw exception
            }
        }
        .map { preferences ->
            val keys = preferences[PreferenceKeys.TRUSTED_DEVICE_KEYS] ?: emptySet()
            keys.mapNotNull { key ->
                // For now, use key as name - we'll enhance this later
                TrustedUsbDevice.fromKey(key, key)
            }.toSet()
        }

    /**
     * Check if a device is in the trusted list.
     *
     * @param vendorId USB Vendor ID
     * @param productId USB Product ID
     * @return true if device is trusted
     */
    suspend fun isDeviceTrusted(vendorId: Int, productId: Int): Boolean {
        val key = "${vendorId}:${productId}"
        val preferences = context.trustedUsbDevicesDataStore.data
            .catch { emit(emptyPreferences()) }
            .first()
        val trustedKeys = preferences[PreferenceKeys.TRUSTED_DEVICE_KEYS] ?: emptySet()
        return key in trustedKeys
    }

    /**
     * Add a device to the trusted list.
     *
     * Call this after the user grants USB permission to remember the device.
     *
     * @param vendorId USB Vendor ID
     * @param productId USB Product ID
     * @param deviceName Human-readable device name
     */
    suspend fun addTrustedDevice(vendorId: Int, productId: Int, deviceName: String) {
        val key = "${vendorId}:${productId}"
        context.trustedUsbDevicesDataStore.edit { preferences ->
            val currentKeys = preferences[PreferenceKeys.TRUSTED_DEVICE_KEYS] ?: emptySet()
            preferences[PreferenceKeys.TRUSTED_DEVICE_KEYS] = currentKeys + key
        }
        android.util.Log.i("TrustedUsbDevices", "Added trusted device: $deviceName ($key)")
    }

    /**
     * Remove a device from the trusted list.
     *
     * @param vendorId USB Vendor ID
     * @param productId USB Product ID
     */
    suspend fun removeTrustedDevice(vendorId: Int, productId: Int) {
        val key = "${vendorId}:${productId}"
        context.trustedUsbDevicesDataStore.edit { preferences ->
            val currentKeys = preferences[PreferenceKeys.TRUSTED_DEVICE_KEYS] ?: emptySet()
            preferences[PreferenceKeys.TRUSTED_DEVICE_KEYS] = currentKeys - key
        }
        android.util.Log.i("TrustedUsbDevices", "Removed trusted device: $key")
    }

    /**
     * Clear all trusted devices.
     */
    suspend fun clearAllTrustedDevices() {
        context.trustedUsbDevicesDataStore.edit { preferences ->
            preferences.remove(PreferenceKeys.TRUSTED_DEVICE_KEYS)
        }
        android.util.Log.i("TrustedUsbDevices", "Cleared all trusted devices")
    }

    /**
     * Get count of trusted devices.
     */
    suspend fun getTrustedDeviceCount(): Int {
        val preferences = context.trustedUsbDevicesDataStore.data
            .catch { emit(emptyPreferences()) }
            .first()
        return (preferences[PreferenceKeys.TRUSTED_DEVICE_KEYS] ?: emptySet()).size
    }
}
