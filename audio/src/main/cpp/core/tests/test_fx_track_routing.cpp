// ===========================================================================
// REQ-007 S1 — una pista marcada pasa por la cadena de efectos.
//
// POR QUÉ ESTOS TESTS Y NO OTROS
// ------------------------------
// El criterio de muerte del REQ nombra la clase de defecto que hay que atrapar:
// "un control de nivel/ruteo que el motor acepta y no aplica". Un test que
// setea el flag y después llama al getter NO la atrapa — es exactamente lo que
// dejó pasar `touch.velocity` en SoundFontEngine, escrita durante meses sin que
// nadie la leyera. Así que acá se comparan MUESTRAS RENDERIZADAS, siempre.
//
// El instrumento es `AudioEngine::renderBlock` (WD-2.1): determinista, sin
// device, y el mismo que usa la suite golden.
//
// LA FORMA DE CADA COMPARACIÓN
// ----------------------------
// "¿la pista pasa por la cadena?" se responde renderizando DOS veces la misma
// escena —una con la cadena vacía y otra con un efecto que altera de forma
// inequívoca— y mirando si el audio cambió. Si la pista NO pasa por la cadena,
// agregar un efecto no puede cambiar ni una muestra; si pasa, tiene que
// cambiarlas. No hace falta saber cómo suena el efecto, sólo que no es la
// identidad — que es lo que lo vuelve robusto a que el DSP se retoque después.
// ===========================================================================
#include "../AudioEngine.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kMaxBlock = 512;
constexpr int kTrackFrames = 4800;   // 100 ms de loop
constexpr int kFxTrack = 0;
constexpr int kDryTrack = 1;

/// El motor sin instrumento: sólo se oye lo que ponga el looper. Es lo que
/// permite atribuir cada muestra a la pista y no al synth.
///
/// 🔴 El silencio se hace por AMPLITUD, no con `setOscillatorEnabled(false)`.
/// Medido: con el oscilador deshabilitado, `renderVoiceSystem` hace
/// `std::fill_n(output, 0)` y **no llama a `applyEffectsAndLooper`** — o sea que
/// el looper entero enmudece con el instrumento apagado. Ese camino es anterior a
/// este REQ y queda anotado en las Notas de la etapa; acá sólo hay que no
/// pisarlo, porque si no el test mide silencio y cualquier aserción floja pasa.
void silenceTheInstrument(AudioEngine& engine) {
    engine.setOscillatorEnabled(true);
    engine.setMasterVolume(1.0f);
    engine.setFrequencyAndAmplitude(440.0f, 0.0f);
}

/// Graba `value` constante en una pista y la deja sonando en loop.
/// Constante y no un seno: cualquier desviación es del ruteo, no de la fase.
void recordConstantTrack(AudioEngine& engine, int track, float value) {
    AudioLooper& looper = engine.getAudioLooper();
    // Sin esto `process()` retorna en la primera línea y NO graba ni reproduce:
    // el looper arranca deshabilitado. Cuesta decirlo porque el síntoma es
    // silencio, y contra un umbral flojo el silencio pasa por señal.
    looper.setEnabled(true);
    ASSERT_TRUE(looper.prepareTrack(track, kTrackFrames, kSampleRate));
    looper.startRecording(track);

    std::vector<float> buf(static_cast<size_t>(kMaxBlock) * 2);
    int remaining = kTrackFrames;
    int64_t playFrame = 0;
    while (remaining > 0) {
        const int n = std::min(kMaxBlock, remaining);
        for (int i = 0; i < n * 2; ++i) buf[i] = value;
        looper.process(buf.data(), n, playFrame);
        playFrame += n;
        remaining -= n;
    }
    looper.stopRecording();
}

/// Un efecto que NO es la identidad, con parámetros que garantizan que altere.
void addAnUnmistakableEffect(AudioEngine& engine) {
    ASSERT_TRUE(engine.addEffect(EffectType::DISTORTION));
    engine.setParameter(0, 0, 1.0f);   // drive al máximo
    engine.setParameter(0, 1, 1.0f);   // mix al máximo
}

/// Renderiza `frames` en bloques de `block` y devuelve el audio entero.
std::vector<float> render(AudioEngine& engine, int frames, int block = kMaxBlock) {
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    std::vector<float> scratch(static_cast<size_t>(block) * 2, 0.0f);
    for (int done = 0; done < frames; done += block) {
        const int n = std::min(block, frames - done);
        std::fill(scratch.begin(), scratch.end(), 0.0f);
        if (!engine.renderBlock(scratch.data(), nullptr, n)) {
            ADD_FAILURE() << "renderBlock() falló en el frame " << done;
            return out;
        }
        std::copy(scratch.begin(), scratch.begin() + static_cast<size_t>(n) * 2,
                  out.begin() + static_cast<size_t>(done) * 2);
    }
    return out;
}

bool bitIdentical(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) return false;
    return true;
}

/// Piso de "esto de verdad suena". `> 0.0` NO sirve: el silencio del motor mide
/// ~1e-5 y cumple, así que un test con ese umbral pasa sin medir nada — se
/// descubrió exactamente así en esta etapa.
constexpr double kAudible = 0.01;

double rms(const std::vector<float>& v) {
    double sum = 0.0;
    for (float x : v) sum += static_cast<double>(x) * x;
    return v.empty() ? 0.0 : std::sqrt(sum / v.size());
}

/// Escena mínima: motor offline, instrumento mudo, una pista sonando.
/// `withEffect` decide si la cadena tiene algo; `sendToFx`, si la pista se rutea.
std::vector<float> renderScene(bool withEffect, bool sendToFx, int frames,
                               int block = kMaxBlock) {
    AudioEngine engine;
    EXPECT_TRUE(engine.startOffline(kSampleRate, kMaxBlock));
    silenceTheInstrument(engine);
    recordConstantTrack(engine, kFxTrack, 0.5f);
    engine.getAudioLooper().setTrackSendToFx(kFxTrack, sendToFx);
    if (withEffect) addAnUnmistakableEffect(engine);
    auto out = render(engine, frames, block);
    engine.stop();
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// AC-007.1 — con el flag APAGADO, la cadena no puede tocar la pista.
//
// Es el camino que corre para todos los usuarios de hoy, así que la exigencia es
// bit a bit: si agregar un efecto cambia UNA muestra, la pista se está colando
// por la cadena.
// ---------------------------------------------------------------------------
TEST(FxTrackRouting, AnUnflaggedTrackIsUntouchedByTheChain) {
    const auto sinFx = renderScene(/*withEffect=*/false, /*sendToFx=*/false, kTrackFrames);
    const auto conFx = renderScene(/*withEffect=*/true,  /*sendToFx=*/false, kTrackFrames);

    ASSERT_GT(rms(sinFx), kAudible) << "la pista tiene que sonar; si no, el test no mide nada";
    EXPECT_TRUE(bitIdentical(sinFx, conFx))
        << "con el flag apagado la pista se mezcla downstream: la cadena no la ve";
}

// ---------------------------------------------------------------------------
// AC-007.2 — con el flag PRENDIDO, la cadena sí la procesa.
//
// El control positivo del test de arriba: sin esto, "no cambió nada" se
// satisface trivialmente con un flag que no hace nada.
// ---------------------------------------------------------------------------
TEST(FxTrackRouting, AFlaggedTrackGoesThroughTheChain) {
    const auto sinFx = renderScene(/*withEffect=*/false, /*sendToFx=*/true, kTrackFrames);
    const auto conFx = renderScene(/*withEffect=*/true,  /*sendToFx=*/true, kTrackFrames);

    ASSERT_GT(rms(sinFx), kAudible);
    EXPECT_FALSE(bitIdentical(sinFx, conFx))
        << "con el flag prendido el efecto tiene que llegar a la pista";
}

// ---------------------------------------------------------------------------
// AC-007.2 (borde) — con la cadena VACÍA, marcar la pista no cambia el audio.
//
// Sin este test, "el flag hace algo" se puede cumplir por el motivo equivocado:
// una pasada de más, un buffer que no se limpia, la pista sumada dos veces.
// ---------------------------------------------------------------------------
TEST(FxTrackRouting, WithAnEmptyChainTheFlagChangesNothing) {
    const auto apagado = renderScene(/*withEffect=*/false, /*sendToFx=*/false, kTrackFrames);
    const auto prendido = renderScene(/*withEffect=*/false, /*sendToFx=*/true, kTrackFrames);

    ASSERT_GT(rms(apagado), kAudible);
    EXPECT_TRUE(bitIdentical(apagado, prendido))
        << "sin efectos, pasar por la cadena es la identidad: mismo audio";
}

// ---------------------------------------------------------------------------
// AC-007.4 — la pista marcada conserva su volumen, paneo y el master del looper.
//
// Y es el test que atrapa el defecto más caro de esta etapa: `mixInto` avanza el
// playhead y los tres smoothers, así que mezclar una pista DOS veces por bloque
// no duplica el audio, corre su tiempo al doble. Dos pistas idénticas, una
// marcada y otra no, con la cadena vacía, tienen que aportar exactamente lo
// mismo.
// ---------------------------------------------------------------------------
TEST(FxTrackRouting, AFlaggedTrackKeepsItsOwnGainStaging) {
    auto renderOne = [](bool sendToFx) {
        AudioEngine engine;
        EXPECT_TRUE(engine.startOffline(kSampleRate, kMaxBlock));
        silenceTheInstrument(engine);
        recordConstantTrack(engine, kFxTrack, 0.5f);
        AudioLooper& looper = engine.getAudioLooper();
        looper.setTrackVolume(kFxTrack, 0.6f);
        looper.setTrackPan(kFxTrack, -0.3f);
        looper.setMasterVolume(0.7f);
        looper.setTrackSendToFx(kFxTrack, sendToFx);
        auto out = render(engine, kTrackFrames);
        engine.stop();
        return out;
    };

    const auto porElLooper = renderOne(false);
    const auto porLaCadena = renderOne(true);

    ASSERT_GT(rms(porElLooper), kAudible);
    EXPECT_TRUE(bitIdentical(porElLooper, porLaCadena))
        << "el ruteo cambia POR DÓNDE pasa la pista, no cuánto suena ni dónde";
}

// ---------------------------------------------------------------------------
// AC-007.5 — la pista marcada entra al bus del instrumento, así que RECIBE el
// fade de pausa. Es la contrapartida explícita del ruteo, no un descuido: la no
// marcada tiene que seguir sonando igual que hoy.
// ---------------------------------------------------------------------------
TEST(FxTrackRouting, TheFadeReachesAFlaggedTrackAndOnlyAFlaggedOne) {
    auto rmsDuringFade = [](bool sendToFx) {
        AudioEngine engine;
        EXPECT_TRUE(engine.startOffline(kSampleRate, kMaxBlock));
        silenceTheInstrument(engine);
        recordConstantTrack(engine, kFxTrack, 0.5f);
        engine.getAudioLooper().setTrackSendToFx(kFxTrack, sendToFx);

        render(engine, kMaxBlock);            // un bloque en régimen
        engine.pauseWithFade(50);
        auto out = render(engine, kSampleRate / 10);   // 100 ms: el fade entero
        engine.stop();
        return rms(out);
    };

    const double marcada = rmsDuringFade(true);
    const double sinMarcar = rmsDuringFade(false);

    EXPECT_LT(marcada, sinMarcar * 0.9)
        << "una pista marcada es parte del instrumento: el fade la baja";
    EXPECT_GT(sinMarcar, kAudible)
        << "una pista NO marcada sobrevive al fade — el invariante de siempre";
}

// ---------------------------------------------------------------------------
// AC-007.3 — cambiar el flag entre bloques no clickea, y cada pista se mezcla
// exactamente UNA vez por bloque.
//
// El clic se mide por VECINDAD, con el mismo instrumento que usó WD-3.3: el salto
// entre muestras consecutivas ATRAVESANDO un borde de bloque no puede ser mayor
// que el mayor salto DENTRO de los bloques. Es auto-calibrante, y por eso no lo
// engañan ni el arranque de la pista (que rampea de 0 a régimen en el primer
// bloque) ni la costura del loop — que fue exactamente lo que hizo fallar a la
// primera versión de este test contra un umbral fijo.
// ---------------------------------------------------------------------------
TEST(FxTrackRouting, FlippingTheFlagBetweenBlocksDoesNotClick) {
    AudioEngine engine;
    ASSERT_TRUE(engine.startOffline(kSampleRate, kMaxBlock));
    silenceTheInstrument(engine);
    recordConstantTrack(engine, kFxTrack, 0.5f);
    AudioLooper& looper = engine.getAudioLooper();

    constexpr int kBlocks = 8;
    std::vector<float> todo;
    for (int b = 0; b < kBlocks; ++b) {
        looper.setTrackSendToFx(kFxTrack, b % 2 == 0);   // alterna por bloque
        const auto bloque = render(engine, kMaxBlock);
        todo.insert(todo.end(), bloque.begin(), bloque.end());
    }
    engine.stop();

    ASSERT_GT(rms(todo), kAudible);

    // Canal izquierdo. Un índice de muestra `k` cae en un borde si k % kMaxBlock == 0.
    float peorDentro = 0.0f, peorEnBorde = 0.0f;
    for (int k = 1; k < kBlocks * kMaxBlock; ++k) {
        const float salto = std::fabs(todo[static_cast<size_t>(k) * 2]
                                    - todo[static_cast<size_t>(k - 1) * 2]);
        if (k % kMaxBlock == 0) peorEnBorde = std::max(peorEnBorde, salto);
        else                    peorDentro  = std::max(peorDentro, salto);
    }

    EXPECT_LE(peorEnBorde, peorDentro * 1.5f + 1e-6f)
        << "el borde donde cambia el ruteo salta " << peorEnBorde
        << ", contra " << peorDentro << " que ya produce la propia señal";
}

// AC-007.8 — invariancia de bloque del RUTEO.
//
// Renderizar N frames de una vez y en K bloques de N/K tiene que dar el mismo
// audio. Si no da, el ruteo metió estado que depende del TAMAÑO del bloque — el
// defecto que WD-3.4 pagó caro y que un backend real no deja ver, porque siempre
// entrega el mismo tamaño.
//
// 🔬 La cadena va VACÍA, y no es para ablandar el test: es para que mida el
// ruteo. Medido en esta etapa, con los tres casos: el camino no marcado diverge
// 0,0; el marcado con la cadena vacía diverge 0,0; y el marcado CON `DISTORTION`
// diverge 5,8e-4. O sea que la dependencia de bloque es del EFECTO, y este ruteo
// apenas la destapa —es la primera vez que el audio del looper pasa por uno—.
// Afirmarla acá haría que este test rompa cada vez que alguien toque un efecto,
// culpando al archivo equivocado. Queda anotada en las Notas de la etapa.
// ---------------------------------------------------------------------------
TEST(FxTrackRouting, TheRoutingItselfIsBlockSizeInvariant) {
    constexpr int kFrames = 2048;
    const auto enPocos = renderScene(/*withEffect=*/false, /*sendToFx=*/true,
                                     kFrames, kFrames / 4);
    const auto enMuchos = renderScene(/*withEffect=*/false, /*sendToFx=*/true,
                                      kFrames, kFrames / 16);

    ASSERT_GT(rms(enPocos), kAudible);
    ASSERT_EQ(enPocos.size(), enMuchos.size());
    for (size_t i = 0; i < enPocos.size(); ++i) {
        ASSERT_FLOAT_EQ(enPocos[i], enMuchos[i])
            << "divergen en la muestra " << i << ": el ruteo tiene estado por bloque";
    }
}

// AC-007.6 — el tap de grabación SÍ captura una pista marcada.
//
// No es evitable con esta opción: el tap lee `output` aguas abajo de la cadena,
// donde la pista ya es inseparable del synth. Se fija con un test para que sea
// una decisión documentada y no un descubrimiento del usuario — y para que si
// alguien la cambia, se entere de que cambió un contrato.
// ---------------------------------------------------------------------------
TEST(FxTrackRouting, TheRecordingTapDoesCaptureAFlaggedTrack) {
    AudioEngine engine;
    ASSERT_TRUE(engine.startOffline(kSampleRate, kMaxBlock));
    silenceTheInstrument(engine);
    recordConstantTrack(engine, kFxTrack, 0.5f);
    AudioLooper& looper = engine.getAudioLooper();
    looper.setTrackSendToFx(kFxTrack, true);

    ASSERT_TRUE(looper.prepareTrack(kDryTrack, kTrackFrames, kSampleRate));
    looper.startRecording(kDryTrack);
    render(engine, kTrackFrames);
    looper.stopRecording();

    // La pista recién grabada, sola: si el tap capturó algo, tiene energía.
    looper.setTrackSendToFx(kFxTrack, false);
    looper.setTrackMuted(kFxTrack, true);
    const auto soloLaGrabada = render(engine, kTrackFrames);
    engine.stop();

    EXPECT_GT(rms(soloLaGrabada), kAudible)
        << "documentado: grabar con una pista marcada sonando la mete en la toma";
}
