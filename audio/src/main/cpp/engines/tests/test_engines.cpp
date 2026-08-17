// Smoke + characterization tests for the header-only synth engines. First host
// coverage for engines/ (previously 0 tests). Every engine must: expose sane
// parameter metadata, and stay finite at both parameter extremes over time.
// Oscillator engines must generate sound; Karplus-Strong must ring and decay.

#include "../FMEngine.h"
#include "../SupersawEngine.h"
#include "../WavetableEngine.h"
#include "../GranularEngine.h"
#include "../KarplusStrongEngine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr int kSr = 48000;
constexpr int kBlk = 512;

// Universal safety battery for any SynthEngine.
void engineBattery(SynthEngine& e, const char* label) {
    e.prepare(kSr, kBlk);

    const int n = e.getParameterCount();
    EXPECT_GE(n, 0) << label;
    EXPECT_LE(n, MAX_ENGINE_PARAMS) << label;
    EXPECT_NE(e.getName(), nullptr) << label;
    for (int i = 0; i < n; ++i) {
        const EngineParameterDef d = e.getParameterDef(i);
        EXPECT_NE(d.name, nullptr) << label << " param " << i;
        EXPECT_LE(d.minValue, d.defaultValue) << label << " param " << i;
        EXPECT_LE(d.defaultValue, d.maxValue) << label << " param " << i;
    }

    // Drive params to both extremes; output must stay finite over time.
    std::vector<float> buf(static_cast<size_t>(kBlk) * 2, 0.0f);
    for (float frac : {0.0f, 1.0f}) {
        e.reset();
        for (int i = 0; i < n; ++i) {
            const EngineParameterDef d = e.getParameterDef(i);
            e.setParameter(i, d.minValue + frac * (d.maxValue - d.minValue));
        }
        for (int blk = 0; blk < 40; ++blk) {
            e.process(buf.data(), kBlk, 440.0f, 0.8f);
            for (float s : buf) ASSERT_TRUE(std::isfinite(s)) << label;
        }
    }
}

float blockEnergy(const std::vector<float>& b) {
    float e = 0.0f;
    for (float s : b) e += s * s;
    return e;
}
}  // namespace

TEST(EngineSmoke, FmFiniteAndSane)        { FMEngine e;            engineBattery(e, "FM"); }
TEST(EngineSmoke, SupersawFiniteAndSane)  { SupersawEngine e;      engineBattery(e, "Supersaw"); }
TEST(EngineSmoke, WavetableFiniteAndSane) { WavetableEngine e;     engineBattery(e, "Wavetable"); }
TEST(EngineSmoke, GranularFiniteAndSane)  { GranularEngine e;      engineBattery(e, "Granular"); }
TEST(EngineSmoke, KarplusFiniteAndSane)   { KarplusStrongEngine e; engineBattery(e, "Karplus"); }

// Continuous oscillator engines must actually generate sound.
TEST(EngineSmoke, OscillatorEnginesProduceSound) {
    std::vector<float> buf(static_cast<size_t>(kBlk) * 2, 0.0f);
    auto energyOf = [&](SynthEngine& e) {
        e.prepare(kSr, kBlk);
        e.reset();
        float total = 0.0f;
        for (int blk = 0; blk < 10; ++blk) {
            e.process(buf.data(), kBlk, 440.0f, 0.9f);
            total += blockEnergy(buf);
        }
        return total;
    };
    FMEngine fm;        EXPECT_GT(energyOf(fm), 0.0f);
    SupersawEngine ss;  EXPECT_GT(energyOf(ss), 0.0f);
    WavetableEngine wt; EXPECT_GT(energyOf(wt), 0.0f);
}

// Karplus-Strong is excited on reset(): energy should appear then decay.
TEST(EngineSmoke, KarplusRingsThenDecays) {
    KarplusStrongEngine e;
    e.prepare(kSr, kBlk);
    e.reset();
    std::vector<float> buf(static_cast<size_t>(kBlk) * 2, 0.0f);

    e.process(buf.data(), kBlk, 220.0f, 0.9f);
    const float early = blockEnergy(buf);
    for (int blk = 0; blk < 300; ++blk) e.process(buf.data(), kBlk, 220.0f, 0.9f);
    const float late = blockEnergy(buf);

    EXPECT_GT(early, 0.0f);
    EXPECT_LT(late, early);
}

// ===========================================================================
// El contrato de `SynthEngine` sobre sus smoothers de parametros
//
// Los dos tests de abajo cubren el primitivo COMPARTIDO por los seis engines.
// La deuda estaba ahi, no en Karplus-Strong: el desafine de un semitono que
// documenta `test_engine_pitch.cpp` es UN sintoma de esto, y el fade-in de
// ~2,5 s del `expression` de SoundFont (default 1,0, arrancando en 0) es otro.
// ===========================================================================

namespace {

/// Un engine minimo que expone `smoothParam()` para poder medir el contrato de
/// la clase base sin pasar por el DSP de ninguno de los seis.
class ProbeEngine : public SynthEngine {
public:
    ProbeEngine() {
        mParams[0].store(0.7f, std::memory_order_relaxed);
        mParams[1].store(1.0f, std::memory_order_relaxed);
    }
    void reset() override {}
    void process(float*, int32_t, float, float) override {}
    const char* getName() const override { return "Probe"; }
    int getParameterCount() const override { return 2; }
    EngineParameterDef getParameterDef(int) const override {
        return {"Probe", "PROBE", 0.0f, 1.0f, 0.7f};
    }
    using SynthEngine::smoothParam;
};

}  // namespace

TEST(EngineParams, PrepareSeedsTheSmoothersInsteadOfRampingUpFromZero) {
    // Sin sembrar, TODO parametro arranca en 0 y trepa hacia su valor, sea cual
    // sea. Con el suavizado avanzando por bloque eso duraba segundos: el
    // `expression` de SoundFont (1,0) entraba con un fade-in que nadie pidio y
    // el `brightness` de Karplus (0,5) dejaba la cuerda casi muda Y desafinada.
    ProbeEngine e;
    e.prepare(48000, 512);

    EXPECT_FLOAT_EQ(e.smoothParam(0, 512), 0.7f)
        << "el primer bloque despues de prepare() no entrega el valor del "
        << "parametro: el smoother arranco en otro lado y hay una rampa que "
        << "nadie pidio.";
    EXPECT_FLOAT_EQ(e.smoothParam(1, 512), 1.0f);

    // Y un parametro puesto ANTES de prepare() tambien tiene que quedar
    // sembrado: `SynthEngineDispatcher` construye, configura y recien despues
    // prepara.
    ProbeEngine late;
    late.setParameter(0, 0.25f);
    late.prepare(48000, 512);
    EXPECT_FLOAT_EQ(late.smoothParam(0, 512), 0.25f);
}

TEST(EngineParams, AParameterMoveTakesTheDeclaredTimeRegardlessOfBlockSize) {
    // `kParamSmoothingMs` es una promesa en tiempo REAL. Antes valia eso
    // multiplicado por el tamaño del bloque — 2,56 s con bloques de 512 — y por
    // eso el sonido de los engines dependia del bloque que negociara el device.
    constexpr int kRate = 48000;
    const int framesForOneTau = static_cast<int>(kParamSmoothingMs * 0.001f * kRate);

    float reference = -1.0f;
    for (int block : {1, 16, 64, 512, 1024}) {
        ProbeEngine e;
        e.setParameter(0, 0.0f);
        e.prepare(kRate, block);
        e.setParameter(0, 1.0f);  // salto de perilla

        int done = 0;
        float value = 0.0f;
        while (done < framesForOneTau) {
            const int n = std::min(block, framesForOneTau - done);
            value = e.smoothParam(0, n);
            done += n;
        }

        EXPECT_NEAR(value, 0.632f, 0.005f)
            << "con bloques de " << block << ", tras los "
            << kParamSmoothingMs << " ms declarados el parametro llego a "
            << value << " en vez de ~0,632.";
        if (reference < 0.0f) reference = value;
        EXPECT_NEAR(value, reference, 1e-6f)
            << "el suavizado de parametros depende del tamaño del bloque.";
    }
}
