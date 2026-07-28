package com.watermellonstudios.audio.callback

/**
 * En iOS todavía no hay dónde escribir desde Kotlin, así que el default es el no-op.
 *
 * **No es lo mismo que en Android, y conviene saberlo antes de buscar estos logs en la
 * consola de Xcode**: allá el `actual` es logcat porque ahí venían saliendo; acá no hay
 * un equivalente escrito. El motor de C++ **sí** loguea por su cuenta a `os_log` (ver
 * `platform/Logger.cpp`), y esas líneas se pueden leer con
 * `IAudioNativeBridge.drainCapturedLogs()` — pero eso es el log del motor, no el de estas
 * clases de Kotlin.
 *
 * Escribir un `AudioLogger` sobre `os_log` es chico y no tiene consumidor todavía; entra
 * cuando alguien necesite estas líneas en device. Queda anotado como hueco y no como
 * decisión: acá la ausencia **degrada** el diagnóstico, no es el comportamiento correcto.
 */
internal actual val platformDefaultAudioLogger: AudioLogger = NoOpAudioLogger
