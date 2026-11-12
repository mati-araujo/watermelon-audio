/**
 * test_mapped_file.cpp — AC-061.2 (REQ-061 S2): `SoundFontManager` mapea por `platform/MappedFile`.
 *
 * Corre en las dos ramas de `MappedFile` (mmap en POSIX, CreateFileMappingW en Windows). Lo que
 * cambia entre ellas es el camino por fd: en Windows la JVM no entrega fds de CRT, asi que el
 * pedido se rechaza con un log que nombra la razon (tests sólo-Windows al final).
 */

#include "support/MinimalSoundFont.h"

#include "engines/SoundFontManager.h"
#include "platform/Logger.h"
#include "platform/MappedFile.h"
#include "tests/support/WindowsNotRun.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#define WMA_TEST_GETPID _getpid
#else
#include <fcntl.h>
#include <unistd.h>
#define WMA_TEST_GETPID getpid
#endif

namespace {

namespace fs = std::filesystem;
using wma::platform::MappedFile;

std::string gLog;

void captureLog(wma::LogLevel, const char*, const char* msg) {
    gLog += msg;
    gLog += '\n';
}

/// Bytes que delatan un offset o un largo mal calculados: no hay dos paginas iguales.
std::vector<uint8_t> patterned(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i * 31u + (i >> 8) * 7u + 5u) & 0xFFu);
    return v;
}

class MappedFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        gLog.clear();
        wma::setLogCallback(captureLog);
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        dir = fs::temp_directory_path() /
              ("wma-mapped-" + std::to_string(static_cast<long long>(WMA_TEST_GETPID())) + "-" +
               std::to_string(tick));
        std::error_code ec;
        fs::create_directories(dir, ec);
        ASSERT_FALSE(ec) << "no pude crear " << dir;
    }
    void TearDown() override {
        wma::setLogCallback(nullptr);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::string write(const char* name, const std::vector<uint8_t>& bytes) const {
        const fs::path p = dir / name;
        std::ofstream out(p, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
        return p.string();
    }

    fs::path dir;
};

}  // namespace

/**
 * AC-061.2 — el mapeo expone el contenido y el tamano EXACTOS del archivo.
 *
 * Bug que atrapa: un mapeo con el largo redondeado a pagina (size de mas), un puntero corrido o una
 * vista que no es del archivo. El tamano no es multiplo de pagina a proposito.
 */
TEST_F(MappedFileTest, AC_061_2_MapsTheExactContentAndSizeOfTheFile) {
    const std::vector<uint8_t> bytes = patterned(3 * 4096 + 17);
    const std::string path = write("datos.bin", bytes);

    MappedFile mapped = MappedFile::mapPath(path.c_str());

    ASSERT_TRUE(mapped.isMapped());
    ASSERT_EQ(mapped.byteCount(), bytes.size());
    EXPECT_EQ(std::memcmp(mapped.bytes(), bytes.data(), bytes.size()), 0);
}

/**
 * AC-061.2 — un archivo que no existe da un mapeo invalido, sin datos ni crash.
 *
 * Bug que atrapa: tratar el handle de error (-1 / INVALID_HANDLE_VALUE) como valido.
 */
TEST_F(MappedFileTest, AC_061_2_AMissingFileIsInvalid) {
    const std::string path = (dir / "no-existe.bin").string();

    MappedFile mapped = MappedFile::mapPath(path.c_str());

    EXPECT_FALSE(mapped.isMapped());
    EXPECT_EQ(mapped.bytes(), nullptr);
    EXPECT_EQ(mapped.byteCount(), 0u);
}

/**
 * AC-061.2 — un archivo vacio es invalido en las dos ramas (mmap de 0 bytes da EINVAL; Windows
 * rechaza una seccion de largo 0): nunca un mapeo valido de largo 0.
 *
 * Bug que atrapa: un mapeo "valido" de 0 bytes que el parser de SF2 desreferencia.
 */
TEST_F(MappedFileTest, AC_061_2_AnEmptyFileIsInvalid) {
    const std::string path = write("vacio.bin", {});
    ASSERT_TRUE(fs::exists(path));

    MappedFile mapped = MappedFile::mapPath(path.c_str());

    EXPECT_FALSE(mapped.isMapped());
    EXPECT_EQ(mapped.byteCount(), 0u);
}

/**
 * AC-061.2 — una ruta nula o vacia o un directorio no mapean.
 *
 * Bug que atrapa: `open()` abre un directorio en POSIX; sin el chequeo del mapeo, un directorio
 * pasaria por archivo. Una ruta nula no puede ser un crash en el borde JNI.
 */
TEST_F(MappedFileTest, AC_061_2_ANullPathAnEmptyPathAndADirectoryAreInvalid) {
    EXPECT_FALSE(MappedFile::mapPath(nullptr).isMapped());
    EXPECT_FALSE(MappedFile::mapPath("").isMapped());
    const std::string d = dir.string();
    EXPECT_FALSE(MappedFile::mapPath(d.c_str()).isMapped());
}

/**
 * AC-061.2 — `release()` y el mover sueltan el mapeo una sola vez.
 *
 * Bug que atrapa: un move que deja al origen apuntando a la vista (doble unmap) o un release que no
 * invalida.
 */
TEST_F(MappedFileTest, AC_061_2_ReleaseAndMoveTransferOwnership) {
    const std::vector<uint8_t> bytes = patterned(5000);
    const std::string path = write("datos.bin", bytes);

    MappedFile a = MappedFile::mapPath(path.c_str());
    ASSERT_TRUE(a.isMapped());
    MappedFile b = std::move(a);
    EXPECT_FALSE(a.isMapped());  // NOLINT(bugprone-use-after-move): el contrato es justo ese
    ASSERT_TRUE(b.isMapped());
    EXPECT_EQ(std::memcmp(b.bytes(), bytes.data(), bytes.size()), 0);

    b.release();
    EXPECT_FALSE(b.isMapped());
    EXPECT_EQ(b.bytes(), nullptr);
    b.release();  // idempotente
}

/**
 * AC-061.2 — `SoundFontManager::loadFromPath` carga un .sf2 real PASANDO por el mapeo, y el font
 * queda activo con la tasa pedida.
 *
 * Bug que atrapa: el manager que parsea un buffer ya liberado (el mapeo se suelta tras parsear) o que
 * pasa el tamano equivocado a tsf.
 */
TEST_F(MappedFileTest, AC_061_2_SoundFontManagerLoadsAnSf2ThroughTheMapping) {
    const std::vector<uint8_t> sf2 = wma_test::makeMinimalSoundFont(22050);
    const std::string path = write("fuente.sf2", sf2);

    SoundFontManager manager;
    ASSERT_TRUE(manager.loadFromPath(path.c_str(), 44100));

    EXPECT_TRUE(manager.isLoaded());
    EXPECT_EQ(manager.getPresetCount(), 1);
    EXPECT_EQ(manager.getSampleRate(), 44100);
}

/**
 * AC-061.2 — lo que no se puede mapear o no es un SoundFont devuelve false y NO deja nada cargado
 * encima de lo que ya habia.
 *
 * Bug que atrapa: un fallo de mapeo que descarta el font activo, o que devuelve true.
 */
TEST_F(MappedFileTest, AC_061_2_AFailedLoadReturnsFalseAndKeepsThePreviousFont) {
    const std::vector<uint8_t> sf2 = wma_test::makeMinimalSoundFont(22050);
    SoundFontManager manager;
    ASSERT_TRUE(manager.loadFromPath(write("bueno.sf2", sf2).c_str(), 48000));

    EXPECT_FALSE(manager.loadFromPath((dir / "nada.sf2").string().c_str(), 48000));
    EXPECT_FALSE(manager.loadFromPath(write("vacio.sf2", {}).c_str(), 48000));
    EXPECT_FALSE(manager.loadFromPath(write("basura.sf2", patterned(2048)).c_str(), 48000));

    EXPECT_TRUE(manager.isLoaded());
    EXPECT_EQ(manager.getPresetCount(), 1);
}

/**
 * AC-061.2 (rama POSIX) — `mapFdRegion` mapea una sub-region con offset NO alineado a pagina y expone
 * exactamente esos bytes.
 *
 * Bug que atrapa: perder el `dataDelta` (los datos arrancan en el byte equivocado) o mapear de mas.
 */
TEST_F(MappedFileTest, AC_061_2_PosixMapsAnUnalignedSubRegionOfAnFd) {
    WMA_WINDOWS_NOT_RUN("mapFdRegion (fd + mmap de una sub-region) no existe en Windows: ahi lo cubre AC_061_2_WindowsRefusesTheFdPath");
#if !defined(_WIN32)
    const std::vector<uint8_t> bytes = patterned(4 * 4096 + 321);
    const std::string path = write("region.bin", bytes);
    const int fd = ::open(path.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);

    const int64_t offset = 4096 + 1234;
    const int64_t length = 5000;
    MappedFile mapped = MappedFile::mapFdRegion(fd, offset, length);
    ::close(fd);  // el mapeo no retiene el fd

    ASSERT_TRUE(mapped.isMapped());
    ASSERT_EQ(mapped.byteCount(), static_cast<size_t>(length));
    EXPECT_EQ(std::memcmp(mapped.bytes(), bytes.data() + offset, static_cast<size_t>(length)), 0);
#endif  // !_WIN32
}

/**
 * AC-061.2 (rama POSIX) — una region fuera del archivo o un fd invalido no mapean.
 *
 * Bug que atrapa: leer mas alla del archivo (SIGBUS) por no validar offset+length contra su tamano.
 */
TEST_F(MappedFileTest, AC_061_2_PosixRejectsARegionOutsideTheFileAndABadFd) {
    WMA_WINDOWS_NOT_RUN("mapFdRegion (fd + mmap de una sub-region) no existe en Windows: ahi lo cubre AC_061_2_WindowsRefusesTheFdPath");
#if !defined(_WIN32)
    const std::string path = write("region.bin", patterned(8192));
    const int fd = ::open(path.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);

    EXPECT_FALSE(MappedFile::mapFdRegion(fd, 8000, 1000).isMapped());
    EXPECT_FALSE(MappedFile::mapFdRegion(fd, 0, 0).isMapped());
    ::close(fd);
    EXPECT_FALSE(MappedFile::mapFdRegion(-1, 0, 10).isMapped());
#endif  // !_WIN32
}

#if defined(_WIN32)

/**
 * AC-061.2 (rama Windows) — el camino por fd se rechaza SIEMPRE, aunque el fd sea de CRT, valido y
 * la region exista: la JVM no entrega fds de CRT, asi que aceptar uno seria mapear el fd de otro.
 *
 * Bug que atrapa: un `mapFdRegion` que interpreta el int como HANDLE y mapea cualquier cosa, o que
 * devuelve un mapeo valido.
 */
TEST_F(MappedFileTest, AC_061_2_WindowsRefusesTheFdPath) {
    const std::vector<uint8_t> bytes = patterned(8192);
    const std::string path = write("region.bin", bytes);
    const int fd = ::_open(path.c_str(), _O_RDONLY | _O_BINARY);
    ASSERT_GE(fd, 0);

    MappedFile mapped = MappedFile::mapFdRegion(fd, 0, 4096);
    ::_close(fd);

    EXPECT_FALSE(mapped.isMapped());
}

/**
 * AC-061.2 (rama Windows) — `SoundFontManager::loadFromFd` devuelve false y el log NOMBRA la razon,
 * con un fd de CRT valido sobre un .sf2 que si cargaria por path.
 *
 * Bug que atrapa: el fallo mudo (el consumidor ve false y no sabe por que) o, peor, cargar.
 */
TEST_F(MappedFileTest, AC_061_2_WindowsLoadFromFdReturnsFalseAndNamesTheReason) {
    const std::vector<uint8_t> sf2 = wma_test::makeMinimalSoundFont(22050);
    const std::string path = write("fuente.sf2", sf2);
    const int fd = ::_open(path.c_str(), _O_RDONLY | _O_BINARY);
    ASSERT_GE(fd, 0);

    SoundFontManager manager;
    const bool ok = manager.loadFromFd(fd, 0, static_cast<int64_t>(sf2.size()), 44100);
    ::_close(fd);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(manager.isLoaded());
    EXPECT_NE(gLog.find("Windows"), std::string::npos) << "el log no nombra la razon: " << gLog;
    EXPECT_NE(gLog.find("loadFromFd"), std::string::npos) << gLog;
}

#endif  // _WIN32
