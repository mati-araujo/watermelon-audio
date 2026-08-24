/**
 * test_parallel_branch_alignment.cpp — WD-3.1 / REQ-011: las ramas se suman ALINEADAS.
 *
 * ## Que se mide
 *
 * `Effect.h` declara que sumar ramas con distinta latencia es un filtro peine, y
 * `EffectChain::accumulateBranch` —que alinea Y suma en una sola operacion— existe para
 * evitarlo. Esto lo MIDE, en los modos que suman.
 *
 * En vez de comparar contra una o dos candidatas fijas, se BARRE el retardo aplicado a la rama
 * rapida y se reporta cual explica mejor la salida real. El test se autodiagnostica:
 *
 *   d* == maxLat - latRapida  → alineado (correcto)
 *   d* == 0                   → no compensa
 *   d* == 2*(maxLat - lat)    → compensa dos veces
 *
 * ## Dos trampas que este archivo respeta
 *
 * 1. 🔴 **El crossfade de cambio de modo.** `EffectChain::process` arranca en SERIAL y, al pedir
 *    otro modo, cruza los dos durante 30 ms: en el PRIMER bloque `progress` vale 0, o sea que la
 *    salida es 100% del modo VIEJO. Un test que pide un modo y procesa un bloque **mide SERIAL**.
 *    Medido: sin calentamiento, poner la rama entera en cero no cambiaba NI UN SAMPLE.
 * 2. 🪤 **Un impulso no da "una llegada por rama"**: el filtro repica y el sample-and-hold de
 *    `DECI_HPF` hace escalera. Contar llegadas dio 28 entre la muestra 4 y la 969.
 */

#include "effects/EffectChain.h"
#include "effects/EffectTypes.h"
#include "effects/DeciHpfEffect.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kFrames = 2048;
constexpr int kStereo = kFrames * 2;

/** A 48 kHz deja la latencia de DECI_HPF en `48000/1000 - 1` = 47. */
constexpr float kDeciTargetHz = 1000.0f;

/** Hasta donde se barre el retardo candidato. */
constexpr int kMaxProbe = 160;

using Buf = std::vector<float>;

Buf impulse() {
    Buf in(kStereo, 0.0f);
    in[0] = 1.0f;
    in[1] = 1.0f;
    return in;
}

/** Un bloque de silencio, para dejar atras el crossfade de cambio de modo. */
void warmUp(EffectChain& chain) {
    Buf silence(kStereo, 0.0f), sink(kStereo, 0.0f);
    chain.process(silence.data(), sink.data(), kFrames);
}

/** Arma una cadena con los efectos dados; el slot `deciSlot` es el DECI_HPF con latencia. */
void build(EffectChain& chain, RoutingMode mode, const std::vector<EffectType>& fx, int deciSlot) {
    chain.setSampleRate(kSampleRate);
    chain.setRoutingMode(mode);
    for (size_t i = 0; i < fx.size(); ++i) {
        EXPECT_TRUE(chain.addEffect(fx[i]));
    }
    if (deciSlot >= 0) {
        chain.setParameter(static_cast<size_t>(deciSlot),
                           DeciHpfEffect::PARAM_SAMPLE_RATE, kDeciTargetHz);
    }
}

/** Renderiza `in` por la cadena ya armada, despues del calentamiento. */
Buf run(EffectChain& chain, const Buf& in) {
    warmUp(chain);
    Buf out(kStereo, 0.0f);
    chain.process(const_cast<float*>(in.data()), out.data(), kFrames);
    return out;
}

/** Una rama sola: cadena SERIAL de esos efectos, que no compensa nada. */
Buf branch(const std::vector<EffectType>& fx, int deciSlot, const Buf& in) {
    EffectChain c;
    build(c, RoutingMode::SERIAL, fx, deciSlot);
    return run(c, in);
}

Buf delayed(const Buf& src, int d) {
    Buf out(src.size(), 0.0f);
    for (size_t f = static_cast<size_t>(d); f < src.size() / 2; ++f) {
        out[f * 2]     = src[(f - d) * 2];
        out[f * 2 + 1] = src[(f - d) * 2 + 1];
    }
    return out;
}

double mse(const Buf& x, const Buf& y) {
    double acc = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double e = static_cast<double>(x[i]) - y[i];
        acc += e * e;
    }
    return acc / static_cast<double>(x.size());
}

/**
 * Barre el retardo aplicado a la rama rapida y devuelve el que mejor explica `got`.
 * `mk(d)` arma la referencia con la rama rapida retrasada `d`.
 */
template <typename MakeRef>
int bestDelay(const Buf& got, MakeRef mk, double* outErr, int expected, double* errAtExpected) {
    int best = -1;
    double bestErr = 0.0;
    for (int d = 0; d <= kMaxProbe; ++d) {
        const double e = mse(got, mk(d));
        if (d == expected) *errAtExpected = e;
        if (best < 0 || e < bestErr) { best = d; bestErr = e; }
    }
    *outErr = bestErr;
    return best;
}

Buf mean2(const Buf& a, const Buf& b) {
    Buf o(a.size(), 0.0f);
    for (size_t i = 0; i < a.size(); ++i) o[i] = (a[i] + b[i]) * 0.5f;
    return o;
}

void report(const char* mode, int expected, int found, double err, double errExp) {
    // Cuanto MEJOR explica el minimo respecto del d esperado. Si es ~1, el barrido no
    // distingue entre los dos y la diferencia es del modelo, no del codigo.
    const double ratio = err > 0.0 ? errExp / err : 0.0;
    std::printf("  %-18s esperado d=%3d | MEDIDO d=%3d | mse(min)=%.3e mse(esperado)=%.3e"
                " | mejora x%.1f  %s\n",
                mode, expected, found, err, errExp, ratio,
                found == expected ? "OK"
                                  : (ratio < 2.0 ? "~ indistinguible del esperado"
                                                 : (found == 0 ? "<-- NO COMPENSA"
                                                    : (found == 2 * expected ? "<-- COMPENSA DOS VECES"
                                                                             : "<-- OTRO"))));
}

}  // namespace

// ---------------------------------------------------------------------------
// El fixture, antes que nada: una configuracion contra si misma.
// ---------------------------------------------------------------------------
TEST(BranchAlignment, TheHarnessIsDeterministic) {
    const auto in = impulse();
    EffectChain a, b;
    build(a, RoutingMode::PARALLEL, {DECI_HPF, FILTER}, 0);
    build(b, RoutingMode::PARALLEL, {DECI_HPF, FILTER}, 0);
    const auto x = run(a, in);
    const auto y = run(b, in);
    ASSERT_EQ(x.size(), y.size());
    for (size_t i = 0; i < x.size(); ++i) ASSERT_FLOAT_EQ(x[i], y[i]) << "frame " << i;
    double energy = 0.0;
    for (float s : x) energy += static_cast<double>(s) * s;
    ASSERT_GT(energy, 1e-6) << "no salio energia: no hay nada que medir";
}

// ---------------------------------------------------------------------------
// LA MEDICION: los cuatro modos que suman ramas.
// ---------------------------------------------------------------------------
TEST(BranchAlignment, EveryBranchSummingModeAlignsBeforeSumming) {
    const auto in = impulse();
    std::printf("\n=== alineacion de ramas por modo (REQ-011) ===\n");

    int mismatches = 0;
    auto check = [&](const char* name, int expected, int found, double err, double errExp) {
        report(name, expected, found, err, errExp);
        // Un modo esta MAL solo si el minimo explica la salida sensiblemente mejor que el
        // d correcto. Un desvio de una muestra con la misma mse es ruido del modelo.
        if (found != expected && (err <= 0.0 || errExp / err >= 2.0)) ++mismatches;
    };

    // --- PARALLEL: (A + B)/2, cada efecto es su rama. DECI en slot 0.
    {
        EffectChain c;
        build(c, RoutingMode::PARALLEL, {DECI_HPF, FILTER}, 0);
        const int latA = c.getEffectLatencySamples(0);
        const int latB = c.getEffectLatencySamples(1);
        ASSERT_NE(latA, latB) << "el montaje no ejercita la compensacion";
        const auto got = run(c, in);

        const auto bA = branch({DECI_HPF}, 0, in);
        const auto bB = branch({FILTER}, -1, in);
        double err = 0.0;
        double errExp = 0.0;
        const int d = bestDelay(got, [&](int dd) { return mean2(bA, delayed(bB, dd)); }, &err, latA - latB, &errExp);
        check("PARALLEL", latA - latB, d, err, errExp);
    }

    // --- PARALLEL_SERIAL: (A + B) -> C -> D. Paralelo = slots [0, n-2).
    {
        EffectChain c;
        build(c, RoutingMode::PARALLEL_SERIAL, {DECI_HPF, FILTER, FILTER, FILTER}, 0);
        const int latA = c.getEffectLatencySamples(0);
        const int latB = c.getEffectLatencySamples(1);
        ASSERT_NE(latA, latB);
        const auto got = run(c, in);

        const auto bA = branch({DECI_HPF}, 0, in);
        const auto bB = branch({FILTER}, -1, in);
        double err = 0.0, errExp = 0.0;
        const int d = bestDelay(got, [&](int dd) {
            return branch({FILTER, FILTER}, -1, mean2(bA, delayed(bB, dd)));
        }, &err, latA - latB, &errExp);
        check("PARALLEL_SERIAL", latA - latB, d, err, errExp);
    }

    // --- SERIAL_PARALLEL: A -> B -> (C + D). Paralelo = slots [2, n).
    {
        EffectChain c;
        build(c, RoutingMode::SERIAL_PARALLEL, {FILTER, FILTER, DECI_HPF, FILTER}, 2);
        const int latC = c.getEffectLatencySamples(2);
        const int latD = c.getEffectLatencySamples(3);
        ASSERT_NE(latC, latD);
        const auto got = run(c, in);

        const auto pre = branch({FILTER, FILTER}, -1, in);
        const auto bC = branch({DECI_HPF}, 0, pre);
        const auto bD = branch({FILTER}, -1, pre);
        double err = 0.0;
        double errExp = 0.0;
        const int d = bestDelay(got, [&](int dd) { return mean2(bC, delayed(bD, dd)); }, &err, latC - latD, &errExp);
        check("SERIAL_PARALLEL", latC - latD, d, err, errExp);
    }

    // --- SPLIT_2X2, las DOS orientaciones.
    //
    // 🔑 Hace falta probar la lenta en cada rama, y no es simetria decorativa: el codigo
    // alinea con dos `if` separados (`target > latA` y `target > latB`), y la rama que YA
    // es la mas lenta no entra por ninguno. Con el DECI siempre en A, el `if` de A es
    // `47 > 47` —falso— y romperlo no cambia nada: un mutante que lo desactivaba SOBREVIVIA,
    // acusando al montaje y no al codigo.
    {
        EffectChain c;
        build(c, RoutingMode::SPLIT_2X2, {FILTER, FILTER, DECI_HPF, FILTER}, 2);
        c.setParallelMix(0.5f);
        const int latA = c.getEffectLatencySamples(0) + c.getEffectLatencySamples(1);
        const int latB = c.getEffectLatencySamples(2) + c.getEffectLatencySamples(3);
        ASSERT_NE(latA, latB);
        const auto got = run(c, in);

        const auto bA = branch({FILTER, FILTER}, -1, in);
        const auto bB = branch({DECI_HPF, FILTER}, 0, in);
        double err = 0.0, errExp = 0.0;
        // Aca la rama RAPIDA es A, asi que se barre el retardo de A.
        const int d = bestDelay(got, [&](int dd) {
            const auto da = delayed(bA, dd);
            Buf o(bB.size(), 0.0f);
            for (size_t i = 0; i < o.size(); ++i) o[i] = da[i] * 0.5f + bB[i] * 0.5f;
            return o;
        }, &err, latB - latA, &errExp);
        check("SPLIT_2X2 (lenta en B)", latB - latA, d, err, errExp);
    }

    // Rama A lleva el DECI: ejercita el `if` de la rama B.
    {
        EffectChain c;
        build(c, RoutingMode::SPLIT_2X2, {DECI_HPF, FILTER, FILTER, FILTER}, 0);
        c.setParallelMix(0.5f);
        const int latA = c.getEffectLatencySamples(0) + c.getEffectLatencySamples(1);
        const int latB = c.getEffectLatencySamples(2) + c.getEffectLatencySamples(3);
        ASSERT_NE(latA, latB);
        const auto got = run(c, in);

        const auto bA = branch({DECI_HPF, FILTER}, 0, in);
        const auto bB = branch({FILTER, FILTER}, -1, in);
        double err = 0.0, errExp = 0.0;
        const int d = bestDelay(got, [&](int dd) {
            const auto db = delayed(bB, dd);
            Buf o(bA.size(), 0.0f);
            for (size_t i = 0; i < o.size(); ++i) o[i] = bA[i] * 0.5f + db[i] * 0.5f;
            return o;
        }, &err, latA - latB, &errExp);
        check("SPLIT_2X2 (lenta en A)", latA - latB, d, err, errExp);
    }

    std::printf("=== modos con alineacion equivocada: %d ===\n\n", mismatches);

    EXPECT_EQ(mismatches, 0)
        << mismatches << " modo(s) suman ramas desalineadas — ver la tabla de arriba. "
        << "Cada uno es un filtro peine cuyo periodo es el desvio medido.";
}

// ---------------------------------------------------------------------------
// AC-011.4 — una cadena SIN latencias no puede cambiar de sonido.
//
// Es la red que protege a la enorme mayoria de las cadenas reales, donde todos los
// efectos declaran cero y el retardo pedido es cero, asi que no se toca nada. Si el arreglo de la
// alineacion le cambiara el audio a estas, seria una regresion que ningun otro test
// de este archivo veria: los de arriba SOLO miran montajes con latencias distintas.
//
// "Igual que hoy" se afirma sobre una propiedad y no sobre un golden: sin nada que
// compensar, la suma paralela ES el promedio crudo de las ramas, sin corrimiento.
// ---------------------------------------------------------------------------
TEST(BranchAlignment, AChainWithNoLatencyIsSummedWithNoShiftAtAll) {
    const auto in = impulse();

    EffectChain c;
    build(c, RoutingMode::PARALLEL, {FILTER, CHORUS}, -1);
    ASSERT_EQ(c.getEffectLatencySamples(0), 0) << "el montaje no es de latencia cero";
    ASSERT_EQ(c.getEffectLatencySamples(1), 0) << "el montaje no es de latencia cero";

    const auto got = run(c, in);
    const auto bA = branch({FILTER}, -1, in);
    const auto bB = branch({CHORUS}, -1, in);
    const auto plain = mean2(bA, bB);

    double energy = 0.0;
    for (float s : got) energy += static_cast<double>(s) * s;
    ASSERT_GT(energy, 1e-6) << "no salio energia: no hay nada que comparar";

    // Sin latencias, el promedio crudo tiene que explicar la salida MEJOR que cualquier
    // version corrida. Si algun corrimiento gana, la cadena esta retrasando ramas que no
    // tenian nada que compensar.
    const double errPlain = mse(got, plain);
    for (int d = 1; d <= 8; ++d) {
        EXPECT_LT(errPlain, mse(got, mean2(bA, delayed(bB, d))))
            << "un corrimiento de " << d << " explica la salida mejor que el promedio "
            << "crudo: la cadena esta compensando algo que declara latencia cero";
    }
}
