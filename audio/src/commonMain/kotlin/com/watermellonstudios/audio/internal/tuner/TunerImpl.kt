package com.watermellonstudios.audio.internal.tuner

import com.watermellonstudios.audio.api.ITuner
import com.watermellonstudios.audio.api.ITunerBridge
import com.watermellonstudios.audio.api.TunerReading
import com.watermellonstudios.audio.domain.tuner.TunerSnapshot
import com.watermellonstudios.audio.domain.tuning.StringTarget
import com.watermellonstudios.audio.domain.tuning.TuningConfiguration

/**
 * [ITuner] sobre el puente nativo (REQ-010 S1).
 *
 * Junta las dos mitades que `ITuner` declara separadas: el modelo musical —Kotlin puro, sin
 * audio— y lo que el motor midió. Igual que [com.watermellonstudios.audio.internal.input.AudioInputImpl],
 * es deliberadamente delgada; pero **acá delgada no quiere decir sin decisiones**, y la que
 * lleva adentro es la razón de que esta clase exista en la librería y no en cada app.
 *
 * LA OBLIGACIÓN QUE LAS FIRMAS NO EXPRESAN
 * ----------------------------------------
 * El estimador **afina alrededor de un objetivo, no lo busca**: alguien tiene que decirle
 * contra qué medir. `ITuner` lo declara en prosa como obligación del implementador, y tiene
 * dos formas de incumplirse que desde afuera se ven iguales —un DSP roto—:
 *
 *   - **no empujar nunca** ⇒ el motor reporta "sin enganche" para siempre;
 *   - **empujar de más** ⇒ [ITunerBridge.setTunerTargetHz] *"reinicia la integración"*, así
 *     que un empuje por asignación deja un afinador que **nunca converge**.
 *
 * Por eso el objetivo se empuja **sólo cuando el objetivo efectivo cambió**. "Efectivo" son
 * los Hz contra los que se mide, no la identidad del objeto de configuración: cambiar la
 * referencia a los mismos 440 Hz no mueve nada y no tiene por qué reiniciar la integración.
 *
 * EL OBJETIVO VIGENTE SE LE PREGUNTA AL MOTOR, NO SE CACHEA
 * ---------------------------------------------------------
 * El guardia compara contra [ITunerBridge.getTunerTargetHz], y no contra un último-empujado
 * propio. Son dos cosas distintas apenas hay más de una vista del afinador —y la factory de
 * S2 devuelve **una instancia nueva por llamada**, con el último empuje ganando—: un caché
 * local diría "no cambió" mientras el motor está midiendo contra la cuerda que empujó otra
 * vista, y esta se quedaría callada para siempre. Preguntar cuesta un `atomic load` y sólo
 * ocurre al asignar, nunca por frame.
 *
 * Es el mismo criterio que `isRunning`, que sale del motor en vez de un flag propio: un
 * booleano espejado se desincroniza —el nodo de entrada se puede caer sin que nadie llame a
 * [stop]— y miente.
 */
internal class TunerImpl(
    private val bridge: ITunerBridge,
    configuration: TuningConfiguration,
) : ITuner {

    override var configuration: TuningConfiguration = configuration
        set(value) {
            field = value
            // Los objetivos son estado DERIVADO del modelo musical, no un espejo del motor:
            // cachearlos es seguro y evita recalcular seis potencias por frame de UI.
            targets = value.targets()
            syncCandidatesWithEngine()
            syncTargetWithEngine()
        }

    override var targets: List<StringTarget> = configuration.targets()
        private set

    override var selectedString: Int? = null
        set(value) {
            field = value
            syncCandidatesWithEngine()
            syncTargetWithEngine()
        }

    override var automaticStringSelection: Boolean = false
        set(value) {
            field = value
            syncCandidatesWithEngine()
            syncTargetWithEngine()
        }

    override val isRunning: Boolean get() = bridge.isTunerRunning()

    /**
     * Arranca, y **re-declara los candidatos** si el automático está encendido.
     *
     * No es redundante con el setter: encender el automático antes de que haya camino de análisis
     * deja el pedido sin efecto, igual que pasa con el objetivo. Y es el único punto donde el
     * espejo de [declarados] se invalida a propósito — ver su comentario.
     */
    override fun start(): Boolean {
        val arranco = bridge.startTunerSync()
        if (arranco) {
            declarados = null
            syncCandidatesWithEngine()
        }
        return arranco
    }

    override fun stop() = bridge.stopTunerSync()

    /**
     * Un solo cruce de frontera por lectura, y **sin caché**.
     *
     * Los dos `null` del camino —el del motor que todavía no publicó nada y el de un array
     * corto— se propagan tal cual. Guardar la última lectura convertiría "no sé" en "sigue
     * igual": la aguja se queda clavada en un número que ya nadie está midiendo, que es el
     * no-op disfrazado de dato.
     *
     * Que la última lectura siga siendo legible después de [stop] no lo hace este envoltorio:
     * lo garantiza el motor, y por eso mismo no hay nada que replicar acá.
     */
    override fun reading(): TunerReading? {
        val snapshot = bridge.getTunerSnapshot()?.let(TunerSnapshot::fromNative) ?: return null
        return TunerReading(targetOf(snapshot), snapshot)
    }

    /**
     * El objetivo de la cuerda elegida, o `null`.
     *
     * `getOrNull` y no `coerceIn`: un índice fuera de rango deja el afinador **sin objetivo**.
     * Saturarlo afinaría la cuerda equivocada en silencio, que es peor —el usuario ve una
     * aguja plausible contra un objetivo que no eligió—. El índice es 1-based, como lo numera
     * el músico.
     */
    private fun currentTarget(): StringTarget? =
        selectedString?.let { targets.getOrNull(it - 1) }

    /**
     * Contra qué objetivo se publicó esta lectura.
     *
     * 🔴 **No es `currentTarget() ?: lockedTarget(...)`, y la diferencia la encontraron tres tests
     * que ya existían.** Ese `?:` confunde dos cosas distintas: *"no elegí cuerda"* —donde el motor
     * puede elegir— y *"elegí una cuerda que no existe"*, donde el afinador queda **sin objetivo**
     * a propósito (AC-001.15: saturar afinaría la cuerda equivocada en silencio). Con el `?:`, un
     * `selectedString` fuera de rango caía al enganche del motor y le devolvía al consumidor una
     * cuerda plausible que nunca eligió.
     *
     * La elección del consumidor manda incluso cuando es inválida: sólo su AUSENCIA abre la puerta.
     *
     * 🔴 **Y la ausencia sola no alcanza: hace falta que el automático esté ENCENDIDO.** Sin esa
     * condición, un consumidor que nunca pidió nada empezaría a recibir un objetivo derivado del
     * enganche del motor donde antes recibía `null` — un cambio de comportamiento que no pidió, que
     * es exactamente lo que Tunio pidió que NO hiciéramos. Lo atrapó
     * `TunerContractTest.convergidoExigeObjetivoMedicionYEstado`, que afirma que sin cuerda elegida
     * no hay convergencia: con el fallback incondicional, esa lectura pasaba a converger sola.
     */
    private fun targetOf(snapshot: TunerSnapshot): StringTarget? =
        when {
            selectedString != null -> currentTarget()
            automaticStringSelection -> lockedTarget(snapshot)
            else -> null
        }

    /**
     * El objetivo que el motor enganchó solo, o `null`.
     *
     * 🔴 **`lockedString` es 0-based y [selectedString] es 1-based**, así que acá NO va el `- 1`
     * que sí lleva [currentTarget]. El motor lo usa para indexar el arreglo de candidatos que
     * recibió (`FastModeTracker::lockTo` valida `0 <= index < count`), no para numerar cuerdas como
     * el músico. Un `- 1` de más devuelve la cuerda de al lado con cara de lectura válida.
     *
     * `getOrNull` y no `coerceIn`, por la misma razón que [currentTarget]: fuera de rango es
     * **sin objetivo**, no la cuerda más parecida.
     */
    private fun lockedTarget(snapshot: TunerSnapshot): StringTarget? =
        snapshot.lockedString?.let { targets.getOrNull(it) }

    /**
     * Le ofrece al motor las cuerdas del instrumento, o se las retira.
     *
     * 🔴 **La guarda de igualdad no es una optimización: sin ella el enganche se cae solo.**
     * `FastModeTracker::setCandidates` llama `release()` **incondicionalmente** —no compara con lo
     * que ya tenía—, así que re-declarar los MISMOS Hz suelta el enganche y tira la integración del
     * strobe. Un consumidor que reasigna `configuration` por cada frame de UI —lo normal en un
     * ViewModel— dejaría al modo rápido buscando para siempre. Es REQ-030 por la puerta de al lado,
     * y `targetAppliedByUser` no lo ve porque acá no se empuja ningún objetivo.
     *
     * Es un **espejo local**, que este archivo evita en otros lados por buenas razones
     * (`isRunning` sale del motor justamente para no mentir). Se acepta acá porque el puente **no
     * tiene getter de candidatos**, y el modo de falla se acota invalidándolo en [start] — que es
     * donde el motor puede haber quedado en otro estado.
     */
    private fun syncCandidatesWithEngine() {
        val ofrecer = automaticStringSelection && selectedString == null
        val deseados =
            if (ofrecer) FloatArray(targets.size) { targets[it].frequency.hz.toFloat() }
            else FloatArray(0)
        if (declarados != null && declarados.contentEquals(deseados)) return
        if (bridge.setTunerCandidates(deseados)) declarados = deseados
    }

    /**
     * Lo último que se le declaró al motor, para no re-declararlo. `null` = no se sabe.
     *
     * Sólo se actualiza cuando el puente **aceptó** el pedido: si lo rechazó —todavía no hay camino
     * de análisis— el próximo intento vuelve a empujar, igual que hace [syncTargetWithEngine] con
     * el objetivo.
     */
    private var declarados: FloatArray? = null

    private fun syncTargetWithEngine() {
        // En automático y sin cuerda elegida **el objetivo lo manda el motor**: empujarle el
        // "sin objetivo" que sale de `currentTarget() == null` le borraría el enganche que el modo
        // rápido acaba de hacer, y el afinador se apagaría solo justo cuando empezó a funcionar.
        if (automaticStringSelection && selectedString == null) return
        val desired = currentTarget()?.frequency?.hz?.toFloat() ?: NO_TARGET_HZ
        // Igualdad exacta y no una tolerancia: el motor guarda el mismo float que se le
        // mandó, así que una diferencia de bits ES un objetivo distinto. Y si el motor
        // rechazó el empuje —todavía no hay camino de análisis—, esto vuelve a intentarlo en
        // la próxima asignación en vez de creerse que ya está puesto.
        if (bridge.getTunerTargetHz() == desired) return
        bridge.setTunerTargetHz(desired)
    }

    private companion object {
        /** Lo que el puente entiende por "sin objetivo". Ver `ITunerBridge.setTunerTargetHz`. */
        const val NO_TARGET_HZ = 0f
    }
}
