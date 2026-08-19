/**
 * El DSP del camino de ENTRADA — los primeros tests de comportamiento que tiene.
 *
 * POR QUE NO EXISTIAN
 * -------------------
 * No es que nadie los escribio: **no se podian escribir**. La suite de host
 * sustituia `nodes/InputNode.cpp` por un doble, asi que `processInputBlock` —
 * ganancia, DC blocker, noise gate, medidor, ring de monitoreo— no lo corria
 * nadie fuera de Android e iOS. Es el caso de libro de una ausencia de tests que
 * es una IMPOSIBILIDAD, no un olvido.
 *
 * Sacar el doble cerro la ceguera de COMPILACION —medido: borrar el include de
 * `Platform.h` ahora rompe el build de host, y antes lo agarraba solo iOS— pero
 * no la de comportamiento: con el nodo real adentro y sin estos tests, ignorar
 * la ganancia de entrada ENTERA seguia pasando los 894.
 *
 * Importa ahora y no en abstracto: REQ-001 S1 edita `processInputBlock` para
 * escribir el ring de analisis del afinador.
 *
 * COMO SE MIDE
 * ------------
 * Directo sobre `InputNode`, no por el motor: lo que se afirma es el DSP del
 * nodo, y el camino motor→MIX ya lo cubren `test_c_api_master_bus.cpp` y
 * `test_input_node_retire.cpp`. `feedExternalInput()` es la puerta que usa el
 * backend USB y la que el host puede manejar.
 */

#include "../../nodes/InputNode.h"
#include "../../analysis/AnalysisRing.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kBlockFrames = 256;

/// 187,5 Hz entra exactamente un ciclo en 256 frames a 48 kHz, y la muestra 64
/// cae justo en el pico: el pico por bloque es la amplitud pedida, sin
/// depender de donde cayo el corte del buffer.
constexpr double kToneHz = 187.5;

/// Amplitudes y ganancias elegidas NO representables en binario a proposito:
/// con 0,5 o 6 dB un error de redondeo o de factor 2 puede pasar inadvertido.
constexpr float kAmp = 0.23f;

class InputHarness {
public:
    InputHarness() {
        mNode.prepare(kSampleRate, kBlockFrames);
        mNode.setMonitoringEnabled(true);
        mNode.setMonitoringVolume(1.0f);
        mNode.setInputGain(0.0f);
        mIn.resize(static_cast<size_t>(kBlockFrames) * 2);
        mOut.resize(static_cast<size_t>(kBlockFrames) * 2);
    }

    InputNode& node() { return mNode; }

    /// Empuja `blocks` bloques de tono (o de DC si `dc` es true) y devuelve el
    /// pico del ULTIMO bloque que salio del ring de monitoreo.
    float pump(int blocks, float amplitude, bool dc = false) {
        float peak = 0.0f;
        for (int b = 0; b < blocks; ++b) {
            for (int f = 0; f < kBlockFrames; ++f) {
                const float v = dc ? amplitude
                                   : static_cast<float>(amplitude * std::sin(mPhase));
                mPhase += 2.0 * M_PI * kToneHz / kSampleRate;
                if (mPhase >= 2.0 * M_PI) mPhase -= 2.0 * M_PI;
                mIn[static_cast<size_t>(f) * 2] = v;
                mIn[static_cast<size_t>(f) * 2 + 1] = v;
            }
            mNode.feedExternalInput(mIn.data(), kBlockFrames);
            std::fill(mOut.begin(), mOut.end(), 0.0f);
            const int got = mNode.getMonitoringSamples(mOut.data(), kBlockFrames);
            peak = 0.0f;
            for (int i = 0; i < got * 2; ++i) peak = std::fmax(peak, std::fabs(mOut[i]));
        }
        return peak;
    }

private:
    InputNode mNode;
    std::vector<float> mIn, mOut;
    double mPhase = 0.0;
};

}  // namespace

// ---------------------------------------------------------------------------

/// La precondicion. Sin esto ningun test de abajo mide nada.
TEST(InputNodeDsp, AToneFedInComesOutOfTheMonitoringRing) {
    InputHarness h;
    EXPECT_NEAR(h.pump(20, kAmp), kAmp, kAmp * 0.02f)
        << "con ganancia 0 dB y volumen 1,0 el nodo tiene que transportar el tono";
}

/**
 * La ganancia de entrada hace algo, y hace lo que dice.
 *
 * Antes de este archivo, ignorar el bloque de ganancia ENTERO pasaba los 894
 * tests: los tests de nivel que habia miden RAZONES (el master partido al medio),
 * y una razon es invariante a una ganancia constante.
 */
TEST(InputNodeDsp, TheInputGainScalesByTheDecibelsItWasAsked) {
    for (float dB : {-7.3f, 3.7f, 11.1f}) {
        InputHarness h;
        h.node().setInputGain(dB);
        const float expected = kAmp * std::pow(10.0f, dB / 20.0f);
        EXPECT_NEAR(h.pump(20, kAmp), expected, expected * 0.02f)
            << "con " << dB << " dB de ganancia de entrada";
    }
}

/**
 * El camino de entrada tiene SU PROPIO DC blocker, y esto es lo que lo dice.
 *
 * No es un detalle: `CApiFixture` alimentaba la entrada con un valor CONSTANTE y
 * justificaba la eleccion afirmando que *"la senal monitoreada nunca pasa por el
 * DC blocker"*. Es falso —hay dos en el motor, y `processInputBlock` corre el
 * suyo— y no se notaba porque el doble copiaba el buffer sin DSP.
 *
 * Los dos casos van en el MISMO test a proposito: que el tono pase es lo que
 * impide que "el DC no sale" se cumpla por estar todo en silencio.
 */
TEST(InputNodeDsp, ConstantOffsetIsRemovedWhileTheToneSurvives) {
    InputHarness dcCase;
    const float dcOut = dcCase.pump(20, kAmp, /*dc=*/true);

    InputHarness toneCase;
    const float toneOut = toneCase.pump(20, kAmp);

    EXPECT_LT(dcOut, kAmp * 0.1f)
        << "una componente continua tiene que quedar en el camino, no salir por el monitoreo";
    EXPECT_NEAR(toneOut, kAmp, kAmp * 0.02f)
        << "y el tono tiene que sobrevivir, o lo de arriba se cumple por silencio";
    EXPECT_LT(dcOut, toneOut * 0.2f);
}

/**
 * El noise gate: apagado por defecto, y cuando se lo enciende con un umbral por
 * encima de la senal, cierra.
 */
TEST(InputNodeDsp, TheNoiseGateIsOffByDefaultAndClosesOnSignalBelowItsThreshold) {
    InputHarness open;
    ASSERT_FALSE(open.node().isNoiseGateEnabled()) << "el gate arranca apagado";
    const float ungated = open.pump(20, kAmp);
    ASSERT_NEAR(ungated, kAmp, kAmp * 0.02f);

    InputHarness gated;
    gated.node().setNoiseGateEnabled(true);
    gated.node().setNoiseGateThreshold(-6.1f);   // muy por encima de kAmp (-12,8 dBFS)
    EXPECT_LT(gated.pump(40, kAmp), ungated * 0.2f)
        << "con el umbral arriba de la senal el gate tiene que cerrar";
}

/// El medidor de entrada publica el nivel que se le dio de comer. Lo lee el UI
/// thread, y REQ-001 lo va a leer para decidir si hay senal que afinar.
TEST(InputNodeDsp, TheLevelMeterReadsWhatWasFedIn) {
    InputHarness h;
    ASSERT_LT(h.node().getInputLevelLinear(0), 0.01f) << "arranca en silencio";
    h.pump(40, kAmp);
    EXPECT_GT(h.node().getInputLevelLinear(0), kAmp * 0.3f)
        << "el medidor tiene que moverse con la senal";
    EXPECT_LT(h.node().getInputLevelLinear(0), kAmp * 1.2f)
        << "y no inventar nivel que no hay";
}

/**
 * Un bloque mas grande que lo que el nodo preparo no puede escribir fuera de
 * sus buffers.
 *
 * ESTE TEST NACE DE UN BUG REAL, encontrado al de-duplicar el DSP de entrada.
 * `feedExternalInput` calculaba `numSamples` ANTES de recortar `numFrames` y lo
 * dejaba `const`, asi que el recorte protegia al DSP de abajo pero NO a la
 * copia: el `std::copy` seguia escribiendo el largo original.
 *
 * En debug no se veia: un `assert` disparaba primero. Y `assert` desaparece con
 * `NDEBUG`, o sea justo en el build que shippea. Medido con ASan y `-DNDEBUG`:
 *
 *     ERROR: AddressSanitizer: container-overflow
 *     WRITE of size 8192 ... in InputNode::feedExternalInput InputNode.cpp:656
 *
 * `processInputBlock` tenia la misma clase de agujero y ni siquiera el recorte:
 * solo el `assert`. Los dos caminos comparten ahora `clampToWorkBuffers()`.
 *
 * El test corre bajo ASan en el CI (`cpp-tests-asan`), que es donde una
 * escritura fuera de rango deja de ser invisible.
 */
TEST(InputNodeDsp, ABlockBiggerThanWhatWasPreparedIsClampedInsteadOfOverflowing) {
    InputNode node;
    node.prepare(kSampleRate, kBlockFrames);   // buffers para kBlockFrames
    node.setMonitoringEnabled(true);
    node.setMonitoringVolume(1.0f);
    node.setInputGain(0.0f);

    const int oversized = kBlockFrames * 8;
    std::vector<float> in(static_cast<size_t>(oversized) * 2, 0.17f);

    ASSERT_EQ(node.getFeedClampedBlocks(), 0u) << "todavia no se recorto nada";
    node.feedExternalInput(in.data(), oversized);   // bajo ASan, esto abortaba
    EXPECT_GT(node.getFeedClampedBlocks(), 0u)
        << "el recorte tiene que quedar CONTADO, no solo hecho: sin contador un "
           "device que entrega bloques mas grandes de lo negociado pierde audio "
           "en silencio";

    // Y el nodo sigue sirviendo: recortar no puede dejarlo roto.
    std::vector<float> out(static_cast<size_t>(kBlockFrames) * 2, 0.0f);
    const int got = node.getMonitoringSamples(out.data(), kBlockFrames);
    EXPECT_GT(got, 0) << "despues del recorte el ring tiene que tener algo";
    for (int i = 0; i < got * 2; ++i) {
        ASSERT_TRUE(std::isfinite(out[i])) << "muestra " << i;
    }
}

/**
 * Un nodo al que se le da de comer ANTES de `prepare()` no escribe fuera de
 * rango ni se queda mudo.
 *
 * El constructor dimensionaba `mTempBuffer` y dejaba `mMonitorTempBuffer` en
 * cero, asi que este camino escribia en un buffer vacio en cuanto el volumen de
 * monitoreo no fuera 1,0 — con un `assert` como unica defensa, que en release
 * no existe. Es la misma clase que el recorte de arriba: una mitad preparada y
 * la otra no.
 */
TEST(InputNodeDsp, FeedingBeforePrepareIsSafeAndStillCarriesAudio) {
    InputNode node;                       // SIN prepare()
    node.setMonitoringEnabled(true);
    node.setMonitoringVolume(0.63f);      // != 1,0: fuerza el camino del temp buffer
    node.setInputGain(0.0f);

    std::vector<float> in(static_cast<size_t>(kBlockFrames) * 2);
    for (int f = 0; f < kBlockFrames; ++f) {
        const float v = static_cast<float>(
            kAmp * std::sin(2.0 * M_PI * kToneHz * f / kSampleRate));
        in[static_cast<size_t>(f) * 2] = v;
        in[static_cast<size_t>(f) * 2 + 1] = v;
    }
    node.feedExternalInput(in.data(), kBlockFrames);

    std::vector<float> out(static_cast<size_t>(kBlockFrames) * 2, 0.0f);
    const int got = node.getMonitoringSamples(out.data(), kBlockFrames);
    EXPECT_GT(got, 0) << "un nodo sin preparar no puede tragarse el audio en silencio";
    float peak = 0.0f;
    for (int i = 0; i < got * 2; ++i) {
        ASSERT_TRUE(std::isfinite(out[i])) << "muestra " << i;
        peak = std::fmax(peak, std::fabs(out[i]));
    }
    EXPECT_GT(peak, 0.0f);
}

/**
 * 1.11 — lo que el camino de captura procesa llega al ring del afinador.
 *
 * Dos cosas se afirman juntas y las dos importan:
 *
 *  1. Llega. Sin esto el afinador no tiene senal.
 *  2. Llega DESPUES del DSP de entrada, no antes. El afinador tiene que
 *     analizar lo mismo que el usuario escucha —con la ganancia de entrada
 *     aplicada— y no la senal cruda del conversor. Se distingue poniendo una
 *     ganancia que no sea 0 dB y midiendo cual de las dos amplitudes aparece.
 *
 * Y el ring recibe con el monitoreo APAGADO, que es el caso normal de un
 * afinador: nadie quiere escuchar su propia guitarra por los parlantes mientras
 * afina.
 */
TEST(InputNodeDsp, WhatTheCapturePathProcessedReachesTheAnalysisRing) {
    wma::analysis::AnalysisRing ring;
    InputNode node;
    node.prepare(kSampleRate, kBlockFrames);
    node.setMonitoringEnabled(false);          // el afinador no necesita monitoreo
    node.setInputGain(6.1f);                   // != 0 dB, y no una potencia de dos
    node.setAnalysisRing(&ring);

    const float gain = std::pow(10.0f, 6.1f / 20.0f);

    std::vector<float> in(static_cast<size_t>(kBlockFrames) * 2);
    for (int f = 0; f < kBlockFrames; ++f) {
        const float v = static_cast<float>(
            kAmp * std::sin(2.0 * M_PI * kToneHz * f / kSampleRate));
        in[static_cast<size_t>(f) * 2] = v;
        in[static_cast<size_t>(f) * 2 + 1] = v;
    }
    // Cuatro bloques y se mide el ULTIMO. El DSP de entrada lleva un DC blocker,
    // y su transitorio de arranque le suma un 2 % al primer bloque — medido.
    // Estrechar la ventana al regimen establecido es mas honesto que aflojar la
    // tolerancia hasta que el transitorio quepa.
    const int kBlocks = 4;
    for (int b = 0; b < kBlocks; ++b) node.feedExternalInput(in.data(), kBlockFrames);

    std::vector<float> out(static_cast<size_t>(kBlockFrames) * kBlocks, 0.0f);
    const int got = ring.read(out.data(), kBlockFrames * kBlocks);
    ASSERT_EQ(got, kBlockFrames * kBlocks) << "el ring no recibio los bloques";

    float peak = 0.0f;
    for (int i = kBlockFrames * (kBlocks - 1); i < got; ++i) {
        peak = std::fmax(peak, std::fabs(out[i]));
    }
    EXPECT_NEAR(peak, kAmp * gain, kAmp * gain * 0.02f)
        << "el ring recibio " << peak << "; con la ganancia aplicada seria "
        << (kAmp * gain) << " y sin aplicar " << kAmp;

    // Y desconectar lo desconecta: no puede quedar escribiendo en un ring que
    // el dueño ya considera retirado.
    node.setAnalysisRing(nullptr);
    node.feedExternalInput(in.data(), kBlockFrames);
    EXPECT_EQ(ring.read(out.data(), kBlockFrames), 0)
        << "siguio escribiendo despues de desconectarlo";
    (void)kBlocks;
}
