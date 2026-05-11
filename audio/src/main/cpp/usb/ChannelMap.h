#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace watermelon_audio {
namespace usb {

class ChannelMap {
public:
    static constexpr int kMaxChannels = 8;

    using Matrix = std::array<std::array<float, kMaxChannels>, kMaxChannels>;

    static ChannelMap identity(int channels) {
        ChannelMap map;
        map.setIdentity(channels, channels, channels, channels);
        return map;
    }

    static ChannelMap identity(int outputEngineChannels,
                               int outputDeviceChannels,
                               int inputDeviceChannels,
                               int inputEngineChannels) {
        ChannelMap map;
        map.setIdentity(outputEngineChannels, outputDeviceChannels,
                        inputDeviceChannels, inputEngineChannels);
        return map;
    }

    static ChannelMap swapStereo() {
        ChannelMap map;
        map.setSilent(2, 2, 2, 2);
        map.mOutputMix[0][1] = 1.0f;
        map.mOutputMix[1][0] = 1.0f;
        map.mInputMix[0][1] = 1.0f;
        map.mInputMix[1][0] = 1.0f;
        return map;
    }

    static ChannelMap leftOnlyToStereo() {
        ChannelMap map;
        map.setSilent(2, 2, 2, 2);
        map.mOutputMix[0][0] = 1.0f;
        map.mOutputMix[1][0] = 1.0f;
        map.mInputMix[0][0] = 1.0f;
        map.mInputMix[1][0] = 1.0f;
        return map;
    }

    static ChannelMap monoToStereoDownmix() {
        ChannelMap map;
        map.setSilent(2, 1, 1, 2);
        map.mOutputMix[0][0] = 0.707f;
        map.mInputMix[0][0] = 0.707f;
        map.mInputMix[1][0] = 0.707f;
        return map;
    }

    void setIdentity(int outputEngineChannels,
                     int outputDeviceChannels,
                     int inputDeviceChannels,
                     int inputEngineChannels) {
        setSilent(outputEngineChannels, outputDeviceChannels,
                  inputDeviceChannels, inputEngineChannels);
        const int outputPairs = std::min(mOutputEngineChannels, mOutputDeviceChannels);
        for (int ch = 0; ch < outputPairs; ++ch) {
            mOutputMix[ch][ch] = 1.0f;
        }
        const int inputPairs = std::min(mInputDeviceChannels, mInputEngineChannels);
        for (int ch = 0; ch < inputPairs; ++ch) {
            mInputMix[ch][ch] = 1.0f;
        }
    }

    void setSilent(int outputEngineChannels,
                   int outputDeviceChannels,
                   int inputDeviceChannels,
                   int inputEngineChannels) {
        mOutputEngineChannels = clampChannels(outputEngineChannels);
        mOutputDeviceChannels = clampChannels(outputDeviceChannels);
        mInputDeviceChannels = clampChannels(inputDeviceChannels);
        mInputEngineChannels = clampChannels(inputEngineChannels);
        clear(mOutputMix);
        clear(mInputMix);
    }

    bool isOutputIdentity(int engineChannels, int deviceChannels) const {
        if (clampChannels(engineChannels) != mOutputEngineChannels ||
            clampChannels(deviceChannels) != mOutputDeviceChannels ||
            mOutputEngineChannels != mOutputDeviceChannels) {
            return false;
        }
        return isIdentityMatrix(mOutputMix, mOutputDeviceChannels, mOutputEngineChannels);
    }

    bool isInputIdentity(int deviceChannels, int engineChannels) const {
        if (clampChannels(deviceChannels) != mInputDeviceChannels ||
            clampChannels(engineChannels) != mInputEngineChannels ||
            mInputDeviceChannels != mInputEngineChannels) {
            return false;
        }
        return isIdentityMatrix(mInputMix, mInputEngineChannels, mInputDeviceChannels);
    }

    void applyOutput(const float* engineInput,
                     float* deviceOutput,
                     int frames,
                     int engineChannels,
                     int deviceChannels) const {
        const int sourceChannels = clampChannels(engineChannels);
        const int targetChannels = clampChannels(deviceChannels);
        for (int frame = 0; frame < frames; ++frame) {
            const float* inFrame = engineInput + frame * sourceChannels;
            float* outFrame = deviceOutput + frame * targetChannels;
            for (int outCh = 0; outCh < targetChannels; ++outCh) {
                float sample = 0.0f;
                for (int inCh = 0; inCh < sourceChannels; ++inCh) {
                    sample += inFrame[inCh] * mOutputMix[outCh][inCh];
                }
                outFrame[outCh] = sample;
            }
        }
    }

    void applyInput(const float* deviceInput,
                    float* engineOutput,
                    int frames,
                    int deviceChannels,
                    int engineChannels) const {
        const int sourceChannels = clampChannels(deviceChannels);
        const int targetChannels = clampChannels(engineChannels);
        for (int frame = 0; frame < frames; ++frame) {
            const float* inFrame = deviceInput + frame * sourceChannels;
            float* outFrame = engineOutput + frame * targetChannels;
            for (int outCh = 0; outCh < targetChannels; ++outCh) {
                float sample = 0.0f;
                for (int inCh = 0; inCh < sourceChannels; ++inCh) {
                    sample += inFrame[inCh] * mInputMix[outCh][inCh];
                }
                outFrame[outCh] = sample;
            }
        }
    }

    const Matrix& outputMix() const { return mOutputMix; }
    const Matrix& inputMix() const { return mInputMix; }

private:
    static int clampChannels(int channels) {
        return std::clamp(channels, 0, kMaxChannels);
    }

    static void clear(Matrix& matrix) {
        for (auto& row : matrix) {
            row.fill(0.0f);
        }
    }

    static bool isIdentityMatrix(const Matrix& matrix, int rows, int cols) {
        for (int row = 0; row < kMaxChannels; ++row) {
            for (int col = 0; col < kMaxChannels; ++col) {
                const float expected = (row < rows && col < cols && row == col) ? 1.0f : 0.0f;
                if (matrix[row][col] != expected) {
                    return false;
                }
            }
        }
        return true;
    }

    Matrix mOutputMix{};
    Matrix mInputMix{};
    int mOutputEngineChannels = 0;
    int mOutputDeviceChannels = 0;
    int mInputDeviceChannels = 0;
    int mInputEngineChannels = 0;
};

} // namespace usb
} // namespace watermelon_audio
