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
            pushCurrentTarget()
        }

    override var targets: List<StringTarget> = configuration.targets()
        private set

    override var selectedString: Int? = null
        set(value) {
            field = value
            pushCurrentTarget()
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

    private fun pushCurrentTarget() {
        currentTarget()?.let { pushedTargets += it.frequency.hz }
    }

    companion object {
        /** Un snapshot sin medición de afinación — el estado REAL del motor hoy. */
        fun snapshotWithoutPitch(
            captureSampleRate: Int = 48000,
            levelRms: Float = 0.1f,
            state: TunerState = TunerState.NO_LOCK,
            detectedHz: Float? = null,
            detectionClarity: Float = 0f,
            inharmonicityB: Float? = null,
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
        )

        /** Un snapshot con medición, para los tests que necesitan un número. */
        fun snapshotWithPitch(
            cents: Float,
            state: TunerState = TunerState.CONVERGED,
            uncertainty: Float = 0.01f,
            detectedHz: Float? = 440f,
            detectionClarity: Float = 0.99f,
            inharmonicityB: Float? = null,
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
        )
    }
}
