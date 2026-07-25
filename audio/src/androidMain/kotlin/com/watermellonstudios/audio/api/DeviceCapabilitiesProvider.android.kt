package com.watermellonstudios.audio.api

import android.app.ActivityManager
import android.content.Context
import android.os.Build
import com.watermellonstudios.audio.domain.device.DeviceCapabilities
import com.watermellonstudios.audio.domain.device.DeviceCapabilitiesSnapshot
import com.watermellonstudios.audio.domain.device.DevicePlatform
import java.io.File
// El object legacy de androidMain se llama igual que la interfaz de commonMain, y este
// archivo necesita los dos. El alias es para que quede claro cuál es cuál en cada uso.
import com.watermellonstudios.audio.internal.util.DeviceCapabilities as AndroidDeviceCapabilities

/** Menos de 2 GB de RAM: el mismo umbral que ya usaba [AndroidDeviceCapabilities]. */
private const val LOW_RAM_THRESHOLD_MB = 2048L

/** 4 núcleos o menos en Android es, en la práctica, un dispositivo de entrada. */
private const val LOW_CORE_COUNT = 4

private val cached: DeviceCapabilities by lazy { readCapabilities() }

/**
 * Actual Android **sin `Context`**.
 *
 * La librería no recibe un `Context` en ningún punto de su API de motor, así que el
 * default tiene que arreglárselas sin él. Lo que se pierde es
 * `ActivityManager.isLowRamDevice` —la señal que el propio sistema publica— y por eso
 * existe la sobrecarga [deviceCapabilities] para el consumidor que sí tiene un
 * `Context` a mano.
 *
 * **Gama baja** = menos de 2 GB de RAM **o** 4 núcleos o menos, que es exactamente el
 * criterio que ya venía aplicando [AndroidDeviceCapabilities.isLowEndDevice]. No se
 * cambió al portarlo: un dispositivo no debería cambiar de gama por un refactor.
 */
actual fun currentDeviceCapabilities(): DeviceCapabilities = cached

/**
 * Igual que [currentDeviceCapabilities], pero además consulta al sistema.
 *
 * `ActivityManager.isLowRamDevice` es la palabra del OEM sobre el dispositivo, y le
 * gana a cualquier heurística de RAM y núcleos: un dispositivo marcado así corre
 * Android Go y tiene el sistema recortado, no sólo poca memoria.
 *
 * Delega en [AndroidDeviceCapabilities], que es donde ese criterio vive desde antes de
 * WA-1.2, para no terminar con dos definiciones de "gama baja" en el mismo módulo.
 *
 * @param context cualquier `Context`; sólo se usa para `getSystemService`, no se retiene.
 */
fun deviceCapabilities(context: Context): DeviceCapabilities {
    val base = cached
    val activityManager =
        context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager

    // Si el servicio no está (tests unitarios con stubs), la foto sin Context ya es
    // la mejor respuesta disponible — mejor eso que inventar un total de RAM en 0.
    val totalRamMb = activityManager?.let {
        val memInfo = ActivityManager.MemoryInfo()
        it.getMemoryInfo(memInfo)
        memInfo.totalMem / (1024L * 1024L)
    } ?: base.totalRamMb

    return DeviceCapabilitiesSnapshot(
        platform = DevicePlatform.ANDROID,
        apiLevel = base.apiLevel,
        totalRamMb = totalRamMb,
        cpuCoreCount = base.cpuCoreCount,
        supportsLowLatencyAudio = base.supportsLowLatencyAudio,
        isLowEndDevice = AndroidDeviceCapabilities.isLowEndDevice(context),
    )
}

private fun readCapabilities(): DeviceCapabilities {
    val cores = readCoreCount()
    val ramMb = readTotalRamMb()

    return DeviceCapabilitiesSnapshot(
        platform = DevicePlatform.ANDROID,
        apiLevel = Build.VERSION.SDK_INT,
        totalRamMb = ramMb,
        cpuCoreCount = cores,
        // minSdk 29 y AAudio existe desde 26: el path de baja latencia siempre está.
        // Que el hardware lo conceda es otra cosa, y eso sólo se sabe con el stream
        // abierto — por eso el contrato dice "hint".
        supportsLowLatencyAudio = true,
        isLowEndDevice = (ramMb in 1 until LOW_RAM_THRESHOLD_MB) || cores <= LOW_CORE_COUNT,
    )
}

/**
 * Núcleos que el dispositivo **tiene**, no los que están encendidos ahora.
 *
 * `Runtime.availableProcessors()` mapea a `_SC_NPROCESSORS_ONLN`: cuenta CPUs
 * *online*. Un governor que apagó cores por temperatura o por idle hace que un
 * octa-core reporte 4 — y como esta foto se cachea por proceso, una lectura
 * transitoria quedaría congelada y marcaría el dispositivo como gama baja para
 * siempre.
 *
 * `/sys/devices/system/cpu/possible` no tiene ese problema: es la topología que el
 * kernel configuró al bootear, y viene como `0-7` (o `0` en un solo núcleo).
 * Si no se puede leer, se cae al conteo de online, que es un piso razonable.
 */
private fun readCoreCount(): Int {
    val fromTopology = runCatching {
        val range = File("/sys/devices/system/cpu/possible").readText().trim()
        // "0-7" → 8; "0" → 1. El formato admite listas ("0-3,8-11") pero ningún
        // teléfono las usa; si aparece una, el parseo falla y cae al fallback.
        val parts = range.split("-")
        when (parts.size) {
            1 -> parts[0].toInt() + 1
            2 -> parts[1].toInt() - parts[0].toInt() + 1
            else -> 0
        }
    }.getOrDefault(0)

    return maxOf(fromTopology, Runtime.getRuntime().availableProcessors())
}

/**
 * RAM total desde `/proc/meminfo`, en MB. `0` si no se pudo leer.
 *
 * Es el único camino a la RAM física sin un `Context`. La línea es
 * `MemTotal:       3908588 kB` y el formato no cambió nunca en Linux; aun así, todo
 * lo que falle devuelve `0` —"no sé"— en vez de un número inventado, porque
 * `isLowEndDevice` trata el `0` como desconocido y no como "poca RAM".
 */
private fun readTotalRamMb(): Long = runCatching {
    File("/proc/meminfo").useLines { lines ->
        lines.firstOrNull { it.startsWith("MemTotal:") }
            ?.split(Regex("\\s+"))
            ?.getOrNull(1)
            ?.toLongOrNull()
            ?.div(1024L)
            ?: 0L
    }
}.getOrDefault(0L)
