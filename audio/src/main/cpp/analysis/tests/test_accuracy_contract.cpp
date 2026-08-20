/**
 * test_accuracy_contract.cpp — REQ-001 S10 · 10.9.
 *
 * **Un contrato que puede quedar stale contra su propia evidencia es un contrato
 * que va a quedar stale.** Este test lee `docs/tuner/accuracy_contract.md`,
 * saca las cifras que declara, y las compara contra los `.resp` commiteados de
 * los que salieron.
 *
 * El escenario que existe para atajar es concreto y humano: alguien redondea un
 * numero del contrato para que quede mas lindo, o el DSP mejora y el contrato se
 * queda viejo. En los dos casos el `.md` empieza a describir un producto que no
 * es el que se compila, y **nada lo dice** — salvo esto.
 *
 * Es la misma regla que gobierna `regen-golden.sh` y la atestacion del gate: una
 * afirmacion que no se puede verificar contra lo que de verdad corrio no vale.
 */

#include "support/AnalysisGolden.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace wma_test {
namespace {

/// Las filas de un golden commiteado.
std::vector<golden::Sample> goldenRows(const std::string& name) {
    std::vector<golden::Sample> rows;
    EXPECT_TRUE(golden::readGolden(golden::goldenPath(name), rows))
        << "no se pudo leer el golden " << name;
    return rows;
}

/// Las claves del bloque CONTRACT-DATA del `.md`.
std::map<std::string, double> contractData() {
    std::map<std::string, double> out;
    const std::string path = std::string(WMA_TUNER_CONTRACT_MD);
    std::FILE* f = std::fopen(path.c_str(), "rb");
    EXPECT_NE(f, nullptr) << "no existe el contrato en " << path;
    if (f == nullptr) return out;

    char line[512];
    bool inBlock = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string text(line);
        if (text.find("CONTRACT-DATA") != std::string::npos) { inBlock = true; continue; }
        if (!inBlock) continue;
        if (text.find("-->") != std::string::npos) break;

        const size_t eq = text.find('=');
        if (eq == std::string::npos) continue;
        std::string key = text.substr(0, eq);
        std::string val = text.substr(eq + 1);
        // Recorte a ambos lados; las claves son ASCII y sin espacios internos.
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front()))) key.erase(key.begin());
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
        try {
            out[key] = std::stod(val);
        } catch (const std::exception&) {
            ADD_FAILURE() << "la clave '" << key << "' del contrato no es un numero";
        }
    }
    std::fclose(f);
    return out;
}

double at(const std::map<std::string, double>& m, const char* key) {
    const auto it = m.find(key);
    EXPECT_NE(it, m.end()) << "el contrato no declara '" << key << "'";
    return it == m.end() ? std::nan("") : it->second;
}

/**
 * Holgura RELATIVA del 1 %, no absoluta.
 *
 * 🔴 Arranque con 5e-4 absoluto y el test **no atajaba el caso que existe para
 * atajar**: cambiar `0.001092` por un `0.001` mas prolijo son 9,2e-5 de
 * diferencia, o sea que pasaba. Una tolerancia mas ancha que el redondeo que se
 * quiere prohibir no prohibe nada.
 *
 * Con 1 % relativo ese mismo retoque es un 8,4 % y muere. El piso absoluto es
 * para las cifras que valen cero, donde lo relativo no significa nada.
 */
double tolFor(double actual) {
    return std::max(1e-9, 0.01 * std::abs(actual));
}

// ---------------------------------------------------------------------------
// 10.9 — el contrato no puede divergir de su evidencia
// ---------------------------------------------------------------------------
TEST(AccuracyContract, TheDeclaredFiguresMatchTheirGoldens) {
    const auto c = contractData();
    ASSERT_FALSE(c.empty()) << "no se pudo leer el bloque CONTRACT-DATA";

    // --- strobe: peor error y peor σ a los 3 s ------------------------------
    {
        double worstErr = 0.0, worstSigma = 0.0;
        for (const auto& r : goldenRows("strobe_convergence")) {
            if (std::abs(r.seconds - 3.0) > 1e-9) continue;
            worstErr = std::max(worstErr, std::abs(r.cents - 1.0));
            worstSigma = std::max(worstSigma, r.uncertainty);
        }
        EXPECT_NEAR(at(c, "strobe_worst_error_cents"), worstErr, tolFor(worstErr))
            << "el contrato declara un error del strobe que el golden no respalda";
        EXPECT_NEAR(at(c, "strobe_worst_sigma_cents"), worstSigma, tolFor(worstSigma))
            << "el contrato declara una incertidumbre que el golden no respalda";
    }

    // --- deteccion gruesa ---------------------------------------------------
    {
        double worst = 0.0, minClarity = 1.0;
        for (const auto& r : goldenRows("coarse_detection")) {
            // `seconds` guarda la frecuencia verdadera y `cents` la detectada.
            worst = std::max(worst, 1200.0 * std::abs(std::log2(r.cents / r.seconds)));
            minClarity = std::min(minClarity, r.uncertainty);
        }
        EXPECT_NEAR(at(c, "coarse_worst_error_cents"), worst, tolFor(worst));
        EXPECT_NEAR(at(c, "coarse_min_clarity"), minClarity, tolFor(minClarity));
    }

    // --- modo rapido: los barridos no saltan de cuerda -----------------------
    {
        double switches = 0.0;
        for (const auto& r : goldenRows("fast_mode_sweep")) {
            switches = std::max(switches, r.uncertainty);
        }
        EXPECT_NEAR(at(c, "fast_mode_sweep_switches"), switches, 1e-9)
            << "el contrato promete que no salta de cuerda y el golden dice otra cosa";
    }

    // --- inarmonicidad: la correccion maxima del catalogo --------------------
    {
        double maxCorr = 0.0;
        for (const auto& r : goldenRows("inharmonicity_corrections")) {
            maxCorr = std::max(maxCorr, std::abs(r.uncertainty));
        }
        EXPECT_NEAR(at(c, "inharmonicity_max_correction_cents"), maxCorr, tolFor(maxCorr));
    }

    // --- la cifra que hace HONESTO al contrato -------------------------------
    //
    // No sale de un golden sino de la fisica del cristal, asi que se recomputa
    // acá: 50 ppm de error de escala en cents. Es el numero que impide declarar
    // exactitud ABSOLUTA de ±0,1 cent, y por eso tiene que ser verificable igual
    // que los demas — es el que un redactor optimista borraria primero.
    EXPECT_NEAR(at(c, "crystal_50ppm_cents"), 1200.0 * std::log2(1.0 + 50e-6),
                tolFor(1200.0 * std::log2(1.0 + 50e-6)));
}

/**
 * El contrato tiene que **decir** lo que no puede prometer. Un contrato que sólo
 * lista logros no es un contrato: es marketing con tabla.
 */
TEST(AccuracyContract, ItStatesWhatTheProductCannotPromise) {
    const std::string path = std::string(WMA_TUNER_CONTRACT_MD);
    std::FILE* f = std::fopen(path.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    std::string all;
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    std::fclose(f);

    EXPECT_NE(all.find("NO puede prometer"), std::string::npos)
        << "el contrato no declara sus limites";
    EXPECT_NE(all.find("50 ppm"), std::string::npos)
        << "no menciona la tolerancia del cristal, que es LA razon por la que la "
           "exactitud absoluta no se puede prometer";
    EXPECT_NE(all.find("sin medir"), std::string::npos)
        << "no marca lo que todavia no se midio: una tabla sin huecos se lee como "
           "cobertura completa";
}

}  // namespace
}  // namespace wma_test
