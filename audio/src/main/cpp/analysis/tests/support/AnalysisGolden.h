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

inline bool writeGolden(const std::string& path, const std::string& name,
                        int sampleRate, int windowFrames,
                        const std::vector<Sample>& rows,
                        const ColumnNames& cols = {}) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    std::fprintf(f, "# watermelon-audio golden — analysis/%s (REQ-001 S2)\n", name.c_str());
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

inline bool readGolden(const std::string& path, std::vector<Sample>& rows) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char label[128] = {0};
        double sec = 0.0, cents = 0.0, unc = 0.0;
        if (std::sscanf(line, "%127s %lf %lf %lf", label, &sec, &cents, &unc) == 4) {
            rows.push_back({label, sec, cents, unc});
        }
    }
    std::fclose(f);
    return true;
}

/**
 * Compara contra el golden commiteado, o lo reescribe en modo regeneracion.
 *
 * El mensaje de fallo dice **que fila** se movio y cuanto: un golden que solo
 * dice "cambio" obliga a reconstruir el diff a mano, y entonces no se lee.
 */
inline void checkOrRegen(const std::string& name, int sampleRate, int windowFrames,
                         const std::vector<Sample>& rows, const ColumnNames& cols = {}) {
    const std::string path = goldenPath(name);

    if (regenRequested()) {
        ASSERT_TRUE(writeGolden(path, name, sampleRate, windowFrames, rows, cols))
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
