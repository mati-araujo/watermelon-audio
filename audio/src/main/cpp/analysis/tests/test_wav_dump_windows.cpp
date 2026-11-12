/**
 * test_wav_dump_windows.cpp — AC-061.4 (REQ-061 S2): el dump de WAV da en Windows las garantias de POSIX.
 *
 * Solo se compila en Windows (analysis/tests/CMakeLists.txt). Las garantias son las del hallazgo 1 de
 * REQ-055: el `.part` y el destino no se siguen si son un enlace (*reparse point*), y un destino
 * existente no se pisa nunca.
 *
 * La *junction* se crea en el test con DeviceIoControl(FSCTL_SET_REPARSE_POINT): no pide privilegio ni
 * shell. Apunta a una ruta que NO EXISTE dentro de un directorio hermano, asi que un chequeo de
 * existencia que atraviese el enlace diria "libre".
 *
 * Medido (Windows 11, NTFS): CreateFileW sobre una junction da ERROR_ACCESS_DENIED con CREATE_NEW,
 * CREATE_ALWAYS y OPEN_ALWAYS, con o sin FILE_FLAG_OPEN_REPARSE_POINT (la junction es un directorio:
 * falla antes de atravesarla). Por eso el test de la junction NO distingue ese flag del CREATE_NEW del
 * `.part`. Lo distingue AForeignReparseFileAtThePartIsOpenedAsItself: un ARCHIVO con un reparse point
 * que nadie atiende (sin privilegio) da ERROR_FILE_EXISTS con el flag y ERROR_CANT_ACCESS_FILE sin el.
 * (Con un tag sin el bit de Microsoft, NTFS rechaza el FSCTL con ERROR_INVALID_REPARSE_DATA.)
 */

#include "../TunerInputDump.h"
#include "../WavFileDumpSink.h"
#include "../../platform/DumpFile.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

namespace {

using namespace wma::analysis;
namespace fs = std::filesystem;

constexpr char kForeign[] = "contenido ajeno que nadie puede pisar";

/// REPARSE_DATA_BUFFER de un mount point (junction); ntifs.h no esta en MinGW.
struct MountPointBuffer {
    ULONG reparseTag;
    USHORT reparseDataLength;
    USHORT reserved;
    USHORT substituteNameOffset;
    USHORT substituteNameLength;
    USHORT printNameOffset;
    USHORT printNameLength;
    WCHAR pathBuffer[MAX_PATH * 2];
};

/// Convierte `link` (un directorio VACIO que se crea aca) en una junction hacia `target`.
bool makeJunction(const fs::path& link, const fs::path& target) {
    std::error_code ec;
    if (!fs::create_directory(link, ec) || ec) return false;
    const HANDLE h = ::CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    const std::wstring print = fs::absolute(target).wstring();
    const std::wstring substitute = L"\\??\\" + print;
    MountPointBuffer buf{};
    buf.reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    buf.substituteNameOffset = 0;
    buf.substituteNameLength = static_cast<USHORT>(substitute.size() * sizeof(WCHAR));
    buf.printNameOffset = static_cast<USHORT>(buf.substituteNameLength + sizeof(WCHAR));
    buf.printNameLength = static_cast<USHORT>(print.size() * sizeof(WCHAR));
    std::memcpy(buf.pathBuffer, substitute.c_str(), buf.substituteNameLength);
    std::memcpy(reinterpret_cast<char*>(buf.pathBuffer) + buf.printNameOffset, print.c_str(),
                buf.printNameLength);
    buf.reparseDataLength =
        static_cast<USHORT>(8 + buf.printNameOffset + buf.printNameLength + sizeof(WCHAR));
    const DWORD total = static_cast<DWORD>(8 + buf.reparseDataLength);

    DWORD returned = 0;
    const BOOL ok = ::DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &buf, total, nullptr, 0, &returned,
                                      nullptr);
    ::CloseHandle(h);
    return ok != 0;
}

/// Convierte el archivo `file` (que se crea aca, vacio) en uno con un reparse point que NADIE atiende:
/// tag 0x8000F123 con payload GUID. Es el caso "reparse point de un tercero" sin privilegio (a diferencia
/// de un symlink). Un tag SIN el bit de Microsoft lo rechaza NTFS (ERROR_INVALID_REPARSE_DATA), medido.
bool makeUnhandledReparseFile(const fs::path& file) {
    {
        const HANDLE created = ::CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (created == INVALID_HANDLE_VALUE) return false;
        ::CloseHandle(created);
    }
    const HANDLE h = ::CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    struct GuidBuffer {
        ULONG reparseTag;
        USHORT reparseDataLength;
        USHORT reserved;
        GUID guid;
        BYTE data[4];
    } buf{};
    buf.reparseTag = 0x8000F123;
    buf.reparseDataLength = sizeof(GUID) + sizeof(buf.data);
    buf.guid = {0xA1B2C3D4, 0x1111, 0x2222, {9, 8, 7, 6, 5, 4, 3, 2}};
    DWORD returned = 0;
    const BOOL ok = ::DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &buf, sizeof(buf), nullptr, 0, &returned,
                                      nullptr);
    ::CloseHandle(h);
    return ok != 0;
}

bool isReparsePoint(const fs::path& p) {
    const DWORD a = ::GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

std::vector<std::string> namesIn(const fs::path& dir) {
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(dir)) names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeText(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

class WavDumpWindowsTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() /
               ("wma-dumpwin-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(tick));
        work = root / "work";
        sibling = root / "hermano";
        std::error_code ec;
        fs::create_directories(work, ec);
        ASSERT_FALSE(ec);
        fs::create_directories(sibling, ec);
        ASSERT_FALSE(ec);
    }
    void TearDown() override {
        // remove_all no sabe borrar una junction (EINVAL): se sacan primero, como directorios vacios,
        // sin atravesarlas.
        std::error_code ec;
        for (const fs::path& dir : {work, sibling}) {
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (isReparsePoint(entry.path())) fs::remove(entry.path(), ec);
            }
        }
        fs::remove_all(root, ec);
    }

    static std::vector<uint8_t> headerFor(uint64_t frames) {
        InputDumpStats s;
        s.framesWritten = frames;
        s.sampleRate = 44100;
        s.inputSource = 1;
        s.valid = true;
        std::vector<uint8_t> h(kInputDumpHeaderBytes, 0);
        writeInputDumpHeader(h.data(), s);
        return h;
    }

    fs::path root, work, sibling;
};

}  // namespace

/**
 * AC-061.4 (a) — UNA JUNCTION EN EL LUGAR DEL `.part` NO SE SIGUE: el dump falla, el destino de la
 * junction (inexistente) no aparece, la junction queda intacta y no queda ningun temporal.
 *
 * Bug que atrapa: cualquier camino que atraviese la junction (resolver el path con canonical antes de
 * crear) y CREE `hermano\fantasma`. NO atrapa quitar FILE_FLAG_OPEN_REPARSE_POINT del CREATE_NEW
 * (medido: ACCESS_DENIED con y sin el flag): ese lo cubre AForeignReparseFileAtThePartIsOpenedAsItself.
 */
TEST_F(WavDumpWindowsTest, AC_061_4_AJunctionAtThePartIsNeverFollowed) {
    const fs::path ghost = sibling / "fantasma";
    const fs::path path = work / "toma.wav";
    ASSERT_TRUE(makeJunction(fs::path(path.string() + ".part"), ghost)) << "premisa: sin junction";

    WavFileDumpSink sink;
    EXPECT_FALSE(sink.openPart(path.string(), kInputDumpHeaderBytes)) << "siguio la junction";

    EXPECT_FALSE(fs::exists(ghost)) << "el open creo el archivo al que apunta la junction";
    EXPECT_TRUE(namesIn(sibling).empty());
    EXPECT_TRUE(isReparsePoint(fs::path(path.string() + ".part"))) << "la junction ya no es un enlace";
    EXPECT_EQ(namesIn(work), std::vector<std::string>{"toma.wav.part"}) << "quedo un temporal";
    EXPECT_TRUE(sink.finishPart(headerFor(0).data(), kInputDumpHeaderBytes, true).empty());
    EXPECT_FALSE(fs::exists(ghost));
}

/**
 * AC-061.4 (a) — UNA JUNCTION EN EL DESTINO NO SE SIGUE NI SE REEMPLAZA: ni al abrir (el nombre ya
 * existe) ni cuando aparece durante la toma (el renombre no la atraviesa).
 *
 * Bug que atrapa: un renombre que sigue el enlace del destino y deja el WAV en `hermano\fantasma`, o
 * un chequeo de existencia que sigue el enlace (un enlace colgante "no existe").
 */
TEST_F(WavDumpWindowsTest, AC_061_4_AJunctionAtTheDestinationIsNeverFollowedOrReplaced) {
    // Ya existe al abrir: el chequeo de existencia no sigue el enlace (el destino esta colgante).
    const fs::path ghost1 = sibling / "fantasma1";
    const fs::path path1 = work / "uno.wav";
    ASSERT_TRUE(makeJunction(path1, ghost1));
    WavFileDumpSink early;
    EXPECT_FALSE(early.openPart(path1.string(), kInputDumpHeaderBytes));
    EXPECT_FALSE(fs::exists(ghost1));
    EXPECT_FALSE(fs::exists(fs::path(path1.string() + ".part"))) << "creo un .part para un nombre ocupado";

    // Aparece durante la toma.
    const fs::path ghost2 = sibling / "fantasma2";
    const fs::path path2 = work / "dos.wav";
    WavFileDumpSink late;
    ASSERT_TRUE(late.openPart(path2.string(), kInputDumpHeaderBytes));
    const std::vector<float> samples = {0.5f, -0.5f};
    ASSERT_TRUE(late.appendSamples(samples.data(), samples.size()));
    ASSERT_TRUE(makeJunction(path2, ghost2));

    const std::vector<uint8_t> header = headerFor(samples.size());
    EXPECT_TRUE(late.finishPart(header.data(), header.size(), true).empty()) << "renombro sobre la junction";
    EXPECT_FALSE(fs::exists(ghost2));
    EXPECT_TRUE(isReparsePoint(path2));
    EXPECT_TRUE(namesIn(sibling).empty());

    // El `.part` de esta toma se borra (es nuestro); la junction ajena queda.
    EXPECT_TRUE(late.abortPart());
    EXPECT_EQ(namesIn(work), (std::vector<std::string>{"dos.wav", "uno.wav"}));
}

/**
 * AC-061.4 (b) — NO PISAR: un destino que aparece durante la toma con contenido X queda con X; el
 * dump devuelve "" y, abortado, no deja temporales.
 *
 * Bug que atrapa: MoveFileExW con MOVEFILE_REPLACE_EXISTING: la toma pisa el archivo del usuario.
 * (MOVEFILE_COPY_ALLOWED no es observable aca: sin REPLACE_EXISTING y en el mismo volumen no cambia
 * nada; solo degradaria a copiar + borrar entre volumenes.)
 */
TEST_F(WavDumpWindowsTest, AC_061_4_AnExistingDestinationIsNeverReplaced) {
    for (const bool valid : {true, false}) {
        SCOPED_TRACE(valid ? "toma sana" : "toma invalida");
        const fs::path path = work / (valid ? "sana.wav" : "rota.wav");
        const std::string finalName = valid ? path.string() : invalidDumpPathFor(path.string());

        WavFileDumpSink sink;
        ASSERT_TRUE(sink.openPart(path.string(), kInputDumpHeaderBytes));
        const std::vector<float> samples = {0.5f, -0.5f, 0.25f};
        ASSERT_TRUE(sink.appendSamples(samples.data(), samples.size()));
        writeText(finalName, kForeign);

        const std::vector<uint8_t> header = headerFor(samples.size());
        EXPECT_TRUE(sink.finishPart(header.data(), header.size(), valid).empty())
            << "devolvio un nombre que no es el de la toma";
        EXPECT_TRUE(readText(finalName) == kForeign) << "el cierre piso el archivo que ya estaba";

        EXPECT_TRUE(sink.abortPart()) << "el .part era nuestro y se tiene que borrar";
        EXPECT_FALSE(fs::exists(fs::path(path.string() + ".part"))) << "quedo un temporal";
    }
    EXPECT_EQ(namesIn(work), (std::vector<std::string>{"rota.invalid.wav", "sana.wav"}));
}

/**
 * AC-061.4 — EL `.part` SE CREA EN EXCLUSIVA (CREATE_NEW, share 0): un `.part` ajeno no se trunca y,
 * mientras la toma corre, nadie mas puede abrirlo para escribir.
 *
 * Bug que atrapa: CREATE_ALWAYS / OPEN_ALWAYS (trunca lo ajeno) o un share mode permisivo.
 */
TEST_F(WavDumpWindowsTest, AC_061_4_ThePartIsCreatedExclusively) {
    const fs::path path = work / "toma.wav";
    writeText(fs::path(path.string() + ".part"), kForeign);
    WavFileDumpSink refused;
    EXPECT_FALSE(refused.openPart(path.string(), kInputDumpHeaderBytes));
    EXPECT_TRUE(readText(fs::path(path.string() + ".part")) == kForeign) << "trunco un .part ajeno";
    fs::remove(fs::path(path.string() + ".part"));

    WavFileDumpSink sink;
    ASSERT_TRUE(sink.openPart(path.string(), kInputDumpHeaderBytes));
    const HANDLE other = ::CreateFileW(fs::path(path.string() + ".part").c_str(), GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD openError = ::GetLastError();
    EXPECT_EQ(other, INVALID_HANDLE_VALUE) << "otro proceso pudo abrir el .part de la toma viva";
    if (other != INVALID_HANDLE_VALUE) ::CloseHandle(other);
    EXPECT_EQ(openError, static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    EXPECT_TRUE(sink.abortPart());
    EXPECT_TRUE(namesIn(work).empty());
}

/**
 * AC-061.4 (c) — UN SYMLINK DE ARCHIVO EN EL LUGAR DEL `.part` NO SE SIGUE, apunte a un archivo o a
 * nada. Crear un symlink pide modo desarrollador o SeCreateSymbolicLinkPrivilege: sin eso SKIPPED.
 *
 * Bug que atrapa: el mismo que la junction, por la otra via de enlace.
 */
TEST_F(WavDumpWindowsTest, AC_061_4_ASymlinkAtThePartIsNeverFollowed) {
    const fs::path victim = sibling / "victima.bin";
    writeText(victim, kForeign);
    const fs::path ghost = sibling / "fantasma.bin";
    const fs::path path = work / "toma.wav";
    const fs::path part(path.string() + ".part");

    if (!::CreateSymbolicLinkW(part.c_str(), victim.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_PRIVILEGE_NOT_HELD) {
            GTEST_SKIP() << "ERROR_PRIVILEGE_NOT_HELD: crear symlinks pide modo desarrollador o "
                            "SeCreateSymbolicLinkPrivilege";
        }
        FAIL() << "premisa: no pude crear el symlink (error " << err << ")";
    }

    WavFileDumpSink sink;
    EXPECT_FALSE(sink.openPart(path.string(), kInputDumpHeaderBytes)) << "siguio el symlink del .part";
    EXPECT_TRUE(readText(victim) == kForeign) << "el archivo apuntado por el symlink cambio";
    EXPECT_TRUE(isReparsePoint(part));

    fs::remove(part);
    ASSERT_TRUE(::CreateSymbolicLinkW(part.c_str(), ghost.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE));
    WavFileDumpSink sink2;
    EXPECT_FALSE(sink2.openPart(path.string(), kInputDumpHeaderBytes)) << "siguio un symlink colgante";
    EXPECT_FALSE(fs::exists(ghost)) << "el open creo el archivo al que apuntaba el symlink";
}

/**
 * AC-061.4 — UN REPARSE POINT AJENO EN EL LUGAR DEL `.part` SE ABRE COMO LO QUE ES: la primitiva falla
 * CERRADA con "existe" (ERROR_FILE_EXISTS), sin atravesar nada ni tocar el archivo.
 *
 * Es el unico test que distingue FILE_FLAG_OPEN_REPARSE_POINT en el CREATE_NEW del `.part`: sin el flag,
 * Windows intenta atender el reparse point y da ERROR_CANT_ACCESS_FILE (medido: 1920 sin el flag, 80 con
 * el). Se mira el error de la primitiva y no el del sink porque el sink solo dice false.
 *
 * Bug que atrapa: quitar el flag (el nombre ocupado se reportaria como "inaccesible", y con un reparse
 * point que SI se atiende, la creacion iria a donde apunta).
 */
TEST_F(WavDumpWindowsTest, AC_061_4_AForeignReparseFileAtThePartIsOpenedAsItself) {
    const fs::path part = work / "toma.wav.part";
    ASSERT_TRUE(makeUnhandledReparseFile(part)) << "premisa: sin reparse point ajeno (error "
                                                << ::GetLastError() << ")";

    wma::platform::PartIdentity identity;
    ::SetLastError(0);
    std::FILE* file = wma::platform::dumpCreatePartExclusive(part.string(), identity);
    const DWORD error = ::GetLastError();
    if (file != nullptr) std::fclose(file);
    EXPECT_EQ(file, nullptr) << "creo el .part encima de un reparse point";
    EXPECT_EQ(error, static_cast<DWORD>(ERROR_FILE_EXISTS)) << "no lo trato como un nombre ocupado";
    EXPECT_TRUE(isReparsePoint(part)) << "el reparse point ajeno cambio";
    EXPECT_EQ(namesIn(work), std::vector<std::string>{"toma.wav.part"});
}

/**
 * AC-061.4 — `abortPart` BORRA SOLO LO QUE CREO: si el `.part` de la toma ya no es el archivo que el
 * sink creo y hay otro con el mismo nombre, el aborto no lo toca y dice false.
 *
 * La premisa se arma sin privilegio: `finishPart` contra un destino ocupado falla, CIERRA el archivo y
 * deja el nombre en el sink; entonces el `.part` ya no esta abierto y se puede reemplazar por uno ajeno.
 *
 * Bug que atrapa: borrar a ciegas por nombre, o `ours` sin comparar volumen + indice del archivo.
 */
TEST_F(WavDumpWindowsTest, AC_061_4_AbortNeverRemovesAPartItDidNotCreate) {
    const fs::path path = work / "toma.wav";
    const fs::path part(path.string() + ".part");

    WavFileDumpSink sink;
    ASSERT_TRUE(sink.openPart(path.string(), kInputDumpHeaderBytes));
    const std::vector<float> samples = {0.5f, -0.5f};
    ASSERT_TRUE(sink.appendSamples(samples.data(), samples.size()));

    writeText(path, "destino ocupado");
    const std::vector<uint8_t> header = headerFor(samples.size());
    ASSERT_TRUE(sink.finishPart(header.data(), header.size(), true).empty()) << "premisa: el destino ocupado";
    ASSERT_TRUE(fs::remove(part)) << "premisa: el .part cerrado de la toma";
    writeText(part, kForeign);

    EXPECT_FALSE(sink.abortPart()) << "el nombre lo ocupa un archivo ajeno: no se borra, y se dice";
    ASSERT_TRUE(fs::exists(part)) << "se borro un archivo que el sink no creo";
    EXPECT_TRUE(readText(part) == kForeign);
    EXPECT_TRUE(readText(path) == "destino ocupado");
    EXPECT_EQ(namesIn(work), (std::vector<std::string>{"toma.wav", "toma.wav.part"}));
}
