/**
 * UsbVolumeControl.cpp
 *
 * Implementation of USB Audio Class volume control via Feature Unit.
 */

#include "UsbVolumeControl.h"
#include <cstring>

namespace watermelon_audio {
namespace usb {

UsbVolumeControl::UsbVolumeControl(libusb_device_handle* deviceHandle,
                                   uint8_t controlInterface)
    : mDeviceHandle(deviceHandle)
    , mControlInterface(controlInterface) {
}

bool UsbVolumeControl::initialize(uint8_t featureUnitId, bool hasVolume,
                                  bool hasMute, bool isOutput, uint8_t uacVersion) {
    UsbFeatureUnit featureUnit;
    featureUnit.unitId = featureUnitId;
    featureUnit.numChannels = 0;
    uint32_t masterControls = 0;
    if (hasMute) masterControls |= (uacVersion == 2) ? 0x00000003 : (1u << 0);
    if (hasVolume) masterControls |= (uacVersion == 2) ? 0x0000000C : (1u << 1);
    featureUnit.channelControls.push_back(masterControls);
    return initialize(featureUnit, isOutput, uacVersion);
}

bool UsbVolumeControl::initialize(const UsbFeatureUnit& featureUnit,
                                  bool isOutput,
                                  uint8_t uacVersion) {
    std::lock_guard<std::mutex> lock(mMutex);

    mUacVersion = uacVersion;
    mCapabilities = VolumeCapabilities{};
    mCapabilities.featureUnitId = featureUnit.unitId;
    mCapabilities.sourceId = featureUnit.sourceId;
    mCapabilities.hasVolumeControl = featureUnit.hasAnyVolumeControl(uacVersion);
    mCapabilities.hasMuteControl = featureUnit.hasAnyMuteControl(uacVersion);
    mCapabilities.numChannels = featureUnit.numChannels;
    mCapabilities.isOutput = isOutput;
    populateChannelCapabilities(featureUnit, uacVersion);

    VOLUME_LOGI("UsbVolumeControl: Initializing FU=%d, hasVolume=%d, hasMute=%d, channels=%d, isOutput=%d, UAC=%d",
         featureUnit.unitId, mCapabilities.hasVolumeControl, mCapabilities.hasMuteControl,
         mCapabilities.numChannels, isOutput, uacVersion);

    if (!mDeviceHandle) {
        VOLUME_LOGW("UsbVolumeControl: No device handle, using digital volume only");
        return false;
    }

    // Try to query volume range if volume control is present
    if (mCapabilities.hasVolumeControl) {
        const uint8_t volumeChannel = preferredVolumeChannel();
        if (queryVolumeRange(volumeChannel)) {
            // Try to get current volume to verify the control works
            int16_t currentVol = getHardwareVolume(volumeChannel);
            if (currentVol != UAC_VOLUME_SILENCE || getHardwareMute(volumeChannel)) {
                mHardwareVolumeAvailable = true;
                mCurrentVolume = currentVol;
                VOLUME_LOGI("UsbVolumeControl: Hardware volume available, current=%d (%.1f dB)",
                     currentVol, mCapabilities.volumeToDb(currentVol));
            } else {
                // Try setting volume to verify write works
                if (setHardwareVolume(UAC_VOLUME_0DB, volumeChannel)) {
                    mHardwareVolumeAvailable = true;
                    mCurrentVolume = UAC_VOLUME_0DB;
                    VOLUME_LOGI("UsbVolumeControl: Hardware volume verified via SET_CUR");
                }
            }
            for (auto& channel : mCapabilities.channels) {
                if (channel.hasHardwareVolume) {
                    channel.volumeVerified = mHardwareVolumeAvailable;
                }
            }
        }
    }

    // Check mute control
    if (mCapabilities.hasMuteControl) {
        const uint8_t muteChannel = preferredMuteChannel();
        bool muteState = getHardwareMute(muteChannel);
        // If we can read mute state without error, mute control is available
        mHardwareMuteAvailable = true;
        mDigitalMute.store(muteState, std::memory_order_relaxed);
        VOLUME_LOGI("UsbVolumeControl: Hardware mute available, current=%d", muteState);
        for (auto& channel : mCapabilities.channels) {
            if (channel.hasHardwareMute) {
                channel.muteVerified = true;
            }
        }
    }

    // Set initial digital volume based on hardware state
    if (mHardwareVolumeAvailable) {
        float linear = mCapabilities.volumeToLinear(mCurrentVolume);
        mDigitalVolume.store(linear, std::memory_order_relaxed);
    }

    VOLUME_LOGI("UsbVolumeControl: Initialized - HW volume=%d, HW mute=%d, range=[%d, %d] res=%d",
         mHardwareVolumeAvailable, mHardwareMuteAvailable,
         mCapabilities.minVolume, mCapabilities.maxVolume, mCapabilities.volumeResolution);

    return mHardwareVolumeAvailable;
}

bool UsbVolumeControl::queryVolumeRange(uint8_t channel) {
    if (!mDeviceHandle) return false;

    int16_t minVol = 0, maxVol = 0, resVol = 1;
    uint8_t data[2];
    int result;

    uint16_t wValue = buildWValue(UAC_FU_VOLUME_CONTROL, channel);
    uint16_t wIndex = buildWIndex();

    if (mUacVersion == 2) {
        // UAC 2.0 uses RANGE request which returns triplets of (min, max, res)
        uint8_t rangeData[8];  // wNumSubRanges (2) + min (2) + max (2) + res (2)
        result = libusb_control_transfer(
            mDeviceHandle,
            UAC_REQUEST_TYPE_GET,
            UAC2_REQUEST_RANGE,
            wValue,
            wIndex,
            rangeData,
            sizeof(rangeData),
            CONTROL_TIMEOUT_MS
        );

        if (result >= 8) {
            uint16_t numRanges = rangeData[0] | (rangeData[1] << 8);
            if (numRanges >= 1) {
                std::memcpy(&minVol, &rangeData[2], 2);
                std::memcpy(&maxVol, &rangeData[4], 2);
                std::memcpy(&resVol, &rangeData[6], 2);
                VOLUME_LOGD("UsbVolumeControl: UAC 2.0 RANGE returned %d ranges, using first", numRanges);
            }
        } else {
            VOLUME_LOGW("UsbVolumeControl: UAC 2.0 RANGE failed: %d", result);
        }
    } else {
        // UAC 1.0 uses separate GET_MIN, GET_MAX, GET_RES requests

        // GET_MIN
        result = libusb_control_transfer(
            mDeviceHandle,
            UAC_REQUEST_TYPE_GET,
            UAC_REQUEST_GET_MIN,
            wValue,
            wIndex,
            data,
            2,
            CONTROL_TIMEOUT_MS
        );
        if (result == 2) {
            std::memcpy(&minVol, data, 2);
        } else {
            VOLUME_LOGW("UsbVolumeControl: GET_MIN failed: %d", result);
            return false;
        }

        // GET_MAX
        result = libusb_control_transfer(
            mDeviceHandle,
            UAC_REQUEST_TYPE_GET,
            UAC_REQUEST_GET_MAX,
            wValue,
            wIndex,
            data,
            2,
            CONTROL_TIMEOUT_MS
        );
        if (result == 2) {
            std::memcpy(&maxVol, data, 2);
        } else {
            VOLUME_LOGW("UsbVolumeControl: GET_MAX failed: %d", result);
            return false;
        }

        // GET_RES (optional, some devices don't support it)
        result = libusb_control_transfer(
            mDeviceHandle,
            UAC_REQUEST_TYPE_GET,
            UAC_REQUEST_GET_RES,
            wValue,
            wIndex,
            data,
            2,
            CONTROL_TIMEOUT_MS
        );
        if (result == 2) {
            std::memcpy(&resVol, data, 2);
        } else {
            resVol = 1;  // Default resolution
            VOLUME_LOGD("UsbVolumeControl: GET_RES not supported, using default");
        }
    }

    mCapabilities.minVolume = minVol;
    mCapabilities.maxVolume = maxVol;
    mCapabilities.volumeResolution = resVol > 0 ? resVol : 1;

    VOLUME_LOGI("UsbVolumeControl: Volume range: min=%d (%.1f dB), max=%d (%.1f dB), res=%d",
         minVol, mCapabilities.volumeToDb(minVol),
         maxVol, mCapabilities.volumeToDb(maxVol),
         resVol);

    return true;
}

bool UsbVolumeControl::setHardwareVolume(int16_t volume, uint8_t channel) {
    if (!mDeviceHandle) return false;

    uint16_t wValue = buildWValue(UAC_FU_VOLUME_CONTROL, channel);
    uint16_t wIndex = buildWIndex();

    uint8_t data[2];
    std::memcpy(data, &volume, 2);

    uint8_t request = (mUacVersion == 2) ? UAC2_REQUEST_CUR : UAC_REQUEST_SET_CUR;

    int result = libusb_control_transfer(
        mDeviceHandle,
        UAC_REQUEST_TYPE_SET,
        request,
        wValue,
        wIndex,
        data,
        2,
        CONTROL_TIMEOUT_MS
    );

    if (result < 0) {
        VOLUME_LOGW("UsbVolumeControl: SET_CUR volume failed: %s", libusb_error_name(result));
        return false;
    }

    VOLUME_LOGD("UsbVolumeControl: Set hardware volume to %d (%.1f dB) on channel %d",
         volume, mCapabilities.volumeToDb(volume), channel);
    return true;
}

int16_t UsbVolumeControl::getHardwareVolume(uint8_t channel) const {
    if (!mDeviceHandle) return UAC_VOLUME_SILENCE;

    uint16_t wValue = buildWValue(UAC_FU_VOLUME_CONTROL, channel);
    uint16_t wIndex = buildWIndex();

    uint8_t data[2] = {0, 0};
    uint8_t request = (mUacVersion == 2) ? UAC2_REQUEST_CUR : UAC_REQUEST_GET_CUR;

    int result = libusb_control_transfer(
        mDeviceHandle,
        UAC_REQUEST_TYPE_GET,
        request,
        wValue,
        wIndex,
        data,
        2,
        CONTROL_TIMEOUT_MS
    );

    if (result != 2) {
        VOLUME_LOGW("UsbVolumeControl: GET_CUR volume failed: %d", result);
        return UAC_VOLUME_SILENCE;
    }

    int16_t volume;
    std::memcpy(&volume, data, 2);
    return volume;
}

bool UsbVolumeControl::setHardwareMute(bool muted, uint8_t channel) {
    if (!mDeviceHandle) return false;

    uint16_t wValue = buildWValue(UAC_FU_MUTE_CONTROL, channel);
    uint16_t wIndex = buildWIndex();

    uint8_t data[1] = { muted ? UAC_MUTE_ON : UAC_MUTE_OFF };
    uint8_t request = (mUacVersion == 2) ? UAC2_REQUEST_CUR : UAC_REQUEST_SET_CUR;

    int result = libusb_control_transfer(
        mDeviceHandle,
        UAC_REQUEST_TYPE_SET,
        request,
        wValue,
        wIndex,
        data,
        1,
        CONTROL_TIMEOUT_MS
    );

    if (result < 0) {
        VOLUME_LOGW("UsbVolumeControl: SET_CUR mute failed: %s", libusb_error_name(result));
        return false;
    }

    VOLUME_LOGD("UsbVolumeControl: Set hardware mute to %d on channel %d", muted, channel);
    return true;
}

bool UsbVolumeControl::getHardwareMute(uint8_t channel) const {
    if (!mDeviceHandle) return false;

    uint16_t wValue = buildWValue(UAC_FU_MUTE_CONTROL, channel);
    uint16_t wIndex = buildWIndex();

    uint8_t data[1] = {0};
    uint8_t request = (mUacVersion == 2) ? UAC2_REQUEST_CUR : UAC_REQUEST_GET_CUR;

    int result = libusb_control_transfer(
        mDeviceHandle,
        UAC_REQUEST_TYPE_GET,
        request,
        wValue,
        wIndex,
        data,
        1,
        CONTROL_TIMEOUT_MS
    );

    if (result != 1) {
        return false;
    }

    return data[0] != UAC_MUTE_OFF;
}

bool UsbVolumeControl::setVolume(float volume) {
    std::lock_guard<std::mutex> lock(mMutex);

    volume = std::clamp(volume, 0.0f, 1.0f);
    mDigitalVolume.store(volume, std::memory_order_relaxed);

    if (!mHardwareVolumeAvailable) {
        return true;
    }

    const int16_t uacVolume = mCapabilities.linearToVolume(volume);
    if (const auto* master = mCapabilities.findChannel(0);
        master && master->hasHardwareVolume) {
        if (setHardwareVolume(uacVolume, 0)) {
            mCurrentVolume = uacVolume;
            return true;
        }
        VOLUME_LOGW("UsbVolumeControl: Master hardware volume set failed, using digital");
        return true;
    }

    bool anySet = false;
    for (const auto& channel : mCapabilities.channels) {
        if (channel.hasHardwareVolume &&
            setHardwareVolume(uacVolume, channel.channelNumber)) {
            anySet = true;
        }
    }
    if (anySet) {
        mCurrentVolume = uacVolume;
    } else {
        VOLUME_LOGW("UsbVolumeControl: Per-channel hardware volume set failed, using digital");
    }
    return true;
}

float UsbVolumeControl::getVolume() const {
    return mDigitalVolume.load(std::memory_order_relaxed);
}

bool UsbVolumeControl::setMute(bool muted) {
    std::lock_guard<std::mutex> lock(mMutex);

    mDigitalMute.store(muted, std::memory_order_relaxed);

    if (!mHardwareMuteAvailable) {
        return true;
    }

    if (const auto* master = mCapabilities.findChannel(0);
        master && master->hasHardwareMute) {
        if (!setHardwareMute(muted, 0)) {
            VOLUME_LOGW("UsbVolumeControl: Master hardware mute set failed, using digital");
        }
        return true;
    }

    bool anySet = false;
    for (const auto& channel : mCapabilities.channels) {
        if (channel.hasHardwareMute &&
            setHardwareMute(muted, channel.channelNumber)) {
            anySet = true;
        }
    }
    if (!anySet) {
        VOLUME_LOGW("UsbVolumeControl: Per-channel hardware mute set failed, using digital");
    }
    return true;
}

bool UsbVolumeControl::isMuted() const {
    return mDigitalMute.load(std::memory_order_relaxed);
}

bool UsbVolumeControl::setChannelVolume(uint8_t channel, float volume) {
    std::lock_guard<std::mutex> lock(mMutex);

    volume = std::clamp(volume, 0.0f, 1.0f);

    // Always update digital volume for fallback/consistency
    mDigitalVolume.store(volume, std::memory_order_relaxed);

    const auto* channelCaps = mCapabilities.findChannel(channel);
    const bool channelHasHardware = channelCaps && channelCaps->hasHardwareVolume;

    if (mHardwareVolumeAvailable && channelHasHardware) {
        int16_t uacVolume = mCapabilities.linearToVolume(volume);
        if (setHardwareVolume(uacVolume, channel)) {
            mCurrentVolume = uacVolume;
            return true;
        } else {
            VOLUME_LOGW("UsbVolumeControl: Hardware volume set failed on channel %d, using digital", channel);
        }
    }

    return true;  // Digital volume always succeeds
}

float UsbVolumeControl::getChannelVolume(uint8_t channel) const {
    const auto* channelCaps = mCapabilities.findChannel(channel);
    if (mHardwareVolumeAvailable && channelCaps && channelCaps->hasHardwareVolume) {
        return mCapabilities.volumeToLinear(getHardwareVolume(channel));
    }
    return mDigitalVolume.load(std::memory_order_relaxed);
}

bool UsbVolumeControl::setChannelMute(uint8_t channel, bool muted) {
    std::lock_guard<std::mutex> lock(mMutex);

    // Always update digital mute
    mDigitalMute.store(muted, std::memory_order_relaxed);

    const auto* channelCaps = mCapabilities.findChannel(channel);
    const bool channelHasHardware = channelCaps && channelCaps->hasHardwareMute;

    if (mHardwareMuteAvailable && channelHasHardware) {
        if (setHardwareMute(muted, channel)) {
            return true;
        } else {
            VOLUME_LOGW("UsbVolumeControl: Hardware mute set failed on channel %d, using digital", channel);
        }
    }

    return true;  // Digital mute always succeeds
}

bool UsbVolumeControl::isChannelMuted(uint8_t channel) const {
    const auto* channelCaps = mCapabilities.findChannel(channel);
    if (mHardwareMuteAvailable && channelCaps && channelCaps->hasHardwareMute) {
        return getHardwareMute(channel);
    }
    return mDigitalMute.load(std::memory_order_relaxed);
}

float UsbVolumeControl::getVolumeDb() const {
    std::lock_guard<std::mutex> lock(mMutex);

    if (mHardwareVolumeAvailable) {
        return mCapabilities.volumeToDb(mCurrentVolume);
    } else {
        float linear = mDigitalVolume.load(std::memory_order_relaxed);
        if (linear <= 0.001f) {
            return -INFINITY;
        }
        // Convert linear to dB: 20 * log10(linear)
        return 20.0f * std::log10(linear);
    }
}

void UsbVolumeControl::populateChannelCapabilities(const UsbFeatureUnit& featureUnit,
                                                   uint8_t uacVersion) {
    mCapabilities.channels.clear();
    for (uint8_t ch = 0; ch < featureUnit.channelControls.size(); ++ch) {
        VolumeCapabilities::Channel channel;
        channel.channelNumber = ch;
        channel.hasHardwareVolume = featureUnit.hasVolumeControl(ch, uacVersion);
        channel.hasHardwareMute = featureUnit.hasMuteControl(ch, uacVersion);
        if (channel.hasHardwareVolume || channel.hasHardwareMute) {
            mCapabilities.channels.push_back(channel);
        }
    }
}

uint8_t UsbVolumeControl::preferredVolumeChannel() const {
    if (const auto* master = mCapabilities.findChannel(0);
        master && master->hasHardwareVolume) {
        return 0;
    }
    for (const auto& channel : mCapabilities.channels) {
        if (channel.hasHardwareVolume) {
            return channel.channelNumber;
        }
    }
    return 0;
}

uint8_t UsbVolumeControl::preferredMuteChannel() const {
    if (const auto* master = mCapabilities.findChannel(0);
        master && master->hasHardwareMute) {
        return 0;
    }
    for (const auto& channel : mCapabilities.channels) {
        if (channel.hasHardwareMute) {
            return channel.channelNumber;
        }
    }
    return 0;
}

} // namespace usb
} // namespace watermelon_audio
