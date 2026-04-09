/**
 * AudioFormatConverter.cpp
 *
 * Implementation of non-inline format conversion methods.
 */

#include "AudioFormatConverter.h"
#include <vector>

namespace watermelon_audio {
namespace usb {

AudioFormatConverter::AudioFormatConverter()
    : mDitheringEnabled(true)
    , mSoftClipThreshold(0.95f)
    , mDither()
{
}

void AudioFormatConverter::floatToPcm(
    const float* input,
    uint8_t* output,
    size_t numSamples,
    PcmFormat format
) {
    switch (format) {
        case PcmFormat::PCM_S16_LE:
            floatToS16(input, reinterpret_cast<int16_t*>(output), numSamples);
            break;

        case PcmFormat::PCM_S24_LE:
        case PcmFormat::PCM_S24_3LE:
            floatToS24_3LE(input, output, numSamples);
            break;

        case PcmFormat::PCM_S24_4LE:
            floatToS24_4LE(input, reinterpret_cast<int32_t*>(output), numSamples);
            break;

        case PcmFormat::PCM_S32_LE:
            floatToS32(input, reinterpret_cast<int32_t*>(output), numSamples);
            break;
    }
}

void AudioFormatConverter::pcmToFloat(
    const uint8_t* input,
    float* output,
    size_t numSamples,
    PcmFormat format
) {
    switch (format) {
        case PcmFormat::PCM_S16_LE:
            s16ToFloat(reinterpret_cast<const int16_t*>(input), output, numSamples);
            break;

        case PcmFormat::PCM_S24_LE:
        case PcmFormat::PCM_S24_3LE:
            s24_3LEToFloat(input, output, numSamples);
            break;

        case PcmFormat::PCM_S24_4LE:
            s24_4LEToFloat(reinterpret_cast<const int32_t*>(input), output, numSamples);
            break;

        case PcmFormat::PCM_S32_LE:
            s32ToFloat(reinterpret_cast<const int32_t*>(input), output, numSamples);
            break;
    }
}

size_t AudioFormatConverter::getBytesForSamples(PcmFormat format, size_t numSamples) {
    return getBytesPerSample(format) * numSamples;
}

int AudioFormatConverter::getBytesPerSample(PcmFormat format) {
    switch (format) {
        case PcmFormat::PCM_S16_LE:
            return 2;
        case PcmFormat::PCM_S24_LE:
        case PcmFormat::PCM_S24_3LE:
            return 3;
        case PcmFormat::PCM_S24_4LE:
        case PcmFormat::PCM_S32_LE:
            return 4;
        default:
            return 2;
    }
}

void AudioFormatConverter::convertFormat(
    const uint8_t* input,
    uint8_t* output,
    size_t numSamples,
    PcmFormat inputFormat,
    PcmFormat outputFormat
) {
    // Quick path: same format
    if (inputFormat == outputFormat) {
        size_t bytes = getBytesForSamples(inputFormat, numSamples);
        std::memcpy(output, input, bytes);
        return;
    }

    // Convert via float intermediate
    // Allocate temp buffer on stack for small conversions, heap for large
    constexpr size_t STACK_THRESHOLD = 4096;

    if (numSamples <= STACK_THRESHOLD) {
        float tempBuffer[STACK_THRESHOLD];
        pcmToFloat(input, tempBuffer, numSamples, inputFormat);
        floatToPcm(tempBuffer, output, numSamples, outputFormat);
    } else {
        // Large buffer - use heap
        std::vector<float> tempBuffer(numSamples);
        pcmToFloat(input, tempBuffer.data(), numSamples, inputFormat);
        floatToPcm(tempBuffer.data(), output, numSamples, outputFormat);
    }
}

} // namespace usb
} // namespace watermelon_audio
