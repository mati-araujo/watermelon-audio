package com.watermellonstudios.audio.domain.device

/**
 * Plataforma sobre la que corre el motor.
 *
 * Existe porque [DeviceCapabilities.apiLevel] **no** es comparable entre
 * plataformas: sin saber de cuál viene, un `35` no significa nada.
 */
enum class DevicePlatform {
    ANDROID,
    IOS,
}

/**
 * Lo que el dispositivo puede dar, en términos que valen en las dos plataformas (WA-1.2).
 *
 * Es deliberadamente una foto de **hechos**, no de decisiones: acá no hay "cuántos
 * efectos usar" ni "qué intervalo de polling". Esa es política y vive en
 * `AudioEngineConfig.tunedFor()`, donde se la puede cambiar sin tocar la detección —
 * y donde el consumidor puede ignorarla y pasar su propia config.
 *
 * La razón de que exista en commonMain es que el `object DeviceCapabilities` de
 * androidMain, que ya encodeaba esta misma idea, pide un `Context` y devuelve tipos
 * de Android. iOS necesitaba lo mismo sin ninguna de las dos cosas.
 *
 * Es una interfaz y no una data class para que el consumidor pueda inyectar la suya
 * —en un test, o en NoisyPad, que ya tiene su propia noción de gama del dispositivo—
 * sin que la librería se lo impida. Para construir una al vuelo está
 * [DeviceCapabilitiesSnapshot].
 */
interface DeviceCapabilities {

    /** Sobre qué plataforma corre esto. Da sentido a [apiLevel]. */
    val platform: DevicePlatform

    /**
     * Nivel de API del sistema, **abstracto**: en Android es `Build.VERSION.SDK_INT`;
     * en iOS, la versión mayor del OS (`15`, `17`, …).
     *
     * Sólo tiene sentido comparado contra otro valor de la **misma** [platform].
     * Sirve para logging, analytics y gating platform-scoped; no para decidir nada
     * cross-platform.
     *
     * `0` si no se pudo determinar.
     */
    val apiLevel: Int

    /**
     * RAM física total del dispositivo en MB, o `0` si no se pudo determinar.
     *
     * Es RAM del **dispositivo**, no presupuesto del proceso: en Android el heap de
     * la app es una fracción de esto, y en iOS el límite antes del jetsam también.
     * Se usa como proxy de gama, que es para lo único que es confiable.
     */
    val totalRamMb: Long

    /**
     * Núcleos de CPU disponibles para el proceso.
     *
     * **No es comparable entre plataformas.** En Android big.LITTLE un octa-core
     * barato rinde menos que un hexa-core de Apple; por eso cada actual lo pondera
     * distinto al derivar [isLowEndDevice].
     */
    val cpuCoreCount: Int

    /**
     * El sistema ofrece un path de audio de baja latencia (AAudio / Core Audio).
     *
     * Es un **hint**: que exista el path no garantiza que el hardware concreto lo
     * conceda. La latencia real recién se conoce con el stream abierto.
     */
    val supportsLowLatencyAudio: Boolean

    /**
     * El dispositivo es de gama baja para correr síntesis en tiempo real.
     *
     * Heurística, y **por plataforma**: cada actual documenta la suya. No hay un
     * umbral universal porque no hay una unidad universal.
     */
    val isLowEndDevice: Boolean
}

/**
 * [DeviceCapabilities] como valor inmutable.
 *
 * Es lo que devuelven los actuals de cada plataforma, y lo que conviene usar en
 * tests para fijar un dispositivo hipotético sin depender del host donde corren.
 */
data class DeviceCapabilitiesSnapshot(
    override val platform: DevicePlatform,
    override val apiLevel: Int,
    override val totalRamMb: Long,
    override val cpuCoreCount: Int,
    override val supportsLowLatencyAudio: Boolean,
    override val isLowEndDevice: Boolean,
) : DeviceCapabilities
