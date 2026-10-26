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
