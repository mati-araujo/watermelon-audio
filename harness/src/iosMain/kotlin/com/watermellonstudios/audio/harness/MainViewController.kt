package com.watermellonstudios.audio.harness

import androidx.compose.ui.window.ComposeUIViewController
import platform.UIKit.UIViewController

/**
 * Shell de iOS: el punto de entrada que el proyecto de Xcode envuelve en un
 * `UIViewControllerRepresentable`.
 *
 * Es lo unico especifico de iOS en todo el harness — todo lo demas vive en
 * commonMain, que es la superficie que consume un cliente KMP de verdad.
 *
 * Se exporta desde el framework `HarnessKit` (ver build.gradle.kts). Que sea una
 * `fun` de nivel superior importa: cinterop/K-N la expone a Swift como
 * `MainViewControllerKt.MainViewController()`.
 */
fun MainViewController(): UIViewController = ComposeUIViewController { HarnessApp() }
