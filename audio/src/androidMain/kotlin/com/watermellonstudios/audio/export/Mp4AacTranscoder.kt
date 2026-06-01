package com.watermellonstudios.audio.export

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Transcode a stereo (or mono) WAV file into an AAC LC M4A container using
 * Android's hardware-accelerated MediaCodec encoder.
 *
 * Supports input formats: 16-bit PCM, 24-bit PCM, 32-bit IEEE float (mono or stereo).
 * Output is always 2-channel AAC LC inside an MPEG-4 container (.m4a).
 *
 * Use case: convert the lossless WAV produced by [com.watermellonstudios.audio.internal.bridge.AudioNativeBridge.looperExportMixPro]
 * into a compressed file ~10x smaller, suitable for sharing via WhatsApp/Telegram/email.
 *
 * NOT RT-safe — runs on Dispatchers.IO. The encoder/muxer are released even on failure.
 */
object Mp4AacTranscoder {
    private const val TAG = "Mp4AacTranscoder"
    private const val MIME_AAC = "audio/mp4a-latm"
    private const val INPUT_TIMEOUT_US = 10_000L
    private const val FRAMES_PER_CHUNK = 1024

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
    ): Result = withContext(Dispatchers.IO) {
        val src = File(wavPath)
        if (!src.exists() || src.length() < 44) {
            return@withContext Result(false, error = "source missing or too small")
        }

        val header = readWavHeader(src)
            ?: return@withContext Result(false, error = "invalid WAV header")

        if (header.channels !in 1..2) {
            return@withContext Result(false, error = "unsupported channel count: ${header.channels}")
        }
        if (!header.isSupported()) {
            return@withContext Result(false, error = "unsupported sample format (${header.audioFormat}/${header.bitsPerSample}b)")
        }

        // Delete output if it exists — MediaMuxer refuses to overwrite.
        File(m4aPath).also { if (it.exists()) it.delete() }

        val outChannels = 2
        val format = MediaFormat.createAudioFormat(MIME_AAC, header.sampleRate, outChannels).apply {
            setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrateBps)
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16 * 1024)
        }

        val encoder: MediaCodec = try {
            MediaCodec.createEncoderByType(MIME_AAC).also {
                it.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                it.start()
            }
        } catch (e: Exception) {
            Log.e(TAG, "encoder init failed", e)
            return@withContext Result(false, error = "encoder init: ${e.message}")
        }

        val muxer: MediaMuxer = try {
            MediaMuxer(m4aPath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        } catch (e: Exception) {
            Log.e(TAG, "muxer init failed", e)
            runCatching { encoder.stop() }
            encoder.release()
            return@withContext Result(false, error = "muxer init: ${e.message}")
        }

        val startNs = System.nanoTime()
        var muxerStarted = false
        var trackIndex = -1
        val bufferInfo = MediaCodec.BufferInfo()
        var presentationTimeUs = 0L
        var totalFramesRead = 0L
        val totalFrames = header.numFrames.toLong()

        val inputBytesPerFrame = header.channels * (header.bitsPerSample / 8)
        val rawChunk = ByteArray(FRAMES_PER_CHUNK * inputBytesPerFrame)
        val pcmOut = ByteArray(FRAMES_PER_CHUNK * outChannels * 2)
        var eosSent = false

        try {
            RandomAccessFile(src, "r").use { raf ->
                raf.seek(header.dataOffset.toLong())

                outer@ while (true) {
                    // -------- Drain output --------
                    while (true) {
                        val outIdx = encoder.dequeueOutputBuffer(
                            bufferInfo,
                            if (eosSent) INPUT_TIMEOUT_US else 0L
                        )
                        when {
                            outIdx == MediaCodec.INFO_TRY_AGAIN_LATER -> break
                            outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                                if (muxerStarted) {
                                    throw IllegalStateException("format changed twice")
                                }
                                trackIndex = muxer.addTrack(encoder.outputFormat)
                                muxer.start()
                                muxerStarted = true
                            }
                            outIdx >= 0 -> {
                                val outBuf = encoder.getOutputBuffer(outIdx)
                                val isConfig = (bufferInfo.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
                                if (outBuf != null && bufferInfo.size > 0 && muxerStarted && !isConfig) {
                                    outBuf.position(bufferInfo.offset)
                                    outBuf.limit(bufferInfo.offset + bufferInfo.size)
                                    muxer.writeSampleData(trackIndex, outBuf, bufferInfo)
                                }
                                encoder.releaseOutputBuffer(outIdx, false)
                                if ((bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                                    break@outer
                                }
                            }
                        }
                    }

                    if (eosSent) continue

                    // -------- Feed input --------
                    val inputIdx = encoder.dequeueInputBuffer(INPUT_TIMEOUT_US)
                    if (inputIdx < 0) continue
                    val inputBuf = encoder.getInputBuffer(inputIdx) ?: continue
                    inputBuf.clear()

                    val framesRemaining = totalFrames - totalFramesRead
                    if (framesRemaining <= 0L) {
                        encoder.queueInputBuffer(
                            inputIdx, 0, 0, presentationTimeUs,
                            MediaCodec.BUFFER_FLAG_END_OF_STREAM
                        )
                        eosSent = true
                        continue
                    }

                    val framesThisChunk = minOf(FRAMES_PER_CHUNK.toLong(), framesRemaining).toInt()
                    val bytesToRead = framesThisChunk * inputBytesPerFrame
                    val readBytes = raf.read(rawChunk, 0, bytesToRead)
                    if (readBytes <= 0) {
                        encoder.queueInputBuffer(
                            inputIdx, 0, 0, presentationTimeUs,
                            MediaCodec.BUFFER_FLAG_END_OF_STREAM
                        )
                        eosSent = true
                        continue
                    }

                    val framesRead = readBytes / inputBytesPerFrame
                    val pcmBytes = convertToInt16Stereo(
                        rawChunk, framesRead, header.channels,
                        header.bitsPerSample, header.audioFormat, pcmOut
                    )
                    inputBuf.put(pcmOut, 0, pcmBytes)
                    encoder.queueInputBuffer(inputIdx, 0, pcmBytes, presentationTimeUs, 0)

                    presentationTimeUs += framesRead.toLong() * 1_000_000L / header.sampleRate
                    totalFramesRead += framesRead

                    if (totalFrames > 0) {
                        onProgress?.invoke(
                            (totalFramesRead.toDouble() / totalFrames).toFloat().coerceIn(0f, 1f)
                        )
                    }
                }
            }
            onProgress?.invoke(1f)
        } catch (e: Exception) {
            Log.e(TAG, "transcode failure", e)
            runCatching { encoder.stop() }
            encoder.release()
            if (muxerStarted) runCatching { muxer.stop() }
            runCatching { muxer.release() }
            File(m4aPath).delete()
            return@withContext Result(false, error = "transcode: ${e.message}")
        }

        runCatching { encoder.stop() }
        encoder.release()
        if (muxerStarted) {
            runCatching { muxer.stop() }
        }
        runCatching { muxer.release() }

        val outLen = runCatching { File(m4aPath).length() }.getOrDefault(0L)
        val durMs = (System.nanoTime() - startNs) / 1_000_000L
        Result(success = outLen > 0, bytesWritten = outLen, durationMs = durMs)
    }

    // ============================================================
    // WAV parsing
    // ============================================================

    private data class WavHeader(
        val sampleRate: Int,
        val channels: Int,
        val bitsPerSample: Int,
        val audioFormat: Int,    // 1 = PCM, 3 = IEEE float
        val dataOffset: Int,
        val dataSize: Int
    ) {
        val numFrames: Int get() = dataSize / (channels * (bitsPerSample / 8))
        fun isSupported(): Boolean = when {
            audioFormat == 1 && (bitsPerSample == 16 || bitsPerSample == 24) -> true
            audioFormat == 3 && bitsPerSample == 32 -> true
            else -> false
        }
    }

    private fun readWavHeader(file: File): WavHeader? {
        RandomAccessFile(file, "r").use { raf ->
            val riff = ByteArray(4); raf.readFully(riff)
            if (!riff.matches("RIFF")) return null
            raf.skipBytes(4) // file size
            val wave = ByteArray(4); raf.readFully(wave)
            if (!wave.matches("WAVE")) return null

            var sampleRate = 0
            var channels = 0
            var bitsPerSample = 0
            var audioFormat = 0
            var dataOffset = 0
            var dataSize = 0

            val id = ByteArray(4)
            val sizeBytes = ByteArray(4)
            while (raf.filePointer <= raf.length() - 8) {
                raf.readFully(id)
                raf.readFully(sizeBytes)
                val size = ByteBuffer.wrap(sizeBytes).order(ByteOrder.LITTLE_ENDIAN).int
                when {
                    id.matches("fmt ") -> {
                        val fmt = ByteArray(size); raf.readFully(fmt)
                        val bb = ByteBuffer.wrap(fmt).order(ByteOrder.LITTLE_ENDIAN)
                        audioFormat = bb.short.toInt() and 0xFFFF
                        channels = bb.short.toInt() and 0xFFFF
                        sampleRate = bb.int
                        bb.int          // byteRate
                        bb.short        // blockAlign
                        bitsPerSample = bb.short.toInt() and 0xFFFF
                    }
                    id.matches("data") -> {
                        dataOffset = raf.filePointer.toInt()
                        dataSize = size
                        // we have everything we need; stop scanning
                        return WavHeader(sampleRate, channels, bitsPerSample, audioFormat, dataOffset, dataSize)
                    }
                    else -> raf.skipBytes(size + (size and 1)) // pad to even
                }
            }
            return null
        }
    }

    private fun ByteArray.matches(s: String): Boolean {
        if (size < s.length) return false
        for (i in s.indices) if (this[i].toInt().toChar() != s[i]) return false
        return true
    }

    // ============================================================
    // Sample format conversion → int16 stereo LE
    // ============================================================

    private fun convertToInt16Stereo(
        raw: ByteArray, frames: Int, srcChannels: Int,
        bitsPerSample: Int, audioFormat: Int, out: ByteArray
    ): Int {
        val outBuf = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN)
        outBuf.clear()
        val inBuf = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN)

        for (f in 0 until frames) {
            val l: Short
            val r: Short
            when {
                audioFormat == 3 && bitsPerSample == 32 -> {
                    val s0 = floatToInt16(inBuf.float)
                    val s1 = if (srcChannels == 2) floatToInt16(inBuf.float) else s0
                    l = s0; r = s1
                }
                bitsPerSample == 16 -> {
                    val s0 = inBuf.short
                    val s1 = if (srcChannels == 2) inBuf.short else s0
                    l = s0; r = s1
                }
                bitsPerSample == 24 -> {
                    val s0 = read24bitLE(inBuf)
                    val s1 = if (srcChannels == 2) read24bitLE(inBuf) else s0
                    l = s0; r = s1
                }
                else -> { l = 0; r = 0 }
            }
            outBuf.putShort(l); outBuf.putShort(r)
        }
        return outBuf.position()
    }

    private fun floatToInt16(f: Float): Short {
        val c = if (f > 1f) 1f else if (f < -1f) -1f else f
        return (c * 32767f).toInt().toShort()
    }

    private fun read24bitLE(buf: ByteBuffer): Short {
        val b0 = buf.get().toInt() and 0xFF
        val b1 = buf.get().toInt() and 0xFF
        val b2 = buf.get().toInt() and 0xFF
        var v = b0 or (b1 shl 8) or (b2 shl 16)
        if (v and 0x800000 != 0) v = v or 0xFF000000.toInt()
        // Scale ±2^23 down to ±2^15 by arithmetic right shift.
        return (v shr 8).toShort()
    }
}
