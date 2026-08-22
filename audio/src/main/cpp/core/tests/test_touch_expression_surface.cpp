/**
 * test_touch_expression_surface.cpp — REQ-008 S1, tarea 1.6.
 *
 * ## Qué prueba esto que `test_touch_expression.cpp` NO prueba
 *
 * Aquel archivo llama a `SoundFontEngine::setTouchExpression` **directamente sobre el
 * motor**. Es lo correcto para el DSP, pero entre ese metodo y un consumidor real hay
 * ahora tres capas que la tarea 1.6 agrego —`SynthEngineDispatcher`, `AudioEngine` y la
 * C-API—, y ninguna de ellas estaba bajo test: son reenvios de una linea, y un reenvio de
 * una linea que pierde el `touchId` compila perfecto y deja verde a todo el archivo de
 * abajo, porque aquel entra por otra puerta.
 *
 * Asi que esto entra por la puerta de afuera que SI se puede renderizar offline
 * (`AudioEngine::startOffline` + `renderBlock`) y compara MUESTRAS, que es el criterio de
 * muerte del REQ.
 *
 * 🔴 **Lo que deliberadamente NO cubre**: `wma_sf_set_touch_expression`, la capa C. No hay
 * render offline en la C-API —es una decision vieja del repo, ver el encabezado de
 * `test_c_api_synth.cpp`— asi que la unica forma de cubrirla con muestras seria agregarle
 * una puerta de render, que es otro REQ. Lo que hay para esa capa es el test de contrato de
 * `test_c_api_voice.cpp`: sobrevive a cualquier `touch_id` y a un handle nulo.
 *
 * ## La trampa que este archivo respeta
 *
 * "Suena" nunca es `> 0`: el piso de silencio del motor mide ~1e-5. Y antes de comparar dos
 * configuraciones se compara una CONSIGO MISMA — el primer test de abajo no afirma nada
 * mas que eso, y si se cae, todo lo demas es ruido.
 */

#include "support/MinimalSoundFont.h"

#include "core/AudioEngine.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using wma_test::makeMinimalSoundFont;

constexpr int kSampleRate = 48000;
constexpr int kBlock = 256;
constexpr int kEngineTypeSoundFont = 6;
constexpr int kNote = 60;
constexpr int kOtherNote = 64;
constexpr float kVelocity = 0.9f;

/** Bloques por tramo: 0,1 s. Bien pasado el ataque del font sintetico. */
constexpr int kBlocksPerLeg = 19;

/** El piso de silencio del motor mide ~1e-5. Esto es "sono", con margen. */
constexpr double kAudible = 0.005;

/** Un gesto de expresion, o ninguno. */
struct Gesture {
    bool send = false;
    int touchId = 0;
    float value = 1.0f;
};

/** RMS de un tramo estereo. */
double rms(const std::vector<float>& buf) {
    if (buf.empty()) return 0.0;
    double acc = 0.0;
    for (const float s : buf) acc += static_cast<double>(s) * s;
    return std::sqrt(acc / static_cast<double>(buf.size()));
}

/**
 * Renderiza dos tramos con una nota viva, mandando @p gesture en el medio.
 *
 * Instancia fresca por llamada, a proposito: `tsf_reset` NO deja pizarra limpia (medido en
 * el spike del REQ), asi que reusar el motor entre corridas hace divergir renders que
 * deberian ser identicos.
 *
 * @param second [out] el tramo posterior al gesto
 * @return el tramo anterior al gesto
 */
std::vector<float> renderAround(const Gesture& gesture,
                                std::vector<float>& second,
                                bool secondTouch = false) {
    const auto sf2 = makeMinimalSoundFont(kSampleRate, /*looping=*/true);

    AudioEngine engine;
    EXPECT_TRUE(engine.startOffline(kSampleRate, kBlock));
    engine.setEngineType(kEngineTypeSoundFont);
    EXPECT_TRUE(engine.loadSoundFont(sf2.data(), static_cast<int>(sf2.size())));
    engine.setSoundFontPreset(0);

    engine.sfNoteOn(0, kNote, kVelocity);
    if (secondTouch) engine.sfNoteOn(1, kOtherNote, kVelocity);

    std::vector<float> first;
    std::vector<float> block(static_cast<size_t>(kBlock) * 2, 0.0f);

    for (int i = 0; i < kBlocksPerLeg; ++i) {
        std::fill(block.begin(), block.end(), 0.0f);
        EXPECT_TRUE(engine.renderBlock(block.data(), nullptr, kBlock));
        first.insert(first.end(), block.begin(), block.end());
    }

    if (gesture.send) {
        engine.sfSetTouchExpression(gesture.touchId, gesture.value);
    }

    second.clear();
    for (int i = 0; i < kBlocksPerLeg; ++i) {
        std::fill(block.begin(), block.end(), 0.0f);
        EXPECT_TRUE(engine.renderBlock(block.data(), nullptr, kBlock));
        second.insert(second.end(), block.begin(), block.end());
    }

    engine.stop();
    return first;
}

}  // namespace

// ===========================================================================
// El fixture, antes que nada: una configuracion contra si misma
// ===========================================================================

TEST(TouchExpressionSurface, TheHarnessIsDeterministicAndActuallyMakesSound) {
    // Si esto falla, ninguna comparacion de abajo significa nada: estariamos
    // leyendo ruido del harness y no el efecto del gesto. Es la leccion del
    // spike del REQ, donde dos renders identicos divergian 1,6e-01 y el
    // veredicto salio al reves dos veces seguidas.
    std::vector<float> secondA;
    const auto firstA = renderAround(Gesture{}, secondA);

    std::vector<float> secondB;
    const auto firstB = renderAround(Gesture{}, secondB);

    ASSERT_GT(rms(firstA), kAudible) << "el motor no esta sonando: no hay nada que medir";
    ASSERT_EQ(firstA.size(), firstB.size());
    ASSERT_EQ(secondA.size(), secondB.size());

    for (size_t i = 0; i < firstA.size(); ++i) {
        ASSERT_FLOAT_EQ(firstA[i], firstB[i]) << "el harness no es determinista, frame " << i;
    }
    for (size_t i = 0; i < secondA.size(); ++i) {
        ASSERT_FLOAT_EQ(secondA[i], secondB[i]) << "el harness no es determinista, frame " << i;
    }
}

// ===========================================================================
// AC-008.1 por la puerta de afuera: baja el nivel, sin volver a atacar
// ===========================================================================

TEST(TouchExpressionSurface, TheGestureReachesTheEngineThroughEveryNewLayer) {
    // El control: sin gesto, el segundo tramo NO baja. Sin el, "baja" podria ser
    // simplemente el decay del font y el test pasaria con la cadena rota.
    std::vector<float> quietLeg;
    const auto loudLeg = renderAround(Gesture{true, 0, 0.25f}, quietLeg);

    std::vector<float> untouchedSecond;
    const auto untouchedFirst = renderAround(Gesture{}, untouchedSecond);

    ASSERT_GT(rms(loudLeg), kAudible);
    ASSERT_GT(rms(untouchedFirst), kAudible);

    EXPECT_LT(rms(quietLeg), rms(loudLeg) * 0.8)
        << "la expresion no bajo el nivel: el gesto no llego al motor por alguna "
           "de las capas nuevas (dispatcher, AudioEngine)";

    EXPECT_GT(rms(untouchedSecond), rms(untouchedFirst) * 0.8)
        << "sin gesto el segundo tramo ya bajaba solo — el test de arriba estaria "
           "midiendo el decay del font y no la expresion";
}

// ===========================================================================
// AC-008.5 / el mutante M6: el touchId sobrevive el viaje
// ===========================================================================

TEST(TouchExpressionSurface, TheGestureLandsOnTheTouchItNamesAndNoOther) {
    // ESTE es el test que mata al reenvio que pierde o corre el touchId. Con dos
    // toques vivos, un gesto sobre el 1 no puede alterar lo que aporta el 0 —y si
    // alguna capa mandara el gesto al toque de al lado, o a todos, el nivel del
    // render entero cambiaria de forma distinta a la esperada.
    std::vector<float> afterOnTouchOne;
    const auto beforeOnTouchOne = renderAround(Gesture{true, 1, 0.25f}, afterOnTouchOne,
                                               /*secondTouch=*/true);

    std::vector<float> afterOnTouchZero;
    const auto beforeOnTouchZero = renderAround(Gesture{true, 0, 0.25f}, afterOnTouchZero,
                                                /*secondTouch=*/true);

    ASSERT_GT(rms(beforeOnTouchOne), kAudible);
    ASSERT_FLOAT_EQ(rms(beforeOnTouchOne), rms(beforeOnTouchZero))
        << "los dos tramos previos son la misma configuracion: si difieren, el "
           "harness no esta comparando lo que cree";

    // Los dos bajan —cada uno atenua su propio toque— pero NO al mismo sitio: las
    // dos notas son distintas y aportan distinto. Que sean distintos es la prueba
    // de que el touchId llego intacto.
    EXPECT_LT(rms(afterOnTouchOne), rms(beforeOnTouchOne))
        << "atenuar el toque 1 no cambio nada: el gesto no llego a ese toque";
    EXPECT_LT(rms(afterOnTouchZero), rms(beforeOnTouchZero))
        << "atenuar el toque 0 no cambio nada: el gesto no llego a ese toque";
    EXPECT_NE(rms(afterOnTouchOne), rms(afterOnTouchZero))
        << "atenuar el toque 0 y atenuar el 1 dieron el MISMO audio: el touchId se "
           "esta perdiendo en alguna capa y el gesto va a parar siempre al mismo lado";
}

// ===========================================================================
// AC-008.5: un toque fuera de rango no hace nada y no rompe
// ===========================================================================

TEST(TouchExpressionSurface, AnOutOfRangeTouchIsANoOpAndNotACrash) {
    std::vector<float> afterBogus;
    const auto beforeBogus = renderAround(Gesture{true, 9999, 0.25f}, afterBogus);

    std::vector<float> afterNone;
    const auto beforeNone = renderAround(Gesture{}, afterNone);

    ASSERT_GT(rms(beforeBogus), kAudible);
    ASSERT_EQ(afterBogus.size(), afterNone.size());

    for (size_t i = 0; i < afterBogus.size(); ++i) {
        ASSERT_FLOAT_EQ(afterBogus[i], afterNone[i])
            << "un touchId fuera de rango cambio el audio, frame " << i;
    }
}
