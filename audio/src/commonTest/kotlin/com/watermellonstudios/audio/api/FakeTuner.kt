package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.tuner.TunerSnapshot
import com.watermellonstudios.audio.domain.tuner.TunerState
import com.watermellonstudios.audio.domain.tuning.StringTarget
import com.watermellonstudios.audio.domain.tuning.TuningConfiguration

/**
 * Doble de [ITuner] **con comportamiento real** (REQ-001 S3 · 3.17).
 *
 * POR QUÉ NO ES UN DOBLE INERTE, QUE ES LA PARTE QUE IMPORTA
 * ----------------------------------------------------------
 * Un doble que devuelve listas vacías y `null` no es "sin comportamiento": es un **punto
 * ciego**, y ahí se acumulan los bugs. Esta librería ya shippeó dos stubs que devolvían
 * arrays de ceros, y los ceros derrotaron los fallbacks elvis de sus propios callers — el
 * bug pasó con los tests en verde.
 *
 * Así que este doble **usa el modelo de verdad**: los objetivos salen de
 * [TuningConfiguration], con su temperamento, su referencia y su capo. Lo único simulado es
 * la mitad que necesita audio — el snapshot del motor, que se guiona desde el test.
 *
 * Consecuencia práctica: un test que use este doble prueba de verdad el camino
 * configuración → objetivos → lectura, y sólo finge lo que no puede existir sin hardware.
 *
 * LO QUE ESTE DOBLE VIGILA, ADEMÁS DE RESPONDER
 * ---------------------------------------------
 * Registra en [pushedTargets] cada frecuencia objetivo que un consumidor "empujaría al
 * motor". Es la obligación que la interfaz declara en prosa y que las firmas no pueden
 * expresar: un implementador que nunca empuje el objetivo deja al estimador sin dónde
 * enganchar. Con esto, un test puede exigir que se haya empujado.
 *
 * 🔴 **Y las DOS mitades, que es lo que MINI-004 vino a arreglar.** Este doble empujaba en
 * cada asignación, sin guardia de "cambió", así que él mismo hacía lo que el puente prohíbe
 * —`setTunerTargetHz` *reinicia la integración*— mientras su KDoc se presentaba como vigía
 * de esa obligación. Vigilaba **una sola mitad**: que nadie deje de empujar. Al que empuja
 * de más no lo miraba nadie.
 *
 * Que en un doble sea inocuo es justamente lo que lo volvía peligroso: como **referencia**,
 * quien lo copiara escribía el implementador que nunca converge. Hoy el setter tiene el
 * mismo guardia que `TunerImpl`, así que las dos implementaciones que el contrato ejerce se
 * comportan igual — y el contrato puede exigir las dos mitades sin que el doble sea la
 * excepción. Ver [syncTargetWithEngine].
 */
class FakeTuner(
    configuration: TuningConfiguration,
) : ITuner {

    /** Frecuencias objetivo empujadas "al motor", en orden. Lo afirman los tests. */
    val pushedTargets = mutableListOf<Double>()

    /** Snapshots que devuelve [reading], en orden. El test los guiona. */
    var scriptedSnapshots: ArrayDeque<TunerSnapshot> = ArrayDeque()

    private var lastSnapshot: TunerSnapshot? = null

    override var configuration: TuningConfiguration = configuration
        set(value) {
            field = value
            targets = value.targets()
            // Cambiar la configuración mueve los objetivos, así que hay que re-empujar:
            // es exactamente la obligación que declara ITuner.
            syncTargetWithEngine()
        }

    override var targets: List<StringTarget> = configuration.targets()
        private set

    override var selectedString: Int? = null
        set(value) {
            field = value
            syncTargetWithEngine()
        }

    override var isRunning: Boolean = false
        private set

    override fun start(): Boolean {
        isRunning = true
        return true
    }

    override fun stop() {
        isRunning = false
    }

    override fun reading(): TunerReading? {
        if (!isRunning) return lastSnapshot?.let { TunerReading(currentTarget(), it) }
        val next = scriptedSnapshots.removeFirstOrNull()
        if (next != null) lastSnapshot = next
        val snapshot = lastSnapshot ?: return null
        return TunerReading(currentTarget(), snapshot)
    }

    private fun currentTarget(): StringTarget? =
        selectedString?.let { i -> targets.getOrNull(i - 1) }

    /**
     * Empuja el objetivo **sólo cuando el objetivo efectivo cambió** (MINI-004).
     *
     * 🔴 Antes esto empujaba en CADA asignación, y era una contradicción con el puente que
     * este mismo doble dice vigilar: `ITunerBridge.setTunerTargetHz` documenta que
     * *"cambiarlo reinicia la integración, así que no llamarlo por frame con el mismo
     * valor"*. En un doble empujar de más es inocuo —agregar a una lista no cuesta nada—,
     * pero como **referencia** era una trampa: quien lo copiara escribía el implementador
     * que nunca converge, y ese fallo se ve desde afuera como un DSP roto.
     *
     * El punto ciego era exacto y vale nombrarlo: [pushedTargets] permitía exigir que
     * alguien **empujara**, y nada miraba al que empujaba **de más**. O sea que vigilaba la
     * mitad barata de una obligación de dos mitades — que es el mismo modo de falla contra
     * el que previene el KDoc de apertura de esta clase, una capa más arriba.
     *
     * "Cambió" se mide sobre los **Hz efectivos**, no sobre la identidad de la
     * configuración: mover la referencia a los mismos 440 Hz no mueve nada. Y quedarse sin
     * objetivo empuja [NO_TARGET_HZ], que es una orden ("borrá el objetivo") y no una
     * ausencia de empuje.
     */
    private fun syncTargetWithEngine() {
        val desired = currentTarget()?.frequency?.hz ?: NO_TARGET_HZ
        if (desired == lastPushedHz) return
        lastPushedHz = desired
        pushedTargets += desired
    }

    /** El último objetivo empujado. Arranca en "sin objetivo", igual que el motor. */
    private var lastPushedHz: Double = NO_TARGET_HZ

    companion object {
        /** Lo que el puente entiende por "sin objetivo". Ver `ITunerBridge.setTunerTargetHz`. */
        const val NO_TARGET_HZ = 0.0

        /** Un snapshot sin medición de afinación — el estado REAL del motor hoy. */
        fun snapshotWithoutPitch(
            captureSampleRate: Int = 48000,
            levelRms: Float = 0.1f,
            state: TunerState = TunerState.NO_LOCK,
            detectedHz: Float? = null,
            detectionClarity: Float = 0f,
            inharmonicityB: Float? = null,
            lockedString: Int? = null,
            fastModeState: Int = 1,
            usableRangeCents: Float? = null,
        ) = TunerSnapshot(
            captureSampleRate = captureSampleRate,
            levelRms = levelRms,
            framesAnalyzed = 4096,
            droppedFrames = 0,
            state = state,
            cents = null,
            phaseAngle = null,
            uncertainty = null,
            detectedHz = detectedHz,
            detectionClarity = detectionClarity,
            inharmonicityB = inharmonicityB,
            lockedString = lockedString,
            fastModeState = fastModeState,
            usableRangeCents = usableRangeCents,
        )

        /** Un snapshot con medición, para los tests que necesitan un número. */
        fun snapshotWithPitch(
            cents: Float,
            state: TunerState = TunerState.CONVERGED,
            uncertainty: Float = 0.01f,
            detectedHz: Float? = 440f,
            detectionClarity: Float = 0.99f,
            inharmonicityB: Float? = null,
            lockedString: Int? = 0,
            fastModeState: Int = 2,
            usableRangeCents: Float? = 21.0f,
        ) = TunerSnapshot(
            captureSampleRate = 48000,
            levelRms = 0.2f,
            framesAnalyzed = 48000,
            droppedFrames = 0,
            state = state,
            cents = cents,
            phaseAngle = 0.3f,
            uncertainty = uncertainty,
            detectedHz = detectedHz,
            detectionClarity = detectionClarity,
            inharmonicityB = inharmonicityB,
            lockedString = lockedString,
            fastModeState = fastModeState,
            usableRangeCents = usableRangeCents,
        )
    }
}
