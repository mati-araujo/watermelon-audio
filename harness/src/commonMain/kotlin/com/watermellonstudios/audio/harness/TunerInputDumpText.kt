package com.watermellonstudios.audio.harness

import com.watermellonstudios.audio.api.InternalWatermelonApi
import com.watermellonstudios.audio.api.TunerInputDumpResult

/**
 * Los textos del control de la grabación de entrada del afinador (REQ-055 S0 · 0.1), puros para poder
 * afirmarlos sin Compose, igual que [tunerStatusText]. Lo que el humano lee acá decide si re-graba:
 * el veredicto y el porqué tienen que ser inequívocos, y el comando para bajar el archivo tiene que
 * poder copiarse tal cual.
 */

/**
 * `afinador-entrada-AAAAMMDD-HHMMSS.wav` desde un instante ISO-8601 (`2026-10-14T15:04:05.123Z`, lo
 * que da `Clock.System.now().toString()`). Es hora UTC: Kotlin común no trae zona horaria sin una
 * dependencia, y para ordenar las tomas alcanza. Si el texto no tiene la forma esperada el nombre
 * lo dice (`sin-hora`) en vez de inventar una fecha.
 */
internal fun tunerInputDumpFileName(isoInstant: String): String {
    val shape = Regex("""^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})""").find(isoInstant)
    val stamp = shape?.destructured?.let { (y, mo, d, h, mi, s) -> "$y$mo$d-$h$mi$s" } ?: "sin-hora"
    return "afinador-entrada-$stamp.wav"
}

internal fun tunerInputDumpPath(dir: String, fileName: String): String = dir.trimEnd('/') + "/" + fileName

internal fun adbPullCommand(path: String): String = "adb pull $path"

/** El estado mientras NO se graba (o mientras se graba): qué falta para que la toma sirva. */
internal fun tunerInputDumpStatusText(
    recordingPath: String?,
    elapsedSeconds: Long,
    tunerRunning: Boolean,
): String = buildString {
    if (recordingPath != null) {
        append("grabando · ${elapsedSeconds}s\n$recordingPath.part")
    } else {
        append("sin grabar")
    }
    if (!tunerRunning) {
        // Sin afinador no hay ring enganchado a la captura: la toma saldría vacía e inválida.
        append("\nEL AFINADOR NO ESTÁ CORRIENDO: sin afinador no entra audio. Arrancalo arriba.")
    }
}

/** `start` devolvió false: el motivo más probable primero, sin afirmar uno solo. */
internal fun tunerInputDumpStartFailedText(path: String): String =
    "no se pudo empezar a grabar en $path (¿directorio inexistente o sin permiso, ya hay una toma, o no hay motor?)"

internal const val DUMP_STOP_WITHOUT_TAKE_TEXT = "no había toma en curso"

internal const val DUMP_UNAVAILABLE_TEXT = "este puente no ofrece el diagnóstico del afinador"

internal const val DUMP_NO_DIR_TEXT = "esta plataforma no dio un directorio donde grabar"

private fun sourceLabel(id: Int): String = when (id) {
    0 -> "micrófono"
    1 -> "línea"
    2 -> "USB"
    3 -> "Bluetooth"
    else -> "fuente $id"
}

/** Por qué una toma no sirve, en las palabras de los contadores. Vacío si la toma es válida. */
@OptIn(InternalWatermelonApi::class)
private fun invalidReasons(r: TunerInputDumpResult): List<String> = buildList {
    if (r.valid) return@buildList
    if (r.framesWritten == 0L) add("no entró audio (¿el afinador estaba corriendo?)")
    if (r.sampleRate <= 0) add("rate desconocido")
    if (r.framesLost > 0) add("se perdieron ${r.framesLost} frames")
    if (r.discontinuities > 0) add("hubo ${r.discontinuities} discontinuidades (costura, cambio de fuente o de rate)")
    if (isEmpty() && r.finalized) add("falló la escritura o el cierre del archivo")
}

/** El veredicto de la toma, con todo lo que el humano necesita para decidir y para bajar el archivo. */
@OptIn(InternalWatermelonApi::class)
internal fun tunerInputDumpResultText(r: TunerInputDumpResult): String = buildString {
    append(if (!r.finalized) "SIN CERRAR" else if (r.valid) "VÁLIDA" else "INVÁLIDA")
    append(" · ${r.seconds.toFloat().oneDecimal()} s · ${r.sampleRate} Hz · ${sourceLabel(r.inputSource)}")
    append("\nframes ${r.framesWritten} · perdidos ${r.framesLost} · discontinuidades ${r.discontinuities}")
    invalidReasons(r).forEach { append("\n  - $it") }
    if (!r.valid) append("\nesta toma NO sirve para medir")
    append("\n${r.path}")
    if (r.finalized) {
        append("\n${adbPullCommand(r.path)}")
    } else {
        // No hay archivo con nombre final: un `adb pull` apuntaría a algo que no existe.
        append("\nNO SE PUDO CERRAR NI RENOMBRAR el archivo: revisá ese .part en el teléfono (puede faltar o estar incompleto).")
    }
}

/** La línea para `adb logcat` / stdout: una sola, con clave=valor. */
@OptIn(InternalWatermelonApi::class)
internal fun tunerInputDumpLogLine(r: TunerInputDumpResult): String =
    "HARNESS-DUMP path=${r.path} valid=${r.valid} seconds=${r.seconds.toFloat().oneDecimal()} " +
        "rate=${r.sampleRate} source=${r.inputSource} frames=${r.framesWritten} " +
        "lost=${r.framesLost} discontinuities=${r.discontinuities} finalized=${r.finalized}"
