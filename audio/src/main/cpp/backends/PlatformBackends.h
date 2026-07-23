/**
 * PlatformBackends.h
 *
 * The single registration point for platform-specific audio backends (WA-2.7).
 *
 * Rationale: BackendManager used to name OboeBackend and LibusbBackend
 * directly, which dragged <oboe/Oboe.h> and <libusb.h> into every translation
 * unit that touched backend selection — neither exists on iOS. Sprinkling
 * `#if defined(__ANDROID__)` over the manager would have hidden the policy in
 * a dozen places. Instead every concrete implementation is named in exactly one
 * translation unit (PlatformBackends.cpp) and handed out as IAudioBackend, so
 * the manager is platform-agnostic by construction rather than by discipline.
 *
 * Adding a platform means adding a branch there and nothing else. CoreAudio
 * (WA-2.4) plugs into createSystemAudioBackend().
 */

#pragma once

#include "IAudioBackend.h"

#include <memory>

namespace watermelon_audio {

// Only ever complete inside PlatformBackends.cpp on Android; the forward
// declaration keeps getLibusbBackend()'s signature usable everywhere else.
class LibusbBackend;

/**
 * Create the platform's built-in audio backend.
 *
 * Android: Oboe (AAudio/OpenSL ES). Elsewhere: nullptr until the platform
 * backend lands — iOS gets CoreAudioBackend in WA-2.4.
 *
 * @return Owned backend, or nullptr if the platform has none yet.
 */
std::unique_ptr<IAudioBackend> createSystemAudioBackend();

/**
 * Create a USB Audio Class backend.
 *
 * USB audio is Android-only by design (requirement decision D4): it needs a
 * file descriptor handed over by UsbDeviceConnection, which has no counterpart
 * on iOS. Returns nullptr where unsupported, which BackendManager reports as a
 * failed USB initialization.
 *
 * @return Owned backend, or nullptr if the platform has no USB audio support.
 */
std::unique_ptr<IAudioBackend> createUsbAudioBackend();

/**
 * Recover the concrete LibusbBackend behind a backend pointer.
 *
 * The USB JNI surface needs the full LibusbBackend API (transfer stats,
 * altsetting selection, volume control), which is far too USB-specific to lift
 * into IAudioBackend. This downcast is the one sanctioned escape hatch, and it
 * lives here because only this file may know the concrete type. The backend
 * type tag is checked instead of dynamic_cast to keep the build RTTI-free.
 *
 * @return The USB backend, or nullptr if @p backend is not one.
 */
LibusbBackend* asLibusbBackend(IAudioBackend* backend);

} // namespace watermelon_audio
