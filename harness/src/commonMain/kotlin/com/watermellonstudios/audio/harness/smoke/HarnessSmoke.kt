package com.watermellonstudios.audio.harness.smoke

/**
 * MINI-038 — el formato de las líneas `HARNESS-SMOKE`. **Este archivo es el único lugar donde está
 * escrito**; `scripts/smoke-device.sh` lo lee y `HarnessSmokeTest` lo fija.
 *
 * ```
 * HARNESS-SMOKE v=1 run=<id> panel=<panel> step=<paso> ok=<true|false> [clave=valor ...]
 * ```
 *
 * - Los cinco primeros campos van **siempre, en ese orden**. El script los lee por nombre, pero el
 *   orden fijo hace que una línea se pueda leer a ojo en logcat.
 * - `run` separa una corrida de otra: el buffer de logcat conserva corridas viejas y el script no lo
 *   borra (es del device, no nuestro), así que filtra por el `run` que él mismo mandó.
 * - `ok` es el veredicto del PASO, medido: nunca un "OK" porque la llamada no tiró.
 * - Una clave es `[a-z0-9-]+`. Un valor no tiene espacios: los blancos se vuelven `_` y un valor
 *   vacío o nulo se escribe `-`, para que `clave=valor` se parta siempre por el primer `=` y la
 *   línea por espacios.
 * - `step=esperando-humano` es el único paso que NO es un veredicto: dice que la corrida está
 *   parada esperando un gesto humano (el diálogo de permiso USB) y lleva `accion=` con lo que hay
 *   que hacer. El script lo cuenta como `HUMANO`, no como `FAIL`.
 * - `medido=false` (D11) marca un paso que NO se pudo medir por un defecto conocido fuera del harness
 *   (hoy: las filas de la suite USB cuyo rate el runner no aplica). Va con `ok=false` y su `motivo`;
 *   el script lo da como `NO-MEDIDO`, que no es PASS ni FAIL y no cuenta como cobertura.
 * - La corrida empieza con `panel=plan step=inicio` y termina con `panel=plan step=fin`. Sin `fin`
 *   el script no sabe si terminó, y lo dice.
 *
 * En Android las líneas van a logcat con el tag [TAG]; en iOS, a la salida estándar.
 */
object HarnessSmoke {
    const val TAG: String = "HARNESS-SMOKE"
    const val VERSION: Int = 1

    /** El paso que no es un veredicto. Ver el KDoc del objeto. */
    const val STEP_WAITING_HUMAN: String = "esperando-humano"

    /** La clave de un paso no medido (D11). Ver el KDoc del objeto. */
    const val FIELD_MEASURED: String = "medido"

    private val KEY = Regex("[a-z0-9-]+")
    private val RESERVED = setOf("v", "run", "panel", "step", "ok", FIELD_MEASURED)

    /**
     * Arma una línea. Tira [IllegalArgumentException] si una clave no es `[a-z0-9-]+` o pisa un
     * campo reservado: es un error de programación del harness, no un dato del device, y una línea
     * ambigua que el script parte mal es peor que un crash en el desarrollo.
     */
    fun format(
        run: String,
        panel: String,
        step: String,
        ok: Boolean,
        fields: List<Pair<String, Any?>> = emptyList(),
        measured: Boolean = true,
    ): String {
        require(KEY.matches(panel)) { "panel invalido: '$panel'" }
        require(KEY.matches(step)) { "step invalido: '$step'" }
        return buildString {
            append(TAG)
            append(" v=").append(VERSION)
            append(" run=").append(value(run))
            append(" panel=").append(panel)
            append(" step=").append(step)
            append(" ok=").append(ok)
            // D11: la marca la pone SÓLO este parámetro; como clave de `fields` está reservada.
            if (!measured) append(' ').append(FIELD_MEASURED).append("=false")
            for ((k, v) in fields) {
                require(KEY.matches(k)) { "clave invalida: '$k'" }
                require(k !in RESERVED) { "clave reservada: '$k'" }
                append(' ').append(k).append('=').append(value(v))
            }
        }
    }

    /** Un valor sin blancos, nunca vacío. */
    fun value(v: Any?): String {
        val s = v?.toString() ?: return "-"
        if (s.isEmpty()) return "-"
        return buildString(s.length) {
            for (c in s) append(if (c.isWhitespace()) '_' else c)
        }
    }
}

/** A dónde van las líneas. Lo pone el shell de cada plataforma (logcat / stdout). */
fun interface SmokeSink {
    fun emit(line: String)
}

/**
 * Lo que usan los paneles y el plan para reportar. Devuelve el `ok` que emitió, para que quien
 * reporta y quien decide el paso siguiente lean el MISMO veredicto.
 */
class SmokeReporter(private val sink: SmokeSink, val run: String) {

    fun report(panel: String, step: String, ok: Boolean, vararg fields: Pair<String, Any?>): Boolean {
        sink.emit(HarnessSmoke.format(run, panel, step, ok, fields.toList()))
        return ok
    }

    /**
     * Un paso que NO se pudo medir (D11): `ok=false medido=false motivo=<reason>`. Devuelve `false`:
     * quien lo emite no puede contarlo como pasado.
     */
    fun notMeasured(panel: String, step: String, reason: String, vararg fields: Pair<String, Any?>): Boolean {
        sink.emit(
            HarnessSmoke.format(
                run, panel, step, ok = false,
                fields = listOf("motivo" to reason) + fields.toList(),
                measured = false,
            ),
        )
        return false
    }

    /** `step=esperando-humano`, con la acción exacta que hay que hacer. */
    fun waitingForHuman(panel: String, action: String, vararg fields: Pair<String, Any?>) {
        sink.emit(
            HarnessSmoke.format(
                run, panel, HarnessSmoke.STEP_WAITING_HUMAN, ok = false,
                fields = listOf("accion" to action) + fields.toList(),
            ),
        )
    }
}
