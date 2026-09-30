package com.watermellonstudios.audio.harness.usb

import android.content.Context
import com.watermellonstudios.audio.api.IAudioNativeBridge
import com.watermellonstudios.audio.api.IUsbAudioManager
import com.watermellonstudios.audio.api.InternalWatermelonApi
import com.watermellonstudios.audio.api.UsbAudioManagerFactory
import com.watermellonstudios.audio.api.UsbAudioTestRunnerFactory
import com.watermellonstudios.audio.domain.AudioBackendType
import com.watermellonstudios.audio.domain.usb.UsbAudioDevice
import com.watermellonstudios.audio.domain.usb.UsbAudioError
import com.watermellonstudios.audio.domain.usb.UsbCapabilitySnapshot
import com.watermellonstudios.audio.domain.usb.UsbConnectionState
import com.watermellonstudios.audio.domain.usb.UsbResult
import com.watermellonstudios.audio.domain.usb.UsbTestPresets
import com.watermellonstudios.audio.domain.usb.UsbTestResult
import com.watermellonstudios.audio.domain.usb.UsbTestStatus
import com.watermellonstudios.audio.domain.usb.UsbTransferStats
import com.watermellonstudios.audio.harness.smoke.SmokeReporter
import com.watermellonstudios.audio.harness.smoke.SuiteRowVerdict
import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull

/**
 * MINI-038 — el camino USB del harness sobre la API pública de la librería: [IUsbAudioManager]
 * (el fd que entrega `UsbManager.openDevice()` → `initializeUsbDevice` → libusb) y
 * `UsbAudioTestRunner`. Lo usan igual el panel ([UsbPanel]) y el plan por adb ([runAutomatic]):
 * cada método emite su línea `HARNESS-SMOKE panel=usb` con lo MEDIDO.
 *
 * Reglas que vienen de lo que ya se aprendió en este harness:
 * - El backend se verifica con `getCurrentBackendType()`, nunca con el `Boolean` de
 *   `selectBackend` (ítem 10 del smoke de WA-5.5: devuelve `true` aunque caiga a Oboe).
 * - Un `UsbResult.Failure` se muestra con su error tipado y su mensaje; nunca un `catch` mudo.
 * - El diálogo de permiso USB es un gesto humano: se emite `step=esperando-humano` con la acción
 *   exacta y se ESPERA. No se acepta por adb ni se simula.
 */
@OptIn(InternalWatermelonApi::class)
class UsbHarness(context: Context) {

    val manager: IUsbAudioManager = UsbAudioManagerFactory.create(context)
    private val bridge: IAudioNativeBridge = getAudioBridge()
    private val runner = UsbAudioTestRunnerFactory.create(manager)

    init {
        // Sin monitoreo no hay receiver registrado, y el resultado del diálogo de permiso nunca
        // llega: `connectDevice` quedaría esperando para siempre.
        manager.startMonitoring()
    }

    fun release() {
        // `manager.release()` lanza el disconnect en su scope y enseguida lo cancela, así que el
        // disconnect puede no correr nunca y libusb quedaría inicializado con el fd viejo. Se
        // desconecta antes, con techo: esto corre en `onDestroy`.
        if (manager.isDeviceReady()) {
            runBlocking { withTimeoutOrNull(RELEASE_DISCONNECT_MS) { manager.disconnectDevice() } }
        }
        runner.release()
        manager.stopMonitoring()
        manager.release()
    }

    /** `step=dispositivos`: vale si hay al menos un dispositivo de audio USB. */
    suspend fun listDevices(r: SmokeReporter): List<UsbAudioDevice> {
        manager.refreshDevices()
        val devices = manager.getConnectedDevices()
        r.report(
            PANEL, "dispositivos", devices.isNotEmpty(),
            "cantidad" to devices.size,
            "lista" to devices.joinToString(";") { describe(it) }.ifEmpty { null },
            "host-soportado" to manager.isUsbAudioSupported(),
        )
        return devices
    }

    /**
     * Permiso + conexión + lo que AC-1 pide ver: fd, UAC, capacidades, descriptores y el backend
     * activo. Si falta el permiso, emite `esperando-humano` y espera hasta [humanTimeoutMs].
     */
    suspend fun connect(r: SmokeReporter, device: UsbAudioDevice, humanTimeoutMs: Long): Boolean {
        val hadPermission = manager.hasPermission(device)
        if (hadPermission) {
            r.report(PANEL, "permiso", true, "origen" to "ya-concedido", "dispositivo" to device.vidPid)
        } else {
            r.waitingForHuman(
                PANEL,
                "aceptar_el_dialogo_de_permiso_USB_de_WMA_Harness_para_${device.vidPid}_en_el_telefono",
                "dispositivo" to device.vidPid, "espera-max-s" to humanTimeoutMs / 1000,
            )
        }

        val result = withTimeoutOrNull(humanTimeoutMs) { manager.connectDevice(device) }
        if (result == null) {
            r.report(
                PANEL, "conectar", false,
                "motivo" to "sin-respuesta-humana", "espera-s" to humanTimeoutMs / 1000,
                "estado" to manager.connectionState.value,
            )
            return false
        }
        if (!hadPermission) {
            val denied = (result as? UsbResult.Failure)?.error == UsbAudioError.PERMISSION_DENIED
            r.report(PANEL, "permiso", !denied, "origen" to "dialogo", "concedido" to !denied)
        }

        val fd = manager.getFileDescriptor()
        val ready = manager.isDeviceReady()
        val connected = r.report(
            PANEL, "conectar", result is UsbResult.Success && fd > 0 && ready,
            "fd" to fd, "listo" to ready, "uac" to manager.getUacVersion(),
            "usbfs" to manager.getUsbfsPath(), "estado" to manager.connectionState.value,
            "error" to (result as? UsbResult.Failure)?.error, "mensaje" to (result as? UsbResult.Failure)?.message,
        )
        if (!connected) return false

        val caps = manager.getDeviceCapabilities(device)
        val c = caps.getOrNull()
        r.report(
            PANEL, "capacidades", c != null,
            "rates" to c?.supportedSampleRates?.joinToString(","), "bits" to c?.supportedBitDepths?.joinToString(","),
            "canales-out" to c?.maxChannelsOutput, "canales-in" to c?.maxChannelsInput,
            "sync" to c?.syncMode, "full-duplex" to c?.supportsFullDuplex, "captura" to manager.hasCapture(),
            "error" to (caps as? UsbResult.Failure)?.error, "mensaje" to (caps as? UsbResult.Failure)?.message,
        )

        val snapshot = awaitSnapshot()
        r.report(
            PANEL, "descriptores", snapshot != null && snapshot.playbackAltsettings.isNotEmpty(),
            "uac" to snapshot?.uacVersion,
            "alt-playback" to snapshot?.playbackAltsettings?.size,
            "alt-captura" to snapshot?.captureAltsettings?.size,
            "relojes" to snapshot?.clockSources?.size,
            "formatos-playback" to snapshot?.let { formats(it) },
            "motivo" to if (snapshot == null) "sin-snapshot" else null,
        )

        return selectBackend(r, AudioBackendType.LIBUSB, "backend")
    }

    /** `step=<step>`: pide [wanted] y afirma lo que el motor REPORTA después. */
    fun selectBackend(r: SmokeReporter, wanted: AudioBackendType, step: String): Boolean {
        val returned = bridge.selectBackend(wanted.id)
        val actual = AudioBackendType.fromId(bridge.getCurrentBackendType())
        return r.report(
            PANEL, step, actual == wanted,
            "pedido" to wanted, "select-devolvio" to returned, "reporta" to actual,
        )
    }

    /** `step=streaming-start` y, después de [warmupMs], `step=streaming-stats`. */
    suspend fun startStreaming(r: SmokeReporter, warmupMs: Long = 2000): Boolean {
        val bits = manager.getCurrentCapabilitySnapshot()?.effectiveOutputBitDepths
            ?.let { if (24 in it) 24 else it.firstOrNull() } ?: 16
        val result = manager.startStreaming(sampleRate = STREAM_RATE_HZ, channels = 2, bitDepth = bits)
        val started = r.report(
            PANEL, "streaming-start", result is UsbResult.Success,
            "rate" to STREAM_RATE_HZ, "canales" to 2, "bits" to bits, "estado" to manager.connectionState.value,
            "error" to (result as? UsbResult.Failure)?.error, "mensaje" to (result as? UsbResult.Failure)?.message,
        )
        if (!started) return false
        delay(warmupMs)
        return reportStats(r, "streaming-stats")
    }

    fun reportStats(r: SmokeReporter, step: String): Boolean {
        val s: UsbTransferStats? = manager.getTransferStats()
        // El rate medido sólo existe con feedback (async o implícito por captura); sin él vale 0 y no
        // se puede verificar — se dice, no se da por bueno ni por malo. Si existe y no es el pedido,
        // es un rate mal negociado: FAIL.
        val measured = s?.currentSampleRateHz
        val contradicts = SuiteRowVerdict.rateContradicts(measured, STREAM_RATE_HZ)
        return r.report(
            PANEL, step, s != null && s.packetsCompleted > 0 && !contradicts,
            "enviados" to s?.packetsSubmitted, "completados" to s?.packetsCompleted,
            "errores" to s?.packetsErrors, "underruns" to s?.underruns, "overruns" to s?.overruns,
            "rate-pedido" to STREAM_RATE_HZ, "rate-real" to measured,
            "rate-verificable" to (measured != null && measured > 0f), "latencia-ms" to s?.avgLatencyMs,
            "motivo" to when {
                s == null -> "sin-stats"
                contradicts -> "rate-real-distinto-del-pedido"
                s.packetsCompleted <= 0 -> "sin-paquetes"
                else -> null
            },
        )
    }

    fun stopStreaming(r: SmokeReporter): Boolean {
        manager.stopStreaming()
        val state = manager.connectionState.value
        return r.report(PANEL, "streaming-stop", state != UsbConnectionState.STREAMING, "estado" to state)
    }

    /**
     * AC-2: la suite estándar de `UsbAudioTestRunner`. Una línea `step=suite-<n>` por resultado
     * (su `ok` es `status == PASSED`: un test que falla se ve como falla) y un `step=suite` final.
     */
    suspend fun runSuite(r: SmokeReporter, device: UsbAudioDevice, onResult: (UsbTestResult) -> Unit = {}): Boolean {
        val report = runner.runTestSuite(
            configs = UsbTestPresets.STANDARD_SUITE,
            deviceName = device.displayName,
            deviceVidPid = device.vidPid,
            uacVersion = manager.getUacVersion(),
        )
        var passed = 0
        var measured = 0
        report.results.forEachIndexed { i, res ->
            onResult(res)
            val real = res.statsSamples.lastOrNull()?.currentSampleRateHz
            val first = res.statsSamples.firstOrNull()?.packetsCompleted
            val last = res.statsSamples.lastOrNull()?.packetsCompleted
            // D11: tres veredictos. El PASSED de la librería no alcanza (también sale con las stats
            // nulas o el stream trabado), y una fila cuyo rate el runner no aplicó no midió ese rate.
            val verdict = suiteVerdict(res)
            val fields = arrayOf<Pair<String, Any?>>(
                "test" to res.testType, "estado" to res.status,
                "trafico" to (first != null && last != null && last > first), "muestras" to res.statsSamples.size,
                "rate-config" to res.config.sampleRate, "rate-stream" to STREAM_RATE_HZ,
                "bits-config" to res.config.bitDepth, "rate-real" to real,
                "paquetes" to res.totalPackets, "ok-paquetes" to res.successfulPackets,
                "underruns" to res.underruns, "overruns" to res.overruns, "errores" to res.errors,
                "latencia-ms" to res.avgLatencyMs, "mensaje" to res.errorMessage,
            )
            val step = "suite-${i + 1}"
            when (verdict) {
                SuiteRowVerdict.NOT_MEASURED ->
                    r.notMeasured(PANEL, step, "rate-no-aplicado:el-runner-ignora-config.sampleRate", *fields)
                SuiteRowVerdict.PASS -> {
                    measured++
                    passed++
                    r.report(PANEL, step, true, *fields)
                }
                SuiteRowVerdict.FAIL -> {
                    measured++
                    r.report(
                        PANEL, step, false, *fields,
                        "motivo" to when {
                            first == null || last == null || last <= first -> "sin-trafico"
                            SuiteRowVerdict.rateContradicts(real, STREAM_RATE_HZ) -> "rate-real-distinto-del-pedido"
                            else -> "estado-${res.status}"
                        },
                    )
                }
            }
        }
        // El verde de la suite lo deciden las filas MEDIDAS (D11): tiene que haber al menos una, y
        // todas tienen que pasar. Las no medidas se listan aparte y no cuentan como cobertura.
        val expected = UsbTestPresets.STANDARD_SUITE.size
        return r.report(
            PANEL, "suite", report.results.size == expected && measured > 0 && passed == measured,
            "tests" to report.results.size, "medidas" to measured, "pasaron" to passed,
            "no-medidas" to report.results.size - measured, "esperados" to expected,
        )
    }

    /** Vuelve a Oboe y suelta el dispositivo. */
    suspend fun disconnect(r: SmokeReporter): Boolean {
        val restored = selectBackend(r, AudioBackendType.OBOE, "backend-restaurado")
        manager.disconnectDevice()
        val fd = manager.getFileDescriptor()
        val state = manager.connectionState.value
        val released = r.report(
            PANEL, "desconectar", fd < 0 && !manager.isDeviceReady(),
            "fd" to fd, "estado" to state,
        )
        return restored && released
    }

    /**
     * La parte USB del plan por adb: dispositivos → permiso (humano) → conexión por libusb →
     * streaming con stats → suite → desconexión. Lo que falla no frena el cierre: la desconexión y
     * la vuelta a Oboe corren siempre que hubo conexión.
     */
    suspend fun runAutomatic(r: SmokeReporter, humanTimeoutMs: Long): Boolean {
        val device = listDevices(r).firstOrNull() ?: return false
        if (!connect(r, device, humanTimeoutMs)) {
            if (manager.isDeviceReady()) disconnect(r)
            return false
        }
        var ok = true
        try {
            ok = startStreaming(r) && ok
            ok = runSuite(r, device) && ok
            ok = stopStreaming(r) && ok
        } finally {
            ok = disconnect(r) && ok
        }
        return ok
    }

    private suspend fun awaitSnapshot(deadlineMs: Long = 2000): UsbCapabilitySnapshot? {
        var waited = 0L
        while (waited <= deadlineMs) {
            manager.getCurrentCapabilitySnapshot()?.let { return it }
            delay(100)
            waited += 100
        }
        return null
    }

    companion object {
        const val PANEL = "usb"

        /** El rate al que el harness abre el stream USB. La suite mide ESTE stream (D11). */
        const val STREAM_RATE_HZ = 48_000
        private const val RELEASE_DISCONNECT_MS = 1000L

        /** El veredicto D11 de una fila, con los datos del resultado de la librería. */
        fun suiteVerdict(res: UsbTestResult): SuiteRowVerdict = SuiteRowVerdict.of(
            libraryPassed = res.status == UsbTestStatus.PASSED,
            firstCompleted = res.statsSamples.firstOrNull()?.packetsCompleted,
            lastCompleted = res.statsSamples.lastOrNull()?.packetsCompleted,
            measuredRateHz = res.statsSamples.lastOrNull()?.currentSampleRateHz,
            rowRateHz = res.config.sampleRate,
            streamRateHz = STREAM_RATE_HZ,
        )

        fun describe(d: UsbAudioDevice): String =
            "${d.vidPid}|${d.displayName}|UAC${d.capabilities.uacVersion}|captura=${d.capabilities.hasCapture}"

        private fun formats(s: UsbCapabilitySnapshot): String =
            s.playbackAltsettings.joinToString(",") { alt ->
                "${alt.interfaceNumber}/${alt.alternateSetting}:" + alt.formats.joinToString("+") { f ->
                    "${f.channels}ch${f.bitResolution}b@" + f.sampleRates.joinToString("/")
                }
            }
    }
}
