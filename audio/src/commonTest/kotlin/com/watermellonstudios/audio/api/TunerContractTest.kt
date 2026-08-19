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
 * REQ-001 S3 · 3.16–3.17 — el contrato de [ITuner], ejercitado con un doble que **usa el
 * modelo de verdad**.
 *
 * Lo que estos tests prueban no es el doble: es que la unión de las dos mitades —lo que
 * debería sonar y lo que suena— conserva las propiedades que cada mitad garantiza por
 * separado. Es el único lugar donde se ve si el pegamento las traiciona.
 */
class TunerContractTest {

    private fun tuner(
        tuning: Tuning = Tuning.GUITAR_STANDARD,
        config: TuningConfiguration = TuningConfiguration(tuning),
    ) = FakeTuner(config)

    @Test
    fun losObjetivosSalenDelModeloYNoDeUnaTablaInerte() {
        val t = tuner()

        assertEquals(6, t.targets.size)
        assertEquals("E2", t.targets[5].note.name)
        assertTrue(abs(t.targets[5].frequency.hz - 82.407) < 0.001)

        // Y responden a la configuración: si fueran una tabla fija, esto no se movería.
        t.configuration = t.configuration.copy(reference = TuningReference.BAROQUE)
        assertTrue(
            abs(t.targets[5].frequency.hz - 82.407 * 415.0 / 440.0) < 0.001,
            "cambiar la referencia no movió los objetivos: el doble está inerte",
        )

        t.configuration = t.configuration.copy(capo = Semitones(2))
        assertEquals("F#2", t.targets[5].note.name)
    }

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
        val t = tuner()
        assertTrue(t.pushedTargets.isEmpty(), "sin cuerda elegida no hay nada que empujar")

        t.selectedString = 6
        assertEquals(1, t.pushedTargets.size)
        assertTrue(abs(t.pushedTargets.last() - 82.407) < 0.001)

        t.selectedString = 1
        assertTrue(abs(t.pushedTargets.last() - 329.628) < 0.001)

        // Y la configuración también: el objetivo de la MISMA cuerda cambió de frecuencia.
        val before = t.pushedTargets.size
        t.configuration = t.configuration.copy(reference = TuningReference.BAROQUE)
        assertEquals(before + 1, t.pushedTargets.size,
            "cambiar la referencia movió el objetivo y no se re-empujó")
        assertTrue(abs(t.pushedTargets.last() - 329.628 * 415.0 / 440.0) < 0.01)
    }

    /**
     * Sin lectura no hay lectura: `null` y **no** un cero que se lea como "afinado".
     */
    @Test
    fun sinDatosLaLecturaEsNullYNoUnCeroQueParezcaAfinado() {
        val t = tuner()
        t.selectedString = 6
        assertNull(t.reading(), "no se publicó nada todavía")

        t.start()
        assertNull(t.reading(), "arrancar no inventa una medición")
    }

    /**
     * **El estado de HOY del motor**: el snapshot llega con `cents = null` porque el
     * estimador todavía no está cableado al thread de análisis.
     *
     * Este test existe para que ese estado sea visible en la suite y no una sorpresa en la
     * app: la lectura tiene objetivo, tiene nivel y tiene estado, pero **no tiene cents**. Una
     * UI que asuma un número acá se rompe hoy, no dentro de tres meses.
     */
    @Test
    fun unSnapshotSinPitchDaLecturaConObjetivoPeroSinCents() {
        val t = tuner()
        t.selectedString = 6
        t.scriptedSnapshots.addLast(FakeTuner.snapshotWithoutPitch())
        t.start()

        val reading = assertNotNull(t.reading())
        assertNotNull(reading.target, "el objetivo sale del modelo y siempre está")
        assertEquals("E2", reading.target.note.name)
        assertNull(reading.cents, "el motor todavía no mide altura: cents es ausencia, no cero")
        assertTrue(!reading.isConverged, "sin cents no se puede declarar convergido")
        assertEquals(48000, reading.snapshot.captureSampleRate)
    }

    @Test
    fun conPitchLaLecturaJuntaObjetivoYMedicion() {
        val t = tuner()
        t.selectedString = 5                      // A2
        t.scriptedSnapshots.addLast(FakeTuner.snapshotWithPitch(cents = +3.2f))
        t.start()

        val reading = assertNotNull(t.reading())
        assertEquals("A2", assertNotNull(reading.target).note.name)
        assertEquals(3.2f, assertNotNull(reading.cents))
        assertTrue(reading.isConverged)
    }

    /**
     * `isConverged` exige las TRES cosas: objetivo, medición y estado convergido. Sin esta
     * exigencia, una UI mostraría el tilde verde sobre una lectura sin objetivo.
     */
    @Test
    fun convergidoExigeObjetivoMedicionYEstado() {
        val t = tuner()
        t.scriptedSnapshots.addLast(FakeTuner.snapshotWithPitch(cents = 0.1f))
        t.start()
        // Sin cuerda elegida: hay medición pero no hay contra qué.
        assertTrue(!assertNotNull(t.reading()).isConverged, "sin objetivo no hay convergencia")

        t.selectedString = 5
        t.scriptedSnapshots.addLast(
            FakeTuner.snapshotWithPitch(cents = 0.1f, state = TunerState.MEASURING),
        )
        assertTrue(!assertNotNull(t.reading()).isConverged, "midiendo todavía no es convergido")
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
        val t = tuner(Tuning.UKULELE_HIGH_G)

        t.selectedString = 4
        assertEquals("G4", assertNotNull(t.targets.getOrNull(3)).note.name)
        assertTrue(abs(t.pushedTargets.last() - 392.0) < 0.1,
            "empujó ${t.pushedTargets.last()} Hz: la cuerda 4 del high-G es G4, no G3")

        t.selectedString = 3
        assertTrue(t.pushedTargets.last() < 300.0, "la cuerda 3 (C4) es MÁS GRAVE que la 4")
    }

    @Test
    fun pararNoBorraLaUltimaLectura() {
        val t = tuner()
        t.selectedString = 6
        t.scriptedSnapshots.addLast(FakeTuner.snapshotWithPitch(cents = -1.5f))
        t.start()
        assertNotNull(t.reading())

        t.stop()
        assertTrue(!t.isRunning)
        val after = assertNotNull(t.reading(), "parar no puede borrar la última medición")
        assertEquals(-1.5f, assertNotNull(after.cents))
    }
}
