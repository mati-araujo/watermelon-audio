package com.watermellonstudios.audio.domain.usb

/**
 * USB audio latency profile (Fase 1).
 *
 * Selects how the native USB transfer pipeline is parametrized, trading
 * buffering headroom against round-trip latency. The profile may only be
 * changed while USB streaming is stopped.
 *
 * - [SAFE]: current behavior — ~8 ms iso transfers, generous jitter budget.
 *   Maximum robustness across devices; round-trip on the order of tens of ms.
 * - [LOW_LATENCY]: 1 ms iso transfers, 4 URBs in flight, tight jitter budget
 *   and a small DSP block — targets ~10–14 ms round-trip. Disables adaptive
 *   buffering until the adaptive tuning of Fase 2 lands.
 *
 * The ordinal is the wire encoding used by the native bridge
 * (0 = SAFE, 1 = LOW_LATENCY); keep it in sync with the C++
 * `usb::UsbLatencyProfile` enum.
 */
enum class UsbLatencyProfile {
    SAFE,
    LOW_LATENCY,
}
