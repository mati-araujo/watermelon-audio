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
 * - `aplica=false` (REQ-050 S3, D5/D19) marca una fila de la suite USB que el DEVICE no ofrece: la
 *   librería la devolvió `NOT_APPLICABLE` y no la midió. Va con `ok=false` y su `motivo`; el script
 *   la da como `NO-APLICA`, que no es PASS ni FAIL y no cuenta como cobertura. Reemplaza a la vieja
 *   marca `medido=false` (D11 de MINI-038: el runner ignoraba el rate de la fila), que ya no existe:
 *   el script la da como FAIL si reaparece.
 * - La corrida empieza con `panel=plan step=inicio` y termina con `panel=plan step=fin`. Sin `fin`
 *   el script no sabe si terminó, y lo dice.
 * - `step=precondicion` (REQ-053 S1) tampoco es un veredicto: es lo que la APP verificó de una
 *   precondición de la ficha de setup (`scripts/smoke-setup.json`), con formato fijo
 *   `id=<id> cumplida=<true|false> evidencia=<texto>` y `ok` igual a `cumplida`. Sólo la arma
 *   [precondition]; `format` la rechaza. El juez del script la cruza con la ficha y decide
 *   BLOQUEADO. Las del HOST las escribe el script con `verificador=host`, una clave que la app no
 *   puede escribir: así una línea de la app nunca pasa por la verificación del host.
 * - `step=escuchar` (REQ-053 S3) tampoco es un veredicto: es el AVISO de una ventana de escucha
 *   (`n`, `de`, `estimulo`, `ruta`, `en-ms`, `ventana-ms`), y es CIEGO — el de un estímulo y el de un
 *   control son la misma línea salvo `n`. Al cerrar la ventana la app emite `step=estimulo` o
 *   `step=control` con lo que el motor rindió (ver `ListeningWindows`).
 * - `step=sensor` (REQ-053 S3) es el juicio de un sensor sobre una ventana. Lo escribe el SCRIPT en
 *   su propio registro, nunca la app: `format` lo rechaza, y el juez descarta el que llegue por
 *   logcat (cualquier app puede escribir con el tag).
 *
 * En Android las líneas van a logcat con el tag [TAG]; en iOS, a la salida estándar.
 */
object HarnessSmoke {
    const val TAG: String = "HARNESS-SMOKE"
    const val VERSION: Int = 1

    /** El paso que no es un veredicto. Ver el KDoc del objeto. */
    const val STEP_WAITING_HUMAN: String = "esperando-humano"

    /** La clave de una fila que el device no ofrece (REQ-050 S3). Ver el KDoc del objeto. */
    const val FIELD_APPLICABLE: String = "aplica"

    /** La marca vieja de D11 (MINI-038). Reservada para que nadie la vuelva a escribir a mano. */
    private const val FIELD_MEASURED_RETIRED: String = "medido"

    /** REQ-053 S1: el paso que lleva una precondición verificada por la app. Ver el KDoc del objeto. */
    const val STEP_PRECONDITION: String = "precondicion"

    /** REQ-053 S3: el aviso (ciego) de una ventana de escucha. No es un veredicto. */
    const val STEP_LISTEN: String = "escuchar"

    /** REQ-053 S3: el juicio de un sensor. Lo escribe el script; la app no lo puede armar. */
    const val STEP_SENSOR: String = "sensor"

    /** La clave con la que el SCRIPT firma sus precondiciones de host. La app no la puede escribir. */
    private const val FIELD_VERIFIER: String = "verificador"

    private val KEY = Regex("[a-z0-9-]+")
    private val RESERVED = setOf("v", "run", "panel", "step", "ok", FIELD_APPLICABLE, FIELD_MEASURED_RETIRED, FIELD_VERIFIER)

    /**
     * La línea de una precondición que verificó la app:
     * `... step=precondicion ok=<met> id=<id> cumplida=<met> evidencia=<evidence>`.
     */
    fun precondition(run: String, panel: String, id: String, met: Boolean, evidence: String): String {
        require(KEY.matches(id)) { "id de precondicion invalido: '$id'" }
        return line(run, panel, STEP_PRECONDITION, met, listOf("id" to id, "cumplida" to met, "evidencia" to evidence), true)
    }

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
        applicable: Boolean = true,
    ): String {
        require(step != STEP_PRECONDITION) { "una precondicion se arma con precondition(), no con format()" }
        require(step != STEP_SENSOR) { "un juicio de sensor lo registra el script, no la app" }
        return line(run, panel, step, ok, fields, applicable)
    }

    private fun line(
        run: String,
        panel: String,
        step: String,
        ok: Boolean,
        fields: List<Pair<String, Any?>>,
        applicable: Boolean,
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
            // La marca la pone SÓLO este parámetro; como clave de `fields` está reservada.
            if (!applicable) append(' ').append(FIELD_APPLICABLE).append("=false")
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
     * Una fila que el device no ofrece (REQ-050 S3): `ok=false aplica=false motivo=<reason>`.
     * Devuelve `false`: quien la emite no puede contarla como pasada.
     */
    fun notApplicable(panel: String, step: String, reason: String, vararg fields: Pair<String, Any?>): Boolean {
        sink.emit(
            HarnessSmoke.format(
                run, panel, step, ok = false,
                fields = listOf("motivo" to reason) + fields.toList(),
                applicable = false,
            ),
        )
        return false
    }

    /**
     * `step=precondicion` (REQ-053 S1): lo que la app verificó de una precondición de la ficha.
     * Devuelve [met], por la misma razón que [report].
     */
    fun precondition(panel: String, id: String, met: Boolean, evidence: String): Boolean {
        sink.emit(HarnessSmoke.precondition(run, panel, id, met, evidence))
        return met
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
