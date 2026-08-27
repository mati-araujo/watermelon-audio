package com.watermellonstudios.audio.internal.bridge

import com.watermellonstudios.audio.internal.native.NativeLibraryLoader
import java.io.File
import kotlin.test.fail

/**
 * El arnés que hace que un test de JVM **ejecute** funciones `JNIEXPORT` reales
 * contra un `JNIEnv` real (REQ-016).
 *
 * ## Qué prueba, y qué NO
 *
 * Prueba **la frontera**: que el símbolo que Kotlin declara existe, que su FIRMA
 * coincide con la que el C++ implementa, y que el manejo de `JNIEnv` del otro
 * lado —pinneo de arrays, referencias, excepciones— no rompe nada. El gate de
 * MINI-001 (`check-jni-symbols.py`) compara sólo NOMBRES: un `Int` declarado
 * donde el C++ espera `jlong` compila de los dos lados, linkea, pasa ese gate y
 * corrompe memoria en el device. Esto es lo único que lo agarra antes.
 *
 * 🔴 **NO prueba audio en dispositivo, y no puede.** La librería que carga es
 * `audio/src/main/cpp/tests/hostjni`, que lleva adentro un `FakeAudioBackend`
 * (la misma sustitución de UNA sola unidad de traducción que usa la suite de
 * C++). Leer "el JNI está probado" de acá sería exactamente la falsa sensación
 * de cobertura que REQ-016 vino a borrar: son 13 funciones de 310.
 *
 * ## "No pude" nunca es un pase
 *
 * `NativeLibraryLoader.ensureLoaded()` devuelve `false` **en silencio** cuando no
 * encuentra la librería, y sólo loguea. Un arnés que lo usara tal cual pasaría en
 * verde sin haber cargado nada. [requireNativeLibrary] lo convierte en un fallo
 * que nombra la causa — la misma regla que AC-3 de MINI-001 y que
 * `fetch-corpus.sh`.
 */
internal object JniHarness {

    /** Lo que `System.loadLibrary("watermelon_audio")` va a buscar. */
    private const val LIBRARY_NAME = "watermelon_audio"

    @Volatile
    private var loaded = false

    /**
     * Carga la librería nativa **por el camino de producción** y falla si no pudo,
     * nombrando la causa.
     *
     * Se entra por [NativeLibraryLoader] y no por `System.loadLibrary` directo a
     * propósito: es el camino que corre en el device, y probarlo es parte del
     * punto. La llamada directa de abajo existe sólo para **recuperar el mensaje**
     * que `ensureLoaded()` se traga, y sólo se alcanza cuando ya falló.
     */
    fun requireNativeLibrary() {
        if (loaded) return
        if (NativeLibraryLoader.ensureLoaded()) {
            loaded = true
            return
        }
        fail(loadDiagnosis(LIBRARY_NAME))
    }

    /**
     * El mensaje que explica **por qué** no se pudo cargar, con la causa real, dónde
     * se buscó y cómo construirla.
     *
     * Está separado y es alcanzable desde el self-test a propósito: se lo ejerce con
     * un nombre inexistente, porque un diagnóstico que nunca se probó es prosa —
     * exactamente la clase de cosa que este repo dejó de aceptar.
     *
     * La llamada directa a `System.loadLibrary` de acá abajo NO es el camino del
     * arnés (ése es [NativeLibraryLoader], el de producción): existe sólo para
     * recuperar el `UnsatisfiedLinkError` que `ensureLoaded()` se traga.
     */
    fun loadDiagnosis(name: String): String {
        val path = System.getProperty("java.library.path").orEmpty()
        val cause = try {
            System.loadLibrary(name)
            "ninguna — cargó al reintentar, o sea que ensureLoaded() devolvió false sin motivo"
        } catch (e: UnsatisfiedLinkError) {
            e.message ?: e.toString()
        }
        return "el arnés JNI no pudo cargar lib$name. Esto NO es un test que se saltea:\n" +
            "  causa            : $cause\n" +
            "  java.library.path: ${path.ifEmpty { "(vacío)" }}\n" +
            "  candidatos en esas rutas: ${describeCandidates(name, path)}\n" +
            "Construila con: bash scripts/build-host-jni.sh"
    }

    private fun describeCandidates(name: String, path: String): String {
        if (path.isEmpty()) return "no hay rutas para mirar"
        val hits = path.split(File.pathSeparator)
            .filter { it.isNotBlank() }
            .flatMap { dir ->
                listOf("lib$name.so", "lib$name.dylib")
                    .map { File(dir, it) }
                    .filter { it.isFile }
            }
        return if (hits.isEmpty()) "ninguno" else hits.joinToString { it.path }
    }

    /**
     * Ejecuta [call] contra el bridge real y **anota** que la función `native$name`
     * cruzó la frontera.
     *
     * El nombre se anota acá y no se declara en una lista porque el conteo de
     * AC-016.3 tiene que salir de lo que REALMENTE corrió: una lista escrita a
     * mano envejece en silencio, que es como quedaron stale cuatro veces seguidas
     * los conteos de `CLAUDE.md`.
     */
    fun <T> exercise(nativeName: String, call: (AudioNativeBridge) -> T): T {
        requireNativeLibrary()
        val result = call(AudioNativeBridge.getInstance())
        JniCoverage.record(nativeName)
        return result
    }
}
