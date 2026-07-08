#pragma once

/**
 * DspPacer.h
 *
 * Pure, host-testable extraction of the two most delicate decisions in the USB
 * DSP loop (LibusbBackend::dspThreadFunc): (1) the ring-level pacer gate that
 * decides WAIT vs PRODUCE each iteration, and (2) the combined-excess input
 * trim that bounds capture latency without breaking the pipeline conservation
 * law. Both used to live inline in the loop where they were untestable; this
 * header reproduces them bit-identically as free functions over plain numbers
 * (no libusb, no Android), so the host suite can pin their behavior (Etapa 2).
 *
 * See docs/usb-audio (Fase 1) for the rationale behind each rule. The comments
 * on the loop call sites remain the source of truth for *why*; this header is
 * purely the *what*, isolated for testing and for the later SRP split (Etapa 5).
 */

#include <algorithm>
#include <cstddef>

namespace watermelon_audio {
namespace usb {

/**
 * Outcome of the per-iteration pacer gate.
 * - WAIT             : the output ring is at/above target, or input isn't ready
 *                      and the output isn't critically low → block on the wake.
 * - PRODUCE          : normal produce cycle (input ready, or playback-only).
 * - PRODUCE_CRITICAL : emergency produce — output ring is critically low in
 *                      duplex and input hasn't delivered a block yet. Downstream
 *                      behavior is identical to PRODUCE (readInput fails without
 *                      consuming and the last-valid-block fade covers it); the
 *                      distinction exists for telemetry/tests.
 */
enum class PacerAction {
    WAIT,
    PRODUCE,
    PRODUCE_CRITICAL,
};

/** Inputs to the pacer gate, all in interleaved float samples unless noted. */
struct PacerState {
    bool hasPlayback = false;
    bool hasCapture = false;
    size_t outputRingLevel = 0;        // current fill of the output ring
    size_t outputRingTarget = 0;       // pacer target level
    size_t outputSamplesPerBlock = 0;  // one DSP block of output
    size_t inputAvailable = 0;         // samples available in the input ring
    size_t inputSamplesPerBlock = 0;   // one DSP block of input
};

/**
 * Ring-level pacer gate. Bit-identical to the inline logic:
 *   outputReady    = !hasPlayback || outputRingLevel < outputRingTarget
 *   inputReady     = !hasCapture  || inputAvailable  >= inputSamplesPerBlock
 *   outputCritical = hasPlayback && hasCapture &&
 *                    outputRingLevel < max(outputSamplesPerBlock, outputRingTarget/2)
 *   WAIT when !outputReady || (!inputReady && !outputCritical), else PRODUCE.
 */
inline PacerAction evaluatePacer(const PacerState& s) {
    const bool outputReady =
        !s.hasPlayback || (s.outputRingLevel < s.outputRingTarget);
    const bool inputReady =
        !s.hasCapture || (s.inputAvailable >= s.inputSamplesPerBlock);
    const bool outputCritical =
        s.hasPlayback && s.hasCapture &&
        s.outputRingLevel < std::max(s.outputSamplesPerBlock, s.outputRingTarget / 2);

    if (!outputReady || (!inputReady && !outputCritical)) {
        return PacerAction::WAIT;
    }
    return inputReady ? PacerAction::PRODUCE : PacerAction::PRODUCE_CRITICAL;
}

/** Inputs to the combined-excess trim, all in interleaved float samples. */
struct TrimState {
    bool hasPlayback = false;
    size_t inputAvailable = 0;         // samples in the input ring right now
    size_t inputTarget = 0;            // input ring target level
    int inputChannels = 1;
    size_t outputRingLevel = 0;
    size_t outputRingTarget = 0;
    int outputChannels = 1;
    int framesPerBlock = 0;
    size_t inputSamplesPerBlock = 0;   // samples consumed per trimmed block
};

/**
 * How many input blocks to discard this iteration to keep capture latency at
 * its target — but ONLY the excess the output ring does not need (conservation
 * law: a stall parks audio as input backlog that the output ring later reclaims;
 * trimming it unconditionally turns every transient stall into a permanent
 * output deficit). Both terms are normalized to frames because the rings may run
 * different channel counts.
 *
 * Bit-identical to the inline while-loop: same signed excess computation, same
 * decrement law, same termination condition. The caller executes exactly this
 * many readInput() trims, stopping early only if a read fails (as the original
 * did) — so a partial failure yields the same net trim count.
 */
inline int computeTrimBlocks(const TrimState& t) {
    if (t.framesPerBlock <= 0 || t.inputSamplesPerBlock == 0) {
        return 0;
    }
    const int inCh = std::max(1, t.inputChannels);
    long excessFrames =
        (static_cast<long>(t.inputAvailable) - static_cast<long>(t.inputTarget)) / inCh;
    if (t.hasPlayback) {
        const int outCh = std::max(1, t.outputChannels);
        excessFrames +=
            (static_cast<long>(t.outputRingLevel) - static_cast<long>(t.outputRingTarget)) /
            outCh;
    }

    int blocks = 0;
    size_t inAvail = t.inputAvailable;
    while (excessFrames >= t.framesPerBlock &&
           inAvail >= t.inputTarget + t.inputSamplesPerBlock) {
        inAvail -= t.inputSamplesPerBlock;
        excessFrames -= t.framesPerBlock;
        ++blocks;
    }
    return blocks;
}

}  // namespace usb
}  // namespace watermelon_audio
