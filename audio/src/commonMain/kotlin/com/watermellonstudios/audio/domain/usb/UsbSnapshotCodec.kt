package com.watermellonstudios.audio.domain.usb

/**
 * Decodes a binary capability snapshot produced by the C++ UsbSnapshotCodec
 * (encodeSnapshot in UsbSnapshotCodec.h) into a [UsbCapabilitySnapshot].
 *
 * Wire format: v1 little-endian, mirrors the C++ encoder exactly.
 *
 * Stage 2 — USB Audio Discovery & Directed Selection.
 */
object UsbSnapshotCodec {

    private const val FORMAT_VERSION: Int = 0x01

    fun decode(bytes: ByteArray): UsbCapabilitySnapshot {
        val reader = SnapshotReader(bytes)

        // Version
        val version = reader.readU8()
        check(version == FORMAT_VERSION) {
            "Unknown snapshot format version: $version (expected $FORMAT_VERSION)"
        }

        // Total length (informational, we use buffer limit)
        reader.readU32()  // skip

        // Device info
        val vendorId = reader.readU16()
        val productId = reader.readU16()
        val uacVersion = reader.readU8()
        val productName = reader.readString()
        val manufacturer = reader.readString()
        val serialNumber = reader.readString()

        // Playback altsettings
        val numPlayback = reader.readU16()
        val playbackAlts = (0 until numPlayback).map { readAltsetting(reader) }

        // Capture altsettings
        val numCapture = reader.readU16()
        val captureAlts = (0 until numCapture).map { readAltsetting(reader) }

        // Clock sources (stage 3: now carries sample rate list from RANGE query)
        val numClocks = reader.readU8()
        val clockSources = (0 until numClocks).map {
            val clockId = reader.readU8()
            val type = ClockSourceType.fromId(reader.readU8())
            val syncedToSof = reader.readU8() != 0
            val hasFreqControl = reader.readU8() != 0
            val hasValidityControl = reader.readU8() != 0
            // Stage 3: rates
            val hasContinuous = reader.readU8() != 0
            val minRate = reader.readU32()
            val maxRate = reader.readU32()
            val numRates = reader.readU8()
            val rates = (0 until numRates).map { reader.readU32() }
            ClockSourceInfo(
                clockId = clockId,
                type = type,
                syncedToSof = syncedToSof,
                hasFrequencyControl = hasFreqControl,
                hasValidityControl = hasValidityControl,
                sampleRates = rates,
                hasContinuousRates = hasContinuous,
                minSampleRate = minRate,
                maxSampleRate = maxRate,
            )
        }

        // Feature units
        val numFUs = reader.readU8()
        val featureUnits = (0 until numFUs).map { readFeatureUnit(reader) }

        return UsbCapabilitySnapshot(
            vendorId = vendorId,
            productId = productId,
            productName = productName,
            manufacturer = manufacturer,
            serialNumber = serialNumber,
            uacVersion = uacVersion,
            playbackAltsettings = playbackAlts,
            captureAltsettings = captureAlts,
            clockSources = clockSources,
            featureUnits = featureUnits,
        )
    }

    private fun readAltsetting(reader: SnapshotReader): AltsettingInfo {
        val ifNum = reader.readU8()
        val altNum = reader.readU8()
        val syncTypeRaw = reader.readU8()
        val flags = reader.readU8()
        val epAddress = reader.readU8()
        val termLink = reader.readU8()
        val endpointInterval = reader.readU8()

        val syncMode = when (syncTypeRaw) {
            0x01 -> UsbSyncMode.ASYNCHRONOUS
            0x02 -> UsbSyncMode.ADAPTIVE
            0x03 -> UsbSyncMode.SYNCHRONOUS
            else -> UsbSyncMode.UNKNOWN
        }

        val numFormats = reader.readU8()
        val formats = (0 until numFormats).map { readFormat(reader) }

        return AltsettingInfo(
            interfaceNumber = ifNum,
            alternateSetting = altNum,
            formats = formats,
            syncType = syncMode,
            hasFeedbackEndpoint = (flags and 0x01) != 0,
            hasImplicitFeedback = (flags and 0x02) != 0,
            dataEndpointAddress = epAddress,
            terminalLinkId = termLink,
            endpointInterval = endpointInterval,
        )
    }

    private fun readFormat(reader: SnapshotReader): AudioFormatInfo {
        val channels = reader.readU8()
        val bitRes = reader.readU8()
        val bytesPerSample = reader.readU8()
        val hasContinuous = reader.readU8() != 0
        val minRate = reader.readU32()
        val maxRate = reader.readU32()
        val numRates = reader.readU8()
        val rates = (0 until numRates).map { reader.readU32() }

        return AudioFormatInfo(
            channels = channels,
            bitResolution = bitRes,
            bytesPerSample = bytesPerSample,
            sampleRates = rates,
            hasContinuousRates = hasContinuous,
            minSampleRate = minRate,
            maxSampleRate = maxRate,
        )
    }

    private fun readFeatureUnit(reader: SnapshotReader): FeatureUnitInfo {
        val unitId = reader.readU8()
        val sourceId = reader.readU8()
        val numChannels = reader.readU8()
        val masterFlags = reader.readU8()
        val numCh = reader.readU8()

        val perChVolume = mutableListOf<Boolean>()
        val perChMute = mutableListOf<Boolean>()
        for (i in 0 until numCh) {
            val chFlags = reader.readU8()
            perChVolume.add((chFlags and 0x01) != 0)
            perChMute.add((chFlags and 0x02) != 0)
        }

        return FeatureUnitInfo(
            unitId = unitId,
            sourceId = sourceId,
            channelCount = numChannels,
            hasMasterVolume = (masterFlags and 0x01) != 0,
            hasMasterMute = (masterFlags and 0x02) != 0,
            perChannelVolume = perChVolume,
            perChannelMute = perChMute,
        )
    }

    private class SnapshotReader(private val bytes: ByteArray) {
        private var position = 0

        fun readU8(): Int {
            requireRemaining(1)
            return bytes[position++].toInt() and 0xFF
        }

        fun readU16(): Int {
            val b0 = readU8()
            val b1 = readU8()
            return b0 or (b1 shl 8)
        }

        fun readU32(): Int {
            val b0 = readU8()
            val b1 = readU8()
            val b2 = readU8()
            val b3 = readU8()
            return b0 or (b1 shl 8) or (b2 shl 16) or (b3 shl 24)
        }

        fun readString(): String {
            val length = readU16()
            if (length == 0) return ""
            requireRemaining(length)
            val value = bytes.copyOfRange(position, position + length).decodeToString()
            position += length
            return value
        }

        private fun requireRemaining(count: Int) {
            require(position + count <= bytes.size) {
                "Truncated USB capability snapshot at byte $position"
            }
        }
    }
}
