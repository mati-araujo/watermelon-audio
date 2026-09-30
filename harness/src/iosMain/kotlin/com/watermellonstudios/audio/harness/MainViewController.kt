package com.watermellonstudios.audio.harness

import androidx.compose.ui.window.ComposeUIViewController
import com.watermellonstudios.audio.harness.smoke.SmokeSink
import kotlinx.cinterop.BetaInteropApi
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.usePinned
import platform.Foundation.NSData
import platform.Foundation.NSTemporaryDirectory
import platform.Foundation.create
import platform.Foundation.writeToFile
import platform.UIKit.UIViewController

/**
 * Shell de iOS: el punto de entrada que el proyecto de Xcode envuelve en un
 * `UIViewControllerRepresentable`.
 *
 * Es lo unico especifico de iOS en todo el harness — todo lo demas vive en
 * commonMain, que es la superficie que consume un cliente KMP de verdad.
 *
 * MINI-038: el slot de plataforma NO lleva panel USB (en su lugar el harness dice "USB no aplica
 * en iOS") ni selector por fd. Las lineas `HARNESS-SMOKE` van a la salida estandar, y los fixtures
 * se escriben al directorio temporal de la app para cargarlos por path.
 *
 * Se exporta desde el framework `HarnessKit` (ver build.gradle.kts). Que sea una
 * `fun` de nivel superior importa: cinterop/K-N la expone a Swift como
 * `MainViewControllerKt.MainViewController()`.
 */
fun MainViewController(): UIViewController = ComposeUIViewController {
    HarnessApp(
        HarnessPlatform(
            smokeSink = SmokeSink { line -> println(line) },
            writeFile = ::writeTempFile,
        ),
    )
}

@OptIn(ExperimentalForeignApi::class, BetaInteropApi::class)
private fun writeTempFile(name: String, bytes: ByteArray): String {
    val path = NSTemporaryDirectory().trimEnd('/') + "/" + name
    val data = bytes.usePinned { pinned ->
        NSData.create(bytes = pinned.addressOf(0), length = bytes.size.toULong())
    }
    check(data.writeToFile(path, atomically = true)) { "no se pudo escribir $path" }
    return path
}
