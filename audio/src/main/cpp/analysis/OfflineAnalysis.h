#pragma once

/**
 * @file OfflineAnalysis.h
 * @brief Analizar un buffer sin microfono, sin dispositivo y sin thread (REQ-015).
 *
 * POR QUE EXISTE
 * --------------
 * Los dos defectos que REQ-014 arreglo se encontraron **a mano**: un telefono,
 * una guitarra, un parlante y una persona punteando. Nada impide que vuelvan,
 * porque no habia forma de escribir un test que los detecte. Esto es esa forma.
 *
 * POR QUE SIN THREAD, Y NO ES UNA OPTIMIZACION
 * --------------------------------------------
 * 🔴 El thread es la fuente de la no-reproducibilidad, y esta MEDIDO en este
 * repo: REQ-014 levanto CINCO andamios de host sobre el `AnalysisThread` y los
 * cinco necesitaron esperas por condicion para no ser escamosos; dos de sus
 * tests igual terminaron corriendo una carrera y hubo que sacarlos de ahi. Un
 * puerto de regresion construido sobre el thread hereda esa clase entera, y un
 * test de regresion escamoso es peor que no tenerlo: ensucia el CI y despues
 * nadie le cree.
 *
 * Aca el analisis se empuja a mano, vuelta por vuelta, hasta agotar el buffer.
 * No hay relojes, no hay esperas y no hay dos threads: el mismo buffer da el
 * mismo resultado, siempre.
 *
 * NO REIMPLEMENTA EL ANALISIS
 * ---------------------------
 * Usa `AnalysisThread::drainOnce()`, que es exactamente el cuerpo que corre el
 * thread. Un camino paralelo mediria OTRO motor y su verde no diria nada del
 * producto (AC-015.3).
 */

#include "AnalysisSnapshot.h"

namespace wma::analysis {

/**
 * @brief Corre el analisis sobre `frames` cuadros ESTEREO INTERCALADOS.
 *
 * @param interleaved  `frames * 2` floats, L R L R…  (el ring suma a mono, igual
 *                     que en el camino de captura).
 * @param frames       cuadros, no floats.
 * @param sampleRate   el rate del material. NO se asume 48000: un buffer de 44,1
 *                     analizado como si fuera de 48 da una lectura bien formada y
 *                     equivocada, y este repo ya pago esa constante hardcodeada.
 * @param targetHz     contra que medir. 0 = sin objetivo (el snapshot sale con
 *                     `cents` en NaN, igual que en vivo).
 * @param outValues    `kSnapshotValueCount` floats. No se toca si devuelve false.
 *
 * @return false si los argumentos no describen audio analizable, o si el analisis
 *         nunca llego a publicar. **No devuelve ceros en ese caso**: un buffer de
 *         ceros es una medicion plausible y un consumidor lo mostraria como tal.
 */
bool analyzeBuffer(const float* interleaved, int frames, int sampleRate,
                   double targetHz, float* outValues) noexcept;

/**
 * @brief Igual que el anterior, pero DECLARANDO el instrumento (REQ-029 S1).
 *
 * POR QUE HACE FALTA, Y NO ES "precision adicional"
 * -------------------------------------------------
 * Sin candidatos el motor no puede preguntarse *"?esta altura es alguna cuerda?"*,
 * asi que una nota que no es el objetivo cae en la compuerta de ausencia y sale
 * como SIN SEÑAL. Un consumidor lo midio sobre 2.15.0: de 36 combinaciones de
 * objetivo x tono, las 30 de afuera de la diagonal daban `NO_SIGNAL` con la altura
 * EXACTA y claridad 0,9999 — tocando fuerte.
 *
 * 🔴 Con candidatos ese caso NO EXISTE, y esta medido: el modo rapido reengancha el
 * objetivo a la cuerda que suena (`AnalysisThread.cpp:324-332`) y el motor la mide.
 * O sea que declarar el instrumento **no mejora** la respuesta: es la que hace que
 * la pregunta del consumidor sea contestable. Esta funcion existe porque el puerto
 * offline no tenia como expresarlo, y era el unico camino donde el consumidor mide.
 *
 * @param candidatesHz    los objetivos en Hz, EN ORDEN DE CUERDA. `nullptr` o
 *                        `candidateCount <= 0` es legal y equivale a la sobrecarga
 *                        de arriba: sin instrumento declarado.
 * @param candidateCount  cuantos. Se recortan a `FastModeTracker::kMaxCandidates`.
 */
bool analyzeBuffer(const float* interleaved, int frames, int sampleRate,
                   double targetHz, const double* candidatesHz, int candidateCount,
                   float* outValues) noexcept;

}  // namespace wma::analysis
