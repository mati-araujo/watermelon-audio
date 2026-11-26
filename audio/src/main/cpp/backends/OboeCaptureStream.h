/**
 * OboeCaptureStream.h
 *
 * INodeCaptureStream sobre Oboe (Android). Concentra toda la dependencia de Oboe
 * del stream de captura propio de InputNode: la herencia del callback y el handle
 * del stream. Solo PlatformBackends.cpp lo nombra.
 */

#pragma once

#include "INodeCaptureStream.h"

#include <oboe/Oboe.h>

#include <memory>

class InputNode;

namespace watermelon_audio {

class OboeCaptureStream final : public INodeCaptureStream,
                                private oboe::AudioStreamDataCallback {
public:
    explicit OboeCaptureStream(InputNode& node) : mNode(&node) {}

    bool openCaptureStream(InputSource source) override;
    bool startCaptureStream() override;
    void stopAndCloseCaptureStream() override;
    bool isCaptureStreamOpen() const override { return mStream != nullptr; }
    int captureStreamSampleRate() const override;
    int64_t captureStreamLatencyFrames() const override;

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream,
                                          void* audioData,
                                          int32_t numFrames) override;

private:
    // Oboe guarda un puntero crudo a este objeto mientras el stream esta abierto:
    // el objeto tiene que sobrevivir a todo stream registrado con el.
    InputNode* mNode;
    std::shared_ptr<oboe::AudioStream> mStream;
};

}  // namespace watermelon_audio
