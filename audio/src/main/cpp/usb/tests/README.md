# USB Audio Subsystem Unit Tests

Host-side gtest suite for the USB audio backend (stage 1 — foundations).

These tests run on the developer machine (x86_64 Linux/macOS/Windows), not on
an Android device. They cover:

- `ClockController` — UAC1 (3-byte 10.14) and UAC2 (4-byte 16.16) feedback
  parsing, drift convergence under simulated noise.
- `SampleRateRequest` builders — exact bitfield encoding of UAC1 endpoint
  requests and UAC2 clock-source-interface requests.
- `UsbDescriptorParser` — feedback endpoint detection from a hand-built
  configuration descriptor (golden fixture).

## Build & run

```bash
cd audio/src/main/cpp/usb/tests
mkdir -p build && cd build
cmake ..
make -j$(nproc)
ctest --output-on-failure
```

The first build downloads Google Test 1.15.2 via `FetchContent`. Subsequent
builds reuse the cached copy in `build/_deps/`.

## What the tests do NOT cover

The tests deliberately avoid pulling in libusb, Oboe, Android, or any of the
larger backend modules. Anything that touches `libusb_control_transfer`
directly is unit-testable through the pure helpers in `SampleRateRequest.h`
(builders) and `ClockController.h` (feedback parsing). Wiring those helpers
into a real device is exercised by the on-device `RATE_NEGOTIATION_SWEEP`
preset in `UsbAudioTestRunner.kt`.
