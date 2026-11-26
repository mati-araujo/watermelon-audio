#pragma once

#include <filesystem>
#include <string>
#include <utility>

namespace wma_test {

/// El path como UTF-8, que es lo que el motor espera en todo borde (REQ-061 D5). `path::c_str()` es
/// `wchar_t*` en Windows, asi que no sirve para un `const char*` de API. (`path::string()` en libstdc++
/// tambien da UTF-8, medido con U+3042; es una propiedad del toolchain, por eso este helper lo explicita.)
inline std::string pathUtf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

/// Una ruta de fixture con `c_str()`/`string()` en UTF-8, para que el codigo de test que la pasa a la API del
/// motor no cambie de forma: en Windows `path::c_str()` es `wchar_t*`, y un `string()` que fuera la pagina ANSI
/// (MSVC) romperia el borde UTF-8. Convierte implicitamente a `std::filesystem::path` para `remove`, `exists`, contenedores de paths.
class Utf8Path {
public:
    Utf8Path(std::filesystem::path p) : mPath(std::move(p)), mUtf8(pathUtf8(mPath)) {}
    const char* c_str() const { return mUtf8.c_str(); }
    const std::string& string() const { return mUtf8; }
    const std::filesystem::path& fs() const { return mPath; }
    operator const std::filesystem::path&() const { return mPath; }

private:
    std::filesystem::path mPath;
    std::string mUtf8;
};

}  // namespace wma_test
