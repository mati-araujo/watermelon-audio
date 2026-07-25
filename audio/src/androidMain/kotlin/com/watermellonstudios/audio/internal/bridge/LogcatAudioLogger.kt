package com.watermellonstudios.audio.internal.bridge

import android.util.Log
import com.watermellonstudios.audio.callback.AudioLogger

/**
 * `AudioLogger` que escribe a logcat (WA-1.4, avanza WA-1.1).
 *
 * Existe por una razón concreta: `BridgeConcurrency` vive en commonMain y por
 * defecto usa `NoOpAudioLogger`, porque una librería no decide por el consumidor a
 * dónde van sus logs. Pero `AudioNativeBridge` **hoy** loguea sus errores a logcat
 * con `Log.e`, y perder eso al centralizar el manejo de excepciones sería una
 * regresión de diagnosticabilidad silenciosa: los errores dejarían de aparecer donde
 * todo el mundo los busca.
 *
 * En androidMain `android.util.Log` está permitido por CLAUDE.md, así que este es el
 * lugar correcto para el acoplamiento.
 */
internal object LogcatAudioLogger : AudioLogger {

    override fun debug(tag: String, message: String, params: Map<String, Any>) {
        Log.d(tag, format(message, params))
    }

    override fun info(tag: String, message: String, params: Map<String, Any>) {
        Log.i(tag, format(message, params))
    }

    override fun warn(tag: String, message: String, params: Map<String, Any>) {
        Log.w(tag, format(message, params))
    }

    override fun error(
        tag: String,
        message: String,
        throwable: Throwable?,
        params: Map<String, Any>,
    ) {
        Log.e(tag, format(message, params), throwable)
    }

    private fun format(message: String, params: Map<String, Any>): String =
        if (params.isEmpty()) {
            message
        } else {
            "$message ${params.entries.joinToString(", ", "{", "}") { "${it.key}=${it.value}" }}"
        }
}
