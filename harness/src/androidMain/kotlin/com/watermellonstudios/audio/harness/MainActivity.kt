package com.watermellonstudios.audio.harness

import android.content.pm.ApplicationInfo
import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import com.watermellonstudios.audio.harness.smoke.HarnessSmoke
import com.watermellonstudios.audio.harness.smoke.SmokeSink
import com.watermellonstudios.audio.harness.soundfont.SoundFontFdPicker
import com.watermellonstudios.audio.harness.usb.UsbHarness
import com.watermellonstudios.audio.harness.usb.UsbPanel
import java.io.File

/**
 * Shell de Android. Todo lo común vive en [HarnessApp]; acá sólo entra lo que existe únicamente en
 * Android, por el slot [HarnessPlatform] (MINI-038, D4): el panel USB, el selector por fd, logcat.
 *
 * ## El disparo por adb (MINI-038, D7)
 *
 * ```
 * adb shell am start -n com.watermellonstudios.audio.harness/.MainActivity \
 *     --es harness.smoke todo --es harness.smoke.run <id> [--es harness.smoke.usb-espera-s 120]
 * ```
 *
 * corre la secuencia automática y emite sus líneas `HARNESS-SMOKE` con `run=<id>`. Sólo en un
 * build DEBUGGABLE: el extra no es una puerta que un release deba tener abierta. Sólo en el primer
 * `onCreate` (no en una recreación): una rotación no puede volver a disparar el plan.
 */
class MainActivity : ComponentActivity() {

    private var usb: UsbHarness? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val usbHarness = UsbHarness(this).also { usb = it }

        val debuggable = (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0
        val plan = intent?.getStringExtra(EXTRA_PLAN)
        // Un extra presente e inválido NO cae callado al default: la corrida lo reporta y no arranca.
        val rawWait = intent?.getStringExtra(EXTRA_USB_WAIT_S)
        val waitS = rawWait?.toLongOrNull()?.takeIf { it > 0 }
        val waitProblem = if (rawWait != null && waitS == null) "usb-espera-s-invalido:$rawWait" else null
        val humanWaitMs = (waitS ?: DEFAULT_USB_WAIT_S) * 1000
        val request = if (debuggable && plan != null && savedInstanceState == null) {
            SmokeRequest(
                plan = plan,
                run = intent.getStringExtra(EXTRA_RUN) ?: "adb-${System.currentTimeMillis()}",
                problem = waitProblem,
            )
        } else {
            null
        }

        val platform = HarnessPlatform(
            smokeSink = SmokeSink { line -> Log.i(HarnessSmoke.TAG, line) },
            writeFile = { name, bytes ->
                val dir = File(cacheDir, "harness-fixtures").apply { mkdirs() }
                File(dir, name).apply { writeBytes(bytes) }.absolutePath
            },
            usbPanel = { reporter, prepareForUsb -> UsbPanel(usbHarness, reporter, prepareForUsb) },
            usbSmoke = { reporter -> usbHarness.runAutomatic(reporter, humanWaitMs) },
            soundFontExtras = { check, reporter -> SoundFontFdPicker(check, reporter) },
            smokeRequest = request,
        )
        setContent { HarnessApp(platform) }
    }

    override fun onDestroy() {
        usb?.release()
        usb = null
        super.onDestroy()
    }

    private companion object {
        const val EXTRA_PLAN = "harness.smoke"
        const val EXTRA_RUN = "harness.smoke.run"
        const val EXTRA_USB_WAIT_S = "harness.smoke.usb-espera-s"
        const val DEFAULT_USB_WAIT_S = 120L
    }
}
