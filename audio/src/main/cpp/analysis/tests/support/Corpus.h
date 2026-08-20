/**
 * Corpus.h — REQ-001 S10. La DECISION sobre el corpus grabado, aislada.
 *
 * Vive en su propia funcion y no adentro de cada test porque es lo que un mutante
 * romperia: convertir un SKIPPED en un PASSED es una linea, y el reporte de la
 * suite cambia de significado entero sin que nadie lo note.
 */
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

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
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0;
}

/// sha256 del archivo, en minusculas, o vacio si no se pudo.
inline std::string sha256Of(const std::string& path) {
    const std::string cmd = "shasum -a 256 '" + path + "' 2>/dev/null";
    std::FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe == nullptr) return {};
    char buf[256] = {0};
    const char* got = std::fgets(buf, sizeof(buf), pipe);
    ::pclose(pipe);
    if (got == nullptr) return {};
    std::string out(buf);
    const size_t sp = out.find(' ');
    return sp == std::string::npos ? std::string{} : out.substr(0, sp);
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
    char tmpl[] = "/tmp/wma-corpus-XXXXXX";
    const char* d = ::mkdtemp(tmpl);
    return d != nullptr ? std::string(d) : std::string{};
}

inline void removeTempDir(const std::string& dir) {
    if (dir.empty() || dir.find("/tmp/wma-corpus-") != 0) return;   // paranoia
    const std::string cmd = "rm -rf '" + dir + "'";
    (void)std::system(cmd.c_str());
}

}  // namespace wma_test::corpus
