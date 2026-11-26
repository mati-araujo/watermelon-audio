package com.watermellonstudios.audio.export

/**
 * WAV → AAC LC (`.m4a`): lo único que [com.watermellonstudios.audio.internal.bridge.AudioNativeBridge]
 * necesita del transcoder (REQ-055 S1, ADR-0009 de NoisyPad).
 *
 * Existe para que el bridge no dependa de MediaCodec: en Android la implementación es
 * [MediaCodecAacEncoder]; en la JVM no hay encoder todavía (lo trae una etapa posterior).
 * Mínima a propósito: un método, con la firma que el bridge ya usaba.
 */
internal interface IAacEncoder {
    /**
     * @param onProgress en [0..1]; la implementación decide desde qué thread lo llama.
     * @return el resultado del transcode. Un fallo esperable (WAV inválido, encoder que no
     *         arranca) es `success = false` con su `error`, no una excepción.
     */
    suspend fun wavToM4a(
        wavPath: String,
        m4aPath: String,
        bitrateBps: Int,
        onProgress: ((Float) -> Unit)?,
    ): Mp4AacTranscoder.Result
}

/**
 * El encoder AAC de la plataforma: MediaCodec en Android, [UnsupportedAacEncoder] en
 * desktop. Sin setter público: la inyección del encoder de desktop la trae su etapa.
 */
internal expect val platformAacEncoder: IAacEncoder

/**
 * El encoder de una plataforma que todavía no tiene AAC (desktop, REQ-055 S1). Falla
 * EXPLÍCITO con una excepción: devolver `Result(success = false)` se confunde con un WAV
 * inválido, y un éxito vacío sería peor. Vive en jvmShared y no en desktopMain para que
 * el arnés de `androidUnitTest`, que sí corre en el gate, lo pueda fijar con un test.
 */
internal object UnsupportedAacEncoder : IAacEncoder {
    const val MESSAGE = "AAC no disponible en esta plataforma: la JVM todavía no tiene " +
        "IAacEncoder (REQ-055; el de desktop llega con su etapa)"

    override suspend fun wavToM4a(
        wavPath: String,
        m4aPath: String,
        bitrateBps: Int,
        onProgress: ((Float) -> Unit)?,
    ): Mp4AacTranscoder.Result = throw UnsupportedOperationException(MESSAGE)
}
