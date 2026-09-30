package com.watermellonstudios.audio.internal.usb

import android.content.BroadcastReceiver
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.hardware.usb.FakeUsbDevice
import android.hardware.usb.FakeUsbManager
import android.hardware.usb.UsbManager
import android.os.Bundle
import android.os.Handler
import android.os.Parcelable
import com.watermellonstudios.audio.domain.usb.UsbAudioCapabilities
import com.watermellonstudios.audio.domain.usb.UsbAudioDevice
import com.watermellonstudios.audio.domain.usb.UsbAudioError
import com.watermellonstudios.audio.domain.usb.UsbDeviceEvent
import com.watermellonstudios.audio.domain.usb.UsbResult
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.AfterClass
import java.io.File
import java.nio.file.Files
import java.util.Collections
import java.util.concurrent.TimeUnit
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * REQ-050 S1 — **el resultado del permiso USB sale de `UsbManager`, no del intent**.
 *
 * Hasta acá la librería le creía al extra `EXTRA_PERMISSION_GRANTED` del broadcast
 * `com.watermellonstudios.audio.USB_PERMISSION`, con un receiver EXPORTADO. Cualquier app instalada
 * podía mandar ese broadcast mientras el diálogo estaba abierto (MINI-040): con `true` la librería
 * marcaba el device como confiable y emitía `PermissionGranted` sin que el usuario dijera nada; con
 * `false` abortaba la conexión a voluntad. El camino con `device == null` era el atajo: reanudaba la
 * espera con el extra tal cual.
 *
 * ## Cómo se prueba sin una espera ciega
 *
 * Cada caso de "el broadcast falso NO hizo nada" es una AUSENCIA, y una ausencia no se puede esperar
 * por condición. Así que no se espera: después del falso se entrega la respuesta GENUINA del usuario,
 * con el signo opuesto al que el falso habría impuesto, y se afirma sobre el resultado de
 * `connectDevice`. La espera es una sola, así que la reanuda el primero que llega: si el falso la
 * hubiera reanudado, el resultado tendría SU signo. El orden lo fija el test, no el reloj.
 *
 * ## Lo que NO prueba
 *
 * Que el sistema siga entregando el resultado del diálogo y ATTACHED/DETACHED a un receiver NO
 * exportado. Eso depende del `system_server` del device, y se verifica en el g42 con la CM720
 * (tarea 1.3, `smoke-device.sh --plan usb`). Acá sólo se afirma que el registro se pide así.
 */
class UsbPermissionResultTest {

    companion object {
        /** Techo de las esperas POR CONDICIÓN. Nunca se duerme este tiempo: se lo usa si algo se cuelga. */
        private const val CEILING_MS = 10_000L

        private const val ACTION_USB_PERMISSION = "com.watermellonstudios.audio.USB_PERMISSION"
        private const val PACKAGE = "com.watermellonstudios.audio.test"

        /**
         * UN directorio para toda la clase: el `preferencesDataStore` de la lista de confianza es un
         * singleton de proceso que se queda con el primer `filesDir` que ve. Por eso cada test usa su
         * propio VID:PID, y ninguno afirma sobre otro.
         */
        private val storeDir: File = Files.createTempDirectory("wma-usb-permission").toFile()

        @JvmStatic
        @AfterClass
        fun cleanUp() {
            storeDir.deleteRecursively()
        }
    }

    // ==================== AC-050.1 — el broadcast de otra app no decide ====================

    /**
     * AC-050.1. El escenario exacto de MINI-040: `permission=true`, sin device.
     *
     * Bug que atrapa: volver a reanudar la espera con el extra cuando `device == null`. Con eso, el
     * falso gana la carrera, la librería escribe el VID:PID en la lista de confianza, emite
     * `PermissionGranted` e intenta abrir el device que el usuario nunca autorizó.
     */
    @Test
    fun `AC-050_1 un broadcast falso con permission true y sin device no marca confianza ni emite PermissionGranted`() =
        withRig(vid = 0x1001, pid = 0x2001) { rig ->
            val pending = rig.startConnect()

            rig.deliver(permissionIntent(device = null, extraGranted = true))
            // El usuario niega: el sistema NO le da el permiso y manda el resultado con el device.
            rig.deliver(permissionIntent(device = rig.device, extraGranted = false))

            val result = rig.await(pending)
            assertEquals(
                UsbResult.Failure(UsbAudioError.PERMISSION_DENIED),
                result,
                "el broadcast falso decidió la espera: el resultado tiene que ser la negación del usuario",
            )
            assertFalse(rig.isTrusted(), "un broadcast falso marcó el device como confiable")
            assertTrue(
                rig.events.none { it is UsbDeviceEvent.PermissionGranted },
                "un broadcast falso hizo emitir PermissionGranted: ${rig.events}",
            )
            assertEquals(0, rig.usb.openDeviceCalls.get(), "se intentó abrir un device sin permiso")
        }

    /**
     * AC-050.1. El mismo broadcast con `permission=false`: la otra app intenta abortar la conexión.
     *
     * Bug que atrapa: que un `false` sin device reanude la espera. Con eso cualquier app corta
     * cualquier conexión USB en curso, y el usuario que después acepta el diálogo ya no conecta.
     */
    @Test
    fun `AC-050_1 un broadcast falso con permission false y sin device no aborta la conexion en curso`() =
        withRig(vid = 0x1002, pid = 0x2002) { rig ->
            val pending = rig.startConnect()

            rig.deliver(permissionIntent(device = null, extraGranted = false))
            // El usuario acepta: el sistema da el permiso y después manda el resultado.
            rig.usb.permissionGranted.set(true)
            rig.deliver(permissionIntent(device = rig.device, extraGranted = true))

            rig.await(pending)
            assertEquals(
                1,
                rig.usb.openDeviceCalls.get(),
                "el broadcast falso abortó la conexión: el permiso que dio el usuario nunca llegó a abrir el device",
            )
            assertTrue(rig.events.any { it is UsbDeviceEvent.PermissionGranted }, "falta PermissionGranted: ${rig.events}")
            assertTrue(rig.events.none { it is UsbDeviceEvent.PermissionDenied }, "se emitió PermissionDenied: ${rig.events}")
        }

    /**
     * AC-050.1, defensa en profundidad (D4). El receiver no queda exportado.
     *
     * Bug que atrapa: volver a `RECEIVER_EXPORTED`, o registrar sin bandera (API < 33), que deja a
     * cualquier app mandarle el broadcast. En la JVM `Build.VERSION.SDK_INT` es 0, así que
     * `ContextCompat` toma el camino previo a 26: lo protege con el permiso de firma
     * `<paquete>.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION`. En API 33+ pasa la bandera. Se aceptan
     * las dos formas; lo que no se acepta es un registro abierto.
     */
    @Test
    fun `AC-050_1 el receiver del permiso se registra no exportado`() =
        withRig(vid = 0x1003, pid = 0x2003) { rig ->
            val registration = rig.context.registrations.single()
            val protectedByPermission =
                registration.permission == "$PACKAGE.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION"
            val flaggedNotExported = (registration.flags and Context.RECEIVER_NOT_EXPORTED) != 0
            assertTrue(
                protectedByPermission || flaggedNotExported,
                "el receiver quedó abierto a otras apps: permission=${registration.permission}, flags=${registration.flags}",
            )
            assertEquals(0, registration.flags and Context.RECEIVER_EXPORTED, "el receiver se registró RECEIVER_EXPORTED")
        }

    // ==================== AC-050.2 — decide UsbManager, no el extra ====================

    /**
     * AC-050.2. El resultado trae el device y `permission=true`, pero `UsbManager` dice que no.
     *
     * Bug que atrapa: leer el extra. Es la variante del falso que SÍ trae un device (cualquier app
     * puede enumerar `deviceList` y meterlo en el intent): con el extra, pasaría.
     */
    @Test
    fun `AC-050_2 con el extra en true y sin permiso real, deniega`() =
        withRig(vid = 0x1004, pid = 0x2004) { rig ->
            val pending = rig.startConnect()

            rig.deliver(permissionIntent(device = rig.device, extraGranted = true))

            assertEquals(UsbResult.Failure(UsbAudioError.PERMISSION_DENIED), rig.await(pending))
            assertFalse(rig.isTrusted(), "se marcó confiable un device que UsbManager no autorizó")
            assertEquals(0, rig.usb.openDeviceCalls.get())
        }

    /**
     * AC-050.2, el gemelo en la otra dirección: el extra dice que no, `UsbManager` dice que sí.
     *
     * Bug que atrapa: leer el extra en cualquiera de sus dos signos. Sin este caso, un arreglo que
     * sólo desconfiara de los `true` pasaría el test de arriba y seguiría dejando que un `false`
     * ajeno le gane al usuario.
     */
    @Test
    fun `AC-050_2 con el extra en false y el permiso real dado, conecta`() =
        withRig(vid = 0x1005, pid = 0x2005) { rig ->
            val pending = rig.startConnect()

            rig.usb.permissionGranted.set(true)
            rig.deliver(permissionIntent(device = rig.device, extraGranted = false))

            rig.await(pending)
            assertEquals(1, rig.usb.openDeviceCalls.get(), "el extra en false le ganó a UsbManager")
            assertTrue(rig.isTrusted())
        }

    /**
     * AC-050.2, el control positivo: el usuario acepta el diálogo y la conexión sigue.
     *
     * Bug que atrapa: un arreglo que "cierra" el defecto negándolo todo. Sin este gemelo, los tests
     * de ausencia de arriba pasarían con un receiver que no reanuda nunca.
     */
    @Test
    fun `AC-050_2 gemelo - el usuario acepta el dialogo y conecta`() =
        withRig(vid = 0x1006, pid = 0x2006) { rig ->
            val pending = rig.startConnect()

            rig.usb.permissionGranted.set(true)
            rig.deliver(permissionIntent(device = rig.device, extraGranted = true))

            val result = rig.await(pending)
            assertEquals(1, rig.usb.openDeviceCalls.get(), "con el permiso dado no se abrió el device: $result")
            assertTrue(rig.isTrusted(), "el device autorizado no quedó en la lista de confianza")
            assertTrue(rig.events.any { it is UsbDeviceEvent.PermissionGranted }, "falta PermissionGranted: ${rig.events}")
        }

    /**
     * AC-050.2. Un resultado sin device, con el permiso ya dado en `UsbManager`.
     *
     * El código viejo tenía este camino a propósito ("algunos Android no incluyen el device"). Que
     * deje de ser un atajo no quiere decir que deje de existir: decide `UsbManager`.
     *
     * Bug que atrapa: "arreglar" el atajo descartando todo broadcast sin device, que dejaría colgado
     * `connectDevice` justo en los devices que el comentario original describía.
     */
    @Test
    fun `AC-050_2 un resultado sin device con el permiso dado en UsbManager conecta`() =
        withRig(vid = 0x1007, pid = 0x2007) { rig ->
            val pending = rig.startConnect()

            rig.usb.permissionGranted.set(true)
            rig.deliver(permissionIntent(device = null, extraGranted = false))

            rig.await(pending)
            assertEquals(1, rig.usb.openDeviceCalls.get(), "el resultado sin device no reanudó la espera")
        }

    // ==================== El arnés ====================

    private fun withRig(vid: Int, pid: Int, body: suspend (Rig) -> Unit) = runBlocking {
        val rig = Rig(vid, pid)
        try {
            body(rig)
        } finally {
            rig.close()
        }
    }

    private fun permissionIntent(device: FakeUsbDevice?, extraGranted: Boolean) =
        FakeIntent(ACTION_USB_PERMISSION, device, extraGranted)

    private inner class Rig(val vid: Int, val pid: Int) {
        val usb = FakeUsbManager()
        val device = FakeUsbDevice(id = 7, vid = vid, pid = pid)
        val context = FakeContext(usb)
        private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val manager = UsbAudioManagerImpl(context, scope)
        val events: MutableList<UsbDeviceEvent> = Collections.synchronizedList(mutableListOf())
        private val receiver: BroadcastReceiver

        init {
            usb.attach(device)
            // Sin auto-connect: el único `connectDevice` es el del test.
            manager.setAutoConnectEnabled(false)
            // Unconfined: el colector corre DENTRO del `emit` de la librería, así que cada evento
            // está en `events` antes de que `connectDevice` siga y devuelva. Con un dispatcher
            // propio, "falta PermissionGranted" podía salir rojo con el código bien, sólo porque
            // el worker todavía no lo había recolectado (y una ausencia pasaba sin haber mirado).
            scope.launch(Dispatchers.Unconfined, start = CoroutineStart.UNDISPATCHED) {
                manager.deviceEvents.collect { events += it }
            }
            manager.startMonitoring()
            receiver = assertNotNull(context.registrations.singleOrNull()?.receiver, "startMonitoring no registró el receiver")
        }

        /** Arranca la conexión y espera, POR CONDICIÓN, a que la librería haya pedido el diálogo. */
        fun startConnect(): Deferred<UsbResult<Unit>> {
            val audioDevice = UsbAudioDevice(
                deviceId = device.deviceId.toString(),
                vendorId = vid,
                productId = pid,
                deviceName = device.productName,
                manufacturerName = device.manufacturerName,
                serialNumber = null,
                capabilities = UsbAudioCapabilities.UNKNOWN,
            )
            val pending = scope.async { manager.connectDevice(audioDevice) }
            usb.permissionRequested.get(CEILING_MS, TimeUnit.MILLISECONDS)
            return pending
        }

        fun deliver(intent: Intent) = receiver.onReceive(context, intent)

        suspend fun await(pending: Deferred<UsbResult<Unit>>): UsbResult<Unit> =
            withTimeout(CEILING_MS) { pending.await() }.also { assertIs<UsbResult<Unit>>(it) }

        suspend fun isTrusted(): Boolean = TrustedUsbDevicesRepository(context).isDeviceTrusted(vid, pid)

        fun close() = scope.cancel()
    }

    data class Registration(val receiver: BroadcastReceiver, val permission: String?, val flags: Int)

    /**
     * El `Context` que ve la librería. Sólo responde lo que el camino del permiso usa, y ANOTA cada
     * registro de receiver con su protección para que el test la pueda afirmar.
     */
    private class FakeContext(private val usb: FakeUsbManager) : ContextWrapper(null) {
        val registrations: MutableList<Registration> = Collections.synchronizedList(mutableListOf())

        override fun getSystemService(name: String): Any? = if (name == Context.USB_SERVICE) usb else null
        override fun getPackageName(): String = PACKAGE
        override fun getOpPackageName(): String = PACKAGE
        override fun getApplicationContext(): Context = this
        override fun getFilesDir(): File = storeDir
        override fun checkPermission(permission: String, pid: Int, uid: Int): Int = PackageManager.PERMISSION_GRANTED

        override fun registerReceiver(receiver: BroadcastReceiver?, filter: IntentFilter?): Intent? =
            record(receiver, null, 0)

        override fun registerReceiver(receiver: BroadcastReceiver?, filter: IntentFilter?, flags: Int): Intent? =
            record(receiver, null, flags)

        override fun registerReceiver(
            receiver: BroadcastReceiver?, filter: IntentFilter?, permission: String?, scheduler: Handler?,
        ): Intent? = record(receiver, permission, 0)

        override fun registerReceiver(
            receiver: BroadcastReceiver?, filter: IntentFilter?, permission: String?, scheduler: Handler?, flags: Int,
        ): Intent? = record(receiver, permission, flags)

        override fun unregisterReceiver(receiver: BroadcastReceiver?) {
            registrations.removeAll { it.receiver === receiver }
        }

        private fun record(receiver: BroadcastReceiver?, permission: String?, flags: Int): Intent? {
            registrations += Registration(requireNotNull(receiver), permission, flags)
            return null
        }
    }

    /**
     * El broadcast, tal como lo recibe el `BroadcastReceiver`. `Intent` en el jar mockable no guarda
     * nada, así que la acción y los extras se contestan acá.
     */
    private class FakeIntent(
        private val act: String,
        private val device: FakeUsbDevice?,
        private val granted: Boolean,
    ) : Intent() {
        override fun getAction(): String = act
        override fun getExtras(): Bundle? = null

        override fun getBooleanExtra(name: String?, defaultValue: Boolean): Boolean =
            if (name == UsbManager.EXTRA_PERMISSION_GRANTED) granted else defaultValue

        @Suppress("UNCHECKED_CAST", "OVERRIDE_DEPRECATION")
        override fun <T : Parcelable?> getParcelableExtra(name: String?): T? =
            if (name == UsbManager.EXTRA_DEVICE) device as T? else null

        @Suppress("UNCHECKED_CAST")
        override fun <T : Any> getParcelableExtra(name: String?, clazz: Class<T>): T? =
            if (name == UsbManager.EXTRA_DEVICE) device as T? else null
    }
}
