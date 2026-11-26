package com.watermellonstudios.audio.internal.usb

import com.watermellonstudios.audio.domain.usb.StreamPreference
import com.watermellonstudios.audio.domain.usb.UsbCapabilitySnapshot
import com.watermellonstudios.audio.domain.usb.UsbSnapshotCodec
import com.watermellonstudios.audio.internal.bridge.AudioNativeBridge

/**
 * Las operaciones nativas del ciclo de conexión y streaming USB que [UsbAudioManagerImpl]
 * ordena (REQ-050 S2, D14).
 *
 * Existe para poder probar ese orden en el host: ahí `initializeUsbDevice` nunca da `true`
 * (`createUsbAudioBackend()` devuelve `nullptr`), así que ningún test de `connectDevice` pasaba
 * de la inicialización contra el `.so` de host. El resto del manager (volumen, stats,
 * descriptores) sigue hablando con [AudioNativeBridge] directo: lo que pasa por acá es sólo lo
 * que el contrato de conexión necesita afirmar.
 */
internal interface UsbNativePort {
    /** Si el motor existe. Es una lectura: no lo crea. */
    fun isEngineInitialized(): Boolean

    /** `EngineState` nativo: 0 Stopped, 1 Starting, 2 Running, 3 Stopping. */
    fun engineState(): Int

    /**
     * Crea el motor si hace falta e instala su callback en el `BackendManager`
     * (`setUseBackendManager(true)`). El nativo lo ignora con el motor corriendo, por eso
     * [UsbAudioManagerImpl] lo llama sólo con el motor ausente o parado (D6, D12).
     */
    fun installEngineCallback()

    fun initializeUsbDevice(fileDescriptor: Int, usbfsPath: String): Boolean
    fun parseUsbDescriptors(): FloatArray?
    fun isUsbDeviceInitialized(): Boolean
    fun stopUsbStreaming()
    fun closeUsbDevice()
    fun setUsbStreamPreference(preference: StreamPreference): Boolean
    fun selectUsbAltsetting(interfaceNumber: Int, alternateSetting: Int, formatIndex: Int): Boolean

    /**
     * REQ-050 S3 (D17): la selección manual de reloj; 0 la limpia (selección automática). Pasa
     * por acá, junto con la de altsetting, para que el test de host afirme el centinela.
     */
    fun selectUsbClockSource(clockSourceId: Int): Boolean

    /**
     * REQ-050 S3: el snapshot de descriptores ya decodificado, o null si no hay device. Tira si
     * los bytes no se pueden decodificar (el manager lo registra y conserva el último bueno).
     */
    fun capabilitySnapshot(): UsbCapabilitySnapshot?

    /** Uno de los valores de `UsbStreamStartStatus` (AC-050.3). */
    fun startUsbStreamingWithModeStatus(sampleRate: Int, channels: Int, bitDepth: Int, streamingMode: Int): Int
}

/** La implementación de producción: todo va al [AudioNativeBridge]. */
internal class BridgeUsbNativePort(private val bridge: AudioNativeBridge) : UsbNativePort {
    override fun isEngineInitialized(): Boolean = bridge.isEngineInitialized()
    override fun engineState(): Int = bridge.getEngineState()
    override fun installEngineCallback() = bridge.setUseBackendManager(true)
    override fun initializeUsbDevice(fileDescriptor: Int, usbfsPath: String): Boolean =
        bridge.initializeUsbDevice(fileDescriptor, usbfsPath)
    override fun parseUsbDescriptors(): FloatArray? = bridge.parseUsbDescriptors()
    override fun isUsbDeviceInitialized(): Boolean = bridge.isUsbDeviceInitialized()
    override fun stopUsbStreaming() = bridge.stopUsbStreaming()
    override fun closeUsbDevice() = bridge.closeUsbDevice()
    override fun setUsbStreamPreference(preference: StreamPreference): Boolean =
        bridge.setUsbStreamPreference(preference)
    override fun selectUsbAltsetting(interfaceNumber: Int, alternateSetting: Int, formatIndex: Int): Boolean =
        bridge.selectUsbAltsetting(interfaceNumber, alternateSetting, formatIndex)
    override fun selectUsbClockSource(clockSourceId: Int): Boolean = bridge.selectUsbClockSource(clockSourceId)
    override fun capabilitySnapshot(): UsbCapabilitySnapshot? =
        bridge.getUsbCapabilitySnapshot()?.let { UsbSnapshotCodec.decode(it) }
    override fun startUsbStreamingWithModeStatus(sampleRate: Int, channels: Int, bitDepth: Int, streamingMode: Int): Int =
        bridge.startUsbStreamingWithModeStatus(sampleRate, channels, bitDepth, streamingMode)
}
