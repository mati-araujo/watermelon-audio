package com.watermellonstudios.audio.internal.sync

import com.watermellonstudios.audio.api.IEffectStateProvider
import com.watermellonstudios.audio.callback.AudioLogger
import com.watermellonstudios.audio.callback.NoOpAudioLogger
import com.watermellonstudios.audio.api.NativeEffectSnapshot
import com.watermellonstudios.audio.domain.effect.EffectState
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlin.math.abs

/**
 * Bidirectional state synchronizer between Kotlin and C++.
 *
 * This component is responsible for:
 * - Polling state periodically from C++
 * - Detecting divergence between Kotlin and C++ state
 * - Reconciling divergence automatically
 * - Emitting sync events for UI feedback
 *
 * Thread Safety: All operations are coroutine-safe. State updates happen
 * on the provided CoroutineScope.
 *
 * Usage:
 * ```kotlin
 * val synchronizer = StateSynchronizer(stateProvider, viewModelScope)
 * synchronizer.startSync()
 *
 * // Observe state
 * synchronizer.syncedState.collect { state ->
 *     updateUI(state.effects)
 * }
 *
 * // Observe events
 * synchronizer.syncEvents.collect { event ->
 *     when (event) {
 *         is SyncEvent.DivergenceDetected -> showWarning()
 *         is SyncEvent.SyncError -> showError(event.error)
 *         else -> {}
 *     }
 * }
 * ```
 *
 * @param stateProvider Interface to read state from native C++
 * @param scope CoroutineScope for sync operations (typically viewModelScope)
 * @param config Configuration for polling interval and reconciliation strategy
 */
class StateSynchronizer(
    private val stateProvider: IEffectStateProvider,
    private val scope: CoroutineScope,
    private val config: SyncConfig = SyncConfig.DEFAULT,
    private val logger: AudioLogger = NoOpAudioLogger
) {
    private val TAG = "StateSynchronizer"

    // Synchronized state (Single Source of Truth for UI)
    private val _syncedState = MutableStateFlow(SyncedAudioState())
    val syncedState: StateFlow<SyncedAudioState> = _syncedState.asStateFlow()

    // Sync events stream
    private val _syncEvents = MutableSharedFlow<SyncEvent>(
        replay = 0,
        extraBufferCapacity = 16
    )
    val syncEvents: SharedFlow<SyncEvent> = _syncEvents.asSharedFlow()

    // Sync loop control
    private var syncJob: Job? = null
    private var lastNativeVersion: Long = 0
    private var consecutiveDivergences: Int = 0

    // FIX P0.2: Track consecutive errors for backoff
    private var consecutiveErrors: Int = 0
    private val maxConsecutiveErrors = 5
    private val maxBackoffMs = 2000L

    /**
     * Starts the synchronization loop.
     *
     * If already running, this is a no-op with a warning log.
     * The loop polls state from C++ at the configured interval
     * and updates [syncedState] accordingly.
     */
    fun startSync() {
        if (syncJob?.isActive == true) {
            logger.warn(TAG, "startSync: sync already running")
            return
        }

        logger.info(TAG, "startSync: STARTING synchronization loop with interval ${config.pollInterval}ms")

        syncJob = scope.launch {
            logger.debug(TAG, "startSync: sync loop coroutine started")
            _syncEvents.emit(SyncEvent.SyncStarted)

            var cycleCount = 0L
            while (isActive) {
                try {
                    performSyncCycle()
                    cycleCount++
                    // FIX P0.2: Reset error counter on success
                    consecutiveErrors = 0
                    // Log every 100 cycles (5 seconds at 50ms interval) to verify sync is running
                    if (cycleCount % 100 == 0L) {
                        logger.debug(TAG, "startSync: sync running, cycle #$cycleCount, effects=${_syncedState.value.effects.size}")
                    }
                    delay(config.pollInterval)
                } catch (e: CancellationException) {
                    logger.debug(TAG, "startSync: sync cancelled")
                    throw e
                } catch (e: Exception) {
                    consecutiveErrors++
                    logger.error(TAG, "startSync: sync cycle error ($consecutiveErrors/$maxConsecutiveErrors)", e)
                    handleSyncError(e)

                    // FIX P0.2: Exponential backoff on repeated errors
                    val backoffMs = minOf(
                        config.pollInterval * (1L shl minOf(consecutiveErrors, 6)),
                        maxBackoffMs
                    )
                    logger.warn(TAG, "startSync: backing off for ${backoffMs}ms")
                    delay(backoffMs)

                    // If too many consecutive errors, pause and wait longer
                    if (consecutiveErrors >= maxConsecutiveErrors) {
                        logger.error(TAG, "startSync: too many consecutive errors, pausing sync for 5s")
                        delay(5000L)
                        consecutiveErrors = 0 // Reset and try again
                    }
                }
            }
            logger.debug(TAG, "startSync: sync loop ended")
        }
    }

    /**
     * Pauses the synchronization loop.
     *
     * The current state is preserved. Call [startSync] or [resumeSync]
     * to restart synchronization.
     */
    fun pauseSync() {
        syncJob?.cancel()
        syncJob = null
        // FIX P1.4: Use tryEmit for fire-and-forget event emission
        // This avoids potential issues if scope is being cancelled
        val emitted = _syncEvents.tryEmit(SyncEvent.SyncPaused)
        if (!emitted) {
            logger.warn(TAG, "pauseSync: failed to emit SyncPaused event (buffer full)")
        }
        logger.debug(TAG, "pauseSync: sync paused")
    }

    /**
     * Resumes the synchronization loop after pause.
     *
     * Equivalent to [startSync] but emits [SyncEvent.SyncResumed] instead.
     */
    fun resumeSync() {
        if (syncJob?.isActive == true) {
            logger.warn(TAG, "resumeSync: sync already running")
            return
        }

        syncJob = scope.launch {
            logger.debug(TAG, "resumeSync: resuming sync")
            _syncEvents.emit(SyncEvent.SyncResumed)
            consecutiveErrors = 0 // Reset on resume

            while (isActive) {
                try {
                    performSyncCycle()
                    consecutiveErrors = 0
                    delay(config.pollInterval)
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    consecutiveErrors++
                    logger.error(TAG, "resumeSync: sync cycle error ($consecutiveErrors/$maxConsecutiveErrors)", e)
                    handleSyncError(e)

                    // FIX P0.2: Same backoff logic as startSync
                    val backoffMs = minOf(
                        config.pollInterval * (1L shl minOf(consecutiveErrors, 6)),
                        maxBackoffMs
                    )
                    delay(backoffMs)

                    if (consecutiveErrors >= maxConsecutiveErrors) {
                        logger.error(TAG, "resumeSync: too many consecutive errors, pausing sync for 5s")
                        delay(5000L)
                        consecutiveErrors = 0
                    }
                }
            }
        }
    }

    /**
     * Forces an immediate synchronization outside the polling loop.
     *
     * Useful when you know state has changed and want immediate update.
     */
    suspend fun forceSync() {
        logger.debug(TAG, "forceSync: forcing immediate sync")
        performSyncCycle()
    }

    /**
     * Checks if synchronization is currently active.
     */
    val isSyncing: Boolean
        get() = syncJob?.isActive == true

    /**
     * Disposes the synchronizer and cancels any running sync.
     *
     * Call this when the synchronizer is no longer needed.
     */
    fun dispose() {
        syncJob?.cancel()
        syncJob = null
        logger.debug(TAG, "dispose: synchronizer disposed")
    }

    // ==================== Private Implementation ====================

    /**
     * Executes a single synchronization cycle.
     */
    private suspend fun performSyncCycle() {
        _syncedState.update { it.copy(isSyncing = true) }

        try {
            // 1. Get snapshot from C++
            val nativeSnapshot = stateProvider.getEffectChainSnapshot()

            // 2. Check if state changed (fast path via version)
            if (nativeSnapshot.version == lastNativeVersion && lastNativeVersion != 0L) {
                // No changes, skip processing
                _syncedState.update { it.copy(isSyncing = false) }
                return
            }

            // FIX P2.3: Changed to verbose to reduce hot path logging
            logger.debug(TAG, "performSyncCycle: version changed ${lastNativeVersion} -> ${nativeSnapshot.version}, effects=${nativeSnapshot.effects.size}")

            // 3. Detect divergence between current local state and native
            val currentEffects = _syncedState.value.effects
            val divergence = detectDivergence(
                local = currentEffects,
                native = nativeSnapshot.effects
            )

            // 4. Handle divergence if detected
            if (divergence.hasDivergence) {
                handleDivergence(divergence, nativeSnapshot)
            } else {
                // No divergence - reset counter
                if (consecutiveDivergences > 0) {
                    consecutiveDivergences = 0
                    _syncedState.update { it.resetDivergence() }
                }
            }

            // 5. Update local state from native snapshot
            val newEffects = nativeSnapshot.effects.map { it.toEffectState() }
            _syncedState.update { current ->
                current.copy(
                    effects = newEffects,
                    effectsBypassed = nativeSnapshot.isGloballyBypassed,
                    lastSyncTimestamp = nativeSnapshot.timestamp,
                    syncVersion = nativeSnapshot.version,
                    lastError = null,
                    isSyncing = false
                )
            }

            // 6. Update version tracker
            lastNativeVersion = nativeSnapshot.version

            // 7. Emit state updated event (only if we actually processed)
            _syncEvents.emit(SyncEvent.StateUpdated(nativeSnapshot.version))

        } catch (e: Exception) {
            _syncedState.update {
                it.copy(isSyncing = false, lastError = e)
            }
            throw e
        }
    }

    /**
     * Detects divergence between local Kotlin state and native C++ state.
     */
    private fun detectDivergence(
        local: List<EffectState>,
        native: List<NativeEffectSnapshot>
    ): StateDivergence {
        // Check effect count mismatch
        val effectCountMismatch = local.size != native.size

        // Prepare divergence lists
        val parameterDivergences = mutableListOf<ParameterDivergence>()
        val bypassDivergences = mutableListOf<BypassDivergence>()
        val typeMismatches = mutableListOf<TypeMismatch>()

        // Compare effects that exist in both
        val minSize = minOf(local.size, native.size)
        for (i in 0 until minSize) {
            val localEffect = local[i]
            val nativeEffect = native[i]

            // Check type mismatch
            if (localEffect.type.id != nativeEffect.typeId) {
                typeMismatches.add(
                    TypeMismatch(
                        effectIndex = i,
                        localTypeId = localEffect.type.id,
                        nativeTypeId = nativeEffect.typeId
                    )
                )
            }

            // Check bypass mismatch
            if (localEffect.isBypassed != nativeEffect.isBypassed) {
                bypassDivergences.add(
                    BypassDivergence(
                        effectIndex = i,
                        localBypassed = localEffect.isBypassed,
                        nativeBypassed = nativeEffect.isBypassed
                    )
                )
            }

            // Check parameter mismatches
            for ((paramId, nativeValue) in nativeEffect.parameters) {
                val localValue = localEffect.parameters[paramId]
                if (localValue != null && !localValue.isApproximatelyEqual(nativeValue)) {
                    parameterDivergences.add(
                        ParameterDivergence(
                            effectIndex = i,
                            paramId = paramId,
                            localValue = localValue,
                            nativeValue = nativeValue
                        )
                    )
                }
            }
        }

        // Check for order divergence (same types but different order)
        val orderDivergence = if (!effectCountMismatch && typeMismatches.isEmpty()) {
            // Only check order if counts match and types at same indices match
            // This would be when effects were reordered
            false // For now, type mismatch already covers reorder detection
        } else {
            false
        }

        return StateDivergence(
            effectCountMismatch = effectCountMismatch,
            parameterDivergences = parameterDivergences,
            bypassDivergences = bypassDivergences,
            orderDivergence = orderDivergence,
            typeMismatches = typeMismatches
        )
    }

    /**
     * Handles detected divergence based on reconciliation strategy.
     */
    private suspend fun handleDivergence(
        divergence: StateDivergence,
        nativeSnapshot: com.watermellonstudios.audio.api.EffectChainSnapshot
    ) {
        consecutiveDivergences++
        logger.debug(TAG, "handleDivergence: divergence #$consecutiveDivergences: $divergence")

        // Emit divergence event
        _syncEvents.emit(SyncEvent.DivergenceDetected(divergence))

        // Warn if divergences are recurring
        if (consecutiveDivergences >= config.maxDivergenceBeforeWarning) {
            logger.warn(TAG, "handleDivergence: high divergence count ($consecutiveDivergences)")
        }

        // Update divergence counter in state
        _syncedState.update { it.incrementDivergence() }

        // Reconcile based on strategy
        when (config.reconciliationStrategy) {
            ReconciliationStrategy.ACCEPT_NATIVE -> {
                // C++ wins: state will be updated from nativeSnapshot in performSyncCycle
                // Nothing extra to do here, the normal update path handles it
                logger.debug(TAG, "handleDivergence: accepting native state")
            }

            else -> {
                logger.warn(
                    TAG,
                    "handleDivergence: ${config.reconciliationStrategy} is not implemented, using ACCEPT_NATIVE"
                )
            }
        }

        // Emit reconciled event
        _syncEvents.emit(SyncEvent.Reconciled(nativeSnapshot.version))
    }

    /**
     * Handles errors during sync cycle.
     */
    private suspend fun handleSyncError(error: Throwable) {
        _syncedState.update {
            it.copy(lastError = error, isSyncing = false)
        }
        _syncEvents.emit(SyncEvent.SyncError(error))
    }
}

/**
 * Checks if two floats are approximately equal within epsilon.
 */
private fun Float.isApproximatelyEqual(other: Float, epsilon: Float = 0.0001f): Boolean {
    return abs(this - other) < epsilon
}
