/**
 * LatencyProfile.h
 *
 * USB audio latency profile — the set of tunable knobs that re-parametrize the
 * existing transfer pipeline to trade buffering headroom for round-trip latency.
 *
 * Fase 1 (docs/usb_latency/fase_1_latencia_configuracion.md):
 *   - SAFE        == current behavior, bit-identical (8 ms transfers, 24 ms
 *                    jitter budget, 256-frame DSP block, 100 ms ring capacity).
 *   - LOW_LATENCY == ~10–14 ms round-trip via 1 ms iso transfers, 4 URBs in
 *                    flight, 4 ms jitter budget and a 96-frame (2 ms) DSP block.
 *
 * The struct is plain data: LibusbBackend stores one instance and feeds its
 * fields into TransferConfig at setupTransferManager() time. The profile may
 * only change while the stream is stopped (enforced at the JNI/backend layer).
 */

#pragma once

namespace watermelon_audio {
namespace usb {

/**
 * Selectable latency profile. Mirrored 1:1 by the Kotlin UsbLatencyProfile enum
 * and the JNI integer encoding (0 = SAFE, 1 = LOW_LATENCY).
 */
enum class UsbLatencyProfile {
    SAFE = 0,
    LOW_LATENCY = 1,
};

/**
 * Fine-grained tuning knobs behind a latency profile. The presets below are the
 * supported configurations; the individual fields are also exposed for advanced
 * tuning (e.g. targetTransferMs=2 as a CPU/latency middle ground).
 */
struct UsbLatencyTuning {
    int targetTransferMs = 8;    // duration of each iso URB
    int numTransfers     = 3;    // URBs in flight per direction
    int jitterBudgetMs   = 24;   // pacer margin ABOVE one transfer
    int dspBlockFrames   = 256;  // user DSP callback block size
    int ringCapacityMs   = 100;  // ring capacity (NOT latency)

    /** == current behavior. */
    static UsbLatencyTuning safe() { return {}; }

    static UsbLatencyTuning lowLatency() {
        return {
            .targetTransferMs = 1,
            .numTransfers     = 4,
            .jitterBudgetMs   = 4,
            .dspBlockFrames   = 96,   // 2 ms @ 48k
            .ringCapacityMs   = 50,
        };
    }

    static UsbLatencyTuning forProfile(UsbLatencyProfile profile) {
        switch (profile) {
            case UsbLatencyProfile::LOW_LATENCY: return lowLatency();
            case UsbLatencyProfile::SAFE:        return safe();
        }
        return safe();
    }
};

// ============================================================================
// Pacer math (L2/L4) — pure helpers shared by UsbTransferManager and tests.
// ============================================================================

/**
 * Output ring pacer target, in float samples. One transfer worth of audio
 * (so the ring can always feed the next URB) plus jitterBudgetMs of absolute
 * wall-clock margin. SAFE (jitterBudgetMs=24, 8 ms transfer) yields the
 * historical 32 ms target; LOW_LATENCY (jitterBudgetMs=4, 1 ms transfer)
 * yields 5 ms.
 */
inline size_t outputRingTargetSamples(int packetsPerTransfer, int framesPerPacket,
                                      int jitterBudgetMs, int sampleRate,
                                      int channelCount) {
    const int framesPerTransfer = packetsPerTransfer * framesPerPacket;
    const int jitterFrames = jitterBudgetMs * sampleRate / 1000;
    return static_cast<size_t>(framesPerTransfer + jitterFrames)
        * static_cast<size_t>(channelCount);
}

/**
 * Initial output prefill, in float samples: numTransfers worth of audio
 * (consumed by the initial fills) plus the pacer target, so the ring lands
 * exactly at target once the initial transfers are submitted (L4).
 */
inline size_t outputPrefillSamples(int numTransfers, int packetsPerTransfer,
                                   int framesPerPacket, int channelCount,
                                   size_t targetSamples) {
    const size_t inflight = static_cast<size_t>(numTransfers) *
                            static_cast<size_t>(packetsPerTransfer) *
                            static_cast<size_t>(framesPerPacket) *
                            static_cast<size_t>(channelCount);
    return inflight + targetSamples;
}

}  // namespace usb
}  // namespace watermelon_audio
