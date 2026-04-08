/**
 * @file jni_bridge.h
 * @brief JNI utilities and error codes synchronized with Kotlin.
 *
 * This header provides:
 * - Error codes matching NativeErrorCode.kt
 * - RAII wrappers for JNI resources
 * - Helper functions for common JNI operations
 * - Logging macros
 */

#ifndef JNI_BRIDGE_H
#define JNI_BRIDGE_H

#include <jni.h>
#include "platform/Logger.h"
#include <string>
#include <vector>
#include <memory>

// ==================== Logging ====================

#define JNI_LOG_TAG "NoisyPadJNI"
#define JNI_LOGI(...) wma::logMessage(wma::LogLevel::INFO, JNI_LOG_TAG, __VA_ARGS__)
#define JNI_LOGW(...) wma::logMessage(wma::LogLevel::WARN, JNI_LOG_TAG, __VA_ARGS__)
#define JNI_LOGE(...) wma::logMessage(wma::LogLevel::ERROR, JNI_LOG_TAG, __VA_ARGS__)
#define JNI_LOGD(...) wma::logMessage(wma::LogLevel::DEBUG, JNI_LOG_TAG, __VA_ARGS__)

// ==================== Error Codes ====================

/**
 * Error codes synchronized with Kotlin NativeErrorCode enum.
 *
 * These codes are used as return values from JNI functions.
 * Negative values indicate errors, 0 or positive values indicate success.
 */
enum class NativeErrorCode : int {
    SUCCESS = 0,
    ENGINE_NOT_INITIALIZED = -1,
    INVALID_EFFECT_INDEX = -2,
    INVALID_PARAMETER_ID = -3,
    PARAMETER_OUT_OF_RANGE = -4,
    EFFECT_CHAIN_FULL = -5,
    MEMORY_ALLOCATION_FAILED = -6,
    STREAM_ERROR = -7,
    MODE_TRANSITION_IN_PROGRESS = -8,
    INVALID_OPERATION = -9,
    INVALID_EFFECT_TYPE = -10,
    TIMEOUT = -11,
    UNKNOWN_ERROR = -99
};

/**
 * Convert error code to int for JNI return.
 */
inline jint toJint(NativeErrorCode code) {
    return static_cast<jint>(code);
}

/**
 * Check if a code indicates success.
 */
inline bool isSuccess(NativeErrorCode code) {
    return static_cast<int>(code) >= 0;
}

// ==================== JNI Cache ====================

/**
 * Cache for JNI class/method references.
 *
 * References are initialized in JNI_OnLoad and released in JNI_OnUnload.
 * This avoids repeated FindClass/GetMethodID calls which are expensive.
 *
 * Usage:
 *   // In JNI_OnLoad:
 *   gJniCache.initialize(env);
 *
 *   // In JNI function:
 *   if (gJniCache.isInitialized) {
 *       jobject snapshot = env->NewObject(gJniCache.effectSnapshotClass, ...);
 *   }
 *
 *   // In JNI_OnUnload:
 *   gJniCache.release(env);
 */
struct JniCache {
    // Class references (must be GlobalRef)
    jclass effectSnapshotClass = nullptr;
    jclass nativeEffectSnapshotClass = nullptr;

    // Method IDs (do not need GlobalRef)
    jmethodID effectSnapshotConstructor = nullptr;

    bool isInitialized = false;

    /**
     * Initialize cache. Must be called from JNI_OnLoad.
     */
    void initialize(JNIEnv* env) {
        if (isInitialized) return;

        // NativeEffectSnapshot class
        jclass localClass = env->FindClass(
            "com/watermellonstudios/audio/api/NativeEffectSnapshot"
        );
        if (localClass != nullptr) {
            nativeEffectSnapshotClass = (jclass)env->NewGlobalRef(localClass);
            env->DeleteLocalRef(localClass);
        } else {
            JNI_LOGW("JniCache: NativeEffectSnapshot class not found");
            env->ExceptionClear();
        }

        isInitialized = true;
        JNI_LOGI("JniCache initialized");
    }

    /**
     * Release cached references. Must be called from JNI_OnUnload.
     */
    void release(JNIEnv* env) {
        if (effectSnapshotClass != nullptr) {
            env->DeleteGlobalRef(effectSnapshotClass);
            effectSnapshotClass = nullptr;
        }
        if (nativeEffectSnapshotClass != nullptr) {
            env->DeleteGlobalRef(nativeEffectSnapshotClass);
            nativeEffectSnapshotClass = nullptr;
        }

        effectSnapshotConstructor = nullptr;
        isInitialized = false;
        JNI_LOGI("JniCache released");
    }
};

// Global cache instance (defined in native-lib.cpp)
extern JniCache gJniCache;

// ==================== RAII Wrappers ====================

/**
 * RAII wrapper for local JNI references.
 * Automatically deletes the local reference when going out of scope.
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

    // Prevent copy
    ScopedLocalRef(const ScopedLocalRef&) = delete;
    ScopedLocalRef& operator=(const ScopedLocalRef&) = delete;

    // Allow move
    ScopedLocalRef(ScopedLocalRef&& other) noexcept
        : env_(other.env_), obj_(other.obj_) {
        other.obj_ = nullptr;
    }

    jobject get() const { return obj_; }
    operator jobject() const { return obj_; }

    /**
     * Release ownership without deleting.
     */
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
 * RAII wrapper for GetPrimitiveArrayCritical.
 * Provides zero-copy access to Java arrays for maximum performance.
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
            if (ptr_ == nullptr) {
                JNI_LOGE("GetPrimitiveArrayCritical failed");
                size_ = 0;
            }
        }
    }

    ~ScopedCriticalArray() {
        if (ptr_ != nullptr && array_ != nullptr) {
            env_->ReleasePrimitiveArrayCritical(array_, ptr_, 0);
        }
    }

    // Prevent copy
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

// Type aliases for common array types
using ScopedIntArray = ScopedCriticalArray<jint>;
using ScopedFloatArray = ScopedCriticalArray<jfloat>;

/**
 * RAII wrapper for GetFloatArrayElements (non-critical, safe for JNI calls).
 */
class ScopedFloatArrayRW {
public:
    ScopedFloatArrayRW(JNIEnv* env, jfloatArray array)
        : env_(env), array_(array), elements_(nullptr), size_(0) {
        if (array != nullptr) {
            size_ = env->GetArrayLength(array);
            elements_ = env->GetFloatArrayElements(array, nullptr);
            if (elements_ == nullptr) {
                JNI_LOGE("GetFloatArrayElements failed");
                size_ = 0;
            }
        }
    }

    ~ScopedFloatArrayRW() {
        if (elements_ != nullptr && array_ != nullptr) {
            env_->ReleaseFloatArrayElements(array_, elements_, 0);
        }
    }

    // Prevent copy
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
 * RAII wrapper for GetStringUTFChars.
 */
class ScopedUtfChars {
public:
    ScopedUtfChars(JNIEnv* env, jstring jstr)
        : env_(env), jstr_(jstr), chars_(nullptr) {
        if (jstr != nullptr) {
            chars_ = env->GetStringUTFChars(jstr, nullptr);
            if (chars_ == nullptr) {
                JNI_LOGE("GetStringUTFChars failed");
            }
        }
    }

    ~ScopedUtfChars() {
        if (chars_ != nullptr && jstr_ != nullptr) {
            env_->ReleaseStringUTFChars(jstr_, chars_);
        }
    }

    // Prevent copy
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

// ==================== Helper Functions ====================

/**
 * Check for and clear any pending JNI exception.
 * @return true if an exception was pending
 */
inline bool checkJniException(JNIEnv* env, const char* context = nullptr) {
    if (env->ExceptionCheck()) {
        if (context) {
            JNI_LOGE("JNI exception in %s", context);
        }
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }
    return false;
}

/**
 * Create a new Java float array from a C++ vector.
 * @return The new array, or nullptr on failure
 */
inline jfloatArray vectorToJFloatArray(JNIEnv* env, const std::vector<float>& vec) {
    jfloatArray result = env->NewFloatArray(static_cast<jsize>(vec.size()));
    if (result == nullptr) {
        JNI_LOGE("Failed to allocate float array of size %zu", vec.size());
        return nullptr;
    }
    env->SetFloatArrayRegion(result, 0, static_cast<jsize>(vec.size()), vec.data());
    return result;
}

/**
 * Create a new Java int array from a C++ vector.
 */
inline jintArray vectorToJIntArray(JNIEnv* env, const std::vector<int>& vec) {
    jintArray result = env->NewIntArray(static_cast<jsize>(vec.size()));
    if (result == nullptr) {
        JNI_LOGE("Failed to allocate int array of size %zu", vec.size());
        return nullptr;
    }
    env->SetIntArrayRegion(result, 0, static_cast<jsize>(vec.size()), vec.data());
    return result;
}

#endif // JNI_BRIDGE_H
