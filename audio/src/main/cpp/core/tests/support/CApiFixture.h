#pragma once

/**
 * CApiFixture.h — host test support.
 *
 * An engine built the way a real consumer builds one: through
 * wma_engine_create(), not by newing an AudioEngine. That matters because the C
 * API owns more than the engine — it also builds the BackendManager and
 * registers it as the global instance — so a test that constructs the pieces by
 * hand (BackendPathFixture) is testing a different assembly than the one iOS and
 * the JNI actually run.
 *
 * The fake backend still arrives the same way: BackendManager's constructor
 * reaches the substituted platform registration point. See
 * test_platform_backends.cpp.
 */

#include "FakeAudioBackend.h"

#include "api/watermelon_audio.h"
#include "api/watermelon_audio_internal.h"
#include "platform/Logger.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

namespace wma_test {

class CApiFixture : public ::testing::Test {
protected:
    void SetUp() override {
        // The engine logs generously outside the RT path; a no-op sink keeps
        // the ctest output readable without touching production defaults.
        wma::setLogCallback([](wma::LogLevel, const char*, const char*) {});

        resetLastCreatedSystemBackend();
        mWma = wma_engine_create();
        ASSERT_NE(mWma, nullptr);

        mBackend = lastCreatedSystemBackend();
        ASSERT_NE(mBackend, nullptr)
            << "the test platform registration point did not hand the manager a fake";
    }

    void TearDown() override {
        // Destroys the engine and clears the global manager pointer, in that
        // order. AudioEngine's destructor reclaims any stop-fade worker.
        wma_engine_destroy(mWma);
        mWma = nullptr;
        mBackend = nullptr;
        wma::setLogCallback(nullptr);
    }

    /// Bring the engine up over the fake backend with the given fade argument.
    void startAt(int negotiatedSampleRate, int fadeTimeMs) {
        mBackend->setNegotiatedSampleRate(negotiatedSampleRate);
        wma_set_use_backend_manager(mWma, true);
        // BackendType::OBOE — the fake registers itself as the system backend.
        ASSERT_TRUE(wma_select_backend(1));
        ASSERT_EQ(wma_engine_start(mWma, fadeTimeMs), WMA_OK);
    }

    /**
     * Render @p blocks callbacks of @p framesPerBlock frames through the engine.
     *
     * More than a way to advance a fade: this is the only way several
     * subsystems make progress at all. Voice allocation, for one, happens in
     * VoiceManager::processSourceEvents() on the audio thread — updateMultiTouch
     * only hands the touches to the trigger source, so the active-voice count
     * does not move until a block has been rendered.
     */
    void render(int blocks, int framesPerBlock) {
        std::vector<float> buffer(static_cast<size_t>(framesPerBlock) * 2, 0.0f);
        for (int i = 0; i < blocks; ++i) {
            mWma->engine->onAudioReady(buffer.data(), nullptr, framesPerBlock);
        }
    }

    /// Render one block and return the loudest sample in it, as a magnitude.
    /// For the tests that are about what the user would actually hear.
    float renderBlockPeak(int framesPerBlock) {
        std::vector<float> buffer(static_cast<size_t>(framesPerBlock) * 2, 0.0f);
        mWma->engine->onAudioReady(buffer.data(), nullptr, framesPerBlock);
        float peak = 0.0f;
        for (float sample : buffer) {
            peak = std::max(peak, std::abs(sample));
        }
        return peak;
    }

    /**
     * Render @p blocks callbacks with the BACKEND delivering input, the way
     * CoreAudioBackend and the USB backend do — inputData non-null on
     * onAudioReady().
     *
     * This is the only road to MIX-mode monitoring from the host suite: the
     * engine hands those frames to InputNode::feedExternalInput(), which runs
     * the real input DSP and fills the monitoring ring that
     * handleMixMonitoring() sums.
     *
     * EL ESTIMULO ES UN SENO, Y ANTES ERA DC — LA RAZON IMPORTA
     * ---------------------------------------------------------
     * Esta fixture llenaba el buffer de entrada con un valor CONSTANTE, y lo
     * justificaba asi: *"DC is fine and deliberate: the monitored signal never
     * meets the DC blocker (that one runs on the instrument bus, upstream)"*.
     *
     * Eso es FALSO y ahora esta medido. `InputNode::processInputBlock` corre su
     * PROPIO `mDCBlocker` sobre la senal monitoreada — hay dos DC blockers en el
     * motor, no uno. Con el `InputNode` real, 20 bloques de DC a 0,2 salen del
     * ring en **0,0083 y bajando**; el mismo nivel como seno sale en **0,2002**.
     * O sea que el estimulo viejo medía una senal que en un device no puede
     * existir.
     *
     * No se notaba porque la suite sustituia `InputNode.cpp` por un doble que
     * copiaba el buffer sin DSP: el doble devolvia lo conveniente y la creencia
     * falsa quedo escrita como justificacion.
     *
     * POR QUE 187,5 Hz Y NO UNA FRECUENCIA CUALQUIERA
     * -----------------------------------------------
     * A 48 kHz entra EXACTAMENTE un ciclo en un bloque de 256 frames, y la
     * muestra 64 cae justo en el pico. Con eso el pico por bloque es igual a la
     * amplitud pedida, identico en todos los bloques, y las razones de nivel que
     * miden los tests no dependen de donde cayo el corte del buffer. Con una
     * frecuencia arbitraria el pico bailaria unos puntos porcentuales por
     * alineacion de fase — ruido que no dice nada sobre el master bus.
     *
     * @param inputAmplitude amplitud del seno, no el valor de la muestra.
     */
    void renderWithInput(int blocks, int framesPerBlock, float inputAmplitude) {
        std::vector<float> out(static_cast<size_t>(framesPerBlock) * 2, 0.0f);
        std::vector<float> in(static_cast<size_t>(framesPerBlock) * 2, 0.0f);
        for (int i = 0; i < blocks; ++i) {
            fillInput(in, framesPerBlock, inputAmplitude);
            std::fill(out.begin(), out.end(), 0.0f);
            mWma->engine->onAudioReady(out.data(), in.data(), framesPerBlock);
        }
    }

    /// renderWithInput() for one block, returning the loudest OUTPUT sample.
    float renderBlockPeakWithInput(int framesPerBlock, float inputAmplitude) {
        std::vector<float> out(static_cast<size_t>(framesPerBlock) * 2, 0.0f);
        std::vector<float> in(static_cast<size_t>(framesPerBlock) * 2, 0.0f);
        fillInput(in, framesPerBlock, inputAmplitude);
        mWma->engine->onAudioReady(out.data(), in.data(), framesPerBlock);
        float peak = 0.0f;
        for (float sample : out) {
            peak = std::max(peak, std::abs(sample));
        }
        return peak;
    }

    /// renderWithInput() para un bloque, devolviendo el RMS del canal L de la
    /// SALIDA. Existe porque el medidor del motor publica RMS: comparar RMS
    /// contra PICO solo da lo mismo si la senal es constante, y dejo de serlo.
    float renderBlockRmsWithInput(int framesPerBlock, float inputAmplitude) {
        std::vector<float> out(static_cast<size_t>(framesPerBlock) * 2, 0.0f);
        std::vector<float> in(static_cast<size_t>(framesPerBlock) * 2, 0.0f);
        fillInput(in, framesPerBlock, inputAmplitude);
        mWma->engine->onAudioReady(out.data(), in.data(), framesPerBlock);
        double sum = 0.0;
        for (int f = 0; f < framesPerBlock; ++f) {
            const double v = out[static_cast<size_t>(f) * 2];
            sum += v * v;
        }
        return static_cast<float>(std::sqrt(sum / framesPerBlock));
    }

private:
    /// Seno de kInputToneHz, con la fase CONTINUA entre bloques: un salto de
    /// fase en el borde seria un transitorio que el DSP de entrada veria como
    /// senal, y es justo el artefacto que este archivo acaba de dejar de tener.
    void fillInput(std::vector<float>& in, int framesPerBlock, float amplitude) {
        for (int f = 0; f < framesPerBlock; ++f) {
            const float v = static_cast<float>(amplitude * std::sin(mInputPhase));
            mInputPhase += 2.0 * M_PI * kInputToneHz / kInputToneRate;
            if (mInputPhase >= 2.0 * M_PI) mInputPhase -= 2.0 * M_PI;
            in[static_cast<size_t>(f) * 2] = v;
            in[static_cast<size_t>(f) * 2 + 1] = v;
        }
    }

    static constexpr double kInputToneHz = 187.5;
    static constexpr double kInputToneRate = 48000.0;
    double mInputPhase = 0.0;

protected:
    WmaEngine* mWma = nullptr;
    FakeAudioBackend* mBackend = nullptr;
};

}  // namespace wma_test
