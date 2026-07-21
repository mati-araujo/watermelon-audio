#pragma once

/**
 * DspBlockOps.h
 *
 * Pure, host-testable block transforms extracted from the USB duplex DSP loop
 * (LibusbBackend::dspThreadFunc). Three operations over a single
 * interleaved-stereo block that used to live inline in the loop — and, for the
 * splice head de-click, were DUPLICATED across the splice-pending and
 * underrun-fade paths. Reproduced here bit-identically as free functions over
 * plain float buffers (no libusb, no Android) so the host suite can pin the
 * delicate splice/fade behavior (hallazgo H6) and the SRP split (Etapa 5) can
 * proceed one seam at a time.
 *
 * The loop call-site comments remain the source of truth for *why*; this header
 * is purely the *what*, isolated for testing.
 */

#include <algorithm>
#include <cstddef>

namespace watermelon_audio {
namespace usb {

/** -3 dB gain used when duplicating a mono source into two channels. */
inline constexpr float kMonoToStereoGain = 0.707f;

/**
 * Mono → interleaved stereo with a fixed gain. Each mono frame is written to
 * both L and R. The default gain (−3 dB) prevents clipping when a mono capture
 * is duplicated to a stereo callback.
 *
 * @param mono   [in]  `frames` samples
 * @param stereo [out] `frames * 2` samples (interleaved LRLR…)
 */
inline void monoToStereo(const float* mono, float* stereo, int frames,
                         float gain = kMonoToStereoGain) {
    for (int i = 0; i < frames; ++i) {
        const float s = mono[i] * gain;
        stereo[i * 2]     = s;
        stereo[i * 2 + 1] = s;
    }
}

/**
 * Number of frames the head crossfade spans: capped at 48 so short blocks fade
 * fully rather than over-running the block.
 */
inline int spliceCrossfadeFrames(int frames) {
    return std::min(frames, 48);
}

/**
 * De-click the head of an interleaved-stereo block by crossfading its first
 * [spliceCrossfadeFrames] frames from a held (holdL, holdR) value into the
 * block:  out = w*block + (1-w)*hold,  with  w = (f+1)/xf  ramping  1/xf … 1.
 *
 * Used at a splice — a latency-trim discard, or resuming after a faded dropout
 * — to remove the step discontinuity at the join without repeating audio.
 */
inline void spliceDeclickHead(float* stereo, int frames, float holdL, float holdR) {
    const int xf = spliceCrossfadeFrames(frames);
    for (int f = 0; f < xf; ++f) {
        const float w = static_cast<float>(f + 1) / static_cast<float>(xf);
        stereo[f * 2]     = w * stereo[f * 2]     + (1.0f - w) * holdL;
        stereo[f * 2 + 1] = w * stereo[f * 2 + 1] + (1.0f - w) * holdR;
    }
}

/**
 * Linearly ramp an interleaved-stereo block from 1.0 down to ~0 across its full
 * length. The ramp is applied per-SAMPLE (fade = 1 − i/(frames*2)), so the L and
 * R of a frame receive adjacent, slightly different weights — preserved exactly
 * from the inline underrun path (an underrun repeats the previous block, so this
 * fades that repeat to silence instead of hard-cutting).
 */
inline void fadeBlockToSilence(float* stereo, int frames) {
    const std::size_t n = static_cast<std::size_t>(frames) * 2;
    for (std::size_t i = 0; i < n; ++i) {
        const float fade = 1.0f - (static_cast<float>(i) / static_cast<float>(n));
        stereo[i] *= fade;
    }
}

}  // namespace usb
}  // namespace watermelon_audio
