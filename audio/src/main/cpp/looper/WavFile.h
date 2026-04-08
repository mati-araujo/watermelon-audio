#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include <cmath>
#include <algorithm>

/**
 * @brief Header-only WAV file reader/writer for PCM 16-bit stereo audio.
 *
 * WavWriter: Mixes looper tracks and writes to a .wav file.
 * WavReader: Reads a .wav file into a float buffer (stereo interleaved).
 *
 * NOT RT-safe — call from IO/background thread only.
 */

namespace wav {

// ========== WAV Header Structures ==========

#pragma pack(push, 1)
struct WavHeader {
    char riff[4]{'R', 'I', 'F', 'F'};
    uint32_t fileSize{0};
    char wave[4]{'W', 'A', 'V', 'E'};
    char fmt[4]{'f', 'm', 't', ' '};
    uint32_t fmtSize{16};
    uint16_t audioFormat{1};  // PCM
    uint16_t numChannels{2};  // Stereo
    uint32_t sampleRate{48000};
    uint32_t byteRate{0};     // sampleRate * numChannels * bitsPerSample/8
    uint16_t blockAlign{0};   // numChannels * bitsPerSample/8
    uint16_t bitsPerSample{16};
    char data[4]{'d', 'a', 't', 'a'};
    uint32_t dataSize{0};
};
#pragma pack(pop)

// ========== WAV Writer ==========

/**
 * @brief Write stereo float audio to a 16-bit PCM WAV file.
 * @param filePath Output file path
 * @param buffer Stereo interleaved float samples (-1.0..+1.0)
 * @param numFrames Number of stereo frames
 * @param sampleRate Sample rate (default 48000)
 * @return true if successful
 */
inline bool writeWav(const char* filePath,
                     const float* buffer,
                     int numFrames,
                     int sampleRate = 48000) {
    if (!filePath || !buffer || numFrames <= 0) return false;

    WavHeader header;
    header.sampleRate = static_cast<uint32_t>(sampleRate);
    header.byteRate = header.sampleRate * header.numChannels * header.bitsPerSample / 8;
    header.blockAlign = header.numChannels * header.bitsPerSample / 8;
    header.dataSize = static_cast<uint32_t>(numFrames) * header.blockAlign;
    header.fileSize = sizeof(WavHeader) - 8 + header.dataSize;

    std::ofstream file(filePath, std::ios::binary);
    if (!file.is_open()) return false;

    // Write header
    file.write(reinterpret_cast<const char*>(&header), sizeof(WavHeader));

    // Convert float -> int16 and write in chunks to avoid large temp buffer
    constexpr int CHUNK_FRAMES = 4096;
    int16_t chunk[CHUNK_FRAMES * 2];

    for (int offset = 0; offset < numFrames; offset += CHUNK_FRAMES) {
        int count = std::min(CHUNK_FRAMES, numFrames - offset);
        for (int i = 0; i < count * 2; ++i) {
            float sample = buffer[(offset * 2) + i];
            // Clamp and convert to int16
            sample = std::clamp(sample, -1.0f, 1.0f);
            chunk[i] = static_cast<int16_t>(sample * 32767.0f);
        }
        file.write(reinterpret_cast<const char*>(chunk),
                   static_cast<std::streamsize>(count) * 2 * sizeof(int16_t));
    }

    return file.good();
}

// ========== WAV Reader ==========

struct WavData {
    std::vector<float> buffer;  // Stereo interleaved float samples
    int numFrames{0};
    int sampleRate{0};
    int numChannels{0};
};

/**
 * @brief Read a WAV file into stereo interleaved float buffer.
 *        Supports 16-bit and 24-bit PCM. Mono files are duplicated to stereo.
 * @param filePath Input file path
 * @return WavData with buffer, or empty on failure
 */
inline WavData readWav(const char* filePath) {
    WavData result;
    if (!filePath) return result;

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return result;

    // Read and validate RIFF header
    char riff[4], wave[4];
    uint32_t fileSize;
    file.read(riff, 4);
    file.read(reinterpret_cast<char*>(&fileSize), 4);
    file.read(wave, 4);
    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
        return result;
    }

    // Find fmt and data chunks
    uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0, dataSize = 0;
    bool foundFmt = false, foundData = false;

    while (!file.eof() && !(foundFmt && foundData)) {
        char chunkId[4];
        uint32_t chunkSize;
        file.read(chunkId, 4);
        file.read(reinterpret_cast<char*>(&chunkSize), 4);
        if (!file.good()) break;

        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            file.read(reinterpret_cast<char*>(&audioFormat), 2);
            file.read(reinterpret_cast<char*>(&numChannels), 2);
            file.read(reinterpret_cast<char*>(&sampleRate), 4);
            file.seekg(6, std::ios::cur);  // skip byteRate + blockAlign
            file.read(reinterpret_cast<char*>(&bitsPerSample), 2);
            // Skip any extra format bytes
            if (chunkSize > 16) {
                file.seekg(chunkSize - 16, std::ios::cur);
            }
            foundFmt = true;
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            dataSize = chunkSize;
            foundData = true;
            // Don't skip — we'll read data next
        } else {
            file.seekg(chunkSize, std::ios::cur);
        }
    }

    if (!foundFmt || !foundData) return result;
    // Support PCM (1) and IEEE float (3)
    if (audioFormat != 1 && audioFormat != 3) return result;
    if (numChannels < 1 || numChannels > 2) return result;
    if (audioFormat == 1 && bitsPerSample != 16 && bitsPerSample != 24) return result;
    if (audioFormat == 3 && bitsPerSample != 32) return result;

    int bytesPerSample = bitsPerSample / 8;
    int totalSamples = static_cast<int>(dataSize) / bytesPerSample;
    int numFrames = totalSamples / numChannels;

    // Read raw PCM data
    std::vector<uint8_t> rawData(dataSize);
    file.read(reinterpret_cast<char*>(rawData.data()), dataSize);
    if (!file.good() && !file.eof()) return result;

    // Convert to stereo interleaved float
    result.buffer.resize(static_cast<size_t>(numFrames) * 2);
    result.numFrames = numFrames;
    result.sampleRate = static_cast<int>(sampleRate);
    result.numChannels = static_cast<int>(numChannels);

    for (int frame = 0; frame < numFrames; ++frame) {
        float left, right;

        if (audioFormat == 3 && bitsPerSample == 32) {
            // IEEE 32-bit float — read directly
            int sampleIdx = frame * numChannels;
            auto readFloat32 = [&](int idx) -> float {
                float val;
                std::memcpy(&val, &rawData[idx * 4], 4);
                return val;
            };
            left = readFloat32(sampleIdx);
            right = (numChannels == 2) ? readFloat32(sampleIdx + 1) : left;
        } else if (bitsPerSample == 16) {
            int sampleIdx = frame * numChannels;
            auto readInt16 = [&](int idx) -> float {
                int16_t val;
                std::memcpy(&val, &rawData[idx * 2], 2);
                return static_cast<float>(val) / 32768.0f;
            };
            left = readInt16(sampleIdx);
            right = (numChannels == 2) ? readInt16(sampleIdx + 1) : left;
        } else {  // 24-bit
            int sampleIdx = frame * numChannels;
            auto readInt24 = [&](int idx) -> float {
                int byteOffset = idx * 3;
                int32_t val = static_cast<int32_t>(rawData[byteOffset])
                            | (static_cast<int32_t>(rawData[byteOffset + 1]) << 8)
                            | (static_cast<int32_t>(rawData[byteOffset + 2]) << 16);
                if (val & 0x800000) val |= 0xFF000000;  // Sign extend
                return static_cast<float>(val) / 8388608.0f;
            };
            left = readInt24(sampleIdx);
            right = (numChannels == 2) ? readInt24(sampleIdx + 1) : left;
        }

        result.buffer[frame * 2] = left;
        result.buffer[frame * 2 + 1] = right;
    }

    return result;
}

}  // namespace wav
