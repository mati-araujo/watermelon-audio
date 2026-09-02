package com.watermellonstudios.audio.internal.bridge

import java.io.ByteArrayOutputStream

/**
 * El `.sf2` más chico que `tsf_load` acepta, **con DOS presets**, generado en memoria.
 *
 * ## Por qué se ESCRIBE acá y no se porta el de C++
 *
 * `core/tests/support/MinimalSoundFont.h` genera **un** preset, y con uno solo un getter
 * cableado al primero **sobrevive a todos los asserts** — el mismo mutante que REQ-022 y
 * REQ-023 midieron por separado. Así que esto no es un port: es un fixture para otro
 * propósito, con dos presets de **nombre, banco y programa distintos**, ninguno potencia
 * de dos.
 *
 * Si driftea respecto de lo que `tsf_load` exige, el modo de falla es **ruidoso** — no
 * carga, `isSoundFontLoaded` da `false` — nunca un verde falso.
 *
 * ## Lo que el loader realmente exige (leído de `tsf_load`, no del spec SF2)
 *
 * Los requisitos salen de la lección que dejó el fixture de C++, y difieren del spec en
 * los dos sentidos:
 *
 * - `RIFF` … `sfbk`.
 * - Un `LIST pdta` con **los nueve** chunks de la hydra (`phdr pbag pmod pgen inst ibag
 *   imod igen shdr`): falta uno y `tsf_load` aborta. Cada uno además tiene que medir
 *   múltiplo exacto de su registro — **38/4/10/4/22/4/10/4/46** — o el parser lo saltea
 *   como desconocido y termina igual de nulo.
 * - Un `LIST sdta` con un `smpl` de al menos un `short`.
 * - **`ifil` NO hace falta**: `tsf_load` lo saltea.
 *
 * Cada lista termina en un registro **terminal** (EOP/EOI/EOS): tsf recorre `num - 1`
 * entradas y usa la última como centinela de índices.
 *
 * 🔴 **Los 46 sample points en cero del final no son ceremonia.** El spec (§6.1) los
 * exige después de cada sample, y el render de tsf interpola leyendo `pos + 1`. Sin
 * ellos, ASan agarró un `heap-buffer-overflow` en `tsf_voice_render` contra el fixture
 * de C++ — que estuvo semanas así porque ningún test renderizaba desde él.
 *
 * ## Sobre el rango de teclas
 *
 * 🔴 **Se declara acá, y hasta MINI-017 no se podía.** El fixture escribe un generador
 * `keyRange` (genOper 43) en la zona de CADA preset, y el motor lo lee de las regiones.
 *
 * Antes, `SoundFontManager` lo **adivinaba del NOMBRE** con `strstr`, así que este
 * fixture declaraba como esperado justo lo que la heurística iba a devolver
 * (`"Cello Uno"` → 36..84) y **medía la heurística contra sí misma**: un test que no
 * podía fallar por el defecto que tenía delante. Ahora los rangos declarados
 * **contradicen** a propósito lo que aquella cadena habría dado para estos nombres, así
 * que si el nombre volviera a decidir, el arnés se pone rojo.
 */
internal object MinimalSoundFont {

    /** Generadores SF2 usados acá (`tsf.h`, enum de genOper). */
    private const val GEN_KEY_RANGE = 43
    private const val GEN_INSTRUMENT = 41
    private const val GEN_SAMPLE_ID = 53

    /**
     * Los dos presets del fixture, y **todos** sus valores esperados.
     *
     * 🔴 Los rangos **contradicen** lo que la vieja heurística por nombre habría dado:
     * `"cello"` daba 36..84 y acá el archivo declara 41..79; `"violin"` daba 55..103 y
     * acá declara 47..91. Elegidos así a propósito — si el nombre volviera a decidir,
     * estos asserts se caen. Ninguno de los cuatro límites, ni los bancos, ni los
     * programas, es potencia de dos: un getter cableado a una constante redonda no puede
     * acertar por casualidad.
     *
     * @property nombre lo que tiene que devolver `getSoundFontPresetName` (retorno `jstring`).
     * @property banco / [programa] lo que tiene que devolver `getSoundFontPresetBankProgram`.
     * @property teclaMin / [teclaMax] el `keyRange` que este fixture ESCRIBE en el archivo.
     */
    internal data class Preset(
        val nombre: String,
        val banco: Int,
        val programa: Int,
        val teclaMin: Int,
        val teclaMax: Int,
    )

    val PRESETS = listOf(
        Preset(nombre = "Cello Uno", banco = 3, programa = 7, teclaMin = 41, teclaMax = 79),
        Preset(nombre = "Violin Dos", banco = 5, programa = 41, teclaMin = 47, teclaMax = 91),
    )

    /** `keyRange` + `instrument` en cada zona de preset. Lo usa el índice de `pbag`. */
    private const val GENS_POR_ZONA_DE_PRESET = 2

    private const val SAMPLE_COUNT = 64
    private const val AMPLITUDE = 16384
    private const val TRAILING_ZERO_POINTS = 46

    /** El `.sf2` completo. Ronda el medio kilobyte: se regenera por llamada sin costo. */
    fun bytes(sampleRateInHeader: Int = 22050): ByteArray {
        // ---- sdta: onda cuadrada, NO silencio. Mientras el fixture de C++ fue todo
        // ceros, cualquier test de render medía "salió silencio" tanto si el motor
        // andaba como si no.
        val smpl = ByteArrayOutputStream()
        for (i in 0 until SAMPLE_COUNT) {
            smpl.put16(if (i % 16 < 8) AMPLITUDE else -AMPLITUDE)
        }
        repeat(TRAILING_ZERO_POINTS) { smpl.put16(0) }

        val sdta = ByteArrayOutputStream()
        sdta.putFourCC("sdta")
        sdta.putChunk("smpl", smpl.toByteArray())

        // ---- phdr: 38 bytes por registro. Dos presets reales + el terminal.
        val phdr = ByteArrayOutputStream()
        PRESETS.forEachIndexed { i, p ->
            phdr.putName20(p.nombre)
            phdr.put16(p.programa)   // preset (= programa GM)
            phdr.put16(p.banco)      // bank
            phdr.put16(i)            // presetBagNdx -> pbag[i]
            phdr.put32(0); phdr.put32(0); phdr.put32(0)  // library / genre / morphology
        }
        phdr.putName20("EOP")
        phdr.put16(0); phdr.put16(0)
        phdr.put16(PRESETS.size)     // el terminal cierra las zonas
        phdr.put32(0); phdr.put32(0); phdr.put32(0)

        // ---- pbag: una zona por preset, cada una con DOS generadores (`keyRange`
        // + `instrument`). El genNdx de la zona i es i*2, y el terminal cierra en
        // el total: un terminal que no cuente los generadores reales deja el
        // `instrument` FUERA de su zona y el preset se queda sin instrumento.
        val pbag = ByteArrayOutputStream()
        PRESETS.indices.forEach { i -> pbag.put16(i * GENS_POR_ZONA_DE_PRESET); pbag.put16(0) }
        pbag.put16(PRESETS.size * GENS_POR_ZONA_DE_PRESET); pbag.put16(0)  // terminal

        val pmod = ByteArrayOutputStream()
        repeat(5) { pmod.put16(0) }  // sólo el terminal (10 bytes)

        // ---- pgen: los dos presets apuntan al MISMO instrumento, y cada uno declara
        // su PROPIO `keyRange`. Eso es lo que permite dos rangos distintos con un solo
        // instrumento — tsf intersecta el rango de la zona de preset con el de la de
        // instrumento.
        //
        // El orden lo fija el spec (§7.5): `keyRange` PRIMERO de la zona, `instrument`
        // ÚLTIMO. El amount del rango son dos bytes: lo en el bajo, hi en el alto.
        val pgen = ByteArrayOutputStream()
        PRESETS.forEach { p ->
            pgen.put16(GEN_KEY_RANGE); pgen.put16((p.teclaMax shl 8) or p.teclaMin)
            pgen.put16(GEN_INSTRUMENT); pgen.put16(0)
        }
        pgen.put16(0); pgen.put16(0)  // terminal

        val inst = ByteArrayOutputStream()
        inst.putName20("Test Instrument")
        inst.put16(0)                 // instBagNdx -> ibag[0]
        inst.putName20("EOI")
        inst.put16(1)                 // terminal

        val ibag = ByteArrayOutputStream()
        ibag.put16(0); ibag.put16(0)
        ibag.put16(1); ibag.put16(0)  // terminal: UN generador en la zona 0

        val imod = ByteArrayOutputStream()
        repeat(5) { imod.put16(0) }   // terminal

        val igen = ByteArrayOutputStream()
        igen.put16(GEN_SAMPLE_ID); igen.put16(0)  // -> sample 0
        igen.put16(0); igen.put16(0)              // terminal

        val shdr = ByteArrayOutputStream()
        shdr.putName20("Test Sample")
        shdr.put32(0)                    // start
        shdr.put32(SAMPLE_COUNT - 1)     // end
        shdr.put32(1)                    // startLoop
        shdr.put32(SAMPLE_COUNT - 2)     // endLoop
        shdr.put32(sampleRateInHeader)
        shdr.write(60)                   // originalPitch = C4
        shdr.write(0)                    // pitchCorrection
        shdr.put16(0)                    // sampleLink
        shdr.put16(1)                    // sampleType = monoSample
        shdr.putName20("EOS")            // terminal
        repeat(5) { shdr.put32(0) }
        shdr.write(0); shdr.write(0)
        shdr.put16(0); shdr.put16(0)

        val pdta = ByteArrayOutputStream()
        pdta.putFourCC("pdta")
        pdta.putChunk("phdr", phdr.toByteArray())
        pdta.putChunk("pbag", pbag.toByteArray())
        pdta.putChunk("pmod", pmod.toByteArray())
        pdta.putChunk("pgen", pgen.toByteArray())
        pdta.putChunk("inst", inst.toByteArray())
        pdta.putChunk("ibag", ibag.toByteArray())
        pdta.putChunk("imod", imod.toByteArray())
        pdta.putChunk("igen", igen.toByteArray())
        pdta.putChunk("shdr", shdr.toByteArray())

        // ---- INFO. tsf lo saltea; va para que el archivo sea un .sf2 de verdad y no
        // el único del mundo que sólo este repo acepta.
        val info = ByteArrayOutputStream()
        info.putFourCC("INFO")
        val ifil = ByteArrayOutputStream()
        ifil.put16(2); ifil.put16(1)  // SoundFont 2.01
        info.putChunk("ifil", ifil.toByteArray())
        info.putChunk("isng", "EMU8000".toByteArray(Charsets.US_ASCII) + 0)
        info.putChunk("INAM", "watermelon-test".toByteArray(Charsets.US_ASCII) + 0)

        val body = ByteArrayOutputStream()
        body.putFourCC("sfbk")
        body.putChunk("LIST", info.toByteArray())
        body.putChunk("LIST", sdta.toByteArray())
        body.putChunk("LIST", pdta.toByteArray())

        val out = ByteArrayOutputStream()
        out.putChunk("RIFF", body.toByteArray())
        return out.toByteArray()
    }

    // ---- Primitivas RIFF (little-endian) --------------------------------------

    private fun ByteArrayOutputStream.put16(v: Int) {
        write(v and 0xFF); write((v ushr 8) and 0xFF)
    }

    private fun ByteArrayOutputStream.put32(v: Int) {
        for (i in 0 until 4) write((v ushr (8 * i)) and 0xFF)
    }

    private fun ByteArrayOutputStream.putFourCC(cc: String) {
        require(cc.length == 4) { "un fourCC mide 4: '$cc'" }
        write(cc.toByteArray(Charsets.US_ASCII))
    }

    /** Campo de nombre de 20 bytes rellenado con ceros (`tsf_char20`). */
    private fun ByteArrayOutputStream.putName20(name: String) {
        val raw = name.toByteArray(Charsets.US_ASCII)
        require(raw.size <= 19) { "un nombre de preset entra en 19 bytes + NUL: '$name'" }
        write(raw)
        repeat(20 - raw.size) { write(0) }
    }

    /** `<id><u32 size><payload>`, con el byte de padding que RIFF exige si es impar. */
    private fun ByteArrayOutputStream.putChunk(id: String, payload: ByteArray) {
        putFourCC(id)
        put32(payload.size)
        write(payload)
        if (payload.size % 2 != 0) write(0)
    }

    private operator fun ByteArray.plus(b: Int): ByteArray = this + byteArrayOf(b.toByte())
}
