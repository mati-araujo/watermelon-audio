/**
 * test_touch_expression.cpp — REQ-008 S1: expresión por toque en SoundFont.
 *
 * ## Por qué todos estos tests comparan MUESTRAS
 *
 * Es el criterio de muerte del REQ, y no es una preferencia de estilo. `touch.velocity` se
 * escribía en `SoundFontEngine` desde hacía meses y **nadie la leía**: el motor aceptaba un
 * nivel y no lo aplicaba, y las dos superficies del consumidor creían estar cambiando el
 * volumen. Un test que setea y después llama al getter habría dado eso por bueno.
 *
 * Por eso acá no hay un solo `EXPECT_EQ(engine.getExpression(...), x)`. Todo se afirma sobre
 * audio renderizado offline.
 *
 * ## Dos trampas de método que este archivo respeta a propósito
 *
 * 1. **Instancia fresca por render.** Medido en el spike del REQ: `tsf_reset` NO deja pizarra
 *    limpia — dos renders idénticos sobre el mismo `tsf` divergían 1,6e-01 entre sí, y ese
 *    número parecía una propiedad de TSF que no existe. Cada `render()` de acá arma su propio
 *    manager y su propio engine.
 * 2. **"Suena" nunca es `> 0`.** El piso de silencio del motor mide ~1e-5, así que un umbral
 *    laxo deja pasar tests que comparan silencio contra silencio. `kAudible` es explícito, y
 *    hay un test que no afirma nada más que eso — si se cae, todo lo de abajo es ruido.
 */

#include "support/MinimalSoundFont.h"

#include "engines/SoundFontEngine.h"
#include "engines/SoundFontManager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using wma_test::makeMinimalSoundFont;

constexpr int kSampleRate = 48000;
constexpr int kNote = 60;
constexpr int kOtherNote = 64;
constexpr float kVelocity = 0.9f;
constexpr int kTotalFrames = 24000;   // 0,5 s
constexpr int kGestureFrame = 4800;   // 0,1 s: bien pasado el ataque

/** El piso de silencio del motor mide ~1e-5. Esto es "sonó", con margen. */
constexpr double kAudible = 0.005;

/** Un evento de expresión: en qué frame se manda y con qué valor. */
struct Gesture {
    int atFrame;
    float value;
    int touchId = 0;
};

/** Un `noteOn` diferido, para el test del reset. */
struct LateNote {
    int atFrame;
    int midiNote;
    int touchId = 0;
    float velocity = kVelocity;
};

/** Un `noteOff` diferido, para separar una nota de la siguiente sin que se solapen. */
struct LateOff {
    int atFrame;
    int touchId = 0;
};

struct Scenario {
    int blockFrames = 256;
    std::vector<Gesture> gestures{};
    std::vector<LateNote> lateNotes{};
    std::vector<LateOff> lateOffs{};
    /// Toques extra que arrancan junto con el 0, para probar aislamiento entre toques.
    std::vector<int> extraTouches{};
    /// Índices que el test manda a propósito fuera de rango o a un toque inactivo.
    std::vector<Gesture> strayGestures{};
};

/**
 * Renderiza el escenario con un motor RECIÉN construido y devuelve el canal izquierdo.
 *
 * El font se genera en memoria (`makeMinimalSoundFont`), así que el test es hermético: no
 * baja corpus ni depende de ningún `.sf2` del disco. Medido: ese font rinde RMS ~0,016 con
 * pico 0,38, o sea que **suena** y sirve para comparar niveles.
 */
std::vector<float> render(const Scenario& sc) {
    // `looping=true`: el one-shot dura 4 ms, menos que los 240 frames que tarda en
    // converger el suavizador. Sin loop no hay test de nivel posible.
    // REQ-040: sin sends. El default #8 (CC91 -> reverb, en reset) le pondria +6,3 % de reverb a
    // la nota, y la COLA de la reverb recuerda el gesto: dos renders que difieren solo en un gesto
    // ya pasado dejan de ser identicos muestra a muestra aunque el dry lo sea. Este archivo mide
    // la expresion por toque sobre el dry, asi que el font borra el default #8 (amount 0, misma
    // identidad: SF2 §8.4).
    wma_test::sf2::ModulatorPlacement sinSends;
    wma_test::sf2::Modulator borraDefaultOcho;
    borraDefaultOcho.srcOper = wma_test::sf2::srcOper(91, true, false, false, 0);
    borraDefaultOcho.destOper = 16;   // reverbEffectsSend
    borraDefaultOcho.amount = 0;
    sinSends.instrumentGlobal.push_back(borraDefaultOcho);
    auto sf2 = makeMinimalSoundFont(kSampleRate, /*looping=*/true, -1, -1, sinSends);
    auto manager = std::make_unique<SoundFontManager>();
    if (!manager->loadFromMemory(sf2.data(), static_cast<int>(sf2.size()), kSampleRate)) {
        return {};
    }
    SoundFontEngine engine;
    engine.setSoundFontManager(manager.get());
    engine.prepare(kSampleRate, sc.blockFrames);
    engine.noteOn(0, kNote, kVelocity);
    // Los toques extra entran MUCHO mas bajo a proposito: con todos parejos, bajar el
    // toque 0 y bajar el 1 dan la misma energia total, y el test no puede distinguir
    // 'se aplico al mio' de 'se aplico al de al lado'. Medido con el mutante M6.
    for (int tid : sc.extraTouches) engine.noteOn(tid, kOtherNote, kVelocity * 0.25f);

    std::vector<float> stereo(static_cast<size_t>(kTotalFrames) * 2, 0.0f);
    int done = 0;
    while (done < kTotalFrames) {
        const int n = std::min(sc.blockFrames, kTotalFrames - done);
        const int blockEnd = done + n;

        for (const auto& g : sc.gestures) {
            if (g.atFrame >= done && g.atFrame < blockEnd) {
                engine.setTouchExpression(g.touchId, g.value);
            }
        }
        for (const auto& g : sc.strayGestures) {
            if (g.atFrame >= done && g.atFrame < blockEnd) {
                engine.setTouchExpression(g.touchId, g.value);
            }
        }
        for (const auto& lo : sc.lateOffs) {
            if (lo.atFrame >= done && lo.atFrame < blockEnd) {
                engine.noteOff(lo.touchId);
            }
        }
        for (const auto& ln : sc.lateNotes) {
            if (ln.atFrame >= done && ln.atFrame < blockEnd) {
                engine.noteOn(ln.touchId, ln.midiNote, ln.velocity);
            }
        }

        engine.render(stereo.data() + static_cast<size_t>(done) * 2, n);
        done = blockEnd;
    }

    std::vector<float> mono(kTotalFrames);
    for (int i = 0; i < kTotalFrames; ++i) {
        mono[i] = stereo[static_cast<size_t>(i) * 2];
    }
    return mono;
}

double rms(const std::vector<float>& v, int from = 0, int to = -1) {
    if (to < 0) to = static_cast<int>(v.size());
    if (to <= from) return 0.0;
    double acc = 0.0;
    for (int i = from; i < to; ++i) acc += static_cast<double>(v[i]) * v[i];
    return std::sqrt(acc / (to - from));
}

double worstDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double w = 0.0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        w = std::max(w, std::fabs(static_cast<double>(a[i]) - b[i]));
    }
    return w;
}

class TouchExpressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        wma::setLogCallback([](wma::LogLevel, const char*, const char*) {});
    }
    void TearDown() override { wma::setLogCallback(nullptr); }
};

// ===========================================================================
// El fixture, antes que nada
// ===========================================================================

/**
 * Si esto falla, TODO lo de abajo es ruido: estaría comparando silencio contra silencio,
 * que es como seis de los ocho tests de REQ-007 pasaron sobre nada hasta que alguien miró.
 */
TEST_F(TouchExpressionTest, TheFixtureActuallyMakesSound) {
    const auto out = render({});
    ASSERT_FALSE(out.empty()) << "el font mínimo no cargó";
    EXPECT_GT(rms(out), kAudible)
        << "el fixture no suena — el piso de silencio del motor mide ~1e-5";
}

/** Y el control de que el harness no miente: la misma configuración contra sí misma. */
TEST_F(TouchExpressionTest, TheHarnessIsDeterministic) {
    EXPECT_DOUBLE_EQ(worstDiff(render({}), render({})), 0.0)
        << "dos renders idénticos difieren: el harness arrastra estado y nada de "
           "lo que mida abajo significa algo";
}

// ===========================================================================
// AC-008.4 — en el neutro, el audio es el de siempre
// ===========================================================================

/**
 * Mandar la expresión en su valor neutro tiene que dar el MISMO audio que no mandarla nunca.
 *
 * Es lo que hace que la expresión por toque no le cambie el sonido a ningún consumidor que
 * no la use — incluida la migración al API por canal que trajo esta etapa, que es el cambio
 * con más riesgo de regresión silenciosa de todo el REQ.
 */
TEST_F(TouchExpressionTest, NeutralExpressionIsBitIdenticalToNeverSendingIt) {
    const auto sinExpresion = render({});
    Scenario conNeutro;
    conNeutro.gestures = {{kGestureFrame, 1.0f}};
    const auto conExpresion = render(conNeutro);

    ASSERT_GT(rms(sinExpresion), kAudible);
    EXPECT_DOUBLE_EQ(worstDiff(sinExpresion, conExpresion), 0.0)
        << "el neutro cambió el audio: dejó de ser un multiplicador puro";
}

// ===========================================================================
// AC-008.1 — cambia el nivel SIN volver a atacar
// ===========================================================================

/**
 * 🔴 EL test. Con la nota sonando, bajar la expresión a la mitad tiene que dar
 * **la misma onda escalada**, no una nota nueva.
 *
 * Que sea una copia escalada es lo que prueba las dos mitades a la vez: que el nivel cambió
 * (si no, el ratio daría 1) y que **no hubo re-ataque** (un `note_on` nuevo reiniciaría la
 * envolvente y la señal dejaría de ser proporcional a la referencia, muestra por muestra).
 */
TEST_F(TouchExpressionTest, HalvingExpressionScalesTheSoundingNoteWithoutReattacking) {
    const auto referencia = render({});
    Scenario aMitad;
    aMitad.gestures = {{kGestureFrame, 0.5f}};
    const auto bajada = render(aMitad);

    // Bien después de la convergencia del smoother (5 ms = 240 frames).
    const int desde = kGestureFrame + 2400;
    const double rmsRef = rms(referencia, desde);
    const double rmsBaj = rms(bajada, desde);
    ASSERT_GT(rmsRef, kAudible) << "la referencia no suena; el test no mide nada";

    const double ratio = rmsBaj / rmsRef;
    EXPECT_NEAR(ratio, 0.5, 0.05)
        << "la expresión no llevó el nivel a la mitad (ratio=" << ratio << ")";

    // La prueba del NO re-ataque: proporcional muestra por muestra, no sólo en RMS.
    double peorDesvio = 0.0;
    for (int i = desde; i < kTotalFrames; ++i) {
        peorDesvio = std::max(peorDesvio,
                              std::fabs(static_cast<double>(bajada[i]) - 0.5 * referencia[i]));
    }
    EXPECT_LT(peorDesvio, 0.01)
        << "la onda no es la misma escalada — hubo re-ataque o la envolvente se movió";
}

// ===========================================================================
// AC-008.3 — un NOTE_ON reinicia la expresión
// ===========================================================================

/**
 * Una nota nueva en el mismo toque no puede heredar el nivel del gesto anterior.
 *
 * Sin el reset, el músico baja la expresión con un dedo, levanta, y la nota siguiente sale
 * atenuada sin que nada lo explique.
 */
TEST_F(TouchExpressionTest, ANewNoteOnTheSameTouchStartsAtNeutralImmediately) {
    constexpr int kOffFrame = 8000;
    constexpr int kNuevaNota = 14000;   // bien despues de que la anterior se libero
    // La ventana ES la rampa del suavizador (5 ms = 240 frames). Medir despues no sirve:
    // el mutante que borra el `reset` rampea de 0,2 a 1,0 y a los 2400 frames ya convergio,
    // asi que una ventana tardia lo deja pasar. Medido con el control de mutacion.
    constexpr int kVentana = 240;

    Scenario limpio;
    limpio.lateOffs = {{kOffFrame}};
    limpio.lateNotes = {{kNuevaNota, kOtherNote}};
    const auto sinGesto = render(limpio);

    Scenario conGestoPrevio;
    conGestoPrevio.gestures = {{kGestureFrame, 0.2f}};
    conGestoPrevio.lateOffs = {{kOffFrame}};
    conGestoPrevio.lateNotes = {{kNuevaNota, kOtherNote}};
    const auto conGesto = render(conGestoPrevio);

    // La nota vieja bajo de nivel: si esto no se ve, el gesto no hizo nada y el test de
    // abajo no probaria nada.
    ASSERT_LT(rms(conGesto, kGestureFrame + 2400, kOffFrame),
              rms(sinGesto, kGestureFrame + 2400, kOffFrame) * 0.5)
        << "el gesto previo no bajo el nivel; no hay nada de que recuperarse";

    ASSERT_GT(rms(sinGesto, kNuevaNota, kNuevaNota + kVentana), kAudible);
    const double ratio = rms(conGesto, kNuevaNota, kNuevaNota + kVentana) /
                         rms(sinGesto, kNuevaNota, kNuevaNota + kVentana);
    EXPECT_NEAR(ratio, 1.0, 0.05)
        << "la nota nueva no arranco en el neutro: entro rampeando desde el nivel del "
           "gesto anterior (ratio=" << ratio << ")";
}

/**
 * R-MOT-13 + R-MOT-14, juntas, porque un consumidor las pisa juntas: un NOTE_ON con la
 * MISMA nota sobre un toque activo NO vuelve a atacar y su velocity NO cambia la nota
 * que suena (`drainEvents` solo dispara si la nota difiere), pero SI reinicia la
 * expresion del toque a 1,0 — el reset de R-MOT-14 corre para todo NOTE_ON, no solo
 * para los que atacan.
 *
 * Nacio de #4 de la respuesta de NoisyPad a v2.17.0 (2026-09-12): mandaban 16 escalones
 * de velocity en un arrastre con la misma nota y "nunca cambiaron la ganancia"; el KDoc
 * de `sfNoteOn` decia "arranca o actualiza", que sugiere lo contrario. Este test es lo
 * que el KDoc corregido afirma.
 *
 * El observable es MUESTRA A MUESTRA contra la nota que nunca recibio ni gesto ni
 * reenvio, y no un RMS: medido, la base da 6e-8 (un ulp). Un RMS no alcanzaba — el
 * mutante que re-ataca con la misma nota SUMA una segunda voz a la que sigue sonando
 * (tsf no mata la anterior), y con la velocity 0,3 elegida el RMS quedo en 0,92 de la
 * referencia: muere por 0,026 sobre la tolerancia, o sea por la fase. Muestra a muestra
 * la segunda voz es una diferencia del orden de la señal, con cualquier fase. Y si el
 * reset no corriera para la misma nota, queda el 0,2 del gesto: tambien del orden de la
 * señal.
 */
TEST_F(TouchExpressionTest, ARepeatedNoteOnWithTheSameNoteKeepsTheAttackAndResetsExpression) {
    constexpr int kRepeat = 14000;
    constexpr float kOtraVelocity = 0.3f;   // distinta a proposito: un re-ataque la delataria

    Scenario limpio;   // sin gesto y sin reenvio: la nota como nacio
    const auto referencia = render(limpio);

    Scenario reenvio;
    reenvio.gestures = {{kGestureFrame, 0.2f}};
    reenvio.lateNotes = {{kRepeat, kNote, 0, kOtraVelocity}};
    const auto conReenvio = render(reenvio);

    // El gesto bajo el nivel antes del reenvio: sin esto, "volvio al neutro" no prueba nada.
    ASSERT_LT(rms(conReenvio, kGestureFrame + 2400, kRepeat),
              rms(referencia, kGestureFrame + 2400, kRepeat) * 0.5)
        << "el gesto previo no bajo el nivel; no hay nada de que recuperarse";

    // Despues del reenvio, la nota ORIGINAL en el neutro, muestra a muestra. El nivel por
    // toque entra una vez por bloque (R-MOT-16), de ahi el margen de un bloque largo.
    constexpr int kDesde = kRepeat + 512;
    constexpr int kHasta = kRepeat + 4800;
    ASSERT_GT(rms(referencia, kDesde, kHasta), kAudible);
    const std::vector<float> a(conReenvio.begin() + kDesde, conReenvio.begin() + kHasta);
    const std::vector<float> b(referencia.begin() + kDesde, referencia.begin() + kHasta);
    const double diff = worstDiff(a, b);
    EXPECT_LT(diff, 1e-5)
        << "un NOTE_ON repetido con la misma nota tiene que dejar la nota como estaba y la "
           "expresion en el neutro (peor diferencia muestra a muestra=" << diff << ")";
}

// ===========================================================================
// AC-008.5 — lo que hay que ignorar, se ignora
// ===========================================================================

/**
 * El CONTROL NEGATIVO de la superficie pública: un toque inactivo y un índice fuera de rango
 * no pueden hacer nada, y tampoco crashear.
 *
 * Sin esto, una implementación que aplicara la expresión al toque equivocado —o que
 * escribiera fuera del array— pasaría todos los tests de arriba.
 */
TEST_F(TouchExpressionTest, ExpressionOnAnInactiveOrOutOfRangeTouchIsIgnored) {
    const auto referencia = render({});

    Scenario extraviados;
    extraviados.strayGestures = {
        {kGestureFrame, 0.1f, /*touchId=*/5},                        // inactivo
        {kGestureFrame, 0.1f, /*touchId=*/-1},                       // fuera de rango
        {kGestureFrame, 0.1f, SoundFontEngine::MAX_TOUCHES},         // fuera de rango
        {kGestureFrame, 0.1f, SoundFontEngine::MAX_TOUCHES + 99},    // bien fuera
    };
    const auto conExtraviados = render(extraviados);

    ASSERT_GT(rms(referencia), kAudible);
    EXPECT_DOUBLE_EQ(worstDiff(referencia, conExtraviados), 0.0)
        << "un gesto que no le corresponde a ningún toque activo cambió el audio";
}

/**
 * 🔴 El gesto afecta **sólo a su toque**.
 *
 * Este test existe porque el de arriba NO alcanza, y eso se supo midiendo: el control de
 * mutación mostró que quitarle el guard de "toque activo" al evento **no mata ningún test**.
 * Y no puede matarlo — un toque inactivo no tiene voces, así que aplicarle expresión no
 * suena distinto por más mal que esté. Lo que el guard protege de verdad es el índice, y
 * eso se ve con DOS toques sonando: si el gesto se aplicara al toque equivocado —o a todos—
 * el toque 1 cambiaría de nivel.
 *
 * Con esto, AC-008.5 deja de descansar en una afirmación que no puede fallar.
 */
TEST_F(TouchExpressionTest, AGestureOnlyAffectsItsOwnTouch) {
    Scenario dosToques;
    dosToques.extraTouches = {1};
    const auto ambosEnNeutro = render(dosToques);

    Scenario soloElCero;
    soloElCero.extraTouches = {1};
    soloElCero.gestures = {{kGestureFrame, 0.25f}};
    const auto conGesto = render(soloElCero);

    const int desde = kGestureFrame + 2400;
    const double base = rms(ambosEnNeutro, desde);
    ASSERT_GT(base, kAudible);

    // El toque 0 domina la energia (el extra entra a un cuarto de velocity), asi que bajarlo
    // a 0,25 tiene que producir una caida GRANDE. Las tres cotas cierran tres huecos:
    //   · si el gesto se derramara a todos, la caida seria aun mayor  -> cota inferior
    //   · si fuera al toque de al lado, apenas se notaria             -> cota superior
    //   · si no llegara, no habria caida                              -> la misma superior
    const double ratio = rms(conGesto, desde) / base;
    EXPECT_GT(ratio, 0.20)
        << "bajar el toque 0 se llevo tambien al resto (ratio=" << ratio << ")";
    EXPECT_LT(ratio, 0.60)
        << "el gesto no bajo al toque 0: o no llego, o se aplico a otro toque (ratio="
        << ratio << ")";
}

// ===========================================================================
// AC-008.7 — el suavizado se mide en MUESTRAS, no en llamadas
// ===========================================================================

/**
 * El mismo gesto con dos tamaños de bloque tiene que converger en el mismo tiempo REAL.
 *
 * Es el defecto de WD-3.4 al revés: allá 5 ms se volvían 2,56 s con bloques de 512 porque el
 * suavizado avanzaba **por llamada**. Acá el smoother avanza `numFrames` de una vez, así que
 * los 5 ms son 5 ms con cualquier bloque.
 *
 * ⚠️ Lo que este test NO pide es invariancia EXACTA: aplicar por bloque un valor que cambia
 * tiene la granularidad del bloque, y medido da 3,9e-03 de divergencia entre 512 y 128. El
 * AC se escribió con ese número en la mano.
 */
TEST_F(TouchExpressionTest, SmoothingTimeIsMeasuredInSamplesNotInCalls) {
    auto convergencia = [](int blockFrames) {
        Scenario sc;
        sc.blockFrames = blockFrames;
        sc.gestures = {{kGestureFrame, 0.25f}};
        const auto out = render(sc);
        Scenario ref;
        ref.blockFrames = blockFrames;
        const auto base = render(ref);

        // Primer frame, después del gesto, en el que el nivel local ya bajó al 90 % del
        // camino hacia el objetivo. Se mide sobre ventanas cortas para no depender de la fase.
        constexpr int kVentana = 240;
        for (int i = kGestureFrame; i + kVentana < kTotalFrames; i += kVentana) {
            const double r = rms(base, i, i + kVentana);
            if (r < 1e-6) continue;
            const double ratio = rms(out, i, i + kVentana) / r;
            if (ratio <= 0.25 + 0.10 * 0.75) return i;
        }
        return kTotalFrames;
    };

    const int c512 = convergencia(512);
    const int c128 = convergencia(128);
    ASSERT_LT(c512, kTotalFrames) << "con bloques de 512 nunca convergió";
    ASSERT_LT(c128, kTotalFrames) << "con bloques de 128 nunca convergió";

    EXPECT_LE(std::abs(c512 - c128), 512)
        << "el tiempo de convergencia depende del TAMAÑO del bloque: "
        << c512 << " vs " << c128 << " frames — es el defecto de WD-3.4";
}

// ===========================================================================
// AC-008.2 — no clickea
// ===========================================================================

/**
 * Un gesto brusco no puede producir un salto en el borde de bloque más grande que la propia
 * pendiente de la onda.
 *
 * La medida es **auto-calibrante** —el mayor salto en los bordes contra el mayor salto
 * adentro— y no un umbral fijo, por la razón que dejó escrita REQ-007: un umbral fijo mide el
 * ARRANQUE de la nota, no el cambio que se quiere vigilar.
 *
 * El gesto es de 1,0 a 0,0 **de un golpe**, que es el peor caso: el barrido suave del spike
 * no clickeaba ni sin suavizador, así que afirmarlo con un barrido no probaría nada.
 */
TEST_F(TouchExpressionTest, AnAbruptGestureDoesNotClick) {
    constexpr int kBlock = 256;
    Scenario sc;
    sc.blockFrames = kBlock;
    sc.gestures = {{kGestureFrame, 0.0f}};
    const auto out = render(sc);
    ASSERT_GT(rms(out, 0, kGestureFrame), kAudible);

    double peorEnBorde = 0.0, peorAdentro = 0.0;
    for (int i = kGestureFrame + 1; i < kTotalFrames; ++i) {
        const double salto = std::fabs(static_cast<double>(out[i]) - out[i - 1]);
        if (i % kBlock == 0) peorEnBorde = std::max(peorEnBorde, salto);
        else                 peorAdentro = std::max(peorAdentro, salto);
    }

    ASSERT_GT(peorAdentro, 0.0) << "no hay señal para calibrar contra";
    EXPECT_LE(peorEnBorde, peorAdentro)
        << "el borde de bloque salta más que la onda misma: eso es un clic ("
        << peorEnBorde << " vs " << peorAdentro << ")";
}

}  // namespace
