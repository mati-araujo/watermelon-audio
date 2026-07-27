package com.watermellonstudios.audio.api

/**
 * Superficie **de diagnóstico**, no de consumo. Compila sólo con opt-in explícito.
 *
 * ## Qué marca, y qué NO
 *
 * Marca el acceso directo al puente nativo — [com.watermellonstudios.audio.internal.bridge.getAudioBridge].
 * Eso es routing, BPM, backend, looper y captura de logs: cosas que un harness de
 * prueba necesita ver y que **ningún consumidor real pidió**.
 *
 * No marca la API pública. `AudioEngine`, `EffectManager`, `AudioInput` y las
 * factories siguen siendo lo que se consume, sin anotación y sin opt-in.
 *
 * ## Por qué existe (decisión de 2026-07-27)
 *
 * Cuatro de los siete controles del harness (WA-5.5) estaban bloqueados por lo
 * mismo: su superficie no llega a `commonMain`. Había dos salidas — ensanchar
 * `AudioEngine` con routing + BPM + looper + logs, o dar acceso al bridge detrás
 * de un opt-in. Se eligió la segunda.
 *
 * **El porqué importa más que la decisión.** El camino de entrada **sí** merecía
 * API pública —un cliente real va a querer capturar, y por eso `AudioInput` es una
 * interfaz de primera clase—. Routing, looper, BPM y logs son otra cosa: el harness
 * es tooling, no un consumidor. Ensanchar la API publicada para cuatro subsistemas
 * por conveniencia de una app de prueba es exactamente cómo una API termina llena
 * de cosas que después nadie puede sacar.
 *
 * **La regla que queda: algo entra a la API pública porque un consumidor real lo
 * necesita, no porque el harness lo necesite.** Si mañana NoisyPad pide el looper
 * desde `commonMain`, eso es un ticket con su propia justificación — y este opt-in
 * no lo estorba.
 *
 * ## Nivel ERROR, no WARNING
 *
 * Un warning se acumula y se ignora; para lo que esto separa —API que se sostiene
 * contra herramienta que puede cambiar sin aviso— la diferencia tiene que ser un
 * "no compila". Quien lo necesite lo dice en una línea:
 *
 * ```kotlin
 * @OptIn(InternalWatermelonApi::class)
 * fun diagnostico() { val bridge = getAudioBridge() }
 * ```
 *
 * Sin contrato de compatibilidad: lo que está detrás de esta anotación puede
 * cambiar o desaparecer en cualquier versión, incluida una de patch.
 */
@RequiresOptIn(
    level = RequiresOptIn.Level.ERROR,
    message = "Superficie interna de diagnóstico de Watermelon Audio: sin garantía de " +
        "compatibilidad y sujeta a cambios sin aviso. Si lo que necesitás es consumir el " +
        "motor, usá AudioEngine / EffectManager / AudioInput. Si de verdad necesitás el " +
        "puente nativo, marcá el uso con @OptIn(InternalWatermelonApi::class).",
)
@Retention(AnnotationRetention.BINARY)
@Target(
    AnnotationTarget.CLASS,
    AnnotationTarget.FUNCTION,
    AnnotationTarget.PROPERTY,
    AnnotationTarget.TYPEALIAS,
)
public annotation class InternalWatermelonApi
