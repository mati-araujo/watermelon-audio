#include "RealFFT.h"

#include "pffft.h"

#include <cstddef>
#include <cstring>

RealFFT::RealFFT(int n) {
    if (n < kMinSize || n > kMaxSize || (n & (n - 1)) != 0) return;

    const std::size_t bytes = static_cast<std::size_t>(n) * sizeof(float);
    in_ = static_cast<float*>(pffft_aligned_malloc(bytes));
    out_ = static_cast<float*>(pffft_aligned_malloc(bytes));
    work_ = static_cast<float*>(pffft_aligned_malloc(bytes));
    if (in_ != nullptr && out_ != nullptr && work_ != nullptr) {
        plan_ = pffft_new_setup(n, PFFFT_REAL);
    }
    if (plan_ == nullptr) {
        releaseBuffers();
        return;
    }
    size_ = n;
}

RealFFT::~RealFFT() { releaseBuffers(); }

void RealFFT::releaseBuffers() noexcept {
    if (plan_ != nullptr) pffft_destroy_setup(static_cast<PFFFT_Setup*>(plan_));
    pffft_aligned_free(in_);
    pffft_aligned_free(out_);
    pffft_aligned_free(work_);
    plan_ = nullptr;
    in_ = out_ = work_ = nullptr;
}

bool RealFFT::transformForward(const float* signal, float* spectrum) noexcept {
    if (!valid()) return false;
    const std::size_t bytes = static_cast<std::size_t>(size_) * sizeof(float);
    std::memcpy(in_, signal, bytes);
    // work_ nunca es null: con null, pffft pone el scratch en el stack.
    pffft_transform_ordered(static_cast<const PFFFT_Setup*>(plan_), in_, out_, work_, PFFFT_FORWARD);
    std::memcpy(spectrum, out_, bytes);
    return true;
}

bool RealFFT::transformInverse(const float* spectrum, float* signal) noexcept {
    if (!valid()) return false;
    std::memcpy(in_, spectrum, static_cast<std::size_t>(size_) * sizeof(float));
    pffft_transform_ordered(static_cast<const PFFFT_Setup*>(plan_), in_, out_, work_, PFFFT_BACKWARD);
    const float scale = 1.0f / static_cast<float>(size_);
    for (int i = 0; i < size_; ++i) signal[i] = out_[i] * scale;
    return true;
}
