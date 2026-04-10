// Stage 1 — SampleRateRequest builder unit tests
//
// These verify the exact bitfields the helpers in SampleRateRequest.h
// produce. The whole point of extracting them as pure builders was to
// pin the encoding without needing to mock libusb_control_transfer.
//
// References:
//  - UAC 1.0 spec, 5.2.3.2.3.1 (Sampling Frequency Control)
//  - UAC 2.0 spec, 5.2.5.1 (Clock Source Sampling Frequency Control)
//  - USB 2.0 spec, 9.3 (Standard Device Requests / bmRequestType layout)

#include <gtest/gtest.h>

#include "../SampleRateRequest.h"

using namespace watermelon_audio::usb;

namespace {

constexpr uint8_t kEndpointAddress = 0x01;  // OUT ep 1, typical playback
constexpr uint8_t kClockSourceId = 0x09;
constexpr uint8_t kControlInterface = 0x00;

}  // namespace

TEST(SampleRateRequestTest, Uac1Set48000HasCorrectBitfields) {
    auto req = buildUac1SetSampleRateRequest(kEndpointAddress, 48000);

    // bmRequestType = 0x22
    //   D7  = 0   host-to-device
    //   D6:5 = 01 class
    //   D4:0 = 00010 endpoint recipient
    EXPECT_EQ(req.bmRequestType, 0x22);

    // bRequest = SET_CUR (0x01)
    EXPECT_EQ(req.bRequest, 0x01);

    // wValue = SAMPLING_FREQ_CONTROL << 8 = 0x0100
    EXPECT_EQ(req.wValue, 0x0100);

    // wIndex = endpoint address
    EXPECT_EQ(req.wIndex, kEndpointAddress);

    // payload = 48000 in 24-bit LE = 0x00BB80
    //   0x80 = 128, 0xBB = 187, 0x00 = 0
    //   0x80 + (0xBB << 8) = 128 + 47872 = 48000 ✓
    EXPECT_EQ(req.payload[0], 0x80);
    EXPECT_EQ(req.payload[1], 0xBB);
    EXPECT_EQ(req.payload[2], 0x00);
}

TEST(SampleRateRequestTest, Uac1Set96000FitsIn24Bits) {
    auto req = buildUac1SetSampleRateRequest(kEndpointAddress, 96000);
    // 96000 = 0x017700
    EXPECT_EQ(req.payload[0], 0x00);
    EXPECT_EQ(req.payload[1], 0x77);
    EXPECT_EQ(req.payload[2], 0x01);
}

TEST(SampleRateRequestTest, Uac1Set192000FitsIn24Bits) {
    auto req = buildUac1SetSampleRateRequest(kEndpointAddress, 192000);
    // 192000 = 0x02EE00
    EXPECT_EQ(req.payload[0], 0x00);
    EXPECT_EQ(req.payload[1], 0xEE);
    EXPECT_EQ(req.payload[2], 0x02);
}

TEST(SampleRateRequestTest, Uac1GetIsTheCorrectMirrorOfSet) {
    auto getReq = buildUac1GetSampleRateRequest(kEndpointAddress);
    // Same wValue + wIndex, but bmRequestType has the direction bit flipped
    // (D7 = 1) and bRequest is GET_CUR (0x81).
    EXPECT_EQ(getReq.bmRequestType, 0xA2);  // 0x22 | 0x80
    EXPECT_EQ(getReq.bRequest, 0x81);
    EXPECT_EQ(getReq.wValue, 0x0100);
    EXPECT_EQ(getReq.wIndex, kEndpointAddress);
}

TEST(SampleRateRequestTest, Uac2Set48000HasCorrectBitfields) {
    auto req = buildUac2SetSampleRateRequest(kClockSourceId, kControlInterface, 48000);

    // bmRequestType = 0x21
    //   D7   = 0   host-to-device
    //   D6:5 = 01  class
    //   D4:0 = 00001 interface recipient
    EXPECT_EQ(req.bmRequestType, 0x21);

    // bRequest = CUR (0x01)
    EXPECT_EQ(req.bRequest, 0x01);

    // wValue = CS_SAM_FREQ_CONTROL << 8 = 0x0100
    EXPECT_EQ(req.wValue, 0x0100);

    // wIndex layout: high byte = clockSourceId, low byte = controlInterface
    //   0x09 << 8 = 0x0900, OR 0x00 = 0x0900
    EXPECT_EQ(req.wIndex, static_cast<uint16_t>((kClockSourceId << 8) | kControlInterface));

    // payload = 48000 as 32-bit LE = 0x0000BB80
    EXPECT_EQ(req.payload[0], 0x80);
    EXPECT_EQ(req.payload[1], 0xBB);
    EXPECT_EQ(req.payload[2], 0x00);
    EXPECT_EQ(req.payload[3], 0x00);
}

TEST(SampleRateRequestTest, Uac2WIndexCombinesClockIdAndInterfaceCorrectly) {
    // clockSrc 0x05, controlIface 0x02 → wIndex 0x0502
    auto req = buildUac2SetSampleRateRequest(0x05, 0x02, 96000);
    EXPECT_EQ(req.wIndex, 0x0502);

    // clockSrc 0xAB, controlIface 0xCD → wIndex 0xABCD
    auto req2 = buildUac2SetSampleRateRequest(0xAB, 0xCD, 48000);
    EXPECT_EQ(req2.wIndex, 0xABCD);
}

TEST(SampleRateRequestTest, Uac2GetIsTheCorrectMirrorOfSet) {
    auto getReq = buildUac2GetSampleRateRequest(kClockSourceId, kControlInterface);
    EXPECT_EQ(getReq.bmRequestType, 0xA1);  // 0x21 | 0x80
    EXPECT_EQ(getReq.bRequest, 0x01);
    EXPECT_EQ(getReq.wValue, 0x0100);
    EXPECT_EQ(getReq.wIndex, static_cast<uint16_t>((kClockSourceId << 8) | kControlInterface));
}

TEST(SampleRateRequestTest, Uac1DecoderInvertsEncoder) {
    auto req = buildUac1SetSampleRateRequest(kEndpointAddress, 88200);
    EXPECT_EQ(decodeUac1SampleRateResponse(req.payload), 88200u);
}

TEST(SampleRateRequestTest, Uac2DecoderInvertsEncoder) {
    auto req = buildUac2SetSampleRateRequest(kClockSourceId, kControlInterface, 192000);
    EXPECT_EQ(decodeUac2SampleRateResponse(req.payload), 192000u);
}

TEST(SampleRateRequestTest, Uac2DecoderHandlesRatesAbove24BitRange) {
    // 384000 doesn't fit in 24 bits — confirms UAC2 path uses the full 32.
    auto req = buildUac2SetSampleRateRequest(kClockSourceId, kControlInterface, 384000);
    // 384000 = 0x0005DC00
    EXPECT_EQ(req.payload[0], 0x00);
    EXPECT_EQ(req.payload[1], 0xDC);
    EXPECT_EQ(req.payload[2], 0x05);
    EXPECT_EQ(req.payload[3], 0x00);
    EXPECT_EQ(decodeUac2SampleRateResponse(req.payload), 384000u);
}
