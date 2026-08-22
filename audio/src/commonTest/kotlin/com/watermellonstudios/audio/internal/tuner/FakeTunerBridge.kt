package com.watermellonstudios.audio.internal.tuner

import com.watermellonstudios.audio.api.ITunerBridge

/**
 * El puente de mentira, con el mínimo estado para observar qué se le pidió.
 *
 * [setTunerTargetHz] copia la normalización del motor —`hz > 0 ? hz : 0`, literal de
 * `AnalysisThread::setTargetHz`— porque el impl **le pregunta al motor** cuál es el
 * objetivo vigente en vez de espejarlo. Un doble que guardara el valor crudo haría pasar
 * un impl que en producción empuja de más.
 */
internal class FakeTunerBridge : ITunerBridge {
    /** Cada Hz empujado, en orden. `0f` es "borrar el objetivo", no ausencia de empuje. */
    val pushedHz = mutableListOf<Float>()

    var running = false
    var startSucceeds = true
    var setTargetSucceeds = true

    /** Lo que devuelve [getTunerSnapshot]. `null` = el motor todavía no publicó nada. */
    var snapshot: FloatArray? = null
    var snapshotCalls = 0

    private var targetHz = 0f

    override fun startTunerSync(): Boolean {
        running = startSucceeds
        return startSucceeds
    }

    override fun stopTunerSync() { running = false }
    override fun isTunerRunning(): Boolean = running

    override fun setTunerTargetHz(hz: Float): Boolean {
        pushedHz += hz
        if (!setTargetSucceeds) return false
        targetHz = if (hz > 0f) hz else 0f
        return true
    }

    override fun getTunerTargetHz(): Float = targetHz

    override fun getTunerSnapshot(): FloatArray? {
        snapshotCalls++
        return snapshot
    }

    // Los seis de abajo están FUERA del alcance de REQ-010: `TunerImpl` no los toca.
    //
    // 🔴 Explotan en vez de devolver un valor inerte, y es por la misma razón que este
    // archivo ya invoca dos veces: un doble que devuelve ceros no es "sin comportamiento",
    // es un PUNTO CIEGO. `intonationState(): Int = 0` es exactamente el stub que esta
    // librería ya shippeó dos veces. Hoy no se nota porque nadie los llama; el día que
    // alguien cablee intonación o el modo rápido, un cero fabricado dejaría el test en
    // verde sobre una medición que nadie hizo. Así, ese día el test grita.
    override fun captureIntonation(slot: Int): Boolean = fueraDeAlcance("captureIntonation")
    override fun resetIntonation(): Unit = fueraDeAlcance("resetIntonation")
    override fun intonationState(): Int = fueraDeAlcance("intonationState")
    override fun intonationDifferenceCents(): Float = fueraDeAlcance("intonationDifferenceCents")
    override fun setTunerCandidates(hz: FloatArray): Boolean = fueraDeAlcance("setTunerCandidates")
    override fun lockTunerString(index: Int): Boolean = fueraDeAlcance("lockTunerString")

    private fun fueraDeAlcance(nombre: String): Nothing =
        error(
            "$nombre está fuera del alcance de REQ-010 y este doble NO lo modela. " +
                "Si TunerImpl empezó a llamarlo, el alcance cambió: modelalo de verdad " +
                "acá en vez de devolver un valor inventado.",
        )
}
