#pragma once

/**
 * @file AscentVsSweep.h
 * @brief REQ-034 S1 — el instrumento: ¿un ASCENSO desde la muestra gruesa encuentra el mismo
 *        pico que el barrido ±span entero, y a que costo?
 *
 * REQ-033 refina cada candidato del barrido proporcional al maximo real de la NSDF en su
 * vecindad ±lag/12 (2·span+1 evaluaciones por candidato). La hipotesis de REQ-034 es que en
 * esa vecindad la NSDF es unimodal —un candidato es maximo local entre muestras separadas
 * lag/12, asi que su pico esta a menos de medio paso— y un ascenso de a un lag desde la muestra
 * gruesa lo encuentra en 3 a 6 evaluaciones. Esto lo MIDE: alimenta un `McLeodPitch` en bloques
 * de UNA ventana entera (para que `mWindow` siga intacta, la leccion de REQ-033 S1), y tras cada
 * ventana compara, candidato por candidato, el pico que produccion refino (sondas
 * `sweepCandidateRefined*`) contra el que encuentra el ascenso hecho desde aca con `nsdfAt`.
 *
 * NO toca produccion: el detector sigue barriendo entero. Lo que sale es una cuenta —ventanas
 * iguales/distintas, evaluaciones de cada metodo— que decide la forma de S2.
 */

#include "McLeodPitch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace wma_test::ascent {

struct Tally {
    long windows = 0;        ///< ventanas con altura (las sin candidatos no comparan nada)
    long candidates = 0;
    long differLag = 0;      ///< candidatos donde el ascenso termino en OTRO lag
    long differValue = 0;    ///< o en el mismo lag pero con otro valor (no deberia pasar nunca)
    long chosenDiffer = 0;   ///< ventanas donde el candidato ELEGIDO por la regla cambiaria
    long evalSweep = 0;      ///< evaluaciones del barrido (sonda)
    long evalFull = 0;       ///< evaluaciones del refinamiento entero (sonda)
    long evalAscent = 0;     ///< evaluaciones que costo el ascenso (contadas aca)
    std::vector<std::string> examples;  ///< las primeras diferencias de candidato, para leerlas
    std::vector<std::string> chosenExamples;  ///< TODAS las ventanas donde la eleccion cambiaria
};

/// Los limites del barrido, calcados de `McLeodPitch::prepare`. Si divergieran, la comparacion
/// lo mostraria como diferencias en los bordes — y eso seria un hallazgo, no un error del test.
inline void lagBounds(int rate, int& minLag, int& maxLag, double& working) {
    using wma::dsp::McLeodPitch;
    int decimation = static_cast<int>(std::lround(static_cast<double>(rate) / McLeodPitch::kTargetRate));
    if (decimation < 1) decimation = 1;
    working = static_cast<double>(rate) / decimation;
    minLag = static_cast<int>(std::floor(working / McLeodPitch::kMaxHz));
    maxLag = static_cast<int>(std::ceil(working / McLeodPitch::kMinHz));
    if (minLag < 2) minLag = 2;
    if (maxLag > McLeodPitch::kWindowFrames / 2) maxLag = McLeodPitch::kWindowFrames / 2;
}

/// Un paso de una ventana entera en frames de ENTRADA.
inline int wholeWindow(int rate) {
    int decimation = static_cast<int>(std::lround(static_cast<double>(rate) / wma::dsp::McLeodPitch::kTargetRate));
    if (decimation < 1) decimation = 1;
    return wma::dsp::McLeodPitch::kWindowFrames * decimation;
}

/**
 * El ascenso: desde la muestra gruesa `lag`, mira los dos vecinos; si alguno sube, sigue en
 * esa direccion de a uno hasta que baje o toque el borde [from, to]. Devuelve el pico y cuenta
 * evaluaciones en `evals`.
 */
inline int ascend(const wma::dsp::McLeodPitch& mpm, int lag, double lagValue, int from, int to,
                  double& peakValue, long& evals) {
    int cur = lag;
    double curV = lagValue;
    double left = -2.0, right = -2.0;
    if (cur - 1 >= from) { left = mpm.nsdfAt(cur - 1); ++evals; }
    if (cur + 1 <= to)   { right = mpm.nsdfAt(cur + 1); ++evals; }
    int dir = 0;
    if (left > curV && left >= right) { dir = -1; cur -= 1; curV = left; }
    else if (right > curV)            { dir = +1; cur += 1; curV = right; }
    while (dir != 0) {
        const int next = cur + dir;
        if (next < from || next > to) break;
        const double v = mpm.nsdfAt(next);
        ++evals;
        if (v > curV) { cur = next; curV = v; } else break;
    }
    peakValue = curV;
    return cur;
}

/// Alimenta `mono` en ventanas enteras y compara tras cada una. `label` va a los ejemplos.
inline void compare(int rate, const std::vector<float>& mono, Tally& t, const std::string& label) {
    using wma::dsp::McLeodPitch;
    McLeodPitch mpm;
    mpm.prepare(rate);
    int minLag, maxLag; double working;
    lagBounds(rate, minLag, maxLag, working);
    const int step = wholeWindow(rate);
    int fed = 0;
    int windowsSeen = 0;
    while (fed + step <= static_cast<int>(mono.size())) {
        mpm.process(mono.data() + fed, step);
        fed += step;
        if (mpm.windowsAnalyzed() == windowsSeen) continue;   // no deberia pasar: paso = ventana
        windowsSeen = mpm.windowsAnalyzed();
        t.evalSweep += mpm.nsdfEvaluationsSweep();
        t.evalFull += mpm.nsdfEvaluationsRefine();
        if (!mpm.hasPitch()) continue;
        ++t.windows;

        // La regla del primer pico, con los picos de cada metodo, para saber si la ELECCION
        // cambiaria — que es lo unico que el consumidor veria.
        double bestFull = -1.0, bestAsc = -1.0;
        std::vector<int> fullLags, ascLags;
        std::vector<double> fullVals, ascVals;
        for (int i = 0; i < mpm.sweepCandidateCount(); ++i) {
            const int lag = mpm.sweepCandidateLag(i);
            const int span = std::max(1, lag / 12);
            const int from = std::max(minLag, lag - span);
            const int to = std::min(maxLag, lag + span);
            const int fLag = mpm.sweepCandidateRefinedLag(i);
            const double fVal = mpm.sweepCandidateRefinedNsdf(i);
            double aVal = 0.0;
            const int aLag = ascend(mpm, lag, mpm.sweepCandidateNsdf(i), from, to, aVal, t.evalAscent);
            ++t.candidates;
            if (aLag != fLag) {
                ++t.differLag;
                if (t.examples.size() < 12)
                    t.examples.push_back(label + ": ventana " + std::to_string(windowsSeen) + ", candidato " +
                                         std::to_string(lag) + " -> entero " + std::to_string(fLag) + " (" +
                                         std::to_string(fVal) + ") / ascenso " + std::to_string(aLag) + " (" +
                                         std::to_string(aVal) + ")");
            } else if (aVal != fVal) {
                ++t.differValue;
            }
            fullLags.push_back(fLag); fullVals.push_back(fVal); bestFull = std::max(bestFull, fVal);
            ascLags.push_back(aLag);  ascVals.push_back(aVal);  bestAsc = std::max(bestAsc, aVal);
        }
        int chosenFull = -1, chosenAsc = -1;
        for (size_t i = 0; i < fullVals.size(); ++i)
            if (fullVals[i] >= McLeodPitch::kPeakThreshold * bestFull) { chosenFull = fullLags[i]; break; }
        for (size_t i = 0; i < ascVals.size(); ++i)
            if (ascVals[i] >= McLeodPitch::kPeakThreshold * bestAsc) { chosenAsc = ascLags[i]; break; }
        if (chosenFull != chosenAsc) {
            ++t.chosenDiffer;
            t.chosenExamples.push_back(label + ": ventana " + std::to_string(windowsSeen) + " — entero elige " +
                                       std::to_string(chosenFull) + " (" + std::to_string(working / chosenFull) +
                                       " Hz), ascenso elige " + std::to_string(chosenAsc) + " (" +
                                       std::to_string(working / chosenAsc) + " Hz); max entero " +
                                       std::to_string(bestFull) + ", max ascenso " + std::to_string(bestAsc));
        }
    }
}

inline void print(const char* title, const Tally& t) {
    std::printf("  [REQ-034] %-34s ventanas %5ld  candidatos %6ld  lag distinto %ld  valor distinto %ld"
                "  ELECCION distinta %ld  |  evaluaciones: barrido %ld  refinado entero %ld  ascenso %ld"
                "  (%.2fx)\n",
                title, t.windows, t.candidates, t.differLag, t.differValue, t.chosenDiffer,
                t.evalSweep, t.evalFull, t.evalAscent,
                t.evalAscent > 0 ? static_cast<double>(t.evalFull) / t.evalAscent : 0.0);
    for (const auto& e : t.examples) std::printf("      %s\n", e.c_str());
    for (const auto& e : t.chosenExamples) std::printf("      ELECCION: %s\n", e.c_str());
}

inline void add(Tally& into, const Tally& t) {
    into.windows += t.windows; into.candidates += t.candidates; into.differLag += t.differLag;
    into.differValue += t.differValue; into.chosenDiffer += t.chosenDiffer;
    into.evalSweep += t.evalSweep; into.evalFull += t.evalFull; into.evalAscent += t.evalAscent;
    for (const auto& e : t.examples) if (into.examples.size() < 12) into.examples.push_back(e);
    for (const auto& e : t.chosenExamples) into.chosenExamples.push_back(e);
}

}  // namespace wma_test::ascent
