package com.watermellonstudios.audio.harness

import com.watermellonstudios.audio.api.TunerReading
import com.watermellonstudios.audio.harness.smoke.decimal1
import com.watermellonstudios.audio.harness.smoke.peakDbfs

/**
 * MINI-053 — una línea por lectura del panel del afinador, para medir en el Mac si el hilo de
 * análisis de producción pierde frames (10 min con cuerda real en el g42). Pura, sin Compose.
 *
 * `TUNER-READ t-ms=… analizados=… perdidos=… discontinuidades=… costura=0|1 rate=… estado-motor=…
 * nivel-dbfs=… hz=… cents=… cuerda=…`, key=value separados por espacio y sin locale ([decimal1]).
 * Una ausencia se escribe `-`, nunca un cero plausible: `rate=0` es "desconocido" para el motor y
 * se escribe `-`, como en `reportTunerTick`. Sin lectura (`null`): `TUNER-READ t-ms=… sin-lectura`,
 * con el `t-ms` igual, para que el hueco se vea en la serie.
 *
 * Una diferencia deliberada con `reportTunerTick`: un `levelRms` NaN sale `nivel-dbfs=-` (vía
 * `peakDbfs`) y no `-inf`, porque un NaN no es un silencio digital. Para el análisis: `t-ms` y
 * `analizados` vuelven a cero con cada Parar → Arrancar, y `analizados` viaja como float desde el
 * motor (exacto hasta 2^24 frames, ~5,8 min a 48 kHz; después cuantizado a 2-4 frames).
 *
 * @param tMs reloj monótono desde el arranque del loop de lectura.
 */
internal fun tunerReadLine(tMs: Long, reading: TunerReading?): String {
    val s = reading?.snapshot ?: return "$TUNER_READ_PREFIX t-ms=$tMs sin-lectura"
    return buildString {
        append(TUNER_READ_PREFIX)
        append(" t-ms=").append(tMs)
        append(" analizados=").append(s.framesAnalyzed)
        append(" perdidos=").append(s.droppedFrames)
        append(" discontinuidades=").append(s.discontinuityCount)
        append(" costura=").append(if (s.inputDiscontinuity) 1 else 0)
        append(" rate=").append(s.captureSampleRate.takeIf { it > 0 } ?: "-")
        append(" estado-motor=").append(s.state.name)
        append(" nivel-dbfs=").append(decimal1(peakDbfs(s.levelRms)))
        append(" hz=").append(decimal1(s.detectedHz))
        append(" cents=").append(decimal1(s.cents))
        append(" cuerda=").append(s.lockedString ?: "-")
    }
}

/** El prefijo de cada línea. */
internal const val TUNER_READ_PREFIX = "TUNER-READ"

/** El tag de logcat en Android, propio y estable: `adb logcat -s WMA-TUNER-READ`. */
const val TUNER_READ_TAG = "WMA-TUNER-READ"
