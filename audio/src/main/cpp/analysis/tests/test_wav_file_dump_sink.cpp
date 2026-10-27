/**
 * REQ-055 S0 · tarea 0.1 — WavFileDumpSink: el borde de DISCO del volcado del afinador.
 *
 * AC-055.1 — el corpus se captura por el camino del afinador y este sink es lo que lo deja en un
 * archivo. El nucleo (`test_input_dump.cpp`) decide si la toma es valida con un sink en memoria;
 * lo que se prueba ACA es lo unico que la memoria no puede probar: que el archivo que queda en
 * disco es el que el nucleo dijo.
 *
 *   - El nombre MIENTE si el rename falla o se hace al reves: una toma valida tiene que quedar en
 *     `path` (y no en `path.part`), una invalida en `invalidDumpPathFor(path)` y NUNCA en `path`.
 *     Un WAV con hueco con nombre de sano es exactamente lo que este REQ no puede producir.
 *   - El header se reescribe en el offset 0 DESPUES de los datos: si los datos pisaran el espacio
 *     reservado, o el header se escribiera al final, el archivo no se abre.
 *   - Las muestras llegan BIT A BIT: son floats crudos, no texto ni cuantizados.
 *
 *   - HALLAZGO 1 (security-auditor de REQ-055): el diagnostico no destruye archivos. Ni el `.part`
 *     ni el nombre final pisan, truncan o siguen (symlink) algo que ya estaba en el disco.
 *
 * Corre contra un directorio temporal propio, que se borra al terminar.
 */

#include "../TunerInputDump.h"
#include "../WavFileDumpSink.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace wma::analysis;
namespace fs = std::filesystem;

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Muestras que delatan un volcado que no es crudo: NaN con payload, -0, denormal, infinito,
/// valores fuera de [-1, 1], mas una tanda larga que cruza el buffer de stdio.
std::vector<float> trickyFloats() {
    std::vector<float> v;
    uint32_t nanBits = 0x7FC12345u;
    float nan;
    std::memcpy(&nan, &nanBits, sizeof nan);
    v.push_back(nan);
    v.push_back(-0.0f);
    v.push_back(1.0e-40f);   // denormal
    v.push_back(std::numeric_limits<float>::infinity());
    v.push_back(-3.5e10f);
    v.push_back(1.5f);
    v.push_back(-1.0000001f);
    return v;
}

class WavSinkTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        dir = fs::temp_directory_path()
              / ("wma-wavsink-" + std::to_string(tick) + "-"
                 + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::create_directories(dir, ec);
        ASSERT_FALSE(ec) << "no pude crear el directorio temporal " << dir;
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::string pathOf(const std::string& name) const { return (dir / name).string(); }
    static bool exists(const std::string& p) { return fs::exists(fs::path(p)); }

    /// Header de una toma con `frames` frames, armado por el nucleo.
    static std::vector<uint8_t> headerFor(uint64_t frames, bool valid) {
        InputDumpStats s;
        s.framesWritten = frames;
        s.sampleRate = 44100;
        s.inputSource = 1;
        s.valid = valid;
        std::vector<uint8_t> h(kInputDumpHeaderBytes, 0);
        writeInputDumpHeader(h.data(), s);
        return h;
    }

    fs::path dir;
};

}  // namespace

/**
 * AC-055.1 — UNA TOMA VALIDA queda en `path`, sin `.part`, con el header al principio y los
 * floats detras, bit a bit.
 *
 * Bugs que atrapa y mutantes que mata:
 *   - el rename no ocurre / el archivo queda en `path.part`          -> exists(path) falla
 *   - el header se escribe al final o los datos pisan su reserva     -> el header releido no coincide
 *   - openPart no reserva headerBytes                                -> los floats caen en el offset 0
 *   - fwrite con el conteo en bytes en vez de floats (trunca/corrompe)-> memcmp de la tanda larga
 *   - NaN/-0/denormal normalizados al escribir                        -> memcmp
 */
TEST_F(WavSinkTest, AValidTakeEndsNamedPathWithTheHeaderUpFrontAndTheSamplesBitExact) {
    const std::string path = pathOf("toma.wav");
    WavFileDumpSink sink;
    ASSERT_TRUE(sink.openPart(path, kInputDumpHeaderBytes));

    // Mientras la toma esta abierta existe SOLO el temporal. (No se lee su contenido: stdio puede
    // tener la reserva sin vaciar, y eso no es un defecto. Que la reserva exista y este bien
    // puesta lo prueba el archivo final: sin ella, los datos arrancarian en el offset 0 y el
    // header los pisaria.)
    EXPECT_TRUE(exists(path + ".part")) << "el destino temporal es <path>.part";
    EXPECT_FALSE(exists(path)) << "el nombre final no puede existir antes de cerrar";

    std::vector<float> all = trickyFloats();
    ASSERT_TRUE(sink.appendSamples(all.data(), all.size()));

    std::vector<float> one = {0.25f};
    ASSERT_TRUE(sink.appendSamples(one.data(), one.size()));
    all.insert(all.end(), one.begin(), one.end());

    std::vector<float> longRun(70000);   // cruza el buffer de stdio varias veces
    for (size_t i = 0; i < longRun.size(); ++i) longRun[i] = static_cast<float>(i) * 0.37f - 5000.0f;
    ASSERT_TRUE(sink.appendSamples(longRun.data(), longRun.size()));
    all.insert(all.end(), longRun.begin(), longRun.end());

    const std::vector<uint8_t> header = headerFor(all.size(), true);
    const std::string finalPath = sink.finishPart(header.data(), header.size(), true);

    EXPECT_EQ(finalPath, path);
    EXPECT_TRUE(exists(path));
    EXPECT_FALSE(exists(path + ".part")) << "quedo el temporal: el rename no se hizo";
    EXPECT_FALSE(exists(invalidDumpPathFor(path))) << "una toma valida no deja un .invalid";

    const std::vector<uint8_t> bytes = readFile(path);
    ASSERT_EQ(bytes.size(), kInputDumpHeaderBytes + all.size() * sizeof(float));
    EXPECT_EQ(std::memcmp(bytes.data(), header.data(), kInputDumpHeaderBytes), 0)
        << "el header releido no es el que se entrego al cerrar";
    EXPECT_EQ(std::memcmp(bytes.data() + kInputDumpHeaderBytes, all.data(),
                          all.size() * sizeof(float)),
              0)
        << "los floats releidos no son los que se agregaron";
}

/**
 * AC-055.1 — UNA TOMA INVALIDA queda en `invalidDumpPathFor(path)` y NUNCA en `path`.
 *
 * Es EL requisito: "un WAV con huecos nunca tiene nombre de sano". Se verifica por la ausencia
 * del nombre bueno, no solo por la presencia del malo (un sink que escribiera los dos pasaria el
 * primer chequeo). El contenido es el mismo: el archivo invalido sirve para diagnosticar.
 * Mutantes que mata: ignorar `valid` en el rename; renombrar a `path` y copiar a `.invalid`.
 */
TEST_F(WavSinkTest, AnInvalidTakeIsNamedInvalidAndTheHealthyNameNeverExists) {
    const std::string path = pathOf("x.wav");
    WavFileDumpSink sink;
    ASSERT_TRUE(sink.openPart(path, kInputDumpHeaderBytes));
    const std::vector<float> samples = {0.5f, -0.25f, 2.0f, 0.0f};
    ASSERT_TRUE(sink.appendSamples(samples.data(), samples.size()));

    const std::vector<uint8_t> header = headerFor(samples.size(), false);
    const std::string finalPath = sink.finishPart(header.data(), header.size(), false);

    const std::string invalid = pathOf("x.invalid.wav");
    EXPECT_EQ(finalPath, invalid);
    EXPECT_TRUE(exists(invalid));
    EXPECT_FALSE(exists(path)) << "una toma invalida NO puede quedar con el nombre de una sana";
    EXPECT_FALSE(exists(path + ".part"));

    const std::vector<uint8_t> bytes = readFile(invalid);
    ASSERT_EQ(bytes.size(), kInputDumpHeaderBytes + samples.size() * sizeof(float));
    EXPECT_EQ(std::memcmp(bytes.data(), header.data(), kInputDumpHeaderBytes), 0);
    EXPECT_EQ(std::memcmp(bytes.data() + kInputDumpHeaderBytes, samples.data(),
                          samples.size() * sizeof(float)),
              0);
}

/**
 * AC-055.1 — UN NOMBRE CON NO-ASCII viaja intacto a disco (UTF-8 en el std::string).
 *
 * Bug que atrapa: una conversion de path que solo entiende ASCII (el telefono guarda en
 * "Grabaciones/guitarra-ñ.wav"). Mutante que mata: construir el nombre del `.part` o del
 * `.invalid` con un recorte por caracter.
 */
TEST_F(WavSinkTest, ANonAsciiPathReachesTheDiskIntact) {
    const std::string name = "toma-\xC3\xB1.wav";   // "toma-ñ.wav" en UTF-8
    const std::string path = pathOf(name);
    WavFileDumpSink sink;
    ASSERT_TRUE(sink.openPart(path, kInputDumpHeaderBytes));
    EXPECT_TRUE(exists(path + ".part"));
    const std::vector<float> samples = {1.0f};
    ASSERT_TRUE(sink.appendSamples(samples.data(), samples.size()));
    const std::vector<uint8_t> header = headerFor(1, true);
    EXPECT_EQ(sink.finishPart(header.data(), header.size(), true), path);
    EXPECT_TRUE(exists(path));
}

/**
 * AC-055.1 — openPart que no puede abrir devuelve false (directorio inexistente), sin crear nada.
 *
 * Es lo que hace que `wma_tuner_diag_input_dump_start` devuelva false y el consumidor se entere
 * de que no esta grabando. Mutante que mata: ignorar el retorno de fopen.
 */
TEST_F(WavSinkTest, OpenPartFailsWhenTheDirectoryDoesNotExist) {
    const std::string path = pathOf("no/existe/toma.wav");
    WavFileDumpSink sink;
    EXPECT_FALSE(sink.openPart(path, kInputDumpHeaderBytes));
    EXPECT_FALSE(exists(path + ".part"));
    EXPECT_FALSE(exists(path));
}

/**
 * AC-055.1 — SI EL RENAME FALLA, finishPart devuelve "" (no un path que no existe).
 *
 * Con un directorio ocupando el nombre final, el rename del archivo falla. El nucleo toma ese ""
 * como "no hay archivo sano que ofrecer" (la toma queda invalida). Un sink que devolviera `path`
 * igual le diria al humano que su toma esta ahi cuando no esta.
 * Mutante que mata: devolver el path sin mirar el retorno de std::rename.
 */
TEST_F(WavSinkTest, AFailedRenameReturnsAnEmptyPathInsteadOfAFalseOne) {
    const std::string path = pathOf("ocupado.wav");
    WavFileDumpSink sink;
    ASSERT_TRUE(sink.openPart(path, kInputDumpHeaderBytes));

    // El directorio aparece DURANTE la toma: con el nombre ocupado antes, `openPart` ya rechaza
    // (hallazgo 1, ver `AnExistingFinalNameRejectsTheOpenAndIsLeftUntouched`).
    std::error_code ec;
    fs::create_directories(fs::path(path) / "adentro", ec);   // un directorio NO vacio con ese nombre
    ASSERT_FALSE(ec);
    const std::vector<float> samples = {0.1f, 0.2f};
    ASSERT_TRUE(sink.appendSamples(samples.data(), samples.size()));
    const std::vector<uint8_t> header = headerFor(2, true);
    EXPECT_EQ(sink.finishPart(header.data(), header.size(), true), "");
}

/// finishPart sin haber abierto nada devuelve "" y no revienta (un nucleo con un bug de orden no
/// puede tumbar el proceso por un FILE* nulo).
TEST_F(WavSinkTest, FinishingATakeThatWasNeverOpenedReturnsEmpty) {
    WavFileDumpSink sink;
    const std::vector<uint8_t> header = headerFor(0, false);
    EXPECT_EQ(sink.finishPart(header.data(), header.size(), false), "");
}

// ===========================================================================
// HALLAZGO 1 (security-auditor de REQ-055): el diagnostico no destruye archivos.
// El path lo arma la app; un nombre repetido o un symlink plantado no pueden costar un archivo ajeno.
// ===========================================================================

namespace {

void writeBytes(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string readText(const std::string& path) {
    const std::vector<uint8_t> b = readFile(path);
    return std::string(b.begin(), b.end());
}

/// Un contenido que delata cualquier escritura: ni ceros ni un header de WAV.
const std::string kForeign = "contenido-ajeno: no es un WAV y no se puede tocar\x01\x02";

}  // namespace

/**
 * Hallazgo 1 — UN `.part` QUE YA EXISTE NO SE TRUNCA: `openPart` falla y lo deja byte a byte.
 *
 * Bug que atrapa: `fopen(part, "wb")` trunca lo que hubiera con ese nombre (otra toma viva de otro
 * proceso, o un archivo del usuario). Mutante que mata: abrir sin `O_EXCL` (o con `fopen "wb"`).
 */
TEST_F(WavSinkTest, Hallazgo1_APreexistingPartIsNeverTruncatedAndTheOpenFails) {
    const std::string path = pathOf("toma.wav");
    writeBytes(path + ".part", kForeign);

    WavFileDumpSink sink;
    EXPECT_FALSE(sink.openPart(path, kInputDumpHeaderBytes)) << "abrio sobre un .part ajeno";
    EXPECT_EQ(readText(path + ".part"), kForeign) << "el .part ajeno cambio";
    EXPECT_FALSE(exists(path));
    EXPECT_EQ(sink.finishPart(headerFor(0, false).data(), kInputDumpHeaderBytes, false), "")
        << "un open fallido no deja nada que cerrar";
    EXPECT_EQ(readText(path + ".part"), kForeign) << "el cierre de un open fallido toco el .part";
}

/**
 * Hallazgo 1 — UN SYMLINK EN EL LUGAR DEL `.part` NO SE SIGUE, apunte a un archivo o a nada.
 *
 * Bug que atrapa: un symlink plantado en `<path>.part` que redirige la escritura a otro archivo
 * (lo trunca) o lo crea donde apunta. Mutantes que mata: abrir sin `O_EXCL` y sin `O_NOFOLLOW`.
 */
TEST_F(WavSinkTest, Hallazgo1_ASymlinkAtThePartIsNeverFollowed) {
    const std::string victim = pathOf("victima.bin");
    writeBytes(victim, kForeign);
    const std::string path = pathOf("toma.wav");
    std::error_code ec;
    fs::create_symlink(victim, path + ".part", ec);
    ASSERT_FALSE(ec) << "premisa: no pude crear el symlink";

    WavFileDumpSink sink;
    EXPECT_FALSE(sink.openPart(path, kInputDumpHeaderBytes)) << "siguio el symlink del .part";
    EXPECT_EQ(readText(victim), kForeign) << "el archivo apuntado por el symlink cambio";

    // Colgante: el destino del symlink NO puede aparecer.
    const std::string ghost = pathOf("fantasma.bin");
    const std::string path2 = pathOf("toma2.wav");
    fs::create_symlink(ghost, path2 + ".part", ec);
    ASSERT_FALSE(ec);
    EXPECT_FALSE(sink.openPart(path2, kInputDumpHeaderBytes)) << "siguio un symlink colgante";
    EXPECT_FALSE(exists(ghost)) << "el open creo el archivo al que apuntaba el symlink colgante";
}

/**
 * Hallazgo 1 — SI EL NOMBRE FINAL YA EXISTE, `openPart` RECHAZA de entrada (sano o `.invalid`).
 *
 * El veredicto (sano o invalido) se sabe recien al cerrar, asi que se exige libre cualquiera de los
 * dos: si no, la toma correria minutos para terminar en un `.part` sin nombre. No crea el `.part`.
 * Mutante que mata: no chequear el nombre final al abrir (o solo uno de los dos).
 */
TEST_F(WavSinkTest, Hallazgo1_AnExistingFinalNameRejectsTheOpenAndIsLeftUntouched) {
    for (const std::string occupied : {"toma.wav", "toma.invalid.wav"}) {
        SCOPED_TRACE(occupied);
        const std::string path = pathOf("toma.wav");
        writeBytes(pathOf(occupied), kForeign);

        WavFileDumpSink sink;
        EXPECT_FALSE(sink.openPart(path, kInputDumpHeaderBytes));
        EXPECT_FALSE(exists(path + ".part")) << "un open rechazado dejo un .part";
        EXPECT_EQ(readText(pathOf(occupied)), kForeign);
        fs::remove(pathOf(occupied));
    }
}

/**
 * Hallazgo 1 — UN DESTINO QUE APARECE DURANTE LA TOMA QUEDA INTACTO, byte a byte, y la toma no se
 * nombra: `finishPart` devuelve "" (=> `finalized = 0`) y los datos quedan en el `.part`.
 *
 * Bug que atrapa: `rename(2)` reemplaza el destino en silencio, asi que una toma que cierra sobre
 * un archivo del usuario lo destruye. Vale para los dos nombres (sana e invalida).
 * Mutante que mata: `std::rename` en lugar de `renameWithoutReplacing`.
 */
TEST_F(WavSinkTest, Hallazgo1_AFinalNameThatAppearsDuringTheTakeIsNeverReplaced) {
    for (const bool valid : {true, false}) {
        SCOPED_TRACE(valid ? "toma sana" : "toma invalida");
        const std::string path = pathOf(valid ? "sana.wav" : "rota.wav");
        const std::string finalName = valid ? path : invalidDumpPathFor(path);

        WavFileDumpSink sink;
        ASSERT_TRUE(sink.openPart(path, kInputDumpHeaderBytes));
        const std::vector<float> samples = {0.5f, -0.5f, 0.25f};
        ASSERT_TRUE(sink.appendSamples(samples.data(), samples.size()));
        writeBytes(finalName, kForeign);   // aparece mientras la toma corre

        const std::vector<uint8_t> header = headerFor(samples.size(), valid);
        EXPECT_EQ(sink.finishPart(header.data(), header.size(), valid), "")
            << "devolvio un nombre que no es el de la toma";
        EXPECT_EQ(readText(finalName), kForeign) << "el cierre piso el archivo que ya estaba";

        const std::vector<uint8_t> part = readFile(path + ".part");
        ASSERT_EQ(part.size(), kInputDumpHeaderBytes + samples.size() * sizeof(float))
            << "los datos de la toma no quedaron en el .part";
        EXPECT_EQ(std::memcmp(part.data(), header.data(), kInputDumpHeaderBytes), 0);
    }
}

/**
 * Hallazgo 1 — CADA ESCALON DEL RENOMBRE SIN REEMPLAZO: con el destino ocupado no lo toca y deja
 * el origen; con el destino libre, mueve.
 *
 * En el host solo se alcanza el primer escalon (macOS y el ext4 del CI soportan el nativo): los
 * otros dos son los de un FUSE/sdcardfs de Android, y sin esto nadie los ejecutaria.
 * Mutantes que mata: un escalon que cae a `rename(2)` sin mirar el destino; `link` sin `unlink`
 * del origen (quedan los dos nombres); un escalon que devuelve true sin mover.
 */
TEST_F(WavSinkTest, Hallazgo1_EveryStepOfTheNoReplaceRenameKeepsAnExistingDestination) {
    const std::array<NoReplaceRenameStep, 3> steps = {NoReplaceRenameStep::kNative,
                                                      NoReplaceRenameStep::kLinkUnlink,
                                                      NoReplaceRenameStep::kCheckThenRename};
    int i = 0;
    for (const NoReplaceRenameStep step : steps) {
        SCOPED_TRACE("escalon " + std::to_string(i));
        const std::string from = pathOf("origen-" + std::to_string(i));
        const std::string to = pathOf("destino-" + std::to_string(i));
        const std::string dirTo = pathOf("dir-" + std::to_string(i));
        ++i;

        writeBytes(from, "nuevo");
        writeBytes(to, kForeign);
        EXPECT_FALSE(renameWithoutReplacing(from, to, step)) << "reemplazo un archivo";
        EXPECT_EQ(readText(to), kForeign);
        EXPECT_EQ(readText(from), "nuevo") << "el origen se perdio con el destino ocupado";

        std::error_code ec;
        fs::create_directories(fs::path(dirTo) / "adentro", ec);
        ASSERT_FALSE(ec);
        EXPECT_FALSE(renameWithoutReplacing(from, dirTo, step)) << "reemplazo un directorio";
        EXPECT_TRUE(fs::is_directory(fs::path(dirTo) / "adentro"));

        fs::remove(to);
        EXPECT_TRUE(renameWithoutReplacing(from, to, step)) << "con el destino libre tiene que mover";
        EXPECT_EQ(readText(to), "nuevo");
        EXPECT_FALSE(exists(from)) << "quedaron los dos nombres";
    }
}

/**
 * Hallazgo 1 — LOS ERRNO QUE DICEN "ESTE FS NO SABE" BAJAN DE ESCALON; EL DESTINO OCUPADO CORTA.
 *
 * En Android el nativo da EINVAL en FUSE/sdcardfs y ENOSYS sin renameat2; si eso abortara, ninguna
 * toma quedaria nombrada ahi. Mutantes que mata: EINVAL/ENOSYS/EPERM clasificados como fallo;
 * EEXIST clasificado como "no soportado" (bajaria al escalon con TOCTOU).
 */
TEST(WavSinkRename, Hallazgo1_UnsupportedErrnosFallThroughAndAnOccupiedDestinationStops) {
    for (int e : {EINVAL, ENOSYS, ENOTSUP, EPERM, EXDEV}) {
        EXPECT_EQ(classifyRenameErrno(e), RenameStepOutcome::kUnsupported) << "errno " << e;
    }
    EXPECT_EQ(classifyRenameErrno(EEXIST), RenameStepOutcome::kExists);
    EXPECT_EQ(classifyRenameErrno(ENOTEMPTY), RenameStepOutcome::kExists);
    EXPECT_EQ(classifyRenameErrno(EIO), RenameStepOutcome::kFailed);
    EXPECT_EQ(classifyRenameErrno(ENOSPC), RenameStepOutcome::kFailed);
}

namespace {
RenameStepOutcome nativeUnsupported(const std::string&, const std::string&) {
    return RenameStepOutcome::kUnsupported;
}
RenameStepOutcome nativeFailed(const std::string&, const std::string&) {
    return RenameStepOutcome::kFailed;
}
int gLinkCalls = 0;
/// Un escalon de link que se cuenta y nombra con un rename plano SOLO si el destino esta libre.
RenameStepOutcome linkCounted(const std::string& from, const std::string& to) {
    ++gLinkCalls;
    if (fs::exists(fs::symlink_status(to))) return RenameStepOutcome::kExists;
    return std::rename(from.c_str(), to.c_str()) == 0 ? RenameStepOutcome::kDone
                                                      : RenameStepOutcome::kFailed;
}
RenameStepOutcome linkUnsupported(const std::string&, const std::string&) {
    ++gLinkCalls;
    return RenameStepOutcome::kUnsupported;
}
}  // namespace

/**
 * Hallazgo 1 — CON EL NATIVO NO SOPORTADO (Android 29, FUSE) EL RENOMBRE BAJA DE ESCALON: mueve si
 * el destino esta libre y no lo toca si esta ocupado. Con el nativo FALLIDO no insiste.
 *
 * En el host el nativo siempre resuelve: sin inyectarlo nadie ejecutaria el paso de escalon.
 * Mutantes que mata: el escalon nativo que devuelve sin bajar (toda toma en Android 29 quedaria
 * `finalized = 0`); bajar de escalon tras un fallo real.
 */
TEST_F(WavSinkTest, Hallazgo1_AnUnsupportedNativeStepFallsThroughWithoutEverReplacing) {
    const std::string from = pathOf("origen");
    const std::string to = pathOf("destino");
    writeBytes(from, "nuevo");
    writeBytes(to, kForeign);
    EXPECT_FALSE(renameWithoutReplacing(from, to, NoReplaceRenameStep::kNative, nativeUnsupported));
    EXPECT_EQ(readText(to), kForeign);
    EXPECT_EQ(readText(from), "nuevo");

    fs::remove(to);
    EXPECT_FALSE(renameWithoutReplacing(from, to, NoReplaceRenameStep::kNative, nativeFailed))
        << "un fallo real del nativo no se reintenta por otro escalon";
    EXPECT_TRUE(exists(from));
    EXPECT_FALSE(exists(to));

    EXPECT_TRUE(renameWithoutReplacing(from, to, NoReplaceRenameStep::kNative, nativeUnsupported))
        << "con el nativo no soportado y el destino libre tiene que nombrar por el escalon siguiente";
    EXPECT_EQ(readText(to), "nuevo");
    EXPECT_FALSE(exists(from));
}

/**
 * Hallazgo 1 — EL ORDEN DE LOS ESCALONES: con el nativo no soportado nombra el de LINK (atomico),
 * no el de lstat+rename (con TOCTOU); y con link tampoco soportado (sdcardfs, FUSE de Android 29)
 * se llega al ultimo, que nombra si el destino esta libre y no lo toca si esta ocupado.
 *
 * Mutantes que matan: saltar del nativo directo al ultimo escalon (pierde el no-reemplazo atomico);
 * un escalon de link terminal (toda toma en Android 29 sobre sdcardfs quedaria `finalized = 0`).
 */
TEST_F(WavSinkTest, Hallazgo1_TheStepsRunInOrderLinkBeforeCheckAndTheLastOneIsReached) {
    const std::string from = pathOf("origen");
    const std::string to = pathOf("destino");
    writeBytes(from, "nuevo");

    gLinkCalls = 0;
    EXPECT_TRUE(renameWithoutReplacing(from, to, NoReplaceRenameStep::kNative, nativeUnsupported,
                                       linkCounted));
    EXPECT_EQ(gLinkCalls, 1) << "con el nativo no soportado tiene que nombrar el escalon de link";
    EXPECT_EQ(readText(to), "nuevo");

    // Link tampoco soportado: el ultimo escalon nombra si esta libre...
    const std::string to2 = pathOf("destino2");
    gLinkCalls = 0;
    EXPECT_TRUE(renameWithoutReplacing(to, to2, NoReplaceRenameStep::kNative, nativeUnsupported,
                                       linkUnsupported))
        << "sin nativo ni link el ultimo escalon tiene que nombrar";
    EXPECT_EQ(gLinkCalls, 1);
    EXPECT_EQ(readText(to2), "nuevo");
    EXPECT_FALSE(exists(to));

    // ...y no toca un destino ocupado.
    writeBytes(from, "otro");
    writeBytes(to, kForeign);
    EXPECT_FALSE(renameWithoutReplacing(from, to, NoReplaceRenameStep::kNative, nativeUnsupported,
                                        linkUnsupported));
    EXPECT_EQ(readText(to), kForeign);
    EXPECT_EQ(readText(from), "otro");
}
