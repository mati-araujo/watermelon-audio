/**
 * TakeManifest.h — REQ-055 S0 · 0.3 (D35, D36). El MANIFIESTO de las tomas reales del afinador y la
 * validacion de sus cortes en TRES VIAS.
 *
 * EL MANIFIESTO (`tests/corpus-g42-manifest.txt`, D35)
 * ----------------------------------------------------
 * Una linea por toma con la forma de `corpus-manifest.txt` (asi `Corpus.h::stateOf` y
 * `fetch-corpus.sh` la leen igual):
 *
 *     NOMBRE SHA256 RELEASE RATE FRAMES FAMILIA descripcion...
 *
 * y DESPUES de la linea de su toma, los datos de esa toma con la sintaxis del TSV del oraculo:
 *
 *     # corte NOMBRE INI FIN PERDIDOS|?    intervalo cerrado en frames del archivo; perdida INI = FIN
 *     # objetivo NOMBRE FRAME HZ           la cuerda que el usuario eligio, aplicada en FRAME
 *     # aplauso NOMBRE FRAME               el aplauso de sincronizacion (D9): exactamente uno
 *
 * Cualquier otra linea `#` es comentario, salvo una cuya primera palabra empiece (sin importar
 * mayusculas ni un `#` pegado) con corte/objetivo/aplauso: es un dato mal escrito y se rechaza. Reglas propias (`parseTakeManifest`): ningun dato antes de
 * la linea de su toma; cortes estrictamente crecientes en INI con INI <= FIN <= FRAMES; `# objetivo`
 * no vacio, estrictamente creciente, en [0, FRAMES), con HZ de una cuerda de FAMILIA en
 * `catalogStrings()` y nunca dentro de (INI, FIN] de una costura; un solo `# aplauso`, en [0, FRAMES).
 *
 * 🔴 El manifiesto real NO se commitea antes del enganche post-057: su aparicion hace FALLAR
 * `LatchInstrumentRealTake` a proposito. Los tests escriben los suyos en temporales.
 *
 * LAS TRES VIAS (D36)
 * -------------------
 * Los cortes de una toma, como secuencia EXACTA de {ini, fin, perdidos|?}: manifiesto == `wmad` del
 * WAV == `# corte` del TSV del oraculo. El `wmad` lo lee `parseWmadV2Chunk`, un parser C++ PROPIO que
 * recorre el RIFF por offsets literales (no incluye `InputDumpStats`): el cruce que vale es entre dos
 * parsers independientes del mismo chunk (este y el de `corpus-reference-pitch.py`). El manifiesto ata
 * el asset y el TSV contra el drift. Ademas sha256 (manifiesto == archivo == `# archivo` del TSV) y
 * rate/frames (manifiesto == WAV == TSV): `checkTakeFile`.
 *
 * Todas las funciones devuelven el MOTIVO del rechazo ("" = bien): un rojo tiene que decir que fallo.
 */
#pragma once

#include "Corpus.h"
#include "SyntheticSignal.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace wma_test::take {

/// Un corte: intervalo CERRADO [ini, fin] en frames del archivo. `lost` = frames perdidos (perdida,
/// ini == fin) o -1 = `?` (costura de largo desconocido).
struct TakeCutLine {
    int64_t ini = 0;
    int64_t fin = 0;
    int64_t lost = -1;
    bool operator==(const TakeCutLine& o) const { return ini == o.ini && fin == o.fin && lost == o.lost; }
    bool operator!=(const TakeCutLine& o) const { return !(*this == o); }
};

/// `# objetivo NOMBRE FRAME HZ`.
struct TakeTargetLine {
    int64_t frame = 0;
    double hz = 0.0;
};

struct TakeEntry {
    std::string name;
    std::string sha256;
    std::string release;
    int rate = 0;
    int64_t frames = 0;
    std::string family;
    std::string description;
    std::vector<TakeCutLine> cuts;
    std::vector<TakeTargetLine> targets;
    std::vector<int64_t> claps;
};

struct TakeManifest {
    std::vector<TakeEntry> takes;
};

/// Texto -> string con el formato "%lld" sin depender de la plataforma.
inline std::string takeNum(int64_t v) { return std::to_string(static_cast<long long>(v)); }

inline std::string describeTakeCut(const TakeCutLine& c) {
    return "{" + takeNum(c.ini) + ", " + takeNum(c.fin) + ", " + (c.lost < 0 ? std::string("?") : takeNum(c.lost)) + "}";
}

/// Entero >= 0 en decimal estricto (sin signo, sin basura). false si no lo es.
inline bool parseTakeUInt(const std::string& s, int64_t* out) {
    if (s.empty() || s.size() > 18) return false;
    int64_t v = 0;
    for (char ch : s) {
        if (ch < '0' || ch > '9') return false;
        v = v * 10 + (ch - '0');
    }
    *out = v;
    return true;
}

/// Un NOMBRE de toma: `^[A-Za-z0-9_-][A-Za-z0-9._-]*$`. Ni `/`, ni `.` inicial, ni comillas: el nombre
/// termina en un path (`dir + "/" + nombre`) y en el `popen` de `corpus::sha256Of`, entre comillas simples.
inline bool isValidTakeName(const std::string& n) {
    if (n.empty() || n[0] == '.') return false;
    for (char ch : n) {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                        ch == '-' || ch == '.';
        if (!ok) return false;
    }
    return true;
}

/// Las palabras de una linea separadas por blancos.
inline std::vector<std::string> takeWords(const std::string& line) {
    std::vector<std::string> w;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '\r') ++j;
        if (j > i) w.push_back(line.substr(i, j - i));
        i = j;
    }
    return w;
}

/// `PERDIDOS|?` de una linea `# corte`. false si no es ni numero ni `?`.
inline bool parseTakeLost(const std::string& s, int64_t* out) {
    if (s == "?") {
        *out = -1;
        return true;
    }
    return parseTakeUInt(s, out);
}

/// Las alturas de las cuerdas de FAMILIA en el catalogo compartido. Vacio si la familia no existe.
inline std::vector<double> takeFamilyHz(const std::string& family) {
    std::vector<double> hz;
    for (const auto& s : wma_test::catalogStrings()) {
        const std::string n(s.name);
        if (n.size() > family.size() && n.compare(0, family.size(), family) == 0 && n[family.size()] == ' ')
            hz.push_back(s.hz);
    }
    return hz;
}

/// Lee y valida la gramatica del manifiesto (ver la cabecera). "" = bien.
inline std::string parseTakeManifest(const std::string& text, TakeManifest* out) {
    out->takes.clear();
    auto find = [out](const std::string& name) -> TakeEntry* {
        for (TakeEntry& e : out->takes)
            if (e.name == name) return &e;
        return nullptr;
    };
    size_t pos = 0;
    int lineNo = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        ++lineNo;
        const std::vector<std::string> w = takeWords(line);
        if (w.empty()) continue;
        const std::string where = "linea " + std::to_string(lineNo) + ": ";
        if (w[0][0] == '#') {
            // Un dato mal escrito (`#objetivo`, `# Objetivo`, `# objetivos`) NO es un comentario: tomarlo
            // como tal perderia un cambio de cuerda que ningun cruce delata (el `# corte` lo delatarian las
            // tres vias; el `# objetivo` y el `# aplauso`, no). Un comentario no puede empezar con esas palabras.
            const bool spaced = w[0] == "#";
            if (spaced && w.size() < 2) continue;
            std::string head = spaced ? w[1] : w[0].substr(1);
            for (char& ch : head) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const bool looksLikeData = head.rfind("corte", 0) == 0 || head.rfind("objetivo", 0) == 0 ||
                                       head.rfind("aplauso", 0) == 0;
            const std::string& kind = spaced ? w[1] : w[0];
            if (!looksLikeData) continue;   // comentario
            if (!spaced || (kind != "corte" && kind != "objetivo" && kind != "aplauso"))
                return where + "'" + line + "' parece un dato mal escrito (va '# corte|objetivo|aplauso NOMBRE ...')";
            if (w.size() < 3) return where + "'# " + kind + "' sin nombre de toma";
            TakeEntry* e = find(w[2]);
            if (e == nullptr) return where + "'# " + kind + " " + w[2] + "' antes de la linea de su toma";
            if (kind == "corte") {
                TakeCutLine c;
                if (w.size() != 6 || !parseTakeUInt(w[3], &c.ini) || !parseTakeUInt(w[4], &c.fin) ||
                    !parseTakeLost(w[5], &c.lost))
                    return where + "'# corte' ilegible (NOMBRE INI FIN PERDIDOS|?)";
                if (c.fin < c.ini) return where + "corte con FIN < INI";
                if (c.fin > e->frames) return where + "corte fuera de [0, FRAMES]";
                if (!e->cuts.empty() && c.ini <= e->cuts.back().ini)
                    return where + "cortes no estrictamente crecientes en INI";
                e->cuts.push_back(c);
            } else if (kind == "objetivo") {
                TakeTargetLine t;
                char* endp = nullptr;
                if (w.size() != 5 || !parseTakeUInt(w[3], &t.frame)) return where + "'# objetivo' ilegible (NOMBRE FRAME HZ)";
                t.hz = std::strtod(w[4].c_str(), &endp);
                if (endp == w[4].c_str() || *endp != '\0' || !(t.hz > 0.0)) return where + "'# objetivo' con HZ ilegible";
                if (t.frame >= e->frames) return where + "objetivo fuera de [0, FRAMES)";
                if (!e->targets.empty() && t.frame <= e->targets.back().frame)
                    return where + "objetivos no estrictamente crecientes";
                bool inFamily = false;
                for (double hz : takeFamilyHz(e->family))
                    if (std::fabs(hz - t.hz) < 1e-6) inFamily = true;
                if (!inFamily) return where + "objetivo " + w[4] + " Hz no es una cuerda de " + e->family;
                e->targets.push_back(t);
            } else {
                int64_t f = 0;
                if (w.size() != 4 || !parseTakeUInt(w[3], &f)) return where + "'# aplauso' ilegible (NOMBRE FRAME)";
                if (f >= e->frames) return where + "aplauso fuera de [0, FRAMES)";
                e->claps.push_back(f);
            }
            continue;
        }
        TakeEntry e;
        int64_t rate = 0;
        if (w.size() < 6) return where + "toma ilegible (NOMBRE SHA256 RELEASE RATE FRAMES FAMILIA desc)";
        e.name = w[0];
        if (!isValidTakeName(e.name)) return where + "NOMBRE invalido (solo [A-Za-z0-9._-], sin '.' inicial): " + e.name;
        e.sha256 = w[1];
        e.release = w[2];
        if (e.sha256.size() != 64) return where + "sha256 de " + e.name + " no tiene 64 caracteres";
        for (char ch : e.sha256)
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return where + "sha256 de " + e.name + " no es hex en minusculas";
        if (!parseTakeUInt(w[3], &rate) || rate <= 0 || rate > 1000000) return where + "RATE ilegible";
        e.rate = static_cast<int>(rate);
        if (!parseTakeUInt(w[4], &e.frames) || e.frames <= 0) return where + "FRAMES ilegible";
        e.family = w[5];
        if (takeFamilyHz(e.family).empty()) return where + "familia desconocida: " + e.family;
        for (size_t i = 6; i < w.size(); ++i) e.description += (i > 6 ? " " : "") + w[i];
        if (find(e.name) != nullptr) return where + "toma repetida: " + e.name;
        out->takes.push_back(e);
    }
    // Las reglas que cruzan lineas: se chequean con la toma completa (un corte puede venir despues
    // del objetivo que cae adentro de el).
    for (const TakeEntry& e : out->takes) {
        if (e.targets.empty()) return e.name + ": sin '# objetivo' (sin objetivo no hay lectura posible)";
        if (e.claps.size() != 1)
            return e.name + ": " + std::to_string(e.claps.size()) + " '# aplauso' (tiene que haber exactamente uno)";
        for (const TakeTargetLine& t : e.targets)
            for (const TakeCutLine& c : e.cuts)
                if (c.ini < c.fin && t.frame > c.ini && t.frame <= c.fin)
                    return e.name + ": objetivo en " + takeNum(t.frame) + " dentro de la costura " + describeTakeCut(c);
    }
    return "";
}

/// Lee el archivo entero. false si no existe.
inline bool slurpTakeFile(const std::string& path, std::string* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    out->clear();
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    std::fclose(f);
    return true;
}

/**
 * LA DECISION sobre las tomas, con la MISMA regla que `corpus::stateOf`: sin manifiesto, o un
 * manifiesto SIN TOMAS, es `kAbsent` (no haber verificado nada no es haber verificado); una toma
 * declarada que falta tambien; un sha256 distinto, o un manifiesto que no pasa su gramatica, es
 * `kCorrupt`. Solo `kVerified` habilita medir.
 */
///
/// `why` (opcional) recibe el MOTIVO de lo que no es `kVerified`: quien salte la medicion tiene que
/// poder decir por que (un HZ mal tipeado en el manifiesto no puede pasar por "no hay toma").
inline corpus::State takeStateOf(const std::string& dir, const std::string& manifestPath, std::string* why = nullptr) {
    auto because = [why](corpus::State st, const std::string& r) {
        if (why != nullptr) *why = r;
        return st;
    };
    std::string text;
    if (!slurpTakeFile(manifestPath, &text)) return because(corpus::State::kAbsent, "no existe " + manifestPath);
    TakeManifest m;
    const std::string bad = parseTakeManifest(text, &m);
    if (!bad.empty()) return because(corpus::State::kCorrupt, "manifiesto invalido: " + bad);
    if (m.takes.empty()) return because(corpus::State::kAbsent, "el manifiesto no declara tomas");
    bool corrupt = false;
    std::string corruptWhy;
    for (const TakeEntry& e : m.takes) {
        const std::string path = dir + "/" + e.name;
        if (!corpus::fileExists(path)) return because(corpus::State::kAbsent, "falta " + path);
        if (corpus::sha256Of(path) != e.sha256) {
            corrupt = true;
            corruptWhy = e.name + ": sha256 no coincide";
        }
    }
    if (why != nullptr) *why = corruptWhy;
    return corrupt ? corpus::State::kCorrupt : corpus::State::kVerified;
}

/// Lo que dice el `wmad` v2 de un archivo, leido por offsets literales.
struct WmadV2 {
    int fmtRate = 0;
    int fmtTag = 0;
    int fmtChannels = 0;
    int fmtBits = 0;
    uint64_t dataBytes = 0;
    uint64_t framesWritten = 0;
    uint64_t framesLost = 0;
    uint64_t discontinuities = 0;
    int32_t sampleRate = 0;
    int32_t inputSource = 0;
    uint32_t valid = 0;
    uint64_t unlocatedFrames = 0;
    uint64_t unlocatedDiscontinuities = 0;
    uint32_t cutCount = 0;
    uint32_t cutCapacity = 0;
    std::vector<TakeCutLine> cuts;
};

inline uint64_t takeLE(const uint8_t* p, int bytes) {
    uint64_t v = 0;
    for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

/**
 * El `wmad` v2 del archivo `bytes` (que se llama `name`), con los rechazos del oraculo: nombre
 * `.invalid.wav`; RIFF sin fmt/data/wmad; (sin aritmetica que de la vuelta en u64); fmt que no es float32 mono; version != 2; chunk != 64 + 24·cutCapacity; cutCount >
 * cutCapacity; kind fuera de {1, 2}; cortes desordenados (fileFrame decreciente) o fuera de
 * [0, framesWritten]; perdida con fileSpan; costura con lostFrames; valid = 0; unlocated* > 0;
 * framesLost != suma de perdidas + unlocatedFrames; rate de `wmad` != el de fmt; `data` !=
 * framesWritten·4. Los cortes salen como {fileFrame, fileFrame, lostFrames} (perdida) o
 * {fileFrame, fileFrame + fileSpan, ?} (costura). "" = bien.
 */
inline std::string parseWmadV2Chunk(const std::vector<uint8_t>& bytes, const std::string& name, WmadV2* out) {
    *out = WmadV2{};
    const std::string inv = ".invalid.wav";
    if (name.size() >= inv.size() && name.compare(name.size() - inv.size(), inv.size(), inv) == 0)
        return name + ": el nombre dice .invalid.wav";
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return name + ": no es un RIFF/WAVE";
    const uint8_t* wm = nullptr;
    uint64_t wmSize = 0;
    bool haveFmt = false, haveData = false;
    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const uint8_t* id = bytes.data() + pos;
        const uint64_t size = takeLE(bytes.data() + pos + 4, 4);
        const size_t body = pos + 8;
        if (size > bytes.size() - body) {
            if (std::memcmp(id, "data", 4) == 0) {
                return name + ": el chunk data declara " + std::to_string(size) + " bytes y hay " +
                       std::to_string(bytes.size() - body) + " (archivo cortado)";
            }
            return name + ": chunk que pasa el fin del archivo";
        }
        if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16) {
            haveFmt = true;
            out->fmtTag = static_cast<int>(takeLE(bytes.data() + body + 0, 2));
            out->fmtChannels = static_cast<int>(takeLE(bytes.data() + body + 2, 2));
            out->fmtBits = static_cast<int>(takeLE(bytes.data() + body + 14, 2));
            out->fmtRate = static_cast<int>(takeLE(bytes.data() + body + 4, 4));
        } else if (std::memcmp(id, "data", 4) == 0 && !haveData) {
            haveData = true;
            out->dataBytes = size;
        } else if (std::memcmp(id, "wmad", 4) == 0) {
            wm = bytes.data() + body;
            wmSize = size;
        }
        pos = body + static_cast<size_t>(size) + static_cast<size_t>(size & 1);
    }
    if (!haveFmt) return name + ": falta el chunk fmt";
    if (!haveData) return name + ": falta el chunk data";
    if (out->fmtTag != 3 || out->fmtChannels != 1 || out->fmtBits != 32)
        return name + ": el volcado es float32 mono (fmt 3, 1 canal, 32 bits)";
    if (wm == nullptr) return name + ": falta el chunk wmad";
    if (wmSize < 64) return name + ": chunk wmad de " + std::to_string(wmSize) + " bytes (v2 pide al menos 64)";
    const uint32_t version = static_cast<uint32_t>(takeLE(wm + 0, 4));
    if (version != 2) return name + ": wmad version " + std::to_string(version) + " (solo vale la 2)";
    out->framesWritten = takeLE(wm + 4, 8);
    out->framesLost = takeLE(wm + 12, 8);
    out->discontinuities = takeLE(wm + 20, 8);
    out->sampleRate = static_cast<int32_t>(takeLE(wm + 28, 4));
    out->inputSource = static_cast<int32_t>(takeLE(wm + 32, 4));
    out->valid = static_cast<uint32_t>(takeLE(wm + 36, 4));
    out->unlocatedFrames = takeLE(wm + 40, 8);
    out->unlocatedDiscontinuities = takeLE(wm + 48, 8);
    out->cutCount = static_cast<uint32_t>(takeLE(wm + 56, 4));
    out->cutCapacity = static_cast<uint32_t>(takeLE(wm + 60, 4));
    if (wmSize != 64 + 24 * static_cast<uint64_t>(out->cutCapacity))
        return name + ": chunk wmad de " + std::to_string(wmSize) + " bytes y 64 + 24·" +
               std::to_string(out->cutCapacity) + " es otro";
    if (out->cutCount > out->cutCapacity) return name + ": wmad cutCount mayor que cutCapacity";
    uint64_t prev = 0, lostSum = 0;
    for (uint32_t i = 0; i < out->cutCount; ++i) {
        const uint8_t* c = wm + 64 + 24 * static_cast<size_t>(i);
        const uint64_t frame = takeLE(c, 8);
        const uint64_t lost = takeLE(c + 8, 8);
        const uint64_t span = takeLE(c + 16, 4);
        const uint32_t kind = static_cast<uint32_t>(takeLE(c + 20, 4));
        const std::string which = name + ": wmad corte " + std::to_string(i);
        if (kind != 1 && kind != 2) return which + " con kind " + std::to_string(kind) + " (1 o 2)";
        if (frame < prev) return which + " desordenado";
        // Sin `frame + span`: con un frame cerca de 2^64 la suma da la vuelta y entra en rango.
        if (span > out->framesWritten || frame > out->framesWritten - span) return which + " fuera de [0, framesWritten]";
        if (kind == 1 && span != 0) return which + ": perdida con fileSpan";
        if (kind == 2 && lost != 0) return which + ": costura con lostFrames";
        // Invariante: lostSum <= framesLost (sin sumar: la suma de u64 da la vuelta).
        if (lost > out->framesLost - lostSum) return name + ": wmad framesLost no es la suma de los cortes + unlocatedFrames";
        prev = frame;
        lostSum += lost;
        TakeCutLine line;
        line.ini = static_cast<int64_t>(frame);
        line.fin = static_cast<int64_t>(kind == 2 ? frame + span : frame);
        line.lost = kind == 1 ? static_cast<int64_t>(lost) : -1;
        out->cuts.push_back(line);
    }
    if (out->valid == 0) return name + ": wmad valid=0";
    if (out->unlocatedFrames > 0 || out->unlocatedDiscontinuities > 0)
        return name + ": wmad con perdidas o discontinuidades NO ubicadas";
    if (out->unlocatedFrames != out->framesLost - lostSum)
        return name + ": wmad framesLost no es la suma de los cortes + unlocatedFrames";
    if (out->sampleRate != out->fmtRate) return name + ": el rate de wmad no es el de fmt";
    if (out->framesWritten > UINT64_MAX / 4 || out->dataBytes != out->framesWritten * 4)
        return name + ": data de " + std::to_string(out->dataBytes) + " bytes y framesWritten·4 es otro";
    return "";
}

/// Los `# corte NOMBRE INI FIN PERDIDOS|?` de `name` en el TSV del oraculo, en su orden. "" = bien.
inline std::string oracleCutsOfTake(const std::string& tsv, const std::string& name, std::vector<TakeCutLine>* out) {
    out->clear();
    size_t pos = 0;
    while (pos < tsv.size()) {
        size_t eol = tsv.find('\n', pos);
        if (eol == std::string::npos) eol = tsv.size();
        const std::vector<std::string> w = takeWords(tsv.substr(pos, eol - pos));
        pos = eol + 1;
        if (w.size() < 3 || w[0] != "#" || w[1] != "corte" || w[2] != name) continue;
        TakeCutLine c;
        if (w.size() != 6 || !parseTakeUInt(w[3], &c.ini) || !parseTakeUInt(w[4], &c.fin) || !parseTakeLost(w[5], &c.lost))
            return "TSV: '# corte " + name + "' ilegible";
        out->push_back(c);
    }
    return "";
}

/// Las tres vias, por secuencia EXACTA (ini, fin y perdidos|? de cada corte, en orden). "" = coinciden.
inline std::string takeCutsAgree(const std::vector<TakeCutLine>& manifest, const std::vector<TakeCutLine>& wmad,
                                 const std::vector<TakeCutLine>& oracle) {
    auto cmp = [](const char* a, const std::vector<TakeCutLine>& x, const char* b,
                  const std::vector<TakeCutLine>& y) -> std::string {
        if (x.size() != y.size())
            return std::string(a) + " declara " + std::to_string(x.size()) + " cortes y " + b + " " + std::to_string(y.size());
        for (size_t i = 0; i < x.size(); ++i)
            if (x[i] != y[i])
                return std::string("corte ") + std::to_string(i) + ": " + a + " " + describeTakeCut(x[i]) + " y " + b +
                       " " + describeTakeCut(y[i]);
        return "";
    };
    std::string why = cmp("el manifiesto", manifest, "el wmad", wmad);
    if (!why.empty()) return why;
    return cmp("el manifiesto", manifest, "el TSV", oracle);
}

/// `# archivo NOMBRE sha256 HEX rate HZ frames N` del TSV. false si no esta.
inline bool oracleHeaderOfTake(const std::string& tsv, const std::string& name, std::string* sha, int* rate, int64_t* frames) {
    size_t pos = 0;
    while (pos < tsv.size()) {
        size_t eol = tsv.find('\n', pos);
        if (eol == std::string::npos) eol = tsv.size();
        const std::vector<std::string> w = takeWords(tsv.substr(pos, eol - pos));
        pos = eol + 1;
        if (w.size() != 9 || w[0] != "#" || w[1] != "archivo" || w[2] != name || w[3] != "sha256" || w[5] != "rate" ||
            w[7] != "frames")
            continue;
        int64_t r = 0;
        if (!parseTakeUInt(w[6], &r) || !parseTakeUInt(w[8], frames)) return false;
        *sha = w[4];
        *rate = static_cast<int>(r);
        return true;
    }
    return false;
}

/**
 * Todo lo que ata una toma: sha256 (manifiesto == archivo == `# archivo` del TSV), rate y frames
 * (manifiesto == WAV == TSV), el `wmad` v2 sano (`parseWmadV2Chunk`) y los cortes en tres vias
 * (`takeCutsAgree`). `wmadOut` (opcional) recibe lo leido. "" = bien.
 */
inline std::string checkTakeFile(const std::string& dir, const TakeEntry& e, const std::string& oracleTsv,
                                 WmadV2* wmadOut = nullptr) {
    const std::string path = dir + "/" + e.name;
    std::string raw;
    if (!slurpTakeFile(path, &raw)) return e.name + ": no existe " + path;
    const std::string sha = corpus::sha256Of(path);
    if (sha != e.sha256) return e.name + ": sha256 del archivo " + sha + " y el manifiesto dice " + e.sha256;
    std::string tsvSha;
    int tsvRate = 0;
    int64_t tsvFrames = 0;
    if (!oracleHeaderOfTake(oracleTsv, e.name, &tsvSha, &tsvRate, &tsvFrames)) return e.name + ": el TSV no tiene su '# archivo'";
    if (tsvSha != e.sha256) return e.name + ": sha256 del TSV " + tsvSha + " y el manifiesto dice " + e.sha256;
    WmadV2 w;
    const std::vector<uint8_t> bytes(raw.begin(), raw.end());
    const std::string bad = parseWmadV2Chunk(bytes, e.name, &w);
    if (wmadOut != nullptr) *wmadOut = w;
    if (!bad.empty()) return bad;
    if (w.fmtRate != e.rate || tsvRate != e.rate)
        return e.name + ": rate manifiesto " + std::to_string(e.rate) + ", WAV " + std::to_string(w.fmtRate) + ", TSV " +
               std::to_string(tsvRate);
    if (static_cast<int64_t>(w.framesWritten) != e.frames || tsvFrames != e.frames)
        return e.name + ": frames manifiesto " + takeNum(e.frames) + ", WAV " + std::to_string(w.framesWritten) + ", TSV " +
               takeNum(tsvFrames);
    std::vector<TakeCutLine> oracle;
    const std::string tsvBad = oracleCutsOfTake(oracleTsv, e.name, &oracle);
    if (!tsvBad.empty()) return tsvBad;
    const std::string disagree = takeCutsAgree(e.cuts, w.cuts, oracle);
    return disagree.empty() ? "" : e.name + ": " + disagree;
}

}  // namespace wma_test::take
