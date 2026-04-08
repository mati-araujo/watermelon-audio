/**
 * @file jni_common.h
 * @brief Common utilities for JNI layer.
 *
 * This header provides:
 * - Error codes synchronized with Kotlin NativeErrorCode
 * - RAII wrappers for JNI resources
 * - Logging macros
 * - Shared state access (engine, inputNode)
 *
 * All jni_*.cpp files should include this header.
 */

#ifndef JNI_COMMON_H
#define JNI_COMMON_H

#include <jni.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <cmath>
#include <vector>
#include <string>

// Platform-agnostic logging (Phase 0A: audio lib extraction)
#include "../platform/Logger.h"

// Forward declarations
class AudioEngine;
class InputNode;

// ==================== Logging ====================
// Aliases for backward compatibility — all 32+ files use LOGI/LOGW/LOGE/LOGD.
// These now delegate to wma::logMessage() via platform/Logger.h.

#define JNI_TAG "NoisyPadJNI"
#define LOGI(...) wma::logMessage(wma::LogLevel::INFO,  JNI_TAG, __VA_ARGS__)
#define LOGW(...) wma::logMessage(wma::LogLevel::WARN,  JNI_TAG, __VA_ARGS__)
#define LOGE(...) wma::logMessage(wma::LogLevel::ERROR, JNI_TAG, __VA_ARGS__)
#define LOGD(...) wma::logMessage(wma::LogLevel::DEBUG, JNI_TAG, __VA_ARGS__)

// ==================== Error Codes ====================

/**
 * Error codes synchronized with Kotlin NativeErrorCode enum.
 * Negative = error, Zero/Positive = success.
 */
namespace JniError {
    constexpr jint SUCCESS = 0;
    constexpr jint ENGINE_NOT_INITIALIZED = -1;
    constexpr jint INVALID_EFFECT_INDEX = -2;
    constexpr jint INVALID_PARAMETER_ID = -3;
    constexpr jint PARAMETER_OUT_OF_RANGE = -4;
    constexpr jint EFFECT_CHAIN_FULL = -5;
    constexpr jint MEMORY_ALLOCATION_FAILED = -6;
    constexpr jint STREAM_ERROR = -7;
    constexpr jint MODE_TRANSITION_IN_PROGRESS = -8;
    constexpr jint INVALID_OPERATION = -9;
    constexpr jint INVALID_EFFECT_TYPE = -10;
    constexpr jint TIMEOUT = -11;
    constexpr jint UNKNOWN_ERROR = -99;
}

// ==================== Shared State ====================

// Forward declaration
struct WmaEngine;

/**
 * Global state shared across all JNI files.
 * Defined in jni_engine.cpp.
 *
 * Phase 0D: Ownership is in g_wmaEngine (WmaEngine*).
 * g_jniState provides raw pointers for fast access by JNI functions.
 */
struct JniGlobalState {
    AudioEngine* engine = nullptr;            // Non-owning, points into g_wmaEngine
    std::shared_ptr<InputNode> inputNode;
    std::mutex engineMutex;

    // Mode system
    std::atomic<int> currentMode{0};
    std::atomic<bool> modeTransitionInProgress{false};
    std::atomic<float> modeTransitionProgress{0.0f};
};

extern JniGlobalState g_jniState;
extern WmaEngine* g_wmaEngine;  // Owns AudioEngine + BackendManager + InputNode

// ==================== Helper Functions ====================

/**
 * Ensure engine exists, create if needed.
 * @return true if engine is ready
 */
bool ensureEngine();

/**
 * Ensure input node exists, create if needed.
 * @return true if input node is ready
 */
bool ensureInputNode();

// ==================== JNI Cache ====================

/**
 * Cache for JNI class/method references.
 */
struct JniCache {
    jclass nativeEffectSnapshotClass = nullptr;
    bool isInitialized = false;

    void initialize(JNIEnv* env);
    void release(JNIEnv* env);
};

extern JniCache g_jniCache;

// ==================== RAII Wrappers ====================

/**
 * RAII wrapper for local JNI references.
 */
class ScopedLocalRef {
public:
    ScopedLocalRef(JNIEnv* env, jobject obj)
        : env_(env), obj_(obj) {}

    ~ScopedLocalRef() {
        if (obj_ != nullptr) {
            env_->DeleteLocalRef(obj_);
        }
    }

    ScopedLocalRef(const ScopedLocalRef&) = delete;
    ScopedLocalRef& operator=(const ScopedLocalRef&) = delete;

    ScopedLocalRef(ScopedLocalRef&& other) noexcept
        : env_(other.env_), obj_(other.obj_) {
        other.obj_ = nullptr;
    }

    jobject get() const { return obj_; }
    operator jobject() const { return obj_; }

    jobject release() {
        jobject tmp = obj_;
        obj_ = nullptr;
        return tmp;
    }

private:
    JNIEnv* env_;
    jobject obj_;
};

/**
 * RAII wrapper for GetPrimitiveArrayCritical (zero-copy, fast).
 * WARNING: No JNI calls allowed while holding the array.
 */
template<typename T>
class ScopedCriticalArray {
public:
    ScopedCriticalArray(JNIEnv* env, jarray array)
        : env_(env), array_(array), ptr_(nullptr), size_(0) {
        if (array != nullptr) {
            size_ = env->GetArrayLength(array);
            ptr_ = static_cast<T*>(env->GetPrimitiveArrayCritical(array, nullptr));
        }
    }

    ~ScopedCriticalArray() {
        if (ptr_ != nullptr && array_ != nullptr) {
            env_->ReleasePrimitiveArrayCritical(array_, ptr_, 0);
        }
    }

    ScopedCriticalArray(const ScopedCriticalArray&) = delete;
    ScopedCriticalArray& operator=(const ScopedCriticalArray&) = delete;

    T* get() const { return ptr_; }
    jsize size() const { return size_; }
    bool isValid() const { return ptr_ != nullptr; }

    T& operator[](size_t i) { return ptr_[i]; }
    const T& operator[](size_t i) const { return ptr_[i]; }

private:
    JNIEnv* env_;
    jarray array_;
    T* ptr_;
    jsize size_;
};

using ScopedIntArray = ScopedCriticalArray<jint>;
using ScopedFloatArray = ScopedCriticalArray<jfloat>;

/**
 * RAII wrapper for GetFloatArrayElements (safe for JNI calls).
 */
class ScopedFloatArrayRW {
public:
    ScopedFloatArrayRW(JNIEnv* env, jfloatArray array)
        : env_(env), array_(array), elements_(nullptr), size_(0) {
        if (array != nullptr) {
            size_ = env->GetArrayLength(array);
            elements_ = env->GetFloatArrayElements(array, nullptr);
        }
    }

    ~ScopedFloatArrayRW() {
        if (elements_ != nullptr && array_ != nullptr) {
            env_->ReleaseFloatArrayElements(array_, elements_, 0);
        }
    }

    ScopedFloatArrayRW(const ScopedFloatArrayRW&) = delete;
    ScopedFloatArrayRW& operator=(const ScopedFloatArrayRW&) = delete;

    jfloat* get() { return elements_; }
    jsize size() const { return size_; }
    bool isValid() const { return elements_ != nullptr; }

private:
    JNIEnv* env_;
    jfloatArray array_;
    jfloat* elements_;
    jsize size_;
};

/**
 * RAII wrapper for GetIntArrayElements.
 */
class ScopedIntArrayRW {
public:
    ScopedIntArrayRW(JNIEnv* env, jintArray array)
        : env_(env), array_(array), elements_(nullptr), size_(0) {
        if (array != nullptr) {
            size_ = env->GetArrayLength(array);
            elements_ = env->GetIntArrayElements(array, nullptr);
        }
    }

    ~ScopedIntArrayRW() {
        if (elements_ != nullptr && array_ != nullptr) {
            env_->ReleaseIntArrayElements(array_, elements_, 0);
        }
    }

    ScopedIntArrayRW(const ScopedIntArrayRW&) = delete;
    ScopedIntArrayRW& operator=(const ScopedIntArrayRW&) = delete;

    jint* get() { return elements_; }
    jsize size() const { return size_; }
    bool isValid() const { return elements_ != nullptr; }

private:
    JNIEnv* env_;
    jintArray array_;
    jint* elements_;
    jsize size_;
};

/**
 * RAII wrapper for GetStringUTFChars.
 */
class ScopedUtfChars {
public:
    ScopedUtfChars(JNIEnv* env, jstring jstr)
        : env_(env), jstr_(jstr), chars_(nullptr) {
        if (jstr != nullptr) {
            chars_ = env->GetStringUTFChars(jstr, nullptr);
        }
    }

    ~ScopedUtfChars() {
        if (chars_ != nullptr && jstr_ != nullptr) {
            env_->ReleaseStringUTFChars(jstr_, chars_);
        }
    }

    ScopedUtfChars(const ScopedUtfChars&) = delete;
    ScopedUtfChars& operator=(const ScopedUtfChars&) = delete;

    const char* c_str() const { return chars_; }
    bool isValid() const { return chars_ != nullptr; }
    operator const char*() const { return chars_; }

private:
    JNIEnv* env_;
    jstring jstr_;
    const char* chars_;
};

// ==================== Validation Helpers ====================

inline bool isValidFloat(float value) {
    return std::isfinite(value);
}

inline bool isInRange(float value, float min, float max) {
    return isValidFloat(value) && value >= min && value <= max;
}

inline float clampFloat(float value, float min, float max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

// ==================== Array Helpers ====================

inline jfloatArray vectorToJFloatArray(JNIEnv* env, const std::vector<float>& vec) {
    jfloatArray result = env->NewFloatArray(static_cast<jsize>(vec.size()));
    if (result != nullptr) {
        env->SetFloatArrayRegion(result, 0, static_cast<jsize>(vec.size()), vec.data());
    }
    return result;
}

inline jintArray vectorToJIntArray(JNIEnv* env, const std::vector<int>& vec) {
    jintArray result = env->NewIntArray(static_cast<jsize>(vec.size()));
    if (result != nullptr) {
        env->SetIntArrayRegion(result, 0, static_cast<jsize>(vec.size()), vec.data());
    }
    return result;
}

inline bool checkJniException(JNIEnv* env, const char* context = nullptr) {
    if (env->ExceptionCheck()) {
        if (context) {
            LOGE("JNI exception in %s", context);
        }
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }
    return false;
}

// ==================== Phase 4.5: Array Pooling ====================
// TODO(Phase 4.5): Enable pooling for high-frequency operations

/**
 * Pool of reusable float arrays for waveform data (Phase 4.5).
 *
 * Avoids repeated allocation for high-frequency operations
 * like getWaveformSamples().
 *
 * Usage:
 * ```cpp
 * float* buffer = g_floatArrayPool.acquire(512);
 * // use buffer...
 * g_floatArrayPool.release(buffer, 512);
 * ```
 */
class FloatArrayPool {
public:
    static constexpr size_t DEFAULT_SIZE = 512;
    static constexpr size_t POOL_SIZE = 4;

    FloatArrayPool() {
        for (size_t i = 0; i < POOL_SIZE; i++) {
            pool_[i] = std::make_unique<float[]>(DEFAULT_SIZE);
            available_[i].store(true, std::memory_order_relaxed);
        }
    }

    /**
     * Acquire a float array from the pool.
     * If pool is exhausted or size > DEFAULT_SIZE, allocates new array.
     *
     * @param size Required array size
     * @return Pointer to float array (caller must release)
     */
    float* acquire(size_t size) {
        if (size > DEFAULT_SIZE) {
            // Large request - allocate directly
            return new float[size];
        }

        // Try to get from pool
        for (size_t i = 0; i < POOL_SIZE; i++) {
            bool expected = true;
            if (available_[i].compare_exchange_strong(expected, false,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return pool_[i].get();
            }
        }

        // Pool exhausted - allocate
        return new float[size];
    }

    /**
     * Release a float array back to the pool.
     *
     * @param ptr Pointer returned from acquire()
     * @param size Size passed to acquire()
     */
    void release(float* ptr, size_t size) {
        if (ptr == nullptr) return;

        if (size > DEFAULT_SIZE) {
            // Was allocated directly
            delete[] ptr;
            return;
        }

        // Check if this is a pooled buffer
        for (size_t i = 0; i < POOL_SIZE; i++) {
            if (pool_[i].get() == ptr) {
                available_[i].store(true, std::memory_order_release);
                return;
            }
        }

        // Not from pool (was allocated when pool was exhausted)
        delete[] ptr;
    }

    /**
     * RAII wrapper for pool buffers.
     */
    class ScopedBuffer {
    public:
        ScopedBuffer(FloatArrayPool& pool, size_t size)
            : pool_(pool), size_(size), ptr_(pool.acquire(size)) {}

        ~ScopedBuffer() {
            pool_.release(ptr_, size_);
        }

        ScopedBuffer(const ScopedBuffer&) = delete;
        ScopedBuffer& operator=(const ScopedBuffer&) = delete;

        float* get() { return ptr_; }
        size_t size() const { return size_; }

    private:
        FloatArrayPool& pool_;
        size_t size_;
        float* ptr_;
    };

private:
    std::unique_ptr<float[]> pool_[POOL_SIZE];
    std::atomic<bool> available_[POOL_SIZE];
};

// Global pool instance (Phase 4.5)
extern FloatArrayPool g_floatArrayPool;

#endif // JNI_COMMON_H
