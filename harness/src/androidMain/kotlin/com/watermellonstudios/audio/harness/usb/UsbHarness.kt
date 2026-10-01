package com.watermellonstudios.audio.harness.usb

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
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
import com.watermellonstudios.audio.domain.usb.UsbDeviceEvent
import com.watermellonstudios.audio.domain.usb.UsbResult
import com.watermellonstudios.audio.domain.usb.UsbTestPresets
import com.watermellonstudios.audio.domain.usb.UsbTestResult
import com.watermellonstudios.audio.domain.usb.UsbTestStatus
import com.watermellonstudios.audio.domain.usb.UsbTransferStats
import com.watermellonstudios.audio.harness.smoke.ENGINE_STATE_STOPPED
import com.watermellonstudios.audio.harness.smoke.SmokeReporter
import com.watermellonstudios.audio.harness.smoke.SuiteRowVerdict
import com.watermellonstudios.audio.internal.bridge.getAudioBridge
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
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
class UsbHarness(private val context: Context) {

    val manager: IUsbAudioManager = UsbAudioManagerFactory.create(context)
    private val bridge: IAudioNativeBridge = getAudioBridge()
    private val runner = UsbAudioTestRunnerFactory.create(manager)

    init {
        // SIN auto-connect, y antes de monitorear. Medido en el g42 (30/09): con el permiso ya
        // persistido, el "cold-start auto-connect" de la librería dispara OTROS dos `connectDevice`
        // en paralelo con el del harness; el último termina después de que el streaming arrancó,
        // re-inicializa el backend libusb con otro fd y lo destruye (`LibusbBackend destroyed`,
        // `completed=64`): el estado vuelve a CONNECTED y no hay stats. El harness prueba el camino
        // EXPLÍCITO (permiso → connect → motor → backend → streaming), así que ese camino es el
        // único que conecta.
        manager.setAutoConnectEnabled(false)
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
        // REQ-050 S2 (D6): el motor va ANTES que el device, y desde S2 lo pone la LIBRERÍA dentro de
        // `connectDevice`. El harness ya no lo prepara a mano: así el smoke prueba AC-050.4 de
        // punta a punta (MINI-041 #2: un `initializeUsbDevice` sin motor dejaba el backend libusb
        // en el `BackendManager` de respaldo). `motor-callback` queda como verificación, después de
        // conectar. Si algo falla, el motor no puede quedar en modo BackendManager: salida y
        // captura medirían ese camino en vez del directo de Oboe.
        val ok = connectWithEngineReady(r, device, humanTimeoutMs)
        if (!ok) restoreEngineMode()
        return ok
    }

    private suspend fun connectWithEngineReady(r: SmokeReporter, device: UsbAudioDevice, humanTimeoutMs: Long): Boolean {
        val hadPermission = manager.hasPermission(device)
        val result = if (hadPermission) {
            r.report(PANEL, "permiso", true, "origen" to "ya-concedido", "dispositivo" to device.vidPid)
            withTimeoutOrNull(humanTimeoutMs) { manager.connectDevice(device) }
        } else {
            connectThroughDialog(r, device, humanTimeoutMs)
        }
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
        if (!verifyEngineCallback(r)) return false

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

    /**
     * El camino con diálogo: `connectDevice` queda esperando al humano, y mientras tanto
     * `smoke-device.sh` le manda a la app el broadcast de resultado FALSO por `am broadcast`
     * (REQ-050 S1, tarea 1.4). `step=esperando-humano` se emite recién cuando la librería ya pidió
     * el diálogo (`PERMISSION_REQUESTED`): es la señal que el script usa para mandarlo, y un falso
     * que llega sin espera pendiente no prueba nada. La librería escribe ese estado justo ANTES de
     * pedir el diálogo, no después; la vuelta de adb (leer logcat, mandar el broadcast) es de
     * cientos de ms y tapa esa ventana, pero no es una garantía.
     *
     * `step=permiso-falso` afirma que nada cambió, en las dos mitades de AC-050.1:
     * - ningún `PermissionGranted` de este device llegó con `UsbManager` diciendo que no (el falso
     *   con `permission=true` no marcó nada);
     * - el resultado no salió `PERMISSION_DENIED` (el falso con `permission=false` no abortó). El
     *   smoke le pide al humano ACEPTAR, así que una negación es un humano que se equivocó o un
     *   broadcast ajeno que abortó: desde acá no se distinguen, y los dos invalidan la corrida.
     *   Por eso sale FAIL con `motivo=negado:humano-o-broadcast-ajeno`, no HUMANO.
     *
     * Devuelve `null` si el humano no contestó dentro de [humanTimeoutMs].
     */
    private suspend fun connectThroughDialog(
        r: SmokeReporter,
        device: UsbAudioDevice,
        humanTimeoutMs: Long,
    ): UsbResult<Unit>? = coroutineScope {
        var grants = 0
        var forged = 0
        // Unconfined: el colector corre DENTRO del emit de la librería, así que cada
        // PermissionGranted se juzga contra UsbManager en el instante en que se emitió, y antes de
        // que `connectDevice` siga. Con otro dispatcher el evento podía procesarse después de que
        // el humano aceptara, y un grant falso se leería como bueno.
        val watcher = launch(Dispatchers.Unconfined, start = CoroutineStart.UNDISPATCHED) {
            manager.deviceEvents.collect { e ->
                if (e is UsbDeviceEvent.PermissionGranted && e.device.deviceId == device.deviceId) {
                    grants++
                    if (!manager.hasPermission(device)) forged++
                }
            }
        }
        val pending = async { manager.connectDevice(device) }
        val requested = withTimeoutOrNull(DIALOG_REQUESTED_MS) {
            manager.connectionState.first { it == UsbConnectionState.PERMISSION_REQUESTED }
        } != null
        r.waitingForHuman(
            PANEL,
            "aceptar_el_dialogo_de_permiso_USB_de_WMA_Harness_para_${device.vidPid}_en_el_telefono",
            "dispositivo" to device.vidPid, "espera-max-s" to humanTimeoutMs / 1000,
            "dialogo-pedido" to requested,
        )
        val result = withTimeoutOrNull(humanTimeoutMs) { pending.await() }
        if (result == null) pending.cancel()
        watcher.cancelAndJoin()
        val deniedResult = (result as? UsbResult.Failure)?.error == UsbAudioError.PERMISSION_DENIED
        r.report(
            PANEL, "permiso-falso", forged == 0 && !deniedResult,
            "granted" to grants, "granted-sin-permiso-en-usbmanager" to forged,
            "motivo" to when {
                forged > 0 -> "granted-con-usbmanager-diciendo-que-no"
                deniedResult -> "negado:humano-o-broadcast-ajeno"
                else -> null
            },
            "resultado" to when (result) {
                null -> "sin-respuesta-humana"
                is UsbResult.Success -> "conectado"
                is UsbResult.Failure -> result.error
            },
        )
        result
    }

    /**
     * `step=motor-callback` — después de `connectDevice` (REQ-050 S2, D6): la librería tuvo que dejar
     * el motor creado y PARADO, con su callback instalado en `BackendManager`. El harness para el
     * motor antes del panel (`motor-parado`), así que acá vale D6 y no D12 (motor corriendo).
     *
     * No hay un getter del callback: lo que se verifica es que el motor exista y siga parado. La
     * línea lo dice (`verificado=inicializado`) para que nadie la lea como más de lo que es; que el
     * callback esté lo prueba `streaming-start`, que sin él falla con `NO_AUDIO_CALLBACK`.
     */
    private fun verifyEngineCallback(r: SmokeReporter): Boolean {
        val initialized = bridge.isEngineInitialized()
        val state = bridge.getEngineState()
        return r.report(
            PANEL, "motor-callback", initialized && state == ENGINE_STATE_STOPPED,
            "verificado" to "inicializado", "preparado-por" to "libreria", "inicializado" to initialized,
            "estado-motor" to state,
            "motivo" to when {
                !initialized -> "connectDevice-no-creo-el-motor"
                state != ENGINE_STATE_STOPPED -> "motor-corriendo"
                else -> null
            },
        )
    }

    /** Deshace lo que `connectDevice` le hizo al motor (D6): sin BackendManager, si está parado. */
    private fun restoreEngineMode() {
        if (bridge.getEngineState() == ENGINE_STATE_STOPPED) bridge.setUseBackendManager(false)
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
        reportWakeLockPermission(r)
        val state = manager.connectionState.value
        if (!manager.isDeviceReady() || (state != UsbConnectionState.CONNECTED && state != UsbConnectionState.STREAMING)) {
            r.report(PANEL, "streaming-start", false, "motivo" to UsbAudioError.NOT_CONNECTED, "estado" to state)
            reportNoStreaming(r)
            return false
        }
        // Sin precondición del callback acá (REQ-050 S2, AC-050.3): si falta, la librería lo dice
        // con NO_AUDIO_CALLBACK y sale en el campo `error` de `streaming-start`.
        val bits = manager.getCurrentCapabilitySnapshot()?.effectiveOutputBitDepths
            ?.let { if (24 in it) 24 else it.firstOrNull() } ?: 16
        val result = manager.startStreaming(sampleRate = STREAM_RATE_HZ, channels = 2, bitDepth = bits)
        val started = r.report(
            PANEL, "streaming-start", result is UsbResult.Success,
            "rate" to STREAM_RATE_HZ, "canales" to 2, "bits" to bits, "estado" to manager.connectionState.value,
            "error" to (result as? UsbResult.Failure)?.error, "mensaje" to (result as? UsbResult.Failure)?.message,
        )
        if (!started) {
            reportNoStreaming(r)
            return false
        }
        delay(warmupMs)
        return reportStats(r, "streaming-stats")
    }

    /**
     * `step=wake-lock` (REQ-050 S2, D9, AC-050.7): el harness NO declara `WAKE_LOCK` en su manifest,
     * así que si el permiso está concedido llegó por el merge del manifest de `:audio`. Sin él,
     * `startStreaming` tomaba el wake lock y `PowerManager` tiraba `SecurityException`.
     */
    private fun reportWakeLockPermission(r: SmokeReporter) {
        val granted = context.checkSelfPermission(Manifest.permission.WAKE_LOCK) == PackageManager.PERMISSION_GRANTED
        r.report(
            PANEL, "wake-lock", granted,
            "concedido" to granted, "declarado-por-el-harness" to false, "origen" to "merge-del-manifest-de-audio",
            "motivo" to if (granted) null else "WAKE_LOCK-no-llego-por-el-merge",
        )
    }

    /**
     * REQ-050 S2 (AC-050.5, D7) — con el stream VIVO, dos pedidos de conexión que antes lo
     * destruían (MINI-041 #3: el último `connectDevice` re-inicializaba el backend libusb con otro fd):
     *
     * - `step=reconectar-mismo`: `connectDevice` al MISMO device tiene que dar éxito sin tocar nada:
     *   el mismo fd, el estado sigue en STREAMING y los paquetes completados SIGUEN creciendo desde
     *   donde estaban. Un re-init crea un backend nuevo y sus contadores arrancan de cero, así que
     *   "siguen creciendo" no lo puede imitar. Se espera POR CONDICIÓN, con techo.
     * - `step=conectar-otro`: `connectDevice` a un deviceId que no existe tiene que dar
     *   `DEVICE_BUSY` (el chequeo de ocupado va antes de buscar el device), y el stream sigue.
     *
     * Los dos pasos se emiten SIEMPRE: sin streaming, con `ok=false motivo=sin-streaming`.
     */
    suspend fun connectAgainWhileStreaming(r: SmokeReporter, device: UsbAudioDevice, streaming: Boolean): Boolean {
        if (!streaming) {
            r.report(PANEL, "reconectar-mismo", false, "motivo" to "sin-streaming", "estado" to manager.connectionState.value)
            r.report(PANEL, "conectar-otro", false, "motivo" to "sin-streaming", "estado" to manager.connectionState.value)
            return false
        }
        val fdBefore = manager.getFileDescriptor()
        val before = manager.getTransferStats()?.packetsCompleted ?: -1L
        val same = manager.connectDevice(device)
        val fdAfter = manager.getFileDescriptor()
        val stateAfter = manager.connectionState.value
        val advanced = awaitPacketsAbove(before, RECONNECT_TRAFFIC_MS)
        val sameOk = r.report(
            PANEL, "reconectar-mismo",
            same is UsbResult.Success && fdAfter == fdBefore && stateAfter == UsbConnectionState.STREAMING &&
                before >= 0 && advanced != null,
            "resultado" to (if (same is UsbResult.Success) "exito" else (same as UsbResult.Failure).error),
            "fd-antes" to fdBefore, "fd-despues" to fdAfter, "estado" to stateAfter,
            "completados-antes" to before, "completados-despues" to advanced,
            "motivo" to when {
                same !is UsbResult.Success -> "el-mismo-device-no-dio-exito"
                fdAfter != fdBefore -> "cambio-el-fd:re-init"
                stateAfter != UsbConnectionState.STREAMING -> "el-stream-no-sigue"
                before < 0 -> "sin-stats-antes"
                advanced == null -> "los-paquetes-no-siguen-creciendo"
                else -> null
            },
        )

        val other = device.copy(deviceId = "${device.deviceId}-falso", deviceName = "dispositivo-falso")
        val otherResult = manager.connectDevice(other)
        val stateAfterOther = manager.connectionState.value
        val otherOk = r.report(
            PANEL, "conectar-otro",
            (otherResult as? UsbResult.Failure)?.error == UsbAudioError.DEVICE_BUSY &&
                stateAfterOther == UsbConnectionState.STREAMING && manager.getFileDescriptor() == fdBefore,
            "device-id" to other.deviceId,
            "resultado" to (if (otherResult is UsbResult.Success) "exito" else (otherResult as UsbResult.Failure).error),
            "estado" to stateAfterOther, "fd" to manager.getFileDescriptor(),
            "motivo" to when {
                (otherResult as? UsbResult.Failure)?.error != UsbAudioError.DEVICE_BUSY -> "no-dio-DEVICE_BUSY"
                stateAfterOther != UsbConnectionState.STREAMING -> "el-stream-no-sigue"
                else -> null
            },
        )
        return sameOk && otherOk
    }

    /** Espera a que los paquetes completados pasen de [floor], con techo. Devuelve el valor, o null. */
    private suspend fun awaitPacketsAbove(floor: Long, ceilingMs: Long): Long? {
        var waited = 0L
        while (waited <= ceilingMs) {
            val now = manager.getTransferStats()?.packetsCompleted
            if (now != null && now > floor && floor >= 0) return now
            delay(TRAFFIC_POLL_MS)
            waited += TRAFFIC_POLL_MS
        }
        return null
    }

    /** `streaming-stats` se emite SIEMPRE: si no hubo streaming, con `ok=false` y el porqué. */
    private fun reportNoStreaming(r: SmokeReporter) {
        r.report(PANEL, "streaming-stats", false, "motivo" to "sin-streaming", "estado" to manager.connectionState.value)
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
        // El camino inverso de NoisyPad: sin BackendManager, y Oboe seleccionado. Con el motor
        // corriendo no se puede (el set se ignora): se dice, no se reporta "restaurado".
        val engineStopped = bridge.getEngineState() == ENGINE_STATE_STOPPED
        if (engineStopped) bridge.setUseBackendManager(false)
        val returned = bridge.selectBackend(AudioBackendType.OBOE.id)
        val actual = AudioBackendType.fromId(bridge.getCurrentBackendType())
        val restored = r.report(
            PANEL, "backend-restaurado", engineStopped && actual == AudioBackendType.OBOE,
            "pedido" to AudioBackendType.OBOE, "select-devolvio" to returned, "reporta" to actual,
            "motivo" to if (!engineStopped) "backend-manager-sigue-activo" else null,
        )
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
            val streaming = startStreaming(r)
            ok = streaming && ok
            ok = connectAgainWhileStreaming(r, device, streaming) && ok
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

        /** Techo para ver crecer los paquetes después de reconectar (a 48 kHz son ~1000 por segundo). */
        private const val RECONNECT_TRAFFIC_MS = 2000L
        private const val TRAFFIC_POLL_MS = 50L

        /** Techo para que la librería pida el diálogo. Es una espera por condición: no se duerme. */
        private const val DIALOG_REQUESTED_MS = 5000L

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
