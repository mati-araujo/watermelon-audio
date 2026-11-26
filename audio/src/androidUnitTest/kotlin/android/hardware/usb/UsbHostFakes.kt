package android.hardware.usb

import android.app.PendingIntent
import java.util.concurrent.CompletableFuture
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

/**
 * Los dobles de `UsbManager` y `UsbDevice` para los tests de host del camino USB (REQ-050 S1).
 *
 * ## Por qué viven en `android.hardware.usb`
 *
 * El repo no usa Robolectric ni una librería de mocks, y los dos tipos del SDK tienen el
 * constructor **package-private**: desde otro paquete no se pueden instanciar ni heredar. En la
 * JVM de `testDebugUnitTest` el `android.jar` "mockable" y estas clases los carga el mismo
 * classloader, así que comparten paquete de runtime y el constructor es accesible.
 *
 * La alternativa era abrir una costura en producción (una interfaz sobre `UsbManager`) sólo para
 * poder probar: más superficie en una clase de 1700 líneas que S2 también toca. Esto no cambia
 * una línea de `androidMain`.
 *
 * 🔴 Sólo sustituye lo que el camino del permiso lee. Lo demás de la clase base devuelve el valor
 * por defecto del jar mockable (`isReturnDefaultValues = true`), no lo que haría un device.
 */
class FakeUsbManager : UsbManager() {

    private val devices = HashMap<String, UsbDevice>()

    /** Lo que `UsbManager.hasPermission` contesta: la verdad del sistema, no la del intent. */
    val permissionGranted = AtomicBoolean(false)

    /** Se completa cuando la librería pidió el diálogo, o sea cuando ya hay una espera pendiente. */
    val permissionRequested = CompletableFuture<UsbDevice>()

    /** Cuántas veces se intentó abrir el device: sólo pasa si el permiso se dio por bueno. */
    val openDeviceCalls = AtomicInteger(0)

    fun attach(device: UsbDevice) {
        devices[device.deviceName] = device
    }

    override fun getDeviceList(): HashMap<String, UsbDevice> = HashMap(devices)

    /** Se invoca en cada `hasPermission`: deja retener a un hilo en ese punto (MINI-042). */
    @Volatile
    var onHasPermission: (() -> Unit)? = null

    /** Cuántas veces la librería pidió el diálogo (MINI-042: tiene que ser una por pedido en curso). */
    val requestPermissionCalls = AtomicInteger(0)

    override fun hasPermission(device: UsbDevice?): Boolean {
        onHasPermission?.invoke()
        return permissionGranted.get()
    }

    /** Si no es null, `requestPermission` lo lanza (MINI-042: el lugar registrado se tiene que soltar). */
    @Volatile
    var requestPermissionThrows: RuntimeException? = null

    override fun requestPermission(device: UsbDevice?, pi: PendingIntent?) {
        requestPermissionCalls.incrementAndGet()
        requestPermissionThrows?.let { throw it }
        permissionRequested.complete(device)
    }

    /**
     * Lo que devuelve [openDevice]. Por defecto `null`: una conexión de verdad necesita un file
     * descriptor de usbfs que el host no tiene, y en los tests del permiso (S1) que se haya LLAMADO
     * es la observación. Los del contrato de conexión (S2) le ponen una [FakeUsbDeviceConnection].
     */
    @Volatile
    var connectionFactory: (UsbDevice?) -> UsbDeviceConnection? = { null }

    override fun openDevice(device: UsbDevice?): UsbDeviceConnection? {
        openDeviceCalls.incrementAndGet()
        return connectionFactory(device)
    }
}

/**
 * Una conexión con un fd inventado (REQ-050 S2). El fd no se usa: el puerto nativo de los tests
 * es un doble, así que lo único que importa es que la librería lo pase y cierre la conexión cuando
 * corresponde.
 */
class FakeUsbDeviceConnection(private val fd: Int) : UsbDeviceConnection() {
    val closeCalls = AtomicInteger(0)
    override fun getFileDescriptor(): Int = fd
    override fun close() {
        closeCalls.incrementAndGet()
    }
}

class FakeUsbDevice(
    private val id: Int,
    private val vid: Int,
    private val pid: Int,
) : UsbDevice() {
    override fun getDeviceId(): Int = id
    override fun getVendorId(): Int = vid
    override fun getProductId(): Int = pid
    override fun getDeviceName(): String = "/dev/bus/usb/001/%03d".format(id)
    override fun getProductName(): String = "Fake DAC $id"
    override fun getManufacturerName(): String = "Fake"
    override fun getSerialNumber(): String? = null
    override fun getDeviceClass(): Int = 1 // USB_CLASS_AUDIO
    override fun getInterfaceCount(): Int = 0
}
