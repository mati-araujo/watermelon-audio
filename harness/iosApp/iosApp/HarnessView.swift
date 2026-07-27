import SwiftUI
import UIKit
import HarnessKit

/// Envuelve el `UIViewController` que produce Compose Multiplatform.
///
/// `MainViewControllerKt.MainViewController()` es la `fun` de nivel superior de
/// `harness/src/iosMain/.../MainViewController.kt`. Kotlin/Native nombra la clase
/// ObjC por el ARCHIVO —de ahi el sufijo `Kt`— y no por la funcion; el gate
/// (`scripts/build-harness.sh`) afirma la firma contra el header generado
/// justamente porque el nombre de la clase no dice nada sobre si el punto de
/// entrada existe.
struct HarnessView: UIViewControllerRepresentable {
    func makeUIViewController(context: Context) -> UIViewController {
        MainViewControllerKt.MainViewController()
    }

    // Compose maneja su propio estado; no hay nada que empujar desde SwiftUI.
    func updateUIViewController(_ uiViewController: UIViewController, context: Context) {}
}
