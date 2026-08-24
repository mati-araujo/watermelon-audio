package com.watermellonstudios.audio.internal.tuner

import com.watermellonstudios.audio.api.ITunerBridge
import com.watermellonstudios.audio.domain.tuner.TunerState
import com.watermellonstudios.audio.domain.tuning.Semitones
import com.watermellonstudios.audio.domain.tuning.Tuning
import com.watermellonstudios.audio.domain.tuning.TuningConfiguration
import com.watermellonstudios.audio.domain.tuning.TuningReference
import com.watermellonstudios.audio.domain.tuning.Frequency
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * REQ-010 S1 — el pegamento entre las 12 funciones de [ITunerBridge] y los 7 miembros de
 * `ITuner`.
 *
 * QUÉ SE PRUEBA ACÁ, Y POR QUÉ NO ES "UN ENVOLTORIO TRIVIAL"
 * ----------------------------------------------------------
 * El DSP no se prueba acá: eso necesita audio y ya lo cubre la suite de C++. Lo que vive en
 * esta capa —y hasta ahora no existía en ninguna parte del artefacto publicado— es **una
 * decisión con dos fallos simétricos que se ven idénticos desde afuera**:
 *
 *   - empujar el objetivo en **cada** asignación ⇒ [ITunerBridge.setTunerTargetHz] *"reinicia
 *     la integración"* ⇒ el afinador **nunca converge**;
 *   - no empujarlo **nunca** ⇒ el estimador se queda sin dónde enganchar ⇒ el motor reporta
 *     "sin enganche" para siempre.
 *
 * Los dos parecen un DSP roto. Por eso los tests de AC-010.2 afirman **las dos mitades**: que
 * empuja cuando el objetivo efectivo cambió, y que **no** empuja cuando no cambió. Un archivo
 * que sólo afirme la primera deja pasar el fallo más caro — y es exactamente el punto ciego
 * que tiene hoy `FakeTuner`, la única implementación previa de `ITuner`, que empuja en cada
 * asignación sin comparar con el anterior.
 *
 * EL DOBLE ES DE [ITunerBridge], NO DE `ITuner`
 * ---------------------------------------------
 * `FakeTuner` no sirve como arnés acá: implementa la interfaz de arriba, así que su
 * `pushedTargets` registra lo que **él mismo** hace, no lo que el motor recibe. El doble que
 * hace falta es el de la frontera, y va **con comportamiento real** —guarda el objetivo y lo
 * devuelve— y no inerte: esta librería ya shippeó dos stubs que devolvían ceros, y los ceros
 * derrotaron los fallbacks elvis de sus propios callers con los tests en verde.
 */
class TunerImplTest {

    private val guitar = TuningConfiguration(Tuning.GUITAR_STANDARD)

    private fun tunerOver(
        bridge: FakeTunerBridge,
        configuration: TuningConfiguration = guitar,
    ) = TunerImpl(bridge, configuration)

    /** Los 16 floats en el orden que documenta `wma_tuner_get_snapshot`. */
    private fun nativeSnapshot(
        cents: Float = -5f,
        state: Float = 3f,
        inputDiscontinuity: Float = 0f,
    ): FloatArray = floatArrayOf(
        48000f, 0.2f, 48000f, 0f, state,
        cents, 0.3f, 0.01f,
        440f, 0.99f,
        0f, 0f,
        0f, 2f, 21f,
        inputDiscontinuity,
    )

    // =======================================================================
    // AC-010.2 — empujar cuando cambió, y SÓLO cuando cambió
    // =======================================================================

    @Test
    fun choosingAStringPushesItsTargetOnce() {
        val bridge = FakeTunerBridge()
        val tuner = tunerOver(bridge)

        tuner.selectedString = 6

        assertEquals(
            listOf(guitar.targets()[5].frequency.hz.toFloat()),
            bridge.pushedHz,
            "elegir una cuerda tiene que empujar SU frecuencia al motor, una vez: el " +
                "estimador afina alrededor de un objetivo, no lo busca",
        )
    }

    @Test
    fun reassigningTheSameStringDoesNotPushAgain() {
        // 🔴 La mitad que se olvida, y la cara: setTunerTargetHz REINICIA la integración,
        // así que re-empujar el mismo objetivo es un afinador que nunca converge — y el
        // síntoma es indistinguible de un DSP roto.
        val bridge = FakeTunerBridge()
        val tuner = tunerOver(bridge)

        tuner.selectedString = 3
        tuner.selectedString = 3
        tuner.selectedString = 3

        assertEquals(
            1,
            bridge.pushedHz.size,
            "reasignar el MISMO valor no puede volver a empujar el objetivo: cada empuje " +
                "reinicia la integración del estimador. Empujes vistos: ${bridge.pushedHz}",
        )
    }

    @Test
    fun changingToAnotherStringPushesTheNewTarget() {
        val bridge = FakeTunerBridge()
        val tuner = tunerOver(bridge)

        tuner.selectedString = 1
        tuner.selectedString = 2

        assertEquals(
            listOf(
                guitar.targets()[0].frequency.hz.toFloat(),
                guitar.targets()[1].frequency.hz.toFloat(),
            ),
            bridge.pushedHz,
            "cambiar de cuerda sí mueve el objetivo efectivo, así que sí hay que empujar",
        )
    }

    @Test
    fun changingTheConfigurationPushesTheMovedTarget() {
        val bridge = FakeTunerBridge()
        val tuner = tunerOver(bridge)
        tuner.selectedString = 1
        val beforeCapo = bridge.pushedHz.size

        tuner.configuration = guitar.copy(capo = Semitones(2))

        assertEquals(
            beforeCapo + 1,
            bridge.pushedHz.size,
            "el capo mueve el objetivo de la cuerda elegida: si no se re-empuja, el motor " +
                "sigue midiendo contra la nota vieja y la app muestra cents de otra cuerda",
        )
        assertEquals(
            tuner.targets[0].frequency.hz.toFloat(),
            bridge.pushedHz.last(),
            "y lo que se empuja es el objetivo NUEVO",
        )
    }

    @Test
    fun aConfigurationThatLandsOnTheSameTargetDoesNotPush() {
        // El guardia es sobre el objetivo EFECTIVO en Hz, no sobre la identidad del objeto de
        // configuración. Una config distinta que deja la cuerda elegida en la misma frecuencia
        // no reinicia nada — y un impl que compare configuraciones en vez de Hz falla acá.
        val bridge = FakeTunerBridge()
        val tuner = tunerOver(bridge)
        tuner.selectedString = 1
        val pushesAfterSelecting = bridge.pushedHz.size

        tuner.configuration = guitar.copy(reference = TuningReference(Frequency(440.0)))

        assertEquals(
            pushesAfterSelecting,
            bridge.pushedHz.size,
            "misma frecuencia objetivo ⇒ nada que reiniciar. Empujes: ${bridge.pushedHz}",
        )
    }

    @Test
    fun deselectingTheStringClearsTheTargetInsteadOfLeavingTheOldOne() {
        val bridge = FakeTunerBridge()
        val tuner = tunerOver(bridge)
        tuner.selectedString = 4

        tuner.selectedString = null

        assertEquals(
            0f,
            bridge.pushedHz.last(),
            "soltar la cuerda tiene que BORRAR el objetivo (0 Hz, como documenta el puente): " +
                "dejar el viejo hace que el motor siga publicando cents contra una cuerda que " +
                "el usuario ya no está afinando",
        )
        assertEquals(0f, bridge.getTunerTargetHz())
    }

    @Test
    fun constructingTheTunerDoesNotTouchTheEngineTarget() {
        // Construir una vista no puede pisar el objetivo que otra puso: `create()` devuelve
        // una instancia nueva y el último empuje gana, así que un empuje en el constructor
        // convertiría "mirar el afinador" en "reiniciar el afinador de otro".
        val bridge = FakeTunerBridge()

        tunerOver(bridge)

        assertTrue(
            bridge.pushedHz.isEmpty(),
            "sin cuerda elegida no hay objetivo que empujar. Empujes: ${bridge.pushedHz}",
        )
    }

    // =======================================================================
    // AC-010.3 — "no sé" no se convierte en "sigue igual"
    // =======================================================================

    @Test
    fun readingIsNullWhenTheEngineHasNotPublishedAnything() {
        val bridge = FakeTunerBridge().apply { snapshot = null }

        assertNull(
            tunerOver(bridge).reading(),
            "sin snapshot el contrato es null: null no es 'afinado' ni 'cero'",
        )
    }

    @Test
    fun readingStopsBeingServedWhenTheEngineGoesQuiet() {
        // 🔴 El test que mata el caché. Un envoltorio que guarde la última lectura convierte
        // "no sé" en "sigue igual" — el no-op disfrazado de dato, y la aguja se queda clavada
        // en un número que nadie está midiendo.
        val bridge = FakeTunerBridge()
        val tuner = tunerOver(bridge)
        tuner.selectedString = 1
        bridge.snapshot = nativeSnapshot(cents = -5f)
        assertNotNull(tuner.reading(), "control positivo: con snapshot sí hay lectura")

        bridge.snapshot = null

        assertNull(
            tuner.reading(),
            "cuando el motor deja de publicar, la lectura vuelve a ser null y no la última " +
                "conocida",
        )
    }

    @Test
    fun aShortSnapshotIsRefusedInsteadOfReadAsAMeasurement() {
        val bridge = FakeTunerBridge().apply { snapshot = floatArrayOf(48000f, 0.2f, 1f) }

        assertNull(
            tunerOver(bridge).reading(),
            "un array corto es un contrato roto con la capa nativa: leer el prefijo como si " +
                "fuera una medición es peor que no devolver nada",
        )
    }

    @Test
    fun theReadingCarriesBothHalves() {
        val bridge = FakeTunerBridge()
        val tuner = tunerOver(bridge)
        tuner.selectedString = 1
        bridge.snapshot = nativeSnapshot(cents = -5f, state = 3f)

        val reading = assertNotNull(tuner.reading())

        assertEquals(
            tuner.targets[0],
            reading.target,
            "la lectura lleva contra QUÉ se midió, no sólo el número",
        )
        assertEquals(-5f, reading.cents)
        assertEquals(TunerState.CONVERGED, reading.snapshot.state)
        assertTrue(reading.isConverged)
    }

    @Test
    fun everyReadingAsksTheEngineAgain() {
        val bridge = FakeTunerBridge().apply { snapshot = nativeSnapshot() }
        val tuner = tunerOver(bridge)

        tuner.reading()
        tuner.reading()
        tuner.reading()

        assertEquals(
            3,
            bridge.snapshotCalls,
            "cada lectura cruza la frontera: el snapshot es del tick, no de la sesión",
        )
    }

    // =======================================================================
    // AC-010.4 — fuera de rango deja SIN objetivo, no afinando otra cuerda
    // =======================================================================

    @Test
    fun anOutOfRangeStringLeavesTheTunerWithoutTarget() {
        // Un `coerceIn` acá afinaría la cuerda equivocada EN SILENCIO, que es peor que no
        // afinar ninguna: el usuario ve una aguja plausible contra un objetivo que no eligió.
        val bridge = FakeTunerBridge()
        val tuner = tunerOver(bridge)
        bridge.snapshot = nativeSnapshot()

        tuner.selectedString = 9

        assertNull(tuner.reading()?.target, "la cuerda 9 no existe en una guitarra de 6")
        assertTrue(
            bridge.pushedHz.none { it > 0f },
            "y no se empujó el objetivo de NINGUNA cuerda. Empujes: ${bridge.pushedHz}",
        )
    }

    @Test
    fun stringZeroIsOutOfRangeBecauseTheIndexIsOneBased() {
        // 1-based, y las tres fuentes coinciden: el KDoc de ITuner ("numerada desde 1"),
        // StringTarget.stringIndex (targets() mapea con i + 1) y FakeTuner (getOrNull(i - 1)).
        val bridge = FakeTunerBridge()
        val tuner = tunerOver(bridge)
        bridge.snapshot = nativeSnapshot()

        tuner.selectedString = 0

        assertNull(tuner.reading()?.target)
        assertTrue(bridge.pushedHz.none { it > 0f }, "empujes: ${bridge.pushedHz}")
    }

    @Test
    fun aNegativeStringDoesNotWrapAroundToTheLastOne() {
        val bridge = FakeTunerBridge()
        val tuner = tunerOver(bridge)
        bridge.snapshot = nativeSnapshot()

        tuner.selectedString = -1

        assertNull(tuner.reading()?.target, "−1 no es 'la última cuerda'")
        assertTrue(bridge.pushedHz.none { it > 0f }, "empujes: ${bridge.pushedHz}")
    }

    @Test
    fun theSelectedStringIsOneBasedAgainstTheTargetList() {
        val bridge = FakeTunerBridge()
        val tuner = tunerOver(bridge)
        bridge.snapshot = nativeSnapshot()

        tuner.selectedString = 1

        assertEquals(
            1,
            tuner.reading()?.target?.stringIndex,
            "la cuerda 1 del músico es targets[0]",
        )
    }

    // =======================================================================
    // Lo que se pregunta al motor en vez de espejarlo
    // =======================================================================

    @Test
    fun isRunningIsAskedToTheEngineInsteadOfMirrored() {
        // Un booleano espejado se desincroniza del motor y miente: el afinador se puede
        // parar por debajo (se cayó el nodo de entrada) sin que nadie llame a stop().
        //
        // 🔴 LAS DOS MITADES, y la primera no es relleno: sin ella, un `isRunning` clavado en
        // false —el flag propio que nunca se enciende— PASA este test. Medido: ese mutante
        // sobrevivía cuando el test sólo afirmaba la segunda mitad.
        val bridge = FakeTunerBridge()
        val tuner = tunerOver(bridge)
        tuner.start()

        assertEquals(
            true,
            tuner.isRunning,
            "control positivo: con el motor andando, isRunning dice true",
        )

        bridge.running = false

        assertEquals(
            false,
            tuner.isRunning,
            "y cuando el motor se para por debajo, lo refleja: isRunning sale del motor, no " +
                "de un flag propio que nadie actualiza",
        )
    }

    @Test
    fun startPropagatesTheEngineRefusal() {
        val bridge = FakeTunerBridge().apply { startSucceeds = false }

        assertEquals(
            false,
            tunerOver(bridge).start(),
            "sin motor o sin nodo de entrada, arrancar devuelve false y no se aplana a éxito",
        )
    }

    @Test
    fun stopDelegatesToTheEngine() {
        val bridge = FakeTunerBridge()
        val tuner = tunerOver(bridge)
        tuner.start()

        tuner.stop()

        assertEquals(false, bridge.running)
    }

    @Test
    fun aRefusedPushIsRetriedOnTheNextAssignment() {
        // El objetivo vigente se le PREGUNTA al motor. Si el motor rechazó el empuje —no hay
        // camino de análisis todavía—, la próxima asignación vuelve a intentarlo en vez de
        // creerse que ya está puesto. No hay integración que reiniciar cuando no hay motor.
        val bridge = FakeTunerBridge().apply { setTargetSucceeds = false }
        val tuner = tunerOver(bridge)

        tuner.selectedString = 2
        tuner.selectedString = 2

        assertEquals(
            2,
            bridge.pushedHz.size,
            "un empuje que el motor no aceptó no cuenta como objetivo puesto",
        )
    }

    @Test
    fun targetsFollowTheConfigurationInStringOrder() {
        val bridge = FakeTunerBridge()
        val tuner = tunerOver(bridge, TuningConfiguration(Tuning.UKULELE_HIGH_G))

        assertEquals(
            TuningConfiguration(Tuning.UKULELE_HIGH_G).targets(),
            tuner.targets,
            "los objetivos van en orden de CUERDA, no de frecuencia (AC-001.15)",
        )
    }
}
