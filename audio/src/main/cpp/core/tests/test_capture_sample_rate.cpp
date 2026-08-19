/**
 * REQ-001 S1, tareas 1.16-1.21 — el rate de CAPTURA llega vivo hasta el analisis.
 *
 * EMPIEZA POR EL REPRO, NO POR EL ARREGLO
 * ---------------------------------------
 * Las cifras que motivan estas tareas (+146,7 cents a 44,1 kHz, +702 a 32 kHz,
 * +1902 a 16 kHz) eran ARITMETICA: el afinador todavia no existe, asi que nadie
 * las habia medido. Este repo ya se cobro dos veces un ticket cuyo escenario no
 * reproducia, asi que estos tests fallan ANTES del arreglo y por el motivo
 * escrito, no por otro.
 *
 * QUE SE ROMPE, Y NO ES EL PRESUPUESTO DE 0,1 CENT
 * ------------------------------------------------
 * Un rate asumido mal es un ESCALADO UNIFORME de todas las frecuencias medidas,
 * asi que PRESERVA las razones entre notas: la exactitud RELATIVA de AC-001.7
 * sobrevive y el instrumento queda consistente consigo mismo, solo transpuesto.
 * Decir que "se come el presupuesto de 0,1 cent" confunde relativo con absoluto.
 *
 * Lo que si rompe es la consistencia ENTRE FUENTES —AC-001.17, dos rates
 * distintos leen la misma altura fisica con hasta 700 cents de diferencia— y la
 * honestidad de la cifra declarada, que culpa al cristal del ADC por 0,087 cents
 * mientras el motor agregaria hasta 1902.
 *
 * Esas tres verificaciones son de S8, S2 y S10. Aca va solo la PLOMERIA, que es
 * lo que las habilita.
 */

#include "support/BackendPathFixture.h"
#include "../../nodes/InputNode.h"

#include <gtest/gtest.h>

#include <memory>

namespace wma_test {
namespace {

/// NO 48000, a proposito: es la constante que el motor traia cableada, y usarla
/// como rate de prueba haria que un defecto de propagacion pase inadvertido por
/// pura coincidencia.
constexpr int kNegotiated = 44100;

using CaptureSampleRateTest = BackendPathFixture;

/**
 * 1.16 — un cambio de configuracion de stream tiene que llegar al `InputNode`.
 *
 * Antes del arreglo: `onStreamConfigChanged` re-preparaba `mOscBank`,
 * `mEffectChain` y `mOutputStage`, y al `InputNode` **no lo tocaba nadie** — el
 * unico `prepare()` que recibe en todo el arbol es el `prepare(48000, 4096)`
 * LITERAL de `wmaEnsureInputNode`.
 */
TEST_F(CaptureSampleRateTest, AStreamConfigChangeReachesTheInputNode) {
    auto node = std::make_shared<InputNode>();
    node->prepare(48000, 4096);              // el prepare literal que hace el motor
    mEngine->setInputNode(node);

    // `prepare()` configura el DSP para un rate; NO es evidencia de que exista
    // un stream a ese rate. Por eso sigue siendo "desconocido".
    ASSERT_EQ(node->getCaptureSampleRate(), 0) << "punto de partida";

    watermelon_audio::StreamInfo info{};
    info.sampleRate = kNegotiated;
    info.channelCount = 2;
    mEngine->onStreamConfigChanged(info);

    EXPECT_EQ(node->getCaptureSampleRate(), kNegotiated)
        << "el camino de captura se quedo con el rate viejo: todo lo que mida "
           "sobre el va a estar escalado por 48000/" << kNegotiated;

    mEngine->setInputNode(nullptr);
}

/**
 * 1.17 — el accesor tiene que decir la verdad, o no llamarse asi.
 *
 * `getStreamSampleRate()` devolvia `mSampleRate`, o sea el rate con el que se
 * PREPARO, no el del stream. Un nodo recien construido ya respondia 48000 —el
 * default de `AudioNode`— sin que existiera stream ninguno.
 */
TEST_F(CaptureSampleRateTest, AFreshNodeDoesNotClaimToKnowARateItWasNeverTold) {
    InputNode node;
    EXPECT_EQ(node.getCaptureSampleRate(), 0)
        << "sin stream ni prepare no hay rate que reportar; 48000 seria inventarlo";
}

/**
 * 1.18 — `mSampleRateMismatch` existe para avisar que entrada y salida corren a
 * rates distintos. Comparaba el rate vivo de salida contra la constante 48000,
 * asi que era falso positivo en un device a 44,1 kHz y falso negativo cuando
 * entrada != salida pero salida = 48 kHz.
 */
TEST_F(CaptureSampleRateTest, TheMismatchFlagComparesTwoRealRates) {
    // Sin stream corriendo, `currentSampleRate()` resuelve por el preferido.
    // Ponerlo modela "el motor esta a 44100" del lado de la SALIDA, que es la
    // mitad contra la que se compara.
    mBackend->setNegotiatedSampleRate(kNegotiated);
    mEngine->setPreferredSampleRate(kNegotiated);

    auto node = std::make_shared<InputNode>();
    node->prepare(48000, 4096);
    mEngine->setInputNode(node);

    watermelon_audio::StreamInfo info{};
    info.sampleRate = kNegotiated;
    info.channelCount = 2;
    mEngine->onStreamConfigChanged(info);

    // El flag lo actualiza `captureMonitoringBlock`, que solo corre con el
    // monitoreo encendido — es el unico camino donde el rate de entrada
    // importa de verdad.
    node->setMonitoringEnabled(true);
    std::vector<float> out(256 * 2, 0.0f);
    std::vector<float> in(256 * 2, 0.1f);
    mEngine->onAudioReady(out.data(), in.data(), 256);

    EXPECT_EQ(mEngine->getLastInputSampleRate(), kNegotiated);
    EXPECT_FALSE(mEngine->hasSampleRateMismatch())
        << "entrada y salida estan las dos en " << kNegotiated
        << ": marcar desajuste es un falso positivo";

    // Y al reves: cuando de verdad difieren, lo dice. Sin esta mitad el test
    // se cumpliria con un flag clavado en `false`.
    node->setCaptureSampleRate(32000);
    mEngine->onAudioReady(out.data(), in.data(), 256);
    EXPECT_EQ(mEngine->getLastInputSampleRate(), 32000);
    EXPECT_TRUE(mEngine->hasSampleRateMismatch())
        << "entrada a 32000 y salida a " << kNegotiated << ": eso ES un desajuste";

    mEngine->setInputNode(nullptr);
}

/**
 * 1.16 (backend PARTIDO) — cuando entrada y salida son streams distintos, el de
 * ENTRADA manda sobre el rate de captura.
 *
 * `SplitBackend::InputCallback::onStreamConfigChanged` era literalmente
 * `(void)newInfo;`: la config del stream de captura se descartaba en el seam. Y
 * ese es el caso PRINCIPAL de un afinador — guitarra por interfaz USB. Sin este
 * camino, el arreglo del rate publicaria el numero del stream equivocado.
 *
 * Se prueban LOS DOS ORDENES, porque el correcto no puede depender de cual
 * callback llegue primero.
 */
TEST_F(CaptureSampleRateTest, OnASplitBackendTheInputStreamWinsOverTheOutput) {
    constexpr int kOutRate = 48000;
    constexpr int kInRate  = 44100;

    for (int order = 0; order < 2; ++order) {
        auto node = std::make_shared<InputNode>();
        node->prepare(kOutRate, 4096);
        mEngine->setInputNode(node);

        watermelon_audio::StreamInfo outInfo{};
        outInfo.sampleRate = kOutRate;
        outInfo.channelCount = 2;
        watermelon_audio::StreamInfo inInfo{};
        inInfo.sampleRate = kInRate;
        inInfo.channelCount = 2;

        if (order == 0) {
            mEngine->onStreamConfigChanged(outInfo);
            mEngine->onInputStreamConfigChanged(inInfo);
        } else {
            mEngine->onInputStreamConfigChanged(inInfo);
            mEngine->onStreamConfigChanged(outInfo);
        }

        EXPECT_EQ(node->getCaptureSampleRate(), kInRate)
            << "orden " << order << ": publico el rate de la SALIDA como si "
               "fuera el de la captura";

        mEngine->setInputNode(nullptr);
    }
}

}  // namespace
}  // namespace wma_test
