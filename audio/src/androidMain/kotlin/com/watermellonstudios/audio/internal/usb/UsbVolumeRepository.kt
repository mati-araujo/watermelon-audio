package com.watermellonstudios.audio.internal.usb

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.emptyPreferences
import androidx.datastore.preferences.core.floatPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import java.io.IOException

/**
 * DataStore instance for USB volume settings.
 */
private val Context.usbVolumeDataStore: DataStore<Preferences> by preferencesDataStore(
    name = "usb_volume_settings"
)

/**
 * Persisted volume settings for a USB device.
 */
data class PersistedUsbVolume(
    val outputVolume: Float = 1.0f,
    val inputVolume: Float = 1.0f,
    val outputMuted: Boolean = false,
    val inputMuted: Boolean = false
) {
    companion object {
        val DEFAULT = PersistedUsbVolume()
    }
}

/**
 * Repository for persisting USB volume settings per device (VID:PID).
 *
 * Volume settings are stored independently for each USB device,
 * allowing users to have different default volumes for different
 * audio interfaces.
 *
 * Usage:
 * ```
 * val repository = UsbVolumeRepository(context)
 *
 * // Get saved volume when device connects
 * val savedVolume = repository.getVolumeForDevice(vendorId, productId)
 * applyVolume(savedVolume.outputVolume)
 *
 * // Save when user changes volume
 * repository.saveOutputVolume(vendorId, productId, 0.75f)
 * ```
 */
class UsbVolumeRepository(
    private val context: Context
) {
    private object PreferenceKeys {
        fun outputVolumeKey(vidPid: String) = floatPreferencesKey("output_volume_$vidPid")
        fun inputVolumeKey(vidPid: String) = floatPreferencesKey("input_volume_$vidPid")
        fun outputMutedKey(vidPid: String) = booleanPreferencesKey("output_muted_$vidPid")
        fun inputMutedKey(vidPid: String) = booleanPreferencesKey("input_muted_$vidPid")
    }

    /**
     * Get device key from VID:PID.
     */
    private fun deviceKey(vendorId: Int, productId: Int): String = "$vendorId:$productId"

    /**
     * Get persisted volume for a device.
     *
     * @param vendorId USB Vendor ID
     * @param productId USB Product ID
     * @return Persisted volume settings or defaults
     */
    suspend fun getVolumeForDevice(vendorId: Int, productId: Int): PersistedUsbVolume {
        val vidPid = deviceKey(vendorId, productId)
        val prefs = context.usbVolumeDataStore.data
            .catch { exception ->
                if (exception is IOException) {
                    emit(emptyPreferences())
                } else {
                    throw exception
                }
            }
            .first()

        return PersistedUsbVolume(
            outputVolume = prefs[PreferenceKeys.outputVolumeKey(vidPid)] ?: 1.0f,
            inputVolume = prefs[PreferenceKeys.inputVolumeKey(vidPid)] ?: 1.0f,
            outputMuted = prefs[PreferenceKeys.outputMutedKey(vidPid)] ?: false,
            inputMuted = prefs[PreferenceKeys.inputMutedKey(vidPid)] ?: false
        )
    }

    /**
     * Save output volume for a device.
     *
     * @param vendorId USB Vendor ID
     * @param productId USB Product ID
     * @param volume Volume level 0.0 to 1.0
     */
    suspend fun saveOutputVolume(vendorId: Int, productId: Int, volume: Float) {
        val vidPid = deviceKey(vendorId, productId)
        context.usbVolumeDataStore.edit { prefs ->
            prefs[PreferenceKeys.outputVolumeKey(vidPid)] = volume.coerceIn(0f, 1f)
        }
    }

    /**
     * Save input volume for a device.
     *
     * @param vendorId USB Vendor ID
     * @param productId USB Product ID
     * @param volume Volume level 0.0 to 1.0
     */
    suspend fun saveInputVolume(vendorId: Int, productId: Int, volume: Float) {
        val vidPid = deviceKey(vendorId, productId)
        context.usbVolumeDataStore.edit { prefs ->
            prefs[PreferenceKeys.inputVolumeKey(vidPid)] = volume.coerceIn(0f, 1f)
        }
    }

    /**
     * Save output mute state for a device.
     */
    suspend fun saveOutputMuted(vendorId: Int, productId: Int, muted: Boolean) {
        val vidPid = deviceKey(vendorId, productId)
        context.usbVolumeDataStore.edit { prefs ->
            prefs[PreferenceKeys.outputMutedKey(vidPid)] = muted
        }
    }

    /**
     * Save input mute state for a device.
     */
    suspend fun saveInputMuted(vendorId: Int, productId: Int, muted: Boolean) {
        val vidPid = deviceKey(vendorId, productId)
        context.usbVolumeDataStore.edit { prefs ->
            prefs[PreferenceKeys.inputMutedKey(vidPid)] = muted
        }
    }

    /**
     * Save all volume settings at once.
     */
    suspend fun saveVolume(vendorId: Int, productId: Int, settings: PersistedUsbVolume) {
        val vidPid = deviceKey(vendorId, productId)
        context.usbVolumeDataStore.edit { prefs ->
            prefs[PreferenceKeys.outputVolumeKey(vidPid)] = settings.outputVolume.coerceIn(0f, 1f)
            prefs[PreferenceKeys.inputVolumeKey(vidPid)] = settings.inputVolume.coerceIn(0f, 1f)
            prefs[PreferenceKeys.outputMutedKey(vidPid)] = settings.outputMuted
            prefs[PreferenceKeys.inputMutedKey(vidPid)] = settings.inputMuted
        }
    }

    /**
     * Observe volume changes for a device.
     *
     * @param vendorId USB Vendor ID
     * @param productId USB Product ID
     * @return Flow of volume settings
     */
    fun observeVolumeForDevice(vendorId: Int, productId: Int): Flow<PersistedUsbVolume> {
        val vidPid = deviceKey(vendorId, productId)
        return context.usbVolumeDataStore.data
            .catch { exception ->
                if (exception is IOException) {
                    emit(emptyPreferences())
                } else {
                    throw exception
                }
            }
            .map { prefs ->
                PersistedUsbVolume(
                    outputVolume = prefs[PreferenceKeys.outputVolumeKey(vidPid)] ?: 1.0f,
                    inputVolume = prefs[PreferenceKeys.inputVolumeKey(vidPid)] ?: 1.0f,
                    outputMuted = prefs[PreferenceKeys.outputMutedKey(vidPid)] ?: false,
                    inputMuted = prefs[PreferenceKeys.inputMutedKey(vidPid)] ?: false
                )
            }
    }
}
