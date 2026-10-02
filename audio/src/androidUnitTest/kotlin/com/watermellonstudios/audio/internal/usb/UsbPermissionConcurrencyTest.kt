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
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.cancel
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.AfterClass
import java.io.File
import java.nio.file.Files
import java.util.concurrent.CountDownLatch
import java.util.concurrent.CyclicBarrier
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * MINI-042 (AC-042.3) — **a lo sumo UNA espera de permiso registrada**.
 *
 * `requestPermissionSuspend` chequeaba "hay pedido en curso" y después registraba la continuación en
 * dos pasos sueltos, y `requestPermission` corre fuera de `connectMutex`: dos hilos veían el campo
 * vacío, el segundo pisaba la continuación del primero y el primero quedaba colgado para siempre (la
 * espera no tiene techo, REQ-050 D13), con el mutex tomado si era un `connectDevice`.
 *
 * ## Costuras
 *
 * - `UsbAudioManagerImpl.permissionRegistrationGate` (`internal`, null en producción): corre entre el
 *   chequeo y el registro. Sin ella la ventana no tiene dónde engancharse y el interleaving dependería
 *   del azar.
 * - `FakeUsbManager.onHasPermission`: retiene a un hilo dentro de `handlePermissionResult`, entre
 *   leer el pendiente y reanudarlo.
 *
 * Los techos de `withTimeout` sólo actúan si algo se cuelga: nada se sincroniza con una duración.
 */
class UsbPermissionConcurrencyTest {

    companion object {
        private const val CEILING_MS = 10_000L
        private const val ACTION_USB_PERMISSION = "com.watermellonstudios.audio.USB_PERMISSION"
        private const val PACKAGE = "com.watermellonstudios.audio.test"

        private val storeDir: File = Files.createTempDirectory("wma-usb-permission-conc").toFile()

        @JvmStatic
        @AfterClass
        fun cleanUp() {
            storeDir.deleteRecursively()
        }
    }

    /**
     * AC-042.3. Dos pedidos a la vez sobre el mismo device, forzados a la ventana chequeo→registro.
     *
     * Orden fijo: el primero en llegar a la costura espera a que llegue el segundo; el segundo espera
     * a que el primero haya registrado y pedido el diálogo. Así el segundo pasó el chequeo ANTES del
     * registro del primero, que es el interleaving del defecto.
     *
     * Bug que atrapa: registrar sin CAS. El segundo pisa la continuación del primero, se pide un
     * segundo diálogo y el primero no se reanuda nunca (se ve como timeout).
     */
    @Test
    fun `AC-042_3 dos pedidos concurrentes dejan una sola espera y ninguno cuelga`() =
        withRig(id = 11, vid = 0x3001, pid = 0x4001) { rig ->
            val arrivals = AtomicInteger(0)
            val secondArrived = CountDownLatch(1)
            rig.manager.permissionRegistrationGate = {
                if (arrivals.incrementAndGet() == 1) {
                    assertTrue(secondArrived.await(CEILING_MS, TimeUnit.MILLISECONDS), "el segundo pedido no llegó")
                } else {
                    secondArrived.countDown()
                    rig.usb.permissionRequested.get(CEILING_MS, TimeUnit.MILLISECONDS)
                }
            }

            val a = rig.scope.async(Dispatchers.Default) { rig.manager.requestPermission(rig.audioDevice) }
            val b = rig.scope.async(Dispatchers.Default) { rig.manager.requestPermission(rig.audioDevice) }

            // Entregar el resultado recién cuando el perdedor ya resolvió su CAS: con el código bueno
            // termina solo; con el viejo registra y pide un segundo diálogo.
            // Espera POR CONDICION con techo: el techo sólo actúa si nada de lo esperado ocurre.
            val deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(CEILING_MS)
            while (!(a.isCompleted || b.isCompleted || rig.usb.requestPermissionCalls.get() >= 2)) {
                check(System.nanoTime() < deadline) { "ningún pedido resolvió su registro" }
                Thread.onSpinWait()
            }
            rig.usb.permissionGranted.set(true)
            rig.deliver(rig.device)

            withTimeout(CEILING_MS) { awaitAll(a, b) }
            assertEquals(1, rig.usb.requestPermissionCalls.get(), "se pidió más de un diálogo")
        }

    /**
     * AC-042.3. Cancelar el pedido viejo no puede dejar la espera anotada, ni borrar la nueva.
     *
     * Bug que atrapa: una liberación que no suelta (el pedido nuevo quedaría como "en curso" para
     * siempre) o que suelta lo que no es suyo (el diálogo nuevo no reanudaría a nadie).
     */
    @Test
    fun `AC-042_3 cancelar el pedido viejo no bloquea ni borra al nuevo`() =
        withRig(id = 12, vid = 0x3002, pid = 0x4002) { rig ->
            val old = rig.scope.launch(start = CoroutineStart.UNDISPATCHED) { rig.manager.requestPermission(rig.audioDevice) }
            assertEquals(1, rig.usb.requestPermissionCalls.get())
            old.cancelAndJoin()

            val fresh: Deferred<Unit> =
                rig.scope.async(start = CoroutineStart.UNDISPATCHED) { rig.manager.requestPermission(rig.audioDevice) }
            assertEquals(2, rig.usb.requestPermissionCalls.get(), "el pedido nuevo se tomó por 'en curso' tras cancelar el viejo")

            rig.usb.permissionGranted.set(true)
            rig.deliver(rig.device)
            withTimeout(CEILING_MS) { fresh.await() }
        }

    /**
     * AC-042.3. El mismo resultado entregado a la vez desde dos hilos reanuda UNA vez.
     *
     * Ambos hilos se retienen dentro de `hasPermission` (ya leyeron el pendiente) y se sueltan juntos.
     *
     * Bug que atrapa: limpiar el campo y recién después reanudar sin CAS. Los dos reanudan, y el
     * segundo lanza `IllegalStateException: Already resumed` dentro del receiver.
     */
    @Test
    fun `AC-042_3 un resultado entregado dos veces a la vez no reanuda dos veces`() =
        withRig(id = 13, vid = 0x3003, pid = 0x4003) { rig ->
            val pending = rig.scope.async(start = CoroutineStart.UNDISPATCHED) { rig.manager.requestPermission(rig.audioDevice) }
            assertEquals(1, rig.usb.requestPermissionCalls.get())

            rig.usb.permissionGranted.set(true)
            val barrier = CyclicBarrier(2)
            rig.usb.onHasPermission = { barrier.await(CEILING_MS, TimeUnit.MILLISECONDS) }

            val deliveries = List(2) {
                rig.scope.async(Dispatchers.Default) { runCatching { rig.deliver(rig.device) } }
            }
            val outcomes = withTimeout(CEILING_MS) { deliveries.awaitAll() }
            rig.usb.onHasPermission = null

            outcomes.forEach { assertTrue(it.isSuccess, "la entrega lanzó: ${it.exceptionOrNull()}") }
            withTimeout(CEILING_MS) { pending.await() }
        }

    /**
     * AC-042.3. Si pedir el diálogo lanza, el pedido falla con esa excepción y el lugar queda libre.
     *
     * Bug que atrapa (review de MINI-042): registrar con el CAS y que `getBroadcast`/`requestPermission`
     * lancen después. El lugar quedaba ocupado para siempre, así que el pedido siguiente se tomaba por
     * "en curso" y no pedía el diálogo nunca más.
     */
    @Test
    fun `AC-042_3 si pedir el dialogo lanza, el lugar se suelta`() =
        withRig(id = 14, vid = 0x3004, pid = 0x4004) { rig ->
            rig.usb.requestPermissionThrows = IllegalStateException("el sistema rechazó el pedido")
            val failed = runCatching {
                withTimeout(CEILING_MS) { rig.manager.requestPermission(rig.audioDevice) }
            }
            assertTrue(failed.exceptionOrNull() is IllegalStateException, "esperaba la excepción del sistema: $failed")
            assertEquals(1, rig.usb.requestPermissionCalls.get())

            rig.usb.requestPermissionThrows = null
            val next: Deferred<Unit> =
                rig.scope.async(start = CoroutineStart.UNDISPATCHED) { rig.manager.requestPermission(rig.audioDevice) }
            assertEquals(2, rig.usb.requestPermissionCalls.get(), "el lugar quedó tomado por el pedido que lanzó")

            rig.usb.permissionGranted.set(true)
            rig.deliver(rig.device)
            withTimeout(CEILING_MS) { next.await() }
        }

    // ==================== El arnés ====================

    private fun withRig(id: Int, vid: Int, pid: Int, body: suspend (Rig) -> Unit) = runBlocking {
        val rig = Rig(id, vid, pid)
        try {
            body(rig)
        } finally {
            rig.scope.cancel()
        }
    }

    private class Rig(id: Int, vid: Int, pid: Int) {
        val usb = FakeUsbManager()
        val device = FakeUsbDevice(id = id, vid = vid, pid = pid)
        val context = FakeContext(usb)
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val manager = UsbAudioManagerImpl(context, scope)
        private val receiver: BroadcastReceiver

        val audioDevice = UsbAudioDevice(
            deviceId = device.deviceId.toString(),
            vendorId = vid,
            productId = pid,
            deviceName = device.productName,
            manufacturerName = device.manufacturerName,
            serialNumber = null,
            capabilities = UsbAudioCapabilities.UNKNOWN,
        )

        init {
            usb.attach(device)
            manager.setAutoConnectEnabled(false)
            manager.startMonitoring()
            receiver = assertNotNull(context.receiver, "startMonitoring no registró el receiver")
        }

        fun deliver(device: FakeUsbDevice) = receiver.onReceive(context, FakeIntent(device))
    }

    private class FakeContext(private val usb: FakeUsbManager) : ContextWrapper(null) {
        @Volatile
        var receiver: BroadcastReceiver? = null

        override fun getSystemService(name: String): Any? = if (name == Context.USB_SERVICE) usb else null
        override fun getPackageName(): String = PACKAGE
        override fun getOpPackageName(): String = PACKAGE
        override fun getApplicationContext(): Context = this
        override fun getFilesDir(): File = storeDir
        override fun checkPermission(permission: String, pid: Int, uid: Int): Int = PackageManager.PERMISSION_GRANTED

        override fun registerReceiver(receiver: BroadcastReceiver?, filter: IntentFilter?): Intent? = record(receiver)
        override fun registerReceiver(receiver: BroadcastReceiver?, filter: IntentFilter?, flags: Int): Intent? = record(receiver)
        override fun registerReceiver(
            receiver: BroadcastReceiver?, filter: IntentFilter?, permission: String?, scheduler: Handler?,
        ): Intent? = record(receiver)
        override fun registerReceiver(
            receiver: BroadcastReceiver?, filter: IntentFilter?, permission: String?, scheduler: Handler?, flags: Int,
        ): Intent? = record(receiver)

        override fun unregisterReceiver(receiver: BroadcastReceiver?) {
            if (this.receiver === receiver) this.receiver = null
        }

        private fun record(receiver: BroadcastReceiver?): Intent? {
            this.receiver = receiver
            return null
        }
    }

    private class FakeIntent(private val device: FakeUsbDevice) : Intent() {
        override fun getAction(): String = ACTION_USB_PERMISSION
        override fun getExtras(): Bundle? = null
        override fun getBooleanExtra(name: String?, defaultValue: Boolean): Boolean = defaultValue

        @Suppress("UNCHECKED_CAST", "OVERRIDE_DEPRECATION")
        override fun <T : Parcelable?> getParcelableExtra(name: String?): T? =
            if (name == UsbManager.EXTRA_DEVICE) device as T? else null

        @Suppress("UNCHECKED_CAST")
        override fun <T : Any> getParcelableExtra(name: String?, clazz: Class<T>): T? =
            if (name == UsbManager.EXTRA_DEVICE) device as T? else null
    }
}
