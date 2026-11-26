package com.watermellonstudios.audio.api

/**
 * Grabar lo que el afinador **ve**, para poder medirlo (REQ-055 S0). Diagnóstico, **sin contrato**.
 *
 * ## Para qué sirve
 *
 * Para afinar el afinador hace falta material real: la guitarra, el teléfono y el micrófono con los
 * que se usa. Este volcado graba a un WAV exactamente la señal que entra al análisis —mono
 * `0,5·(L+R)`, después del DSP de entrada, al rate real del stream—, no la de una grabadora aparte,
 * que mediría otra cosa. Es el instrumento con el que se arma el corpus de medición; no es una
 * función del afinador ni algo que un consumidor deba ofrecerle a un músico.
 *
 * ## Qué garantiza
 *
 * **Un WAV con huecos nunca tiene nombre de sano.** Si se perdió audio (el análisis se atrasó, el
 * disco no dio abasto), si la captura tuvo una discontinuidad, si cambió la fuente o el rate a mitad
 * de la toma, o si no entró un solo frame, la toma cierra *inválida* y el archivo queda con el nombre
 * de [TunerInputDumpResult.invalidPathFor]. Los contadores dicen por qué.
 *
 * **No arranca el afinador.** Sin afinador corriendo no entra audio: la toma cierra inválida y vacía.
 * Se arranca el afinador primero y se graba después.
 *
 * ## Por qué es una interfaz aparte
 *
 * Agregar estos métodos a [ITunerBridge] ensancharía una interfaz que otros implementan, y la
 * regla del repo es que algo entra a la superficie porque un consumidor lo necesita, no porque lo
 * necesite el tooling. Se alcanza con `getAudioBridge() as? ITunerDiagnostics`, como el resto de
 * lo que el harness ve ([InternalWatermelonApi]).
 */
@InternalWatermelonApi
interface ITunerDiagnostics {

    /**
     * Empieza a grabar a [path], que tiene que terminar en `.wav`. El archivo se escribe a
     * `<path>.part` y se nombra al detener.
     *
     * **Nunca pisa nada que ya esté en el disco**: si `<path>`, su `.invalid.wav` o `<path>.part` ya
     * existen (un symlink incluido), no arranca; si el nombre final aparece durante la toma, al
     * detener no se lo reemplaza y la toma queda sin finalizar ([TunerInputDumpResult.finalized]).
     *
     * @return false si no hay motor, si [path] no termina en `.wav` o pasa de 4096 bytes en UTF-8, si
     *   tiene un `'\u0000'` o un surrogate suelto (no se lo puede nombrar en disco tal cual), si ya
     *   hay una toma en curso, si el archivo no se puede crear (directorio inexistente, sin permiso,
     *   nombre ocupado) o si el análisis no aplicó el pedido a tiempo.
     *
     *   **Un false no deja nada de esta toma en disco** (ni `.part` ni archivo nombrado), y nunca
     *   toca una toma que ya estaba en curso. Dos excepciones:
     *   - si el análisis no aplicó ni el armado ni el desarme, la toma queda viva (su `.part` se
     *     puede seguir escribiendo y no se borra). Un false no prueba que no la haya, así que quien
     *     lo recibe llama a [stopTunerInputDump], cuyo [TunerInputDumpResult.path] dice qué archivo
     *     dejó;
     *   - si el `.part` de la toma abortada no se pudo borrar (permisos, o el nombre lo ocupa un
     *     archivo que la toma no creó y que nunca se borra), queda `<path>.part` sin toma viva y el
     *     nativo lo deja logueado con su nombre. Mientras exista, ese path no vuelve a arrancar.
     */
    fun startTunerInputDump(path: String): Boolean

    /**
     * Detiene la toma, cierra el archivo y devuelve su veredicto.
     *
     * @return null si no se cerró ninguna toma: no había una, o el análisis no respondió a tiempo y
     *   la toma sigue viva (reintentar el stop la cierra) — **no** un resultado de ceros, que se
     *   leería como una toma vacía. Única excepción: sin memoria al cerrar, la toma puede haber
     *   quedado cerrada igual (el null no lo distingue).
     */
    fun stopTunerInputDump(): TunerInputDumpResult?
}

/**
 * El veredicto de una toma de [ITunerDiagnostics]: qué se grabó y si se puede usar.
 *
 * [valid] es `false` ante cualquier pérdida o discontinuidad que el archivo **no puede ubicar**
 * (tope de la toma, error del disco, reset del ring, cambio de fuente, reinicio del afinador con la
 * toma viva, cambio de rate, más de 256 cortes): una toma así sirve para diagnosticar y no para
 * medir. Una pérdida o una costura de captura UBICADA no la invalida: el chunk `wmad` del WAV (v2)
 * declara cada corte (frame del archivo y largo) y quien mide corta ahí. Por eso [valid] con
 * [framesLost] o [discontinuities] mayores que 0 es una toma sana **con cortes**. [path] es el
 * archivo FINAL: el pedido si es válida, el de [invalidPathFor] si no.
 *
 * [path] lo informa el NATIVO (el sink sabe qué dejó), no se reconstruye con el path pedido.
 *
 * [finalized] es `false` si el archivo no se pudo cerrar o renombrar —incluido un nombre final que
 * otro archivo ocupó durante la toma, que nunca se reemplaza—: entonces [valid] es `false`,
 * [path] es `<pedido>.part` (donde quedó lo escrito, si algo se escribió) y la toma no escribió
 * nada con el nombre final. Ese nombre PUEDE existir: si otro archivo lo ocupó durante la toma, es
 * ese archivo ajeno, intacto, y no es la toma. Quien lo muestre tiene que decir que se revise el
 * `.part`, no ofrecer (ni borrar) un `x.wav`/`x.invalid.wav` que no es suyo.
 */
@InternalWatermelonApi
data class TunerInputDumpResult(
    val framesWritten: Long,
    val framesLost: Long,
    val discontinuities: Long,
    val sampleRate: Int,
    val inputSource: Int,
    val valid: Boolean,
    val finalized: Boolean,
    val path: String,
) {
    /** Duración grabada en segundos; 0 si el rate es 0 (nunca entró audio). */
    val seconds: Double
        get() = if (sampleRate > 0) framesWritten.toDouble() / sampleRate else 0.0

    companion object {
        /** Cuántos valores cruzan la frontera nativa. Es `WMA_TUNER_DUMP_STATS_VALUES`. */
        const val VALUE_COUNT = 7

        /**
         * Arma el resultado con los [VALUE_COUNT] valores nativos, en el orden de la C API:
         * framesWritten, framesLost, discontinuities, sampleRate, inputSource, valid (0/1), finalized (0/1),
         * y el archivo que la toma dejó en disco, **tal como lo informa el nativo**
         * (`wma_tuner_diag_input_dump_stop`).
         *
         * El path no se reconstruye con el pedido (bug A de REQ-055 S0): tras un start que devolvió
         * false con la toma viva, el último path pedido no es necesariamente el de la toma que
         * corrió. El que sabe qué archivo quedó es el sink.
         *
         * @return null si [values] es null o no tiene exactamente [VALUE_COUNT] valores, o si
         *   [nativePath] es null o vacío (una toma cerrada siempre deja un archivo).
         */
        fun fromNative(values: LongArray?, nativePath: String?): TunerInputDumpResult? {
            if (values == null || values.size != VALUE_COUNT) return null
            if (nativePath.isNullOrEmpty()) return null
            val finalized = values[6] != 0L
            // Sin archivo nombrado no hay toma sana, diga lo que diga el índice 5.
            val valid = finalized && values[5] != 0L
            return TunerInputDumpResult(
                framesWritten = values[0],
                framesLost = values[1],
                discontinuities = values[2],
                sampleRate = values[3].toInt(),
                inputSource = values[4].toInt(),
                valid = valid,
                finalized = finalized,
                path = nativePath,
            )
        }

        /**
         * El nombre que el nativo le da a una toma inválida: `x.wav` -> `x.invalid.wav`, y si no
         * termina en `.wav`, `path + ".invalid.wav"`. La misma regla que `invalidDumpPathFor` de C++.
         * Sirve para PREDECIR el nombre (mostrarlo, buscarlo); el [path] de un resultado no sale de
         * acá sino del nativo.
         */
        fun invalidPathFor(path: String): String =
            if (path.endsWith(WAV_EXTENSION)) {
                path.removeSuffix(WAV_EXTENSION) + ".invalid.wav"
            } else {
                "$path.invalid.wav"
            }

        private const val WAV_EXTENSION = ".wav"
    }
}
