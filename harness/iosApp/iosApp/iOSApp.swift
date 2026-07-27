import SwiftUI

/// WA-5.5 — punto de entrada del shell de iOS.
///
/// Es deliberadamente lo mas fino posible: todo lo que se puede escribir una vez
/// para las dos plataformas vive en `commonMain`, del lado de Kotlin. Si este
/// archivo empieza a crecer, algo se esta escribiendo dos veces.
@main
struct iOSApp: App {
    var body: some Scene {
        WindowGroup {
            HarnessView()
                .ignoresSafeArea(.keyboard)  // el teclado no debe empujar el layout
        }
    }
}
