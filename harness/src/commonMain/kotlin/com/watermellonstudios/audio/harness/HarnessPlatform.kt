package com.watermellonstudios.audio.harness

import androidx.compose.runtime.Composable
import com.watermellonstudios.audio.harness.smoke.SmokeReporter
import com.watermellonstudios.audio.harness.smoke.SmokeSink
import com.watermellonstudios.audio.harness.soundfont.SoundFontCheck

/**
 * MINI-038, D4 — el SLOT de plataforma. Lo que sólo existe en una plataforma entra a [HarnessApp]
 * como parámetro, desde el shell (`MainActivity` / `MainViewController`). Sin expect/actual: el
 * harness sigue siendo commonMain entero salvo los dos shells, y lo Android-only (USB, el selector
 * de archivos por fd) vive en androidMain sin que commonMain sepa que existe.
 *
 * @property smokeSink a dónde van las líneas `HARNESS-SMOKE` (logcat en Android, stdout en iOS).
 * @property writeFile escribe [ByteArray] en un archivo propio de la app y devuelve su path
 *   absoluto. Los fixtures viajan empaquetados (assets / bundle) y el motor los carga POR PATH, que
 *   es el camino de NoisyPad: esto es lo que convierte uno en el otro.
 * @property usbPanel el panel USB. `null` en iOS: el lugar dice "USB no aplica en iOS". Recibe
 *   `prepareForUsb`, que para el motor y lo reporta: el panel lo llama antes de conectar o de
 *   arrancar el streaming, igual que el plan.
 * @property usbSmoke la parte USB del plan automático; `null` si la plataforma no tiene USB. Recibe
 *   la función que toca las ventanas de escucha de REQ-053 S3 por libusb, para llamarla con el
 *   device conectado y el streaming parado.
 * @property soundFontExtras controles de SoundFont que sólo existen en una plataforma (en Android,
 *   el selector de archivos del sistema que carga por fd).
 * @property smokeRequest el plan pedido por adb, si lo hay.
 */
class HarnessPlatform(
    val smokeSink: SmokeSink,
    val writeFile: (name: String, bytes: ByteArray) -> String,
    val usbPanel: (@Composable (reporter: SmokeReporter, prepareForUsb: suspend () -> Boolean) -> Unit)? = null,
    val usbSmoke: (suspend (SmokeReporter, suspend (SmokeReporter) -> Boolean) -> Boolean)? = null,
    val soundFontExtras: (@Composable (SoundFontCheck, SmokeReporter) -> Unit)? = null,
    val smokeRequest: SmokeRequest? = null,
)

/**
 * Lo que llega por `am start ... --es harness.smoke <plan> --es harness.smoke.run <id>`.
 *
 * @property problem un defecto del pedido que el shell detectó al leerlo (un extra inválido). Si no
 *   es `null`, la corrida se reporta como inválida en vez de correr con un default callado.
 * @property seed la semilla del orden de las ventanas de escucha (`--es harness.smoke.semilla`,
 *   REQ-053 S3), o `null` si no vino.
 */
data class SmokeRequest(val plan: String, val run: String, val problem: String? = null, val seed: Long? = null)
