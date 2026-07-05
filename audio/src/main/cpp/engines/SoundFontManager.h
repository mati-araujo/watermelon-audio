#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../platform/Logger.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "tsf.h"
#include "tsf_ext.h"

#ifndef SFM_LOG_TAG
#define SFM_LOG_TAG "SF8.Manager"
#endif
#define SFM_LOGI(...) wma::logMessage(wma::LogLevel::INFO, SFM_LOG_TAG, __VA_ARGS__)
#define SFM_LOGE(...) wma::logMessage(wma::LogLevel::ERROR, SFM_LOG_TAG, __VA_ARGS__)

/**
 * @class SoundFontManager
 * @brief Manages SoundFont loading/unloading lifecycle with RT-safe pointer swap
 *
 * Supports two loading methods:
 *   - loadFromPath(): mmap-based, zero-copy — preferred for large files
 *   - loadFromMemory(): buffer-based — fallback for JNI byte arrays
 *
 * Thread model:
 *   - loadFromPath() / loadFromMemory() / unload(): JNI thread (mutex-protected)
 *   - getActiveSF(): audio thread (lock-free atomic load)
 *   - cleanupPending(): JNI thread after audio is paused
 */
class SoundFontManager {
public:
    /**
     * @brief Cached, immutable metadata for a single preset (AUD-4).
     *
     * Populated once at load time from the tsf instance. Reads on the JNI
     * thread hit the cache and never re-scan tsf internals.
     */
    struct PresetInfo {
        std::string name;
        int minKey;
        int maxKey;
        int bank = -1;     // SF2 bank (128 = GM percussion kit)
        int program = -1;  // GM program number (0-127)
    };

    SoundFontManager() = default;

    ~SoundFontManager() {
        unload();
        cleanupPending();
    }

    /**
     * @brief Load a SoundFont from a file path using mmap (zero-copy)
     * @param path Absolute path to .sf2 file
     * @param sampleRate Audio output sample rate
     * @return true if loading succeeded
     *
     * Uses mmap to map the file into virtual memory without copying.
     * The kernel pages data lazily — only accessed regions use physical RAM.
     * After tsf parses the file, the mmap is released (tsf owns parsed data).
     *
     * Memory savings vs loadFromMemory:
     *   - No Kotlin ByteArray allocation (~30-148 MB saved on JVM heap)
     *   - No JNI byte array copy
     *   - mmap'd region paged lazily by kernel (not all in RAM at once)
     *
     * NOT RT-safe — call from background/JNI thread.
     */
    bool loadFromPath(const char* path, int32_t sampleRate) {
        std::lock_guard<std::mutex> lock(mLoadMutex);

        // Open file
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            SFM_LOGE("[SF8] loadFromPath: failed to open %s (errno=%d)", path, errno);
            return false;
        }

        // Get file size
        struct stat st{};
        if (fstat(fd, &st) < 0) {
            SFM_LOGE("[SF8] loadFromPath: fstat failed (errno=%d)", errno);
            close(fd);
            return false;
        }
        size_t fileSize = static_cast<size_t>(st.st_size);

        // mmap the file — read-only, private (copy-on-write if needed)
        void* mapped = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd); // fd can be closed after mmap

        if (mapped == MAP_FAILED) {
            SFM_LOGE("[SF8] loadFromPath: mmap failed for %zu bytes (errno=%d)", fileSize, errno);
            return false;
        }

        // Advise kernel: we'll read sequentially (improves readahead)
        madvise(mapped, fileSize, MADV_SEQUENTIAL);

        SFM_LOGI("[SF8] loadFromPath: mmap'd %zu bytes from %s", fileSize, path);

        // Parse SF2 from mmap'd memory — tsf copies what it needs internally
        tsf* newSF = tsf_load_memory(mapped, static_cast<int>(fileSize));

        // Release mmap — tsf has its own copy of parsed data
        munmap(mapped, fileSize);

        if (!newSF) {
            SFM_LOGE("[SF8] loadFromPath: tsf_load_memory failed");
            return false;
        }

        return configurAndSwap(newSF, sampleRate, fileSize);
    }

    /**
     * @brief Load a SoundFont from a memory buffer (legacy path)
     * @param data Raw .sf2 file data
     * @param size Size in bytes
     * @param sampleRate Audio output sample rate
     * @return true if loading succeeded
     *
     * NOT RT-safe — allocates memory. Call from background/JNI thread.
     */
    bool loadFromMemory(const void* data, int size, int32_t sampleRate) {
        std::lock_guard<std::mutex> lock(mLoadMutex);

        tsf* newSF = tsf_load_memory(data, size);
        if (!newSF) {
            SFM_LOGE("[SF8] loadFromMemory: Failed to parse SF2 data (%d bytes)", size);
            return false;
        }

        return configurAndSwap(newSF, sampleRate, size);
    }

    /**
     * @brief Unload the current SoundFont
     */
    void unload() {
        std::lock_guard<std::mutex> lock(mLoadMutex);
        tsf* old = mActiveSF.exchange(nullptr, std::memory_order_release);
        mPresetCache.reset();
        if (old) {
            tsf* expected = nullptr;
            if (!mPendingDelete.compare_exchange_strong(expected, old,
                    std::memory_order_release)) {
                tsf_close(expected);
                mPendingDelete.store(old, std::memory_order_release);
            }
        }
    }

    /**
     * @brief Get the active SoundFont for audio rendering
     * RT-safe: lock-free atomic load.
     */
    tsf* getActiveSF() const {
        return mActiveSF.load(std::memory_order_acquire);
    }

    /**
     * @brief Clean up any pending-delete SoundFont
     */
    void cleanupPending() {
        tsf* pending = mPendingDelete.exchange(nullptr, std::memory_order_acquire);
        if (pending) {
            tsf_close(pending);
            SFM_LOGI("[SF8] Cleaned up pending SF2");
        }
    }

    bool isLoaded() const {
        return mActiveSF.load(std::memory_order_acquire) != nullptr;
    }

    /**
     * @brief Preset count from the cache (AUD-4).
     *
     * Thread model: read from JNI/main thread under [mLoadMutex]. Returns 0
     * when no SoundFont is loaded.
     */
    int getPresetCount() const {
        std::lock_guard<std::mutex> lock(mLoadMutex);
        return mPresetCache ? static_cast<int>(mPresetCache->size()) : 0;
    }

    /**
     * @brief Preset name from the cache (AUD-4).
     *
     * Thread model: read from JNI/main thread under [mLoadMutex]. The returned
     * pointer is owned by the cache and is valid until the next load/unload.
     * Returns nullptr for out-of-range indices or when no SoundFont is loaded.
     */
    const char* getPresetName(int presetIndex) const {
        std::lock_guard<std::mutex> lock(mLoadMutex);
        if (!mPresetCache || presetIndex < 0 ||
            presetIndex >= static_cast<int>(mPresetCache->size())) {
            return nullptr;
        }
        return (*mPresetCache)[presetIndex].name.c_str();
    }

    int32_t getSampleRate() const { return mSampleRate; }

    /**
     * @brief Effective MIDI key range for a preset, from the cache (AUD-4).
     *
     * Pre-computed at load time using the same heuristic as before. Thread
     * model: read from JNI/main thread under [mLoadMutex]; no tsf access here.
     * @return true if the preset exists and outMinKey/outMaxKey were populated.
     */
    bool getPresetKeyRange(int presetIndex, int& outMinKey, int& outMaxKey) const {
        std::lock_guard<std::mutex> lock(mLoadMutex);
        if (!mPresetCache || presetIndex < 0 ||
            presetIndex >= static_cast<int>(mPresetCache->size())) {
            return false;
        }
        const auto& info = (*mPresetCache)[presetIndex];
        outMinKey = info.minKey;
        outMaxKey = info.maxKey;
        return true;
    }

    /**
     * @brief SF2 bank + GM program for a preset, from the cache.
     *
     * Used for instrument classification (bank 128 = percussion → DrumGrid).
     * Thread model: read from JNI/main thread under [mLoadMutex].
     * @return true if the preset exists and outBank/outProgram were populated.
     */
    bool getPresetBankProgram(int presetIndex, int& outBank, int& outProgram) const {
        std::lock_guard<std::mutex> lock(mLoadMutex);
        if (!mPresetCache || presetIndex < 0 ||
            presetIndex >= static_cast<int>(mPresetCache->size())) {
            return false;
        }
        const auto& info = (*mPresetCache)[presetIndex];
        outBank = info.bank;
        outProgram = info.program;
        return true;
    }

private:
    /**
     * @brief Configure tsf instance and atomically swap to audio thread
     */
    bool configurAndSwap(tsf* newSF, int32_t sampleRate, size_t fileSize) {
        tsf_set_output(newSF, TSF_STEREO_INTERLEAVED, sampleRate, 0.0f);
        tsf_set_max_voices(newSF, 64);

        int presetCount = tsf_get_presetcount(newSF);
        SFM_LOGI("[SF8] Loaded SF2 (%zu bytes, %d presets, sr=%d)",
                 fileSize, presetCount, sampleRate);

        // Build the immutable preset cache (AUD-4). buildPresetCache must run
        // before the atomic swap so consumers see metadata in lockstep with
        // the active SF — reads on the JNI thread can never observe a window
        // where the SF is published but the cache is still stale.
        auto cache = buildPresetCache(newSF, presetCount);

        // Atomic swap to audio thread
        tsf* old = mActiveSF.exchange(newSF, std::memory_order_release);
        mPresetCache = std::move(cache);
        if (old) {
            tsf* expected = nullptr;
            if (!mPendingDelete.compare_exchange_strong(expected, old,
                    std::memory_order_release)) {
                tsf_close(expected);
                mPendingDelete.store(old, std::memory_order_release);
            }
        }

        mSampleRate = sampleRate;
        return true;
    }

    /**
     * @brief Build the immutable preset cache from a freshly parsed tsf.
     * Implemented in SoundFontManager.cpp where the heuristic lives.
     * Called only from configurAndSwap under mLoadMutex.
     */
    static std::shared_ptr<const std::vector<PresetInfo>> buildPresetCache(
        tsf* sf, int presetCount);

    std::atomic<tsf*> mActiveSF{nullptr};
    std::atomic<tsf*> mPendingDelete{nullptr};
    mutable std::mutex mLoadMutex;
    // Immutable post-load preset metadata. Replaced wholesale on load/unload
    // while [mLoadMutex] is held. Reads on JNI thread take the mutex briefly
    // to copy/inspect; the pointed-to vector is never mutated in place.
    std::shared_ptr<const std::vector<PresetInfo>> mPresetCache;
    int32_t mSampleRate = 48000;
};
