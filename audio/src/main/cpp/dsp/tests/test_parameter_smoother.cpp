#include <gtest/gtest.h>
#include "ParameterSmoother.h"
#include <algorithm>
#include <cmath>

TEST(ParameterSmoother, ConvergesToTarget) {
    ParameterSmoother smoother(0.99f);
    smoother.reset(0.0f);

    float value = 0.0f;
    for (int i = 0; i < 10000; i++) {
        value = smoother.process(1.0f);
    }

    EXPECT_NEAR(value, 1.0f, 0.001f);
}

TEST(ParameterSmoother, StartsAtResetValue) {
    ParameterSmoother smoother(0.99f);
    smoother.reset(0.5f);

    // First call with target=0.5 should return ~0.5
    float value = smoother.process(0.5f);
    EXPECT_FLOAT_EQ(value, 0.5f);
}

TEST(ParameterSmoother, NoOvershoot) {
    ParameterSmoother smoother(0.99f);
    smoother.reset(0.0f);

    for (int i = 0; i < 100000; i++) {
        float value = smoother.process(1.0f);
        EXPECT_GE(value, 0.0f) << "Undershoot at sample " << i;
        EXPECT_LE(value, 1.0f) << "Overshoot at sample " << i;
    }
}

TEST(ParameterSmoother, MonotonicIncrease) {
    ParameterSmoother smoother(0.99f);
    smoother.reset(0.0f);

    float prev = 0.0f;
    for (int i = 0; i < 1000; i++) {
        float value = smoother.process(1.0f);
        EXPECT_GE(value, prev - 1e-6f) << "Non-monotonic at sample " << i;
        prev = value;
    }
}

TEST(ParameterSmoother, FastCoefficientConvergesFaster) {
    ParameterSmoother fast(0.9f);   // Fast
    ParameterSmoother slow(0.999f); // Slow
    fast.reset(0.0f);
    slow.reset(0.0f);

    float fastVal = 0.0f, slowVal = 0.0f;
    for (int i = 0; i < 100; i++) {
        fastVal = fast.process(1.0f);
        slowVal = slow.process(1.0f);
    }

    EXPECT_GT(fastVal, slowVal);
}

TEST(ParameterSmoother, HandlesNegativeTarget) {
    ParameterSmoother smoother(0.99f);
    smoother.reset(1.0f);

    float value = 0.0f;
    for (int i = 0; i < 100000; i++) {
        value = smoother.process(-1.0f);
    }

    EXPECT_NEAR(value, -1.0f, 0.001f);
}

// ===========================================================================
// processBlock() — el defecto que cierra, y por que estos tests son estos
//
// `setSmoothingTime()` calcula el coeficiente para llamadas POR MUESTRA. Todo
// `SynthEngine` lee sus parametros UNA VEZ POR BLOQUE, asi que llamaba a
// `process()` una sola vez cada `numFrames` muestras y el tiempo de suavizado
// real quedaba multiplicado por el tamaño del bloque: los 5 ms declarados eran
// 2,56 s con bloques de 512, y el numero CAMBIABA con el bloque.
//
// De ahi salio un semitono de desafinacion en Karplus-Strong (ver
// engines/tests/test_engine_pitch.cpp): `brightness` valia ~0,02 en vez de 0,5
// durante el primer medio segundo.
// ===========================================================================

TEST(ParameterSmoother, ABlockAdvanceEqualsThatManySingleSteps) {
    // La propiedad que hace correcto al atajo: aplicar n veces
    // `c*x + (1-c)*t` con `t` constante da exactamente `c^n*x + (1-c^n)*t`.
    // Si esto no valiera, `processBlock()` seria una aproximacion y no un
    // reemplazo.
    for (int n : {2, 16, 64, 512, 4096}) {
        ParameterSmoother block, oneByOne;
        block.setSmoothingTime(5.0f, 48000.0f);
        oneByOne.setSmoothingTime(5.0f, 48000.0f);
        block.reset(0.2f);
        oneByOne.reset(0.2f);

        const float blockValue = block.processBlock(0.9f, n);
        float stepValue = 0.0f;
        for (int i = 0; i < n; ++i) stepValue = oneByOne.process(0.9f);

        // Medido: 0 hasta n=64, 1,2e-7 en n=512, 7,0e-6 en n=4096. La
        // diferencia es la acumulacion de redondeo de las n multiplicaciones
        // sueltas — `powf` es el MAS exacto de los dos, no el menos.
        EXPECT_NEAR(blockValue, stepValue, 1e-5f)
            << "n = " << n << ": avanzar el bloque de una no da lo mismo que "
            << "avanzarlo muestra por muestra.";
    }
}

TEST(ParameterSmoother, TheDeclaredSmoothingTimeIsTrueAndDoesNotDependOnTheBlockSize) {
    // ESTE es el test que se habria puesto rojo con el defecto. Un one-pole
    // llega al 63,2 % de su target en una constante de tiempo; se le pide eso
    // mismo despues de avanzar 5 ms de TIEMPO REAL, repartidos en bloques de
    // distinto tamaño.
    //
    // Antes de `processBlock()`: con bloques de 512 se avanzaban 512 veces
    // menos muestras, asi que a los 5 ms el valor era ~0,0023 en vez de 0,63.
    for (int rate : {44100, 48000, 96000}) {
        float reference = -1.0f;
        for (int block : {1, 16, 64, 512, 1024}) {
            ParameterSmoother s;
            s.setSmoothingTime(5.0f, static_cast<float>(rate));
            s.reset(0.0f);

            const int framesNeeded = static_cast<int>(0.005 * rate);
            int done = 0;
            float value = 0.0f;
            while (done < framesNeeded) {
                const int n = std::min(block, framesNeeded - done);
                value = s.processBlock(1.0f, n);
                done += n;
            }

            EXPECT_NEAR(value, 0.632f, 0.005f)
                << "a " << rate << " Hz con bloques de " << block << ", tras los "
                << "5 ms declarados el smoother llego a " << value
                << " en vez de ~0,632.";
            if (reference < 0.0f) reference = value;
            EXPECT_NEAR(value, reference, 1e-6f)
                << "el suavizado depende del tamaño del bloque (" << block
                << " da " << value << ", el primero dio " << reference << ").";
        }
    }
}

TEST(ParameterSmoother, ABlockOfZeroOrOneFrameIsHandledWithoutMoving) {
    ParameterSmoother s;
    s.setSmoothingTime(5.0f, 48000.0f);
    s.reset(0.4f);

    // Un bloque de 0 frames no puede avanzar nada.
    EXPECT_FLOAT_EQ(s.processBlock(1.0f, 0), 0.4f);
    EXPECT_FLOAT_EQ(s.getCurrent(), 0.4f);
    // Y un bloque negativo tampoco: `numFrames` viene de afuera.
    EXPECT_FLOAT_EQ(s.processBlock(1.0f, -7), 0.4f);

    // Un frame tiene que dar exactamente lo mismo que `process()`.
    ParameterSmoother twin;
    twin.setSmoothingTime(5.0f, 48000.0f);
    twin.reset(0.4f);
    EXPECT_FLOAT_EQ(s.processBlock(1.0f, 1), twin.process(1.0f));
}
