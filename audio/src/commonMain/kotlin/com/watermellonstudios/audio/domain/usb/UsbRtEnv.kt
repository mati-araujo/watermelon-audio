package com.watermellonstudios.audio.domain.usb

/**
 * USB real-time environment snapshot for the USB Lab (App V §4, steps 2 & 5):
 * scheduling outcome of the DSP + event threads, ADPF state, and the live
 * adaptive jitter budget with its converged floor.
 */

/** Thread scheduling outcome (ThreadUtils::SchedResult ordinals; -1 = not yet run). */
enum class RtSchedResult(val code: Int) {
    UNKNOWN(-1),
    FIFO_GRANTED(0),
    RR_GRANTED(1),
    NICE_FALLBACK(2),
    FAILED(3);

    /** True when a hard/soft real-time policy was actually granted. */
    val isRealtime: Boolean get() = this == FIFO_GRANTED || this == RR_GRANTED

    companion object {
        fun fromCode(code: Int): RtSchedResult = entries.find { it.code == code } ?: UNKNOWN
    }
}

/** ADPF hint-session availability of the DSP loop. */
enum class AdpfState(val code: Int) {
    UNAVAILABLE(0),        // pre-API 33 or dlsym failed
    AVAILABLE_INACTIVE(1), // API present but no active session
    ACTIVE(2);             // hint session active

    companion object {
        fun fromCode(code: Int): AdpfState = entries.find { it.code == code } ?: UNAVAILABLE
    }
}

data class UsbRtEnv(
    val dspSched: RtSchedResult,
    val eventLoopSched: RtSchedResult,
    val adpf: AdpfState,
    val jitterBudgetMs: Int,
    val convergedFloorMs: Int,
    val profile: UsbLatencyProfile,
)
