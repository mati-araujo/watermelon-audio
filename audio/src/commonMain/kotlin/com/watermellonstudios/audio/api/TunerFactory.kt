package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.domain.tuning.TuningConfiguration
import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import com.watermellonstudios.audio.internal.tuner.TunerImpl

/**
 * Factory de [ITuner] — la única puerta pública al afinador (REQ-010 S2).
 *
 * Mismo patrón que [AudioInputFactory] y [EffectManagerFactory]: el consumidor obtiene el
 * subsistema sin nombrar el bridge, y por lo tanto **sin optar-in a
 * [InternalWatermelonApi]**. Ésa es la razón de existir de este archivo, no un detalle de
 * estilo: hasta acá el único camino al afinador pasaba por `getAudioBridge()`, una superficie
 * que este repo declara sin contrato y que *"puede cambiar o desaparecer en cualquier versión,
 * incluida una de patch"*.
 *
 * ```kotlin
 * val tuner = TunerFactory.create(TuningConfiguration(Tuning.GUITAR_STANDARD))
 * tuner.selectedString = 6      // mi grave, numerada desde 1
 * tuner.start()
 * ```
 *
 * POR QUÉ LA CONFIGURACIÓN ES OBLIGATORIA Y NO TIENE DEFAULT
 * ----------------------------------------------------------
 * [AudioInputFactory.create] no toma argumentos porque envuelve un recurso único del
 * dispositivo. El afinador envuelve ese recurso **más un modelo musical**, y ahí no hay
 * default honesto: un afinador es contra algo. Poner "guitarra estándar" como default metería
 * una opinión de producto adentro de la librería y —peor— le daría al consumidor que se olvidó
 * de configurarlo un afinador que **anda**, midiendo contra las cuerdas equivocadas. Un fallo
 * que se ve como un dato plausible es el modo de falla que este proyecto prohíbe: es preferible
 * que no compile.
 *
 * 🔴 QUÉ DEVUELVE: UNA INSTANCIA NUEVA POR LLAMADA, SOBRE UN MOTOR QUE ES UNO SOLO
 * --------------------------------------------------------------------------------
 * Esto es lo que un consumidor **no puede adivinar**, y es distinto de lo que documenta
 * [AudioInputFactory] — que dice que no crea nada y que dos llamadas dan dos vistas del mismo
 * camino. Acá las dos mitades se comportan distinto:
 *
 *   - **El estado musical es por instancia.** Cada [ITuner] tiene su [ITuner.configuration] y
 *     su [ITuner.selectedString]. Cambiar uno no toca al otro.
 *   - **El motor de abajo es único.** La frecuencia objetivo contra la que se mide es una sola,
 *     global, y cada instancia la empuja cuando su objetivo efectivo cambia. Así que con dos
 *     instancias vivas **gana el último que asignó**, y la otra sigue creyendo que mide contra
 *     su cuerda.
 *
 * De ahí la regla de uso: **una vista viva por vez**. Dos afinadores simultáneos no es un uso
 * soportado — no rompe ni corrompe nada, pero uno de los dos muestra una aguja que no
 * corresponde a su cuerda seleccionada, y desde afuera eso se ve como un DSP roto.
 *
 * Si hace falta compartir el afinador entre varias pantallas, se comparte **la instancia**, no
 * se llama a [create] dos veces.
 *
 * @see ITuner
 * @see AudioInputFactory
 */
object TunerFactory {

    /**
     * Devuelve un afinador configurado, listo para [ITuner.start].
     *
     * No arranca el análisis y no toma el micrófono: eso lo hace [ITuner.start], para que el
     * consumidor decida cuándo — típicamente atado al ciclo de vida de su pantalla, porque el
     * recurso hay que soltarlo con [ITuner.stop].
     *
     * @param configuration qué debería sonar: instrumento, afinación, temperamento, referencia
     *   y capo. Se puede cambiar después por [ITuner.configuration].
     */
    // El motor es el implementador del puente, no un consumidor: las factories
    // publicas se construyen encima de el. Ver [InternalWatermelonApi].
    @OptIn(InternalWatermelonApi::class)
    fun create(configuration: TuningConfiguration): ITuner =
        TunerImpl(getAudioBridge(), configuration)
}
