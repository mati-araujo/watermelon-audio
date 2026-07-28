package com.watermellonstudios.audio.callback

/**
 * El logger que usa el código interno de la librería cuando nadie le pasó uno.
 *
 * ## Por qué existe, y por qué NO es [NoOpAudioLogger] a secas
 *
 * La regla del repo es que una librería no decide por el consumidor a dónde van sus
 * logs, y por eso [AudioLogger] se inyecta y su default público es el no-op. Pero hay
 * clases internas que **hoy** loguean a logcat con `android.util.Log` directo, y bajarlas
 * a `commonMain` con un no-op por default apagaría esos logs sin que nadie lo note. Es
 * exactamente la regresión que documenta `LogcatAudioLogger`: los errores dejan de
 * aparecer donde todo el mundo los busca.
 *
 * Esto resuelve eso sin agregar un parámetro a ninguna API pública: el default lo pone
 * la plataforma, y quien construye a mano puede seguir inyectando el suyo.
 *
 * ## No confundir con el logger de [com.watermellonstudios.audio.api.config.AudioEngineConfig]
 *
 * Aquél es del **consumidor**: lo elige la app y su default es el no-op, porque la
 * librería no tiene por qué hablar salvo que se lo pidan. Éste es de la **librería
 * consigo misma**, para el puñado de clases internas que ya venían logueando. Fundirlos
 * sería tomar la decisión del consumidor por él.
 */
internal expect val platformDefaultAudioLogger: AudioLogger
