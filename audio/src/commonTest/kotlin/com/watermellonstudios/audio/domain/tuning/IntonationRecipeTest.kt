package com.watermellonstudios.audio.domain.tuning

import com.watermellonstudios.audio.api.FakeTuner
import com.watermellonstudios.audio.api.TunerReading
import com.watermellonstudios.audio.domain.tuner.TunerState
import kotlin.math.abs
import kotlin.math.ulp
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * MINI-023 — el afinador del 12º traste SE COMPONE con lo que ya es público (R-API-57).
 *
 * Medir el armónico y la nota pisada de la misma cuerda y reportar su diferencia en cents —lo
 * que ajusta saddles— no necesita un miembro nuevo de `ITuner`. La receta es:
 *
 *   1. `TuningConfiguration(capo = Semitones(12))` pone el objetivo en la octava (2·f₀);
 *   2. `TunerReading.isConverged` es la compuerta de CADA captura;
 *   3. `TunerReading.target.frequency` garantiza que las dos son de la misma cuerda;
 *   4. la diferencia es `cents(pisada) − cents(armónico)`.
 *
 * El motor tiene un modo interno que hace exactamente esto (`IntonationMode`, REQ-001 S9,
 * `wma_intonation_*` en la C API) y NO se expone en `ITuner`: nadie lo pidió, y abrir superficie
 * sin consumidor es el error de REQ-037 con otro nombre. Este test es lo que sostiene la
 * decisión: si la SEMÁNTICA de capo, convergencia u objetivo cambia, esto se pone rojo.
 *
 * 🔴 EL ORÁCULO ES EL MISMO QUE EL DEL TEST C++ (AC-M023.2). Las siete filas de [kTable] son
 * copia literal de `analysis/tests/test_intonation.cpp` —las tres de
 * `TheReportedDifferenceMatchesTheTwoKnownDeviations` y las cuatro del golden
 * `intonation_cycle`— y el valor esperado se calcula ACÁ como `fretted − harmonic`, no se lee de
 * ningún motor ni del `.resp`. Dos implementaciones de la misma resta acotan su riesgo así: si
 * una cambia de semántica, uno de los dos tests se pone rojo sobre la misma fila.
 *
 * LA RECETA VIVE EN ESTE ARCHIVO A PROPÓSITO: no es un helper de `commonMain` ni un control de
 * `:harness`. Un helper publicado sería la puerta que este MINI decidió no abrir; el día que un
 * consumidor necesite el invariante "no hay resultado hasta que las dos convergieron"
 * GARANTIZADO por el motor y no por su disciplina, un call site en su árbol es la precondición
 * para abrirla (criterio de muerte de MINI-023, causa 2).
 */
class IntonationRecipeTest {

    // -----------------------------------------------------------------------
    // La receta, tal como la escribiría un consumidor
    // -----------------------------------------------------------------------

    /**
     * `cents(pisada) − cents(armónico)`, o `null` si la receta no tiene resultado: una de las
     * dos no convergió, o no son de la misma cuerda. Es una función pura de dos lecturas: si
     * una caduca (deja de converger), el resultado caduca con ella al recalcular.
     */
    private fun intonationDifferenceCents(harmonic: TunerReading, fretted: TunerReading): Float? {
        if (!harmonic.isConverged || !fretted.isConverged) return null
        val sameString = harmonic.target?.frequency == fretted.target?.frequency
        if (!sameString) return null
        return checkNotNull(fretted.cents) - checkNotNull(harmonic.cents)
    }

    private fun octaveTarget(tuning: Tuning, stringIndex: Int): StringTarget =
        requireNotNull(
            TuningConfiguration(tuning, capo = Semitones(12)).targetForString(stringIndex),
        ) { "${tuning.id} no tiene cuerda $stringIndex" }

    private fun reading(target: StringTarget?, cents: Float, state: TunerState = TunerState.CONVERGED) =
        TunerReading(target, FakeTuner.snapshotWithPitch(cents = cents, state = state))

    // -----------------------------------------------------------------------
    // AC-M023.1 — capo 12 es exactamente la octava, para toda cuerda y todo temperamento
    // -----------------------------------------------------------------------

    private val allTemperaments: List<Temperament> =
        TemperamentKind.entries.flatMap { kind -> (0..11).map { tonic -> Temperament(kind, tonic) } }

    @Test
    fun capoDoceDaExactamenteLaOctavaParaTodaCuerdaYTodoTemperamento() {
        var checked = 0
        for (tuning in Tuning.ALL) {
            for (temperament in allTemperaments) {
                for (reference in listOf(TuningReference.STANDARD, TuningReference.BAROQUE)) {
                    val open = TuningConfiguration(tuning, temperament, reference)
                    val octave = open.copy(capo = Semitones(12))
                    val openTargets = open.targets()
                    val octaveTargets = octave.targets()
                    assertEquals(openTargets.size, octaveTargets.size, tuning.id)

                    openTargets.zip(octaveTargets).forEach { (o, t) ->
                        val where = "${tuning.id} cuerda ${o.stringIndex} " +
                            "(${temperament.kind}/${temperament.tonic}, A4=${reference.a4.hz})"
                        // Sin saturar: `transpose` satura en MIDI 127 en vez de lanzar, y una
                        // cuerda saturada daría un objetivo que NO es la octava sin que nada
                        // falle. Ninguna del catálogo llega (la más aguda es E5 = 76).
                        assertEquals(o.note.midi + 12, t.note.midi, "$where: saturó el capo")
                        assertEquals(o.stringIndex, t.stringIndex, where)
                        // La octava es exacta porque la desviación del temperamento se indexa
                        // por clase de altura, y +12 conserva la clase. Se afirma a 2 ulps y
                        // no con `==`: MEDIDO, `2^((m+12−69)/12)` y `2·2^((m−69)/12)` difieren
                        // en 1 ulp en D2 de drop-D (146,83238395870376 vs …038) — redondeo de
                        // `pow` sobre exponentes distintos, no el modelo. Un temperamento con
                        // desviación por nota, o un capo corrido un semitono, rompería esto en
                        // ≥ 1e-4 relativo: doce órdenes por encima de lo que se tolera.
                        val expected = 2.0 * o.frequency.hz
                        val ulps = abs(t.frequency.hz - expected) / expected.ulp
                        assertTrue(ulps <= 2.0, "$where: no es 2·f₀ ($ulps ulps: $expected vs ${t.frequency.hz})")
                        checked++
                    }
                }
            }
        }
        // 12 afinaciones × 48 temperamentos × 2 referencias × sus cuerdas: que el bucle
        // haya recorrido algo, y no un catálogo vacío.
        assertTrue(checked > 4000, "recorrió $checked cuerdas; el catálogo no puede ser tan chico")
    }

    // -----------------------------------------------------------------------
    // AC-M023.2 — la diferencia, contra la MISMA tabla que el test C++
    // -----------------------------------------------------------------------

    /**
     * Una fila del oráculo compartido. `tuning`/`stringIndex` nombran la cuerda abierta cuyo
     * 12º traste se mide — el C++ escribe el Hz (82,407 = E2 de guitarra), acá se toma del
     * catálogo para que la fila esté atada al modelo y no a una constante suelta.
     */
    private data class Row(
        val name: String,
        val tuning: Tuning,
        val stringIndex: Int,
        val harmonicCents: Float,
        val frettedCents: Float,
    )

    /** Copia literal de `test_intonation.cpp` (REQ-001 S9): tres casos + las cuatro del golden. */
    private val kTable = listOf(
        // TheReportedDifferenceMatchesTheTwoKnownDeviations — sobre 2·E2
        Row("pisada 3 cents alta", Tuning.GUITAR_STANDARD, 6, -1.0f, +2.0f),
        Row("pisada 4 cents baja", Tuning.GUITAR_STANDARD, 6, +2.0f, -2.0f),
        Row("intonacion perfecta", Tuning.GUITAR_STANDARD, 6, +1.5f, +1.5f),
        // GoldenIntonation.TheFullTwoMeasurementCycleMatchesItsGolden
        Row("guitarra E2", Tuning.GUITAR_STANDARD, 6, -1.0f, +2.0f),
        Row("guitarra A2", Tuning.GUITAR_STANDARD, 5, +0.5f, +3.5f),
        Row("guitarra E4", Tuning.GUITAR_STANDARD, 1, -2.0f, -0.5f),
        Row("bajo E1", Tuning.BASS_4_STANDARD, 4, +1.0f, +5.0f),
    )

    @Test
    fun laDiferenciaEsLaDelOraculoCompartidoConElTestDeCpp() {
        for (row in kTable) {
            val target = octaveTarget(row.tuning, row.stringIndex)
            val harmonic = reading(target, row.harmonicCents)
            val fretted = reading(target, row.frettedCents)

            // El oráculo, calculado acá y no leído de ningún motor: la misma resta que el
            // C++ escribe como `want = c.fretted - c.harmonic`.
            val want = row.frettedCents - row.harmonicCents
            val got = intonationDifferenceCents(harmonic, fretted)
            assertNotNull(got, "${row.name}: dos lecturas convergidas de la misma cuerda tienen resultado")
            assertEquals(want, got, "${row.name}: esperaba $want cents y dio $got")
        }
    }

    @Test
    fun laTablaTieneLasSieteFilasDelTestDeCpp() {
        // Si alguien agrega una fila en C++ y no acá (o al revés), el oráculo deja de ser
        // compartido sin que nadie lo note. El número es la afirmación.
        assertEquals(7, kTable.size)
        assertEquals(3, kTable.count { it.name.startsWith("pisada") || it.name.startsWith("intonacion") })
        assertEquals(4, kTable.count { it.name.startsWith("guitarra") || it.name.startsWith("bajo") })
    }

    // -----------------------------------------------------------------------
    // AC-M023.3 — sin convergencia no hay resultado (y su gemelo: con las dos, sí)
    // -----------------------------------------------------------------------

    @Test
    fun sinConvergenciaEnCualquieraDeLasDosNoHayResultado() {
        val target = octaveTarget(Tuning.GUITAR_STANDARD, 6)
        val harmonicOk = reading(target, -1.0f)
        val frettedOk = reading(target, +2.0f)

        // El gemelo primero: un apagado total ("nunca hay resultado") pasaría cualquier
        // "no produzcas de más". Con las dos convergidas, HAY resultado y es el esperado.
        assertEquals(3.0f, intonationDifferenceCents(harmonicOk, frettedOk))

        for (state in TunerState.entries.filter { it != TunerState.CONVERGED }) {
            assertNull(
                intonationDifferenceCents(reading(target, -1.0f, state), frettedOk),
                "armónico en $state: no hay resultado",
            )
            assertNull(
                intonationDifferenceCents(harmonicOk, reading(target, +2.0f, state)),
                "pisada en $state: no hay resultado",
            )
        }
    }

    @Test
    fun siUnaLecturaCaducaElResultadoCaducaConElla() {
        // El invariante que `IntonationMode` cumple por construcción (su test
        // `LosingASignalExpiresTheResultInsteadOfShowingTheLastGoodOne`), acá como disciplina
        // del consumidor: la receta es función de las DOS lecturas actuales, no de la última
        // buena. Si el armónico deja de converger, recalcular da null, no el 3,0 de antes.
        val target = octaveTarget(Tuning.GUITAR_STANDARD, 6)
        val fretted = reading(target, +2.0f)
        assertEquals(3.0f, intonationDifferenceCents(reading(target, -1.0f), fretted))
        assertNull(intonationDifferenceCents(reading(target, -1.0f, TunerState.NO_SIGNAL), fretted))
    }

    @Test
    fun dosCuerdasDistintasNoSeRestan() {
        // `TwoDifferentStringsAreReportedInsteadOfSubtracted` del C++: 2·E2 contra 2·A2.
        val e2 = octaveTarget(Tuning.GUITAR_STANDARD, 6)
        val a2 = octaveTarget(Tuning.GUITAR_STANDARD, 5)
        assertNull(intonationDifferenceCents(reading(e2, -1.0f), reading(a2, +2.0f)))
        // Y sin objetivo no hay cuerda que comparar: `isConverged` ya lo dice.
        assertNull(intonationDifferenceCents(reading(null, -1.0f), reading(e2, +2.0f)))
    }
}
