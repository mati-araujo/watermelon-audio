package com.watermellonstudios.audio.export

/**
 * Transcode a stereo (or mono) WAV file into an AAC LC M4A container using
 * the platform's encoder (on Android, the hardware-accelerated MediaCodec).
 *
 * Supports input formats: 16-bit PCM, 24-bit PCM, 32-bit IEEE float (mono or stereo).
 * Output is always 2-channel AAC LC inside an MPEG-4 container (.m4a).
 *
 * Use case: convert the lossless WAV produced by [com.watermellonstudios.audio.internal.bridge.AudioNativeBridge.looperExportMixPro]
 * into a compressed file ~10x smaller, suitable for sharing via WhatsApp/Telegram/email.
 *
 * La conversión la hace el [IAacEncoder] de la plataforma (MediaCodec en Android); este
 * objeto conserva la API pública —[Bitrate], [Result], [wavToM4a]— con la forma de siempre.
 *
 * NOT RT-safe — runs on Dispatchers.IO. The encoder/muxer are released even on failure.
 */
object Mp4AacTranscoder {
    /** Common AAC bitrates (bps). Higher = better quality, larger file. */
    object Bitrate {
        const val LOW = 96_000     // ~720 KB/min, voice-quality
        const val MEDIUM = 128_000 // ~960 KB/min, podcast-quality
        const val HIGH = 192_000   // ~1.4 MB/min, transparent for most music
        const val PREMIUM = 256_000 // ~1.9 MB/min, near-CD for AAC LC
    }

    data class Result(
        val success: Boolean,
        val bytesWritten: Long = 0L,
        val durationMs: Long = 0L,
        val error: String? = null
    )

    /**
     * @param wavPath    Source WAV path (must exist, valid RIFF/WAVE header).
     * @param m4aPath    Destination M4A path (overwritten if exists).
     * @param bitrateBps Target bitrate; default 192 kbps. See [Bitrate].
     * @param onProgress Optional callback in [0..1], invoked from IO thread.
     */
    suspend fun wavToM4a(
        wavPath: String,
        m4aPath: String,
        bitrateBps: Int = Bitrate.HIGH,
        onProgress: ((Float) -> Unit)? = null
    ): Result = platformAacEncoder.wavToM4a(wavPath, m4aPath, bitrateBps, onProgress)
}
