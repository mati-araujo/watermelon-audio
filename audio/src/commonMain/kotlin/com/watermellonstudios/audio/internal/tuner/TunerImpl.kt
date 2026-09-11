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
            syncTargetWithEngine()
        }

    override var targets: List<StringTarget> = configuration.targets()
        private set

    override var selectedString: Int? = null
        set(value) {
            field = value
            syncTargetWithEngine()
        }

    override val isRunning: Boolean get() = bridge.isTunerRunning()

    override fun start(): Boolean = bridge.startTunerSync()

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
        return TunerReading(currentTarget(), snapshot)
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

    private fun syncTargetWithEngine() {
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
