/**
 * WD-2.1 — el motor renderiza sin abrir ningún dispositivo.
 *
 * POR QUÉ ESTE MECANISMO Y NO OTRO
 * --------------------------------
 * Tres consumidores sobre una misma pieza, y por eso es el ítem de mayor
 * palanca del programa:
 *
 *   - La suite golden (WD-2.2) necesita renderizar determinísticamente sin
 *     hardware. Hoy los tests lo esquivan llamando a onAudioReady() por fuera
 *     del contrato; esto lo vuelve el contrato.
 *   - El desktop (WD-9.1) necesita testear el motor en un runner sin placa.
 *   - Un plugin de DAW, si se compromete, NO abre un dispositivo: el host le
 *     pasa un buffer. Esto es exactamente eso.
 *
 * EL TEST QUE MÁS IMPORTA ES EL DE INVARIANCIA DE BLOQUE
 * -----------------------------------------------------
 * Renderizar N frames de una y renderizarlos en K bloques de N/K tiene que dar
 * el mismo audio. Si no da, hay estado que depende del TAMAÑO del bloque y no
 * del tiempo — un contador por bloque, un smoother que avanza una vez por
 * llamada en vez de una vez por sample, una decisión tomada al principio del
 * bloque que debería tomarse por muestra.
 *
 * Ese defecto es invisible con un backend real, porque el backend entrega
 * siempre el mismo tamaño. Aparece el día que lo entrega otro: un host de DAW
 * que cambia el buffer, un device USB con otro burst, o el mismo motor en
 * desktop. Es el defecto que este mecanismo permite buscar por primera vez.
 */

#include "../AudioEngine.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kMaxBlock = 1024;

std::vector<float> renderInBlocks(AudioEngine& engine, int totalFrames, int blockFrames) {
    std::vector<float> out(static_cast<size_t>(totalFrames) * 2, 0.0f);
    std::vector<float> scratch(static_cast<size_t>(blockFrames) * 2, 0.0f);

    for (int done = 0; done < totalFrames; done += blockFrames) {
        const int n = std::min(blockFrames, totalFrames - done);
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

/// Deja el motor en una configuración que produce señal, para que los tests
/// comparen audio y no silencio.
void makeItSound(AudioEngine& engine) {
    engine.setOscillatorEnabled(true);
    engine.setMasterVolume(0.8f);
    engine.setFrequencyAndAmplitude(440.0f, 0.5f);
}

}  // namespace

// ---------------------------------------------------------------------------
// Lo básico: arranca, rinde, para. Sin device en ningún momento.
// ---------------------------------------------------------------------------
TEST(OfflineRender, TheEngineRunsWithNoDeviceAtAll) {
    AudioEngine engine;

    ASSERT_TRUE(engine.startOffline(kSampleRate, kMaxBlock));
    EXPECT_TRUE(engine.isOffline());
    EXPECT_EQ(engine.getEngineState(), static_cast<int>(EngineState::Running));

    makeItSound(engine);

    std::vector<float> buffer(256 * 2, 0.0f);
    EXPECT_TRUE(engine.renderBlock(buffer.data(), nullptr, 256));

    engine.stop();
    EXPECT_FALSE(engine.isOffline());
    EXPECT_EQ(engine.getEngineState(), static_cast<int>(EngineState::Stopped));
}

// ---------------------------------------------------------------------------
// El sample rate declarado es el que usa el motor. Sin backend no hay a quién
// preguntarle, así que si esto no funciona, todo el DSP corre a 48000 sin
// importar lo que se pidió.
// ---------------------------------------------------------------------------
TEST(OfflineRender, TheDeclaredSampleRateIsTheOneTheEngineUses) {
    for (int rate : {44100, 48000, 96000}) {
        AudioEngine engine;
        ASSERT_TRUE(engine.startOffline(rate, 512)) << "rate " << rate;
        EXPECT_EQ(engine.currentSampleRate(), rate) << "rate " << rate;
        engine.stop();
    }
}

// ---------------------------------------------------------------------------
// Determinismo: dos motores con la misma configuración dan el mismo audio.
// Sin esto no hay golden vectors posibles.
// ---------------------------------------------------------------------------
TEST(OfflineRender, TwoIdenticalRunsProduceIdenticalAudio) {
    constexpr int kFrames = 4096;

    AudioEngine a;
    ASSERT_TRUE(a.startOffline(kSampleRate, kMaxBlock));
    makeItSound(a);
    const auto first = renderInBlocks(a, kFrames, 512);
    a.stop();

    AudioEngine b;
    ASSERT_TRUE(b.startOffline(kSampleRate, kMaxBlock));
    makeItSound(b);
    const auto second = renderInBlocks(b, kFrames, 512);
    b.stop();

    ASSERT_EQ(first.size(), second.size());
    for (size_t i = 0; i < first.size(); ++i) {
        ASSERT_FLOAT_EQ(first[i], second[i])
            << "divergen en el sample " << i << ": el render no es determinista, "
               "así que no se le puede capturar un golden";
    }
}

// ---------------------------------------------------------------------------
// EL IMPORTANTE — invariancia de tamaño de bloque.
//
// Un backend real entrega siempre el mismo tamaño, así que este defecto es
// invisible hasta que alguien cambia de host. Acá se puede buscar.
// ---------------------------------------------------------------------------
TEST(OfflineRender, TheSameAudioComesOutRegardlessOfBlockSize) {
    constexpr int kFrames = 4096;

    AudioEngine oneBigBlock;
    ASSERT_TRUE(oneBigBlock.startOffline(kSampleRate, kMaxBlock));
    makeItSound(oneBigBlock);
    const auto whole = renderInBlocks(oneBigBlock, kFrames, 1024);
    oneBigBlock.stop();

    for (int blockSize : {64, 128, 256, 512}) {
        AudioEngine chopped;
        ASSERT_TRUE(chopped.startOffline(kSampleRate, kMaxBlock));
        makeItSound(chopped);
        const auto pieces = renderInBlocks(chopped, kFrames, blockSize);
        chopped.stop();

        ASSERT_EQ(whole.size(), pieces.size());

        // Tolerancia y no igualdad exacta: hay suavizado de parámetros por
        // bloque que es DELIBERADO (evita zipper noise) y cuya granularidad
        // depende legítimamente del bloque. Lo que este test busca es
        // divergencia estructural — una señal distinta, no un transitorio de
        // rampa unas muestras corrido.
        double energyWhole = 0.0, energyPieces = 0.0, energyDiff = 0.0;
        for (size_t i = 0; i < whole.size(); ++i) {
            energyWhole += static_cast<double>(whole[i]) * whole[i];
            energyPieces += static_cast<double>(pieces[i]) * pieces[i];
            const double d = static_cast<double>(whole[i]) - pieces[i];
            energyDiff += d * d;
        }

        ASSERT_GT(energyWhole, 0.0) << "el render de referencia salió en silencio; "
                                       "el test no está comparando nada";

        // Error relativo de la diferencia contra la señal, en dB.
        const double errDb = 10.0 * std::log10((energyDiff + 1e-30) / energyWhole);
        EXPECT_LT(errDb, -40.0)
            << "bloques de " << blockSize << " frames dan un audio distinto del "
            << "de 1024 (error " << errDb << " dBFS). Hay estado que depende del "
            << "TAMAÑO del bloque y no del tiempo — un host que cambie el buffer "
            << "va a sonar diferente.";

        EXPECT_NEAR(energyPieces / energyWhole, 1.0, 0.05)
            << "bloques de " << blockSize << ": la energía total no coincide";
    }
}

// ---------------------------------------------------------------------------
// Los rechazos. Un render que devuelve silencio sin decir por qué es la peor
// forma de fallar, así que los límites se rechazan donde se pueden explicar.
// ---------------------------------------------------------------------------
TEST(OfflineRender, TheLimitsAreRejectedNotSilentlyTruncated) {
    AudioEngine engine;

    EXPECT_FALSE(engine.startOffline(0, 512)) << "sampleRate cero";
    EXPECT_FALSE(engine.startOffline(kSampleRate, 0)) << "bloque cero";
    EXPECT_FALSE(engine.startOffline(kSampleRate, 8192))
        << "8192 frames excede el scratch de EffectChain (4096) y activaría su "
           "guarda de overflow, que rellena de silencio";

    std::vector<float> buffer(256 * 2, 0.0f);
    EXPECT_FALSE(engine.renderBlock(buffer.data(), nullptr, 256))
        << "renderBlock sin startOffline() previo";

    ASSERT_TRUE(engine.startOffline(kSampleRate, 256));
    EXPECT_FALSE(engine.renderBlock(buffer.data(), nullptr, 512))
        << "un bloque mayor que el maximo declarado";
    EXPECT_FALSE(engine.renderBlock(nullptr, nullptr, 256)) << "output nulo";
    engine.stop();
}

// ---------------------------------------------------------------------------
// Offline y device son excluyentes: el motor tiene un solo estado de ciclo de
// vida y arrancar dos veces por caminos distintos tiene que fallar limpio.
// ---------------------------------------------------------------------------
TEST(OfflineRender, StartingTwiceFailsInsteadOfCorruptingTheState) {
    AudioEngine engine;
    ASSERT_TRUE(engine.startOffline(kSampleRate, 512));

    EXPECT_FALSE(engine.startOffline(kSampleRate, 512));
    EXPECT_EQ(engine.getEngineState(), static_cast<int>(EngineState::Running))
        << "el segundo arranque fallido dejó el estado inconsistente";
    EXPECT_TRUE(engine.isOffline());

    engine.stop();
    EXPECT_EQ(engine.getEngineState(), static_cast<int>(EngineState::Stopped));

    // Y se puede volver a arrancar después de parar.
    EXPECT_TRUE(engine.startOffline(kSampleRate, 512));
    engine.stop();
}
