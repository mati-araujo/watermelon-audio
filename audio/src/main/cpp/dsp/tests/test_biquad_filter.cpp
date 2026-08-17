#include <gtest/gtest.h>
#include "BiquadFilter.h"
#include <cmath>
#include <vector>

class BiquadFilterTest : public ::testing::Test {
protected:
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int BLOCK_SIZE = 512;
};

TEST_F(BiquadFilterTest, LowPassAttenuatesHighFrequencies) {
    BiquadFilter filter;
    filter.setSampleRate(SAMPLE_RATE);
    filter.setLowpass(1000.0f);

    // Generate 10kHz sine (should be attenuated by LPF at 1kHz)
    std::vector<float> signal(BLOCK_SIZE);
    for (int i = 0; i < BLOCK_SIZE; i++) {
        signal[i] = std::sin(2.0f * M_PI * 10000.0f * i / SAMPLE_RATE);
    }

    float inputPeak = 0.0f;
    for (float s : signal) inputPeak = std::max(inputPeak, std::abs(s));

    for (int i = 0; i < BLOCK_SIZE; i++) {
        signal[i] = filter.process(signal[i]);
    }

    float outputPeak = 0.0f;
    for (float s : signal) outputPeak = std::max(outputPeak, std::abs(s));

    // 10kHz should be heavily attenuated by a 1kHz LPF
    EXPECT_LT(outputPeak, inputPeak * 0.1f);
}

TEST_F(BiquadFilterTest, LowPassPassesLowFrequencies) {
    BiquadFilter filter;
    filter.setSampleRate(SAMPLE_RATE);
    filter.setLowpass(10000.0f);

    // Generate 100Hz sine — should pass through
    std::vector<float> signal(BLOCK_SIZE);

    // Let filter settle
    for (int i = 0; i < BLOCK_SIZE; i++) {
        float s = std::sin(2.0f * M_PI * 100.0f * i / SAMPLE_RATE);
        filter.process(s);
    }

    // Measure settled output
    for (int i = 0; i < BLOCK_SIZE; i++) {
        signal[i] = std::sin(2.0f * M_PI * 100.0f * (i + BLOCK_SIZE) / SAMPLE_RATE);
        signal[i] = filter.process(signal[i]);
    }

    float outputPeak = 0.0f;
    for (float s : signal) outputPeak = std::max(outputPeak, std::abs(s));

    EXPECT_GT(outputPeak, 0.85f);
}

TEST_F(BiquadFilterTest, StabilityWithExtremeInput) {
    BiquadFilter filter;
    filter.setSampleRate(SAMPLE_RATE);
    filter.setLowpass(1000.0f, 10.0f);  // High Q

    float result = 0.0f;
    for (int i = 0; i < 10000; i++) {
        float input = (i % 2 == 0) ? 1000.0f : -1000.0f;
        result = filter.process(input);
        EXPECT_TRUE(std::isfinite(result)) << "NaN/Inf at sample " << i;
    }
}

TEST_F(BiquadFilterTest, HighPassAttenuatesLowFrequencies) {
    BiquadFilter filter;
    filter.setSampleRate(SAMPLE_RATE);
    filter.setHighpass(5000.0f);

    // Generate 100Hz sine (should be attenuated by HPF at 5kHz)
    std::vector<float> signal(BLOCK_SIZE);
    for (int i = 0; i < BLOCK_SIZE; i++) {
        signal[i] = std::sin(2.0f * M_PI * 100.0f * i / SAMPLE_RATE);
    }

    for (int i = 0; i < BLOCK_SIZE; i++) {
        signal[i] = filter.process(signal[i]);
    }

    float outputPeak = 0.0f;
    for (float s : signal) outputPeak = std::max(outputPeak, std::abs(s));

    EXPECT_LT(outputPeak, 0.1f);
}

// ===========================================================================
// WD-3.5 — el clamp contra Nyquist tiene que sobrevivir a un cambio de rate
// ===========================================================================
//
// De donde sale esto: `clampFrequency()` vivia solo en los setters, o sea que
// acotaba contra el rate vigente EN ESE MOMENTO. El idioma que usa medio repo
// es el contrario — configurar los filtros en el constructor, a 48 kHz, y
// llamar a `setSampleRate()` cuando el backend negocia otro rate.
//
// Ese hueco era la causa de CUATRO de las siete entradas de
// `effects/tests/nyquist-baseline.txt` (VOCODER, HPF_DELAY, PLATE_REVERB y
// SHIMMER_REVERB) y no se veia desde ninguno de los cuatro: los cuatro llaman a
// un setter que SI clampea. La deuda estaba en el primitivo que comparten.

namespace {

/// Martilla el filtro con una cuadrada a Nyquist y dice si se escapo.
/// Nyquist y no ruido porque un polo que salio del circulo unitario esta casi
/// siempre en la zona alta, y excitarlo ahi hace que se vea en cientos de
/// muestras en vez de en miles.
bool divergesUnderNyquistHammer(BiquadFilter& f, int samples = 4096) {
    for (int i = 0; i < samples; ++i) {
        const float y = f.process((i % 2 < 1) ? 0.95f : -0.95f);
        if (!std::isfinite(y) || std::abs(y) > 1000.0f) return true;
    }
    return false;
}

}  // namespace

TEST_F(BiquadFilterTest, LoweringTheSampleRateReClampsAFrequencyThatWasAlreadySet) {
    // El repro exacto de los cuatro efectos, en tres lineas. Un LPF de 12 kHz
    // configurado a 48 y llevado a 16: omega = 1,5 pi, sin(omega) = -1, el alpha
    // del cookbook RBJ queda negativo y los polos salen del circulo unitario.
    BiquadFilter filter(48000.0f);
    filter.setLowpass(12000.0f, 0.707f);
    filter.setSampleRate(16000.0f);

    EXPECT_FALSE(divergesUnderNyquistHammer(filter))
        << "el filtro diverge despues de bajar el rate por debajo del doble de "
        << "su cutoff. `setSampleRate()` dejo de re-aplicar el clamp contra "
        << "Nyquist, y eso reabre las cuatro entradas de nyquist-baseline.txt "
        << "que se pagaron en WD-3.5 (VOCODER, HPF_DELAY, PLATE y SHIMMER).";
}

TEST_F(BiquadFilterTest, TheClampHoldsAcrossEveryRateForEveryFilterType) {
    // No alcanza con el LPF: el mismo omega alimenta las siete formas del
    // cookbook. Un clamp que cubriera solo la que tiene repro dejaria las otras
    // seis esperando a que alguien las use a un rate bajo.
    const float rates[] = {8000.0f, 11025.0f, 16000.0f, 22050.0f, 32000.0f, 44100.0f};

    for (float rate : rates) {
        {
            BiquadFilter f(48000.0f); f.setLowpass(19000.0f, 4.0f); f.setSampleRate(rate);
            EXPECT_FALSE(divergesUnderNyquistHammer(f)) << "LPF a " << rate;
        }
        {
            BiquadFilter f(48000.0f); f.setHighpass(19000.0f, 4.0f); f.setSampleRate(rate);
            EXPECT_FALSE(divergesUnderNyquistHammer(f)) << "HPF a " << rate;
        }
        {
            BiquadFilter f(48000.0f); f.setBandpass(19000.0f, 4.0f); f.setSampleRate(rate);
            EXPECT_FALSE(divergesUnderNyquistHammer(f)) << "BPF a " << rate;
        }
        {
            BiquadFilter f(48000.0f); f.setNotch(19000.0f, 4.0f); f.setSampleRate(rate);
            EXPECT_FALSE(divergesUnderNyquistHammer(f)) << "NOTCH a " << rate;
        }
        {
            BiquadFilter f(48000.0f); f.setPeaking(19000.0f, 4.0f, 12.0f); f.setSampleRate(rate);
            EXPECT_FALSE(divergesUnderNyquistHammer(f)) << "PEAK a " << rate;
        }
        {
            BiquadFilter f(48000.0f); f.setLowShelf(19000.0f, 0.7f, 12.0f); f.setSampleRate(rate);
            EXPECT_FALSE(divergesUnderNyquistHammer(f)) << "LOW_SHELF a " << rate;
        }
        {
            BiquadFilter f(48000.0f); f.setHighShelf(19000.0f, 0.7f, 12.0f); f.setSampleRate(rate);
            EXPECT_FALSE(divergesUnderNyquistHammer(f)) << "HIGH_SHELF a " << rate;
        }
    }
}

TEST_F(BiquadFilterTest, GoingThroughALowRateDoesNotDegradeTheFilterForever) {
    // La mitad del contrato que un test de "no explota" no puede ver, y la que
    // decide el diseño: el clamp se aplica a lo que se USA, no a lo que se
    // GUARDO. Clampear en su lugar seria destructivo — un device que arranca en
    // Bluetooth SCO (16 kHz) y despues pasa a 48 dejaria el filtro clavado en
    // 7.840 Hz, y nadie volveria a pedirle el cutoff que ya habia pedido.
    //
    // Se mide por comportamiento y no leyendo un miembro: la respuesta despues
    // de la vuelta tiene que ser la MISMA que la de un filtro que nunca bajo.
    BiquadFilter roundTrip(48000.0f);
    roundTrip.setLowpass(12000.0f, 0.707f);
    roundTrip.setSampleRate(16000.0f);
    roundTrip.setSampleRate(48000.0f);

    BiquadFilter reference(48000.0f);
    reference.setLowpass(12000.0f, 0.707f);

    for (float probe : {100.0f, 1000.0f, 5000.0f, 12000.0f, 20000.0f}) {
        EXPECT_NEAR(roundTrip.getFrequencyResponse(probe),
                    reference.getFrequencyResponse(probe), 1e-6f)
            << "en " << probe << " Hz, el filtro que paso por 16 kHz y volvio a "
            << "48 no responde como el que nunca bajo. El clamp se volvio "
            << "DESTRUCTIVO: se esta acotando la frecuencia guardada en vez de "
            << "la que se usa para diseñar los coeficientes.";
    }
}

TEST_F(BiquadFilterTest, TheEffectiveCutoffSaturatesBelowNyquistInsteadOfFollowingTheRequest) {
    // Y la propiedad positiva: por encima del techo, pedir mas no mueve el codo.
    // Es lo que hace que el clamp sea un clamp y no un "no explota" por suerte.
    //
    // Se mide sobre la respuesta, que es lo que oye el usuario, y no sobre un
    // getter — un getter puede devolver lo pedido (y de hecho lo devuelve) sin
    // que eso diga nada sobre el filtro que se armo.
    constexpr float kRate = 16000.0f;

    BiquadFilter atCeiling(kRate);
    atCeiling.setLowpass(kRate * 0.49f, 0.707f);

    BiquadFilter wayAbove(kRate);
    wayAbove.setLowpass(20000.0f, 0.707f);  // muy por encima de Nyquist

    for (float probe : {100.0f, 1000.0f, 3000.0f, 7000.0f}) {
        EXPECT_NEAR(wayAbove.getFrequencyResponse(probe),
                    atCeiling.getFrequencyResponse(probe), 1e-6f)
            << "pedir 20 kHz a " << kRate << " Hz de rate da una respuesta "
            << "distinta que pedir el techo (0,49 * fs). O el techo se movio, o "
            << "el clamp dejo de aplicarse: en los dos casos hay que volver a "
            << "medir antes de tocar nada.";
    }
}
