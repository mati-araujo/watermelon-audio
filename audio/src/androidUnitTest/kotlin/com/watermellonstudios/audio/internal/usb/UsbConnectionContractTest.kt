package com.watermellonstudios.audio.internal.usb

import android.content.BroadcastReceiver
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.hardware.usb.FakeUsbDevice
import android.hardware.usb.FakeUsbDeviceConnection
import android.hardware.usb.FakeUsbManager
import android.os.Handler
import com.watermellonstudios.audio.domain.usb.AltsettingInfo
import com.watermellonstudios.audio.domain.usb.AudioFormatInfo
import com.watermellonstudios.audio.domain.usb.ClockSourceInfo
import com.watermellonstudios.audio.domain.usb.ClockSourceType
import com.watermellonstudios.audio.domain.usb.StreamPreference
import com.watermellonstudios.audio.domain.usb.UsbStreamingMode
import com.watermellonstudios.audio.domain.usb.UsbSyncMode
import com.watermellonstudios.audio.domain.usb.UsbAudioCapabilities
import com.watermellonstudios.audio.domain.usb.UsbAudioDevice
import com.watermellonstudios.audio.domain.usb.UsbCapabilitySnapshot
import com.watermellonstudios.audio.domain.usb.UsbAudioError
import com.watermellonstudios.audio.domain.usb.UsbConnectionState
import com.watermellonstudios.audio.domain.usb.UsbResult
import com.watermellonstudios.audio.internal.bridge.UsbStreamStartStatus
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.AfterClass
import java.io.File
import java.nio.file.Files
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * REQ-050 S2 — **el contrato de conexión USB no pierde operaciones ni falla mudo** (AC-050.3,
 * AC-050.4, AC-050.5; decisiones D5, D6, D7, D12 y D13).
 *
 * ## Por qué un puerto nativo falso
 *
 * En el host no se puede abrir libusb: `createUsbAudioBackend()` devuelve `nullptr`, así que
 * `initializeUsbDevice` siempre falla y ningún `connectDevice` llega a éxito contra el `.so` de
 * host. [UsbNativePort] es la costura (D14): las operaciones nativas de conexión y streaming
 * pasan por ahí, y este doble ANOTA el orden y deja frenar una llamada con una compuerta. Lo
 * que el doble no puede probar —que el backend libusb quede en el manager del motor y arranque
 * con el callback— lo prueba el smoke en el g42 (el harness conecta sin preparar el motor).
 *
 * ## Concurrencia sin esperas
 *
 * Los tests de AC-050.5 no duermen. La primera conexión se frena DENTRO de
 * `initializeUsbDevice` con una compuerta; la segunda se lanza `UNDISPATCHED`, o sea que corre
 * en el hilo del test hasta su primera suspensión. Con el mutex de D7 esa suspensión es el
 * lock; sin él, la segunda corre hasta su propio `initializeUsbDevice`. Recién ahí se abre la
 * compuerta. El orden lo fija el test, no el reloj.
 */
class UsbConnectionContractTest {

    companion object {
        /** Techo de las esperas POR CONDICIÓN. Nunca se duerme: se usa si algo se cuelga. */
        private const val CEILING_MS = 10_000L
        private const val PACKAGE = "com.watermellonstudios.audio.test"

        private const val ENGINE_STOPPED = 0
        private const val ENGINE_RUNNING = 2

        /** Un directorio por clase: el DataStore de confianza/volumen es singleton de proceso. */
        private val storeDir: File = Files.createTempDirectory("wma-usb-contract").toFile()

        @JvmStatic
        @AfterClass
        fun cleanUp() {
            storeDir.deleteRecursively()
        }
    }

    // ==================== AC-050.3 — startStreaming nombra la causa ====================

    /**
     * AC-050.3. Sin motor.
     *
     * Bug que atrapa: volver a colapsar el resultado nativo en un `Boolean`, que es como "sin
     * motor" llegaba al consumidor como `STREAMING_ERROR`.
     */
    @Test
    fun `AC-050_3 sin motor, startStreaming devuelve NO_ENGINE`() = withRig { rig ->
        rig.connectWithPermission(rig.deviceA)
        rig.port.startStatus = UsbStreamStartStatus.NO_ENGINE

        val result = rig.manager.startStreaming(48000, 2, 24)

        assertEquals(UsbAudioError.NO_ENGINE, (result as? UsbResult.Failure)?.error, "resultado: $result")
        assertEquals(UsbConnectionState.CONNECTED, rig.manager.connectionState.value, "un fallo no puede publicar STREAMING")
    }

    /** AC-050.3. Sin callback: el escenario exacto de MINI-041 #1 ("No audio callback set"). */
    @Test
    fun `AC-050_3 sin callback, startStreaming devuelve NO_AUDIO_CALLBACK`() = withRig { rig ->
        rig.connectWithPermission(rig.deviceA)
        rig.port.startStatus = UsbStreamStartStatus.NO_CALLBACK

        val result = rig.manager.startStreaming(48000, 2, 24)

        assertEquals(UsbAudioError.NO_AUDIO_CALLBACK, (result as? UsbResult.Failure)?.error, "resultado: $result")
    }

    /**
     * AC-050.3, la tabla entera: cada causa nativa llega con su error, y NINGUNA con el genérico
     * salvo el único caso en que es la causa verdadera (libusb no arrancó).
     */
    @Test
    fun `AC-050_3 cada causa nativa se mapea a su error tipado`() = withRig { rig ->
        rig.connectWithPermission(rig.deviceA)
        val expected = mapOf(
            UsbStreamStartStatus.NOT_INITIALIZED to UsbAudioError.NOT_CONNECTED,
            UsbStreamStartStatus.NO_ENGINE to UsbAudioError.NO_ENGINE,
            UsbStreamStartStatus.NO_BACKEND to UsbAudioError.INITIALIZATION_FAILED,
            UsbStreamStartStatus.NO_CALLBACK to UsbAudioError.NO_AUDIO_CALLBACK,
            UsbStreamStartStatus.INVALID_MODE to UsbAudioError.UNSUPPORTED_FORMAT,
            UsbStreamStartStatus.START_FAILED to UsbAudioError.STREAMING_ERROR,
        )
        for ((status, error) in expected) {
            rig.port.startStatus = status
            val result = rig.manager.startStreaming(48000, 2, 24)
            assertEquals(error, (result as? UsbResult.Failure)?.error, "status nativo $status: $result")
        }
    }

    /** AC-050.3, el gemelo positivo: con motor y callback, arranca. */
    @Test
    fun `AC-050_3 gemelo - con motor y callback arranca y publica STREAMING`() = withRig { rig ->
        rig.connectWithPermission(rig.deviceA)
        rig.port.startStatus = UsbStreamStartStatus.OK

        assertEquals(UsbResult.Success(Unit), rig.manager.startStreaming(48000, 2, 24))
        assertEquals(UsbConnectionState.STREAMING, rig.manager.connectionState.value)
    }

    // ==================== AC-050.4 — la librería crea el motor (D6, D12) ====================

    /**
     * AC-050.4. Sin motor, `connectDevice` lo crea e instala el callback ANTES de inicializar el
     * device.
     *
     * Bug que atrapa: inicializar el device sin motor. El backend libusb queda en la instancia de
     * respaldo de `BackendManager` y se pierde al crearse el motor (MINI-041 #2, medido en el
     * g42): `selectBackend(LIBUSB)` dice que sí y después reporta OBOE.
     */
    @Test
    fun `AC-050_4 sin motor, connectDevice instala el motor y el callback antes de inicializar el device`() =
        withRig { rig ->
            rig.port.engineExists = false

            assertEquals(UsbResult.Success(Unit), rig.connectWithPermission(rig.deviceA))

            assertEquals(
                listOf("installEngineCallback", "initializeUsbDevice"),
                rig.port.log.filter { it == "installEngineCallback" || it == "initializeUsbDevice" },
                "el orden motor -> device no se respetó",
            )
        }

    /** AC-050.4 / D6. Con el motor PARADO también se instala el callback antes del device. */
    @Test
    fun `AC-050_4 con el motor parado, instala el callback antes de inicializar el device`() = withRig { rig ->
        rig.port.engineExists = true
        rig.port.engineState = ENGINE_STOPPED

        assertEquals(UsbResult.Success(Unit), rig.connectWithPermission(rig.deviceA))

        assertEquals(
            listOf("installEngineCallback", "initializeUsbDevice"),
            rig.port.log.filter { it == "installEngineCallback" || it == "initializeUsbDevice" },
        )
    }

    /**
     * D12, el gemelo: con el motor CORRIENDO no se toca (el nativo rechazaría reconfigurarlo con
     * el stream vivo) y la conexión sigue. NoisyPad conecta así y después cambia de backend.
     *
     * Bug que atrapa: "preparar" el motor siempre, o rechazar la conexión porque corre.
     */
    @Test
    fun `AC-050_4 gemelo D12 - con el motor corriendo no lo toca y conecta igual`() = withRig { rig ->
        rig.port.engineExists = true
        rig.port.engineState = ENGINE_RUNNING

        assertEquals(UsbResult.Success(Unit), rig.connectWithPermission(rig.deviceA))

        assertTrue("installEngineCallback" !in rig.port.log, "se reconfiguró un motor corriendo: ${rig.port.log}")
        assertEquals(1, rig.port.initCalls.get())
    }

    /**
     * AC-050.4, la otra salida que el criterio admite: si el motor no se pudo crear, se rechaza con
     * error tipado y el device NO se inicializa (quedaría en el manager de respaldo).
     */
    @Test
    fun `AC-050_4 si el motor no se puede crear, rechaza con NO_ENGINE sin inicializar el device`() =
        withRig { rig ->
            rig.port.engineExists = false
            rig.port.engineCreationFails = true

            val result = rig.connectWithPermission(rig.deviceA)

            assertEquals(UsbAudioError.NO_ENGINE, (result as? UsbResult.Failure)?.error, "resultado: $result")
            assertEquals(0, rig.port.initCalls.get(), "se inicializó el device sin motor")
            assertEquals(UsbConnectionState.ERROR, rig.manager.connectionState.value)
        }

    // ==================== AC-050.5 — connectDevice serializado (D7, D13) ====================

    /**
     * AC-050.5. Dos `connectDevice` del MISMO device a la vez: el segundo espera y devuelve éxito
     * sin re-inicializar nada.
     *
     * Bug que atrapa: MINI-041 #3. El auto-connect de la librería y el connect explícito corrían
     * en paralelo; el último re-inicializaba el backend libusb con otro fd y destruía el stream.
     */
    @Test
    fun `AC-050_5 un segundo connectDevice del mismo device espera y no re-inicializa`() = withRig { rig ->
        rig.usb.permissionGranted.set(true)
        val gate = rig.port.gateFirstInit()

        val first = rig.connectAsync(rig.deviceA)
        gate.awaitEntered()
        val second = rig.connectAsync(rig.deviceA, start = CoroutineStart.UNDISPATCHED)
        gate.release()

        assertEquals(UsbResult.Success(Unit), rig.await(first))
        assertEquals(UsbResult.Success(Unit), rig.await(second), "el mismo device ya conectado tiene que ser éxito")
        assertEquals(1, rig.port.initCalls.get(), "el segundo connectDevice re-inicializó el device")
        assertEquals(1, rig.usb.openDeviceCalls.get(), "el segundo connectDevice abrió otra conexión")
        assertEquals(0, rig.port.closeCalls.get())
    }

    /**
     * AC-050.5 / D7. Otro device mientras hay uno conectándose: espera y falla con DEVICE_BUSY.
     *
     * Bug que atrapa: dejar que el segundo pise al primero (lo que destruía el stream).
     */
    @Test
    fun `AC-050_5 otro device mientras uno se conecta espera y falla con DEVICE_BUSY`() = withRig { rig ->
        rig.usb.permissionGranted.set(true)
        val gate = rig.port.gateFirstInit()

        val first = rig.connectAsync(rig.deviceA)
        gate.awaitEntered()
        val other = rig.connectAsync(rig.deviceB, start = CoroutineStart.UNDISPATCHED)
        gate.release()

        assertEquals(UsbResult.Success(Unit), rig.await(first))
        assertEquals(UsbAudioError.DEVICE_BUSY, (rig.await(other) as? UsbResult.Failure)?.error)
        assertEquals(1, rig.port.initCalls.get(), "el otro device se inicializó encima del primero")
        assertEquals(rig.deviceA.deviceId.toString(), rig.manager.selectedDevice.value?.deviceId)
    }

    /** AC-050.5. Con el stream VIVO, reconectar el mismo device no lo toca. */
    @Test
    fun `AC-050_5 con el streaming en curso, reconectar el mismo device no toca el stream`() = withRig { rig ->
        rig.connectWithPermission(rig.deviceA)
        rig.port.startStatus = UsbStreamStartStatus.OK
        assertEquals(UsbResult.Success(Unit), rig.manager.startStreaming(48000, 2, 24))

        assertEquals(UsbResult.Success(Unit), rig.connectWithPermission(rig.deviceA))

        assertEquals(UsbConnectionState.STREAMING, rig.manager.connectionState.value)
        assertEquals(1, rig.port.initCalls.get())
        assertEquals(0, rig.port.stopStreamingCalls.get(), "se paró el stream vivo")
        assertEquals(0, rig.port.closeCalls.get(), "se cerró el device vivo")
    }

    /** AC-050.5, el gemelo: desconectado el primero, el otro device conecta. */
    @Test
    fun `AC-050_5 gemelo - desconectado el primero, otro device conecta`() = withRig { rig ->
        assertEquals(UsbResult.Success(Unit), rig.connectWithPermission(rig.deviceA))
        rig.manager.disconnectDevice()

        assertEquals(UsbResult.Success(Unit), rig.connectWithPermission(rig.deviceB))
        assertEquals(rig.deviceB.deviceId.toString(), rig.manager.selectedDevice.value?.deviceId)
    }

    /**
     * D13. La espera del permiso no tiene techo: la corta quien llama, y cancelar suelta el mutex.
     *
     * Bug que atrapa: un lock que no se suelta al cancelar, que con la espera sin techo dejaría
     * TODA conexión posterior (el auto-connect incluido) colgada para siempre.
     */
    @Test
    fun `D13 cancelar una conexion que espera el permiso suelta el mutex`() = withRig { rig ->
        val waiting = rig.connectAsync(rig.deviceA)
        rig.usb.permissionRequested.get(CEILING_MS, TimeUnit.MILLISECONDS)

        withTimeout(CEILING_MS) { waiting.cancelAndJoin() }

        rig.usb.permissionGranted.set(true)
        assertEquals(
            UsbResult.Success(Unit),
            rig.await(rig.connectAsync(rig.deviceB)),
            "la conexión cancelada dejó el mutex tomado",
        )
    }

    // ==================== REQ-050 S3 — la selección vuelve a automático (D17, D18) ====================

    /** Un device UAC2 con un altsetting 2ch/24 bits y un reloj (id 9) de 44,1 a 96 kHz. */
    private fun uac2Snapshot() = UsbCapabilitySnapshot(
        vendorId = 0x3001, productId = 0x4001, productName = "fake", manufacturer = "fake", serialNumber = "",
        uacVersion = 2,
        playbackAltsettings = listOf(
            AltsettingInfo(
                interfaceNumber = 1, alternateSetting = 2,
                formats = listOf(AudioFormatInfo(2, 24, 3, emptyList(), false, 0, 0)),
                syncType = UsbSyncMode.ADAPTIVE, hasFeedbackEndpoint = false, hasImplicitFeedback = false,
                dataEndpointAddress = 1, terminalLinkId = 2,
            )
        ),
        captureAltsettings = emptyList(),
        clockSources = listOf(
            ClockSourceInfo(9, ClockSourceType.INTERNAL_FIXED, false, true, false, listOf(44100, 48000, 96000))
        ),
        featureUnits = emptyList(),
    )

    /**
     * D17. El centinela (-1,-1,-1) llega al nativo y Kotlin deja de re-aplicar la selección
     * manual en cada arranque.
     *
     * Bug que atrapa: limpiar sólo el nativo. `startStreaming` re-aplica `_selectedAltsetting` en
     * cada arranque, así que la selección del runner volvería a pegarse al stream del consumidor.
     */
    @Test
    fun `D17 el centinela limpia la seleccion y el proximo arranque no la re-aplica`() = withRig { rig ->
        rig.connectWithPermission(rig.deviceA)
        rig.port.snapshot = uac2Snapshot()
        assertEquals(UsbResult.Success(Unit), rig.manager.selectAltsetting(1, 2, 0))

        assertEquals(UsbResult.Success(Unit), rig.manager.selectAltsetting(-1, -1, -1))
        assertTrue("selectUsbAltsetting(-1,-1,-1)" in rig.port.log, "el centinela no llegó al nativo: ${rig.port.log}")
        rig.port.log.clear()
        assertEquals(UsbResult.Success(Unit), rig.manager.startStreaming(48000, 2, 24))

        assertTrue("selectUsbAltsetting(1,2,0)" !in rig.port.log, "se re-aplicó la selección limpiada: ${rig.port.log}")
        assertEquals(null, rig.manager.activeStreamConfig()?.altsetting)
    }

    /** D17, el gemelo: sin limpiar, el arranque SÍ re-aplica la selección (el test la puede ver). */
    @Test
    fun `D17 gemelo - sin limpiar, el arranque re-aplica la seleccion`() = withRig { rig ->
        rig.connectWithPermission(rig.deviceA)
        rig.port.snapshot = uac2Snapshot()
        assertEquals(UsbResult.Success(Unit), rig.manager.selectAltsetting(1, 2, 0))
        rig.port.log.clear()

        assertEquals(UsbResult.Success(Unit), rig.manager.startStreaming(48000, 2, 24))

        assertTrue("selectUsbAltsetting(1,2,0)" in rig.port.log, "premisa: ${rig.port.log}")
        assertEquals(UsbAltsettingSelection(1, 2, 0), rig.manager.activeStreamConfig()?.altsetting)
    }

    /**
     * D18 (review de S3, M6). Si el nativo rechaza re-aplicar la selección al arrancar, el stream
     * corre con la elección automática, y la config activa lo dice: el runner no puede creer que
     * el stream está en esos bits.
     */
    @Test
    fun `D18 una seleccion que el nativo rechaza al arrancar no queda en la config activa`() = withRig { rig ->
        rig.connectWithPermission(rig.deviceA)
        rig.port.snapshot = uac2Snapshot()
        assertEquals(UsbResult.Success(Unit), rig.manager.selectAltsetting(1, 2, 0))
        rig.port.rejectSelect = true

        assertEquals(UsbResult.Success(Unit), rig.manager.startStreaming(48000, 2, 24))

        assertEquals(null, rig.manager.activeStreamConfig()?.altsetting)
        assertEquals(UsbAltsettingSelection(1, 2, 0), rig.manager.manualSelection().altsetting, "la pendiente sigue")
    }

    /**
     * D17. El centinela es EXACTO: cualquier otro negativo se rechaza en Kotlin y no llega al
     * nativo (que lo vuelve a rechazar por su cuenta, `classifyAltsettingRequest`).
     */
    @Test
    fun `D17 un negativo distinto del centinela se rechaza sin llegar al nativo`() = withRig { rig ->
        rig.connectWithPermission(rig.deviceA)
        // Sin descriptores todavía: el índice negativo es un error de formato por sí mismo, no
        // "no hay snapshot" (que es lo que diría la búsqueda en los descriptores).
        val early = rig.manager.selectAltsetting(-1, -1, 0)
        assertEquals(UsbAudioError.UNSUPPORTED_FORMAT, (early as? UsbResult.Failure)?.error, "sin snapshot: $early")
        rig.port.snapshot = uac2Snapshot()

        for ((i, a, f) in listOf(Triple(-1, -1, 0), Triple(0, -1, -1), Triple(-2, -2, -2))) {
            val r = rig.manager.selectAltsetting(i, a, f)
            assertEquals(UsbAudioError.UNSUPPORTED_FORMAT, (r as? UsbResult.Failure)?.error, "($i,$a,$f): $r")
        }
        val clock = rig.manager.selectClockSource(-1)
        assertEquals(UsbAudioError.UNSUPPORTED_FORMAT, (clock as? UsbResult.Failure)?.error, "reloj -1: $clock")
        assertTrue(rig.port.log.none { it.startsWith("select") }, "un pedido rechazado llegó al nativo: ${rig.port.log}")
    }

    /** D17/D18. Reloj 0 vuelve a automático, y la config del stream lo refleja. */
    @Test
    fun `D17 reloj 0 limpia el reloj elegido a mano`() = withRig { rig ->
        rig.connectWithPermission(rig.deviceA)
        rig.port.snapshot = uac2Snapshot()
        assertEquals(UsbResult.Success(Unit), rig.manager.selectClockSource(9))
        assertEquals(UsbResult.Success(Unit), rig.manager.startStreaming(48000, 2, 24))
        assertEquals(9, rig.manager.activeStreamConfig()?.clockSourceId, "premisa: el reloj manual quedó en la config")
        rig.manager.stopStreaming()

        assertEquals(UsbResult.Success(Unit), rig.manager.selectClockSource(0))
        assertTrue("selectUsbClockSource(0)" in rig.port.log, "el centinela no llegó al nativo: ${rig.port.log}")
        assertEquals(UsbResult.Success(Unit), rig.manager.startStreaming(48000, 2, 24))

        assertEquals(null, rig.manager.activeStreamConfig()?.clockSourceId)
    }

    /** D17. Con el stream corriendo, limpiar se rechaza (como la selección misma). */
    @Test
    fun `D17 con el stream corriendo no se limpia la seleccion`() = withRig { rig ->
        rig.connectWithPermission(rig.deviceA)
        assertEquals(UsbResult.Success(Unit), rig.manager.startStreaming(48000, 2, 24))
        rig.port.log.clear()

        val r = rig.manager.selectAltsetting(-1, -1, -1)

        assertEquals(UsbAudioError.STREAMING_ERROR, (r as? UsbResult.Failure)?.error, "resultado: $r")
        assertTrue(rig.port.log.isEmpty(), "llegó al nativo con el stream vivo: ${rig.port.log}")
    }

    /**
     * D18. La config del stream que corre es la del último arranque exitoso, y desaparece al
     * parar: el runner decide con ella si reabre y cómo restaura.
     */
    @Test
    fun `D18 la config activa es la del arranque y se borra al parar`() = withRig { rig ->
        rig.connectWithPermission(rig.deviceA)
        assertEquals(null, rig.manager.activeStreamConfig())

        assertEquals(UsbResult.Success(Unit), rig.manager.startStreaming(44100, 2, 16))
        assertEquals(
            UsbActiveStreamConfig(44100, 2, 16, UsbStreamingMode.PLAYBACK_ONLY, null, null),
            rig.manager.activeStreamConfig(),
        )
        rig.manager.stopStreaming()
        assertEquals(null, rig.manager.activeStreamConfig())

        rig.port.startStatus = UsbStreamStartStatus.START_FAILED
        rig.manager.startStreaming(96000, 2, 24)
        assertEquals(null, rig.manager.activeStreamConfig(), "un arranque fallido dejó una config activa")
    }

    /**
     * D16. Los dos valores nuevos del array de stats van a sus campos, y un `.so` viejo de 19 los
     * deja en 0 en vez de inventarlos.
     *
     * Bug que atrapa: leer [19]/[20] al revés (192 paquetes como techo de latencia) o con
     * índices que un array de 19 no tiene.
     */
    @Test
    fun `D16 el array de 21 llena en vuelo y techo, y uno de 19 los deja en cero`() {
        val arr = FloatArray(21) { it.toFloat() }.also { it[19] = 192f; it[20] = 61.3f }

        val stats = parseUsbTransferStats(arr, bufferMs = 5, healthScore = 100f, bufferAdjustments = 0)
        assertEquals(192L, stats.packetsInFlight)
        assertEquals(61.3, stats.declaredMaxLatencyMs, 0.001)

        val old = parseUsbTransferStats(arr.copyOf(19), bufferMs = 5, healthScore = 100f, bufferAdjustments = 0)
        assertEquals(0L, old.packetsInFlight)
        assertEquals(0.0, old.declaredMaxLatencyMs, 0.0)
        assertEquals(18, old.activeClockSourceId, "premisa: los 19 de antes siguen en su lugar")
    }

    // ==================== El arnés ====================

    private fun withRig(body: suspend (Rig) -> Unit) = runBlocking {
        val rig = Rig()
        try {
            body(rig)
        } finally {
            rig.close()
        }
    }

    private class Rig {
        val usb = FakeUsbManager()
        val deviceA = FakeUsbDevice(id = 11, vid = 0x3001, pid = 0x4001)
        val deviceB = FakeUsbDevice(id = 12, vid = 0x3002, pid = 0x4002)
        val port = FakeUsbNativePort()
        val context = FakeContext(usb)
        private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val manager = UsbAudioManagerImpl(context, scope, port)
        private val nextFd = AtomicInteger(40)

        init {
            usb.attach(deviceA)
            usb.attach(deviceB)
            usb.connectionFactory = { FakeUsbDeviceConnection(nextFd.incrementAndGet()) }
            manager.setAutoConnectEnabled(false)
            // El chequeo de salud y la restauración de volumen hablan con el `.so` de host, que no
            // tiene device: no son lo que este test mide.
            manager.setHealthCheckEnabled(false)
            manager.startMonitoring()
        }

        fun audioDevice(d: FakeUsbDevice) = UsbAudioDevice(
            deviceId = d.deviceId.toString(),
            vendorId = d.vendorId,
            productId = d.productId,
            deviceName = d.productName,
            manufacturerName = d.manufacturerName,
            serialNumber = null,
            capabilities = UsbAudioCapabilities.UNKNOWN,
        )

        suspend fun connectWithPermission(d: FakeUsbDevice): UsbResult<Unit> {
            usb.permissionGranted.set(true)
            return withTimeout(CEILING_MS) { manager.connectDevice(audioDevice(d)) }
        }

        fun connectAsync(d: FakeUsbDevice, start: CoroutineStart = CoroutineStart.DEFAULT): Deferred<UsbResult<Unit>> =
            scope.async(start = start) { manager.connectDevice(audioDevice(d)) }

        suspend fun await(d: Deferred<UsbResult<Unit>>): UsbResult<Unit> = withTimeout(CEILING_MS) { d.await() }

        fun close() {
            port.releaseGates()
            scope.cancel()
        }
    }

    /** Una compuerta sobre UNA llamada: avisa que entró y la retiene hasta que el test la suelte. */
    class Gate {
        private val entered = CountDownLatch(1)
        private val released = CountDownLatch(1)
        fun enter() {
            entered.countDown()
            released.await(CEILING_MS, TimeUnit.MILLISECONDS)
        }
        fun awaitEntered() {
            assertTrue(entered.await(CEILING_MS, TimeUnit.MILLISECONDS), "la primera conexión nunca llegó a inicializar")
        }
        fun release() = released.countDown()
    }

    /** El doble de [UsbNativePort]: anota cada llamada y contesta lo que el test fija. */
    class FakeUsbNativePort : UsbNativePort {
        val log: MutableList<String> = Collections.synchronizedList(mutableListOf())
        @Volatile var engineExists = true
        @Volatile var engineState = ENGINE_STOPPED
        @Volatile var engineCreationFails = false
        @Volatile var startStatus = UsbStreamStartStatus.OK
        @Volatile private var deviceInitialized = false
        @Volatile private var firstInitGate: Gate? = null
        val initCalls = AtomicInteger(0)
        val closeCalls = AtomicInteger(0)
        val stopStreamingCalls = AtomicInteger(0)

        fun gateFirstInit(): Gate = Gate().also { firstInitGate = it }
        fun releaseGates() { firstInitGate?.release() }

        override fun isEngineInitialized(): Boolean = engineExists
        override fun engineState(): Int = engineState
        override fun installEngineCallback() {
            log += "installEngineCallback"
            if (!engineCreationFails) engineExists = true
        }
        override fun initializeUsbDevice(fileDescriptor: Int, usbfsPath: String): Boolean {
            log += "initializeUsbDevice"
            if (initCalls.incrementAndGet() == 1) firstInitGate?.enter()
            deviceInitialized = true
            return true
        }
        override fun parseUsbDescriptors(): FloatArray? = null
        override fun isUsbDeviceInitialized(): Boolean = deviceInitialized
        override fun stopUsbStreaming() {
            log += "stopUsbStreaming"
            stopStreamingCalls.incrementAndGet()
        }
        override fun closeUsbDevice() {
            log += "closeUsbDevice"
            closeCalls.incrementAndGet()
            deviceInitialized = false
        }
        override fun setUsbStreamPreference(preference: StreamPreference): Boolean = true
        @Volatile var snapshot: UsbCapabilitySnapshot? = null
        @Volatile var rejectSelect = false
        override fun selectUsbAltsetting(interfaceNumber: Int, alternateSetting: Int, formatIndex: Int): Boolean {
            log += "selectUsbAltsetting($interfaceNumber,$alternateSetting,$formatIndex)"
            return !rejectSelect
        }
        override fun selectUsbClockSource(clockSourceId: Int): Boolean {
            log += "selectUsbClockSource($clockSourceId)"
            return true
        }
        override fun capabilitySnapshot(): UsbCapabilitySnapshot? = snapshot
        override fun startUsbStreamingWithModeStatus(sampleRate: Int, channels: Int, bitDepth: Int, streamingMode: Int): Int {
            log += "startUsbStreaming"
            return startStatus
        }
    }

    /** El `Context` que ve la librería: sólo lo que el camino de conexión usa. */
    private class FakeContext(private val usb: FakeUsbManager) : ContextWrapper(null) {
        override fun getSystemService(name: String): Any? = if (name == Context.USB_SERVICE) usb else null
        override fun getPackageName(): String = PACKAGE
        override fun getOpPackageName(): String = PACKAGE
        override fun getApplicationContext(): Context = this
        override fun getFilesDir(): File = storeDir
        override fun checkPermission(permission: String, pid: Int, uid: Int): Int = PackageManager.PERMISSION_GRANTED
        override fun registerReceiver(receiver: BroadcastReceiver?, filter: IntentFilter?): Intent? = null
        override fun registerReceiver(receiver: BroadcastReceiver?, filter: IntentFilter?, flags: Int): Intent? = null
        override fun registerReceiver(
            receiver: BroadcastReceiver?, filter: IntentFilter?, permission: String?, scheduler: Handler?,
        ): Intent? = null
        override fun registerReceiver(
            receiver: BroadcastReceiver?, filter: IntentFilter?, permission: String?, scheduler: Handler?, flags: Int,
        ): Intent? = null
        override fun unregisterReceiver(receiver: BroadcastReceiver?) = Unit
    }
}
