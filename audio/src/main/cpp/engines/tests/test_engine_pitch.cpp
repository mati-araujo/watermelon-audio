/**
 * WD-2.3.2 — la fundamental de Karplus-Strong, a 44,1 / 48 / 96 kHz.
 *
 * EL CRITERIO DEL REQUERIMIENTO, Y LO QUE LA MEDICION ENCONTRO
 * -----------------------------------------------------------
 * WD-2.3.2 pide que "la frecuencia fundamental medida por FFT no se corra entre
 * 44,1 / 48 / 96 kHz". **No se cumple**, y lo que hay debajo es peor que una
 * dependencia del rate: la cuerda esta desafinada BAJO en terminos absolutos, y
 * cuanto mas aguda la nota, mas desafinada.
 *
 *      pedido      44,1 kHz     48 kHz      96 kHz
 *      110 Hz      -29 cents    -27         -14
 *      220 Hz      -61          -56         -28
 *      440 Hz      -118         -108        -57
 *
 * A 440 Hz sobre 44,1 kHz eso es **un semitono entero**. No es sutil y no
 * necesita instrumentos para oirse.
 *
 * LA CAUSA SALE DE LOS PROPIOS NUMEROS
 * ------------------------------------
 * El lazo de Karplus-Strong deberia medir `fs / f` muestras. Mide `fs / f + D`,
 * con D CONSTANTE — medido 6,8 / 7,07 / 7,13 muestras para 110 / 220 / 440 Hz.
 * Son el retardo de grupo del filtro de un polo del lazo mas el del interpolador
 * de la linea de retardo, y nadie los descuenta del largo pedido.
 *
 * De ahi salen las dos formas del sintoma, que son la misma:
 *   - el error RELATIVO es D*f/fs, asi que crece con la nota...
 *   - ...y baja con el sample rate. Por eso ademas rompe la invariancia.
 *
 * Compensar D arregla las dos de una. `pitch-baseline.txt` declara la deuda.
 *
 * POR QUE EL INSTRUMENTO TIENE SU PROPIO TEST
 * -------------------------------------------
 * Porque tres estimadores distintos devolvieron numeros plausibles y falsos
 * antes de que este midiera bien — ver el encabezado de `PitchHarness.h`. Un
 * test de afinacion cuyo estimador no esta validado no mide el motor: mide el
 * estimador, y no avisa cual de los dos fallo.
 */

#include "PitchHarness.h"

#include "../KarplusStrongEngine.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace wma::pitch;

/// Renderiza `seconds` de un engine y devuelve el canal izquierdo.
std::vector<float> renderLeft(SynthEngine& e, int rate, double seconds, float hz) {
    const int frames = static_cast<int>(seconds * rate);
    constexpr int kBlock = 512;
    std::vector<float> buf(static_cast<size_t>(kBlock) * 2, 0.0f);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(frames));
    for (int done = 0; done < frames; done += kBlock) {
        const int n = std::min(kBlock, frames - done);
        e.process(buf.data(), n, hz, 0.8f);
        for (int i = 0; i < n; ++i) out.push_back(buf[static_cast<size_t>(i) * 2]);
    }
    return out;
}

/// La fundamental de un Karplus-Strong recien preparado al rate pedido.
/// Se saltean 100 ms de ataque y se miden los 400 ms siguientes: la excitacion
/// no es periodica y arrastra la estimacion si entra en la ventana.
double karplusFundamental(int prepareRate, int playbackRate, float targetHz) {
    KarplusStrongEngine e;
    e.prepare(prepareRate, 512);
    e.reset();
    const std::vector<float> x = renderLeft(e, prepareRate, 1.0, targetHz);
    const size_t from = static_cast<size_t>(0.1 * playbackRate);
    const size_t len = static_cast<size_t>(0.4 * playbackRate);
    return fundamentalHz(x, from, len, playbackRate, targetHz);
}

std::set<std::string> readBaseline(const char* path) {
    std::set<std::string> names;
    std::FILE* f = std::fopen(path, "rb");
    EXPECT_NE(f, nullptr) << "no pude abrir " << path;
    if (f == nullptr) return names;
    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string s(line);
        const size_t hash = s.find('#');
        if (hash != std::string::npos) s = s.substr(0, hash);
        const size_t bar = s.find('|');
        if (bar != std::string::npos) s = s.substr(0, bar);
        const size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        const size_t e = s.find_last_not_of(" \t\r\n");
        names.insert(s.substr(b, e - b + 1));
    }
    std::fclose(f);
    return names;
}

}  // namespace

// ===========================================================================
// PASO 1 — el instrumento. Sin esto, nada de lo de abajo significa nada.
// ===========================================================================

TEST(EnginePitch, TheEstimatorIsExactOnSignalsOfKnownPitch) {
    // Un seno puro y un pluck cuyo SEGUNDO ARMONICO es mas fuerte que la
    // fundamental. El segundo es el que importa: un estimador que buscara el
    // pico espectral devolveria 2*f y pasaria igual todos los casos de seno.
    for (double hz : {110.0, 220.0, 440.0, 880.0, 1320.0}) {
        for (int rate : kRates) {
            const std::vector<float> s = sine(hz, rate, 1.0);
            const double fs = fundamentalHz(s, 0, s.size(), rate, hz);
            ASSERT_GT(fs, 0.0) << "seno " << hz << " Hz a " << rate
                               << ": el estimador se nego a medir (codigo " << fs << ")";
            EXPECT_LT(std::abs(cents(fs, hz)), 0.1)
                << "seno puro de " << hz << " Hz a " << rate << " Hz medido en "
                << fs << " Hz (" << cents(fs, hz) << " cents).\n"
                << "  El instrumento perdio exactitud: cualquier medicion de "
                << "afinacion de esta suite deja de ser confiable.";
        }
    }

    for (double hz : {110.0, 220.0, 440.0, 880.0}) {
        for (int rate : kRates) {
            const std::vector<float> p = pluck(hz, rate, 1.0);
            const size_t from = static_cast<size_t>(0.1 * rate);
            const size_t len = static_cast<size_t>(0.5 * rate);
            const double fp = fundamentalHz(p, from, len, rate, hz);
            ASSERT_GT(fp, 0.0) << "pluck " << hz << " Hz a " << rate;
            EXPECT_LT(std::abs(cents(fp, hz)), 0.1)
                << "pluck de " << hz << " Hz (2do armonico dominante) a " << rate
                << " Hz medido en " << fp << " Hz (" << cents(fp, hz) << " cents).";
        }
    }
}

TEST(EnginePitch, TheEstimatorRefusesToGuessWhenThePeakIsOutsideTheBand) {
    // La regla que costo dos estimadores: una medida pegada al borde del barrido
    // NO es una medida, y el estimador tiene que decirlo en vez de entregar el
    // borde disfrazado de resultado.
    //
    // EL CASO HAY QUE ELEGIRLO CON CUIDADO, y el primer intento estuvo mal: un
    // seno de 440 buscado alrededor de 150 Hz devuelve 146,67 y eso es CORRECTO
    // — 146,67 Hz son 327 muestras, o sea exactamente TRES periodos de 440, y la
    // ACF de una señal periodica tiene un maximo legitimo ahi. El estimador no
    // se equivoco; el caso de prueba si.
    //
    // Buscar alrededor de 1.000 Hz si sirve: la banda va de 769 a 1.300 Hz, o
    // sea lags 37 a 62, y no hay ningun multiplo de los 109,09 samples del
    // periodo ahi adentro. Sin pico interior, el maximo cae en el borde.
    //
    // Lo que esto acota es preciso, y conviene no leerlo de mas: detecta un pico
    // PEGADO AL LIMITE, no cualquier banda mal elegida. Una banda que contenga
    // un submultiplo va a devolver ese submultiplo, y eso es inherente a la
    // autocorrelacion — por eso el llamador pasa una banda de +-30 %.
    const std::vector<float> s = sine(440.0, 48000, 1.0);
    const double f = fundamentalHz(s, 0, s.size(), 48000, 1000.0);

    EXPECT_LT(f, 0.0)
        << "el estimador devolvio " << f << " Hz para un seno de 440 buscado "
        << "alrededor de 1.000, donde no hay ningun periodo suyo.\n"
        << "  Tiene que devolver un valor NEGATIVO cuando el pico cae en el "
        << "limite del barrido. Si entrega el borde como resultado, un rango mal "
        << "elegido se lee como una medicion y los tests de abajo pueden quedar "
        << "verdes midiendo el limite en vez del motor.";
}

// ===========================================================================
// PASO 2 — el motor. La causa, que es lo que le sirve a quien lo arregle.
// ===========================================================================

TEST(EnginePitch, TheKarplusLoopIsLongerThanItAsksFor) {
    // ESTE es el test con valor de diagnostico. No dice "esta desafinado", dice
    // CUANTO sobra en el lazo — y ese numero es lo que hay que descontar.
    //
    // El lazo deberia medir fs/f muestras. Si mide fs/f + D con D constante, el
    // error relativo es D*f/fs: crece con la nota y baja con el rate, que es
    // exactamente el patron de los dos sintomas.
    std::printf("\n%-8s %-9s %-11s %-11s %s\n", "rate", "nota", "ideal", "medido", "D (muestras)");
    double worstD = 0.0;
    for (int rate : kRates) {
        for (float target : {110.0f, 220.0f, 440.0f}) {
            const double f = karplusFundamental(rate, rate, target);
            ASSERT_GT(f, 0.0) << "no se pudo medir " << target << " Hz a " << rate;
            const double idealLag = rate / static_cast<double>(target);
            const double actualLag = rate / f;
            const double d = actualLag - idealLag;
            std::printf("%-8d %-9.1f %-11.3f %-11.3f %.3f\n", rate, target, idealLag,
                        actualLag, d);
            worstD = std::max(worstD, std::abs(d));
        }
    }

    // Medido al escribir esto: D entre 6,8 y 7,2 muestras, estable en los nueve
    // casos. El trinquete va sobre 8: si alguien compensa el lazo esto baja a
    // ~0 y hay que apretar la cota; si alguien agrega otra etapa al lazo sin
    // descontarla, sube y el test avisa.
    EXPECT_LT(worstD, 8.0)
        << "el lazo de Karplus-Strong sobra " << worstD << " muestras sobre las "
        << "fs/f que pide.\n"
        << "  Son el retardo de grupo del filtro del lazo mas el del "
        << "interpolador. Mientras no se descuenten, la cuerda suena BAJA, y "
        << "tanto mas cuanto mas aguda la nota.";
    EXPECT_GT(worstD, 1.0)
        << "el lazo ya no sobra muestras (D = " << worstD << "). Si alguien lo "
        << "compenso: bajar la cota de arriba a lo que mida ahora y vaciar "
        << "pitch-baseline.txt — los dos sintomas se arreglan juntos.";
}

TEST(EnginePitch, TheFundamentalDoesNotShiftBetweenSampleRates) {
    // EL CRITERIO DE WD-2.3.2, literal. Hoy falla, y la deuda esta declarada.
    const std::set<std::string> baseline = readBaseline(WMA_PITCH_BASELINE);
    std::set<std::string> failing;

    for (float target : {110.0f, 220.0f, 440.0f}) {
        double f[3];
        for (int i = 0; i < 3; ++i) {
            f[i] = karplusFundamental(kRates[i], kRates[i], target);
            ASSERT_GT(f[i], 0.0) << "no se pudo medir " << target << " Hz a " << kRates[i];
        }
        double worst = 0.0;
        for (int a = 0; a < 3; ++a) {
            for (int b = a + 1; b < 3; ++b) {
                worst = std::max(worst, std::abs(cents(f[a], f[b])));
            }
        }
        // 5 cents: el umbral en el que un oido entrenado empieza a notar una
        // diferencia de afinacion. Por debajo de eso, "no se corre" es cierto
        // en el unico sentido que le importa al producto.
        if (worst > 5.0) failing.insert("KARPLUS_STRONG");
    }

    for (const std::string& name : failing) {
        EXPECT_TRUE(baseline.count(name) > 0)
            << name << " corre su fundamental al cambiar de sample rate y NO "
            << "esta en pitch-baseline.txt.\n"
            << "  Es el criterio de WD-2.3.2: la misma nota tiene que sonar "
            << "igual de afinada en un device a 44,1 y en uno a 96.";
    }
    for (const std::string& name : baseline) {
        EXPECT_TRUE(failing.count(name) > 0)
            << name << " esta en pitch-baseline.txt pero YA mantiene su "
            << "fundamental entre rates. Si lo arreglaste, borra su linea.";
    }
}

// ===========================================================================
// PASO 3 — lo que suena el defecto de WD-3.4, medido en vez de estimado.
// ===========================================================================

TEST(EnginePitch, AStaleSampleRatePlaysTheStringFlatNotSharp) {
    // `SynthEngineDispatcher` prepara dieciseis engines con `prepare(48000,
    // 4096)` LITERAL, y `onStreamConfigChanged` no los vuelve a preparar. En un
    // device que negocia 44,1 kHz, la cuerda queda preparada para 48.
    //
    // WD-3.4 dice que eso la deja "8,8 % sharp — 1,5 semitonos". **La direccion
    // esta al reves.** Medido contra una instancia bien preparada, con las dos
    // reproducidas a 44,1: sale BAJA, ~7,9 %, ~1,4 semitonos.
    //
    // El razonamiento, para que no haya que volver a medirlo: el lazo se
    // dimensiona en MUESTRAS como fs_preparado/f. Con fs_preparado mas grande
    // que el real, el lazo tiene MAS muestras de las que corresponden, tarda MAS
    // en dar la vuelta, y un periodo mas largo es una frecuencia mas BAJA.
    for (float target : {220.0f, 440.0f}) {
        const double good = karplusFundamental(44100, 44100, target);
        const double stale = karplusFundamental(48000, 44100, target);
        ASSERT_GT(good, 0.0);
        ASSERT_GT(stale, 0.0);

        const double shift = cents(stale, good);
        EXPECT_LT(shift, -100.0)
            << "con el rate stale la nota de " << target << " Hz quedo en "
            << shift << " cents respecto de la bien preparada (" << good
            << " Hz -> " << stale << " Hz).\n"
            << "  Se esperaba MAS DE UN SEMITONO ABAJO. Si ahora sale arriba, o "
            << "casi igual, el defecto cambio de forma y hay que volver a "
            << "medirlo antes de tocar WD-3.4.";
        EXPECT_GT(shift, -200.0) << "el corrimiento es mayor de lo medido; re-medir";
    }
}
