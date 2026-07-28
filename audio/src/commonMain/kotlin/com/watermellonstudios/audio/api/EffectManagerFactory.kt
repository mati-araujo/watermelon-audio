package com.watermellonstudios.audio.api

import com.watermellonstudios.audio.api.config.AudioEngineConfig
import com.watermellonstudios.audio.domain.device.DeviceCapabilities
import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import com.watermellonstudios.audio.internal.effect.EffectManagerConfig
import com.watermellonstudios.audio.internal.effect.EffectManagerImpl
import com.watermellonstudios.audio.internal.sync.StateSynchronizer
import com.watermellonstudios.audio.internal.sync.SyncConfig
import kotlinx.coroutines.CoroutineScope

/**
 * Factory for creating [IEffectManager] instances.
 *
 * This factory provides a clean public API for creating effect managers
 * without exposing internal implementation details.
 *
 * Usage:
 * ```kotlin
 * // In ViewModel or DI module
 * val effectManager = EffectManagerFactory.create(viewModelScope)
 *
 * // Start synchronization
 * effectManager.startSync() // If using StateSynchronizer internally
 *
 * // Use the manager
 * effectManager.addEffect(EffectType.REVERB)
 *     .onSuccess { effect -> /* handle */ }
 *     .onFailure { error -> /* handle */ }
 *
 * // Clean up when done
 * effectManager.dispose()
 * ```
 *
 * For Hilt/Dagger integration:
 * ```kotlin
 * @Module
 * @InstallIn(ViewModelComponent::class)
 * object EffectModule {
 *     @Provides
 *     fun provideEffectManager(
 *         @ViewModelScoped scope: CoroutineScope
 *     ): IEffectManager = EffectManagerFactory.create(scope)
 * }
 * ```
 *
 * @see IEffectManager
 */
object EffectManagerFactory {

    /**
     * Creates an [IEffectManager] with default configuration.
     *
     * This is the recommended way to create an effect manager for most use cases.
     * The returned manager uses:
     * - Default sync polling interval (50ms)
     * - Default sync timeout (500ms)
     * - `maxEffects` = 12, **recortado a [AudioEngineConfig.LOW_END_MAX_EFFECTS] en un
     *   dispositivo de gama baja** — ver la nota de abajo.
     *
     * ## El tope de efectos se ajusta al dispositivo
     *
     * Igual que [AudioEngineFactory.create], esta entrada pasa su config por
     * [EffectManagerConfig.tunedFor] con [currentDeviceCapabilities]. Antes no lo hacía,
     * y como este es el camino que usa un consumidor que no toca [AudioEngineFactory],
     * el recorte de WA-1.2 no llegaba a producción por ningún lado.
     *
     * Sólo **recorta**: nunca sube el tope. Y sólo aplica a las entradas que no reciben
     * una config explícita — si el consumidor pasa la suya
     * ([create] de tres argumentos), se respeta tal cual.
     *
     * @param scope CoroutineScope for StateFlow operations and synchronization.
     *              Typically viewModelScope or a dedicated lifecycle-aware scope.
     * @return New [IEffectManager] instance ready for use
     */
    fun create(scope: CoroutineScope): IEffectManager {
        return create(
            scope = scope,
            syncConfig = SyncConfig.DEFAULT,
            effectConfig = tunedDefault()
        )
    }

    /**
     * La config por defecto ya ajustada al dispositivo.
     *
     * Existe como función y no como constante porque [currentDeviceCapabilities] es una
     * consulta al sistema (cacheada en las dos plataformas, así que llamarla por cada
     * `create` no cuesta I/O), y porque una constante de nivel superior se evaluaría al
     * cargar la clase, antes de que el consumidor pueda hacer nada al respecto.
     */
    internal fun tunedDefault(
        base: EffectManagerConfig = EffectManagerConfig.DEFAULT,
        capabilities: DeviceCapabilities = currentDeviceCapabilities(),
    ) = EffectManagerConfig.tunedFor(capabilities, base)

    /**
     * Creates an [IEffectManager] with custom configuration.
     *
     * Use this when you need to customize behavior for specific devices
     * or testing scenarios.
     *
     * @param scope CoroutineScope for StateFlow operations
     * @param syncConfig Configuration for state synchronization polling
     * @param effectConfig Configuration for effect manager behavior
     * @return New [IEffectManager] instance
     */
    // El motor es el implementador del puente, no un consumidor: las factories
    // publicas se construyen encima de el. Ver [InternalWatermelonApi].
    @OptIn(InternalWatermelonApi::class)
    fun create(
        scope: CoroutineScope,
        syncConfig: SyncConfig,
        effectConfig: EffectManagerConfig
    ): IEffectManager {
        // Get the singleton native bridge instance
        val nativeBridge = getAudioBridge()

        // Create synchronizer with provided config
        val synchronizer = StateSynchronizer(
            stateProvider = nativeBridge,
            scope = scope,
            config = syncConfig
        )

        // Create and return the effect manager
        return EffectManagerImpl(
            stateWriter = nativeBridge,
            synchronizer = synchronizer,
            scope = scope,
            config = effectConfig
        )
    }

    /**
     * Creates an [IEffectManager] using an existing [StateSynchronizer].
     *
     * Use this when you want to share a single synchronizer across multiple
     * components, or when the synchronizer is already being used elsewhere
     * (e.g., in AudioEngineStateManager).
     *
     * @param scope CoroutineScope for StateFlow operations
     * @param synchronizer Existing StateSynchronizer instance
     * @param effectConfig Configuration for effect manager behavior
     * @return New [IEffectManager] instance using the shared synchronizer
     */
    // El motor es el implementador del puente, no un consumidor: las factories
    // publicas se construyen encima de el. Ver [InternalWatermelonApi].
    @OptIn(InternalWatermelonApi::class)
    fun createWithSynchronizer(
        scope: CoroutineScope,
        synchronizer: StateSynchronizer,
        effectConfig: EffectManagerConfig = tunedDefault()
    ): IEffectManager {
        val nativeBridge = getAudioBridge()

        return EffectManagerImpl(
            stateWriter = nativeBridge,
            synchronizer = synchronizer,
            scope = scope,
            config = effectConfig
        )
    }

    /**
     * Creates an [IEffectManager] optimized for battery saving.
     *
     * Uses slower polling intervals to reduce CPU usage.
     * Best for background operation or when battery is low.
     *
     * @param scope CoroutineScope for StateFlow operations
     * @return New [IEffectManager] instance with battery-saving config
     */
    fun createBatterySaver(scope: CoroutineScope): IEffectManager {
        return create(
            scope = scope,
            syncConfig = SyncConfig.BATTERY_SAVER,
            effectConfig = tunedDefault()
        )
    }

    /**
     * Creates an [IEffectManager] with aggressive sync for maximum responsiveness.
     *
     * Uses faster polling intervals for the most responsive UI.
     * Higher CPU usage - use only when needed.
     *
     * @param scope CoroutineScope for StateFlow operations
     * @return New [IEffectManager] instance with aggressive config
     */
    fun createAggressive(scope: CoroutineScope): IEffectManager {
        return create(
            scope = scope,
            syncConfig = SyncConfig.AGGRESSIVE,
            effectConfig = tunedDefault()
        )
    }

    /**
     * Creates an [IEffectManager] optimized for slow devices.
     *
     * Uses longer timeouts to accommodate slower devices while maintaining
     * reliability.
     *
     * "Lento" y "gama baja" son dos ejes distintos —el timeout habla del puente, el
     * tope de efectos del CPU— así que esta entrada pasa igual por [tunedDefault]:
     * si además resulta ser de gama baja, recorta.
     *
     * @param scope CoroutineScope for StateFlow operations
     * @return New [IEffectManager] instance with slow device config
     */
    fun createForSlowDevice(scope: CoroutineScope): IEffectManager {
        return create(
            scope = scope,
            syncConfig = SyncConfig.DEFAULT,
            effectConfig = tunedDefault(EffectManagerConfig.SLOW_DEVICE)
        )
    }
}
