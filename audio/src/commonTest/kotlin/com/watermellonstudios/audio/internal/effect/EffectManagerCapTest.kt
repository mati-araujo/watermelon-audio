package com.watermellonstudios.audio.internal.effect

import com.watermellonstudios.audio.api.EffectChainSnapshot
import com.watermellonstudios.audio.api.EffectManagerFactory
import com.watermellonstudios.audio.api.EffectParameterUpdate
import com.watermellonstudios.audio.api.IEffectStateProvider
import com.watermellonstudios.audio.api.IEffectStateWriter
import com.watermellonstudios.audio.api.MaxEffectsReachedException
import com.watermellonstudios.audio.api.NativeEffectSnapshot
import com.watermellonstudios.audio.api.config.AudioEngineConfig
import com.watermellonstudios.audio.domain.device.DeviceCapabilities
import com.watermellonstudios.audio.domain.device.DeviceCapabilitiesSnapshot
import com.watermellonstudios.audio.domain.device.DevicePlatform
import com.watermellonstudios.audio.domain.effect.EffectType
import com.watermellonstudios.audio.internal.sync.StateSynchronizer
import com.watermellonstudios.audio.internal.sync.SyncConfig
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertIs
import kotlin.test.assertTrue

/**
 * El tope de efectos del camino `EffectManagerFactory`, que es **el que usa NoisyPad**.
 *
 * ## Por qué esta suite existe aparte de `DeviceCapabilitiesTest`
 *
 * `AudioEngineConfig.tunedFor()` ya estaba testeado, y aun así el recorte no llegaba a
 * producción: NoisyPad no usa `AudioEngineFactory` **en ninguna parte** —cero
 * ocurrencias en el repo— sino `EffectManagerFactory.create(scope)`, que tomaba
 * `EffectManagerConfig.DEFAULT` sin mirar el dispositivo. Los dos caminos no son dos
 * mitades de un tope: son dos topes independientes.
 *
 * ## Y por qué afirma comportamiento y no el valor de `maxEffects`
 *
 * Es la lección directa de WA-1.2: ahí `AudioEngineConfig.maxEffects` se recortaba bien
 * —y había cuatro tests verdes probándolo— pero **nadie leía el campo**, así que la
 * cadena aceptaba 7 efectos igual. Un test que compara `config.maxEffects == 6` habría
 * seguido pasando. Por eso acá se cuenta cuántos `addEffect` entran de verdad antes de
 * que uno rebote.
 *
 * El control de gama alta no es decorativo: sin él, una implementación que recortara
 * **siempre** pasaría los mismos asserts.
 *
 * ## Lo que esta suite NO cubre
 *
 * Que las cuatro entradas de `EffectManagerFactory` sin config expliciten
 * `tunedDefault()` en vez de `EffectManagerConfig.DEFAULT`. Esa llamada pasa por
 * `getAudioBridge()`, que en el host no existe, y en el simulador de iOS —donde sí
 * existe— el dispositivo **no es de gama baja**, así que las dos versiones darían 12 y
 * la assertion no distinguiría nada. Escribirla igual sería teatro.
 *
 * La mitigación es estructural, no de test: hay **un solo** lugar que arma la config por
 * defecto (`EffectManagerFactory.tunedDefault`), y es el que estos tests ejercitan.
 *
 * ## Verificado por mutación (2026-07-28)
 *
 * 1. `tunedDefault` devuelve `base` sin ajustar → falla sólo *gama baja*.
 * 2. `tunedFor` recorta siempre (ignora `isLowEndDevice`) → falla sólo *gama alta*.
 * 3. **Mutación del fixture**: el writer reporta éxito pero no hace crecer la cadena →
 *    fallan los dos, y por el `assertIs<MaxEffectsReachedException>`, no por el conteo.
 *    Ese guard es lo que impide que un timeout de sync se lea como "rebotó por el tope".
 * 4. La política se duplica (literal `6` acá) y cambia en su fuente (a `5`) → falla
 *    *el recorte usa el umbral de AudioEngineConfig*, que es para lo único que sirve.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class EffectManagerCapTest {

    private fun caps(lowEnd: Boolean) = DeviceCapabilitiesSnapshot(
        platform = DevicePlatform.ANDROID,
        apiLevel = 34,
        totalRamMb = if (lowEnd) 2048 else 8192,
        cpuCoreCount = if (lowEnd) 4 else 8,
        supportsLowLatencyAudio = true,
        isLowEndDevice = lowEnd,
    )

    /**
     * Cadena compartida entre el writer y el provider: el writer la hace crecer, el
     * provider la publica. Sin ese acople el `addEffect` del manager se colgaría
     * esperando una confirmación de sync que nunca llega.
     */
    private class FakeChain : IEffectStateWriter, IEffectStateProvider {
        private val chain = mutableListOf<EffectType>()
        private var version = 1L

        override suspend fun addEffect(type: EffectType): Result<Int> {
            chain.add(type)
            version++
            return Result.success(chain.lastIndex)
        }

        override suspend fun removeEffect(index: Int): Result<Unit> {
            chain.removeAt(index)
            version++
            return Result.success(Unit)
        }

        override suspend fun clearAllEffects(): Result<Unit> {
            chain.clear()
            version++
            return Result.success(Unit)
        }

        override suspend fun setParameter(effectIndex: Int, paramId: Int, value: Float) =
            Result.success(Unit)

        override suspend fun setParametersBatch(effectIndex: Int, parameters: Map<Int, Float>) =
            Result.success(Unit)

        override suspend fun setMultipleEffectParameters(updates: List<EffectParameterUpdate>) =
            Result.success(Unit)

        override suspend fun setBypass(effectIndex: Int, bypassed: Boolean) = Result.success(Unit)

        override suspend fun setEffectsBypass(bypassed: Boolean) = Result.success(Unit)

        override suspend fun reorderEffects(fromIndex: Int, toIndex: Int) = Result.success(Unit)

        override suspend fun getEffectChainSnapshot(): EffectChainSnapshot = EffectChainSnapshot(
            effects = chain.mapIndexed { i, t ->
                NativeEffectSnapshot(index = i, typeId = t.id, isBypassed = false, parameters = emptyMap())
            },
            version = version,
        )

        override suspend fun getEffectParameters(index: Int): Map<Int, Float> = emptyMap()

        override suspend fun isEffectBypassed(index: Int): Boolean = false

        override suspend fun getEffectCount(): Int = chain.size

        override suspend fun getEffectType(index: Int): EffectType? = chain.getOrNull(index)
    }

    private fun managerFor(
        capabilities: DeviceCapabilities,
        scope: CoroutineScope,
    ): EffectManagerImpl {
        val fake = FakeChain()
        return EffectManagerImpl(
            stateWriter = fake,
            synchronizer = StateSynchronizer(fake, scope = scope, config = SyncConfig(pollInterval = 5L)),
            scope = scope,
            // La config que la factory arma para un `create(scope)` sin argumentos.
            config = EffectManagerFactory.tunedDefault(
                base = EffectManagerConfig.TEST,
                capabilities = capabilities,
            ),
        )
    }

    /**
     * Cuántos `addEffect` entran antes de que uno rebote, y con qué error rebota.
     */
    private suspend fun countAcceptedEffects(manager: EffectManagerImpl): Int {
        var accepted = 0
        repeat(AudioEngineConfig.DEFAULT.maxEffects + 1) {
            val result = manager.addEffect(EffectType.FILTER)
            if (result.isFailure) {
                assertIs<MaxEffectsReachedException>(
                    result.exceptionOrNull(),
                    "el rebote tiene que ser el del tope, no un timeout de sync disfrazado",
                )
                return accepted
            }
            accepted++
        }
        return accepted
    }

    @Test
    fun `en gama baja la cadena acepta seis efectos y rebota el septimo`() = runTest(
        UnconfinedTestDispatcher()
    ) {
        val accepted = countAcceptedEffects(managerFor(caps(lowEnd = true), backgroundScope))

        assertEquals(
            AudioEngineConfig.LOW_END_MAX_EFFECTS,
            accepted,
            "el camino de EffectManagerFactory tiene que recortar igual que AudioEngineFactory",
        )
    }

    @Test
    fun `en gama alta la cadena acepta los doce`() = runTest(UnconfinedTestDispatcher()) {
        val accepted = countAcceptedEffects(managerFor(caps(lowEnd = false), backgroundScope))

        assertEquals(
            AudioEngineConfig.DEFAULT.maxEffects,
            accepted,
            "recortar en un dispositivo que no lo necesita seria una regresion, no una mejora",
        )
    }

    /**
     * El umbral sale de `AudioEngineConfig`, no de una copia. Si alguien cambia la
     * política en un solo lado, esto lo delata.
     */
    @Test
    fun `el recorte usa el umbral de AudioEngineConfig`() {
        assertEquals(
            AudioEngineConfig.LOW_END_MAX_EFFECTS,
            EffectManagerConfig.tunedFor(caps(lowEnd = true)).maxEffects,
        )
    }

    /**
     * Recortar, no fijar: un consumidor que pidió menos que el tope de gama baja se
     * queda con lo suyo. Es el mismo invariante que `AudioEngineConfig.tunedFor`.
     */
    @Test
    fun `tunedFor nunca sube el tope que le pasaron`() {
        val pidioTres = EffectManagerConfig(maxEffects = 3)

        assertEquals(3, EffectManagerConfig.tunedFor(caps(lowEnd = true), pidioTres).maxEffects)
        assertEquals(3, EffectManagerConfig.tunedFor(caps(lowEnd = false), pidioTres).maxEffects)
    }

    /**
     * La gama del dispositivo no dice nada sobre cuánto tarda el puente. Si `tunedFor`
     * tocara el timeout, `createForSlowDevice` en un equipo de gama baja perdería
     * justamente el margen que pidió.
     */
    @Test
    fun `tunedFor no toca el timeout de sync`() {
        val tuned = EffectManagerConfig.tunedFor(caps(lowEnd = true), EffectManagerConfig.SLOW_DEVICE)

        assertEquals(EffectManagerConfig.SLOW_DEVICE.syncTimeoutMs, tuned.syncTimeoutMs)
        assertTrue(tuned.maxEffects < EffectManagerConfig.SLOW_DEVICE.maxEffects)
    }
}
