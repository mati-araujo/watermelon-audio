#pragma once

/**
 * @file FileUtf8.h
 * @brief El borde de paths de archivo: todo `const char*` que es un path es UTF-8 (REQ-061 D5).
 *
 * La JVM entrega los paths en UTF-8. En POSIX los bytes van tal cual al SO. En Windows los APIs
 * "estrechos" (`std::ofstream(const char*)`, `open`, `CreateFileA`) leen esos bytes con la pagina ANSI,
 * y un usuario «Martín» abre otro archivo o ninguno: aca se convierten a UTF-16 y se abre con el
 * constructor ancho. La conversion es ESTRICTA: un UTF-8 invalido no abre nada (falla cerrado).
 *
 * Header-only a proposito: lo incluye `looper/WavFile.h`, que compilan decenas de targets, y ninguno
 * tiene que linkear un `.cpp` de plataforma por esto. Sin headers de SO.
 */

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace wma { namespace platform {

/// UTF-8 -> UTF-16. false (y `out` sin garantias) ante UTF-8 invalido: continuacion faltante,
/// sobrelargo, sustituto (U+D800..DFFF), mayor a U+10FFFF o `utf8 == nullptr`.
inline bool utf8ToUtf16(const char* utf8, std::u16string& out) {
    out.clear();
    if (utf8 == nullptr) return false;
    const unsigned char* s = reinterpret_cast<const unsigned char*>(utf8);
    while (*s != 0) {
        const unsigned char lead = *s;
        uint32_t cp = 0;
        int continuation = 0;
        uint32_t minimum = 0;
        if (lead < 0x80) {
            cp = lead;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            cp = lead & 0x1Fu; continuation = 1; minimum = 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            cp = lead & 0x0Fu; continuation = 2; minimum = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            cp = lead & 0x07u; continuation = 3; minimum = 0x10000;
        } else {
            return false;
        }
        ++s;
        for (int i = 0; i < continuation; ++i, ++s) {
            if ((*s & 0xC0u) != 0x80u) return false;   // incluye el NUL final: truncado
            cp = (cp << 6) | (*s & 0x3Fu);
        }
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return true;
}

/// El path UTF-8 como `std::filesystem::path` nativo. Windows: vacio si el UTF-8 es invalido.
inline std::filesystem::path pathFromUtf8(const char* utf8) {
#if defined(_WIN32)
    std::u16string wide;
    if (!utf8ToUtf16(utf8, wide)) return {};
    return std::filesystem::path(std::wstring(wide.begin(), wide.end()));
#else
    return utf8 != nullptr ? std::filesystem::path(utf8) : std::filesystem::path();
#endif
}

/// Abre para escribir el path UTF-8. Si no abre, `is_open()` da false (tambien con UTF-8 invalido).
inline std::ofstream openOutUtf8(const char* utf8Path, std::ios::openmode mode) {
#if defined(_WIN32)
    const std::filesystem::path path = pathFromUtf8(utf8Path);
    if (path.empty()) return std::ofstream();
    return std::ofstream(path, mode);
#else
    return std::ofstream(utf8Path, mode);
#endif
}

/// Abre para leer el path UTF-8. Si no abre, `is_open()` da false (tambien con UTF-8 invalido).
inline std::ifstream openInUtf8(const char* utf8Path, std::ios::openmode mode) {
#if defined(_WIN32)
    const std::filesystem::path path = pathFromUtf8(utf8Path);
    if (path.empty()) return std::ifstream();
    return std::ifstream(path, mode);
#else
    return std::ifstream(utf8Path, mode);
#endif
}

}} // namespace wma::platform
