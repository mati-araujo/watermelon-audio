/**
 * Corpus.h — REQ-001 S10. La DECISION sobre el corpus grabado, aislada.
 *
 * Vive en su propia funcion y no adentro de cada test porque es lo que un mutante
 * romperia: convertir un SKIPPED en un PASSED es una linea, y el reporte de la
 * suite cambia de significado entero sin que nadie lo note.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "platform/FileUtf8.h"
#include "tests/support/PathUtf8.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace wma_test::corpus {

enum class State {
    kAbsent,    ///< no hay directorio, o el manifiesto no declara nada
    kCorrupt,   ///< hay archivos y algun checksum NO coincide
    kVerified,  ///< todos los declarados estan y coinciden
};

inline std::string manifestPath() {
    return std::string(WMA_ANALYSIS_GOLDEN_DIR) + "/../corpus-manifest.txt";
}

inline std::string defaultCorpusDir() {
    if (const char* env = std::getenv("WMA_CORPUS_DIR")) return env;
    return std::string(WMA_ANALYSIS_GOLDEN_DIR) + "/../corpus";
}

inline bool fileExists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::exists(wma::platform::pathFromUtf8(p.c_str()), ec);
}

/// SHA-256 (FIPS 180-4) incremental. Vive en el propio test y no en el binario `shasum`: ese no existe
/// en Windows ni en un contenedor minimo, y un hash que no se pudo calcular se leia como corpus corrupto.
class Sha256 {
public:
    void update(const unsigned char* data, size_t n) {
        mTotal += n;
        while (n > 0) {
            const size_t take = std::min(n, sizeof(mBlock) - mFill);
            std::copy(data, data + take, mBlock + mFill);
            mFill += take; data += take; n -= take;
            if (mFill == sizeof(mBlock)) { compress(mBlock); mFill = 0; }
        }
    }

    std::string hexDigest() {
        const uint64_t bits = mTotal * 8;
        const unsigned char one = 0x80, zero = 0;
        update(&one, 1);
        while (mFill != 56) update(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        update(len, 8);
        static const char* kHex = "0123456789abcdef";
        std::string out;
        for (uint32_t w : mState)
            for (int i = 3; i >= 0; --i) {
                const unsigned b = (w >> (8 * i)) & 0xFFu;
                out.push_back(kHex[b >> 4]);
                out.push_back(kHex[b & 0xF]);
            }
        return out;
    }

private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress(const unsigned char* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) |
                   uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = mState[0], b = mState[1], c = mState[2], d = mState[3];
        uint32_t e = mState[4], f = mState[5], g = mState[6], h = mState[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        mState[0] += a; mState[1] += b; mState[2] += c; mState[3] += d;
        mState[4] += e; mState[5] += f; mState[6] += g; mState[7] += h;
    }

    uint32_t mState[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char mBlock[64] = {0};
    size_t mFill = 0;
    uint64_t mTotal = 0;
};

/// sha256 del archivo, en minusculas, o vacio si no se pudo leer.
inline std::string sha256Of(const std::string& path) {
    std::ifstream in(wma::platform::pathFromUtf8(path.c_str()), std::ios::binary);
    if (!in) return {};
    Sha256 h;
    char buf[64 * 1024];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
        h.update(reinterpret_cast<const unsigned char*>(buf), static_cast<size_t>(in.gcount()));
    if (in.bad()) return {};
    return h.hexDigest();
}

/**
 * La decision. **Un manifiesto sin entradas es `kAbsent`, no `kVerified`**: no
 * haber encontrado nada que verificar no es haber verificado.
 */
inline State stateOf(const std::string& corpusDir, const std::string& manifest) {
    std::FILE* f = std::fopen(manifest.c_str(), "rb");
    if (f == nullptr) return State::kAbsent;

    int declared = 0;
    bool corrupt = false;
    char line[1024];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char name[512] = {0}, want[256] = {0};
        if (std::sscanf(line, "%511s %255s", name, want) != 2) continue;
        ++declared;
        const std::string path = corpusDir + "/" + name;
        if (!fileExists(path)) { corrupt = false; std::fclose(f); return State::kAbsent; }
        if (sha256Of(path) != std::string(want)) corrupt = true;
    }
    std::fclose(f);

    if (declared == 0) return State::kAbsent;
    return corrupt ? State::kCorrupt : State::kVerified;
}

/// Sólo un corpus VERIFICADO habilita la robustez. Corrupto no alcanza.
inline bool shouldRunRobustness(State s) { return s == State::kVerified; }

/// Y sólo una corrida que de verdad verifico cuenta como cobertura.
inline bool countsAsCoverage(State s) { return s == State::kVerified; }

inline const char* describe(State s) {
    switch (s) {
        case State::kAbsent:  return "ausente o manifiesto vacio";
        case State::kCorrupt: return "checksum no coincide";
        case State::kVerified: return "verificado";
    }
    return "desconocido";
}

inline std::string makeTempDir() {
    static std::atomic<int> sCounter{0};
#if defined(_WIN32)
    const long long pid = _getpid();
#else
    const long long pid = getpid();
#endif
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) /
        ("wma-corpus-" + std::to_string(pid) + "-" + std::to_string(sCounter.fetch_add(1)));
    if (ec || !std::filesystem::create_directory(dir, ec) || ec) return {};
    return wma_test::pathUtf8(dir);
}

inline void removeTempDir(const std::string& dir) {
    const std::filesystem::path p = wma::platform::pathFromUtf8(dir.c_str());
    if (dir.empty() || p.filename().string().rfind("wma-corpus-", 0) != 0) return;   // paranoia
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
}

}  // namespace wma_test::corpus
