package com.watermellonstudios.audio.callback

import com.watermellonstudios.audio.internal.bridge.LogcatAudioLogger

/**
 * Logcat, que es donde estos logs venían saliendo.
 *
 * El `actual` es lo que hace que bajar una clase de `androidMain` a `commonMain` no
 * apague sus logs de paso. Ver el KDoc del `expect`.
 */
internal actual val platformDefaultAudioLogger: AudioLogger = LogcatAudioLogger
