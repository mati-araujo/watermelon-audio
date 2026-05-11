/**
 * UsbVolumeControl.h
 *
 * USB Audio Class volume control via Feature Unit control transfers.
 * Supports both hardware volume (via USB) and digital fallback.
 *
 * References:
 * - USB Audio Class 1.0 Section 5.2.2.3 (Feature Unit)
 * - USB Audio Class 2.0 Section 5.2.5 (Feature Unit)
 */

#pragma once

#include <cstdint>
#include <atomic>
#include <mutex>
#include <cmath>
#include <vector>
#include <libusb.h>
#include "../platform/Logger.h"
#include "UsbConstants.h"
#include "UsbAudioTypes.h"

// Logging macros
#define VOLUME_LOG_TAG "UsbVolumeControl"
#define VOLUME_LOGD(...) wma::logMessage(wma::LogLevel::DEBUG, VOLUME_LOG_TAG, __VA_ARGS__)
#define VOLUME_LOGI(...) wma::logMessage(wma::LogLevel::INFO, VOLUME_LOG_TAG, __VA_ARGS__)
#define VOLUME_LOGW(...) wma::logMessage(wma::LogLevel::WARN, VOLUME_LOG_TAG, __VA_ARGS__)
#define VOLUME_LOGE(...) wma::logMessage(wma::LogLevel::ERROR, VOLUME_LOG_TAG, __VA_ARGS__)

namespace watermelon_audio {
namespace usb {

/**
 * Volume control capabilities for a Feature Unit.
 */
struct VolumeCapabilities {
    struct Channel {
        uint8_t channelNumber = 0;  // 0 = master, 1+ = physical/logical channel
        bool hasHardwareVolume = false;
        bool hasHardwareMute = false;
        bool volumeVerified = false;
        bool muteVerified = false;
    };

    bool hasVolumeControl = false;
    bool hasMuteControl = false;

    uint8_t featureUnitId = 0;
    uint8_t sourceId = 0;

    // Volume range in UAC units (1/256 dB per unit)
    int16_t minVolume = UAC_VOLUME_TYPICAL_MIN;  // Typically ~-96dB
    int16_t maxVolume = UAC_VOLUME_0DB;          // Typically 0dB
    int16_t volumeResolution = 1;                // Step size

    // Number of channels (0 = master only)
    uint8_t numChannels = 0;

    // Direction: true = output (playback), false = input (capture)
    bool isOutput = true;

    std::vector<Channel> channels;

    const Channel* findChannel(uint8_t channelNumber) const {
        for (const auto& channel : channels) {
            if (channel.channelNumber == channelNumber) return &channel;
        }
        return nullptr;
    }

    /**
     * Convert UAC volume units to dB.
     * @param volume UAC volume value (1/256 dB units)
     * @return Volume in dB
     */
    float volumeToDb(int16_t volume) const {
        if (volume == UAC_VOLUME_SILENCE) {
            return -INFINITY;
        }
        return static_cast<float>(volume) * UAC_VOLUME_STEP_DB;
    }

    /**
     * Convert dB to UAC volume units.
     * @param db Volume in dB
     * @return UAC volume value
     */
    int16_t dbToVolume(float db) const {
        if (db <= -96.0f || std::isinf(db)) {
            return UAC_VOLUME_SILENCE;
        }
        int32_t vol = static_cast<int32_t>(db / UAC_VOLUME_STEP_DB);
        return static_cast<int16_t>(std::clamp(vol,
            static_cast<int32_t>(minVolume),
            static_cast<int32_t>(maxVolume)));
    }

    /**
     * Convert UAC volume to linear gain (0.0 - 1.0).
     * @param volume UAC volume value
     * @return Linear gain where 0.0 = silence, 1.0 = 0dB
     */
    float volumeToLinear(int16_t volume) const {
        if (volume == UAC_VOLUME_SILENCE) {
            return 0.0f;
        }
        float db = volumeToDb(volume);
        // Map dB range to 0-1 linear
        // Typical range: -96dB to 0dB
        float minDb = volumeToDb(minVolume);
        float maxDb = volumeToDb(maxVolume);
        if (std::isinf(minDb)) minDb = -96.0f;

        return (db - minDb) / (maxDb - minDb);
    }

    /**
     * Convert linear gain (0.0 - 1.0) to UAC volume.
     * @param linear Linear gain
     * @return UAC volume value
     */
    int16_t linearToVolume(float linear) const {
        linear = std::clamp(linear, 0.0f, 1.0f);
        if (linear <= 0.001f) {
            return UAC_VOLUME_SILENCE;
        }
        float minDb = volumeToDb(minVolume);
        float maxDb = volumeToDb(maxVolume);
        if (std::isinf(minDb)) minDb = -96.0f;

        float db = minDb + linear * (maxDb - minDb);
        return dbToVolume(db);
    }
};

/**
 * USB Audio Volume Controller.
 *
 * Handles hardware volume control via USB Feature Unit control transfers.
 * Falls back to digital volume when hardware control is unavailable.
 */
class UsbVolumeControl {
public:
    /**
     * Construct a volume controller.
     * @param deviceHandle libusb device handle (must remain valid)
     * @param controlInterface Audio Control interface number
     */
    UsbVolumeControl(libusb_device_handle* deviceHandle, uint8_t controlInterface);
    ~UsbVolumeControl() = default;

    // Non-copyable
    UsbVolumeControl(const UsbVolumeControl&) = delete;
    UsbVolumeControl& operator=(const UsbVolumeControl&) = delete;

    /**
     * Initialize volume control for a Feature Unit.
     * Queries GET_MIN, GET_MAX, GET_RES to populate capabilities.
     *
     * @param featureUnitId The Feature Unit ID from descriptor parsing
     * @param hasVolume Whether the FU has volume control
     * @param hasMute Whether the FU has mute control
     * @param isOutput true for playback volume, false for capture
     * @param uacVersion 1 or 2
     * @return true if hardware volume control is available and working
     */
    bool initialize(uint8_t featureUnitId, bool hasVolume, bool hasMute,
                    bool isOutput, uint8_t uacVersion);

    /**
     * Initialize from a parsed Feature Unit descriptor, preserving exact
     * master/per-channel hardware volume and mute capability bits.
     */
    bool initialize(const UsbFeatureUnit& featureUnit,
                    bool isOutput,
                    uint8_t uacVersion);

    /**
     * Check if hardware volume control is available and initialized.
     */
    bool hasHardwareVolume() const { return mHardwareVolumeAvailable; }

    /**
     * Check if hardware mute control is available.
     */
    bool hasHardwareMute() const { return mHardwareMuteAvailable; }

    /**
     * Get volume capabilities.
     */
    const VolumeCapabilities& getCapabilities() const { return mCapabilities; }

    /**
     * Set volume for a specific Feature Unit channel.
     * Channel 0 is master; channels 1..N are physical/logical channels.
     */
    bool setChannelVolume(uint8_t channel, float volume);

    /**
     * Get current volume for a specific Feature Unit channel.
     */
    float getChannelVolume(uint8_t channel) const;

    bool setChannelMute(uint8_t channel, bool muted);
    bool isChannelMuted(uint8_t channel) const;

    /**
     * Set master volume (linear 0.0 - 1.0).
     * Uses hardware control if available, otherwise stores for digital fallback.
     *
     * @param volume Linear volume 0.0 (silent) to 1.0 (max)
     * @return true if successfully set (or stored for digital)
     */
    bool setVolume(float volume);

    /**
     * Get current volume (linear 0.0 - 1.0).
     */
    float getVolume() const;

    /**
     * Set mute state.
     * @param muted true to mute, false to unmute
     * @return true if successfully set
     */
    bool setMute(bool muted);

    /**
     * Get current mute state.
     */
    bool isMuted() const;

    /**
     * Get volume in dB for UI display.
     */
    float getVolumeDb() const;

    /**
     * Check if currently using hardware volume control.
     */
    bool isUsingHardwareVolume() const { return mHardwareVolumeAvailable; }

    /**
     * Get the digital volume multiplier for sample scaling.
     * Returns a value between 0.0 and 1.0.
     * This should be used when hardware volume is not available.
     */
    float getDigitalVolume() const {
        return mDigitalVolume.load(std::memory_order_relaxed);
    }

    /**
     * Get the mute state for digital muting.
     */
    bool getDigitalMute() const {
        return mDigitalMute.load(std::memory_order_relaxed);
    }

private:
    libusb_device_handle* mDeviceHandle;
    uint8_t mControlInterface;
    uint8_t mUacVersion = 1;

    VolumeCapabilities mCapabilities;
    bool mHardwareVolumeAvailable = false;
    bool mHardwareMuteAvailable = false;

    // Current volume in UAC units (for hardware mode)
    int16_t mCurrentVolume = UAC_VOLUME_0DB;

    // Atomic values for thread-safe access from audio thread
    std::atomic<float> mDigitalVolume{1.0f};
    std::atomic<bool> mDigitalMute{false};

    mutable std::mutex mMutex;

    // Control transfer timeout in milliseconds
    static constexpr unsigned int CONTROL_TIMEOUT_MS = 500;

    /**
     * Query volume range from device.
     * @return true if successful
     */
    bool queryVolumeRange(uint8_t channel = 0);

    void populateChannelCapabilities(const UsbFeatureUnit& featureUnit, uint8_t uacVersion);
    uint8_t preferredVolumeChannel() const;
    uint8_t preferredMuteChannel() const;

    /**
     * Set volume via USB control transfer.
     * @param volume UAC volume value
     * @param channel Channel number (0 = master)
     * @return true if successful
     */
    bool setHardwareVolume(int16_t volume, uint8_t channel = 0);

    /**
     * Get current hardware volume.
     * @param channel Channel number (0 = master)
     * @return UAC volume value
     */
    int16_t getHardwareVolume(uint8_t channel = 0) const;

    /**
     * Set hardware mute state.
     * @param muted true to mute
     * @param channel Channel number (0 = master)
     * @return true if successful
     */
    bool setHardwareMute(bool muted, uint8_t channel = 0);

    /**
     * Get hardware mute state.
     * @param channel Channel number (0 = master)
     * @return true if muted
     */
    bool getHardwareMute(uint8_t channel = 0) const;

    /**
     * Build wValue for Feature Unit control request.
     * wValue = (Control Selector << 8) | Channel Number
     */
    uint16_t buildWValue(uint8_t controlSelector, uint8_t channel) const {
        return (static_cast<uint16_t>(controlSelector) << 8) | channel;
    }

    /**
     * Build wIndex for Feature Unit control request.
     * wIndex = (Feature Unit ID << 8) | Interface Number
     */
    uint16_t buildWIndex() const {
        return (static_cast<uint16_t>(mCapabilities.featureUnitId) << 8) | mControlInterface;
    }
};

} // namespace usb
} // namespace watermelon_audio
