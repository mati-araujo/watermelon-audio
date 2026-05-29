package com.watermellonstudios.audio.domain.chord

/**
 * Tipos de acorde disponibles.
 *
 * Los intervalos son semitonos cromáticos desde la raíz.
 * En modo scale-aware, se ajustan al grado más cercano de la escala activa.
 *
 * @property id ID numérico para persistencia
 * @property label Nombre completo
 * @property shortLabel Etiqueta corta para UI compacta
 * @property intervals Semitonos desde la raíz (excluyendo la raíz misma)
 */
enum class ChordType(
    val id: Int,
    val label: String,
    val shortLabel: String,
    val intervals: List<Int>
) {
    POWER(0, "Power", "PWR", listOf(7)),
    TRIAD(1, "Triad", "TRI", listOf(4, 7)),
    SEVENTH(2, "Seventh", "7TH", listOf(4, 7, 10)),
    SUS2(3, "Sus2", "SU2", listOf(2, 7)),
    SUS4(4, "Sus4", "SU4", listOf(5, 7)),
    OCTAVE(5, "Octave", "OCT", listOf(12)),
    FIFTH_OCTAVE(6, "5th+Oct", "5+O", listOf(7, 12)),
    ADD9(7, "Add9", "AD9", listOf(4, 7, 14));

    companion object {
        fun fromId(id: Int): ChordType {
            return entries.find { it.id == id } ?: TRIAD
        }
    }
}

/**
 * Configuración del modo acordes.
 *
 * @property enabled Si el modo acordes está activo. Cuando es false, los toques
 *   sólo disparan la nota fundamental.
 * @property chordType Tipo de acorde activo (POWER, TRIAD, SEVENTH, etc.).
 * @property realtimeSelectorEnabled Si el selector de acordes en tiempo real
 *   está habilitado. Cuando es true, la franja inferior del controller
 *   (Pad XY / Grid XY / Piano) se reserva como zona de selector para cambiar
 *   el [chordType] sin abrir la hoja de configuración. El default por pad type
 *   lo decide la capa de UI (ver `PadMode.defaultChordSelectorEnabled` en NoisyPad).
 *   No tiene efecto en modo Drum.
 */
data class ChordConfig(
    val enabled: Boolean = false,
    val chordType: ChordType = ChordType.TRIAD,
    val realtimeSelectorEnabled: Boolean = false
)
