package com.watermellonstudios.audio.internal.util

import android.app.ActivityManager
import android.content.Context
import android.os.Build
import android.util.Log

/**
 * Utilities for detecting device capabilities and applying optimizations.
 *
 * This object provides methods to detect if a device is low-end and
 * suggest optimized configurations for the audio engine.
 */
object DeviceCapabilities {

    private const val TAG = "DeviceCapabilities"

    /**
     * Detects if the device is low-end.
     *
     * Criteria:
     * - Explicitly marked as low RAM device by the system
     * - Less than 2GB of total RAM
     * - CPU with few cores (<= 4) AND low frequency
     */
    fun isLowEndDevice(context: Context): Boolean {
        val activityManager = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager

        // Android 4.4+ (API 19+) has API to detect low RAM devices
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT) {
            if (activityManager.isLowRamDevice) {
                Log.i(TAG, "Device marked as low RAM by system")
                return true
            }
        }

        // Check total RAM
        val memInfo = ActivityManager.MemoryInfo()
        activityManager.getMemoryInfo(memInfo)
        val totalRamMB = memInfo.totalMem / (1024 * 1024)

        if (totalRamMB < 2048) { // Less than 2GB
            Log.i(TAG, "Low RAM detected: ${totalRamMB}MB")
            return true
        }

        // Check CPU core count
        val cores = Runtime.getRuntime().availableProcessors()
        if (cores <= 4) {
            Log.i(TAG, "Low core count detected: $cores cores")
            return true
        }

        Log.i(TAG, "Normal/High-end device detected (RAM: ${totalRamMB}MB, Cores: $cores)")
        return false
    }

    /**
     * Optimized configuration for the current device.
     */
    data class OptimizedConfig(
        /** Polling interval in milliseconds during playback */
        val pollingIntervalMs: Long = 100,

        /** Polling interval during fade operations */
        val fadingPollingIntervalMs: Long = 16,

        /** Polling interval when stopped */
        val stoppedPollingIntervalMs: Long = 500,

        /** Whether to show waveform viewer by default */
        val showWaveformViewer: Boolean = true,

        /** Maximum number of simultaneous effects */
        val maxEffects: Int = 12,

        /** Whether to use high quality reverb */
        val highQualityReverb: Boolean = true,

        /** Whether to use high quality delay */
        val highQualityDelay: Boolean = true
    )

    /**
     * Gets the optimal configuration based on device capabilities.
     */
    fun getOptimizedConfig(context: Context): OptimizedConfig {
        return if (isLowEndDevice(context)) {
            // Configuration for low-end devices
            OptimizedConfig(
                pollingIntervalMs = 200,  // Reduce polling to 5fps during playback
                fadingPollingIntervalMs = 33,  // Reduce to 30fps during fade
                stoppedPollingIntervalMs = 1000,  // 1fps when stopped
                showWaveformViewer = false,  // Disable waveform by default
                maxEffects = 6,  // Reduced limit for low-end devices
                highQualityReverb = false,  // Reduced quality reverb
                highQualityDelay = false  // Reduced quality delay
            ).also {
                Log.i(TAG,
                    "Using LOW-END optimized config: " +
                    "polling=${it.pollingIntervalMs}ms, " +
                    "maxEffects=${it.maxEffects}, " +
                    "waveform=${it.showWaveformViewer}")
            }
        } else {
            // Standard configuration for normal/high-end devices
            OptimizedConfig().also {
                Log.i(TAG,
                    "Using STANDARD config: " +
                    "polling=${it.pollingIntervalMs}ms, " +
                    "maxEffects=${it.maxEffects}, " +
                    "waveform=${it.showWaveformViewer}")
            }
        }
    }

    /**
     * Device information for debugging and analytics.
     */
    data class DeviceInfo(
        val manufacturer: String,
        val model: String,
        val androidVersion: String,
        val apiLevel: Int,
        val totalRamMB: Long,
        val availableCores: Int,
        val isLowEnd: Boolean
    )

    /**
     * Gets detailed device information.
     */
    fun getDeviceInfo(context: Context): DeviceInfo {
        val activityManager = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val memInfo = ActivityManager.MemoryInfo()
        activityManager.getMemoryInfo(memInfo)

        return DeviceInfo(
            manufacturer = Build.MANUFACTURER,
            model = Build.MODEL,
            androidVersion = Build.VERSION.RELEASE,
            apiLevel = Build.VERSION.SDK_INT,
            totalRamMB = memInfo.totalMem / (1024 * 1024),
            availableCores = Runtime.getRuntime().availableProcessors(),
            isLowEnd = isLowEndDevice(context)
        )
    }
}
