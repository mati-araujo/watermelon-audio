package com.watermellonstudios.audio.internal.bridge

import org.junit.AfterClass
import org.junit.Before
import java.io.File
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
 *
 * 🔴 **`nativeLoadSoundFontFromFd` NO está acá, y su ausencia es deliberada** — ver
 * *"Lo que este archivo NO cubre"* abajo. REQ-024 lo había incluido con dos
 * justificaciones que **resultaron falsas**, y MINI-015 lo sacó.
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
 * ## Lo que este archivo NO cubre, y por qué
 *
 * **`nativeLoadSoundFontFromFd(jint, jlong, jlong)` — hueco DECLARADO** (MINI-015). Estuvo acá
 * y se sacó, porque las dos razones que lo justificaban **no se sostuvieron al verificarlas**:
 *
 * 1. *"el primer cruce de parámetro `jlong` del arnés"* — **falso**:
 *    `nativeLooperSetTrackLoopRegion(jint, jlong, jlong)` ya estaba cubierta desde REQ-022. La
 *    versión corregida —*el primero cuyo **valor** se afirma*, porque aquel test afirma el
 *    no-op y los valores nunca vuelven— tampoco se paga: `wma_looper_arm_in_frames` devuelve
 *    `getPlayFrame() + offset`, así que ese cruce se consigue **sin reflexión**.
 * 2. *"el offset sin alinear no está cubierto de punta a punta"* — **falso**:
 *    `core/tests/test_soundfont_load.cpp` (`LoadFromFdAppliesTheRateItIsGiven`) arma el archivo
 *    con `prefixBytes=1234` —el mismo número— y hace un `mmap` real.
 *
 * Lo único que quedaba era **la firma**, que ningún gate ve (`check-jni-symbols.py` compara
 * sólo NOMBRES) — pero eso vale para **cada una** de las que faltan, y el precio era que el
 * arnés dependiera de un campo privado del JDK (`FileDescriptor.fd`) más un
 * `--add-opens=java.base/java.io` permanente, por UNA función. La causa general —firmas sin
 * verificar— es un REQ propio, no algo que se compre de a una.
 *
 * **`nativeSetSoundFontPreset`**: no tiene getter, así que sería cobertura write-only.
 *
 * **Las cinco de `sfNoteOn/Off/…`**: despachan a voces y no son observables sin render — el
 * mismo muro que descartó arp.
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
     * AC-024.4 — el camino del **path** llega al mismo estado observable que el de bytes.
     *
     * Es `GetStringUTFChars` del otro lado: una ruta que no sobrevive el cruce da un `false`
     * silencioso, y con `getSoundFontPresetName(1)` se afirma que llegó **el mismo** SoundFont,
     * no uno cualquiera.
     *
     * 🔴 **El camino del `fd` estaba acá y MINI-015 lo sacó** — ver *"Lo que este archivo NO
     * cubre"* en el KDoc de la clase.
     */
    @Test
    fun `cargar por path llega al mismo estado que por bytes`() {
        val sf2 = MinimalSoundFont.bytes()

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
    }
}
