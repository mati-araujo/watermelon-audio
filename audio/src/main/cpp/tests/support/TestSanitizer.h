#pragma once

/**
 * TestSanitizer.h — un veredicto de COSTO no se emite con el codigo instrumentado.
 *
 * POR QUE EXISTE (REQ-005 · S2)
 * -----------------------------
 * Un sanitizer multiplica el costo del codigo por un factor de 5 a 10. Cualquier
 * techo razonable de "esto tiene que costar menos del N % del tiempo real" deja
 * de significar nada bajo esa instrumentacion, y el rojo que produce NO ES UN
 * DEFECTO: es el instrumento midiendose a si mismo.
 *
 * Medido el 2026-08-20 bajo TSan: `McLeodPitchCost` dio 0,404 contra su techo de
 * 0,25 — y con CERO carreras reportadas. Un rojo que no es un defecto es
 * exactamente lo que termina haciendo que alguien silencie el guardrail entero.
 *
 * POR QUE ESTA EN UN HEADER COMPARTIDO Y NO COPIADO EN CADA TEST
 * --------------------------------------------------------------
 * Porque la clase de defecto es "un test de costo NACE sin la guarda". Estaba
 * definida dentro de `test_mcleod_pitch.cpp`, asi que el test de costo siguiente
 * —`PhaseSlopeCost`, de otro archivo— nacio sin ella y nadie se entero: pasaba
 * por holgura, no por proteccion. Un guardrail que hay que acordarse de copiar
 * no es un guardrail.
 *
 * 🔴 LA DETECCION VA EN `#if` ANIDADOS, Y ESO NO ES ESTILO
 * --------------------------------------------------------
 * GCC no define `__has_feature`, y en una sola linea `#if` el preprocesador
 * evalua el token igual aunque el `defined(...)` de al lado sea falso — no hay
 * cortocircuito que valga. Sale `error: missing binary operator before token
 * "("`, que es exactamente como se cayo el job de ubuntu la primera vez que se
 * escribio. **Apple clang lo aceptaba**, asi que verificarlo local no alcanza.
 *
 * USO
 * ---
 *     TEST(FooCost, ItCostsAFractionOfRealTime) {
 *         WMA_SKIP_IF_SANITIZED();
 *         ... medir y afirmar ...
 *     }
 *
 * El numero de costo sigue saliendo de la corrida normal, que es donde significa
 * algo. Bajo sanitizers el test sale SKIPPED y nunca PASSED: una corrida que no
 * midio no se puede leer como cobertura — la misma regla que `regen-golden.sh`.
 */

#include <gtest/gtest.h>

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#    define WMA_TEST_UNDER_SANITIZER 1
#elif defined(__has_feature)
#    if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer) \
        || __has_feature(memory_sanitizer)
#        define WMA_TEST_UNDER_SANITIZER 1
#    endif
#endif

#ifdef WMA_TEST_UNDER_SANITIZER
#    define WMA_SKIP_IF_SANITIZED()                                            \
        GTEST_SKIP() << "el costo no se mide bajo sanitizers: la "             \
                        "instrumentacion domina la medicion"
#else
#    define WMA_SKIP_IF_SANITIZED() ((void)0)
#endif
