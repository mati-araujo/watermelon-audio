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
     * Empieza a grabar a [path] (un `.wav`). El archivo se escribe a `<path>.part` y se nombra al
     * detener.
     *
     * @return false si no hay motor, si ya hay una toma en curso, si el archivo no se puede crear
     *   (directorio inexistente, sin permiso) o si el análisis no aplicó el pedido a tiempo.
     */
    fun startTunerInputDump(path: String): Boolean

    /**
     * Detiene la toma, cierra el archivo y devuelve su veredicto.
     *
     * @return null si no había toma en curso — **no** un resultado de ceros, que se leería como una
     *   toma vacía.
     */
    fun stopTunerInputDump(): TunerInputDumpResult?
}

/**
 * El veredicto de una toma de [ITunerDiagnostics]: qué se grabó y si se puede usar.
 *
 * [valid] es `false` ante **cualquier** pérdida o discontinuidad, aunque sea de un frame: una toma
 * así sirve para diagnosticar y no para medir. [path] es el archivo FINAL: el pedido si es válida,
 * el de [invalidPathFor] si no.
 *
 * [finalized] es `false` si el archivo no se pudo cerrar o renombrar: entonces [valid] es `false`,
 * [path] es `<pedido>.part` (donde quedó lo escrito, si algo se escribió) y no existe ningún archivo
 * con el nombre final. Quien lo muestre tiene que decir que se revise ese `.part`, no ofrecer un
 * `x.invalid.wav` que no está.
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
         * framesWritten, framesLost, discontinuities, sampleRate, inputSource, valid (0/1), finalized (0/1).
         *
         * @return null si [values] es null o no tiene exactamente [VALUE_COUNT] valores.
         */
        fun fromNative(values: LongArray?, requestedPath: String): TunerInputDumpResult? {
            if (values == null || values.size != VALUE_COUNT) return null
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
                path = when {
                    !finalized -> "$requestedPath.part"
                    valid -> requestedPath
                    else -> invalidPathFor(requestedPath)
                },
            )
        }

        /**
         * El nombre que el nativo le da a una toma inválida: `x.wav` -> `x.invalid.wav`, y si no
         * termina en `.wav`, `path + ".invalid.wav"`. La misma regla que `invalidDumpPathFor` de C++:
         * Kotlin predice el nombre sin leer el disco.
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
