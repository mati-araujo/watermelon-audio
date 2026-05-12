#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

/**
 * @brief Header-only WAV file reader/writer for stereo audio.
 *
 * Writer formats supported: 16-bit PCM, 24-bit PCM, 32-bit IEEE float.
 * Reader formats supported: 16/24-bit PCM and 32-bit IEEE float (mono auto-stereoized).
 * Optional LIST/INFO metadata chunk (BPM, project name, free-form comment).
 *
 * NOT RT-safe — call from IO/background thread only.
 */

namespace wav {

enum class BitDepth {
    PCM_16 = 16,
    PCM_24 = 24,
    FLOAT_32 = 32,
};

struct WavMetadata {
    std::string projectName;   // INAM
    std::string artist;        // IART
    std::string comment;       // ICMT (free-form, can include BPM, date)
    std::string software;      // ISFT (default: "Watermelon Audio")
    int bpm = 0;               // appended to ICMT if > 0
};

// ========== WAV Header Structures ==========

#pragma pack(push, 1)
struct WavHeader {
    char riff[4]{'R', 'I', 'F', 'F'};
    uint32_t fileSize{0};
    char wave[4]{'W', 'A', 'V', 'E'};
    char fmt[4]{'f', 'm', 't', ' '};
    uint32_t fmtSize{16};
    uint16_t audioFormat{1};  // 1 = PCM, 3 = IEEE float
    uint16_t numChannels{2};  // Stereo
    uint32_t sampleRate{48000};
    uint32_t byteRate{0};     // sampleRate * numChannels * bitsPerSample/8
    uint16_t blockAlign{0};   // numChannels * bitsPerSample/8
    uint16_t bitsPerSample{16};
    char data[4]{'d', 'a', 't', 'a'};
    uint32_t dataSize{0};
};
#pragma pack(pop)

// ========== Metadata helpers (RIFF LIST/INFO) ==========

namespace detail {

inline void writeUInt32LE(std::ofstream& f, uint32_t v) {
    f.write(reinterpret_cast<const char*>(&v), 4);
}

/**
 * @brief Build LIST/INFO chunk bytes for the given metadata. Returns empty
 *        vector if no fields are populated. RIFF info subchunks are 4-byte ID
 *        + uint32 size + null-terminated string + pad byte to align to 2.
 */
inline std::vector<uint8_t> buildInfoChunk(const WavMetadata& meta) {
    auto pushSub = [](std::vector<uint8_t>& out, const char* id, const std::string& s) {
        if (s.empty()) return;
        // Subchunk: ID(4) + size(4) + data + null + optional pad
        std::string payload = s;
        payload.push_back('\0');
        if (payload.size() & 1u) payload.push_back('\0');
        out.insert(out.end(), id, id + 4);
        const uint32_t sz = static_cast<uint32_t>(s.size() + 1);
        out.push_back(static_cast<uint8_t>(sz & 0xFF));
        out.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
        out.insert(out.end(), payload.begin(), payload.end());
    };

    std::vector<uint8_t> body;
    // "INFO" type identifier inside LIST chunk
    const char info[4] = {'I', 'N', 'F', 'O'};
    body.insert(body.end(), info, info + 4);

    pushSub(body, "INAM", meta.projectName);
    pushSub(body, "IART", meta.artist);
    std::string comment = meta.comment;
    if (meta.bpm > 0) {
        if (!comment.empty()) comment += " ";
        comment += "BPM=" + std::to_string(meta.bpm);
    }
    pushSub(body, "ICMT", comment);
    pushSub(body, "ISFT", meta.software.empty() ? std::string("Watermelon Audio") : meta.software);

    if (body.size() == 4) return {};  // only "INFO" tag, nothing else — skip

    // Wrap in LIST chunk: "LIST" + size + body
    std::vector<uint8_t> chunk;
    const char list[4] = {'L', 'I', 'S', 'T'};
    chunk.insert(chunk.end(), list, list + 4);
    const uint32_t sz = static_cast<uint32_t>(body.size());
    chunk.push_back(static_cast<uint8_t>(sz & 0xFF));
    chunk.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
    chunk.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
    chunk.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
    chunk.insert(chunk.end(), body.begin(), body.end());
    return chunk;
}

}  // namespace detail

// ========== WAV Writer ==========

/**
 * @brief Write stereo float audio to a WAV file.
 * @param filePath Output file path.
 * @param buffer Stereo interleaved float samples [-1.0..+1.0].
 * @param numFrames Number of stereo frames.
 * @param sampleRate Sample rate (default 48000).
 * @param bitDepth 16-bit PCM, 24-bit PCM, or 32-bit IEEE float.
 * @param meta Optional metadata. Empty fields are skipped.
 * @return true if successful.
 */
inline bool writeWav(const char* filePath,
                     const float* buffer,
                     int numFrames,
                     int sampleRate = 48000,
                     BitDepth bitDepth = BitDepth::PCM_16,
                     const WavMetadata& meta = {}) {
    if (!filePath || !buffer || numFrames <= 0) return false;

    WavHeader header;
    header.sampleRate = static_cast<uint32_t>(sampleRate);
    header.bitsPerSample = static_cast<uint16_t>(bitDepth);
    header.audioFormat = (bitDepth == BitDepth::FLOAT_32) ? 3 : 1;
    header.byteRate = header.sampleRate * header.numChannels * header.bitsPerSample / 8;
    header.blockAlign = header.numChannels * header.bitsPerSample / 8;
    header.dataSize = static_cast<uint32_t>(numFrames) * header.blockAlign;

    auto infoChunk = detail::buildInfoChunk(meta);
    const uint32_t infoSize = static_cast<uint32_t>(infoChunk.size());
    header.fileSize = sizeof(WavHeader) - 8 + header.dataSize + infoSize;

    std::ofstream file(filePath, std::ios::binary);
    if (!file.is_open()) return false;

    // RIFF header + fmt + data header
    file.write(reinterpret_cast<const char*>(&header), sizeof(WavHeader));

    // Audio payload — chunked to avoid large temp buffer.
    constexpr int CHUNK_FRAMES = 4096;
    if (bitDepth == BitDepth::PCM_16) {
        int16_t chunk[CHUNK_FRAMES * 2];
        for (int offset = 0; offset < numFrames; offset += CHUNK_FRAMES) {
            const int count = std::min(CHUNK_FRAMES, numFrames - offset);
            for (int i = 0; i < count * 2; ++i) {
                float s = std::clamp(buffer[(offset * 2) + i], -1.0f, 1.0f);
                chunk[i] = static_cast<int16_t>(s * 32767.0f);
            }
            file.write(reinterpret_cast<const char*>(chunk),
                       static_cast<std::streamsize>(count) * 2 * sizeof(int16_t));
        }
    } else if (bitDepth == BitDepth::PCM_24) {
        // 24-bit packed little-endian (3 bytes per sample).
        std::vector<uint8_t> chunk(static_cast<size_t>(CHUNK_FRAMES) * 2 * 3);
        for (int offset = 0; offset < numFrames; offset += CHUNK_FRAMES) {
            const int count = std::min(CHUNK_FRAMES, numFrames - offset);
            for (int i = 0; i < count * 2; ++i) {
                float s = std::clamp(buffer[(offset * 2) + i], -1.0f, 1.0f);
                int32_t v = static_cast<int32_t>(s * 8388607.0f);  // 2^23 - 1
                chunk[i * 3 + 0] = static_cast<uint8_t>(v & 0xFF);
                chunk[i * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
                chunk[i * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
            }
            file.write(reinterpret_cast<const char*>(chunk.data()),
                       static_cast<std::streamsize>(count) * 2 * 3);
        }
    } else {  // FLOAT_32
        for (int offset = 0; offset < numFrames; offset += CHUNK_FRAMES) {
            const int count = std::min(CHUNK_FRAMES, numFrames - offset);
            file.write(reinterpret_cast<const char*>(buffer + offset * 2),
                       static_cast<std::streamsize>(count) * 2 * sizeof(float));
        }
    }

    // LIST/INFO metadata chunk (optional, after audio data).
    if (!infoChunk.empty()) {
        file.write(reinterpret_cast<const char*>(infoChunk.data()),
                   static_cast<std::streamsize>(infoChunk.size()));
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
