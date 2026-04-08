#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>
#include <algorithm>

#include "../voice/VoicePool.h"

/**
 * @brief Lock-free chord harmony system (Phase 9C).
 *
 * Dedicated VoicePool for chord notes with an SPSC event queue.
 * UI thread pushes events via triggerNotes/updateNotes/releaseNotes.
 * Audio thread drains the queue in renderInto() before VoicePool operations,
 * ensuring all VoicePool mutations happen exclusively on the audio thread.
 *
 * Fully header-only, RT-safe in the audio path.
 */
class ChordHarmony {
public:
    // ------- Constants -------
    static constexpr int SOURCE_ID       = 3000;
    static constexpr int MAX_NOTES       = 6;
    static constexpr int QUEUE_CAPACITY  = 32;
    static constexpr int QUEUE_MASK      = QUEUE_CAPACITY - 1;

    // ------- SPSC event type -------
    struct Event {
        enum Type : uint8_t { TRIGGER, UPDATE, RELEASE };
        Type     type;
        uint8_t  count;
        float    frequencies[MAX_NOTES];
        float    amplitude;
        int      oscillatorType;
    };

    ChordHarmony() = default;
    ~ChordHarmony() = default;

    // Non-copyable, non-movable (atomics)
    ChordHarmony(const ChordHarmony&) = delete;
    ChordHarmony& operator=(const ChordHarmony&) = delete;

    // ------- Lifecycle (main thread) -------

    /** Prepare / re-prepare the voice pool. Call from prepare(), not the audio thread. */
    void prepare(int sampleRate, int maxBlockSize) {
        if (!mPool) {
            voice::VoiceAllocationConfig cfg;
            cfg.maxVoices        = MAX_NOTES;
            cfg.reservedForTouch = 0;
            cfg.enableStealing   = true;
            cfg.attackTimeMs     = 10.0f;
            cfg.decayTimeMs      = 50.0f;
            cfg.sustainLevel     = 0.9f;
            cfg.releaseTimeMs    = 80.0f;
            mPool = std::make_unique<voice::VoicePool>(cfg);
        }
        mPool->prepare(sampleRate, maxBlockSize);
        mBuffer.resize(maxBlockSize * 2, 0.0f);
        mReady.store(true, std::memory_order_release);
    }

    // ------- UI thread: push events -------

    void triggerNotes(const float* frequencies, int count, float amplitude, int oscillatorType) {
        if (!mReady.load(std::memory_order_acquire) || count <= 0) return;
        count = std::min(count, MAX_NOTES);

        int write     = mWritePos.load(std::memory_order_relaxed);
        int nextWrite = (write + 1) & QUEUE_MASK;
        if (nextWrite == mReadPos.load(std::memory_order_acquire)) return; // full, drop

        auto& evt          = mQueue[write];
        evt.type           = Event::TRIGGER;
        evt.count          = static_cast<uint8_t>(count);
        evt.amplitude      = amplitude;
        evt.oscillatorType = oscillatorType;
        for (int i = 0; i < count; i++) evt.frequencies[i] = frequencies[i];

        mWritePos.store(nextWrite, std::memory_order_release);
        mActive.store(true, std::memory_order_release);
    }

    void updateNotes(const float* frequencies, int count, float amplitude) {
        if (!mReady.load(std::memory_order_acquire) || count <= 0) return;
        count = std::min(count, MAX_NOTES);

        int write     = mWritePos.load(std::memory_order_relaxed);
        int nextWrite = (write + 1) & QUEUE_MASK;
        if (nextWrite == mReadPos.load(std::memory_order_acquire)) return;

        auto& evt     = mQueue[write];
        evt.type      = Event::UPDATE;
        evt.count     = static_cast<uint8_t>(count);
        evt.amplitude = amplitude;
        for (int i = 0; i < count; i++) evt.frequencies[i] = frequencies[i];

        mWritePos.store(nextWrite, std::memory_order_release);
    }

    void releaseNotes() {
        if (!mReady.load(std::memory_order_acquire)) return;

        int write     = mWritePos.load(std::memory_order_relaxed);
        int nextWrite = (write + 1) & QUEUE_MASK;
        if (nextWrite == mReadPos.load(std::memory_order_acquire)) return;

        mQueue[write].type = Event::RELEASE;
        mWritePos.store(nextWrite, std::memory_order_release);
        mActive.store(false, std::memory_order_release);
    }

    // ------- Audio thread: drain queue + render -------

    /** Drain the SPSC queue and render chord voices into @p buffer (interleaved stereo). */
    void renderInto(float* buffer, int numFrames) {
        if (!mReady.load(std::memory_order_acquire)) return;

        // Drain event queue (audio thread only)
        int read  = mReadPos.load(std::memory_order_relaxed);
        int write = mWritePos.load(std::memory_order_acquire);
        while (read != write) {
            const auto& evt = mQueue[read];
            switch (evt.type) {
                case Event::TRIGGER: {
                    mPool->releaseBySourceId(SOURCE_ID);
                    for (int i = 0; i < evt.count; i++) {
                        voice::VoiceParams params;
                        params.frequency      = evt.frequencies[i];
                        params.amplitude      = evt.amplitude;
                        params.oscillatorType = evt.oscillatorType;
                        params.sourceId       = SOURCE_ID;
                        params.noteId         = SOURCE_ID + i + 1;
                        mPool->allocateVoice(params, 0);
                    }
                    break;
                }
                case Event::UPDATE: {
                    for (int i = 0; i < evt.count; i++) {
                        int noteId   = SOURCE_ID + i + 1;
                        int voiceIdx = mPool->findVoiceByNoteId(noteId);
                        if (voiceIdx >= 0) {
                            auto* v = mPool->getVoice(voiceIdx);
                            if (v) {
                                v->setFrequency(evt.frequencies[i]);
                                v->setAmplitude(evt.amplitude);
                            }
                        }
                    }
                    break;
                }
                case Event::RELEASE:
                    mPool->releaseBySourceId(SOURCE_ID);
                    break;
            }
            read = (read + 1) & QUEUE_MASK;
        }
        mReadPos.store(read, std::memory_order_release);

        // Render chord voices
        if (mPool->getActiveVoiceCount() == 0) return;

        int totalSamples = numFrames * 2;
        if (static_cast<int>(mBuffer.size()) < totalSamples) return;

        std::memset(mBuffer.data(), 0, totalSamples * sizeof(float));
        mPool->renderAll(numFrames);
        mPool->mixToOutput(mBuffer.data(), numFrames);

        for (int i = 0; i < totalSamples; i++) {
            buffer[i] += mBuffer[i];
        }
    }

    // ------- Queries -------
    bool isActive() const { return mActive.load(std::memory_order_acquire); }
    bool isReady()  const { return mReady.load(std::memory_order_acquire); }

private:
    // SPSC queue
    std::array<Event, QUEUE_CAPACITY> mQueue{};
    std::atomic<int> mWritePos{0};
    std::atomic<int> mReadPos{0};

    // Voice pool + render buffer
    std::unique_ptr<voice::VoicePool> mPool;
    std::atomic<bool> mActive{false};
    std::atomic<bool> mReady{false};
    std::vector<float> mBuffer;
};
