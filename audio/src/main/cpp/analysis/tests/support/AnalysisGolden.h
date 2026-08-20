#pragma once

/**
 * AnalysisGolden.h — la red golden del camino de análisis (REQ-001 S2, 2.12).
 *
 * MISMAS REGLAS QUE `effects/tests/GoldenHarness.h`, Y NO ES COPIA POR PEREZA
 * ---------------------------------------------------------------------------
 * Aquel depende de `Effect.h` y mide |H(f)| de un sistema LTI. Acá no hay efecto
 * ni respuesta en frecuencia: lo que se congela es la **curva de convergencia**
 * del estimador — cuántos cents reporta, y con cuánta incertidumbre, a medida
 * que integra. Compartir el header significaría arrastrar la libreria de efectos
 * a la sub-lib de analisis, que es justo el limite que `analysis/CMakeLists.txt`
 * declara.
 *
 * Lo que SI se comparte son las reglas, porque son las que valen:
 *
 *   - **Texto, no binario.** El `.resp` se lee en el PR; su diff ES la revision.
 *   - **Regenerar es una tarea explicita** (`bash scripts/regen-golden.sh`), no
 *     un efecto colateral de correr los tests.
 *   - **En modo regeneracion el test queda SKIPPED, no PASSED.** Una corrida que
 *     ESCRIBE no puede pasar por una que VERIFICA. Es la misma regla que
 *     `.github/local-gate.json`.
 *
 * LA TOLERANCIA, Y DE DONDE SALE
 * ------------------------------
 * 1e-3 cents. Es **100 veces mas fina que el presupuesto** del producto (0,1
 * cent) y a la vez holgada contra la deriva de `atan2`/`log2` entre libms —el
 * estimador corre entero en `double`, asi que la deriva esperable esta varios
 * ordenes por debajo. Si un cambio de DSP mueve la curva, la mueve MUCHO mas que
 * eso; si el golden se rompe por 1e-3, lo que cambio es la plataforma y hay que
 * mirarlo, no subir el numero.
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <stdexcept>

#include <gtest/gtest.h>

namespace wma_test::golden {

/// Una fila de la curva: cuanto se integro y que se leyo.
struct Sample {
    std::string label;
    double seconds;
    double cents;
    double uncertainty;
};

inline constexpr double kToleranceCents = 1e-3;

inline bool regenRequested() {
    const char* v = std::getenv("WMA_GOLDEN_REGEN");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

inline std::string goldenPath(const std::string& name) {
    return std::string(WMA_ANALYSIS_GOLDEN_DIR) + "/" + name + ".resp";
}

/**
 * Nombres de las columnas. Se parametrizan porque el mismo formato sirve para dos medidas
 * distintas —la curva de convergencia (cents/incertidumbre) y la deteccion gruesa
 * (Hz/claridad)— y un encabezado fijo estaria MINTIENDO en uno de los dos. Un artefacto
 * commiteado que se lee en un PR no puede tener las columnas cambiadas.
 */
struct ColumnNames {
    const char* x = "segundos";
    const char* y = "cents";
    const char* z = "incertidumbreCents";
};

/**
 * La ETAPA va como parametro por la misma razon que las columnas: estaba fija en
 * "REQ-001 S2" y el golden de la deteccion gruesa —que es de S4— se declaraba de
 * S2. Con S6 agregando un tercero, un sello fijo mentiria en dos de tres.
 */
inline bool writeGolden(const std::string& path, const std::string& name,
                        int sampleRate, int windowFrames,
                        const std::vector<Sample>& rows,
                        const ColumnNames& cols = {},
                        const char* stage = "REQ-001") {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    std::fprintf(f, "# watermelon-audio golden — analysis/%s (%s)\n", name.c_str(), stage);
    std::fprintf(f, "# sampleRate=%d  window=%d  rows=%zu\n",
                 sampleRate, windowFrames, rows.size());
    std::fprintf(f, "# El signo es el del afinador: senal por encima del objetivo = POSITIVO.\n");
    std::fprintf(f, "# caso\t%s\t%s\t%s\n", cols.x, cols.y, cols.z);
    for (const auto& r : rows) {
        std::fprintf(f, "%s\t%.2f\t%.6f\t%.6f\n",
                     r.label.c_str(), r.seconds, r.cents, r.uncertainty);
    }
    std::fclose(f);
    return true;
}

/**
 * 🔴 SE PARTE POR TABS, Y UNA LINEA QUE NO PARSEA ES UN ERROR, NO UN SALTEO.
 *
 * Esto usaba `sscanf(line, "%127s %lf %lf %lf", …)`, y `%s` CORTA EN EL PRIMER
 * ESPACIO. Con etiquetas de una sola palabra —las unicas que habia— andaba; el
 * primer golden con una etiqueta como "guitarra E2" devolvio **cero filas**, en
 * silencio. Y el sintoma no apuntaba al parser: el test fallaba diciendo "el
 * conjunto de casos cambio", que manda a revisar los casos en vez del lector.
 *
 * El writer separa los campos con TAB, asi que el lector se parte por tab. Y una
 * linea que no parsea **falla**: saltearla es lo que convirtio un bug de parseo
 * en un mensaje engañoso.
 */
inline bool readGolden(const std::string& path, std::vector<Sample>& rows) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    char line[512];
    bool ok = true;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == '\0') continue;

        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        if (text.empty()) continue;

        std::string field[4];
        int n = 0;
        size_t start = 0;
        for (size_t i = 0; i <= text.size() && n < 4; ++i) {
            if (i == text.size() || text[i] == '\t') {
                field[n++] = text.substr(start, i - start);
                start = i + 1;
            }
        }
        if (n != 4) { ok = false; continue; }
        try {
            rows.push_back({field[0], std::stod(field[1]), std::stod(field[2]),
                            std::stod(field[3])});
        } catch (const std::exception&) {
            ok = false;
        }
    }
    std::fclose(f);
    return ok;
}

/**
 * Compara contra el golden commiteado, o lo reescribe en modo regeneracion.
 *
 * El mensaje de fallo dice **que fila** se movio y cuanto: un golden que solo
 * dice "cambio" obliga a reconstruir el diff a mano, y entonces no se lee.
 */
inline void checkOrRegen(const std::string& name, int sampleRate, int windowFrames,
                         const std::vector<Sample>& rows, const ColumnNames& cols = {},
                         const char* stage = "REQ-001") {
    const std::string path = goldenPath(name);

    if (regenRequested()) {
        ASSERT_TRUE(writeGolden(path, name, sampleRate, windowFrames, rows, cols, stage))
            << "No pude escribir " << path;
        GTEST_SKIP() << "REGENERADO (no verificado): " << path;
    }

    std::vector<Sample> want;
    ASSERT_TRUE(readGolden(path, want))
        << "Falta el golden " << path << ".\n"
           "Se captura con: bash scripts/regen-golden.sh";

    ASSERT_EQ(want.size(), rows.size())
        << "El golden " << name << " tiene otra cantidad de filas: el conjunto de "
           "casos cambio, y eso se revisa a mano antes de recapturar.";

    for (size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(want[i].label, rows[i].label)
            << "fila " << i << ": cambio el orden o el nombre de los casos";
        EXPECT_NEAR(rows[i].cents, want[i].cents, kToleranceCents)
            << "caso '" << rows[i].label << "' a " << rows[i].seconds << " s: "
            << "el golden dice " << want[i].cents << " cents y ahora mide "
            << rows[i].cents << ".\n"
               "Si el cambio es INTENCIONAL: bash scripts/regen-golden.sh, "
               "y el diff va revisado en el PR.";
        EXPECT_NEAR(rows[i].uncertainty, want[i].uncertainty, kToleranceCents)
            << "caso '" << rows[i].label << "': se movio la incertidumbre";
    }
}

}  // namespace wma_test::golden
