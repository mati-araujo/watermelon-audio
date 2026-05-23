package com.watermellonstudios.audio.internal.effect

import com.watermellonstudios.audio.api.EffectChainSnapshot
import com.watermellonstudios.audio.api.EffectNotFoundException
import com.watermellonstudios.audio.api.EffectParameterUpdate
import com.watermellonstudios.audio.api.IEffectStateProvider
import com.watermellonstudios.audio.api.IEffectStateWriter
import com.watermellonstudios.audio.api.NativeEffectSnapshot
import com.watermellonstudios.audio.domain.effect.EffectType
import com.watermellonstudios.audio.internal.sync.StateSynchronizer
import com.watermellonstudios.audio.internal.sync.SyncConfig
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * AUD-6 coverage: scene loads must apply N effect parameters across the chain
 * in a single JNI roundtrip with a single state-version bump.
 *
 * These tests verify the [EffectManagerImpl.setEffectParametersBatch] path that
 * NoisyPad's `EffectsViewModel.handleLoadScene` should migrate to.
 */
class EffectManagerBatchTest {

    /** Fake writer that records every call instead of touching JNI. */
    private class FakeWriter(
        val initialChain: List<EffectType> = emptyList()
    ) : IEffectStateWriter {
        val batchCalls = mutableListOf<List<EffectParameterUpdate>>()
        var singleSetParameterCalls = 0
        var setParametersBatchCalls = 0
        var setBypassCalls = 0
        var clearAllCalls = 0

        override suspend fun addEffect(type: EffectType): Result<Int> =
            Result.success(0)

        override suspend fun removeEffect(index: Int): Result<Unit> =
            Result.success(Unit)

        override suspend fun setParameter(effectIndex: Int, paramId: Int, value: Float): Result<Unit> {
            singleSetParameterCalls++
            return Result.success(Unit)
        }

        override suspend fun setParametersBatch(
            effectIndex: Int,
            parameters: Map<Int, Float>
        ): Result<Unit> {
            setParametersBatchCalls++
            return Result.success(Unit)
        }

        override suspend fun setMultipleEffectParameters(
            updates: List<EffectParameterUpdate>
        ): Result<Unit> {
            // Snapshot the list to detect tearing — if some caller mutated the
            // list mid-flight the recorded snapshot would diverge from the
            // declared expectation.
            batchCalls.add(updates.toList())
            return Result.success(Unit)
        }

        override suspend fun setBypass(effectIndex: Int, bypassed: Boolean): Result<Unit> {
            setBypassCalls++
            return Result.success(Unit)
        }

        override suspend fun reorderEffects(fromIndex: Int, toIndex: Int): Result<Unit> =
            Result.success(Unit)

        override suspend fun clearAllEffects(): Result<Unit> {
            clearAllCalls++
            return Result.success(Unit)
        }
    }

    /**
     * Fake provider that returns whatever chain we hand it. Version bumps only
     * when we explicitly call [bumpVersion] so the test controls when the
     * synchronizer should observe a state change.
     */
    private class FakeProvider(
        initialChain: List<EffectType> = emptyList()
    ) : IEffectStateProvider {
        private var chain: List<NativeEffectSnapshot> = initialChain.mapIndexed { i, t ->
            NativeEffectSnapshot(index = i, typeId = t.id, isBypassed = false, parameters = emptyMap())
        }
        private var version: Long = 1L

        fun setChain(types: List<EffectType>) {
            chain = types.mapIndexed { i, t ->
                NativeEffectSnapshot(index = i, typeId = t.id, isBypassed = false, parameters = emptyMap())
            }
            version++
        }

        fun bumpVersion() {
            version++
        }

        override suspend fun getEffectChainSnapshot(): EffectChainSnapshot =
            EffectChainSnapshot(effects = chain, version = version)

        override suspend fun getEffectParameters(index: Int): Map<Int, Float> =
            chain.getOrNull(index)?.parameters.orEmpty()

        override suspend fun isEffectBypassed(index: Int): Boolean =
            chain.getOrNull(index)?.isBypassed ?: false

        override suspend fun getEffectCount(): Int = chain.size

        override suspend fun getEffectType(index: Int): EffectType? =
            chain.getOrNull(index)?.let { EffectType.fromId(it.typeId) }
    }

    @Test
    fun `batch with 10 effects x 5 params reaches writer as single call`() = runTest(
        UnconfinedTestDispatcher()
    ) {
        val chain = List(10) { EffectType.FILTER }
        val writer = FakeWriter()
        val provider = FakeProvider(initialChain = chain)
        val sync = StateSynchronizer(provider, scope = backgroundScope, config = SyncConfig(pollInterval = 10L))
        val manager = EffectManagerImpl(
            stateWriter = writer,
            synchronizer = sync,
            scope = backgroundScope,
            config = EffectManagerConfig.TEST
        )
        // Wait until effectsState reflects the 10 effects in the provider.
        manager.effectsState.first { it.size == 10 }

        val updates = (0 until 10).flatMap { effectIndex ->
            (0 until 5).map { paramId ->
                EffectParameterUpdate(effectIndex, paramId, 0.25f + paramId * 0.1f)
            }
        }
        val result = manager.setEffectParametersBatch(updates)

        assertTrue(result.isSuccess, "batch call should succeed")
        assertEquals(1, writer.batchCalls.size, "exactly one JNI batch call (not 50)")
        assertEquals(50, writer.batchCalls.single().size, "all 50 updates delivered")
        // No fallback to single-param path.
        assertEquals(0, writer.singleSetParameterCalls)
        assertEquals(0, writer.setParametersBatchCalls)

        sync.dispose()
    }

    @Test
    fun `effectsState emits once for a single batch`() = runTest(UnconfinedTestDispatcher()) {
        val chain = List(10) { EffectType.FILTER }
        val writer = FakeWriter()
        val provider = FakeProvider(initialChain = chain)
        val sync = StateSynchronizer(provider, scope = backgroundScope, config = SyncConfig(pollInterval = 10L))
        val manager = EffectManagerImpl(
            stateWriter = writer,
            synchronizer = sync,
            scope = backgroundScope,
            config = EffectManagerConfig.TEST
        )
        manager.effectsState.first { it.size == 10 }

        // Subscribe to count emissions while we issue the batch.
        var emissions = 0
        val job = launch {
            manager.effectsState.collect {
                emissions++
                if (emissions >= 3) return@collect // bail out — we only expect 1
            }
        }
        // Reset counter after the initial replay-value emission.
        advanceUntilIdle()
        val baseline = emissions

        // Native side handles the whole batch with exactly one version bump.
        // Simulate that single bump after the batch lands.
        val updates = (0 until 10).flatMap { e -> (0 until 5).map { p -> EffectParameterUpdate(e, p, 0.5f) } }
        manager.setEffectParametersBatch(updates)
        provider.bumpVersion()
        advanceUntilIdle()

        val delta = emissions - baseline
        // Synchronizer cycles may emit at most once: the data does not actually
        // change (params are tracked separately), so often delta == 0. The
        // critical assertion is that we do not get N emissions.
        assertTrue(delta <= 1, "effectsState must emit at most once for the batch, got $delta")
        job.cancel()
        sync.dispose()
    }

    @Test
    fun `out of range effect index fails fast without writer call`() = runTest(
        UnconfinedTestDispatcher()
    ) {
        val chain = List(3) { EffectType.FILTER }
        val writer = FakeWriter()
        val provider = FakeProvider(initialChain = chain)
        val sync = StateSynchronizer(provider, scope = backgroundScope, config = SyncConfig(pollInterval = 10L))
        val manager = EffectManagerImpl(
            stateWriter = writer,
            synchronizer = sync,
            scope = backgroundScope,
            config = EffectManagerConfig.TEST
        )
        manager.effectsState.first { it.size == 3 }

        val updates = listOf(
            EffectParameterUpdate(effectIndex = 0, paramId = 0, value = 0.5f),
            EffectParameterUpdate(effectIndex = 99, paramId = 0, value = 0.5f), // out of range
        )
        val result = manager.setEffectParametersBatch(updates)

        assertTrue(result.isFailure)
        assertTrue(result.exceptionOrNull() is EffectNotFoundException)
        assertEquals(0, writer.batchCalls.size, "writer must not be called when validation fails")

        sync.dispose()
    }

    @Test
    fun `empty updates short circuits without writer call`() = runTest(UnconfinedTestDispatcher()) {
        val writer = FakeWriter()
        val provider = FakeProvider()
        val sync = StateSynchronizer(provider, scope = backgroundScope, config = SyncConfig(pollInterval = 10L))
        val manager = EffectManagerImpl(
            stateWriter = writer,
            synchronizer = sync,
            scope = backgroundScope,
            config = EffectManagerConfig.TEST
        )

        val result = manager.setEffectParametersBatch(emptyList())

        assertTrue(result.isSuccess)
        assertEquals(0, writer.batchCalls.size)
        sync.dispose()
    }
}
