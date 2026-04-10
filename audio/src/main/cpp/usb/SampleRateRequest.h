/**
 * SampleRateRequest.h
 *
 * Pure helpers that build the bytes of a USB Audio Class sample-rate
 * SET_CUR / GET_CUR control transfer. Extracted from LibusbBackend so the
 * bitfield encoding is unit-testable without mocking libusb.
 *
 * UAC 1.0 (full-speed): endpoint-recipient class request, 3-byte payload
 *   - bmRequestType = 0x22 (H2D | Class | Endpoint) for SET
 *                   = 0xA2 (D2H | Class | Endpoint) for GET
 *   - bRequest      = SET_CUR (0x01) / GET_CUR (0x81)
 *   - wValue        = SAMPLING_FREQ_CONTROL (0x01) << 8
 *   - wIndex        = data endpoint address
 *   - wLength       = 3
 *   - data          = 24-bit little-endian sample rate
 *
 * UAC 2.0 (high-speed): interface-recipient class request, 4-byte payload
 *   - bmRequestType = 0x21 (H2D | Class | Interface) for SET
 *                   = 0xA1 (D2H | Class | Interface) for GET
 *   - bRequest      = CUR (0x01)
 *   - wValue        = CS_SAM_FREQ_CONTROL (0x01) << 8
 *   - wIndex        = (clockSourceId << 8) | controlInterfaceNumber
 *   - wLength       = 4
 *   - data          = 32-bit little-endian sample rate
 */

#pragma once

#include "UsbConstants.h"
#include <array>
#include <cstdint>

namespace watermelon_audio {
namespace usb {

/**
 * Encoded UAC 1.0 SET_CUR sample-rate request bytes.
 */
struct Uac1SampleRateRequest {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    std::array<uint8_t, 3> payload;
};

/**
 * Encoded UAC 2.0 SET_CUR sample-rate request bytes.
 */
struct Uac2SampleRateRequest {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    std::array<uint8_t, 4> payload;
};

/**
 * Build a UAC 1.0 SET_CUR sampling-frequency control request targeting
 * the given audio data endpoint.
 *
 * @param endpointAddress Full endpoint address (with direction bit).
 * @param sampleRateHz    Desired sample rate in Hz.
 */
inline Uac1SampleRateRequest buildUac1SetSampleRateRequest(
    uint8_t endpointAddress,
    uint32_t sampleRateHz)
{
    return Uac1SampleRateRequest{
        /*bmRequestType*/ UAC_REQUEST_TYPE_SET_ENDPOINT,
        /*bRequest*/      UAC_REQUEST_SET_CUR,
        /*wValue*/        static_cast<uint16_t>(
                              static_cast<uint16_t>(UAC1_EP_SAMPLING_FREQ_CONTROL) << 8),
        /*wIndex*/        endpointAddress,
        /*payload*/       {{
            static_cast<uint8_t>(sampleRateHz & 0xff),
            static_cast<uint8_t>((sampleRateHz >> 8) & 0xff),
            static_cast<uint8_t>((sampleRateHz >> 16) & 0xff),
        }},
    };
}

/**
 * Build a UAC 1.0 GET_CUR sampling-frequency request for verification.
 */
inline Uac1SampleRateRequest buildUac1GetSampleRateRequest(uint8_t endpointAddress) {
    return Uac1SampleRateRequest{
        UAC_REQUEST_TYPE_GET_ENDPOINT,
        UAC_REQUEST_GET_CUR,
        static_cast<uint16_t>(
            static_cast<uint16_t>(UAC1_EP_SAMPLING_FREQ_CONTROL) << 8),
        endpointAddress,
        {{0, 0, 0}},
    };
}

/**
 * Build a UAC 2.0 SET_CUR sample-frequency control request targeting the
 * given clock source on the given control interface.
 *
 * @param clockSourceId        bClockID of the target clock source.
 * @param controlInterfaceNum  bInterfaceNumber of the AudioControl interface.
 * @param sampleRateHz         Desired sample rate in Hz.
 */
inline Uac2SampleRateRequest buildUac2SetSampleRateRequest(
    uint8_t clockSourceId,
    uint8_t controlInterfaceNum,
    uint32_t sampleRateHz)
{
    return Uac2SampleRateRequest{
        /*bmRequestType*/ UAC_REQUEST_TYPE_SET,  // 0x21
        /*bRequest*/      UAC2_REQUEST_CUR,
        /*wValue*/        static_cast<uint16_t>(
                              static_cast<uint16_t>(UAC2_CS_SAM_FREQ_CONTROL) << 8),
        /*wIndex*/        static_cast<uint16_t>(
                              (static_cast<uint16_t>(clockSourceId) << 8) |
                              static_cast<uint16_t>(controlInterfaceNum)),
        /*payload*/       {{
            static_cast<uint8_t>(sampleRateHz & 0xff),
            static_cast<uint8_t>((sampleRateHz >> 8) & 0xff),
            static_cast<uint8_t>((sampleRateHz >> 16) & 0xff),
            static_cast<uint8_t>((sampleRateHz >> 24) & 0xff),
        }},
    };
}

/**
 * Build a UAC 2.0 GET_CUR sample-frequency request for verification.
 */
inline Uac2SampleRateRequest buildUac2GetSampleRateRequest(
    uint8_t clockSourceId,
    uint8_t controlInterfaceNum)
{
    return Uac2SampleRateRequest{
        UAC_REQUEST_TYPE_GET,  // 0xA1
        UAC2_REQUEST_CUR,
        static_cast<uint16_t>(
            static_cast<uint16_t>(UAC2_CS_SAM_FREQ_CONTROL) << 8),
        static_cast<uint16_t>(
            (static_cast<uint16_t>(clockSourceId) << 8) |
            static_cast<uint16_t>(controlInterfaceNum)),
        {{0, 0, 0, 0}},
    };
}

/**
 * Decode a 24-bit little-endian sample rate from a UAC 1.0 GET_CUR response.
 */
inline uint32_t decodeUac1SampleRateResponse(const std::array<uint8_t, 3>& bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16);
}

/**
 * Decode a 32-bit little-endian sample rate from a UAC 2.0 GET_CUR response.
 */
inline uint32_t decodeUac2SampleRateResponse(const std::array<uint8_t, 4>& bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

}  // namespace usb
}  // namespace watermelon_audio
