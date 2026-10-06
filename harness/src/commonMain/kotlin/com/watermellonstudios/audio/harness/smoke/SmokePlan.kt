package com.watermellonstudios.audio.harness.smoke

/**
 * MINI-038 — qué paneles corre una corrida automática, en qué orden.
 *
 * El plan llega como texto por el extra de intent `harness.smoke` (`am start ... --es harness.smoke
 * <plan>`): una lista separada por comas de [Panel.id], o `todo`. El orden de ejecución es SIEMPRE
 * el de [Panel], no el del texto: `usb` va último porque espera un gesto humano y no puede frenar a
 * los que no lo necesitan, y `salida` va primero porque los SoundFont necesitan el motor andando.
 */
enum class Panel(val id: String) {
    SALIDA("salida"),
    CAPTURA("captura"),
    SF2("sf2"),
    SF3("sf3"),
    USB("usb"),
}

/**
 * REQ-053 S3 (AC-053.10) — la semilla del orden de las ventanas, tal como llega por el extra
 * `harness.smoke.semilla`: un entero de 0 a `Int.MAX_VALUE` (el script la saca de 31 bits).
 *
 * Ausente no es un defecto (una corrida a mano): [seed] y [problem] quedan `null` y el plan elige
 * una y la registra con `semilla-origen=app`. Presente e inválida SÍ lo es: la corrida no arranca,
 * porque el orden ya no saldría de la semilla que el script registró.
 */
data class SeedRequest(val seed: Long?, val problem: String?) {
    companion object {
        fun parse(raw: String?): SeedRequest {
            if (raw == null) return SeedRequest(null, null)
            val seed = raw.toLongOrNull()?.takeIf { it in 0..Int.MAX_VALUE.toLong() }
                ?: return SeedRequest(null, "semilla-invalida:" + HarnessSmoke.value(raw))
            return SeedRequest(seed, null)
        }
    }
}

sealed class SmokePlan {
    data class Valid(val panels: List<Panel>) : SmokePlan()

    /** El texto no es un plan. Se reporta como `panel=plan step=inicio ok=false`, no se adivina. */
    data class Invalid(val reason: String) : SmokePlan()

    companion object {
        const val ALL = "todo"

        fun parse(raw: String?): SmokePlan {
            val text = raw?.trim().orEmpty()
            if (text.isEmpty()) return Invalid("plan-vacio")
            if (text == ALL) return Valid(Panel.entries.toList())
            val ids = text.split(',').map { it.trim() }
            val unknown = ids.filter { id -> Panel.entries.none { it.id == id } }
            if (unknown.isNotEmpty()) return Invalid("panel-desconocido:" + unknown.joinToString("+"))
            val wanted = ids.toSet()
            return Valid(Panel.entries.filter { it.id in wanted })
        }
    }
}
