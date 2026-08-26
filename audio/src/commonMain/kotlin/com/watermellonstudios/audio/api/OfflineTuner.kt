package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.tuner.TunerSnapshot
import com.watermellonstudios.audio.internal.bridge.getAudioBridge

/**
 * Analizar una **grabación** con el afinador, sin micrófono (REQ-015 S2).
 *
 * ```kotlin
 * val snapshot = OfflineTuner.analyze(samples, sampleRate = 44_100, targetHz = 110f)
 * assertEquals(TunerState.CONVERGED, snapshot?.state)
 * ```
 *
 * POR QUÉ EXISTE, Y POR QUÉ NO ES UN MODO DE [ITuner]
 * ---------------------------------------------------
 * [ITuner] es el afinador que el músico está mirando: tiene micrófono, estado musical y un
 * ciclo de vida atado a una pantalla. Esto es lo contrario — una **función pura sobre un
 * buffer**, sin recurso que tomar ni que soltar. Meterlo adentro de [ITuner] obligaría a tener
 * un afinador vivo para analizar un archivo, que es justo lo que este puerto existe para
 * evitar: los dos defectos que motivaron REQ-015 se encontraron **a mano** —un teléfono, una
 * guitarra y una persona punteando— y no había forma de escribir un test que los detecte.
 * Un CI no tiene micrófono.
 *
 * Por eso tampoco hay factory ni configuración: no hay nada que configurar y no hay dos
 * instancias que puedan pelearse por el motor. Es un `object` con una función.
 *
 * LO QUE GARANTIZA, Y ES TODO LO QUE LO VUELVE ÚTIL
 * -------------------------------------------------
 *   - **Es el MISMO análisis del camino de tiempo real.** No hay una segunda implementación:
 *     una medición offline que no fuera la del producto daría un verde que no dice nada.
 *   - **Es determinista.** El mismo buffer da el mismo resultado siempre — no hay thread, no
 *     hay reloj y no hay nada que esperar. Sin eso no sirve para regresión, que es su única
 *     razón de existir.
 *   - **No perturba al afinador vivo.** Se puede llamar con [ITuner] corriendo: el análisis
 *     arma su propio estado por llamada y no toca el del motor (AC-015.4).
 *
 * @see ITuner
 */
object OfflineTuner {

    /** Lo que [analyze] entiende por "sin objetivo". Ver el parámetro `targetHz`. */
    const val NO_TARGET_HZ: Float = 0f

    /** Mono. Es el layout habitual del material grabado. */
    const val MONO: Int = 1

    /** Estéreo intercalado, `L R L R…` — el layout del que habla el camino de captura. */
    const val STEREO_INTERLEAVED: Int = 2

    /**
     * Corre el afinador sobre [samples] y devuelve la lectura.
     *
     * @param samples `frames · channels` floats. Se **lee** nada más: el array no se modifica.
     * @param sampleRate el rate **del material**, no el del dispositivo. No hay default a
     *   propósito: un buffer de 44,1 kHz analizado como si fuera de 48 da una lectura bien
     *   formada y equivocada por casi un semitono y medio, y ese es el modo de falla que esta
     *   librería prohíbe. Que no compile es preferible.
     * @param targetHz contra qué frecuencia medir. [NO_TARGET_HZ] deja `cents` en `null` y el
     *   estado en "sin enganche" — el motor **no adivina** la nota que quisiste tocar; lo que
     *   sí publica sin objetivo es [TunerSnapshot.detectedHz], que es de dónde sale el
     *   objetivo. No tiene default porque un afinador es contra algo, igual que
     *   [TunerFactory.create] no acepta una configuración por omisión.
     * @param channels [MONO] o [STEREO_INTERLEAVED]. Cualquier otro valor devuelve `null` en
     *   vez de interpretarse: mezclar un estéreo a mono de una forma distinta a la del motor
     *   rompería la coincidencia con el camino en vivo **en silencio**.
     *
     * @return la lectura, o `null` si los argumentos no describen audio analizable o si el
     *   análisis nunca llegó a publicar. **`null` no es "todo en cero"**: el motor deja el
     *   buffer intacto cuando falla, justamente para que nadie lea ceros como una medición.
     */
    // El motor es el implementador del puente, no un consumidor: las puertas públicas se
    // construyen encima de él. Ver [InternalWatermelonApi] y [TunerFactory].
    @OptIn(InternalWatermelonApi::class)
    fun analyze(
        samples: FloatArray,
        sampleRate: Int,
        targetHz: Float,
        channels: Int = MONO,
    ): TunerSnapshot? =
        getAudioBridge()
            .analyzeTunerBuffer(samples, channels, sampleRate, targetHz)
            ?.let(TunerSnapshot::fromNative)
}
