package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.internal.usb.UsbAudioTestRunner
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob

/**
 * Factory for creating [UsbAudioTestRunner] instances.
 *
 * Usage:
 * ```kotlin
 * val testRunner = UsbAudioTestRunnerFactory.create(usbManager, scope)
 *
 * // Run a test
 * val result = testRunner.runTest(config)
 *
 * // Observe progress
 * testRunner.progress.collect { progress ->
 *     // Update UI
 * }
 * ```
 */
object UsbAudioTestRunnerFactory {

    /**
     * Create a new UsbAudioTestRunner instance.
     *
     * @param usbManager The USB audio manager to use for testing
     * @param scope Optional coroutine scope for async operations
     * @return UsbAudioTestRunner instance
     */
    fun create(
        usbManager: IUsbAudioManager,
        scope: CoroutineScope = CoroutineScope(Dispatchers.Default + SupervisorJob())
    ): UsbAudioTestRunner {
        return UsbAudioTestRunner(usbManager, scope)
    }
}
