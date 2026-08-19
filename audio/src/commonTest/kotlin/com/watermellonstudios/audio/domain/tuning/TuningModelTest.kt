package com.watermellonstudios.audio.domain.tuning

import kotlin.math.abs
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * REQ-001 S3 — el modelo musical: qué nota DEBERÍA sonar.
 *
 * CÓMO SE ELIGIERON LOS NÚMEROS ESPERADOS, QUE ES LA MITAD DEL TRABAJO
 * --------------------------------------------------------------------
 * Un test que calcula lo esperado **con la misma fórmula que la implementación** no prueba
 * nada: prueba que la fórmula es igual a sí misma. Así que acá los valores esperados salen de
 * tres fuentes, todas independientes del código:
 *
 *   1. **Relaciones de octava**, que son exactas en binario y verificables a mano:
 *      A0 = A4 / 2⁴ = 27,5 exacto.
 *   2. **Constantes publicadas** de la tabla estándar de temperamento igual (E2 = 82,407;
 *      C7 = 2093,005). No salen de este código.
 *   3. **Teoría de intervalos**: la tercera pura son 1200·log2(5/4) = 386,314 cents; la quinta
 *      pura 701,955; la coma pitagórica 23,460. Son propiedades de las razones, no de la
 *      implementación.
 *
 * La spec de la etapa lo pide explícitamente —"recalcularlas, no copiarlas de acá: si una está
 * mal, el test la hereda"— y por eso ninguna de las tablas de este archivo se copió del Given.
 */
class TuningModelTest {

    private fun assertCloseHz(expected: Double, actual: Frequency, tolHz: Double, msg: String) {
        assertTrue(
            abs(actual.hz - expected) <= tolHz,
            "$msg: esperado ${expected} Hz, medido ${actual.hz} Hz (tolerancia $tolHz)",
        )
    }

    // -----------------------------------------------------------------------
    // 3.1 · AC-001.12 — la referencia y la escala igual
    // -----------------------------------------------------------------------

    @Test
    fun conA440LasAnclasIndependientesDanExacto() {
        val config = TuningConfiguration(Tuning.GUITAR_STANDARD)

        // A4 es la referencia misma: cualquier desvío acá es un error de anclaje.
        assertEquals(440.0, config.frequencyOf(Note.A4).hz, 1e-12)

        // A0 son CUATRO OCTAVAS exactas por debajo. 440/16 = 27,5 es exacto en binario, así
        // que esto no admite tolerancia: si no da exacto, la potencia de dos está mal armada.
        assertEquals(27.5, config.frequencyOf(assertNotNull(Note.parse("A0"))).hz, 1e-12)

        // Y las octavas hacia arriba, por la misma razón.
        assertEquals(880.0, config.frequencyOf(assertNotNull(Note.parse("A5"))).hz, 1e-12)

        // Constantes de la tabla publicada de temperamento igual — NO salen de este código.
        assertCloseHz(82.407, config.frequencyOf(assertNotNull(Note.parse("E2"))), 0.001, "E2")
        assertCloseHz(2093.005, config.frequencyOf(assertNotNull(Note.parse("C7"))), 0.001, "C7")
        assertCloseHz(261.626, config.frequencyOf(assertNotNull(Note.parse("C4"))), 0.001, "C4 (do central)")
    }

    @Test
    fun laGuitarraEstandarDaLasSeisFrecuenciasDeLaTabla() {
        val targets = TuningConfiguration(Tuning.GUITAR_STANDARD).targets()

        // En orden de CUERDA (1 = mi agudo), no de frecuencia.
        val expected = listOf("E4" to 329.628, "B3" to 246.942, "G3" to 195.998,
                             "D3" to 146.832, "A2" to 110.000, "E2" to 82.407)
        assertEquals(6, targets.size)
        targets.forEachIndexed { i, t ->
            val (name, hz) = expected[i]
            assertEquals(name, t.note.name, "cuerda ${i + 1}")
            assertCloseHz(hz, t.frequency, 0.001, "cuerda ${i + 1} ($name)")
            assertEquals(i + 1, t.stringIndex)
        }
    }

    // -----------------------------------------------------------------------
    // 3.2 · AC-001.12 — la referencia mueve todo por igual
    // -----------------------------------------------------------------------

    /**
     * Cambiar A4 escala TODAS las frecuencias por el mismo factor y **no cambia un solo
     * intervalo**. Es lo que hace que un conjunto barroco a 415 siga tocando la misma música.
     *
     * El bug que atrapa: aplicar la referencia sólo a la octava de A4, o aplicarla dos veces.
     * Las dos versiones pasan un test que sólo mire A4.
     */
    @Test
    fun cambiarLaReferenciaEscalaTodoPorElMismoFactorYNoMueveLosIntervalos() {
        val a440 = TuningConfiguration(Tuning.GUITAR_STANDARD).targets()
        for (hz in listOf(415.0, 432.0, 466.0)) {
            val other = TuningConfiguration(
                Tuning.GUITAR_STANDARD, reference = TuningReference.of(hz),
            ).targets()

            val factor = hz / 440.0
            a440.zip(other).forEach { (base, moved) ->
                assertTrue(
                    abs(moved.frequency.hz / base.frequency.hz - factor) < 1e-12,
                    "A4=$hz: la cuerda ${base.stringIndex} escaló por " +
                        "${moved.frequency.hz / base.frequency.hz} en vez de $factor",
                )
            }

            // Y los intervalos entre cuerdas quedan IDÉNTICOS.
            val baseIntervals = a440.zipWithNext { a, b -> b.frequency.centsAbove(a.frequency).value }
            val movedIntervals = other.zipWithNext { a, b -> b.frequency.centsAbove(a.frequency).value }
            baseIntervals.zip(movedIntervals).forEach { (x, y) ->
                assertTrue(abs(x - y) < 1e-9, "A4=$hz movió un intervalo: $x → $y")
            }
        }
    }

    @Test
    fun laReferenciaFueraDeRangoSeRechaza() {
        // AC-001.12 declara 415–466. Aceptar 200 Hz no sería generosidad: sería aceptar que
        // alguien afine media octava abajo por un dedo mal puesto en un slider.
        assertFailsWithRequire { TuningReference.of(400.0) }
        assertFailsWithRequire { TuningReference.of(500.0) }
        TuningReference.of(415.0)   // los bordes SÍ entran
        TuningReference.of(466.0)
    }

    // -----------------------------------------------------------------------
    // 3.3 – 3.5 · AC-001.13 — los temperamentos
    // -----------------------------------------------------------------------

    /**
     * En **justo**, la tercera mayor es pura (5/4) y la quinta es pura (3/2). Los valores
     * esperados salen de la teoría de razones, no del código.
     */
    @Test
    fun elTemperamentoJustoTieneTerceraYQuintaPuras() {
        val just = Temperament(TemperamentKind.JUST, tonic = 0)   // en Do
        val dev = just.deviations()

        // Mi (clase 4) contra el Mi de temperamento igual: 386,314 − 400 = −13,686
        assertEquals(-13.686, dev[4], 0.001)
        // Sol (clase 7): 701,955 − 700 = +1,955
        assertEquals(1.955, dev[7], 0.001)

        // Y en hercios, que es lo que ve el usuario: el Mi justo está 13,7 cents por debajo.
        val equalCfg = TuningConfiguration(Tuning.GUITAR_STANDARD)
        val justCfg = equalCfg.copy(temperament = just)
        val e4 = assertNotNull(Note.parse("E4"))
        val delta = justCfg.frequencyOf(e4).centsAbove(equalCfg.frequencyOf(e4)).value
        assertEquals(-13.686, delta, 0.001)
    }

    /**
     * La **coma pitagórica**: doce quintas puras no cierran contra siete octavas, y lo que
     * sobra son 23,460 cents. Es la razón por la que existen todos los demás temperamentos.
     */
    @Test
    fun laComaPitagoricaCierraEnVeintitresComa46() {
        assertEquals(23.460, Temperament.PYTHAGOREAN_COMMA_CENTS, 0.001)
        assertEquals(701.955, Temperament.PYTHAGOREAN_FIFTH_CENTS, 0.001)

        // La tercera pitagórica es ANCHA: 407,8 contra 386,3 de la pura. Es su costo, y un
        // test que sólo mirara las quintas lo dejaría pasar.
        val dev = Temperament(TemperamentKind.PYTHAGOREAN, tonic = 0).deviations()
        assertEquals(407.820 - 400.0, dev[4], 0.001)
    }

    /**
     * **Mesotónico de 1/4 de coma**: la tercera mayor queda PURA y la quinta se achata a
     * 696,578. Cuatro de esas quintas apiladas dan exactamente 5/1.
     */
    @Test
    fun elMesotonicoDeUnCuartoDeComaTieneTerceraPuraYQuintaAchatada() {
        assertEquals(696.578, Temperament.MEANTONE_QUARTER_FIFTH_CENTS, 0.001)

        // La definición: 4 quintas = tercera mayor pura + 2 octavas.
        val fourFifths = 4 * Temperament.MEANTONE_QUARTER_FIFTH_CENTS - 2 * 1200.0
        assertEquals(386.314, fourFifths, 0.001)

        val dev = Temperament(TemperamentKind.MEANTONE_QUARTER, tonic = 0).deviations()
        assertEquals(386.314 - 400.0, dev[4], 0.001)      // tercera pura
        assertEquals(696.578 - 700.0, dev[7], 0.001)      // quinta achatada
    }

    /**
     * 3.6 — la asimetría de la tónica: en los temperamentos NO iguales, mover la tónica mueve
     * los objetivos; en el igual, no mueve nada.
     *
     * Es el test que distingue "implementé el temperamento" de "guardé un campo tónica que
     * nadie lee".
     */
    @Test
    fun laTonicaMueveLosObjetivosSalvoEnTemperamentoIgual() {
        val base = TuningConfiguration(Tuning.GUITAR_STANDARD)

        // Igual: la tónica es irrelevante, y las seis cuerdas tienen que dar IDÉNTICO.
        val equalC = base.copy(temperament = Temperament(TemperamentKind.EQUAL, tonic = 0))
        val equalA = base.copy(temperament = Temperament(TemperamentKind.EQUAL, tonic = 9))
        equalC.targets().zip(equalA.targets()).forEach { (c, a) ->
            assertEquals(c.frequency.hz, a.frequency.hz, 1e-12,
                "en temperamento igual la tónica no puede cambiar nada")
        }

        // No iguales: tiene que cambiar en al menos una cuerda, en los tres.
        for (kind in listOf(TemperamentKind.JUST, TemperamentKind.PYTHAGOREAN,
                            TemperamentKind.MEANTONE_QUARTER)) {
            val inC = base.copy(temperament = Temperament(kind, tonic = 0)).targets()
            val inA = base.copy(temperament = Temperament(kind, tonic = 9)).targets()
            val moved = inC.zip(inA).count { (c, a) -> abs(c.frequency.hz - a.frequency.hz) > 1e-9 }
            assertTrue(moved > 0, "$kind: mover la tónica no cambió ningún objetivo")
        }
    }

    // -----------------------------------------------------------------------
    // 3.7 · AC-001.14 — capo
    // -----------------------------------------------------------------------

    @Test
    fun elCapoTransponeExactoYSaturaSinLanzar() {
        val base = TuningConfiguration(Tuning.GUITAR_STANDARD)

        // +2 semitonos: cada objetivo sube un tono EXACTO (200 cents).
        val capo2 = base.copy(capo = Semitones(2)).targets()
        base.targets().zip(capo2).forEach { (b, c) ->
            assertEquals(200.0, c.frequency.centsAbove(b.frequency).value, 1e-9,
                "cuerda ${b.stringIndex}")
            assertEquals(b.note.midi + 2, c.note.midi)
        }

        // −12: una octava justa abajo, o sea la mitad de la frecuencia.
        val down = base.copy(capo = Semitones(-12)).targets()
        base.targets().zip(down).forEach { (b, d) ->
            assertEquals(b.frequency.hz / 2.0, d.frequency.hz, 1e-9)
        }

        // Saturación: un bajo de 5 cuerdas con capo −12 se sale del rango MIDI por abajo.
        // Tiene que devolver la nota más grave posible, NO lanzar: el capo lo mueve un
        // usuario con un slider.
        val bass = TuningConfiguration(Tuning.BASS_5_STANDARD, capo = Semitones(-12))
        val lowest = bass.targets().last()
        assertTrue(lowest.note.midi >= 0, "saturó por debajo del rango MIDI")

        // Y el rango del capo se valida: ±13 no es un capo.
        assertFailsWithRequire { TuningConfiguration(Tuning.GUITAR_STANDARD, capo = Semitones(13)) }
    }

    // -----------------------------------------------------------------------
    // 3.8 – 3.9 · AC-001.15 — afinaciones REENTRANTES
    // -----------------------------------------------------------------------

    /**
     * **Ukelele high-G**: la cuerda 4 es MÁS AGUDA que la 3.
     *
     * El bug que este test existe para atrapar: resolver el objetivo por orden de frecuencia
     * —"la nota más grave que escucho es la cuerda más grave"—, que le diría al ukelelista que
     * su cuerda 4 está una octava baja. Es la afinación POR DEFECTO del instrumento, así que
     * sería el bug del caso más común.
     */
    @Test
    fun elUkeleleHighGResuelvePorIndiceDeCuerdaYNoPorAltura() {
        val cfg = TuningConfiguration(Tuning.UKULELE_HIGH_G)

        val string3 = assertNotNull(cfg.targetForString(3))
        val string4 = assertNotNull(cfg.targetForString(4))

        assertEquals("C4", string3.note.name)
        assertEquals("G4", string4.note.name)
        assertTrue(
            string4.frequency > string3.frequency,
            "la cuerda 4 (${string4.note}) tiene que ser MÁS AGUDA que la 3 " +
                "(${string3.note}): eso es high-G",
        )
        assertCloseHz(392.0, string4.frequency, 0.01, "G4 de la cuerda 4")

        // Y la MISMA propiedad sobre la lista completa, no sólo por índice suelto: un
        // `sortedBy` adentro de `targets()` daría G4, A4, E4, C4 y el test de arriba —que
        // entra por `targetForString`— no lo vería. Medido: ese mutante sobrevivía.
        val all = cfg.targets()
        assertEquals(listOf("A4", "E4", "C4", "G4"), all.map { it.note.name },
            "targets() reordenó las cuerdas: en high-G el orden de cuerda NO es el de altura")
        assertEquals(listOf(1, 2, 3, 4), all.map { it.stringIndex })

        assertTrue(cfg.tuning.isReentrant, "el ukelele high-G ES reentrante")
        // Y el low-G no lo es: sin esta mitad, `isReentrant` podría devolver true siempre.
        assertTrue(!TuningConfiguration(Tuning.UKULELE_LOW_G).tuning.isReentrant)
    }

    /** **Banjo 5**: el bordón de la cuerda 5 es más agudo que las cuatro restantes. */
    @Test
    fun elBordonDelBanjoEsMasAgudoQueLasCuatroCuerdasRestantes() {
        val cfg = TuningConfiguration(Tuning.BANJO_5_OPEN_G)
        val drone = assertNotNull(cfg.targetForString(5))

        assertEquals("G4", drone.note.name)

        // Igual que en el ukelele: la lista completa conserva el orden de CUERDA.
        assertEquals(listOf("D4", "B3", "G3", "D3", "G4"), cfg.targets().map { it.note.name },
            "targets() reordenó el banjo: el bordón tiene que quedar en la posición 5")

        (1..4).forEach { i ->
            val other = assertNotNull(cfg.targetForString(i))
            assertTrue(
                drone.frequency > other.frequency,
                "el bordón (cuerda 5, ${drone.note}) tiene que ser más agudo que la cuerda " +
                    "$i (${other.note})",
            )
        }
        assertTrue(cfg.tuning.isReentrant)
    }

    // -----------------------------------------------------------------------
    // 3.10 — barrido tabular sobre TODAS las afinaciones
    // -----------------------------------------------------------------------

    /**
     * Cada afinación declarada tiene tantos objetivos como cuerdas su instrumento, y todos
     * caen en un rango audible razonable.
     *
     * Es tabular sobre `Tuning.ALL` a propósito y no sobre una muestra: el modo de falla real
     * de un catálogo es la entrada nueva que alguien agrega mal, y una muestra no la ve.
     */
    @Test
    fun todasLasAfinacionesDeclaradasSonConsistentes() {
        assertTrue(Tuning.ALL.size >= 12, "el catálogo se achicó: ${Tuning.ALL.size}")

        Tuning.ALL.forEach { tuning ->
            val cfg = TuningConfiguration(tuning)
            val targets = cfg.targets()

            assertEquals(tuning.instrument.stringCount, targets.size,
                "'${tuning.id}' declara ${targets.size} notas para ${tuning.instrument.displayName}")

            targets.forEachIndexed { i, t ->
                assertEquals(i + 1, t.stringIndex, "'${tuning.id}': índices desordenados")
                assertTrue(t.frequency.hz in 20.0..5000.0,
                    "'${tuning.id}' cuerda ${t.stringIndex} en ${t.frequency.hz} Hz: fuera de rango")
            }

            // Los ids son únicos: dos afinaciones con el mismo id se pisan en un mapa.
            assertEquals(1, Tuning.ALL.count { it.id == tuning.id }, "id duplicado: ${tuning.id}")
        }

        // El bajo de 5 llega al B0 grave (30,87 Hz), que es la nota que justifica el método
        // de fase — y la que un afinador por FFT no resuelve.
        val b0 = assertNotNull(TuningConfiguration(Tuning.BASS_5_STANDARD).targetForString(5))
        assertCloseHz(30.868, b0.frequency, 0.001, "B0 del bajo de 5")
    }

    // -----------------------------------------------------------------------
    // 3.11 — la conversión es redonda
    // -----------------------------------------------------------------------

    @Test
    fun laConversionHzCentsEsRedondaEnTodoElRango() {
        val reference = Frequency(440.0)
        var f = 20.0
        while (f <= 5000.0) {
            val original = Frequency(f)
            val roundTrip = reference.shiftedBy(original.centsAbove(reference))
            assertTrue(
                abs(roundTrip.hz - f) / f < 1e-9,
                "ida y vuelta en $f Hz devolvió ${roundTrip.hz}",
            )
            f *= 1.037   // ~63 cents por paso: recorre el rango sin caer siempre en semitonos
        }

        // Y el signo: por encima de la referencia es POSITIVO. Es la convención del motor.
        assertTrue(Frequency(880.0).centsAbove(reference).value > 0.0)
        assertEquals(1200.0, Frequency(880.0).centsAbove(reference).value, 1e-9)
        assertEquals(-1200.0, Frequency(220.0).centsAbove(reference).value, 1e-9)
    }

    // -----------------------------------------------------------------------
    // Parseo de notas — la puerta por la que entran los datos
    // -----------------------------------------------------------------------

    @Test
    fun elParseoDeNotasAceptaLoValidoYDevuelveNullEnLoDemas() {
        assertEquals(69, assertNotNull(Note.parse("A4")).midi)
        assertEquals(60, assertNotNull(Note.parse("C4")).midi)
        assertEquals(61, assertNotNull(Note.parse("C#4")).midi)
        assertEquals(61, assertNotNull(Note.parse("Db4")).midi, "C#4 y Db4 son la misma tecla")
        assertEquals(28, assertNotNull(Note.parse("E1")).midi)

        // Devuelve null y NO lanza: estos nombres vienen de datos o de la UI.
        assertNull(Note.parse(""))
        assertNull(Note.parse("H4"))
        assertNull(Note.parse("A"))
        assertNull(Note.parse("A99"))
    }

    private inline fun assertFailsWithRequire(block: () -> Unit) {
        val threw = try { block(); false } catch (e: IllegalArgumentException) { true }
        assertTrue(threw, "se esperaba que rechazara el valor inválido")
    }
}
