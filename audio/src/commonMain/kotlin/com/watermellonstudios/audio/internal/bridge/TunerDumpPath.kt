package com.watermellonstudios.audio.internal.bridge

/**
 * Los bytes UTF-8 EXACTOS con los que el path de un volcado del afinador cruza a la C API
 * (`wma_tuner_diag_input_dump_start`), o `null` si no se lo puede representar sin cambiarle el
 * nombre (hallazgo 3 del security-auditor de REQ-055).
 *
 * El resultado del volcado (`TunerInputDumpResult.path`) es el path PEDIDO más la regla de nombres
 * del nativo: Kotlin no relee el disco. Para que ese path sea el archivo que de verdad quedó, los
 * bytes que llegan a C tienen que ser exactamente los del `String`. Dos cosas lo impiden:
 *
 * - **un `'\u0000'`**: C lo lee como fin de string y el archivo queda con el PREFIJO por nombre
 *   (`"a.wav\u0000.txt"` crearía `a.wav`);
 * - **un surrogate suelto**: no tiene UTF-8; la codificación por defecto lo reemplaza por U+FFFD y
 *   el archivo se llamaría distinto de lo pedido.
 *
 * Los dos puentes la usan ANTES de llamar al nativo: el JNI (que manda estos bytes como `jbyteArray`
 * y no un `jstring`, cuyo UTF-8 *modificado* parte en dos los caracteres fuera del BMP) y el de iOS
 * (cuya conversión `String` → `const char*` es UTF-8 con ese mismo reemplazo, y con esto no lo usa).
 * No lleva terminador: quien arme un `const char*` lo agrega.
 */
internal fun tunerDumpPathUtf8(path: String): ByteArray? {
    if (path.isEmpty() || path.indexOf('\u0000') >= 0) return null
    return try {
        path.encodeToByteArray(throwOnInvalidSequence = true)
    } catch (_: CharacterCodingException) {
        null
    }
}
