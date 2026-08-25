package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.tuner.TunerState
import com.watermellonstudios.audio.domain.tuning.Semitones
import com.watermellonstudios.audio.domain.tuning.Tuning
import com.watermellonstudios.audio.domain.tuning.TuningConfiguration
import com.watermellonstudios.audio.domain.tuning.TuningReference
import kotlin.math.abs
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * REQ-001 S3 · 3.16–3.17 — el contrato de [ITuner], ejercido contra **todas** sus
 * implementaciones (MINI-004).
 *
 * Lo que estos tests prueban no es una implementación: es que la unión de las dos mitades
 * —lo que debería sonar y lo que suena— conserva las propiedades que cada mitad garantiza
 * por separado. Es el único lugar donde se ve si el pegamento las traiciona.
 *
 * ## Qué cambió, y por qué importa
 *
 * Hasta MINI-004 este archivo se llamaba **contrato** y su único constructor de sujeto
 * devolvía `FakeTuner` siempre — el doble que lo cumple **por construcción**. Un contrato
 * verificado sólo contra eso no dice nada sobre un implementador real. Era inevitable
 * mientras no hubiera otro; con `TunerImpl` (REQ-010 S1) dejó de serlo.
 *
 * Cada caso itera [tunerSubjects] y **nombra el sujeto en cada mensaje de fallo**: sin eso,
 * un rojo obliga a adivinar cuál de las dos implementaciones falló. Ver [TunerSubjects.kt]
 * para el porqué de la forma.
 */
class TunerContractTest {

    private fun sujetos(tuning: Tuning = Tuning.GUITAR_STANDARD) =
        tunerSubjects(TuningConfiguration(tuning))

    // =======================================================================
    // AC-004.3 — el contrato ejerce TODA implementación, o falla
    // =======================================================================

    /**
     * El criterio de muerte de MINI-004 hecho test, en su mitad barata.
     *
     * La mitad cara —descubrir implementaciones nuevas en el fuente— la hace
     * `scripts/check-ituner-implementations.py`, porque Kotlin/Native **no tiene
     * reflection** y ningún test en runtime puede enumerar las implementaciones del módulo.
     * Este test sólo ata la lista declarada a los sujetos que realmente se ejercen: sin él,
     * la lista podría decir tres y el contrato ejercer dos.
     */
    @Test
    fun elContratoEjerceExactamenteLasImplementacionesDeclaradas() {
        assertEquals(
            IMPLEMENTACIONES_EJERCIDAS,
            sujetos().map { it.name },
            "la lista declarada y los sujetos que el contrato ejerce se separaron. El guard " +
                "de fuente compara ESA lista contra el árbol, así que si acá sobra o falta " +
                "uno, el guard queda vigilando algo que no se corre.",
        )
    }

    // =======================================================================
    // Los objetivos salen del modelo
    // =======================================================================

    @Test
    fun losObjetivosSalenDelModeloYNoDeUnaTablaInerte() {
        sujetos().forEach { s ->
            val t = s.tuner

            assertEquals(6, t.targets.size, "$s: una guitarra tiene 6 cuerdas")
            assertEquals("E2", t.targets[5].note.name, "$s: la cuerda 6 es el mi grave")
            assertTrue(abs(t.targets[5].frequency.hz - 82.407) < 0.001, "$s: E2 son 82,407 Hz")

            // Y responden a la configuración: si fueran una tabla fija, esto no se movería.
            t.configuration = t.configuration.copy(reference = TuningReference.BAROQUE)
            assertTrue(
                abs(t.targets[5].frequency.hz - 82.407 * 415.0 / 440.0) < 0.001,
                "$s: cambiar la referencia no movió los objetivos — está inerte",
            )

            t.configuration = t.configuration.copy(capo = Semitones(2))
            assertEquals("F#2", t.targets[5].note.name, "$s: el capo transpone")
        }
    }

    // =======================================================================
    // AC-004.2 — la obligación de empuje, SUS DOS MITADES
    // =======================================================================

    /**
     * La obligación que la interfaz declara en prosa: **cambiar la configuración o la cuerda
     * obliga a re-empujar el objetivo al motor.**
     *
     * Sin esto, el estimador —que afina alrededor de un objetivo y no lo busca— se queda
     * midiendo contra la nota anterior y reporta "sin enganche" para siempre. Parecería un
     * problema del DSP y no lo sería.
     */
    @Test
    fun cambiarLaCuerdaOLaConfiguracionEmpujaElObjetivoAlMotor() {
        sujetos().forEach { s ->
            val t = s.tuner
            assertTrue(
                s.pushedTargets.isEmpty(),
                "$s: sin cuerda elegida no hay nada que empujar, y empujó ${s.pushedTargets}",
            )

            t.selectedString = 6
            assertEquals(1, s.pushedTargets.size, "$s: elegir cuerda empuja una vez")
            assertTrue(
                abs(s.pushedTargets.last() - 82.407) < 0.01,
                "$s: empujó ${s.pushedTargets.last()} Hz y la cuerda 6 es E2 (82,407)",
            )

            t.selectedString = 1
            assertTrue(
                abs(s.pushedTargets.last() - 329.628) < 0.01,
                "$s: empujó ${s.pushedTargets.last()} Hz y la cuerda 1 es E4 (329,628)",
            )

            // Y la configuración también: el objetivo de la MISMA cuerda cambió de frecuencia.
            val before = s.pushedTargets.size
            t.configuration = t.configuration.copy(reference = TuningReference.BAROQUE)
            assertEquals(
                before + 1,
                s.pushedTargets.size,
                "$s: cambiar la referencia movió el objetivo y no se re-empujó",
            )
            assertTrue(
                abs(s.pushedTargets.last() - 329.628 * 415.0 / 440.0) < 0.01,
                "$s: el re-empuje no usó la referencia nueva",
            )
        }
    }

    /**
     * 🔴 **La mitad que se olvida, y la cara.**
     *
     * `ITunerBridge.setTunerTargetHz` *"reinicia la integración"*, así que re-empujar el
     * mismo objetivo deja un afinador que **nunca converge** — y desde afuera eso es
     * indistinguible de no empujar nunca: los dos se ven como un DSP roto.
     *
     * Este caso no existía en el contrato hasta MINI-004, y su ausencia era el punto ciego
     * exacto: `pushedTargets` permitía exigir que alguien empujara, y **nadie miraba al que
     * empujaba de más**. `FakeTuner` empujaba en cada asignación, o sea que la única
     * implementación existente incumplía la obligación que este mismo contrato dice vigilar.
     */
    @Test
    fun reasignarElMismoObjetivoNoVuelveAEmpujar() {
        sujetos().forEach { s ->
            val t = s.tuner

            t.selectedString = 3
            t.selectedString = 3
            t.selectedString = 3

            assertEquals(
                1,
                s.pushedTargets.size,
                "$s: reasignar el MISMO valor volvió a empujar. Cada empuje reinicia la " +
                    "integración del estimador, así que esto es un afinador que nunca " +
                    "converge. Empujes vistos: ${s.pushedTargets}",
            )

            // Y la configuración que NO mueve el objetivo tampoco puede empujar: "cambió" se
            // mide sobre los Hz efectivos, no sobre la identidad del objeto de config.
            t.configuration = t.configuration.copy(reference = TuningReference.STANDARD)
            assertEquals(
                1,
                s.pushedTargets.size,
                "$s: reasignar la MISMA referencia (440 Hz) movió cero y empujó igual. " +
                    "Empujes vistos: ${s.pushedTargets}",
            )
        }
    }

    // =======================================================================
    // La lectura
    // =======================================================================

    /** Sin lectura no hay lectura: `null` y **no** un cero que se lea como "afinado". */
    @Test
    fun sinDatosLaLecturaEsNullYNoUnCeroQueParezcaAfinado() {
        sujetos().forEach { s ->
            val t = s.tuner
            t.selectedString = 6
            assertNull(t.reading(), "$s: no se publicó nada todavía")

            t.start()
            assertNull(t.reading(), "$s: arrancar no inventa una medición")
        }
    }

    /**
     * **El estado de HOY del motor**: el snapshot llega con `cents = null` porque el
     * estimador todavía no está cableado al thread de análisis.
     *
     * Este test existe para que ese estado sea visible en la suite y no una sorpresa en la
     * app: la lectura tiene objetivo, tiene nivel y tiene estado, pero **no tiene cents**.
     * Una UI que asuma un número acá se rompe hoy, no dentro de tres meses.
     */
    @Test
    fun unSnapshotSinPitchDaLecturaConObjetivoPeroSinCents() {
        sujetos().forEach { s ->
            val t = s.tuner
            t.selectedString = 6
            s.publicar(nativoSinPitch())
            t.start()

            val reading = assertNotNull(t.reading(), "$s: había un snapshot publicado")
            assertNotNull(reading.target, "$s: el objetivo sale del modelo y siempre está")
            assertEquals("E2", reading.target.note.name, "$s: la cuerda 6 es E2")
            assertNull(reading.cents, "$s: cents es ausencia, no cero")
            assertTrue(!reading.isConverged, "$s: sin cents no se puede declarar convergido")
            assertEquals(48000, reading.snapshot.captureSampleRate, "$s: el sample rate cruzó")
        }
    }

    @Test
    fun conPitchLaLecturaJuntaObjetivoYMedicion() {
        sujetos().forEach { s ->
            val t = s.tuner
            t.selectedString = 5                      // A2
            s.publicar(nativoConPitch(cents = 3.2f))
            t.start()

            val reading = assertNotNull(t.reading(), "$s: había un snapshot publicado")
            assertEquals("A2", assertNotNull(reading.target).note.name, "$s: la cuerda 5 es A2")
            assertEquals(3.2f, assertNotNull(reading.cents), "$s: los cents cruzaron")
            assertTrue(reading.isConverged, "$s: hay objetivo, medición y estado convergido")
        }
    }

    /**
     * REQ-009 (AC-009.3) — `isInputBroken` es la pregunta que sigue a `isConverged`.
     *
     * Cuando aquélla da `false`, ésta dice **qué hacer**: `false` en las dos = esperar;
     * `true` acá = revisar el cable. Van los DOS lados en el mismo test porque una
     * propiedad clavada en `false` pasa la mitad de arriba sola — y clavada en `true`
     * pasaría la de abajo.
     */
    @Test
    fun laEntradaRotaSeDistingueDeTodaviaNoConvergi() {
        sujetos().forEach { s ->
            val t = s.tuner
            t.selectedString = 5
            t.start()

            // Midiendo con la entrada SANA: hay que esperar.
            s.publicar(nativoConPitch(cents = 0.1f, state = TunerState.MEASURING))
            val esperando = assertNotNull(t.reading(), "$s: no publicó lectura")
            assertTrue(!esperando.isConverged, "$s: midiendo no es convergido")
            assertTrue(
                !esperando.isInputBroken,
                "$s: la entrada está entera y aun así dice que llegó rota. Eso manda al " +
                    "músico a buscar un problema que no existe.",
            )

            // Mismo estado, entrada ROTA: hay que revisar el cable.
            s.publicar(
                nativoConPitch(
                    cents = 0.1f,
                    state = TunerState.MEASURING,
                    inputDiscontinuity = 1f,
                ),
            )
            val rota = assertNotNull(t.reading(), "$s: no publicó lectura")
            assertTrue(!rota.isConverged, "$s: midiendo no es convergido")
            assertTrue(
                rota.isInputBroken,
                "$s: el motor avisó que la entrada llegó rota y la superficie no lo " +
                    "transmite. El consumidor ve el MISMO estado que arriba y espera para " +
                    "siempre.",
            )
        }
    }

    /**
     * Y nunca las dos a la vez: el motor no publica convergido sobre una integración con
     * hueco (AC-009.1), así que la superficie no puede inventar esa combinación.
     */
    @Test
    fun convergidoYEntradaRotaSonMutuamenteExcluyentes() {
        sujetos().forEach { s ->
            val t = s.tuner
            t.selectedString = 5
            t.start()
            s.publicar(
                nativoConPitch(
                    cents = 0.1f,
                    state = TunerState.CONVERGED,
                    inputDiscontinuity = 1f,
                ),
            )

            val reading = assertNotNull(t.reading(), "$s: no publicó lectura")
            assertTrue(reading.isConverged, "$s: el estado dice convergido")
            assertTrue(
                !reading.isInputBroken,
                "$s: si la lectura está convergida no hay nada que mandar a revisar — " +
                    "`isInputBroken` explica una AUSENCIA de convergencia.",
            )
        }
    }

    /**
     * `isConverged` exige las TRES cosas: objetivo, medición y estado convergido. Sin esta
     * exigencia, una UI mostraría el tilde verde sobre una lectura sin objetivo.
     */
    @Test
    fun convergidoExigeObjetivoMedicionYEstado() {
        sujetos().forEach { s ->
            val t = s.tuner
            s.publicar(nativoConPitch(cents = 0.1f))
            t.start()
            // Sin cuerda elegida: hay medición pero no hay contra qué.
            assertTrue(
                !assertNotNull(t.reading(), "$s: hay snapshot").isConverged,
                "$s: sin objetivo no hay convergencia",
            )

            t.selectedString = 5
            s.publicar(nativoConPitch(cents = 0.1f, state = TunerState.MEASURING))
            assertTrue(
                !assertNotNull(t.reading(), "$s: hay snapshot").isConverged,
                "$s: midiendo todavía no es convergido",
            )
        }
    }

    /**
     * En ukelele high-G la cuerda 4 es la MÁS AGUDA, y el afinador tiene que apuntar a G4 —
     * no a la nota más grave que le toque el índice si alguien ordenó la lista.
     *
     * Es AC-001.15 atravesando la interfaz: el invariante del modelo tiene que sobrevivir al
     * pegamento.
     */
    @Test
    fun elInvarianteReentranteSobreviveALaInterfaz() {
        sujetos(Tuning.UKULELE_HIGH_G).forEach { s ->
            val t = s.tuner

            t.selectedString = 4
            assertEquals(
                "G4",
                assertNotNull(t.targets.getOrNull(3)).note.name,
                "$s: la cuerda 4 del high-G es G4",
            )
            assertTrue(
                abs(s.pushedTargets.last() - 392.0) < 0.1,
                "$s: empujó ${s.pushedTargets.last()} Hz — la cuerda 4 del high-G es G4, no G3",
            )

            t.selectedString = 3
            assertTrue(
                s.pushedTargets.last() < 300.0,
                "$s: la cuerda 3 (C4) es MÁS GRAVE que la 4, y empujó ${s.pushedTargets.last()}",
            )
        }
    }

    /**
     * Parar no borra la última medición — y **no** porque el envoltorio la cachee.
     *
     * El motor lee de un buffer publicado y `wma_tuner_stop` no libera ni el ring ni el
     * snapshot, así que la garantía es de abajo. Un caché en el envoltorio sobreviviría
     * también a la destrucción del subsistema, convirtiendo *"no sé"* en *"sigue igual"*.
     * Ver MINI-003.
     */
    @Test
    fun pararNoBorraLaUltimaLectura() {
        sujetos().forEach { s ->
            val t = s.tuner
            t.selectedString = 6
            s.publicar(nativoConPitch(cents = -1.5f))
            t.start()
            assertNotNull(t.reading(), "$s: hay una lectura antes de parar")

            t.stop()
            assertTrue(!t.isRunning, "$s: paró")
            val after = assertNotNull(t.reading(), "$s: parar no puede borrar la última medición")
            assertEquals(-1.5f, assertNotNull(after.cents), "$s: y es la MISMA medición")
        }
    }

    // =======================================================================
    // Los guiones, en forma nativa
    // =======================================================================

    /**
     * Los [com.watermellonstudios.audio.domain.tuner.TunerSnapshot.VALUE_COUNT] floats en el
     * orden que documenta `wma_tuner_get_snapshot`.
     *
     * `NaN` y no `0f` para lo ausente: es lo que hace el motor, y por el mismo motivo —0,0
     * cents es una lectura **plausible** (afinado exacto) que una UI dibujaría como medición.
     */
    private fun nativo(
        cents: Float,
        state: Float,
        inputDiscontinuity: Float = 0f,
        discontinuityCount: Float = 0f,
    ): FloatArray = floatArrayOf(
        48000f, 0.2f, 48000f, 0f, state,
        cents, 0.3f, 0.01f,
        440f, 0.99f,
        Float.NaN, 0f,
        0f, 2f, 21f,
        inputDiscontinuity,
        discontinuityCount,
    )

    private fun nativoSinPitch(): FloatArray =
        nativo(cents = Float.NaN, state = 1f)          // NO_LOCK

    private fun nativoConPitch(
        cents: Float,
        state: TunerState = TunerState.CONVERGED,
        inputDiscontinuity: Float = 0f,
    ): FloatArray = nativo(
        inputDiscontinuity = inputDiscontinuity,
        cents = cents,
        state = when (state) {
            TunerState.NO_SIGNAL -> 0f
            TunerState.NO_LOCK -> 1f
            TunerState.MEASURING -> 2f
            TunerState.CONVERGED -> 3f
            TunerState.UNKNOWN -> 9f
        },
    )
}
