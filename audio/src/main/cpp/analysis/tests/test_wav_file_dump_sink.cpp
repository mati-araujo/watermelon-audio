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
 * Corre contra un directorio temporal propio, que se borra al terminar.
 */

#include "../TunerInputDump.h"
#include "../WavFileDumpSink.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
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
    std::error_code ec;
    fs::create_directories(fs::path(path) / "adentro", ec);   // un directorio NO vacio con ese nombre
    ASSERT_FALSE(ec);

    WavFileDumpSink sink;
    ASSERT_TRUE(sink.openPart(path, kInputDumpHeaderBytes));
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
