package com.watermellonstudios.audio.internal.bridge

import java.io.ByteArrayOutputStream
import java.io.File

/**
 * El `.wav` más chico que `wav::readWav` acepta, **generado en memoria** — y su lector,
 * que es lo que hace afirmable lo que el motor **escribe** (REQ-026 S1).
 *
 * ## Por qué se ESCRIBE acá y no se commitea un binario
 *
 * Misma disciplina que [MinimalSoundFont]: un `.wav` commiteado es un archivo que nadie
 * vuelve a mirar y que envejece en silencio contra el parser real. Generado, el modo de
 * falla es **ruidoso** — `readWav` devuelve 0 frames, `looperImportTrack` devuelve
 * `false`, y el test se cae nombrando la causa — nunca un verde falso.
 *
 * ## Por qué hay LECTOR, y no sólo escritor
 *
 * Sin lector, "exportó" sólo se podría afirmar como *"el archivo existe y no está
 * vacío"*, que es lo mismo que devuelve un motor que escribe basura. El racimo entero de
 * este REQ es IO: la mitad de lo que compra el arnés acá es que lo que salió por
 * `GetStringUTFChars` **volvió a entrar**.
 *
 * ## Lo que el lector real exige (leído de `wav::readWav`, no del spec RIFF)
 *
 * - `RIFF` … `WAVE`, y después una caminata de chunks — o sea que el orden no importa y
 *   los desconocidos se saltean por su tamaño.
 * - `fmt ` de al menos 16 bytes: formato, canales, rate, byteRate+blockAlign (que
 *   `readWav` **saltea**), bits por muestra.
 * - `data` con el payload.
 * - Formato **1 (PCM)** con 16 o 24 bits, o **3 (IEEE float)** con 32. Cualquier otra
 *   combinación es rechazo silencioso, con `numFrames == 0`.
 * - 1 o 2 canales; el mono se auto-estereoiza.
 *
 * 🔴 **Acá se escribe float32 a propósito**: es el único de los tres que hace un
 * round-trip **exacto**. Con PCM16 el `assertEquals` sobre las muestras tendría que
 * llevar una tolerancia, y una tolerancia es una decisión de umbral escondida adentro de
 * un test que no habla de umbrales.
 *
 * ## Sobre los valores
 *
 * Ninguna amplitud es potencia de dos (`0,625` y `0,375`, que son `5/8` y `3/8`: exactas
 * en float, y aun así no redondas). Una potencia de dos es exacta en float y esconde
 * defectos de conversión — la lección viene de `test_c_api_tuner.cpp` vía REQ-018.
 */
internal object MinimalWav {

    /** El rate del motor en los tests del arnés. No es potencia de dos. */
    const val RATE = 48_000

    /** Amplitud del canal izquierdo dentro de una región con contenido. `5/8`. */
    const val AMP_L = 0.625f

    /** Amplitud del canal derecho. `3/8` — distinta de la izquierda a propósito. */
    const val AMP_R = 0.375f

    /** Período de la onda cuadrada, en frames. Fija la energía dentro de una región. */
    private const val SQUARE_PERIOD = 16

    private const val HEADER_BYTES = 44

    /**
     * Un tramo con contenido dentro de un fixture que por lo demás es **silencio
     * exacto** (ceros, no ruido de piso).
     *
     * El silencio tiene que ser exacto porque `TrackBuffer::findContentBounds` compara
     * contra `max(pico * ratio, 1e-4)`: un piso de ruido por encima de ese umbral movería
     * los bordes y el test estaría afirmando el ruido, no los bordes.
     *
     * @property start primer frame **inclusive** con contenido.
     * @property endExclusive primer frame después del contenido.
     */
    internal data class Region(val start: Int, val endExclusive: Int)

    /**
     * Lo que [parse] pudo leer de vuelta de un `.wav`.
     *
     * @property frames frames por canal — `dataSize / blockAlign`.
     */
    internal data class Parsed(
        val frames: Int,
        val sampleRate: Int,
        val channels: Int,
        val bitsPerSample: Int,
        val audioFormat: Int,
    )

    /**
     * Un `.wav` float32 estéreo de [frames] frames, con onda cuadrada dentro de cada
     * región de [regions] y **ceros exactos** afuera.
     *
     * Las regiones se dan ordenadas y sin solaparse; no se valida, porque un fixture mal
     * armado se manifiesta en el assert del test que lo usa y no hace falta un segundo
     * lugar donde equivocarse.
     */
    fun floatStereo(
        frames: Int,
        regions: List<Region>,
        sampleRate: Int = RATE,
    ): ByteArray {
        val out = ByteArrayOutputStream(HEADER_BYTES + frames * 2 * 4)
        writeHeader(out, frames = frames, sampleRate = sampleRate)
        for (i in 0 until frames) {
            val region = regions.firstOrNull { i >= it.start && i < it.endExclusive }
            if (region == null) {
                out.putFloat(0.0f)
                out.putFloat(0.0f)
            } else {
                val alto = (i - region.start) % SQUARE_PERIOD < SQUARE_PERIOD / 2
                out.putFloat(if (alto) AMP_L else -AMP_L)
                out.putFloat(if (alto) AMP_R else -AMP_R)
            }
        }
        return out.toByteArray()
    }

    /** El fixture escrito a [file]. Devuelve la **ruta absoluta**, que es lo que cruza el JNI. */
    fun writeTo(file: File, frames: Int, regions: List<Region>, sampleRate: Int = RATE): String {
        file.writeBytes(floatStereo(frames, regions, sampleRate))
        return file.absolutePath
    }

    /**
     * Lee la cabecera de un `.wav` **caminando los chunks**, igual que `wav::readWav`.
     *
     * Caminar y no leer offsets fijos no es purismo: `wav::writeWav` pega un `LIST/INFO`
     * **después** del `data` cuando hay metadatos, que es justo el caso de
     * `exportMixV2`. Un lector de offsets fijos andaría en la mitad de los archivos que
     * este REQ tiene que verificar.
     *
     * @return `null` si no es un RIFF/WAVE, o si le falta `fmt ` o `data`. Nunca un
     *         [Parsed] a medias: un parseo incompleto que igual devuelve algo se lee como
     *         un archivo válido.
     */
    fun parse(bytes: ByteArray): Parsed? {
        if (bytes.size < 12) return null
        if (bytes.ascii(0, 4) != "RIFF" || bytes.ascii(8, 4) != "WAVE") return null

        var pos = 12
        var format = 0
        var channels = 0
        var rate = 0
        var bits = 0
        var dataSize = -1
        var vioFmt = false

        while (pos + 8 <= bytes.size) {
            val id = bytes.ascii(pos, 4)
            val size = bytes.u32(pos + 4)
            val body = pos + 8
            when {
                id == "fmt " && size >= 16 && body + 16 <= bytes.size -> {
                    format = bytes.u16(body)
                    channels = bytes.u16(body + 2)
                    rate = bytes.u32(body + 4)
                    // body + 8 (byteRate) y body + 12 (blockAlign) los saltea también readWav.
                    bits = bytes.u16(body + 14)
                    vioFmt = true
                }
                id == "data" -> dataSize = size
            }
            // Los chunks RIFF se alinean a 2 bytes. `writeWav` no genera ninguno impar,
            // pero un lector que no lo contemple se desincroniza contra cualquier archivo
            // que sí — y entonces el fallo aparecería como "no encontré data".
            pos = body + size + (size and 1)
        }
        if (!vioFmt || dataSize < 0 || channels <= 0 || bits <= 0) return null
        val blockAlign = channels * (bits / 8)
        if (blockAlign <= 0) return null
        return Parsed(
            frames = dataSize / blockAlign,
            sampleRate = rate,
            channels = channels,
            bitsPerSample = bits,
            audioFormat = format,
        )
    }

    /** [parse] sobre un archivo. `null` también si el archivo no existe o está vacío. */
    fun parse(file: File): Parsed? =
        if (!file.isFile || file.length() == 0L) null else parse(file.readBytes())

    /** La cabecera canónica de 44 bytes: `RIFF` + `fmt ` de 16 + `data`, float32 estéreo. */
    private fun writeHeader(out: ByteArrayOutputStream, frames: Int, sampleRate: Int) {
        val channels = 2
        val bits = 32
        val blockAlign = channels * bits / 8
        val dataSize = frames * blockAlign
        out.putAscii("RIFF")
        out.putU32(HEADER_BYTES - 8 + dataSize)
        out.putAscii("WAVE")
        out.putAscii("fmt ")
        out.putU32(16)
        out.putU16(3)                       // 3 = IEEE float
        out.putU16(channels)
        out.putU32(sampleRate)
        out.putU32(sampleRate * blockAlign) // byteRate
        out.putU16(blockAlign)
        out.putU16(bits)
        out.putAscii("data")
        out.putU32(dataSize)
    }

    // ---- little-endian, a mano: el fixture no puede depender de una librería ----

    private fun ByteArrayOutputStream.putAscii(s: String) = write(s.toByteArray(Charsets.US_ASCII))

    private fun ByteArrayOutputStream.putU16(v: Int) {
        write(v and 0xFF)
        write((v ushr 8) and 0xFF)
    }

    private fun ByteArrayOutputStream.putU32(v: Int) {
        putU16(v and 0xFFFF)
        putU16((v ushr 16) and 0xFFFF)
    }

    private fun ByteArrayOutputStream.putFloat(v: Float) = putU32(v.toRawBits())

    private fun ByteArray.ascii(at: Int, len: Int) = String(this, at, len, Charsets.US_ASCII)

    private fun ByteArray.u16(at: Int) = (this[at].toInt() and 0xFF) or ((this[at + 1].toInt() and 0xFF) shl 8)

    private fun ByteArray.u32(at: Int) = u16(at) or (u16(at + 2) shl 16)
}
