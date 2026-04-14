# USB Audio Logging Conventions

Status: **active** — introduced 2026-04-14 as part of the stage 3 / first-playback
bug investigation cleanup.

All diagnostic log lines produced by the USB audio pipeline — both on the C++
side (`watermelon-audio`) and the Kotlin/Android side (NoisyPad, other
consumers) — share a single logcat **tag**:

```
WMA_AUDIT
```

This replaces the previous `INPUTFX_DIAG` tag (native) and the ad-hoc
per-class tags Kotlin was using for USB flow logs. Unifying under a single
tag means you can capture the entire audit trail with one `adb logcat`
invocation and grep by sub-key from there.

---

## Quick reference — `adb logcat` filters

Capture everything (most common — everything USB-audio flow-related):

```bash
adb logcat -s WMA_AUDIT:I
```

Capture plus the per-class error/warn streams that aren't audit-tagged
(library lifecycle, parser, transfer manager errors):

```bash
adb logcat -s WMA_AUDIT:I LibusbBackend:W UsbTransferManager:W UsbDescriptorParser:W AudioEngine:W
```

Filter by sub-key (examples):

```bash
# DSP thread diagnostics (fires every ~1.6 s from LibusbBackend::dspThreadFunc)
adb logcat -s WMA_AUDIT:I | grep "USB_DSP"

# Audio callback entry (reported from AudioEngine::onAudioReady)
adb logcat -s WMA_AUDIT:I | grep "USB_CB"

# INPUT_FX direct path output stats
adb logcat -s WMA_AUDIT:I | grep "USB_DIRECT_OUT"

# Stage 2 capability snapshot (device discovery)
adb logcat -s WMA_AUDIT:I | grep "USB_STAGE2"

# Stage 3 clock source rate negotiation
adb logcat -s WMA_AUDIT:I | grep "Rate negotiation"

# Mode transitions (chaos_pad ↔ input_fx ↔ mix)
adb logcat -s WMA_AUDIT:I | grep -E "(SET_MODE|START_USB_FADE)"

# All the "first playback" investigation (combine DSP + CB + direct out + fade)
adb logcat -s WMA_AUDIT:I | grep -E "(USB_DSP|USB_CB|USB_DIRECT_OUT|START_USB_FADE)"

# UGREEN "loud noise after mode switch" investigation (adds the output peak meter)
adb logcat -s WMA_AUDIT:I | grep -E "(USB_DSP|Rate negotiation|SET_MODE)"
```

Shortcut: save a filter profile to `~/.logcat_wma_audit.txt` for
`adb logcat` to reuse.

---

## Sub-keys currently in use

Each WMA_AUDIT log line starts with one of these string prefixes in its
message body. New audit logs should pick one of these (or propose a new
one in this doc) rather than inventing free-form prefixes.

| Sub-key | Emitted from | What it covers |
|---|---|---|
| `USB_DSP` | `LibusbBackend::dspThreadFunc` | Periodic DSP thread health snapshot: input ring fill, read ok/fail, input peak, **output peak** (added for UGREEN noise investigation), stream mode, frames per block. One line every 300 callbacks (~1.6 s at 48 kHz / 256 frame block). |
| `USB_CB` | `AudioEngine::onAudioReady` | Per-callback decision log (mode, oscillator enabled, input/output pointers, fade state, input peak). Fires periodically, not every block. |
| `USB_DIRECT_OUT` | `AudioEngine::onAudioReady` (INPUT_FX branch) | Output gain ramp + fade + master vol + output peak + effect count. Only fires in INPUT_FX mode — absence is a signal that we're NOT in the direct input-to-output path. |
| `USB_STAGE2` | `MainViewModel.updateUsbCapabilities` (NoisyPad) | Capability snapshot dump when a device is connected: UAC version, altsetting count, effective rates/depths, clock sources, per-altsetting details. |
| `START_USB_FADE` | `AudioEngine::start` (BackendManager branch) | Fade-in envelope kick-off. `sampleRate=X, fadeTimeMs=Y` — marks the moment the engine considers itself ready to produce audible output. |
| `SET_MODE_INPUT_FX` | `AudioEngine` mode transition | Mode change to INPUT_FX, indicates whether USB backend is active. |
| `Rate negotiation` | `LibusbBackend::configureSampleRate` | Per-clock-source SET_CUR (or skipped-because-already-at-target). Includes `clockSrc=N req=X actual=Y`. |
| `USB FEED` | `AudioEngine::onAudioReady` (MIX / vocoder path) | USB input samples being fed into InputNode for vocoder modulator or MIX mode. |
| `USB MIX/VOCODER` | Same | Count of frames passed to InputNode. |

Tags used *outside* WMA_AUDIT remain the existing per-class ones:

- `LibusbBackend` — lifecycle (attach, claim, transfer allocation), errors
- `UsbTransferManager` — transfer callbacks, event loop, stop/start
- `UsbDescriptorParser` — descriptor parsing (UAC1/UAC2)
- `AudioEngine` — engine state transitions, backend selection
- `BackendManager` — backend switch events
- `UsbVolumeControl` — volume / mute control

These stay separate because they're noisy and per-subsystem. Use
`-s Tag:I` on them only when drilling into a specific area.

---

## When to use `WMA_AUDIT`

**DO** tag with `WMA_AUDIT` any log line that:
- Is part of a structured audit trail someone will grep for later
- Summarises state at an interesting point (stream start, mode change,
  clock negotiation result, periodic health snapshot)
- Helps reconstruct *what the USB flow did* in post-mortem analysis

**DO NOT** tag with `WMA_AUDIT`:
- Raw error messages from libusb — keep on the per-class tag so `-s LibusbBackend:E`
  still works
- High-frequency per-sample logs — not appropriate at any tag, only
  rate-limited snapshots belong in audit
- Kotlin unrelated to USB audio (UI state, preferences, analytics)

---

## Filtering in Android Studio

Android Studio's Logcat window has its own filter syntax. Common setups:

```
tag:WMA_AUDIT
```

Or combine with severity and free-text search for UGREEN noise investigation:

```
tag:WMA_AUDIT level:info message:USB_DSP
```

---

## Extending this doc

When adding a new sub-key:
1. Pick a descriptive `UPPER_SNAKE_CASE` string (no spaces).
2. Use it as the first token of the message body: `"USB_FOO: frames=%d ..."`.
3. Tag with `WMA_AUDIT`.
4. Add a row to the table above.

If you need a **new tag** (something that genuinely doesn't belong in the
audit trail), use the `WMA_` prefix to keep it under project namespace.
