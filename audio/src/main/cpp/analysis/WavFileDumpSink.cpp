#include "WavFileDumpSink.h"

#include <bit>
#include <vector>

namespace wma::analysis {

static_assert(std::endian::native == std::endian::little,
              "el WAV es little-endian y las muestras se escriben tal cual las tiene la CPU");

WavFileDumpSink::~WavFileDumpSink() { closeFile(); }

void WavFileDumpSink::closeFile() noexcept {
    if (mFile != nullptr) {
        std::fclose(mFile);
        mFile = nullptr;
    }
}

bool WavFileDumpSink::openPart(const std::string& path, size_t headerBytes) {
    closeFile();
    const std::string part = path + ".part";
    mFile = std::fopen(part.c_str(), "wb");
    if (mFile == nullptr) return false;

    const std::vector<uint8_t> reserve(headerBytes, 0);
    if (std::fwrite(reserve.data(), 1, reserve.size(), mFile) != reserve.size()) {
        closeFile();
        std::remove(part.c_str());
        return false;
    }
    mPath = path;
    return true;
}

bool WavFileDumpSink::appendSamples(const float* samples, size_t count) {
    if (mFile == nullptr) return false;
    return std::fwrite(samples, sizeof(float), count, mFile) == count;
}

std::string WavFileDumpSink::finishPart(const uint8_t* header, size_t headerBytes, bool valid) {
    if (mFile == nullptr) return "";

    bool ok = std::fseek(mFile, 0, SEEK_SET) == 0 &&
              std::fwrite(header, 1, headerBytes, mFile) == headerBytes;
    // `fclose` vacia el buffer de stdio: un error aca es un archivo truncado.
    ok = (std::fclose(mFile) == 0) && ok;
    mFile = nullptr;
    if (!ok) return "";

    const std::string finalPath = valid ? mPath : invalidDumpPathFor(mPath);
    if (std::rename((mPath + ".part").c_str(), finalPath.c_str()) != 0) return "";
    return finalPath;
}

}  // namespace wma::analysis
