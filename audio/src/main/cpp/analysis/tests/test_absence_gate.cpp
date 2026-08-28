/**
 * test_absence_gate.cpp — REQ-019 S1.
 *
 * Las ramas de la compuerta de ausencia, deterministas: sin threads, sin audio y
 * sin timing. Es la leccion de MINI-008, donde el auto-test de un diagnostico
 * probaba la rama equivocada porque otra cortaba antes, y un mutante que forzaba
 * la recomendacion opuesta SOBREVIVIA.
 *
 * El test de extremo a extremo (`test_silence_gate.cpp`) sigue siendo el que
 * prueba que esto esta CABLEADO; este prueba que la decision es correcta.
 */

#include "../AbsenceGate.h"

#include <gtest/gtest.h>

namespace {

using wma::analysis::AbsenceGate;

constexpr int kN = AbsenceGate::kQuietUpdatesToDeclare;

/// Azucar: una lectura con señal tonal buena.
bool tonal(AbsenceGate& g) { return g.update(false, true, true, /*fresh=*/true); }
/// Azucar: una lectura audible pero SIN altura (el transitorio del defecto).
bool sinAltura(AbsenceGate& g) { return g.update(false, true, false, /*fresh=*/true); }
/// Azucar: silencio de verdad.
bool silencio(AbsenceGate& g) { return g.update(true, true, false, /*fresh=*/true); }

// ---------------------------------------------------------------------------
// AC-019.1 — el transitorio no apaga la aguja
// ---------------------------------------------------------------------------

/**
 * EL TEST DEL DEFECTO. MINI-010 midio el transitorio real: CUATRO lecturas
 * seguidas sin altura sobre una cuerda audible (muestras [3,4,5,6], `hz=0`,
 * `pisados=0`). Antes de REQ-019 la primera de esas cuatro ya declaraba ausencia.
 */
TEST(AbsenceGate, ASingleTonalDropoutDoesNotDeclareAbsence) {
    AbsenceGate g;
    ASSERT_FALSE(tonal(g));
    EXPECT_FALSE(sinAltura(g))
        << "una sola lectura sin altura es un hueco, no ausencia: es exactamente "
           "el transitorio que apagaba la aguja sobre una cuerda audible";
}

TEST(AbsenceGate, TheRunIsBrokenByAnySignalAndStartsOver) {
    AbsenceGate g;
    // Casi llega al umbral...
    for (int i = 0; i < kN - 1; ++i) EXPECT_FALSE(sinAltura(g)) << "en i=" << i;
    // ...y una sola lectura buena lo reinicia.
    EXPECT_FALSE(tonal(g));
    EXPECT_EQ(g.quietRun(), 0);
    // Asi que hacen falta N nuevas, no una.
    for (int i = 0; i < kN - 1; ++i) EXPECT_FALSE(sinAltura(g)) << "tras reiniciar, i=" << i;
}

// ---------------------------------------------------------------------------
// AC-019.2 — EL GEMELO: la ausencia igual llega
// ---------------------------------------------------------------------------

/**
 * 🔴 EL GEMELO OBLIGATORIO, y es el freno del alcance entero.
 *
 * Sin el, "no declarar ausencia sobre una cuerda audible" se satisface
 * **apagando la compuerta**: no declarar ausencia NUNCA cumple AC-019.1
 * trivialmente. Ese modo de falla ya destruyo el 80 % del valor de un REQ en este
 * repo con la suite en verde.
 *
 * La demora esta ACOTADA y es exactamente `kQuietUpdatesToDeclare`: ni antes
 * (seria el defecto de vuelta) ni despues (seria el spinner eterno).
 */
TEST(AbsenceGate, SustainedSilenceIsStillDeclared) {
    AbsenceGate g;
    ASSERT_FALSE(tonal(g));
    for (int i = 1; i < kN; ++i) {
        EXPECT_FALSE(sinAltura(g)) << "declaro ausencia DEMASIADO PRONTO, en la lectura " << i;
    }
    EXPECT_TRUE(sinAltura(g)) << "tras " << kN << " lecturas sin altura la ausencia TIENE que "
                                 "llegar: una compuerta que nunca la declara es un spinner eterno";
    // Y se queda declarada mientras siga sin haber nada.
    EXPECT_TRUE(sinAltura(g));
    EXPECT_TRUE(sinAltura(g));
}

// ---------------------------------------------------------------------------
// AC-019.4 — el silencio de verdad no paga la demora
// ---------------------------------------------------------------------------

TEST(AbsenceGate, DigitalSilenceIsDeclaredWithoutDelay) {
    AbsenceGate g;
    ASSERT_FALSE(tonal(g));
    EXPECT_TRUE(silencio(g))
        << "el silencio por NIVEL no pasa por la histeresis: se detecta directo y "
           "demorarlo volveria incumplible al gemelo";
}

/**
 * La razon por la que el silencio SATURA el contador en vez de sólo devolver true.
 *
 * Sin saturar, al primer bloque de ruido de habitacion —que esta POR ENCIMA del
 * piso de nivel pero no tiene altura— el contador arrancaria de cero y el motor
 * diria "midiendo" durante N lecturas sobre una habitacion vacia.
 */
TEST(AbsenceGate, LeavingDigitalSilenceIntoPitchlessNoiseKeepsDeclaringAbsence) {
    AbsenceGate g;
    ASSERT_TRUE(silencio(g));
    EXPECT_TRUE(sinAltura(g))
        << "de silencio a ruido sin altura no puede aparecer un 'midiendo' "
           "transitorio: no hay nada que medir en ninguno de los dos";
}

// ---------------------------------------------------------------------------
// Los bordes que no son de ningun AC pero rompen el mecanismo si se mueven
// ---------------------------------------------------------------------------

/**
 * Sin rate preparado el detector NO CORRIO, y no se puede afirmar ausencia
 * apoyandose en una evidencia que no se produjo. Es la regla que ya traia
 * `nothingToTune` y que esta compuerta tiene que conservar.
 */
TEST(AbsenceGate, WithoutADetectorThereIsNoTonalVerdictToAccumulate) {
    AbsenceGate g;
    for (int i = 0; i < kN * 2; ++i) {
        EXPECT_FALSE(g.update(/*belowSilenceFloor=*/false, /*detectorRan=*/false,
                              /*tunableSourcePresent=*/false, /*freshVerdict=*/true))
            << "sin detector no hay evidencia tonal, asi que tampoco ausencia tonal (i=" << i << ")";
    }
    EXPECT_EQ(g.quietRun(), 0) << "y no puede quedar acumulando en silencio para disparar despues";
}

TEST(AbsenceGate, ResetForgetsTheRun) {
    AbsenceGate g;
    for (int i = 0; i < kN - 1; ++i) sinAltura(g);
    g.reset();
    EXPECT_EQ(g.quietRun(), 0);
    EXPECT_FALSE(sinAltura(g)) << "tras reset hacen falta N de nuevo, no la que faltaba";
}

/**
 * 🔴 REQ-019.2 — LA UNIDAD DEL CONTADOR, y es el test que faltaba en S1.
 *
 * La ventana del detector es NO SOLAPADA y de 4096 frames de entrada; el consumidor
 * lee un snapshot por bloque. Con bloques de 1024 eso son CUATRO lecturas por
 * veredicto, asi que contar LECTURAS cuenta la misma evidencia cuatro veces.
 *
 * Un unico veredicto malo —una ventana que cabalga la transicion de amplitud— NO
 * puede declarar ausencia, por muchas veces que se lo lea. Esto es exactamente el
 * defecto de MINI-010: `noSignal` valia 4 porque un veredicto se observaba 4 veces.
 */
TEST(AbsenceGate, OneBadVerdictReadManyTimesIsStillOneVerdict) {
    AbsenceGate g;
    ASSERT_FALSE(tonal(g));
    // Un solo veredicto sin altura...
    EXPECT_FALSE(g.update(false, true, false, /*freshVerdict=*/true));
    // ...releido veinte veces mientras el detector todavia junta su ventana.
    for (int i = 0; i < 20; ++i) {
        EXPECT_FALSE(g.update(false, true, false, /*freshVerdict=*/false))
            << "releer el MISMO veredicto no es evidencia nueva (relectura " << i << ")";
    }
}

/**
 * El gemelo de arriba: releer no acumula, pero tampoco puede BORRAR una ausencia ya
 * declarada — si no, el rotulo parpadearia entre veredictos.
 */
TEST(AbsenceGate, RereadsHoldADeclaredAbsenceInstead0fFlickering) {
    AbsenceGate g;
    for (int i = 0; i < kN; ++i) g.update(false, true, false, /*freshVerdict=*/true);
    ASSERT_TRUE(g.update(false, true, false, /*freshVerdict=*/false));
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(g.update(false, true, false, /*freshVerdict=*/false))
            << "la ausencia ya declarada tiene que SOSTENERSE entre veredictos (i=" << i << ")";
    }
}

}  // namespace
