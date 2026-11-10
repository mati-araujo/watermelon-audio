#include "WavFileDumpSink.h"

#include <bit>
#include <vector>

#include "../platform/DumpFile.h"

namespace wma::analysis {

static_assert(std::endian::native == std::endian::little,
              "el WAV es little-endian y las muestras se escriben tal cual las tiene la CPU");

namespace {
using Outcome = RenameStepOutcome;
}  // namespace

bool renameWithoutReplacing(const std::string& from, const std::string& to,
                            NoReplaceRenameStep firstStep, RenameStepFn nativeFn,
                            RenameStepFn linkFn) {
    switch (firstStep) {
        case NoReplaceRenameStep::kNative: {
            const Outcome o = nativeFn != nullptr ? nativeFn(from, to) : platform::dumpRenameNative(from, to);
            if (o != Outcome::kUnsupported) return o == Outcome::kDone;
            [[fallthrough]];
        }
        case NoReplaceRenameStep::kLinkUnlink: {
            const Outcome o = linkFn != nullptr ? linkFn(from, to) : platform::dumpRenameByLink(from, to);
            if (o != Outcome::kUnsupported) return o == Outcome::kDone;
            [[fallthrough]];
        }
        case NoReplaceRenameStep::kCheckThenRename:
            return platform::dumpRenameAfterCheck(from, to) == Outcome::kDone;
    }
    return false;
}

WavFileDumpSink::~WavFileDumpSink() { closeFile(); }

void WavFileDumpSink::closeFile() noexcept {
    if (mFile != nullptr) {
        std::fclose(mFile);
        mFile = nullptr;
    }
}

bool WavFileDumpSink::openPart(const std::string& path, size_t headerBytes) {
    closeFile();
    mPath.clear();
    // El veredicto se sabe al cerrar: los dos nombres posibles tienen que estar libres ahora. Si
    // aparecen despues, `finishPart` igual se niega a pisarlos.
    if (platform::dumpPathMayExist(path) || platform::dumpPathMayExist(invalidDumpPathFor(path))) {
        return false;
    }

    const std::string part = path + ".part";
    // Exclusivo y sin seguir enlaces: si ya existe (incluido un symlink, aun colgante) falla sin
    // truncar ni seguir nada.
    platform::PartIdentity created;
    mFile = platform::dumpCreatePartExclusive(part, created);
    if (mFile == nullptr) return false;
    mPartDev = created.device;
    mPartIno = created.inode;

    const std::vector<uint8_t> reserve(headerBytes, 0);
    if (std::fwrite(reserve.data(), 1, reserve.size(), mFile) != reserve.size()) {
        closeFile();
        platform::dumpDiscardCreatedPart(part);
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
    if (!renameWithoutReplacing(mPath + ".part", finalPath)) return "";
    return finalPath;
}

std::string WavFileDumpSink::partPath() const { return mPath.empty() ? "" : mPath + ".part"; }

bool WavFileDumpSink::abortPart() {
    // Lo que se aborta se descarta: un error al cerrar no cambia que el archivo se borra.
    closeFile();
    if (mPath.empty()) return true;   // nunca abrio: no hay nada nuestro en disco
    const std::string part = mPath + ".part";
    mPath.clear();

    platform::PartIdentity created;
    created.device = mPartDev;
    created.inode = mPartIno;
    return platform::dumpRemovePartIfSame(part, created);
}

}  // namespace wma::analysis
