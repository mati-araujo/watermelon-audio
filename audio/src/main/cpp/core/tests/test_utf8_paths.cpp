/**
 * test_utf8_paths.cpp — AC-061.3 (REQ-061 S2): todo `const char*` que es un path es UTF-8.
 *
 * Corre en Windows Y en POSIX. En Windows la JVM entrega los paths en UTF-8 y los APIs "estrechos"
 * (`open`, `std::ofstream(std::string)`, `CreateFileA`) los leen con la pagina ANSI: un usuario
 * «Martín» no cargaba su SoundFont y exportaba el WAV a un nombre distinto. Aca cada carpeta tiene un
 * caracter fuera de ASCII y se verifica con std::filesystem (que no pasa por el borde bajo prueba)
 * que el archivo cayo con el nombre correcto.
 */

#include "support/MinimalSoundFont.h"

#include "analysis/WavFileDumpSink.h"
#include "engines/SoundFontManager.h"
#include "looper/WavFile.h"
#include "platform/FileUtf8.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define WMA_TEST_GETPID _getpid
#else
#include <unistd.h>
#define WMA_TEST_GETPID getpid
#endif

namespace {

namespace fs = std::filesystem;
using namespace wma::analysis;

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

/// Directorios de usuario realistas: latino con tilde, otro latino, CJK (3 bytes UTF-8).
const std::vector<std::u8string>& nonAsciiNames() {
    static const std::vector<std::u8string> names = {u8"Martín", u8"ñandú", u8"音楽"};
    return names;
}

class Utf8PathTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() /
               ("wma-utf8-" + std::to_string(static_cast<long long>(WMA_TEST_GETPID())) + "-" +
                std::to_string(tick));
        std::error_code ec;
        fs::create_directories(root, ec);
        ASSERT_FALSE(ec) << "no pude crear " << root;
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    /// <root>/<nombre>/ creado con std::filesystem (no pasa por el borde bajo prueba).
    fs::path makeDir(const std::u8string& name) const {
        const fs::path d = root / fs::path(name);
        std::error_code ec;
        fs::create_directories(d, ec);
        EXPECT_FALSE(ec) << "no pude crear el directorio no ASCII";
        return d;
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

    fs::path root;
};

}  // namespace

/**
 * AC-061.3 — el .sf2 de una carpeta con nombre no ASCII carga por `loadFromPath`.
 *
 * Bug que atrapa: abrir el path UTF-8 con la pagina ANSI (Windows): «Martín» no existe y la carga
 * falla en silencio; el usuario no tiene sonido.
 */
TEST_F(Utf8PathTest, AC_061_3_ASoundFontLoadsFromANonAsciiDirectory) {
    const std::vector<uint8_t> sf2 = wma_test::makeMinimalSoundFont(22050);
    for (const std::u8string& name : nonAsciiNames()) {
        SCOPED_TRACE(utf8(fs::path(name)));
        const fs::path file = makeDir(name) / fs::path(u8"fuente-ñ.sf2");
        {
            std::ofstream out(file, std::ios::binary);
            out.write(reinterpret_cast<const char*>(sf2.data()),
                      static_cast<std::streamsize>(sf2.size()));
        }
        ASSERT_TRUE(fs::exists(file)) << "premisa: el fixture no se escribio";

        SoundFontManager manager;
        ASSERT_TRUE(manager.loadFromPath(utf8(file).c_str(), 44100));
        EXPECT_EQ(manager.getPresetCount(), 1);
    }
}

/**
 * AC-061.3 — el export de `WavFile` cae en el directorio y con el nombre correctos, y se relee por el
 * mismo borde (`readWav` y `readWavInfo` son la lectura).
 *
 * Bug que atrapa: `std::ofstream(const char*)` con la pagina ANSI crea «MartÃ­n» (o falla) en vez de
 * «Martín»; la lectura con el mismo bug no lo ve y el round trip "pasa" sobre el nombre equivocado, por
 * eso la existencia se verifica por std::filesystem con el nombre correcto.
 */
TEST_F(Utf8PathTest, AC_061_3_AWavExportsToAndReadsFromANonAsciiPath) {
    const int frames = 256;
    std::vector<float> pcm(static_cast<size_t>(frames) * 2);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = (i % 2 ? 0.25f : -0.25f);
    for (const std::u8string& name : nonAsciiNames()) {
        SCOPED_TRACE(utf8(fs::path(name)));
        const fs::path file = makeDir(name) / fs::path(u8"exportación.wav");
        const std::string path = utf8(file);

        ASSERT_TRUE(wav::writeWav(path.c_str(), pcm.data(), frames, 44100));

        std::error_code ec;
        ASSERT_TRUE(fs::exists(file, ec)) << "el WAV no cayo en el nombre correcto";
        EXPECT_GT(fs::file_size(file, ec), 44u);

        wav::WavInfo info;
        EXPECT_EQ(wav::readWavInfo(path.c_str(), info), wav::WavReadStatus::Ok);
        EXPECT_EQ(info.numFrames, frames);
        const wav::WavData data = wav::readWav(path.c_str());
        EXPECT_EQ(data.numFrames, frames);
        EXPECT_EQ(data.buffer.size(), pcm.size());
    }
}

/**
 * AC-061.3 — el dump escribe `<path>` (sin `.part` colgado) en un directorio no ASCII.
 *
 * Bug que atrapa: el `.part` creado con la pagina ANSI o el renombre que no entiende el UTF-8: la toma
 * queda en un nombre distinto o sin finalizar (`finalized = 0`).
 */
TEST_F(Utf8PathTest, AC_061_3_TheDumpWritesToANonAsciiDirectory) {
    for (const std::u8string& name : nonAsciiNames()) {
        SCOPED_TRACE(utf8(fs::path(name)));
        const fs::path file = makeDir(name) / fs::path(u8"toma-á.wav");
        const std::string path = utf8(file);
        const std::vector<float> samples = {0.5f, -0.5f, 0.25f, 1.0f};

        WavFileDumpSink sink;
        ASSERT_TRUE(sink.openPart(path, kInputDumpHeaderBytes));
        ASSERT_TRUE(sink.appendSamples(samples.data(), samples.size()));
        const std::vector<uint8_t> header = headerFor(samples.size());
        ASSERT_EQ(sink.finishPart(header.data(), header.size(), true), path);

        std::error_code ec;
        ASSERT_TRUE(fs::exists(file, ec)) << "la toma no quedo con el nombre correcto";
        EXPECT_EQ(fs::file_size(file, ec), kInputDumpHeaderBytes + samples.size() * sizeof(float));
        EXPECT_FALSE(fs::exists(fs::path(utf8(file) + ".part"), ec)) << "quedo el .part";
    }
}

/**
 * AC-061.3 — el contrato del borde: la conversion UTF-8 → UTF-16 es estricta.
 *
 * Bug que atrapa: un decodificador permisivo acepta UTF-8 invalido (sobrelargo, sustituto, > U+10FFFF,
 * truncado) y abre un archivo que nadie nombro; o decodifica mal un par sustituto (emoji).
 */
TEST(Utf8Boundary, AC_061_3_ConvertsValidUtf8AndRejectsInvalid) {
    std::u16string out;
    ASSERT_TRUE(wma::platform::utf8ToUtf16(reinterpret_cast<const char*>(u8"Martín"), out));
    EXPECT_EQ(out, u"Martín");
    ASSERT_TRUE(wma::platform::utf8ToUtf16(reinterpret_cast<const char*>(u8"音楽"), out));
    EXPECT_EQ(out, u"音楽");
    ASSERT_TRUE(wma::platform::utf8ToUtf16("\xF0\x9F\x98\x80", out));   // U+1F600, par sustituto
    EXPECT_EQ(out, u"\xD83D\xDE00");
    ASSERT_TRUE(wma::platform::utf8ToUtf16("", out));
    EXPECT_TRUE(out.empty());

    for (const char* bad : {"\xFF", "\xC3", "\xC0\x80", "\xE0\x80\x80", "\xED\xA0\x80",
                            "\xF4\x90\x80\x80", "ab\xE9", "\x80"}) {
        EXPECT_FALSE(wma::platform::utf8ToUtf16(bad, out)) << "acepto UTF-8 invalido";
    }
    EXPECT_FALSE(wma::platform::utf8ToUtf16(nullptr, out));
}

/**
 * AC-061.3 — las secuencias de CUATRO bytes son igual de estrictas que las de dos y tres.
 *
 * Bug que atrapa: el minimo de una secuencia de 4 bytes mal puesto (0x800 en vez de 0x10000 acepta
 * `F0 8F BF BF`, U+FFFF sobrelargo); un lead F5..FF aceptado (mayor a U+10FFFF por la cabeza); un
 * truncado a media secuencia de 4 bytes que se lee como valido.
 */
TEST(Utf8Boundary, AC_061_3_FourByteSequencesAreStrict) {
    std::u16string out;
    ASSERT_TRUE(wma::platform::utf8ToUtf16("\xF0\x90\x80\x80", out)) << "U+10000, el minimo legal de 4 bytes";
    EXPECT_EQ(out, u"\xD800\xDC00");
    ASSERT_TRUE(wma::platform::utf8ToUtf16("\xF4\x8F\xBF\xBF", out)) << "U+10FFFF, el maximo";
    EXPECT_EQ(out, u"\xDBFF\xDFFF");

    EXPECT_FALSE(wma::platform::utf8ToUtf16("\xF0\x8F\xBF\xBF", out)) << "U+FFFF sobrelargo en 4 bytes";
    EXPECT_FALSE(wma::platform::utf8ToUtf16("\xF0\x80\x80\x80", out)) << "NUL sobrelargo en 4 bytes";
    // El lead F4 es legal: solo el tope `cp > 0x10FFFF` rechaza lo que pasa de U+10FFFF.
    EXPECT_FALSE(wma::platform::utf8ToUtf16("\xF4\x90\x80\x80", out)) << "U+110000, uno mas que el maximo";
    EXPECT_FALSE(wma::platform::utf8ToUtf16("\xF4\xBF\xBF\xBF", out)) << "U+13FFFF, F4 con continuaciones al tope";
    for (int lead = 0xF5; lead <= 0xFF; ++lead) {
        const char seq[] = {static_cast<char>(lead), static_cast<char>(0x80), static_cast<char>(0x80),
                            static_cast<char>(0x80), 0};
        EXPECT_FALSE(wma::platform::utf8ToUtf16(seq, out)) << "lead 0x" << std::hex << lead;
    }
    for (const char* truncated : {"\xF0", "\xF0\x9F", "\xF0\x9F\x98", "ab\xF0\x9F\x98"}) {
        EXPECT_FALSE(wma::platform::utf8ToUtf16(truncated, out)) << "secuencia de 4 bytes truncada";
    }
}

#if defined(_WIN32)

/**
 * AC-061.3 (Windows) — un path que NO es UTF-8 valido falla cerrado: no se abre nada, ni se escribe a
 * un nombre "parecido" por la pagina ANSI.
 *
 * Bug que atrapa: caer al constructor estrecho cuando la conversion falla.
 */
TEST_F(Utf8PathTest, AC_061_3_WindowsAnInvalidUtf8PathFailsClosed) {
    const std::string bad = utf8(makeDir(u8"ok") / fs::path(u8"x")) + "\xFF.wav";
    const std::vector<float> pcm(8, 0.1f);

    EXPECT_FALSE(wav::writeWav(bad.c_str(), pcm.data(), 4, 44100));
    wav::WavInfo info;
    EXPECT_EQ(wav::readWavInfo(bad.c_str(), info), wav::WavReadStatus::Io);
    EXPECT_TRUE(wma::platform::pathFromUtf8(bad.c_str()).empty());
}

#endif
