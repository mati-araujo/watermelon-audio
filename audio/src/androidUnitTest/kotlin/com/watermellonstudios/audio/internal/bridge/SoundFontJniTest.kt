package com.watermellonstudios.audio.internal.bridge

import org.junit.AfterClass
import org.junit.Before
import java.io.File
import java.io.FileDescriptor
import java.io.RandomAccessFile
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

/**
 * REQ-024 S1 — **el camino SoundFont, ejecutado.**
 *
 * Se eligió este grupo por lo que **cruza**, no por su tamaño — y porque es el único
 * grupo grande que quedaba con estado round-trippable **sin render**:
 *
 * - **`jstring` de vuelta** (`NewStringUTF`, en `getSoundFontPresetName`): ninguna otra
 *   clase del arnés ejerce un retorno de `String`.
 * - **`jintArray` de vuelta ×2** (`NewIntArray` + `SetIntArrayRegion`): REQ-023 cubrió
 *   arrays de **entrada**; de **salida**, ninguno.
 * - **`jbyteArray` de entrada** y **`jstring` de entrada**.
 * - 🔴 **`nativeLoadSoundFontFromFd(jint, jlong, jlong)`** — el **primer cruce de
 *   parámetro `jlong`** que ejerce el arnés. Hay 16 funciones con `Long` en la firma y
 *   hasta hoy la única cubierta era un `Long` de **retorno**. Un `Int` declarado donde el
 *   C++ espera `jlong` compila de los dos lados, linkea, **pasa `check-jni-symbols.py`**
 *   —que compara sólo NOMBRES— y corrompe memoria en el device.
 *
 * ## Por qué el grupo que se venía apuntando NO era éste
 *
 * El backlog apuntaba a **arp/escala**. Se midió y no servía: `getArpTotalSteps`,
 * `getArpCurrentStep` e `isArpGateOpen` se publican **sólo** desde `ArpSequencer::process()`
 * (`:376-378`), o sea desde el callback de audio, y el `FakeAudioBackend` del host
 * **guarda** el callback y nunca lo invoca. De las 26 de arp/chord, **una sola** es
 * round-trip real. Las otras 25 habrían sido cobertura write-only.
 *
 * ## El fixture tiene DOS presets, y es la razón de que exista
 *
 * El de C++ (`core/tests/support/MinimalSoundFont.h`) tiene **uno**, y con uno solo un
 * getter cableado al primero **sobrevive a todos los asserts** — el mutante que REQ-022 y
 * REQ-023 midieron por separado. Ver [MinimalSoundFont].
 *
 * 🔴 **Y el rango de teclas NO sale del SF2**: `SoundFontManager::inferKeyRange` lo deduce
 * del **nombre** del preset con `strstr`. Un test que declarara un generador `keyRange` en
 * el fixture y lo afirmara estaría midiendo la heurística contra sí misma. De ahí
 * `"Cello Uno"` → 36..84 y `"Violin Dos"` → 55..103: dos ramas distintas de esa función.
 *
 * 🔴 Verde acá NO significa "el SoundFont está probado": esto valida la **frontera**, no
 * que un preset suene. Ver el KDoc de [JniHarness].
 */
class SoundFontJniTest {

    companion object {
        private const val OWNER = "SoundFontJniTest"

        private val COVERED = setOf(
            "nativeStartTuner",
            "nativeLoadSoundFont",
            "nativeLoadSoundFontFromPath",
            "nativeLoadSoundFontFromFd",
            "nativeUnloadSoundFont",
            "nativeIsSoundFontLoaded",
            "nativeGetSoundFontPresetCount",
            "nativeGetSoundFontPresetName",
            "nativeGetSoundFontPresetKeyRange",
            "nativeGetSoundFontPresetBankProgram",
        )

        @JvmStatic
        @AfterClass
        fun tally() = JniCoverage.requireCoverage(OWNER, COVERED)
    }

    private fun <T> jni(name: String, call: (AudioNativeBridge) -> T): T =
        JniHarness.exercise(OWNER, name, call)

    private fun cargarBytes(): Boolean =
        jni("nativeLoadSoundFont") { it.loadSoundFont(MinimalSoundFont.bytes()) }

    private fun estaCargado(): Boolean =
        jni("nativeIsSoundFontLoaded") { it.isSoundFontLoaded() }

    private fun cantidad(): Int =
        jni("nativeGetSoundFontPresetCount") { it.getSoundFontPresetCount() }

    private fun descargar() = jni("nativeUnloadSoundFont") { it.unloadSoundFont() }

    /**
     * Motor arriba y **sin SoundFont**. No es higiene: cada clase del arnés corre en su
     * propia JVM, pero los `@Test` de ESTA comparten el motor —que es un singleton de
     * proceso—, así que sin esto lo que un test afirma dependería de qué test corrió antes.
     */
    @Before
    fun engineUpAndSoundFontUnloaded() {
        assertTrue(jni("nativeStartTuner") { it.startTunerSync() }, "el motor no arrancó")
        descargar()
        assertFalse(estaCargado(), "quedó un SoundFont de otro test: las afirmaciones de abajo medirían sobre él")
    }

    /**
     * AC-024.1 — el ciclo de vida completo por el camino del `jbyteArray`.
     *
     * Los **tres** estados importan: sin cargar, cargado y descargado. Sin el primero,
     * "devuelve true" no se distingue de "devuelve true siempre"; sin el tercero,
     * `unload` sería cobertura write-only.
     */
    @Test
    fun `el ciclo de vida por bytes cruza en los tres estados`() {
        assertEquals(0, cantidad(), "sin SoundFont cargado no puede haber presets")

        assertTrue(cargarBytes(), "el fixture no cargó: revisá MinimalSoundFont contra tsf_load")
        assertTrue(estaCargado(), "cargó pero isSoundFontLoaded dice que no")
        assertEquals(
            MinimalSoundFont.PRESETS.size,
            cantidad(),
            "el conteo de presets no coincide con el fixture",
        )

        descargar()
        assertFalse(estaCargado(), "se descargó y isSoundFontLoaded sigue diciendo que sí")
        assertEquals(0, cantidad(), "descargado, el conteo tiene que volver a cero")
    }

    /**
     * AC-024.2 y AC-024.3 — la metadata de **los dos** presets.
     *
     * Recorrer los dos es el punto entero de este archivo: un getter cableado al preset 0
     * pasa el primer assert y muere en el segundo. Y ningún valor esperado —36, 84, 55,
     * 103, 3, 5, 7, 41— es potencia de dos, así que tampoco puede acertar por redondez.
     */
    @Test
    fun `el nombre, el banco, el programa y el rango cruzan para los dos presets`() {
        assertTrue(cargarBytes(), "el fixture no cargó")

        MinimalSoundFont.PRESETS.forEachIndexed { i, esperado ->
            assertEquals(
                esperado.nombre,
                jni("nativeGetSoundFontPresetName") { it.getSoundFontPresetName(i) },
                "el nombre del preset $i no sobrevivió el NewStringUTF",
            )
            assertContentEquals(
                intArrayOf(esperado.banco, esperado.programa),
                jni("nativeGetSoundFontPresetBankProgram") { it.getSoundFontPresetBankProgram(i) },
                "banco/programa del preset $i: el jintArray de salida no trajo lo del fixture",
            )
            assertContentEquals(
                intArrayOf(esperado.teclaMin, esperado.teclaMax),
                jni("nativeGetSoundFontPresetKeyRange") { it.getSoundFontPresetKeyRange(i) },
                "rango de teclas del preset $i. Ojo: NO sale del SF2 — lo infiere " +
                    "SoundFontManager::inferKeyRange del NOMBRE '${esperado.nombre}'",
            )
        }
    }

    /**
     * AC-024.5 — los negativos devuelven `null`, no un array basura.
     *
     * El gemelo obligatorio del test de arriba: sin esto, un getter que devolviera siempre
     * el preset 0 fallaría el anterior pero un getter que devolviera **cualquier** array
     * pasaría éste. Se piden los dos sentidos: índice fuera de rango con SoundFont
     * cargado, y con SoundFont **descargado**.
     */
    @Test
    fun `un indice fuera de rango y un SoundFont ausente devuelven null`() {
        assertNull(
            jni("nativeGetSoundFontPresetName") { it.getSoundFontPresetName(0) },
            "sin SoundFont cargado el nombre tiene que ser null",
        )
        assertNull(
            jni("nativeGetSoundFontPresetKeyRange") { it.getSoundFontPresetKeyRange(0) },
            "sin SoundFont cargado el rango tiene que ser null, no el 21..108 por defecto",
        )
        assertNull(
            jni("nativeGetSoundFontPresetBankProgram") { it.getSoundFontPresetBankProgram(0) },
            "sin SoundFont cargado banco/programa tiene que ser null, no -1/-1",
        )

        assertTrue(cargarBytes(), "el fixture no cargó")
        val fuera = MinimalSoundFont.PRESETS.size
        assertNull(
            jni("nativeGetSoundFontPresetName") { it.getSoundFontPresetName(fuera) },
            "el índice $fuera está fuera de rango",
        )
        assertNull(
            jni("nativeGetSoundFontPresetKeyRange") { it.getSoundFontPresetKeyRange(fuera) },
            "el índice $fuera está fuera de rango",
        )
        assertNull(
            jni("nativeGetSoundFontPresetBankProgram") { it.getSoundFontPresetBankProgram(fuera) },
            "el índice $fuera está fuera de rango",
        )
    }

    /**
     * AC-024.4 — los **otros dos** caminos de carga llegan al mismo estado observable.
     *
     * ## El `fd`, que es lo más valioso de este archivo
     *
     * `nativeLoadSoundFontFromFd(fd: Int, offset: Long, length: Long)` es el **primer
     * cruce de parámetro `jlong`** que ejerce el arnés: hay 16 `external fun` con `Long`
     * en la firma y la única cubierta hasta hoy era un `Long` de **retorno**. Un `Int`
     * declarado donde el C++ espera `jlong` compila de los dos lados, linkea, **pasa
     * `check-jni-symbols.py`** —que compara sólo NOMBRES— y corrompe memoria en el device.
     *
     * 🔴 **El offset es 1234 y NO está alineado a página, a propósito.** Es el caso real
     * —un `.sf2` embebido como asset dentro de un APK— y el que destapó que un test de
     * aritmética pura sobre `computeSoundFontMmapRegion` **no** cubre el uso de lo que
     * calcula: perder el `dataDelta` al usar la región no lo detectaba. Con offset 0 este
     * test pasaría con el delta roto.
     *
     * Y los dos `jlong` llevan valores **distintos** (1234 y el largo real): si el
     * marshalling los corriera, el `offset` tomaría el largo y la carga fallaría.
     */
    @Test
    fun `cargar por path y por fd con offset no alineado llega al mismo estado`() {
        val sf2 = MinimalSoundFont.bytes()

        // --- por path (jstring de entrada) ---
        val sueltoTmp = File.createTempFile("watermelon-sf2-", ".sf2")
        sueltoTmp.deleteOnExit()
        sueltoTmp.writeBytes(sf2)

        assertTrue(
            jni("nativeLoadSoundFontFromPath") { it.loadSoundFontFromPath(sueltoTmp.absolutePath) },
            "no cargó desde '${'$'}{sueltoTmp.absolutePath}'",
        )
        assertTrue(estaCargado(), "cargó por path y isSoundFontLoaded dice que no")
        assertEquals(MinimalSoundFont.PRESETS.size, cantidad(), "por path el conteo tiene que ser el mismo")
        assertEquals(
            MinimalSoundFont.PRESETS[1].nombre,
            jni("nativeGetSoundFontPresetName") { it.getSoundFontPresetName(1) },
            "por path el SEGUNDO preset tiene que estar igual que por bytes",
        )

        descargar()
        assertFalse(estaCargado(), "no se descargó antes del tramo del fd")

        // --- por fd, con la región corrida dentro de un archivo más grande ---
        val relleno = 1234
        val empotradoTmp = File.createTempFile("watermelon-sf2-empotrado-", ".bin")
        empotradoTmp.deleteOnExit()
        empotradoTmp.writeBytes(ByteArray(relleno) { 0x5A } + sf2 + ByteArray(77) { 0x5A })

        RandomAccessFile(empotradoTmp, "r").use { raf ->
            val fd = fdEntero(raf.fd)
            assertTrue(
                jni("nativeLoadSoundFontFromFd") {
                    it.loadSoundFontFromFd(fd, relleno.toLong(), sf2.size.toLong())
                },
                "no cargó desde fd=${'$'}fd offset=${'$'}relleno largo=${'$'}{sf2.size}",
            )
        }

        assertTrue(estaCargado(), "cargó por fd y isSoundFontLoaded dice que no")
        assertEquals(MinimalSoundFont.PRESETS.size, cantidad(), "por fd el conteo tiene que ser el mismo")
        assertEquals(
            MinimalSoundFont.PRESETS[1].nombre,
            jni("nativeGetSoundFontPresetName") { it.getSoundFontPresetName(1) },
            "por fd el SEGUNDO preset tiene que estar igual que por bytes. Si esto falla con " +
                "la carga en true, sospechá del dataDelta de la región mmapeada: el offset " +
                "${'$'}relleno no está alineado a página",
        )
    }

    /**
     * El descriptor crudo que la JVM no expone.
     *
     * Necesita `--add-opens=java.base/java.io=ALL-UNNAMED`, que la task de test declara
     * con su razón. **Falla ruidoso** si un JDK futuro renombra el campo: saltear el caso
     * en silencio dejaría el único cruce de `jlong` del arnés sin ejercer, con el conteo
     * igual de verde.
     */
    private fun fdEntero(fd: FileDescriptor): Int = try {
        FileDescriptor::class.java.getDeclaredField("fd").let { campo ->
            campo.isAccessible = true
            campo.getInt(fd)
        }
    } catch (e: ReflectiveOperationException) {
        throw AssertionError(
            "no pude sacar el descriptor crudo de FileDescriptor: ${'$'}e. Sin él, " +
                "nativeLoadSoundFontFromFd —el único cruce de parámetro jlong del arnés— " +
                "queda sin ejercer. Revisá que la task de test siga pasando " +
                "--add-opens=java.base/java.io=ALL-UNNAMED.",
            e,
        )
    } catch (e: RuntimeException) {
        throw AssertionError(
            "el módulo java.base no abrió java.io: ${'$'}e. La task de test tiene que pasar " +
                "--add-opens=java.base/java.io=ALL-UNNAMED.",
            e,
        )
    }
}
