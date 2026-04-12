package com.watermellonstudios.audio.domain.usb

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Decodes a binary capability snapshot produced by the C++ UsbSnapshotCodec
 * (encodeSnapshot in UsbSnapshotCodec.h) into a [UsbCapabilitySnapshot].
 *
 * Wire format: v1 little-endian, mirrors the C++ encoder exactly.
 *
 * Stage 2 — USB Audio Discovery & Directed Selection.
 */
object UsbSnapshotCodec {

    private const val FORMAT_VERSION: Byte = 0x01

    fun decode(bytes: ByteArray): UsbCapabilitySnapshot {
        val buf = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)

        // Version
        val version = buf.get()
        check(version == FORMAT_VERSION) {
            "Unknown snapshot format version: $version (expected $FORMAT_VERSION)"
        }

        // Total length (informational, we use buffer limit)
        buf.getInt()  // skip

        // Device info
        val vendorId = buf.getShort().toInt() and 0xFFFF
        val productId = buf.getShort().toInt() and 0xFFFF
        val uacVersion = buf.get().toInt() and 0xFF
        val productName = readString(buf)
        val manufacturer = readString(buf)
        val serialNumber = readString(buf)

        // Playback altsettings
        val numPlayback = buf.getShort().toInt() and 0xFFFF
        val playbackAlts = (0 until numPlayback).map { readAltsetting(buf) }

        // Capture altsettings
        val numCapture = buf.getShort().toInt() and 0xFFFF
        val captureAlts = (0 until numCapture).map { readAltsetting(buf) }

        // Clock sources
        val numClocks = buf.get().toInt() and 0xFF
        val clockSources = (0 until numClocks).map {
            ClockSourceInfo(
                clockId = buf.get().toInt() and 0xFF,
                type = ClockSourceType.fromId(buf.get().toInt() and 0xFF),
                syncedToSof = buf.get().toInt() != 0,
                hasFrequencyControl = buf.get().toInt() != 0,
                hasValidityControl = buf.get().toInt() != 0,
            )
        }

        // Feature units
        val numFUs = buf.get().toInt() and 0xFF
        val featureUnits = (0 until numFUs).map { readFeatureUnit(buf) }

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

    private fun readString(buf: ByteBuffer): String {
        val len = buf.getShort().toInt() and 0xFFFF
        if (len == 0) return ""
        val bytes = ByteArray(len)
        buf.get(bytes)
        return String(bytes, Charsets.UTF_8)
    }

    private fun readAltsetting(buf: ByteBuffer): AltsettingInfo {
        val ifNum = buf.get().toInt() and 0xFF
        val altNum = buf.get().toInt() and 0xFF
        val syncTypeRaw = buf.get().toInt() and 0xFF
        val flags = buf.get().toInt() and 0xFF
        val epAddress = buf.get().toInt() and 0xFF
        val termLink = buf.get().toInt() and 0xFF

        val syncMode = when (syncTypeRaw) {
            0x01 -> UsbSyncMode.ASYNCHRONOUS
            0x02 -> UsbSyncMode.ADAPTIVE
            0x03 -> UsbSyncMode.SYNCHRONOUS
            else -> UsbSyncMode.UNKNOWN
        }

        val numFormats = buf.get().toInt() and 0xFF
        val formats = (0 until numFormats).map { readFormat(buf) }

        return AltsettingInfo(
            interfaceNumber = ifNum,
            alternateSetting = altNum,
            formats = formats,
            syncType = syncMode,
            hasFeedbackEndpoint = (flags and 0x01) != 0,
            hasImplicitFeedback = (flags and 0x02) != 0,
            dataEndpointAddress = epAddress,
            terminalLinkId = termLink,
        )
    }

    private fun readFormat(buf: ByteBuffer): AudioFormatInfo {
        val channels = buf.get().toInt() and 0xFF
        val bitRes = buf.get().toInt() and 0xFF
        val bytesPerSample = buf.get().toInt() and 0xFF
        val hasContinuous = buf.get().toInt() != 0
        val minRate = buf.getInt()
        val maxRate = buf.getInt()
        val numRates = buf.get().toInt() and 0xFF
        val rates = (0 until numRates).map { buf.getInt() }

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

    private fun readFeatureUnit(buf: ByteBuffer): FeatureUnitInfo {
        val unitId = buf.get().toInt() and 0xFF
        val sourceId = buf.get().toInt() and 0xFF
        val numChannels = buf.get().toInt() and 0xFF
        val masterFlags = buf.get().toInt() and 0xFF
        val numCh = buf.get().toInt() and 0xFF

        val perChVolume = mutableListOf<Boolean>()
        val perChMute = mutableListOf<Boolean>()
        for (i in 0 until numCh) {
            val chFlags = buf.get().toInt() and 0xFF
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
}
