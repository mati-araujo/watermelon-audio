package com.watermellonstudios.audio.internal.bridge

import java.io.File
import kotlin.test.fail

/**
 * **El hueco, medido y dicho** (REQ-016 S3).
 *
 * Este arnés cubre 13 funciones de más de trescientas. Un arnés que cubre 13 y
 * **no lo dice** se lee como "el JNI está probado", y ése es exactamente el modo
 * de falla que REQ-016 vino a borrar. Así que el conteo se imprime en cada
 * corrida del gate, con los dos números medidos.
 *
 * ## Ninguno de los dos números está escrito a mano
 *
 * El numerador se **anota al cruzar la frontera** ([JniHarness.exercise]); el
 * denominador se **cuenta del árbol** ([JniExports]). No es purismo: la spec de
 * este REQ decía "309" y el árbol tenía **310** al día siguiente, y el bloque de
 * conteos de `CLAUDE.md` quedó stale cuatro veces seguidas. Un número escrito a
 * mano envejece sin cambiar de color.
 *
 * ## Las tres degradaciones son FALLO, nunca un pase (AC-016.4)
 *
 * Un arnés que no cargó la librería, que no encontró el árbol, o que ejecutó
 * **cero** funciones se ve desde afuera igual que uno que ejerció todo: `ok 1s`.
 * Las tres terminan en `fail()`. El `JniHarnessSelfTest` las ejerce.
 */
internal object JniCoverage {

    /**
     * Lo ejecutado, **por clase del arnés**.
     *
     * Por clase y no en un solo conjunto porque el `@AfterClass` corre una vez por
     * clase: un acumulado global se afirmaría distinto según el orden en que Gradle
     * corra las clases, o sea rojo intermitente.
     */
    private val byOwner = linkedMapOf<String, MutableSet<String>>()

    @Synchronized
    fun record(owner: String, nativeName: String) {
        byOwner.getOrPut(owner) { linkedSetOf() } += nativeName
    }

    @Synchronized
    fun allExecuted(): Set<String> = byOwner.values.flatten().toSet()

    /**
     * Cierre de una clase del arnés: **imprime el conteo y falla si no se ejerció
     * nada o si se anotó un nombre que no existe en el árbol.**
     *
     * Se llama desde el `@AfterClass` de cada clase del arnés, así que con más de
     * una clase salen varias líneas ACUMULATIVAS y la última es la completa. Es a
     * propósito: la alternativa —un gancho de apagado de la JVM— imprime después
     * de que Gradle dejó de reenviar la salida del worker, o sea que el número
     * dejaría de estar en el log del gate, que es donde tiene que estar.
     */
    @Synchronized
    fun requireCoverage(owner: String, declared: Set<String>) {
        val inventory = JniExports.fromTree()
        val executed = byOwner[owner].orEmpty()
        verify(owner, executed, inventory)
        ratchet(owner, declared, executed)
        println(report(owner, allExecuted(), inventory))
    }

    /**
     * **Trinquete bidireccional sobre la cobertura**, igual que
     * `scripts/rt-coverage-baseline.txt` y `mechanism-callers-baseline.txt`: falla si
     * la clase ejerce MENOS de lo declarado **y también si ejerce más**.
     *
     * Existe porque el conteo, solo, no defiende nada: lo destapó un mutante que sacó
     * una función del arnés —llamada directa en vez de anotada— y **sobrevivió**. El
     * número bajaba de 13 a 12, el gate lo imprimía, y nadie se ponía rojo. La
     * cobertura se puede erosionar hasta cero mientras el arnés sigue en verde, que es
     * exactamente el eje de degradación que vigila el criterio de muerte de este REQ.
     *
     * Bidireccional para que **su diff sea la revisión**: sumar cobertura obliga a
     * declararla, así que aparece en el PR en vez de colarse.
     */
    private fun ratchet(owner: String, declared: Set<String>, executed: Set<String>) {
        val faltantes = declared - executed
        val nuevas = executed - declared
        if (faltantes.isEmpty() && nuevas.isEmpty()) return
        fail(
            buildString {
                append("$owner declara cubrir ${declared.size} funciones JNIEXPORT y ejecutó ${executed.size}.\n")
                if (faltantes.isNotEmpty()) {
                    append("  DEJÓ DE EJERCER: ${faltantes.joinToString()}\n")
                    append("  La cobertura se erosionó. El arreglo es volver a ejercerlas, NO bajar la lista.\n")
                }
                if (nuevas.isNotEmpty()) {
                    append("  EJERCE SIN DECLARAR: ${nuevas.joinToString()}\n")
                    append("  Sumá esos nombres a la lista de la clase: el trinquete es bidireccional a propósito,\n")
                    append("  para que sumar cobertura aparezca en el diff del PR en vez de colarse.\n")
                }
            },
        )
    }

    /** El chequeo, separado del estado global para que el self-test lo pueda ejercer. */
    fun verify(owner: String, executed: Set<String>, inventory: JniExports.Inventory) {
        if (executed.isEmpty()) {
            fail(
                "$owner terminó sin haber ejecutado UNA sola función JNIEXPORT. Un arnés que no " +
                    "ejerció nada no es cobertura: es un verde vacío.",
            )
        }
        // Cierra el agujero del numerador: el nombre lo pasa el test como string, así
        // que sin esto un typo —o un copy-paste— inflaría la cobertura sin ejecutar
        // nada distinto.
        val unknown = executed - inventory.entryPoints
        if (unknown.isNotEmpty()) {
            fail(
                "$owner anotó como ejecutadas funciones que NO existen como JNIEXPORT en " +
                    "${inventory.root}: ${unknown.joinToString()}. El conteo estaría inflado.",
            )
        }
    }

    fun report(owner: String, executed: Set<String>, inventory: JniExports.Inventory): String = buildString {
        append("[REQ-016] arnés JNI (acumulado, tras $owner): ")
        append("${executed.size} de ${inventory.jniexportCount} funciones JNIEXPORT ejecutadas ")
        append("contra un JNIEnv real · hueco: ${inventory.jniexportCount - executed.size}\n")
        append("          desglose medido: ${inventory.entryPoints.size} entradas Java_* de Kotlin, ")
        append("${inventory.jniexportCount - inventory.entryPoints.size} JNI_OnLoad/JNI_OnUnload, ")
        append("${inventory.notHostPortable} que ni siquiera compilan para el host (jni_benchmark.cpp, depende de Oboe)\n")
        append("          🔴 backend FALSO adentro: esto valida la frontera JNI/Kotlin, NO audio en dispositivo\n")
        append("          ejecutadas: ${executed.joinToString()}")
    }
}

/**
 * El denominador, **contado del árbol** y no declarado.
 *
 * Si no encuentra el árbol, **falla**: un inventario que no pudo mirar devolvería
 * cero y "0 de 0" se imprimiría igual de verde.
 */
internal object JniExports {

    private const val PREFIX = "Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_"
    private val EXPORT_LINE = Regex("""^JNIEXPORT\b""", RegexOption.MULTILINE)
    private val ENTRY_POINT = Regex("""^${Regex.escape(PREFIX)}(\w+)""", RegexOption.MULTILINE)

    /**
     * @property jniexportCount todas las `JNIEXPORT` del árbol — el denominador de AC-016.3.
     * @property entryPoints los nombres que Kotlin declara como `external fun`.
     * @property notHostPortable las de `jni_benchmark.cpp`, que dependen de `<oboe/Oboe.h>`.
     */
    data class Inventory(
        val jniexportCount: Int,
        val entryPoints: Set<String>,
        val notHostPortable: Int,
        val root: File,
    )

    fun fromTree(): Inventory = fromDirectory(locateJniSources())

    /** Separado para que el self-test lo apunte a un directorio sin fuentes. */
    fun fromDirectory(dir: File): Inventory {
        val sources = dir.listFiles { f -> f.isFile && f.name.endsWith(".cpp") }?.sortedBy { it.name }
        if (sources.isNullOrEmpty()) {
            fail(
                "el inventario de JNIEXPORT no encontró fuentes en '$dir'. Esto NO es 'cero funciones': " +
                    "es un conteo que no se pudo medir, y un denominador que no se midió no se puede " +
                    "publicar como cobertura.",
            )
        }
        var total = 0
        var benchmark = 0
        val names = linkedSetOf<String>()
        for (file in sources) {
            val text = file.readText()
            val count = EXPORT_LINE.findAll(text).count()
            total += count
            if (file.name == "jni_benchmark.cpp") benchmark = count
            ENTRY_POINT.findAll(text).forEach { names += it.groupValues[1] }
        }
        if (total == 0) {
            fail("el inventario leyó ${sources.size} archivos en '$dir' y no encontró una sola JNIEXPORT: el parseo se rompió.")
        }
        return Inventory(total, names, benchmark, dir)
    }

    /**
     * Sube desde el directorio de trabajo buscando `audio/src/main/cpp/jni`.
     *
     * Se busca en vez de cablear la ruta porque el directorio de trabajo de la task
     * de test es del build, no de este archivo — y una ruta relativa cableada es lo
     * que rompe el día que alguien mueve el módulo.
     */
    private fun locateJniSources(): File {
        val relative = File("audio/src/main/cpp/jni")
        var dir: File? = File(System.getProperty("user.dir")).absoluteFile
        while (dir != null) {
            val candidate = File(dir, relative.path)
            if (candidate.isDirectory) return candidate
            dir = dir.parentFile
        }
        fail(
            "no encontré 'audio/src/main/cpp/jni' subiendo desde '${System.getProperty("user.dir")}'. " +
                "Sin el árbol no hay denominador que medir, y publicar el conteo sin él sería inventarlo.",
        )
    }
}
