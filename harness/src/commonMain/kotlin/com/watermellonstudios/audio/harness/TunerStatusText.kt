package com.watermellonstudios.audio.harness

import com.watermellonstudios.audio.api.TunerReading
import com.watermellonstudios.audio.harness.smoke.TunerReadout

/**
 * El texto de estado del panel del afinador (MINI-045), puro para poder afirmarlo sin Compose.
 *
 * Los tres estados son los de [TunerReadout]; esto sólo decide la frase. En `MEASURING` dice los Hz
 * que mide el motor (`detectedHz`, sin objetivo) y, si no hay cuerda elegida, que por eso nunca
 * engancha: sin objetivo el motor no da cents (KDoc de `ITuner`). El panel informa, no elige cuerda.
 */
internal fun tunerStatusText(reading: TunerReading?, running: Boolean, hasSelectedString: Boolean): String =
    when (TunerReadout.of(reading)) {
        TunerReadout.NO_READING -> if (running) "sin lectura (el motor no publicó nada)" else "detenido"
        TunerReadout.MEASURING -> buildString {
            append("midiendo")
            reading?.snapshot?.detectedHz?.let { append(" · ${it.oneDecimal()} Hz") }
            append(" · sin enganche")
            if (!hasSelectedString) append(" · sin cuerda elegida: tocá una cuerda abajo")
        }
        TunerReadout.LOCKED -> reading?.let { r ->
            "${r.cents?.oneDecimal()} cents" +
                (r.target?.let { " · objetivo ${it.note.name}" } ?: " · sin objetivo") +
                (if (r.isConverged) " · convergido" else "")
        }.orEmpty()
    }
