package com.watermellonstudios.audio.domain.input

/**
 * Where the input node takes audio from.
 *
 * Los ids son los de `InputSource` en `nodes/InputNode.h` y cruzan la frontera
 * como `int` — no reordenar.
 *
 * **No todas las fuentes existen en todas las plataformas.** `USB_DAC` sólo
 * tiene sentido en Android (D4: el backend USB no porta a iOS), y en iOS la
 * elección real de ruta la hace `AVAudioSession`, no este enum. Se expone
 * completo igual porque el motor las acepta a las cuatro; qué hace cada una es
 * una pregunta de plataforma, no de la API.
 *
 * @property id Native ID usado por el motor C++
 * @property displayName Nombre legible para UI
 */
enum class InputSource(val id: Int, val displayName: String) {
    MIC(0, "Microphone"),
    LINE_IN(1, "Line in"),
    USB_DAC(2, "USB"),
    BLUETOOTH(3, "Bluetooth");

    companion object {
        fun fromId(id: Int): InputSource = entries.find { it.id == id } ?: MIC
    }
}
