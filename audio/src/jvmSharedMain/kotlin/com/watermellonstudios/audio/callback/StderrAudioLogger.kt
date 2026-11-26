package com.watermellonstudios.audio.callback

import java.io.PrintStream

/**
 * `AudioLogger` mínimo, sin dependencias: escribe a stderr con la forma `L/TAG: mensaje`
 * (la misma que `drainCapturedLogs()` usa para el log del motor). Es el default de la
 * librería en desktop (REQ-055 S1) hasta que NoisyPad inyecte el suyo.
 *
 * Por qué stderr y no el no-op de iOS: en desktop la consola existe y es donde se va a
 * mirar mientras se arma la espiga; callar los warnings del bridge degrada el
 * diagnóstico sin ganar nada. Vive en jvmShared para que lo fije un test del arnés.
 *
 * `params` vacío no agrega nada, igual que `LogcatAudioLogger`.
 */
internal class StderrAudioLogger(
    private val out: () -> PrintStream = { System.err },
) : AudioLogger {

    override fun debug(tag: String, message: String, params: Map<String, Any>) =
        write('D', tag, message, params, null)

    override fun info(tag: String, message: String, params: Map<String, Any>) =
        write('I', tag, message, params, null)

    override fun warn(tag: String, message: String, params: Map<String, Any>) =
        write('W', tag, message, params, null)

    override fun error(tag: String, message: String, throwable: Throwable?, params: Map<String, Any>) =
        write('E', tag, message, params, throwable)

    private fun write(level: Char, tag: String, message: String, params: Map<String, Any>, t: Throwable?) {
        val stream = out()
        val suffix = if (params.isEmpty()) "" else
            " " + params.entries.joinToString(", ", "{", "}") { "${it.key}=${it.value}" }
        stream.println("$level/$tag: $message$suffix")
        t?.printStackTrace(stream)
    }
}
