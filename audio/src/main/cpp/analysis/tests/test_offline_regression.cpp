/**
 * test_offline_regression.cpp — REQ-015 S3: los dos defectos de REQ-014,
 * corriendo SOBRE EL PUERTO.
 *
 * POR QUE ESTE ARCHIVO EXISTE
 * ---------------------------
 * Los dos defectos que REQ-014 arreglo se encontraron **a mano**: un telefono,
 * una guitarra, un parlante y una persona punteando. El puerto offline se compro
 * para que esa regresion la pueda correr cualquiera —incluido el consumidor, que
 * no puede compilar los tests de host de este repo— y un puerto sin llamador es
 * el mecanismo sin llamador. Este archivo es el llamador.
 *
 * 🔴 LOS TESTS VIVOS DE REQ-014 NO SE TOCAN, Y ESO ES DELIBERADO
 * ---------------------------------------------------------------
 * `test_sign_outside_range.cpp` y `test_silence_gate.cpp` siguen ahi, corriendo
 * por el thread real con su ring y su jitter. Esto AGREGA cobertura; moverla
 * seria cambiar el chequeo del camino que shippea por uno mas rapido, que es el
 * mismo error que reemplazar el TSan del CI por el local.
 *
 * Lo que este archivo agrega y aquellos no pueden dar:
 *
 *   - **velocidad**, y no es comodidad: medido, el caso del signo cuesta 13 ms
 *     aca contra 27,7 s del barrido vivo. Eso es lo que vuelve viable barrer 44
 *     señales grabadas en vez de tres puntos.
 *   - **determinismo**: no hay un solo `waitUntil` en este archivo. El andamio
 *     vivo necesita DOS por bloque; aca no hay a que esperarle.
 *
 * 🔴 LO QUE ESTE ARCHIVO NO PUEDE PROBAR, Y HAY QUE SABERLO
 * ---------------------------------------------------------
 * El puerto entrega **un** snapshot: el ultimo. Los invariantes de REGIMEN de
 * REQ-014 —el parpadeo, y sobre todo AC-014.5, donde el snapshot que dice "sin
 * señal" trayendo un numero **dura 3 ticks de 40**— no son expresables asi. Su
 * propio KDoc lo dice: *"un test que afirme sobre el estado final no lo ve"*.
 * Esos se quedan en el camino vivo, que es donde existe el jitter que los
 * produce. No se agrego superficie de secuencia porque no tiene consumidor.
 *
 * 🔴 EL ESTIMULO ES PARTE DE LA ASERCION
 * ---------------------------------------
 * Con parciales ARMONICOS el defecto del signo es **inalcanzable**: medido en
 * REQ-014, 0 inversiones en 103 puntos de barrido. El ingrediente que falta es
 * la INARMONICIDAD, que es lo que hace una cuerda de verdad. Por eso el estimulo
 * lleva `B = 1e-3` y por eso 3.1 afirma primero su PREMISA — que la deteccion
 * gruesa siga quedando optimista— antes de afirmar la propiedad: sin esa
 * premisa el test seguiria verde midiendo un escenario que ya no reproduce nada.
 */

#include "../AnalysisSnapshot.h"
#include "../OfflineAnalysis.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

using namespace wma::analysis;

constexpr double kE4 = 329.6276;

// --- el escenario del SIGNO (REQ-014 S2 · AC-014.3) -------------------------

/// 48000 porque de ahi sale el rango util de 30,50 cents con el que la guarda
/// de REQ-003 se compara. Cambiarlo cambia el escenario, no el estilo.
constexpr int kSignRate = 48000;

/// Inarmonicidad de una cuerda de acero real. 🔴 Con B = 0 el defecto es
/// INALCANZABLE — medido en 103 puntos.
constexpr double kSteelB = 1.0e-3;

/// La cuerda del reporte de campo: 35 cents ABAJO. El motor publicaba +27,53.
constexpr double kReportedCents = -35.0;

/// Adentro del rango util, para el gemelo. Con esto el motor TIENE que hablar.
constexpr double kInsideRangeCents = -10.0;

// --- el escenario de la COMPUERTA (REQ-014 S1 · AC-014.1 / AC-014.2) --------

constexpr int kGateRate = 44100;

/// El piso de ruido ambiente MEDIDO sobre hardware en la habitacion del reporte.
/// NO es una constante del motor: es un dato del escenario.
constexpr double kReportedRoomNoiseRms = 0.0070;

/// Amplitudes de los dos gemelos, calcadas de REQ-014 porque ahi estan medidas:
/// 0,010 sobre el ruido del reporte da rms 0,010237 con la gruesa en 332,0 Hz, y
/// 0,002 en habitacion silenciosa da rms 0,001649 con `CONVERGED` 50 de 50.
constexpr double kAudibleStringAmp = 0.010;
constexpr double kQuietRoomStringAmp = 0.002;

constexpr int kPluckFrames = 44100;   // 1,0 s de pua: la PRECONDICION
constexpr int kTailFrames  = 61740;   // 1,4 s de lo que venga despues

double detune(double ref, double cents) { return ref * std::pow(2.0, cents / 1200.0); }

// --- generadores -------------------------------------------------------------

/// `fn = n·f0·sqrt(1 + B·n²)`. Con B = 0 es la cuerda armonica.
double stringSample(double f0, double B, long frame, int rate, double amp) {
    double s = 0.0;
    for (int n = 1; n <= 4; ++n) {
        const double fn = f0 * n * std::sqrt(1.0 + B * static_cast<double>(n) * n);
        s += (amp / n) * std::sin(2.0 * M_PI * fn * static_cast<double>(frame) / rate);
    }
    return s;
}

/// Buffer entero de una cuerda, estereo intercalado. La fase es continua por
/// construccion: se genera el buffer completo de una, no bloque por bloque.
std::vector<float> stringBuffer(double f0, double B, int frames, int rate, double amp) {
    std::vector<double> mono(static_cast<size_t>(frames), 0.0);
    for (int i = 0; i < frames; ++i) mono[static_cast<size_t>(i)] = stringSample(f0, B, i, rate, amp);
    std::vector<float> b(mono.size() * 2, 0.0f);
    for (size_t i = 0; i < mono.size(); ++i) {
        const float v = static_cast<float>(mono[i]);
        b[i * 2]     = v;
        b[i * 2 + 1] = v;
    }
    return b;
}

struct Pink {
    unsigned st = 987654321u;
    double b0 = 0.0, b1 = 0.0, b2 = 0.0;
    double next() {
        st = st * 1664525u + 1013904223u;
        const double w = static_cast<double>(st >> 8) / 8388608.0 - 1.0;
        b0 = 0.99765 * b0 + w * 0.0990460;
        b1 = 0.96300 * b1 + w * 0.2965164;
        b2 = 0.57000 * b2 + w * 1.0526913;
        return (b0 + b1 + b2 + w * 0.1848) * 0.2;
    }
};

struct White {
    unsigned st = 4242u;
    double next() {
        st = st * 1664525u + 1013904223u;
        return static_cast<double>(st >> 8) / 8388608.0 - 1.0;
    }
};

/// Los cuatro modelos de habitacion de REQ-014. 🔴 El zumbido de red no es un
/// adorno: da una altura de 50 Hz con claridad 0,994, asi que una compuerta que
/// solo preguntara "¿hay altura?" pasaria el caso del ruido blanco y dejaria
/// vivo el `MEASURING` eterno en la mitad de las habitaciones reales.
enum RoomKind { kWhite = 0, kPink = 1, kPinkPlusHum = 2, kHumDominant = 3 };

std::string roomName(int kind) {
    switch (kind) {
        case kWhite:       return "White";
        case kPink:        return "Pink";
        case kPinkPlusHum: return "PinkPlusHum";
        default:           return "HumDominant";
    }
}

/// Interleaved estereo a partir de una señal mono, sin tocar el nivel.
std::vector<float> toStereo(const std::vector<double>& mono) {
    std::vector<float> b(mono.size() * 2, 0.0f);
    for (size_t i = 0; i < mono.size(); ++i) {
        const float v = static_cast<float>(mono[i]);
        b[i * 2]     = v;
        b[i * 2 + 1] = v;
    }
    return b;
}

/**
 * La grabacion que hace un musico: puntea, el motor mide, y despues suelta.
 *
 * El `pluck` inicial no es relleno — es la PRECONDICION de todo lo que sigue.
 * Sin haber medido primero, "dejar de publicar" seria trivial: un motor apagado
 * pasaria todos los tests de ausencia. Aca esa precondicion viaja DENTRO del
 * mismo buffer, que es exactamente como se graba en la vida real.
 *
 * 🔴 EL ORDEN DE LOS DOS INGREDIENTES DE LA COLA NO ES INTERCAMBIABLE, Y ME
 * COSTO UN ROJO. El RUIDO se escala al rms del escenario —porque el escenario se
 * especifica por NIVEL, que es lo que el reporte midio— y la CUERDA se suma
 * despues, a su amplitud, sin escalar. Al reves —escalando la suma— el ruido
 * crudo tapa la cuerda y la gruesa no engancha nada (medido: 0 Hz), asi que el
 * test de "no la apagues" fallaba por el estimulo y no por el motor.
 *
 * @param noise      generador de habitacion; se escala a @p noiseRms.
 * @param noiseRms   rms al que se lleva el ruido. 0 = habitacion silenciosa.
 * @param stringAmp  amplitud de la cuerda que sigue sonando en la cola. 0 = el
 *                   musico solto de verdad.
 */
template <typename Noise>
std::vector<float> pluckThen(Noise noise, double noiseRms, double stringAmp) {
    std::vector<double> raw(static_cast<size_t>(kPluckFrames + kTailFrames), 0.0);
    for (int i = 0; i < kPluckFrames; ++i) {
        raw[static_cast<size_t>(i)] = stringSample(kE4, 0.0, i, kGateRate, 0.5);
    }

    // El ruido primero y SOLO, para poder llevarlo a su nivel.
    std::vector<double> tail(static_cast<size_t>(kTailFrames), 0.0);
    double sq = 0.0;
    for (int i = 0; i < kTailFrames; ++i) {
        const double v = noise(static_cast<long>(kPluckFrames) + i);
        tail[static_cast<size_t>(i)] = v;
        sq += v * v;
    }
    if (noiseRms > 0.0) {
        const double have = std::sqrt(sq / kTailFrames);
        const double k = have > 0.0 ? noiseRms / have : 0.0;
        for (double& v : tail) v *= k;
    }

    // Y RECIEN AHORA la cuerda, a su amplitud.
    for (int i = 0; i < kTailFrames; ++i) {
        const long t = static_cast<long>(kPluckFrames) + i;
        if (stringAmp > 0.0) {
            tail[static_cast<size_t>(i)] += stringSample(kE4, 0.0, t, kGateRate, stringAmp);
        }
        raw[static_cast<size_t>(t)] = tail[static_cast<size_t>(i)];
    }
    return toStereo(raw);
}

/// Un generador de habitacion por modelo. Con estado propio, para que dos
/// llamadas den la misma señal: el determinismo del puerto no sirve de nada si
/// el estimulo no lo es.
struct Room {
    Pink pink;
    White white;
    int kind;
    explicit Room(int k) : kind(k) {}
    double operator()(long t) {
        const double ph = 2.0 * M_PI * 50.0 * static_cast<double>(t) / kGateRate;
        switch (kind) {
            case kWhite: return white.next();
            case kPink:  return pink.next();
            case kPinkPlusHum:
                return pink.next()
                       + 0.5 * (std::sin(ph) + 0.5 * std::sin(2 * ph) + 0.33 * std::sin(3 * ph));
            default:
                return 0.2 * pink.next()
                       + std::sin(ph) + 0.6 * std::sin(2 * ph) + 0.4 * std::sin(3 * ph);
        }
    }
};

// --- lectura -----------------------------------------------------------------

struct Reading {
    bool ok = false;
    int state = -1;
    double cents = NAN;
    double coarseCents = NAN;
    double detectedHz = 0.0;
    double rms = 0.0;
    bool published = false;
};

Reading analyze(const std::vector<float>& buf, int frames, int rate, double target) {
    Reading r;
    float v[kSnapshotValueCount];
    r.ok = analyzeBuffer(buf.data(), frames, rate, target, v);
    if (!r.ok) return r;
    r.state = static_cast<int>(v[kSnapState]);
    r.cents = static_cast<double>(v[kSnapCents]);
    r.published = !std::isnan(r.cents);
    r.detectedHz = static_cast<double>(v[kSnapDetectedHz]);
    r.rms = static_cast<double>(v[kSnapLevelRms]);
    if (r.detectedHz > 0.0) r.coarseCents = 1200.0 * std::log2(r.detectedHz / target);
    return r;
}

// ---------------------------------------------------------------------------
// 3.1 / 3.2 — AC-014.3: el signo publicado no puede contradecir a la realidad
// ---------------------------------------------------------------------------

/**
 * 🔴 EL CASO EXACTO DEL REPORTE, POR EL PUERTO. E4 con la cuerda 35 cents ABAJO.
 *
 * Antes de REQ-014 el motor publicaba **+27,53 cents, y como CONVERGED**: le
 * decia al musico que aflojara una cuerda que hay que apretar, con confianza.
 * Medido de nuevo aca revirtiendo el arreglo: sale el mismo +27,53.
 *
 * La PREMISA se afirma antes que la propiedad, y no es ceremonia: el defecto
 * existe porque la deteccion gruesa queda OPTIMISTA con una cuerda inarmonica
 * —informa −28,5 con la cuerda a −35— y su magnitud entra en el rango util, asi
 * que la guarda declara "en dominio" mientras el fundamental esta afuera y
 * vuelve aliasado. Si algun dia la gruesa dejara de quedar corta, este test
 * seguiria verde **sin poder fallar**, y eso es lo que la premisa impide.
 */
TEST(OfflineRegression, TheReportedStringNeverPublishesTheOppositeSign) {
    constexpr int kFrames = 51200;   // ~1,07 s a 48 kHz
    const auto buf = stringBuffer(detune(kE4, kReportedCents), kSteelB, kFrames, kSignRate, 0.5);

    const Reading r = analyze(buf, kFrames, kSignRate, kE4);
    ASSERT_TRUE(r.ok) << "el puerto no analizo nada";

    // --- la premisa del escenario ---
    ASSERT_GT(r.detectedHz, 0.0) << "la gruesa no engancho: el escenario ya no es el del defecto";
    EXPECT_GT(r.coarseCents, kReportedCents)
        << "la deteccion gruesa YA NO queda optimista (informa " << r.coarseCents
        << " con la cuerda a " << kReportedCents << "): sin eso este test no puede fallar "
           "aunque el defecto vuelva";

    // --- la propiedad ---
    if (r.published) {
        EXPECT_LT(r.cents, 0.0)
            << "publico " << r.cents << " cents con la cuerda " << kReportedCents
            << " cents ABAJO: le dice al musico que afloje lo que hay que apretar";
    }
    EXPECT_NE(r.state, kStateConverged)
        << "declaro CONVERGED sobre una lectura que la guarda no pudo verificar";
}

/**
 * EL GEMELO, y sin el 3.1 no afirma nada.
 *
 * Callarse siempre satisface "nunca publiques el signo opuesto". Lo unico que
 * vuelve afirmacion a 3.1 es exigir, con la misma fuerza, que el motor SI hable
 * cuando la cuerda esta dentro del rango util — con la MISMA cuerda inarmonica,
 * porque si el gemelo usara un estimulo mas facil no estaria vigilando el mismo
 * camino.
 */
TEST(OfflineRegression, InsideTheUsableRangeTheInharmonicStringStillPublishes) {
    constexpr int kFrames = 51200;
    const auto buf =
        stringBuffer(detune(kE4, kInsideRangeCents), kSteelB, kFrames, kSignRate, 0.5);

    const Reading r = analyze(buf, kFrames, kSignRate, kE4);
    ASSERT_TRUE(r.ok);

    EXPECT_TRUE(r.published)
        << "se callo con la cuerda a " << kInsideRangeCents << " cents, que esta DENTRO del "
           "rango util: la garantia del signo se pago con enmudecer el afinador";
    if (r.published) {
        EXPECT_LT(r.cents, 0.0) << "publico " << r.cents << " con la cuerda abajo";
    }
}

/**
 * El desplazamiento del parcial mas agudo que sigue el strobe, en cents.
 *
 * De `fn = n·f0·sqrt(1 + B·n²)` con n = 4. Es cuanto puede correr la lectura
 * HACIA SOSTENIDO por la rigidez de la cuerda, sin que nadie se equivoque: los
 * parciales de una cuerda real estan mas arriba que sus armonicos ideales.
 */
double maxInharmonicBiasCents(double B) {
    return 1200.0 * std::log2(std::sqrt(1.0 + B * 16.0));
}

/**
 * 🔴 EL BARRIDO QUE EL CAMINO VIVO NO PUEDE PAGAR.
 *
 * `SignOutsideRange.NoDeviationEverPublishesTheOppositeSign` hace 12 puntos en
 * **27,7 segundos**, y ese numero es el techo real de lo que el camino vivo
 * puede permitirse: cada punto arranca un thread, alimenta el ring en tandas y
 * espera por condicion dos veces por bloque. Aca cada punto cuesta ~12 ms, asi
 * que la pregunta deja de ser "¿cuantos puntos entran?" y pasa a ser "¿donde la
 * propiedad es CIERTA?".
 *
 * 🔴 Y ESA PREGUNTA TIENE RESPUESTA MEDIDA, PORQUE UN BARRIDO UNIFORME LA
 * RESPONDIO MAL PRIMERO
 * ----------------------------------------------------------------------------
 * La primera version barria de −60 a +60 con paso 5 y reporto **una inversion
 * en el arbol sano**: a −5 cents con `B = 1e-3` el motor publicaba **+2,7557**.
 * No era el defecto de REQ-014 —eso vive en el borde del rango util, no cerca de
 * cero— sino el SESGO FISICO de la inarmonicidad, y la cuenta lo cierra sin
 * lugar a dudas:
 *
 *     sesgo del 3er parcial a B = 1e-3 = 1200·log2(sqrt(1 + 9·B)) = +7,754 c
 *     −5,000 + 7,754                                              = +2,754 c
 *     el motor publico                                              +2,7557 c
 *
 * Dos milesimas de cent de diferencia. O sea que **el `cents` nominal deja de
 * ser el sustituto de la verdad** cuando el sesgo lo supera: ahi el motor no
 * miente, mide una cuerda cuyos parciales estan arriba del objetivo. Es la misma
 * leccion que REQ-014 S2 —"la inarmonicidad rompe el sustituto"— aplicada al
 * test en vez de al motor, y por eso el barrido vivo se concentra en el borde y
 * NO barre cerca de cero.
 *
 * El limite se DERIVA de la fisica en vez de copiarse como una lista de puntos:
 * se afirma el signo solo donde la desviacion nominal es mayor que el sesgo
 * maximo posible para esa B. Con `B = 0` eso es todo el rango; con `B = 1e-3`
 * son 13,74 cents para arriba.
 *
 * Los ejes son tres y ninguno es decorativo:
 *   - la DESVIACION, a ambos lados y bien afuera del rango util (que a 48 kHz es
 *     de 30,50 cents): el defecto vive JUSTO en el borde;
 *   - la INARMONICIDAD, porque con `B = 0` el defecto es inalcanzable y con
 *     `B = 1e-3` aparece — o sea que un barrido de una sola B no barre nada;
 *   - el SIGNO, porque la guarda se apoya en la magnitud de la gruesa y una
 *     asimetria ahi es exactamente por donde reentra.
 *
 * 🔴 Y EL BARRIDO TRAE SU PROPIO GEMELO ADENTRO. Un motor apagado no publica
 * ninguna inversion, asi que "cero inversiones" solo significa algo si ademas se
 * exige que los puntos DENTRO del rango util hayan publicado. Sin eso el barrido
 * mas denso del mundo lo pasa un `return NaN`.
 */
TEST(OfflineRegression, NoDeviationEverPublishesTheOppositeSign) {
    constexpr int kFrames = 51200;
    constexpr double kInsideUsableRange = 20.0;   // holgado contra los 30,50 c
    // Margen sobre el sesgo, para no afirmar justo en el filo donde el nominal y
    // el sesgo se cancelan.
    constexpr double kBiasMargin = 1.0;

    int inversions = 0;
    int publishedInside = 0;
    int pointsInside = 0;
    int points = 0;
    int skipped = 0;

    for (double B : {0.0, 1.0e-4, kSteelB}) {
        const double bias = maxInharmonicBiasCents(B);
        for (double cents = -60.0; cents <= 60.0 + 1e-9; cents += 2.5) {
            // Donde el sesgo puede tapar a la desviacion, el nominal NO es el
            // sustituto de la verdad y afirmar el signo seria afirmar sobre el
            // generador, no sobre el motor.
            if (std::fabs(cents) <= bias + kBiasMargin) { ++skipped; continue; }
            ++points;

            const auto buf = stringBuffer(detune(kE4, cents), B, kFrames, kSignRate, 0.5);
            const Reading r = analyze(buf, kFrames, kSignRate, kE4);
            ASSERT_TRUE(r.ok) << "el puerto no analizo el punto " << cents << " c, B = " << B;

            const bool inside = std::fabs(cents) <= kInsideUsableRange;
            if (inside) ++pointsInside;
            if (!r.published) continue;
            if (inside) ++publishedInside;

            if ((cents < 0.0 && r.cents > 0.0) || (cents > 0.0 && r.cents < 0.0)) {
                ++inversions;
                ADD_FAILURE() << "signo invertido en " << cents << " cents (B = " << B
                              << "): el motor publico " << r.cents
                              << " (sesgo maximo por inarmonicidad: " << bias << " c)";
            }
        }
    }

    EXPECT_EQ(inversions, 0)
        << inversions << " inversiones en " << points << " puntos evaluados ("
        << skipped << " salteados por sesgo)";

    // El gemelo del barrido: sin esto, callarse siempre da cero inversiones.
    EXPECT_GT(publishedInside, pointsInside * 3 / 4)
        << "solo " << publishedInside << " de " << pointsInside
        << " puntos DENTRO del rango util publicaron: el barrido esta verde porque el "
           "motor se callo, no porque acierte el signo";
}

// ---------------------------------------------------------------------------
// 3.3 / 3.4 — AC-014.1 y AC-014.2: la ausencia se declara, y no se compra con
//             sensibilidad
// ---------------------------------------------------------------------------

class OfflineRoomNoise : public ::testing::TestWithParam<int> {};

/**
 * La grabacion termina en una habitacion vacia: el veredicto es AUSENCIA.
 *
 * Antes de REQ-014 los cuatro modelos de ruido publicaban `kStateMeasuring`
 * **para siempre** — un spinner eterno que le promete al usuario que el numero
 * esta por llegar, en una habitacion donde no hay nada que medir. Medido de
 * nuevo aca revirtiendo el arreglo: `state` = 2.
 */
TEST_P(OfflineRoomNoise, TheRoomAloneIsDeclaredAbsent) {
    Room room(GetParam());
    const auto buf = pluckThen(room, kReportedRoomNoiseRms, 0.0);   // el musico solto

    const Reading r = analyze(buf, kPluckFrames + kTailFrames, kGateRate, kE4);
    ASSERT_TRUE(r.ok);

    EXPECT_EQ(r.state, kStateNoSignal)
        << "con la habitacion vacia (" << roomName(GetParam()) << ") el veredicto fue "
        << r.state << " y no ausencia (gruesa: " << r.detectedHz << " Hz)";
    EXPECT_FALSE(r.published)
        << "declaro un estado y publico un numero a la vez (AC-014.5)";
}

INSTANTIATE_TEST_SUITE_P(FourRoomModels, OfflineRoomNoise,
                         ::testing::Values(kWhite, kPink, kPinkPlusHum, kHumDominant),
                         [](const ::testing::TestParamInfo<int>& i) {
                             return roomName(i.param);
                         });

/**
 * 🔴 EL GEMELO QUE HACE QUE EL DE ARRIBA SIGNIFIQUE ALGO.
 *
 * Callar siempre satisface cualquier test de "no publiques basura". El unico
 * modo de que "declara ausencia" sea una afirmacion es exigir que NO la declare
 * con una cuerda todavia sonando — y esta, a amp 0,010 sobre el ruido del
 * reporte, es audible y el motor la media bien (`CONVERGED` 40 de 40).
 */
TEST(OfflineRegression, ADecayingStringOverRoomNoiseIsNotDeclaredAbsent) {
    Room room(kPink);
    const auto buf = pluckThen(room, kReportedRoomNoiseRms, kAudibleStringAmp);

    const Reading r = analyze(buf, kPluckFrames + kTailFrames, kGateRate, kE4);
    ASSERT_TRUE(r.ok);

    EXPECT_NE(r.state, kStateNoSignal)
        << "apago el afinador con la cuerda todavia sonando (gruesa: " << r.detectedHz << " Hz)";
    EXPECT_TRUE(r.published)
        << "la ausencia se pago con sensibilidad: dejo de publicar una lectura que antes daba";
}

/**
 * El segundo gemelo, y el que un piso ABSOLUTO no puede pasar.
 *
 * Esta cuerda esta **por debajo** del ruido de la habitacion del reporte (rms
 * 0,001649 contra 0,0070), y aun asi el motor la mide bien. Que este test y el
 * de la habitacion vacia esten los dos en verde es la prueba de que la
 * compuerta NO es de nivel: ningun umbral puede declarar ausencia sobre 0,0070 y
 * seguir midiendo 0,0016 — estan 4,4x separados en la direccion imposible.
 */
TEST(OfflineRegression, TheQuietRoomKeepsMeasuringWellBelowTheReportedNoiseFloor) {
    // Habitacion silenciosa: sin ruido, y la cuerda POR DEBAJO del piso del
    // reporte (rms medido 0,001649 contra 0,0070).
    const auto buf = pluckThen([](long) { return 0.0; }, 0.0, kQuietRoomStringAmp);

    const Reading r = analyze(buf, kPluckFrames + kTailFrames, kGateRate, kE4);
    ASSERT_TRUE(r.ok);

    EXPECT_NE(r.state, kStateNoSignal)
        << "apago una cuerda audible en una habitacion silenciosa (rms " << r.rms
        << "): la compuerta volvio a ser de nivel";
    EXPECT_TRUE(r.published);
}

}  // namespace
