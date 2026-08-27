/**
 * FixturePath.h — una ruta de fixture UNICA POR PROCESO (MINI-009).
 *
 * POR QUE EXISTE
 * --------------
 * `looper/tests/` se compila DOS VECES: `looper_tests` y `looper_tests_dense`, la
 * misma suite con y sin `WM_LOOPER_DENSE_BUFFER` (ver ../CMakeLists.txt). Las dos
 * registran los MISMOS nombres de test, y `ctest -j` las corre EN PARALELO. Asi que
 * toda ruta de fixture de este directorio la comparten dos procesos concurrentes.
 *
 * Eso rompia la invariante que `scripts/run-cpp-tests.sh` declara para justificar el
 * `-j`: "cada test que escribe usa un nombre de archivo propio". La convencion
 * existia y estaba escrita; lo que fallo es que el directorio que la violaba se
 * DUPLICO sin que nadie lo notara. Este helper la vuelve cierta por CONSTRUCCION.
 *
 * Sintoma cuando no estaba, medido en master (44a9a4d, job de TSan):
 *
 *     Test #300: LooperStress.ClearVsPlaybackNoUseAfterFree ... Passed    8.29 sec
 *     Test #400: LooperStress.ClearVsPlaybackNoUseAfterFree ... ***Failed 0.05 sec
 *     E/Looper: importTrack FAILED: readWav returned 0 frames (corrupt file)
 *
 * El MISMO nombre de test con dos numeros distintos es la firma de la duplicacion.
 * TSan no reporto ninguna carrera: no es de memoria, es de archivo.
 */

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <process.h>
#define WMA_TEST_GETPID _getpid
#else
#include <unistd.h>
#define WMA_TEST_GETPID getpid
#endif

namespace wma_test {

/**
 * `<tmp>/wm<pid>_<name>` — el pid la hace unica entre los binarios que comparten
 * este directorio, sin que ningun test tenga que acordarse de elegir un nombre
 * distinto del de su gemelo.
 *
 * ABSOLUTA a proposito: una ruta relativa depende del CWD, y los dos binarios
 * corren con el mismo (era el caso de `test_wav_file.cpp`).
 */
inline std::filesystem::path fixturePath(std::string_view name) {
    return std::filesystem::temp_directory_path() /
           ("wm" + std::to_string(static_cast<long long>(WMA_TEST_GETPID())) + "_" +
            std::string(name));
}

/** Borra al salir del alcance: /tmp no se limpia solo entre corridas. */
class ScopedFixture {
public:
    explicit ScopedFixture(std::string_view name) : mPath(fixturePath(name)) {
        std::error_code ec;
        std::filesystem::remove(mPath, ec);
    }
    ~ScopedFixture() {
        std::error_code ec;
        std::filesystem::remove(mPath, ec);
    }
    ScopedFixture(const ScopedFixture&) = delete;
    ScopedFixture& operator=(const ScopedFixture&) = delete;

    const std::filesystem::path& path() const { return mPath; }
    std::string str() const { return mPath.string(); }
    const char* c_str() const { return mPath.c_str(); }

private:
    std::filesystem::path mPath;
};

}  // namespace wma_test
