// Round-trip tests for the WAV writer/reader at all supported bit depths,
// plus metadata chunk emission. Files are written into a temp path that is
// platform-portable (CMake test working dir).
#include <gtest/gtest.h>
#include "WavFile.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

namespace {

constexpr int SR = 48000;
constexpr int FRAMES = 1024;

std::vector<float> makeStereoSineSweep(int frames, int sr) {
    std::vector<float> buf(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        // Sweep from 110 Hz to 880 Hz across the buffer.
        const float t = static_cast<float>(i) / static_cast<float>(sr);
        const float freq = 110.0f + (770.0f * static_cast<float>(i) / static_cast<float>(frames));
        const float s = 0.5f * std::sin(2.0f * static_cast<float>(M_PI) * freq * t);
        buf[i * 2]     = s;
        buf[i * 2 + 1] = -s;  // out of phase to verify channel separation
    }
    return buf;
}

const char* tempPath(const char* name) {
    static char buf[512];
    std::snprintf(buf, sizeof(buf), "looper_test_%s.wav", name);
    return buf;
}

}  // namespace

TEST(WavFile, WriteAndReadBack16Bit) {
    const auto src = makeStereoSineSweep(FRAMES, SR);
    const char* path = tempPath("rt16");
    ASSERT_TRUE(wav::writeWav(path, src.data(), FRAMES, SR, wav::BitDepth::PCM_16));

    auto data = wav::readWav(path);
    ASSERT_EQ(data.numFrames, FRAMES);
    ASSERT_EQ(data.sampleRate, SR);
    ASSERT_EQ(data.numChannels, 2);
    // 16-bit quantization noise: ±1/32768 ≈ 3e-5
    for (size_t i = 0; i < src.size(); ++i) {
        EXPECT_NEAR(data.buffer[i], src[i], 1e-4f);
    }
    std::remove(path);
}

TEST(WavFile, WriteAndReadBack24Bit) {
    const auto src = makeStereoSineSweep(FRAMES, SR);
    const char* path = tempPath("rt24");
    ASSERT_TRUE(wav::writeWav(path, src.data(), FRAMES, SR, wav::BitDepth::PCM_24));

    auto data = wav::readWav(path);
    ASSERT_EQ(data.numFrames, FRAMES);
    ASSERT_EQ(data.sampleRate, SR);
    // 24-bit quantization noise: ±1/8388608 ≈ 1.2e-7
    for (size_t i = 0; i < src.size(); ++i) {
        EXPECT_NEAR(data.buffer[i], src[i], 1e-6f);
    }
    std::remove(path);
}

TEST(WavFile, WriteAndReadBack32BitFloat) {
    const auto src = makeStereoSineSweep(FRAMES, SR);
    const char* path = tempPath("rt32");
    ASSERT_TRUE(wav::writeWav(path, src.data(), FRAMES, SR, wav::BitDepth::FLOAT_32));

    auto data = wav::readWav(path);
    ASSERT_EQ(data.numFrames, FRAMES);
    ASSERT_EQ(data.sampleRate, SR);
    // 32-bit float should be bit-perfect.
    for (size_t i = 0; i < src.size(); ++i) {
        EXPECT_FLOAT_EQ(data.buffer[i], src[i]);
    }
    std::remove(path);
}

TEST(WavFile, MetadataLISTChunkEmitted) {
    const auto src = makeStereoSineSweep(256, SR);
    const char* path = tempPath("meta");
    wav::WavMetadata meta;
    meta.projectName = "Saturday Jam";
    meta.artist = "Watermelon Studios";
    meta.comment = "test";
    meta.bpm = 120;
    ASSERT_TRUE(wav::writeWav(path, src.data(), 256, SR, wav::BitDepth::PCM_16, meta));

    // Inspect the file byte-by-byte for the LIST chunk + INAM/ICMT subchunks.
    std::ifstream f(path, std::ios::binary);
    ASSERT_TRUE(f.good());
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    auto contains = [&](const char* needle) {
        const std::string n(needle);
        return std::search(bytes.begin(), bytes.end(),
                           n.begin(), n.end()) != bytes.end();
    };
    EXPECT_TRUE(contains("LIST"));
    EXPECT_TRUE(contains("INFO"));
    EXPECT_TRUE(contains("INAM"));
    EXPECT_TRUE(contains("Saturday Jam"));
    EXPECT_TRUE(contains("ICMT"));
    EXPECT_TRUE(contains("BPM=120"));
    std::remove(path);
}

TEST(WavFile, RejectsInvalidArguments) {
    const auto src = makeStereoSineSweep(64, SR);
    EXPECT_FALSE(wav::writeWav(nullptr, src.data(), 64, SR));
    EXPECT_FALSE(wav::writeWav("x.wav", nullptr, 64, SR));
    EXPECT_FALSE(wav::writeWav("x.wav", src.data(), 0, SR));
}
