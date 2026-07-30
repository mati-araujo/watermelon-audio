#pragma once

#include <vector>
#include <cstring>
#include <algorithm>

/**
 * @class AudioBuffer
 * @brief Buffer de audio multi-canal.
 *
 * Vive en `core/graph/` por historia: ese directorio ya no contiene un grafo
 * (ver la nota al principio de AudioNode.h). Lo usan los nodos y tambien el
 * looper (ChunkedAudioBuffer, TrackStorage), asi que no es una clase "de grafo".
 *
 * Soporta tanto formato por canales separados como interleaved (para Oboe).
 * Diseñado para ser RT-safe: sin allocations en hot path después de prepare().
 */
class AudioBuffer {
public:
    AudioBuffer() = default;

    explicit AudioBuffer(int numChannels, int numFrames)
        : mNumChannels(numChannels)
        , mNumFrames(numFrames)
        , mData(numChannels * numFrames, 0.0f) {}

    // Configuración
    void setSize(int numChannels, int numFrames) {
        mNumChannels = numChannels;
        mNumFrames = numFrames;
        mData.resize(numChannels * numFrames);
    }

    void prepare(int maxFrames) {
        mData.reserve(mNumChannels * maxFrames);
    }

    // Acceso a datos por canal (non-interleaved)
    float* getWritePointer(int channel) {
        return mData.data() + (channel * mNumFrames);
    }

    const float* getReadPointer(int channel) const {
        return mData.data() + (channel * mNumFrames);
    }

    // Para audio interleaved (formato Oboe)
    float* getInterleavedPointer() {
        return mData.data();
    }

    const float* getInterleavedPointer() const {
        return mData.data();
    }

    // Utilidades
    void clear() {
        std::fill(mData.begin(), mData.end(), 0.0f);
    }

    void clear(int startFrame, int numFrames) {
        for (int ch = 0; ch < mNumChannels; ++ch) {
            float* channelData = getWritePointer(ch);
            std::fill(channelData + startFrame,
                      channelData + startFrame + numFrames, 0.0f);
        }
    }

    void copyFrom(const AudioBuffer& source, int numFrames) {
        int framesToCopy = std::min(numFrames, std::min(mNumFrames, source.mNumFrames));
        int channels = std::min(mNumChannels, source.mNumChannels);

        for (int ch = 0; ch < channels; ++ch) {
            std::memcpy(getWritePointer(ch),
                        source.getReadPointer(ch),
                        framesToCopy * sizeof(float));
        }
    }

    void addFrom(int destChannel, const AudioBuffer& source,
                 int sourceChannel, int numFrames, float gain = 1.0f) {
        const float* src = source.getReadPointer(sourceChannel);
        float* dst = getWritePointer(destChannel);

        for (int i = 0; i < numFrames; ++i) {
            dst[i] += src[i] * gain;
        }
    }

    void applyGain(float gain) {
        for (float& sample : mData) {
            sample *= gain;
        }
    }

    void applyGain(int channel, float gain) {
        float* data = getWritePointer(channel);
        for (int i = 0; i < mNumFrames; ++i) {
            data[i] *= gain;
        }
    }

    // Copia de buffer interleaved estéreo a este buffer
    void copyFromInterleaved(const float* interleavedData, int numFrames) {
        if (mNumChannels < 2) return;
        float* left = getWritePointer(0);
        float* right = getWritePointer(1);
        for (int i = 0; i < numFrames; ++i) {
            left[i] = interleavedData[i * 2];
            right[i] = interleavedData[i * 2 + 1];
        }
    }

    // Copia de este buffer a formato interleaved estéreo
    void copyToInterleaved(float* interleavedData, int numFrames) const {
        if (mNumChannels < 2) return;
        const float* left = getReadPointer(0);
        const float* right = getReadPointer(1);
        for (int i = 0; i < numFrames; ++i) {
            interleavedData[i * 2] = left[i];
            interleavedData[i * 2 + 1] = right[i];
        }
    }

    // Suma de buffer interleaved estéreo a este buffer
    void addFromInterleaved(const float* interleavedData, int numFrames, float gain = 1.0f) {
        if (mNumChannels < 2) return;
        float* left = getWritePointer(0);
        float* right = getWritePointer(1);
        for (int i = 0; i < numFrames; ++i) {
            left[i] += interleavedData[i * 2] * gain;
            right[i] += interleavedData[i * 2 + 1] * gain;
        }
    }

    // Getters
    int getNumChannels() const { return mNumChannels; }
    int getNumFrames() const { return mNumFrames; }
    bool isEmpty() const { return mData.empty(); }

private:
    int mNumChannels = 2;
    int mNumFrames = 0;
    std::vector<float> mData;
};