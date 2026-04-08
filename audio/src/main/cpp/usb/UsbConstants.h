/**
 * UsbConstants.h
 *
 * USB Audio Class (UAC) constants for USB audio driver.
 *
 * References:
 * - USB Audio Class 1.0 Specification
 * - USB Audio Class 2.0 Specification
 * - Universal Serial Bus Specification 2.0
 */

#pragma once

#include <cstdint>

namespace noisypad {
namespace usb {

// ============================================================================
// USB Standard Constants
// ============================================================================

// USB Descriptor Types (USB 2.0 Spec Table 9-5)
constexpr uint8_t USB_DT_DEVICE = 0x01;
constexpr uint8_t USB_DT_CONFIG = 0x02;
constexpr uint8_t USB_DT_STRING = 0x03;
constexpr uint8_t USB_DT_INTERFACE = 0x04;
constexpr uint8_t USB_DT_ENDPOINT = 0x05;

// USB Class Codes
constexpr uint8_t USB_CLASS_AUDIO = 0x01;
constexpr uint8_t USB_CLASS_HID = 0x03;
constexpr uint8_t USB_CLASS_VENDOR_SPEC = 0xFF;

// ============================================================================
// USB Audio Class 1.0 Constants
// ============================================================================

// Audio Interface Subclass Codes (UAC 1.0 A.2)
constexpr uint8_t UAC_SUBCLASS_UNDEFINED = 0x00;
constexpr uint8_t UAC_SUBCLASS_AUDIOCONTROL = 0x01;
constexpr uint8_t UAC_SUBCLASS_AUDIOSTREAMING = 0x02;
constexpr uint8_t UAC_SUBCLASS_MIDISTREAMING = 0x03;

// Audio Interface Protocol Codes (UAC 1.0 A.3)
constexpr uint8_t UAC_PROTOCOL_UNDEFINED = 0x00;
constexpr uint8_t UAC_VERSION_1 = 0x00;  // UAC 1.0 uses protocol 0x00
constexpr uint8_t UAC_VERSION_2 = 0x20;  // UAC 2.0 uses protocol 0x20

// Audio Class-Specific Descriptor Types (UAC 1.0 A.4)
constexpr uint8_t UAC_CS_UNDEFINED = 0x20;
constexpr uint8_t UAC_CS_DEVICE = 0x21;
constexpr uint8_t UAC_CS_CONFIGURATION = 0x22;
constexpr uint8_t UAC_CS_STRING = 0x23;
constexpr uint8_t UAC_CS_INTERFACE = 0x24;
constexpr uint8_t UAC_CS_ENDPOINT = 0x25;

// Audio Class-Specific AC Interface Descriptor Subtypes (UAC 1.0 A.5)
constexpr uint8_t UAC_AC_DESCRIPTOR_UNDEFINED = 0x00;
constexpr uint8_t UAC_AC_HEADER = 0x01;
constexpr uint8_t UAC_AC_INPUT_TERMINAL = 0x02;
constexpr uint8_t UAC_AC_OUTPUT_TERMINAL = 0x03;
constexpr uint8_t UAC_AC_MIXER_UNIT = 0x04;
constexpr uint8_t UAC_AC_SELECTOR_UNIT = 0x05;
constexpr uint8_t UAC_AC_FEATURE_UNIT = 0x06;
constexpr uint8_t UAC_AC_PROCESSING_UNIT = 0x07;
constexpr uint8_t UAC_AC_EXTENSION_UNIT = 0x08;

// ============================================================================
// USB Audio Class 2.0 Additional Constants
// ============================================================================

// UAC 2.0 Audio Class-Specific AC Interface Descriptor Subtypes (UAC 2.0 A.9)
constexpr uint8_t UAC2_AC_EFFECT_UNIT = 0x07;
constexpr uint8_t UAC2_AC_PROCESSING_UNIT_V2 = 0x08;
constexpr uint8_t UAC2_AC_EXTENSION_UNIT_V2 = 0x09;
constexpr uint8_t UAC2_AC_CLOCK_SOURCE = 0x0A;
constexpr uint8_t UAC2_AC_CLOCK_SELECTOR = 0x0B;
constexpr uint8_t UAC2_AC_CLOCK_MULTIPLIER = 0x0C;
constexpr uint8_t UAC2_AC_SAMPLE_RATE_CONVERTER = 0x0D;

// UAC 2.0 Clock Source bmAttributes
constexpr uint8_t UAC2_CLOCK_SOURCE_TYPE_EXTERNAL = 0x00;
constexpr uint8_t UAC2_CLOCK_SOURCE_TYPE_INTERNAL_FIXED = 0x01;
constexpr uint8_t UAC2_CLOCK_SOURCE_TYPE_INTERNAL_VARIABLE = 0x02;
constexpr uint8_t UAC2_CLOCK_SOURCE_TYPE_INTERNAL_PROGRAMMABLE = 0x03;
constexpr uint8_t UAC2_CLOCK_SOURCE_SYNCED_TO_SOF = 0x04;  // Bit 2

// UAC 2.0 Clock Source bmControls
constexpr uint8_t UAC2_CLOCK_FREQ_CONTROL_MASK = 0x03;
constexpr uint8_t UAC2_CLOCK_VALIDITY_CONTROL_MASK = 0x0C;

// UAC 2.0 Function Category Codes (UAC 2.0 A.7)
constexpr uint8_t UAC2_FUNCTION_SUBCLASS_UNDEFINED = 0x00;
constexpr uint8_t UAC2_FUNCTION_DESKTOP_SPEAKER = 0x01;
constexpr uint8_t UAC2_FUNCTION_HOME_THEATER = 0x02;
constexpr uint8_t UAC2_FUNCTION_MICROPHONE = 0x03;
constexpr uint8_t UAC2_FUNCTION_HEADSET = 0x04;
constexpr uint8_t UAC2_FUNCTION_TELEPHONE = 0x05;
constexpr uint8_t UAC2_FUNCTION_CONVERTER = 0x06;
constexpr uint8_t UAC2_FUNCTION_SOUND_RECORDER = 0x07;
constexpr uint8_t UAC2_FUNCTION_IO_BOX = 0x08;
constexpr uint8_t UAC2_FUNCTION_MUSICAL_INSTRUMENT = 0x09;
constexpr uint8_t UAC2_FUNCTION_PRO_AUDIO = 0x0A;
constexpr uint8_t UAC2_FUNCTION_AUDIO_VIDEO = 0x0B;
constexpr uint8_t UAC2_FUNCTION_CONTROL_PANEL = 0x0C;
constexpr uint8_t UAC2_FUNCTION_OTHER = 0xFF;

// UAC 2.0 Control Selector Codes (for Control Requests)
constexpr uint8_t UAC2_CS_UNDEFINED = 0x00;
constexpr uint8_t UAC2_CS_SAM_FREQ_CONTROL = 0x01;
constexpr uint8_t UAC2_CS_CLOCK_VALID_CONTROL = 0x02;

// Audio Class-Specific AS Interface Descriptor Subtypes (UAC 1.0 A.6)
constexpr uint8_t UAC_AS_DESCRIPTOR_UNDEFINED = 0x00;
constexpr uint8_t UAC_AS_GENERAL = 0x01;
constexpr uint8_t UAC_AS_FORMAT_TYPE = 0x02;
constexpr uint8_t UAC_AS_FORMAT_SPECIFIC = 0x03;

// Audio Class-Specific Endpoint Descriptor Subtypes (UAC 1.0 A.7)
constexpr uint8_t UAC_EP_GENERAL = 0x01;

// ============================================================================
// USB Audio Terminal Types (UAC 1.0 Appendix B)
// ============================================================================

// USB Terminal Types (B.1)
constexpr uint16_t UAC_TERMINAL_USB_UNDEFINED = 0x0100;
constexpr uint16_t UAC_TERMINAL_USB_STREAMING = 0x0101;
constexpr uint16_t UAC_TERMINAL_USB_VENDOR_SPECIFIC = 0x01FF;

// Input Terminal Types (B.2)
constexpr uint16_t UAC_TERMINAL_INPUT_UNDEFINED = 0x0200;
constexpr uint16_t UAC_TERMINAL_MICROPHONE = 0x0201;
constexpr uint16_t UAC_TERMINAL_DESKTOP_MICROPHONE = 0x0202;
constexpr uint16_t UAC_TERMINAL_PERSONAL_MICROPHONE = 0x0203;
constexpr uint16_t UAC_TERMINAL_OMNI_DIR_MICROPHONE = 0x0204;
constexpr uint16_t UAC_TERMINAL_MICROPHONE_ARRAY = 0x0205;
constexpr uint16_t UAC_TERMINAL_PROCESSING_MICROPHONE_ARRAY = 0x0206;

// Output Terminal Types (B.3)
constexpr uint16_t UAC_TERMINAL_OUTPUT_UNDEFINED = 0x0300;
constexpr uint16_t UAC_TERMINAL_SPEAKER = 0x0301;
constexpr uint16_t UAC_TERMINAL_HEADPHONES = 0x0302;
constexpr uint16_t UAC_TERMINAL_HEAD_MOUNTED_DISPLAY = 0x0303;
constexpr uint16_t UAC_TERMINAL_DESKTOP_SPEAKER = 0x0304;
constexpr uint16_t UAC_TERMINAL_ROOM_SPEAKER = 0x0305;
constexpr uint16_t UAC_TERMINAL_COMMUNICATION_SPEAKER = 0x0306;
constexpr uint16_t UAC_TERMINAL_LOW_FREQ_SPEAKER = 0x0307;

// ============================================================================
// USB Audio Format Types (UAC 1.0 Section 2.2.5)
// ============================================================================

// Format Type Codes
constexpr uint8_t UAC_FORMAT_TYPE_UNDEFINED = 0x00;
constexpr uint8_t UAC_FORMAT_TYPE_I = 0x01;    // PCM formats
constexpr uint8_t UAC_FORMAT_TYPE_II = 0x02;   // Non-PCM formats (compressed)
constexpr uint8_t UAC_FORMAT_TYPE_III = 0x03;  // IEC958-style formats

// Audio Data Format Type I Codes (UAC 1.0 A.1.1)
constexpr uint16_t UAC_FORMAT_TYPE_I_UNDEFINED = 0x0000;
constexpr uint16_t UAC_FORMAT_TYPE_I_PCM = 0x0001;
constexpr uint16_t UAC_FORMAT_TYPE_I_PCM8 = 0x0002;
constexpr uint16_t UAC_FORMAT_TYPE_I_IEEE_FLOAT = 0x0003;
constexpr uint16_t UAC_FORMAT_TYPE_I_ALAW = 0x0004;
constexpr uint16_t UAC_FORMAT_TYPE_I_MULAW = 0x0005;

// ============================================================================
// USB Endpoint Attributes
// ============================================================================

// Endpoint Transfer Types (USB 2.0 bmAttributes bits 1:0)
constexpr uint8_t USB_ENDPOINT_XFER_CONTROL = 0x00;
constexpr uint8_t USB_ENDPOINT_XFER_ISOC = 0x01;
constexpr uint8_t USB_ENDPOINT_XFER_BULK = 0x02;
constexpr uint8_t USB_ENDPOINT_XFER_INT = 0x03;

// Isochronous Endpoint Sync Types (USB 2.0 bmAttributes bits 3:2)
constexpr uint8_t USB_ENDPOINT_SYNC_NONE = 0x00 << 2;
constexpr uint8_t USB_ENDPOINT_SYNC_ASYNC = 0x01 << 2;
constexpr uint8_t USB_ENDPOINT_SYNC_ADAPTIVE = 0x02 << 2;
constexpr uint8_t USB_ENDPOINT_SYNC_SYNC = 0x03 << 2;

// Isochronous Endpoint Usage Types (USB 2.0 bmAttributes bits 5:4)
constexpr uint8_t USB_ENDPOINT_USAGE_DATA = 0x00 << 4;
constexpr uint8_t USB_ENDPOINT_USAGE_FEEDBACK = 0x01 << 4;
constexpr uint8_t USB_ENDPOINT_USAGE_IMPLICIT_FB = 0x02 << 4;

// ============================================================================
// Commonly Supported Sample Rates (Hz)
// ============================================================================

constexpr int SAMPLE_RATE_44100 = 44100;
constexpr int SAMPLE_RATE_48000 = 48000;
constexpr int SAMPLE_RATE_88200 = 88200;
constexpr int SAMPLE_RATE_96000 = 96000;
constexpr int SAMPLE_RATE_176400 = 176400;
constexpr int SAMPLE_RATE_192000 = 192000;

// ============================================================================
// USB Audio Transfer Constants
// ============================================================================

// Typical USB Audio Packet Constants
constexpr int USB_AUDIO_PACKETS_PER_MS = 1;        // Full-speed: 1 packet/ms
constexpr int USB_AUDIO_PACKETS_PER_MICROFRAME = 1; // High-speed: 1 packet/125µs

// Maximum packet sizes (bytes)
constexpr int USB_AUDIO_MAX_PACKET_SIZE_FS = 1023;   // Full-speed max
constexpr int USB_AUDIO_MAX_PACKET_SIZE_HS = 3072;   // High-speed max (3x1024)

// Transfer ring buffer constants
constexpr int USB_TRANSFER_QUEUE_DEPTH = 8;          // Transfers in flight
constexpr int USB_TRANSFER_TIMEOUT_MS = 1000;        // Timeout for async transfers

// ============================================================================
// libusb Error Codes (for reference)
// ============================================================================

constexpr int LIBUSB_SUCCESS = 0;
constexpr int LIBUSB_ERROR_IO = -1;
constexpr int LIBUSB_ERROR_INVALID_PARAM = -2;
constexpr int LIBUSB_ERROR_ACCESS = -3;
constexpr int LIBUSB_ERROR_NO_DEVICE = -4;
constexpr int LIBUSB_ERROR_NOT_FOUND = -5;
constexpr int LIBUSB_ERROR_BUSY = -6;
constexpr int LIBUSB_ERROR_TIMEOUT = -7;
constexpr int LIBUSB_ERROR_OVERFLOW = -8;
constexpr int LIBUSB_ERROR_PIPE = -9;
constexpr int LIBUSB_ERROR_INTERRUPTED = -10;
constexpr int LIBUSB_ERROR_NO_MEM = -11;
constexpr int LIBUSB_ERROR_NOT_SUPPORTED = -12;

// ============================================================================
// USB Audio Feature Unit Control Constants (UAC 1.0 Section 5.2.2.3)
// ============================================================================

// Feature Unit Control Selectors (for Control Requests wValue high byte)
constexpr uint8_t UAC_FU_CONTROL_UNDEFINED = 0x00;
constexpr uint8_t UAC_FU_MUTE_CONTROL = 0x01;
constexpr uint8_t UAC_FU_VOLUME_CONTROL = 0x02;
constexpr uint8_t UAC_FU_BASS_CONTROL = 0x03;
constexpr uint8_t UAC_FU_MID_CONTROL = 0x04;
constexpr uint8_t UAC_FU_TREBLE_CONTROL = 0x05;
constexpr uint8_t UAC_FU_GRAPHIC_EQ_CONTROL = 0x06;
constexpr uint8_t UAC_FU_AGC_CONTROL = 0x07;
constexpr uint8_t UAC_FU_DELAY_CONTROL = 0x08;
constexpr uint8_t UAC_FU_BASS_BOOST_CONTROL = 0x09;
constexpr uint8_t UAC_FU_LOUDNESS_CONTROL = 0x0A;

// UAC 1.0 Feature Unit bmaControls bit positions (1 bit per control)
constexpr uint8_t UAC1_FU_MUTE_BIT = 0;        // Bit 0: Mute control
constexpr uint8_t UAC1_FU_VOLUME_BIT = 1;      // Bit 1: Volume control
constexpr uint8_t UAC1_FU_BASS_BIT = 2;        // Bit 2: Bass control
constexpr uint8_t UAC1_FU_MID_BIT = 3;         // Bit 3: Mid control
constexpr uint8_t UAC1_FU_TREBLE_BIT = 4;      // Bit 4: Treble control
constexpr uint8_t UAC1_FU_GRAPHIC_EQ_BIT = 5;  // Bit 5: Graphic EQ
constexpr uint8_t UAC1_FU_AGC_BIT = 6;         // Bit 6: AGC
constexpr uint8_t UAC1_FU_DELAY_BIT = 7;       // Bit 7: Delay
constexpr uint8_t UAC1_FU_BASS_BOOST_BIT = 8;  // Bit 8: Bass Boost
constexpr uint8_t UAC1_FU_LOUDNESS_BIT = 9;    // Bit 9: Loudness

// UAC 2.0 Feature Unit bmaControls (2 bits per control within 4-byte field)
// Each control uses 2 bits: 00=not present, 01=read-only, 11=read-write
constexpr uint32_t UAC2_FU_MUTE_MASK = 0x00000003;      // Bits 0-1
constexpr uint32_t UAC2_FU_VOLUME_MASK = 0x0000000C;    // Bits 2-3
constexpr uint32_t UAC2_FU_BASS_MASK = 0x00000030;      // Bits 4-5
constexpr uint32_t UAC2_FU_MID_MASK = 0x000000C0;       // Bits 6-7
constexpr uint32_t UAC2_FU_TREBLE_MASK = 0x00000300;    // Bits 8-9
constexpr uint32_t UAC2_FU_CONTROL_READ_ONLY = 0x01;
constexpr uint32_t UAC2_FU_CONTROL_READ_WRITE = 0x03;

// ============================================================================
// USB Audio Class Control Request Types
// ============================================================================

// Audio Class Request Codes (UAC 1.0 Table 5-1, UAC 2.0 Table 5-1)
constexpr uint8_t UAC_REQUEST_SET_CUR = 0x01;
constexpr uint8_t UAC_REQUEST_SET_MIN = 0x02;
constexpr uint8_t UAC_REQUEST_SET_MAX = 0x03;
constexpr uint8_t UAC_REQUEST_SET_RES = 0x04;
constexpr uint8_t UAC_REQUEST_GET_CUR = 0x81;
constexpr uint8_t UAC_REQUEST_GET_MIN = 0x82;
constexpr uint8_t UAC_REQUEST_GET_MAX = 0x83;
constexpr uint8_t UAC_REQUEST_GET_RES = 0x84;

// UAC 2.0 additional request codes
constexpr uint8_t UAC2_REQUEST_CUR = 0x01;
constexpr uint8_t UAC2_REQUEST_RANGE = 0x02;

// bmRequestType values for Audio Class requests
// Host-to-device, class, interface recipient
constexpr uint8_t UAC_REQUEST_TYPE_SET = 0x21;  // (ENDPOINT_OUT | TYPE_CLASS | RECIPIENT_INTERFACE)
// Device-to-host, class, interface recipient
constexpr uint8_t UAC_REQUEST_TYPE_GET = 0xA1;  // (ENDPOINT_IN | TYPE_CLASS | RECIPIENT_INTERFACE)

// ============================================================================
// USB Audio Volume Constants
// ============================================================================

// Volume value encoding (UAC 1.0 Section 5.2.2.3.3.2)
// Volume is a signed 16-bit value in 1/256 dB units
// 0x0000 = 0 dB, 0x8000 = -infinity (silence)
constexpr int16_t UAC_VOLUME_SILENCE = static_cast<int16_t>(0x8000);  // -infinity dB (silence)
constexpr int16_t UAC_VOLUME_0DB = 0x0000;                            // 0 dB (unity gain)
constexpr float UAC_VOLUME_STEP_DB = 1.0f / 256.0f;                   // 1/256 dB per unit

// Typical volume range (device-specific, query with GET_MIN/GET_MAX)
constexpr int16_t UAC_VOLUME_TYPICAL_MIN = -0x6000;  // ~ -96 dB
constexpr int16_t UAC_VOLUME_TYPICAL_MAX = 0x0000;   // 0 dB

// Mute value encoding
constexpr uint8_t UAC_MUTE_OFF = 0x00;
constexpr uint8_t UAC_MUTE_ON = 0x01;

} // namespace usb
} // namespace noisypad
