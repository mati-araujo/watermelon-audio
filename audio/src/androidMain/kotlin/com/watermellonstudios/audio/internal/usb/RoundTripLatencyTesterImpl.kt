package com.watermellonstudios.audio.internal.usb

import android.util.Log
import com.watermellonstudios.audio.domain.usb.IRoundTripLatencyTester
import com.watermellonstudios.audio.domain.usb.RoundTripTestConfig
import com.watermellonstudios.audio.domain.usb.RoundTripTestError
import com.watermellonstudios.audio.domain.usb.RoundTripTestProgress
import com.watermellonstudios.audio.domain.usb.RoundTripTestResult
import com.watermellonstudios.audio.domain.usb.RoundTripTestState
import com.watermellonstudios.audio.domain.usb.UsbLatencyProfile
import com.watermellonstudios.audio.internal.bridge.AudioNativeBridge
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Android implementation of [IRoundTripLatencyTester]: polls the native
 * RoundTripMeasurer (via [AudioNativeBridge]) at [POLL_INTERVAL_MS] and maps the
 * packed poll array to the domain types. The native side auto-restores the
 * backend's original callback on the first terminal poll, so no explicit swap
 * management is needed here.
 *
 * @param profileProvider supplies the active latency profile for result context
 *   (metadata only; defaults to SAFE).
 */
class RoundTripLatencyTesterImpl(
    private val bridge: AudioNativeBridge = AudioNativeBridge.getInstance(),
    private val profileProvider: () -> UsbLatencyProfile = { UsbLatencyProfile.SAFE },
) : IRoundTripLatencyTester {

    private val _progress = MutableStateFlow(RoundTripTestProgress())
    override val progress: StateFlow<RoundTripTestProgress> = _progress.asStateFlow()

    override suspend fun run(config: RoundTripTestConfig): RoundTripTestResult {
        val profile = profileProvider()
        if (!bridge.usbRoundTripStart(config.toFloatArray())) {
            // Native rejected: backend not running, not full-duplex, or busy. The
            // duplex case is the actionable one for the UI.
            Log.w(TAG, "usbRoundTripStart rejected (not running / not duplex / busy)")
            return RoundTripTestResult.error(RoundTripTestError.REQUIRES_FULL_DUPLEX, profile)
        }

        val timeoutMs = config.burstCount.toLong() * config.burstIntervalMs + GLOBAL_TIMEOUT_SLACK_MS
        val startTime = System.currentTimeMillis()

        while (true) {
            val data = bridge.usbRoundTripPoll()
            if (data == null || data.size < POLL_FIELDS) {
                delay(POLL_INTERVAL_MS)
                if (System.currentTimeMillis() - startTime > timeoutMs) {
                    bridge.usbRoundTripCancel()
                    return RoundTripTestResult.error(RoundTripTestError.TIMEOUT, profile)
                }
                continue
            }

            val state = RoundTripTestState.fromCode(data[0].toInt())
            _progress.value = RoundTripTestProgress(
                state = state,
                progressPct = data[1],
                currentBurst = data[2].toInt(),
                totalBursts = config.burstCount,
            )

            if (state == RoundTripTestState.COMPLETE || state == RoundTripTestState.ERROR) {
                return buildResult(data, config, profile)
            }

            if (System.currentTimeMillis() - startTime > timeoutMs) {
                bridge.usbRoundTripCancel()
                return RoundTripTestResult.error(RoundTripTestError.TIMEOUT, profile)
            }
            delay(POLL_INTERVAL_MS)
        }
    }

    override fun cancel() {
        bridge.usbRoundTripCancel()
        _progress.value = RoundTripTestProgress()
    }

    private fun buildResult(
        d: FloatArray,
        config: RoundTripTestConfig,
        profile: UsbLatencyProfile,
    ): RoundTripTestResult {
        val median = d[3]
        val swOut = d[6]
        val swIn = d[7]
        return RoundTripTestResult(
            medianMs = median,
            jitterMs = d[4],
            minMs = 0f,   // not carried in the poll array; median/jitter are the headline
            maxMs = 0f,
            confidence = d[5],
            validBursts = d[8].toInt(),
            totalBursts = config.burstCount,
            softwareOutputMs = swOut,
            softwareInputMs = swIn,
            residualMs = median - (swOut + swIn),
            sampleRate = 0,   // owned by the stream; not carried in the poll array
            profile = profile,
            error = RoundTripTestError.fromCode(d[9].toInt()),
        )
    }

    private companion object {
        const val TAG = "RoundTripTester"
        const val POLL_INTERVAL_MS = 75L
        const val POLL_FIELDS = 10
        const val GLOBAL_TIMEOUT_SLACK_MS = 5000L
    }
}
