// Stage 1 — UsbDescriptorParser feedback-endpoint detection tests
//
// Builds a synthetic USB Audio Class 1.0 configuration descriptor in
// memory and verifies that the parser:
//
//   1. Routes the data endpoint (iso OUT, usage type Data) to
//      `UsbStreamingInterface::dataEndpoint`.
//   2. Routes the feedback endpoint (iso IN, usage type Feedback) to
//      `UsbStreamingInterface::feedbackEndpoint`.
//   3. Marks the latter with `isImplicit == false`.
//
// The fixtures are intentionally hand-built (no real device) so the test
// is deterministic and travels with the source. Each byte is annotated
// with its USB spec field name.

#include <gtest/gtest.h>

#include "../UsbDescriptorParser.h"
#include "../UsbAudioTypes.h"
#include "../UsbConstants.h"

#include <vector>
#include <cstdint>

using namespace watermelon_audio::usb;

namespace {

// Build a UAC 1.0 configuration descriptor with one audio function:
//
//   Config (9 bytes)
//   ├─ Interface 0, Alt 0  (AudioControl, no endpoints)
//   │  └─ AC Header (CS_INTERFACE, header subtype)
//   ├─ Interface 1, Alt 0  (AudioStreaming, zero endpoints — idle alt)
//   └─ Interface 1, Alt 1  (AudioStreaming, 2 endpoints)
//      ├─ AS General (CS_INTERFACE, AS_GENERAL subtype)
//      ├─ Format Type I (CS_INTERFACE, FORMAT_TYPE subtype)
//      ├─ Endpoint 0x01    (data OUT iso, sync=async, usage=data)
//      ├─ Audio EP descriptor (CS_ENDPOINT, EP_GENERAL subtype)
//      └─ Endpoint 0x82    (feedback IN iso, sync=none, usage=feedback)
//
// Total length is computed dynamically from the byte vector.
std::vector<uint8_t> buildUac1AsyncConfigDescriptor() {
    std::vector<uint8_t> d;

    // ----- Configuration descriptor -----
    // bLength=9, bDescriptorType=CONFIG (0x02)
    // wTotalLength filled in at the end
    // bNumInterfaces=2, bConfigurationValue=1, iConfiguration=0
    // bmAttributes=0x80 (bus powered), bMaxPower=50 (100 mA)
    const size_t configHeaderStart = d.size();
    d.insert(d.end(), {
        0x09, 0x02,
        0x00, 0x00,         // wTotalLength placeholder
        0x02,               // bNumInterfaces
        0x01,               // bConfigurationValue
        0x00,               // iConfiguration
        0x80,               // bmAttributes
        0x32,               // bMaxPower
    });

    // ----- Interface 0, Alt 0: AudioControl -----
    d.insert(d.end(), {
        0x09, 0x04,         // bLength=9, bDescriptorType=INTERFACE
        0x00,               // bInterfaceNumber
        0x00,               // bAlternateSetting
        0x00,               // bNumEndpoints
        0x01,               // bInterfaceClass = AUDIO
        0x01,               // bInterfaceSubClass = AUDIOCONTROL
        0x00,               // bInterfaceProtocol = UAC1
        0x00,               // iInterface
    });

    // AC Header descriptor (UAC 1.0 Table 4-2). Minimum length 8 bytes plus
    // 1 byte per AudioStreaming interface in the collection. We have 1.
    d.insert(d.end(), {
        0x09,               // bLength = 9
        0x24,               // bDescriptorType = CS_INTERFACE
        0x01,               // bDescriptorSubtype = HEADER
        0x00, 0x01,         // bcdADC = 0x0100 (UAC 1.0)
        0x1E, 0x00,         // wTotalLength of the AC interface descriptors
        0x01,               // bInCollection
        0x01,               // baInterfaceNr(1)
    });

    // ----- Interface 1, Alt 0: AudioStreaming, zero endpoints -----
    d.insert(d.end(), {
        0x09, 0x04,
        0x01,               // bInterfaceNumber
        0x00,               // bAlternateSetting
        0x00,               // bNumEndpoints
        0x01,               // bInterfaceClass = AUDIO
        0x02,               // bInterfaceSubClass = AUDIOSTREAMING
        0x00,               // bInterfaceProtocol = UAC1
        0x00,
    });

    // ----- Interface 1, Alt 1: AudioStreaming, 2 endpoints -----
    d.insert(d.end(), {
        0x09, 0x04,
        0x01,               // bInterfaceNumber
        0x01,               // bAlternateSetting
        0x02,               // bNumEndpoints
        0x01,               // bInterfaceClass = AUDIO
        0x02,               // bInterfaceSubClass = AUDIOSTREAMING
        0x00,
        0x00,
    });

    // AS General descriptor (UAC 1.0 Table 4-19)
    d.insert(d.end(), {
        0x07,               // bLength = 7
        0x24,               // bDescriptorType = CS_INTERFACE
        0x01,               // bDescriptorSubtype = AS_GENERAL
        0x01,               // bTerminalLink (terminal id)
        0x01,               // bDelay
        0x01, 0x00,         // wFormatTag = PCM
    });

    // Format Type I descriptor: 2 channels, 24 bits, 1 discrete rate (48000)
    // (UAC 1.0 Section 2.2.5; bSamFreqType=1)
    d.insert(d.end(), {
        0x0B,               // bLength = 11
        0x24,               // bDescriptorType = CS_INTERFACE
        0x02,               // bDescriptorSubtype = FORMAT_TYPE
        0x01,               // bFormatType = TYPE_I
        0x02,               // bNrChannels
        0x03,               // bSubframeSize (3 bytes per sample)
        0x18,               // bBitResolution (24 bits)
        0x01,               // bSamFreqType = 1 discrete rate
        0x80, 0xBB, 0x00,   // 48000 Hz, 24-bit LE
    });

    // Endpoint descriptor: data endpoint OUT iso, async, usage=data
    // bEndpointAddress = 0x01 (OUT, ep 1)
    // bmAttributes:
    //   bits 1:0 = 01 (iso)
    //   bits 3:2 = 01 (async sync type)
    //   bits 5:4 = 00 (data usage)
    // → 0x05
    //
    // Standard endpoint descriptor is 7 bytes (USB 9.6.6 Table 9-13). The
    // audio class adds an optional 2-byte extension (bRefresh, bSynchAddress)
    // bringing it to 9 bytes — but we declare 7 here and the parser reads
    // exactly that many.
    d.insert(d.end(), {
        0x07,               // bLength = 7 (standard EP descriptor, no audio extension)
        0x05,               // bDescriptorType = ENDPOINT
        0x01,               // bEndpointAddress = 0x01 (OUT, ep 1)
        0x05,               // bmAttributes = iso/async/data
        0xC0, 0x00,         // wMaxPacketSize = 192
        0x01,               // bInterval = 1ms
    });

    // Audio Endpoint descriptor (UAC 1.0 Table 4-21) — class-specific EP
    // Not strictly required for the parse path under test, but real
    // devices include it and the parser handles CS_ENDPOINT below.
    d.insert(d.end(), {
        0x07,               // bLength
        0x25,               // bDescriptorType = CS_ENDPOINT
        0x01,               // bDescriptorSubtype = EP_GENERAL
        0x00,               // bmAttributes
        0x00,               // bLockDelayUnits
        0x00, 0x00,         // wLockDelay
    });

    // Endpoint descriptor: feedback endpoint IN iso, no sync (it IS the sync)
    // bEndpointAddress = 0x82 (IN, ep 2)
    // bmAttributes:
    //   bits 1:0 = 01 (iso)
    //   bits 3:2 = 00 (no sync type — it carries sync)
    //   bits 5:4 = 01 (feedback usage)
    // → 0x11
    d.insert(d.end(), {
        0x07,               // bLength = 7 (standard EP descriptor)
        0x05,               // bDescriptorType = ENDPOINT
        0x82,               // bEndpointAddress = 0x82 (IN, ep 2)
        0x11,               // bmAttributes = iso/none/feedback
        0x03, 0x00,         // wMaxPacketSize = 3 (UAC1 feedback length)
        0x01,               // bInterval
    });

    // Patch wTotalLength now that we know the size.
    const uint16_t totalLen = static_cast<uint16_t>(d.size() - configHeaderStart);
    d[configHeaderStart + 2] = static_cast<uint8_t>(totalLen & 0xff);
    d[configHeaderStart + 3] = static_cast<uint8_t>((totalLen >> 8) & 0xff);

    return d;
}

// Variant of the above that uses an implicit feedback data endpoint
// (usage type bits = 0x20) instead of an explicit feedback endpoint.
std::vector<uint8_t> buildUac1ImplicitFeedbackConfigDescriptor() {
    std::vector<uint8_t> d;

    const size_t configHeaderStart = d.size();
    d.insert(d.end(), {
        0x09, 0x02,
        0x00, 0x00,
        0x02, 0x01, 0x00, 0x80, 0x32,
    });

    d.insert(d.end(), {
        0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    });
    d.insert(d.end(), {
        0x09, 0x24, 0x01, 0x00, 0x01, 0x1E, 0x00, 0x01, 0x01,
    });

    d.insert(d.end(), {
        0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    });
    d.insert(d.end(), {
        0x09, 0x04, 0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    });

    d.insert(d.end(), {
        0x07, 0x24, 0x01, 0x01, 0x01, 0x01, 0x00,
    });
    d.insert(d.end(), {
        0x0B, 0x24, 0x02, 0x01, 0x02, 0x03, 0x18, 0x01,
        0x80, 0xBB, 0x00,
    });

    // Single data endpoint OUT iso, async, usage=implicit feedback (bits 5:4 = 10)
    // → 0x05 | 0x20 = 0x25
    // bLength=7 to match the standard endpoint descriptor we actually emit
    // (no audio class extension trailing bytes).
    d.insert(d.end(), {
        0x07, 0x05,
        0x01,               // OUT ep 1
        0x25,               // iso / async / implicit feedback
        0xC0, 0x00,
        0x01,
    });

    const uint16_t totalLen = static_cast<uint16_t>(d.size() - configHeaderStart);
    d[configHeaderStart + 2] = static_cast<uint8_t>(totalLen & 0xff);
    d[configHeaderStart + 3] = static_cast<uint8_t>((totalLen >> 8) & 0xff);
    return d;
}

UsbDeviceInfo makeFakeDeviceInfo() {
    UsbDeviceInfo info;
    info.vendorId = 0x1234;
    info.productId = 0x5678;
    info.fileDescriptor = 1;  // arbitrary non-negative for isValid()
    info.product = "Synthetic UAC1 Async Device";
    return info;
}

}  // namespace

TEST(UsbDescriptorParserFeedbackTest, ExplicitFeedbackEndpointIsRoutedToFeedbackField) {
    auto descriptor = buildUac1AsyncConfigDescriptor();
    UsbDescriptorParser parser;
    auto result = parser.parse(descriptor.data(), descriptor.size(), makeFakeDeviceInfo());
    ASSERT_TRUE(result.has_value()) << "parse() failed: " << parser.getLastError();

    const auto& device = *result;
    ASSERT_EQ(device.uacVersion, 1);
    ASSERT_EQ(device.playbackInterfaces.size(), 1u)
        << "Expected exactly one playback altsetting (alt 1) — got "
        << device.playbackInterfaces.size();

    const auto& iface = device.playbackInterfaces[0];
    EXPECT_EQ(iface.interfaceNumber, 1);
    EXPECT_EQ(iface.alternateSetting, 1);

    // Data endpoint
    EXPECT_EQ(iface.dataEndpoint.address, 0x01);  // OUT ep 1
    EXPECT_TRUE(iface.dataEndpoint.isOutput());
    EXPECT_TRUE(iface.dataEndpoint.isIsochronous());
    EXPECT_TRUE(iface.dataEndpoint.isAsync());

    // Feedback endpoint must be present and pointing at the IN side
    ASSERT_TRUE(iface.feedbackEndpoint.has_value())
        << "Feedback endpoint was not detected — the parser fix is missing.";
    const auto& fb = *iface.feedbackEndpoint;
    EXPECT_FALSE(fb.isImplicit);
    EXPECT_EQ(fb.endpoint.address, 0x82);  // IN ep 2
    EXPECT_TRUE(fb.endpoint.isInput());
    EXPECT_TRUE(fb.endpoint.isIsochronous());
    // Usage type bits must be feedback (0x10)
    EXPECT_EQ(fb.endpoint.attributes & 0x30, USB_ENDPOINT_USAGE_FEEDBACK);
}

TEST(UsbDescriptorParserFeedbackTest, ImplicitFeedbackOnDataEndpointIsMarked) {
    auto descriptor = buildUac1ImplicitFeedbackConfigDescriptor();
    UsbDescriptorParser parser;
    auto result = parser.parse(descriptor.data(), descriptor.size(), makeFakeDeviceInfo());
    ASSERT_TRUE(result.has_value()) << "parse() failed: " << parser.getLastError();

    const auto& device = *result;
    ASSERT_EQ(device.playbackInterfaces.size(), 1u);
    const auto& iface = device.playbackInterfaces[0];

    // Data endpoint is the only endpoint and IS the implicit feedback source
    EXPECT_EQ(iface.dataEndpoint.address, 0x01);
    EXPECT_EQ(iface.dataEndpoint.attributes & 0x30, USB_ENDPOINT_USAGE_IMPLICIT_FB);

    ASSERT_TRUE(iface.feedbackEndpoint.has_value());
    EXPECT_TRUE(iface.feedbackEndpoint->isImplicit);
    EXPECT_EQ(iface.feedbackEndpoint->endpoint.address, 0x01);
}

// 7-byte endpoint descriptors must leave refresh/synchAddress at 0 and not
// fabricate a phantom feedback endpoint from stale bytes (0.3 test 4).
TEST(UsbDescriptorParserFeedbackTest, SevenByteEndpointHasNoRefreshOrSynch) {
    auto descriptor = buildUac1AsyncConfigDescriptor();
    UsbDescriptorParser parser;
    auto result = parser.parse(descriptor.data(), descriptor.size(), makeFakeDeviceInfo());
    ASSERT_TRUE(result.has_value());
    const auto& iface = result->playbackInterfaces[0];
    EXPECT_EQ(iface.dataEndpoint.refresh, 0);
    EXPECT_EQ(iface.dataEndpoint.synchAddress, 0);
}

// ---------------------------------------------------------------------------
// 0.3 — UAC1 legacy feedback by bSynchAddress + data-EP clobbering regression
// ---------------------------------------------------------------------------

namespace {

// UAC1 legacy playback altsetting:
//   - data EP OUT 0x01, 9-byte descriptor, usage=data(00), bRefresh=4,
//     bSynchAddress=0x81
//   - sync EP IN 0x81, iso, usage=data(00) (NOT the modern feedback usage)
// `reverseOrder` emits the IN endpoint before the OUT one to prove the
// classification is order-independent.
std::vector<uint8_t> buildUac1LegacySynchConfigDescriptor(bool reverseOrder) {
    std::vector<uint8_t> d;
    const size_t cfg = d.size();
    d.insert(d.end(), {0x09, 0x02, 0x00, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32});

    // IF0 AudioControl + AC header
    d.insert(d.end(), {0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00});
    d.insert(d.end(), {0x09, 0x24, 0x01, 0x00, 0x01, 0x1E, 0x00, 0x01, 0x01});

    // IF1 Alt0 idle
    d.insert(d.end(), {0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00});
    // IF1 Alt1 with 2 endpoints
    d.insert(d.end(), {0x09, 0x04, 0x01, 0x01, 0x02, 0x01, 0x02, 0x00, 0x00});
    // AS General + Format Type I
    d.insert(d.end(), {0x07, 0x24, 0x01, 0x01, 0x01, 0x01, 0x00});
    d.insert(d.end(), {0x0B, 0x24, 0x02, 0x01, 0x02, 0x03, 0x18, 0x01,
                       0x80, 0xBB, 0x00});

    // Data EP OUT 0x01, 9-byte (bRefresh=4 @7, bSynchAddress=0x81 @8),
    // bmAttributes = iso/async/data = 0x05, maxPkt=192.
    const std::vector<uint8_t> dataEp = {
        0x09, 0x05, 0x01, 0x05, 0xC0, 0x00, 0x01, 0x04, 0x81};
    // Legacy sync EP IN 0x81, 7-byte, iso/none/data = 0x01, maxPkt=3.
    const std::vector<uint8_t> syncEp = {
        0x07, 0x05, 0x81, 0x01, 0x03, 0x00, 0x01};

    if (reverseOrder) {
        d.insert(d.end(), syncEp.begin(), syncEp.end());
        d.insert(d.end(), dataEp.begin(), dataEp.end());
    } else {
        d.insert(d.end(), dataEp.begin(), dataEp.end());
        d.insert(d.end(), syncEp.begin(), syncEp.end());
    }

    const uint16_t totalLen = static_cast<uint16_t>(d.size() - cfg);
    d[cfg + 2] = static_cast<uint8_t>(totalLen & 0xff);
    d[cfg + 3] = static_cast<uint8_t>((totalLen >> 8) & 0xff);
    return d;
}

void expectLegacyResolved(const std::vector<uint8_t>& descriptor) {
    UsbDescriptorParser parser;
    auto result = parser.parse(descriptor.data(), descriptor.size(), makeFakeDeviceInfo());
    ASSERT_TRUE(result.has_value()) << "parse() failed: " << parser.getLastError();
    ASSERT_EQ(result->playbackInterfaces.size(), 1u);
    const auto& iface = result->playbackInterfaces[0];

    // Data EP must be the OUT endpoint — NOT clobbered by the IN sync EP.
    EXPECT_EQ(iface.dataEndpoint.address, 0x01);
    EXPECT_TRUE(iface.dataEndpoint.isOutput());
    EXPECT_EQ(iface.dataEndpoint.synchAddress, 0x81);
    EXPECT_EQ(iface.dataEndpoint.refresh, 4);

    // Feedback EP resolved from bSynchAddress, explicit, at 0x81.
    ASSERT_TRUE(iface.feedbackEndpoint.has_value());
    EXPECT_FALSE(iface.feedbackEndpoint->isImplicit);
    EXPECT_EQ(iface.feedbackEndpoint->endpoint.address, 0x81);
    EXPECT_TRUE(iface.feedbackEndpoint->endpoint.isInput());
    EXPECT_EQ(iface.feedbackEndpoint->endpoint.refresh, 4);  // inherited from data EP
}

}  // namespace

TEST(UsbDescriptorParserFeedbackTest, LegacyUac1SynchAddressInOrder) {
    expectLegacyResolved(buildUac1LegacySynchConfigDescriptor(/*reverseOrder=*/false));
}

TEST(UsbDescriptorParserFeedbackTest, LegacyUac1SynchAddressReverseOrderNoClobber) {
    // IN sync endpoint appears before the OUT data endpoint in the descriptor.
    expectLegacyResolved(buildUac1LegacySynchConfigDescriptor(/*reverseOrder=*/true));
}
