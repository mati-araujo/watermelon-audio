package com.watermellonstudios.audio.api.diagnostics

import com.watermellonstudios.audio.domain.usb.AdpfState
import com.watermellonstudios.audio.domain.usb.RtSchedResult
import com.watermellonstudios.audio.domain.usb.UsbLatencyProfile
import com.watermellonstudios.audio.domain.usb.UsbRtEnv
import com.watermellonstudios.audio.internal.bridge.AudioNativeBridge
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow

/**
 * Public diagnostics surface for the app's USB Lab / driver-log viewer (App V).
 *
 * Two capabilities:
 *  - [getUsbRtEnv]: the RT-environment snapshot (scheduling, ADPF, jitter budget).
 *  - Native log capture: enable a second in-memory sink of the driver's own
 *    logs (WMA_AUDIT / WMA_CLOCK / Libusb …) so they can be shown and exported
 *    without a USB cable (the DAC occupies the only port during validation).
 */
class DriverDiagnostics(
    private val bridge: AudioNativeBridge = AudioNativeBridge.getInstance(),
) {

    /** RT-environment snapshot, or null if no USB backend is running. */
    fun getUsbRtEnv(): UsbRtEnv? {
        val d = bridge.getUsbRtEnv() ?: return null
        if (d.size < 6) return null
        val profileOrdinal = d[5].toInt().coerceIn(0, UsbLatencyProfile.entries.lastIndex)
        return UsbRtEnv(
            dspSched = RtSchedResult.fromCode(d[0].toInt()),
            eventLoopSched = RtSchedResult.fromCode(d[1].toInt()),
            adpf = AdpfState.fromCode(d[2].toInt()),
            jitterBudgetMs = d[3].toInt(),
            convergedFloorMs = d[4].toInt(),
            profile = UsbLatencyProfile.entries[profileOrdinal],
        )
    }

    // ---- Native log capture ----

    /** Turn the in-memory native log capture on/off. */
    fun setLogCaptureEnabled(enabled: Boolean) = bridge.setLogCaptureEnabled(enabled)

    /** Lines dropped because the capture ring overflowed since it was cleared. */
    val droppedLogCount: Int get() = bridge.getLogCaptureDropped()

    /** Drain captured native lines once ("L/TAG: message"). */
    fun drainLogs(): List<String> = bridge.drainCapturedLogs().toList()

    /**
     * Enable capture and emit each batch of newly captured native log lines at
     * [intervalMs]. Capture stays enabled after collection stops; call
     * [setLogCaptureEnabled]`(false)` to turn it off.
     */
    fun logStream(intervalMs: Long = 250): Flow<List<String>> = flow {
        bridge.setLogCaptureEnabled(true)
        while (true) {
            val batch = bridge.drainCapturedLogs()
            if (batch.isNotEmpty()) emit(batch.toList())
            delay(intervalMs)
        }
    }
}
