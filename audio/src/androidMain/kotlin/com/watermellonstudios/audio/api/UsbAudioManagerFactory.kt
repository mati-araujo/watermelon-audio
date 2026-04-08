package com.watermellonstudios.audio.api

import android.content.Context
import com.watermellonstudios.audio.internal.usb.UsbAudioManagerImpl
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob

/**
 * Factory for creating [IUsbAudioManager] instances.
 *
 * Usage:
 * ```kotlin
 * val usbManager = UsbAudioManagerFactory.create(context)
 * usbManager.startMonitoring()
 *
 * // Observe devices
 * usbManager.connectedDevices.collect { devices ->
 *     // Update UI
 * }
 * ```
 */
object UsbAudioManagerFactory {

    /**
     * Create a new UsbAudioManager instance.
     *
     * @param context Application or Activity context
     * @param scope Optional coroutine scope for async operations
     * @return IUsbAudioManager implementation
     */
    fun create(
        context: Context,
        scope: CoroutineScope = CoroutineScope(Dispatchers.Main + SupervisorJob())
    ): IUsbAudioManager {
        return UsbAudioManagerImpl(
            context = context.applicationContext,
            scope = scope
        )
    }
}
