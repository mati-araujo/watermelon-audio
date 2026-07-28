package com.watermellonstudios.audio.api

/**
 * El motor de SoundFont de la sección 6 de `watermelon_audio.h`, como contrato propio.
 *
 * Misma partición que [IArpeggiatorBridge] e [IInputBridge], y por el mismo motivo:
 * cargar un `.sf2` y disparar notas es un dominio con consumidores propios, y un fake
 * de test no debería tener que implementar todo [IAudioNativeBridge] para cubrirlo.
 *
 * ## Por qué existe ahora
 *
 * Las 13 vivían **sólo** en el `AudioNativeBridge` de Android. La C API las tiene
 * (`wma_sf_*`) y cinterop ya generaba sus bindings; lo que faltaba era el nombre en
 * Kotlin común. El consumidor es NoisyPad: su `AudioEngineStateManager` las llama y
 * no puede bajar a `commonMain` sin esto.
 *
 * ## LOS GUARDS SON CONTRATO, NO PROGRAMACIÓN DEFENSIVA
 *
 * Tres de estos métodos rechazan entradas inválidas devolviendo `false` o no haciendo
 * nada, y hasta ahora eso vivía **sólo dentro de la implementación de Android**. Está
 * acá arriba a propósito: son dos implementaciones separadas del mismo contrato, y
 * cuando el comportamiento de borde vive en una sola, la otra diverge en silencio.
 *
 * Además, en iOS **uno de ellos no es opcional**: `addressOf(0)` sobre un array vacío
 * tira `ArrayIndexOutOfBoundsException` —medido, mutando el guard afuera— así que el
 * de [loadSoundFont] es lo que separa un `false` de una excepción. En Android el mismo
 * caso viaja por JNI y vuelve como el `false` que da el guard del lado C.
 *
 * ## Qué NO está acá
 *
 * `loadSoundFontFromFd(fd, offset, length)`, que existe en Android y en la C API
 * (`wma_sf_load_fd`). Queda afuera por la regla de opt-in: su razón de ser es el
 * `AssetFileDescriptor` de un asset pack de Play, o sea un consumidor exclusivamente
 * Android. Si algún día iOS necesita mapear una región de un fd, sube con su caller.
 */
interface ISoundFontBridge {

    // ==================== CARGA ====================

    /**
     * Carga un SoundFont desde los bytes crudos del `.sf2`.
     *
     * **No es RT-safe**: parsea el archivo entero.
     *
     * Para archivos grandes conviene [loadSoundFontFromPath], que mapea el archivo y
     * se ahorra tener los megas en memoria administrada.
     *
     * @return `false` si [data] está vacío — ver la nota sobre los guards en el KDoc
     *   de la interfaz: en iOS este caso, sin el guard, es una excepción.
     */
    fun loadSoundFont(data: ByteArray): Boolean

    /**
     * Carga un SoundFont mapeando el archivo (zero-copy). **No es RT-safe.**
     *
     * @return `false` si [path] está en blanco, o si el archivo no se pudo mapear.
     */
    fun loadSoundFontFromPath(path: String): Boolean

    /** Descarga el SoundFont actual. **No es RT-safe.** */
    fun unloadSoundFont()

    /** Si hay un SoundFont cargado. */
    fun isSoundFontLoaded(): Boolean

    // ==================== PRESETS ====================

    /**
     * Elige el preset activo.
     *
     * **No hace nada si [presetIndex] es negativo** — guard de contrato, ver el KDoc
     * de la interfaz.
     */
    fun setSoundFontPreset(presetIndex: Int)

    /** Cuántos presets tiene el SoundFont cargado. 0 si no hay ninguno. */
    fun getSoundFontPresetCount(): Int

    /**
     * El nombre del preset, o `null` si el índice no existe.
     *
     * La C API devuelve un puntero a memoria **interna, válida hasta que se descargue
     * el SoundFont**, así que la implementación copia a `String` antes de devolver. Un
     * `String` de Kotlin acá no es una comodidad: es lo que evita que el llamador se
     * quede con un puntero colgando después de [unloadSoundFont].
     */
    fun getSoundFontPresetName(presetIndex: Int): String?

    /**
     * `[minKey, maxKey]` en notas MIDI, o `null` si el preset no existe.
     *
     * Sirve para no dibujar teclas que el preset no va a sonar.
     */
    fun getSoundFontPresetKeyRange(presetIndex: Int): IntArray?

    /**
     * `[bank, program]` MIDI del preset, o `null` si no existe. Bank 128 es el kit de
     * percusión GM.
     *
     * **Es la forma estable de referirse a un preset desde afuera del archivo**: una
     * escena guardada persiste estos dos números y no el índice, que se corre en
     * cuanto se carga otro SoundFont.
     */
    fun getSoundFontPresetBankProgram(presetIndex: Int): IntArray?

    // ==================== POLIFONÍA ====================
    //
    // Los cuatro son `RT-safe` y lock-free: se llaman al ritmo del toque.

    /** Arranca o actualiza la nota de un punto de contacto. `RT-safe`. */
    fun sfNoteOn(touchId: Int, midiNote: Int, velocity: Float)

    /** Suelta la nota de un punto de contacto. `RT-safe`. */
    fun sfNoteOff(touchId: Int)

    /** Suelta todas las notas. `RT-safe`. */
    fun sfNoteOffAll()

    /**
     * Suelta todas las notas **menos** la de [keepTouchId]. `RT-safe`.
     *
     * Es una sola llamada y no un bucle de [sfNoteOff] sobre los demás slots: el
     * barrido del estado de toques corre en el thread de audio. Existe para el arrastre
     * de un solo dedo sobre el XY pad, donde el bucle se pagaba por frame.
     */
    fun sfNoteOffAllExcept(keepTouchId: Int)
}
