/**
 * test_temporal_oracle.cpp — REQ-038 S2, tarea 2.1.
 *
 * EL SELF-TEST DEL ORACULO TEMPORAL.
 *
 * 🔴 EN ESTE REPO UN ORACULO QUE NO PUEDE FALLAR NO ES UN ORACULO. `TemporalOracle.h` es
 * la referencia contra la que el gate del corpus va a juzgar la deteccion gruesa, asi que
 * antes hay que probar que MIDE, y que mide LO QUE DICE — el periodo de la forma de onda,
 * no el pico espectral de H1.
 *
 * Y hay que probarlo en LAS DOS DIRECCIONES, que es la parte que se olvida:
 *
 *   · sobre una serie ARMONICA (B = 0) las dos cantidades coinciden, asi que el oraculo
 *     temporal tiene que dar f0. Esto solo lo pasaria tambien un oraculo cableado a H1.
 *   · sobre una serie INARMONICA (B > 0) NO coinciden, y el temporal tiene que apartarse
 *     de f0 — hacia ARRIBA, porque cada parcial `n` implica un fundamental
 *     `f0·sqrt(1+B·n²) > f0` — y apartarse MAS cuanto mayor sea B.
 *
 * Sin el segundo caso, un oraculo que devolviera H1 siempre pasaria el primero y el gate
 * entero quedaria comparando la gruesa contra la misma cantidad de antes, sin que nada lo
 * dijera. Es la clase de "una copia fiel es invisible para todo instrumento".
 */
#include <gtest/gtest.h>

#include "support/SyntheticSignal.h"
#include "support/TemporalOracle.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int kRate = 44100;
constexpr int kFrames = kRate * 4;  // 4 s: sobra para la ventana desde 1,5 s
constexpr double kF0 = 82.41;       // E2, la cuerda del caso que abrio REQ-038

double centsOf(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

/// El fundamental que IMPLICA el parcial n de una serie estirada de coeficiente B.
double impliedF0Cents(double B, int n) { return 600.0 * std::log2(1.0 + B * n * n); }

}  // namespace

/**
 * AC-038.4 (instrumento) — sobre una serie ARMONICA el oraculo temporal da el fundamental.
 *
 * Es la mitad facil, y sirve de control: si esto fallara, lo que sigue no significaria nada.
 */
TEST(TemporalOracle, OnAHarmonicSeriesItAgreesWithTheFundamental) {
    const std::vector<float> sig =
        wma_test::inharmonicString(kF0, 0.0, 8, kRate, kFrames);

    const wma_test::oracle::TemporalReading t =
        wma_test::oracle::measurePeriod(sig, kRate, kF0);

    ASSERT_TRUE(t.valid) << "no midio: la ventana no alcanzo";
    const double c = centsOf(t.hz, kF0);
    std::printf("  [armonica B=0]      %.4f Hz = %+6.3f c   r = %.5f\n", t.hz, c, t.r);

    EXPECT_LT(std::fabs(c), 0.5)
        << "sobre armonicos exactos el periodo de la forma de onda ES 1/f0: " << c << " c";
    // Una serie exacta se repite: la correlacion en el periodo tiene que ser casi 1.
    EXPECT_GT(t.r, 0.99) << "r = " << t.r << " sobre una señal perfectamente periodica";
}

/**
 * 🔴 LA MITAD QUE IMPORTA — sobre una serie INARMONICA el oraculo temporal se aparta de f0,
 * hacia arriba, y mas cuanto mayor es B.
 *
 * Este es el test que un oraculo cableado a H1 no puede pasar, y es la razon de que
 * `TemporalOracle` no use NSDF: tiene que ser otro METODO midiendo la misma CANTIDAD.
 *
 * La direccion no es una convencion, se deriva: los parciales estan en
 * `f_n = n·f0·sqrt(1+B·n²)`, asi que el parcial n implica un fundamental
 * `f0·sqrt(1+B·n²)`, que es SIEMPRE mayor que f0. El mejor periodo de la forma de onda es
 * un compromiso entre todos ellos, ponderado por su energia (amplitudes 1/n), asi que cae
 * entre f0 y el fundamental que implica el parcial mas alto con energia.
 */
TEST(TemporalOracle, OnAnInharmonicSeriesItDepartsFromTheFundamentalUpwardAndGrowsWithB) {
    struct Caso {
        double B;
        const char* etiqueta;
    };
    // B = 1e-4 es el que usa el resto de la suite para una cuerda tipica; 4e-4 es una
    // bordona. Los dos estan dentro del rango fisico de una cuerda de acero.
    constexpr Caso kCasos[] = {{0.0, "B=0    "}, {1e-4, "B=1e-4"}, {4e-4, "B=4e-4"}};

    // La linea de base la fija el PRIMER caso (B = 0), medida acá y no escrita a mano: las
    // dos propiedades que siguen son MONOTONIAS contra ella, no umbrales.
    double anterior = -1.0;   // cents del caso anterior
    double rAnterior = 2.0;   // r del caso anterior (empieza arriba de cualquier r real)
    const double kResolucion = wma_test::oracle::resolutionCents(kRate, kF0);
    std::printf("  resolucion del oraculo a %.2f Hz: %.3f c\n", kF0, kResolucion);
    for (const Caso& caso : kCasos) {
        const std::vector<float> sig =
            wma_test::inharmonicString(kF0, caso.B, 8, kRate, kFrames);

        const wma_test::oracle::TemporalReading t =
            wma_test::oracle::measurePeriod(sig, kRate, kF0);
        ASSERT_TRUE(t.valid) << caso.etiqueta;

        const double c = centsOf(t.hz, kF0);
        std::printf("  [%s]  %.4f Hz = %+6.3f c   r = %.5f   (H8 implica %+.2f c)\n",
                    caso.etiqueta, t.hz, c, t.r, impliedF0Cents(caso.B, 8));

        // (a) EL TECHO, que sale de la fisica y no de una constante elegida: el desvio no
        //     puede pasar al fundamental que implica el parcial mas agudo con energia. La
        //     holgura es LA RESOLUCION DEL PROPIO ORACULO (el paso de su grilla de lags,
        //     0,16 c acá), derivada y no escrita: sin ella el caso B = 0 —cuyo techo es
        //     exactamente 0— falla por los +0,013 c de la cuantizacion, que no son un error.
        EXPECT_LT(c, impliedF0Cents(caso.B, 8) + kResolucion)
            << caso.etiqueta << ": se aparto MAS que el parcial mas agudo de la serie";
        // (b) MONOTONO EN B, y ESTRICTO. Es lo que ata la lectura a la inarmonicidad en vez
        //     de a un numero: un oraculo cableado a H1 daria ~0 en los tres y muere aca,
        //     igual que uno que devolviera cualquier constante.
        EXPECT_GT(c, anterior) << caso.etiqueta
                               << ": mas inarmonicidad tiene que dar MAS desvio hacia arriba, "
                                  "y dio " << c << " c contra " << anterior;
        anterior = c;

        // (c) Y LA CORRELACION BAJA MONOTONAMENTE: una serie inarmonica no se repite
        //     exactamente, asi que su mejor periodo correlaciona PEOR que el de una exacta.
        //     Tambien contra la linea de base medida, no contra un umbral — con B = 1e-4 sobre
        //     8 parciales r todavia vale 0,9999, y cualquier constante que se escribiera aca
        //     seria una adivinanza (medido: se probo 0,999 y era falsa).
        //     Es la misma firma que S1 midio en el corpus: 0,986 en acero contra 1,000 en el
        //     control, o sea "esto no tiene un periodo exacto".
        EXPECT_LT(t.r, rAnterior) << caso.etiqueta
                                  << ": r = " << t.r << " no bajo respecto del caso menos "
                                                        "inarmonico (" << rAnterior << ")";
        rAnterior = t.r;
    }
}

/**
 * El oraculo no inventa una lectura cuando no hay con que: sin muestras suficientes despues
 * de `fromSec`, devuelve `valid = false` en vez de un numero plausible.
 *
 * Importa porque el corpus tiene notas de 1,5 s y el gate las barre a todas: una lectura
 * fabricada sobre una nota corta seria un dato falso y CONVINCENTE, que es el peor tipo.
 */
TEST(TemporalOracle, WithoutEnoughSignalItReportsNothingInsteadOfGuessing) {
    const std::vector<float> corta =
        wma_test::inharmonicString(kF0, 1e-4, 8, kRate, kRate / 2);  // 0,5 s

    const wma_test::oracle::TemporalReading t =
        wma_test::oracle::measurePeriod(corta, kRate, kF0, /*fromSec=*/1.5);
    EXPECT_FALSE(t.valid) << "midio sobre una ventana que empieza despues del final del archivo";
    EXPECT_EQ(t.hz, 0.0);
}
