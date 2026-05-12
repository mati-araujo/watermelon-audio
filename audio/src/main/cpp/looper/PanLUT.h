#pragma once

#include <array>
#include <cmath>

namespace wm {

/**
 * @class EqualPowerPanLUT
 * @brief Precomputed equal-power (constant-power) pan lookup table.
 *
 * Maps pan ∈ [-1, +1] to (panL, panR) where panL² + panR² = 1.
 *   pan = -1  →  panL = 1, panR = 0   (full left)
 *   pan =  0  →  panL = panR ≈ 0.7071 (centre, -3 dB)
 *   pan = +1  →  panL = 0, panR = 1   (full right)
 *
 * Replaces per-sample `cos`/`sin` calls in the looper hot path. With 8 active
 * tracks and 480-frame callbacks, the previous trig cost was ~7680 transcendentals
 * per audio block; this LUT cuts it to a single integer multiply + 4 floats per
 * frame at the cost of ~2 KB of read-only memory.
 *
 * Thread-safe (read-only after construction). RT-safe.
 */
class EqualPowerPanLUT {
public:
    static constexpr int SIZE = 256;        // resolution: pan step ≈ 0.0078
    static constexpr int MASK = SIZE - 1;

    EqualPowerPanLUT() {
        // Pre-compute panL/panR for each step. Keep results in a single array
        // of pairs so a single LUT lookup gets both channels.
        for (int i = 0; i < SIZE; ++i) {
            const float pan = -1.0f + 2.0f * static_cast<float>(i) /
                              static_cast<float>(SIZE - 1);
            const float angle = (pan + 1.0f) * 0.25f * static_cast<float>(M_PI);
            mTable[i].l = std::cos(angle);
            mTable[i].r = std::sin(angle);
        }
    }

    struct Pair { float l; float r; };

    /** Look up (panL, panR) for pan ∈ [-1, +1]. Out-of-range values clamp. */
    Pair lookup(float pan) const {
        if (pan <= -1.0f) return mTable[0];
        if (pan >=  1.0f) return mTable[SIZE - 1];
        // Map pan ∈ [-1, +1] to index ∈ [0, SIZE-1].
        const float idxF = (pan + 1.0f) * 0.5f * static_cast<float>(SIZE - 1);
        const int idx = static_cast<int>(idxF);
        return mTable[idx & MASK];
    }

    /** Singleton accessor. Static-initialized once. */
    static const EqualPowerPanLUT& instance() {
        static const EqualPowerPanLUT s_lut;
        return s_lut;
    }

private:
    std::array<Pair, SIZE> mTable{};
};

}  // namespace wm
