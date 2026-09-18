package com.watermellonstudios.audio.internal.bridge

import kotlin.test.Test
import kotlin.test.assertEquals

/**
 * REQ-043 S2, auditoría de #343 — **la rama del reintento por región que crece**, que
 * ningún arnés con motor puede observar (mover la región entre los dos cruces de UNA
 * llamada exige otro thread o render, y una carrera no es un test). Acá la cota es una
 * lambda y el "motor" un contador: lo que se afirma es la lógica que los dos bridges
 * comparten.
 *
 * Cada test dice qué bug atraparía.
 */
class BoundedReadTest {

    /** Un motor con [real] puntos: escribe `min(bound, real)`, devuelve cuántos. */
    private class Motor(var real: Int) {
        var lecturas = 0
        fun readOnce(bound: Int): Pair<Int, Int> {
            lecturas++
            return minOf(bound, real) to minOf(bound, real)
        }
    }

    /**
     * El escenario de la auditoría: cota 301 (3 s), la región pasa a 10 s entre la cota y el
     * análisis, el motor llena los 301. Sin el reintento salen 301 puntos de una pista de
     * 1001: **mutante que mata** = devolver `result` en cuanto `written == bound` sin re-leer.
     */
    @Test
    fun aRegionThatGrowsBetweenTheBoundAndTheAnalysisIsReadAgainWithTheNewBound() {
        val cotas = ArrayDeque(listOf(301, 1001))
        val motor = Motor(real = 1000)
        val puntos = BoundedRead.read(bound = { cotas.removeFirstOrNull() ?: 1001 }) { motor.readOnce(it) }
        assertEquals(1000, puntos, "la región creció a 10 s y la serie tiene que cubrirla entera")
        assertEquals(2, motor.lecturas, "una lectura por la cota vieja y una por la nueva")
    }

    /**
     * `written == bound` también es legítimo SIN crecimiento (pitch con `W ≤ L mod hop`): la
     * cota re-leída es igual y NO se reintenta. Bug que atrapa: reintentar por igualdad sola,
     * que duplicaría el costo de cada análisis cuya última ventana cae justo.
     */
    @Test
    fun writingExactlyTheBoundWithoutGrowthDoesNotRetry() {
        val motor = Motor(real = 500)
        val puntos = BoundedRead.read(bound = { 500 }) { motor.readOnce(it) }
        assertEquals(500, puntos)
        assertEquals(1, motor.lecturas, "la cota re-leída es igual: no hay segunda lectura")
    }

    /** Lo normal: el motor escribe menos que la cota y no se vuelve a mirar la región. */
    @Test
    fun writingLessThanTheBoundReturnsAtOnce() {
        var lecturasDeCota = 0
        val motor = Motor(real = 447)
        val puntos = BoundedRead.read(bound = { lecturasDeCota++; 451 }) { motor.readOnce(it) }
        assertEquals(447, puntos)
        assertEquals(1, motor.lecturas)
        assertEquals(1, lecturasDeCota, "con escritos < cota la región no se re-lee")
    }

    /**
     * El caso patológico: una región que no para de crecer. Se devuelve lo que hay en la
     * tercera lectura, no se entra en bucle. Bug que atrapa: un `while (true)` sin techo.
     */
    @Test
    fun aRegionThatNeverStopsGrowingIsGivenUpAfterThreeReads() {
        var cota = 100
        val motor = Motor(real = Int.MAX_VALUE / 2)
        val puntos = BoundedRead.read(bound = { cota += 100; cota }) { motor.readOnce(it) }
        assertEquals(BoundedRead.MAX_ATTEMPTS, motor.lecturas, "tres lecturas y basta")
        assertEquals(400, puntos, "la última cota leída fue 400 (200, 300, 400) y se devuelve lo que hay")
    }
}
