package com.watermellonstudios.audio.export

/** Desktop: todavía sin AAC; falla explícito (ver [UnsupportedAacEncoder]). */
internal actual val platformAacEncoder: IAacEncoder = UnsupportedAacEncoder
