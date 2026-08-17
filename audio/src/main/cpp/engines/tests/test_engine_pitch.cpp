/**
 * WD-2.3.2 / WD-3.4.3 — la afinacion de Karplus-Strong, a 44,1 / 48 / 96 kHz.
 *
 * DE DONDE VENIA EL SEMITONO, Y POR QUE LA PRIMERA EXPLICACION ERA FALSA
 * ---------------------------------------------------------------------
 * La version anterior de este archivo medía que el lazo sobraba `D ~= 7`
 * muestras y lo atribuia al "retardo de grupo del filtro del lazo mas el del
 * interpolador". **La cuenta no daba**: con `brightness` en su default el
 * one-pole aporta `(1-a)/a = 0,905` muestras y el interpolador lineal aporta
 * CERO — su retardo en DC es exactamente el que se le pide. Faltaban ~6
 * muestras sin explicar.
 *
 * No eran del lazo. La prueba que lo zanja esta abajo, en
 * `TheTuningDoesNotDependOnTheBlockSize`: con el codigo viejo, el MISMO rate y
 * la MISMA nota daban D distinto segun el tamaño del bloque —
 *
 *      bloque       16      64     256     512    1024
 *      D a 440 Hz  1,33    3,36    6,08    7,08    7,58
 *      cents      -22,9   -57,1  -102,0  -118,1  -126,3
 *
 * — y un retardo de lazo no puede depender de eso. La causa era
 * `SynthEngine::smoothParam()`: leia el parametro una vez por BLOQUE pero
 * avanzaba el smoother una sola MUESTRA, asi que los 5 ms de suavizado
 * declarados eran 2,56 s con bloques de 512 y `brightness` valia ~0,02 en vez
 * de 0,5 durante todo el primer medio segundo. Con la cuerda casi oscura el
 * one-pole aporta 7,5 muestras, no 0,9. Ver
 * `ParameterSmoother::processBlock()` y sus tests.
 *
 * Lo que quedaba despues de eso —el 0,905 de verdad— si habia que compensarlo,
 * y se compensa con el retardo de FASE del filtro a la frecuencia pedida. No
 * con el de grupo en DC: los dos coinciden cerca del default, pero con la
 * cuerda oscura el de grupo dice 9,0 muestras y el de fase 8,06, y descontar 9
 * dejaba la nota 16 cents ALTA.
 *
 * POR QUE EL INSTRUMENTO TIENE SU PROPIO TEST
 * -------------------------------------------
 * Porque tres estimadores distintos devolvieron numeros plausibles y falsos
 * antes de que este midiera bien — ver el encabezado de `PitchHarness.h`. Un
 * test de afinacion cuyo estimador no esta validado no mide el motor: mide el
 * estimador, y no avisa cual de los dos fallo.
 *
 * Y UNA ADVERTENCIA SOBRE LA VENTANA, QUE COSTO SEIS CENTS DE CONFUSION
 * --------------------------------------------------------------------
 * Con el `decay` por defecto la cuerda de 440 Hz se apaga y el auto-retrigger
 * la vuelve a puntear cada ~0,25 s. Una ventana de analisis que CONTIENE una
 * re-excitacion lee ~6 cents alta: medido, la misma señal da 0,09-0,45 cents en
 * ventanas limpias y 6,22 en una que cruza un re-pluck. Por eso los tests de
 * exactitud usan `decay = 1,0` — cola larga, cero re-excitaciones adentro de la
 * ventana — y lo dicen en vez de esconderlo.
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

/// El bloque de referencia de esta suite. Los tests que barren tamaños de
/// bloque lo pasan explicito.
constexpr int kBlock = 512;

/// `decay` que deja la cuerda sonando toda la ventana de analisis. No es un
/// numero de conveniencia: con el default la cuerda se re-puntea adentro de la
/// ventana y la autocorrelacion lee el re-pluck (~6 cents), no el lazo.
constexpr float kLongTail = 1.0f;

/// Renderiza `seconds` de un engine y devuelve el canal izquierdo.
std::vector<float> renderLeft(SynthEngine& e, int rate, double seconds, float hz,
                              int block = kBlock) {
    const int frames = static_cast<int>(seconds * rate);
    std::vector<float> buf(static_cast<size_t>(block) * 2, 0.0f);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(frames));
    for (int done = 0; done < frames; done += block) {
        const int n = std::min(block, frames - done);
        e.process(buf.data(), n, hz, 0.8f);
        for (int i = 0; i < n; ++i) out.push_back(buf[static_cast<size_t>(i) * 2]);
    }
    return out;
}

/// Karplus-Strong recien preparado, con `brightness` y `decay` a eleccion.
std::vector<float> renderKarplus(int prepareRate, double seconds, float targetHz,
                                 float decay, float brightness = 0.5f,
                                 int block = kBlock) {
    KarplusStrongEngine e;
    e.setParameter(KarplusStrongEngine::PARAM_BRIGHTNESS, brightness);
    e.setParameter(KarplusStrongEngine::PARAM_DECAY, decay);
    e.prepare(prepareRate, block);
    e.reset();
    return renderLeft(e, prepareRate, seconds, targetHz, block);
}

/// La fundamental de un Karplus-Strong recien preparado al rate pedido.
/// Se saltean 100 ms de ataque y se miden los 400 ms siguientes: la excitacion
/// no es periodica y arrastra la estimacion si entra en la ventana.
double karplusFundamental(int prepareRate, int playbackRate, float targetHz,
                          float decay = kLongTail, float brightness = 0.5f) {
    const std::vector<float> x =
        renderKarplus(prepareRate, 1.0, targetHz, decay, brightness);
    const size_t from = static_cast<size_t>(0.1 * playbackRate);
    const size_t len = static_cast<size_t>(0.4 * playbackRate);
    return fundamentalHz(x, from, len, playbackRate, targetHz);
}

/// Cuanto tarda el lazo en caer 60 dB, en segundos, calculado — no medido — a
/// partir de la ganancia por vuelta. Sirve para separar "esta desafinado" de
/// "no hay tono que afinar": con la cuerda del todo oscura el polo se come el
/// lazo y a 880 Hz el tono dura 18 ms.
double loopT60Seconds(float hz, int rate, float brightness, float decay) {
    const double a = 0.1 + static_cast<double>(brightness) * 0.85;
    const double pole = 1.0 - a;
    const double w = 2.0 * kPi * hz / rate;
    const double lp = a / std::hypot(1.0 - pole * std::cos(w), pole * std::sin(w));
    const double perTrip = lp * (0.9 + static_cast<double>(decay) * 0.099);
    if (perTrip >= 1.0) return 1e9;
    return std::log(0.001) / (hz * std::log(perTrip));
}

/// RMS de una ventana de `seconds` que arranca en `t`.
double rmsAt(const std::vector<float>& x, int rate, double t, double seconds) {
    const size_t from = static_cast<size_t>(t * rate);
    const size_t len = static_cast<size_t>(seconds * rate);
    if (from + len > x.size()) return -1.0;
    double sum = 0.0;
    for (size_t i = from; i < from + len; ++i) {
        sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    }
    return std::sqrt(sum / static_cast<double>(len));
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

    // Y el regimen que usa el barrido de brillo: tonos que se mueren rapido,
    // medidos en ventanas cortas. Sin este control no se puede decir de quien
    // son los ~10 cents que quedan ahi — si de la compensacion del lazo o de la
    // autocorrelacion sobre una señal que cae 60 dB en 100 ms. Medido: del
    // motor, porque aca el estimador da <= 0,04 cents.
    for (double hz : {440.0, 880.0}) {
        for (int rate : kRates) {
            for (double t60 : {0.018, 0.103, 0.42}) {
                const double minLen = 4.0 * kBand / hz;
                const double len = std::max(minLen, std::min(0.4, 0.30 * t60));
                const double from = std::max(0.005, 0.06 * t60);
                const std::vector<float> d = damped(hz, rate, t60, 1.0);
                const double fd = fundamentalHz(d, static_cast<size_t>(from * rate),
                                                static_cast<size_t>(len * rate), rate, hz);
                ASSERT_GT(fd, 0.0) << "amortiguada " << hz << " Hz a " << rate
                                   << " con t60 " << t60;
                EXPECT_LT(std::abs(cents(fd, hz)), 0.2)
                    << "senoide de " << hz << " Hz que cae 60 dB en " << t60
                    << " s, a " << rate << " Hz, medida en " << fd << " ("
                    << cents(fd, hz) << " cents).\n"
                    << "  Si el estimador se sesga sobre tonos que se mueren "
                    << "rapido, la cota amortiguada de "
                    << "TheStringStaysInTuneAcrossItsBrightnessRange deja de "
                    << "poder atribuirse al motor.";
            }
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

TEST(EnginePitch, TheKarplusLoopMeasuresWhatItAsksFor) {
    // EL CRITERIO DE WD-3.4.3: exactitud en cents, no solo invariancia entre
    // rates. Un motor puede estar igual de desafinado a los tres rates y pasar
    // el criterio de WD-2.3.2 sin sonar afinado en ninguno — que es justo lo
    // que pasaba con las tres notas mas graves.
    //
    // El lazo tiene que medir `fs/f` muestras contando TODO lo que retarda,
    // incluido el filtro. `D` es lo que sobra.
    std::printf("\n%-8s %-9s %-11s %-11s %-11s %s\n", "rate", "nota", "ideal", "medido",
                "D (muestras)", "cents");
    double worstD = 0.0;
    double worstCents = 0.0;
    for (int rate : kRates) {
        for (float target : {82.41f, 110.0f, 220.0f, 440.0f, 880.0f}) {
            const double f = karplusFundamental(rate, rate, target);
            ASSERT_GT(f, 0.0) << "no se pudo medir " << target << " Hz a " << rate;
            const double idealLag = rate / static_cast<double>(target);
            const double d = rate / f - idealLag;
            std::printf("%-8d %-9.2f %-11.3f %-11.3f %-11.4f %.3f\n", rate, target, idealLag,
                        rate / f, d, cents(f, target));
            worstD = std::max(worstD, std::abs(d));
            worstCents = std::max(worstCents, std::abs(cents(f, target)));
        }
    }

    // Medido al compensar: |D| <= 0,059 muestras y |cents| <= 0,44 en los
    // quince casos. Las cotas van con margen ~3x sobre lo medido; el error que
    // este test existe para atrapar valia 118 cents y 7,1 muestras.
    EXPECT_LT(worstD, 0.2)
        << "al lazo de Karplus-Strong le sobran " << worstD << " muestras sobre "
        << "las fs/f que pide. Mientras no se descuenten, la cuerda suena "
        << "desafinada y ademas se corre con el sample rate.";
    EXPECT_LT(worstCents, 1.5)
        << "la nota mas desafinada quedo a " << worstCents << " cents de lo "
        << "pedido. El umbral en el que un oido entrenado nota una diferencia "
        << "son 5; esto tiene que quedar MUY por debajo, porque el error del "
        << "instrumento sobre señales de pitch conocido es de 0,03 cents.";
}

TEST(EnginePitch, TheStringStaysInTuneAcrossItsBrightnessRange) {
    // La compensacion NO es una constante: vale el retardo de FASE del filtro a
    // la frecuencia pedida, que va de 0,05 muestras con la cuerda brillante a
    // 8-9 con la oscura. Un arreglo con una constante fija pasa el test de
    // arriba y muere aca.
    //
    // LA VENTANA SE ADAPTA AL TONO, y no es un detalle: con `brightness` en 0 a
    // 880 Hz el tono cae 60 dB en 18 ms, asi que la ventana fija de 400 ms del
    // otro test promedia sobre todo su silencio y devuelve el borde del barrido.
    // Aca la ventana entra en el primer decaimiento.
    //
    // DOS COTAS, PORQUE HAY DOS REGIMENES, y las dos estan medidas:
    //
    //   - Lazo casi sin perdidas (t60 >= 0,5 s, que es TODO lo que se toca con
    //     el brillo por defecto): la afinacion es exacta. Peor caso 0,44 cents.
    //
    //   - Lazo muy amortiguado (t60 < 0,5 s): quedan hasta 9,66 cents. Eso NO es
    //     el estimador —`TheEstimatorIsExactOnSignalsOfKnownPitch` lo verifica
    //     sobre senoides amortiguadas con los mismos t60 y las mismas ventanas,
    //     y da <= 0,04 cents— sino un limite de la compensacion: la condicion de
    //     fase se evalua sobre la circunferencia unidad, y con la ganancia del
    //     lazo en 0,86 el polo esta bien adentro. Corregir eso pide resolver el
    //     radio del polo en el thread de audio; no vale lo que cuesta.
    //
    // La cota de 12 no es holgura elegida: descontar el retardo de GRUPO en DC
    // en vez del de fase da **102,9 cents** en el mismo barrido. Es lo que
    // separa las dos formulas.
    double worstSustained = 0.0;
    double worstDamped = 0.0;
    int sustained = 0;
    int damped_ = 0;
    for (int rate : kRates) {
        for (float target : {110.0f, 440.0f, 880.0f}) {
            for (float brightness : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
                const double t60 = loopT60Seconds(target, rate, brightness, kLongTail);
                // El estimador necesita al menos dos veces su lag maximo; se le
                // dan cuatro. Y la ventana no puede pasar del render.
                const double minLen = 4.0 * kBand / target;
                const double len = std::max(minLen, std::min(0.4, 0.30 * t60));
                const double from = std::max(0.005, 0.06 * t60);
                if (from + len > 0.95) continue;

                const std::vector<float> x =
                    renderKarplus(rate, 1.0, target, kLongTail, brightness);
                const double f = fundamentalHz(x, static_cast<size_t>(from * rate),
                                               static_cast<size_t>(len * rate), rate, target);
                ASSERT_GT(f, 0.0) << target << " Hz a " << rate << " con brightness "
                                  << brightness << " (t60 " << t60 << " s): el estimador "
                                  << "se nego a medir (codigo " << f << ")";
                const double off = std::abs(cents(f, target));
                if (t60 >= 0.5) {
                    worstSustained = std::max(worstSustained, off);
                    ++sustained;
                } else {
                    worstDamped = std::max(worstDamped, off);
                    ++damped_;
                }
            }
        }
    }
    // Que el barrido siga cubriendo LOS DOS regimenes. Si uno se vacia, la cota
    // que lo cubre deja de medir nada y el test se vuelve verde por ausencia.
    ASSERT_GE(sustained, 15) << "el barrido se quedo sin casos de lazo sostenido";
    ASSERT_GE(damped_, 5) << "el barrido se quedo sin casos de lazo amortiguado";

    EXPECT_LT(worstSustained, 1.5)
        << "con el lazo sostenido la nota mas desafinada quedo a " << worstSustained
        << " cents. Ahi la compensacion tiene que ser exacta.";
    EXPECT_LT(worstDamped, 12.0)
        << "con la cuerda muy amortiguada la nota se fue a " << worstDamped
        << " cents. Medido con la compensacion correcta: 9,66. Con el retardo de "
        << "GRUPO en DC en vez del de FASE: 102,9 — si esto salto asi, revisar "
        << "que la compensacion siga siguiendo al polo y no a una constante.";
}

TEST(EnginePitch, TheTuningDoesNotDependOnTheBlockSize) {
    // EL TEST QUE HABRIA CAZADO TODO ESTO. El defecto original no era del lazo:
    // era `smoothParam()` avanzando una muestra por bloque, asi que la misma
    // nota al mismo rate sonaba distinta segun el tamaño de bloque que
    // negociara el device — 22,9 cents con bloques de 16 y 126,3 con 1024.
    //
    // Se pide lo mas fuerte que se puede pedir y que hoy es cierto: la salida
    // es IDENTICA muestra a muestra. Vale mientras los parametros esten
    // quietos; con un parametro moviendose, `powf(c, n)` y n multiplicaciones
    // sueltas difieren en el ultimo bit y habria que pedir igualdad aproximada.
    for (int rate : kRates) {
        for (float target : {110.0f, 440.0f}) {
            const std::vector<float> reference =
                renderKarplus(rate, 0.5, target, 0.5f, 0.5f, kBlock);
            for (int block : {16, 64, 128, 1024}) {
                const std::vector<float> got =
                    renderKarplus(rate, 0.5, target, 0.5f, 0.5f, block);
                ASSERT_EQ(reference.size(), got.size());
                size_t differing = 0;
                size_t firstDiff = 0;
                for (size_t i = 0; i < reference.size(); ++i) {
                    if (reference[i] != got[i]) {
                        if (differing == 0) firstDiff = i;
                        ++differing;
                    }
                }
                EXPECT_EQ(differing, 0u)
                    << target << " Hz a " << rate << " Hz suena distinto con "
                    << "bloques de " << block << " que con bloques de " << kBlock
                    << " (" << differing << " muestras distintas, la primera en "
                    << firstDiff << ").\n"
                    << "  El tamaño del bloque lo negocia el device: si el "
                    << "sonido depende de el, depende del telefono. Este es el "
                    << "modo de falla que dejo a Karplus-Strong un semitono "
                    << "bajo — revisar quien lee parametros por bloque sin "
                    << "avanzar el smoother el bloque entero.";
            }
        }
    }
}

TEST(EnginePitch, TheStringKeepsSoundingInsteadOfGoingSilent) {
    // El auto-retrigger existe para que la cuerda siga sonando mientras el dedo
    // esta en el pad ("bowed string feel"). Su contador vivia AFUERA del loop
    // de muestras, asi que contaba bloques: el chequeo que dice "cada ~50 ms"
    // caia cada `rate/20` BLOQUES — 25,6 s con bloques de 512 a 44,1 kHz.
    //
    // Medido con el defecto vivo: la cuerda de 440 Hz daba RMS 0,0039 a los
    // 50 ms y EXACTAMENTE 0 desde los ~0,4 s, durante los 12 s siguientes.
    // Karplus-Strong era, en la practica, un engine mudo.
    for (int rate : kRates) {
        for (float target : {110.0f, 440.0f}) {
            for (int block : {64, kBlock}) {
                const std::vector<float> x =
                    renderKarplus(rate, 10.0, target, 0.5f, 0.5f, block);
                for (double t : {0.5, 3.0, 9.0}) {
                    const double rms = rmsAt(x, rate, t, 0.1);
                    // Medido: el minimo sobre los 36 casos es 0,00185. La cota
                    // va un factor ~4 abajo; lo que atrapa es el silencio, que
                    // valia 0,000000.
                    EXPECT_GT(rms, 0.0005)
                        << "a los " << t << " s la cuerda de " << target << " Hz a "
                        << rate << " Hz (bloques de " << block << ") esta en RMS "
                        << rms << ": se apago y no volvio.\n"
                        << "  El auto-retrigger tiene que re-puntearla cuando la "
                        << "energia cae. Si su contador volvio a contar bloques "
                        << "en vez de muestras, el periodo del chequeo escala con "
                        << "el tamaño del bloque y esto se pone rojo.";
                }
            }
        }
    }
}

TEST(EnginePitch, TheFundamentalDoesNotShiftBetweenSampleRates) {
    // EL CRITERIO DE WD-2.3.2, literal.
    const std::set<std::string> baseline = readBaseline(WMA_PITCH_BASELINE);
    std::set<std::string> failing;

    for (float target : {82.41f, 110.0f, 220.0f, 440.0f, 880.0f}) {
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
        // Medido tras compensar: 0,07 a 0,39 cents.
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
