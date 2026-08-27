package com.watermellonstudios.audio.internal.bridge

import java.io.File
import java.nio.file.Files
import kotlin.test.Test
import kotlin.test.assertContains
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

/**
 * REQ-016 S3 — **el arnés sabe fallar.**
 *
 * Es el `--self-test` de los guardrails de este repo, escrito como tests porque
 * el arnés es un test. La razón es la de siempre: un chequeo que no sabe fallar
 * da verde para siempre y **se ve idéntico** a uno que anda. Acá se ejercen las
 * cuatro degradaciones que convertirían al arnés en un `ok 1s` vacío:
 *
 * 1. la librería nativa no se puede cargar,
 * 2. el árbol de fuentes no aparece, así que no hay denominador,
 * 3. se ejecutaron **cero** funciones,
 * 4. el numerador se infla con un nombre que no existe.
 *
 * Ninguna de las cuatro puede terminar en un pase (AC-016.4). Y el conteo se
 * prueba **haciéndolo mentir**: si sacar una función no baja el número, el conteo
 * está escrito a mano y no medido (AC-016.3).
 */
class JniHarnessSelfTest {

    private val realInventory get() = JniExports.fromTree()

    @Test
    fun `una libreria que no existe falla nombrando la CAUSA REAL y donde busco`() {
        val ausente = "watermelon_audio_que_no_existe"

        // La causa de verdad, sacada de la JVM y no escrita a mano: es contra ESTO que
        // se compara. Lo trajo un mutante que le sacaba `$cause` al mensaje y
        // sobrevivía — el self-test miraba el envoltorio y no el contenido.
        val real = assertFailsWith<UnsatisfiedLinkError> { System.loadLibrary(ausente) }.message.orEmpty()
        assertTrue(real.isNotEmpty(), "premisa: la JVM tiene que dar un mensaje de error")

        val diagnosis = JniHarness.loadDiagnosis(ausente)

        assertContains(diagnosis, real, message = "el diagnóstico NO trae la causa real que dio la JVM")
        assertContains(diagnosis, ausente, message = "el diagnóstico no nombra la librería")
        assertContains(diagnosis, "java.library.path", message = "el diagnóstico no dice dónde buscó")
        assertContains(diagnosis, "scripts/build-host-jni.sh", message = "el diagnóstico no dice cómo construirla")
    }

    @Test
    fun `el trinquete de cobertura falla en las DOS direcciones`() {
        JniCoverage.record("un dueño de mentira", "nativeStartTuner")

        val menos = assertFailsWith<AssertionError> {
            JniCoverage.requireCoverage("un dueño de mentira", setOf("nativeStartTuner", "nativeStopTuner"))
        }
        assertContains(menos.message.orEmpty(), "DEJÓ DE EJERCER")
        assertContains(menos.message.orEmpty(), "nativeStopTuner")

        val mas = assertFailsWith<AssertionError> {
            JniCoverage.requireCoverage("un dueño de mentira", emptySet())
        }
        assertContains(mas.message.orEmpty(), "EJERCE SIN DECLARAR")
        assertContains(mas.message.orEmpty(), "nativeStartTuner")
    }

    @Test
    fun `un arbol sin fuentes falla en vez de devolver cero`() {
        val empty = Files.createTempDirectory("jni-sin-fuentes").toFile()
        try {
            val error = assertFailsWith<AssertionError> { JniExports.fromDirectory(empty) }
            assertContains(
                error.message.orEmpty(),
                "no se pudo medir",
                message = "un inventario vacío tiene que distinguirse de un inventario en cero",
            )
        } finally {
            empty.delete()
        }
    }

    @Test
    fun `cero funciones ejecutadas es un fallo, no un verde vacio`() {
        val error = assertFailsWith<AssertionError> {
            JniCoverage.verify("otro dueño de mentira", emptySet(), realInventory)
        }
        assertContains(error.message.orEmpty(), "verde vacío")
    }

    @Test
    fun `un nombre inventado no puede inflar el conteo`() {
        val error = assertFailsWith<AssertionError> {
            JniCoverage.verify("otro dueño de mentira", setOf("nativeFuncionQueNoExiste"), realInventory)
        }
        assertContains(error.message.orEmpty(), "nativeFuncionQueNoExiste")
    }

    /**
     * 🔴 **El conteo, probado haciéndolo mentir.**
     *
     * Es la mitad que el resto del arnés no puede darse: que el número BAJE al
     * sacar una función. Si no bajara, estaría escrito a mano — y así es como
     * envejecieron los conteos de `CLAUDE.md`, cuatro veces, en verde.
     */
    @Test
    fun `sacar una funcion baja el numerador, y agregar una JNIEXPORT sube el denominador`() {
        val inventory = realInventory
        val dos = setOf("nativeStartTuner", "nativeStopTuner")
        val una = setOf("nativeStartTuner")

        val conDos = JniCoverage.report("x", dos, inventory)
        val conUna = JniCoverage.report("x", una, inventory)
        assertContains(conDos, "2 de ${inventory.jniexportCount}")
        assertContains(conUna, "1 de ${inventory.jniexportCount}")
        assertTrue("nativeStopTuner" !in conUna, "sacar la función no la sacó del reporte")

        // Y el denominador sale del árbol: una JNIEXPORT más en un árbol de mentira
        // tiene que subirlo. Sin esto, "310" podría ser una constante.
        val fake = Files.createTempDirectory("jni-arbol-de-mentira").toFile()
        try {
            val source = File(fake, "jni_falso.cpp")
            source.writeText(
                """
                JNIEXPORT jboolean JNICALL
                Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeUno(
                    JNIEnv* env, jobject thiz) { return JNI_TRUE; }
                """.trimIndent(),
            )
            assertEquals(1, JniExports.fromDirectory(fake).jniexportCount, "una JNIEXPORT en el árbol, una contada")

            source.appendText(
                """

                JNIEXPORT void JNICALL
                Java_com_watermellonstudios_audio_internal_bridge_AudioNativeBridge_nativeDos(
                    JNIEnv* env, jobject thiz) {}
                """.trimIndent(),
            )
            val crecido = JniExports.fromDirectory(fake)
            assertEquals(2, crecido.jniexportCount, "el denominador no siguió al árbol: está escrito a mano")
            assertEquals(setOf("nativeUno", "nativeDos"), crecido.entryPoints)
        } finally {
            fake.deleteRecursively()
        }
    }

    /**
     * El inventario real, contra el árbol real. No fija el número —driftea, y por
     * eso se mide— pero sí fija lo que tiene que seguir siendo cierto de él.
     */
    @Test
    fun `el inventario real es coherente consigo mismo`() {
        val inventory = realInventory
        assertTrue(inventory.jniexportCount > inventory.entryPoints.size, "JNI_OnLoad/JNI_OnUnload son JNIEXPORT y no son entradas de Kotlin")
        assertTrue(inventory.notHostPortable > 0, "jni_benchmark.cpp tiene JNIEXPORT que no cross-compilan")
        assertContains(inventory.entryPoints, "nativeAnalyzeTunerBuffer")
        assertContains(inventory.entryPoints, "nativeStartTuner")
    }
}
