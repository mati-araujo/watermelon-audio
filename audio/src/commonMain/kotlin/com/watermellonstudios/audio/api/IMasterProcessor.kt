package com.watermellonstudios.audio.api

/**
 * El master sin motor (REQ-055 S12, ADR-0013 §2): el MISMO código que corre el perfil
 * [OutputProfile.MASTER], para pasar una premezcla ya renderizada por el master (y medirla) sin
 * reproducirla. Un procesador por stream.
 *
 * Tras [reset], la misma entrada da la misma salida, con cualquier tamaño de bloque. La salida
 * llega [latencyFrames] frames después de la entrada: quien alinea contra otros archivos descarta
 * esos frames del principio y completa la cola con ceros.
 *
 * Una vez cerrado, toda operación falla con `EngineNotInitialized` sin cruzar al nativo.
 */
interface IMasterProcessor : AutoCloseable {
    /** Los frames que el master demora su salida (5 ms). */
    val latencyFrames: Int

    /** Se aplica en el próximo [process]; la ganancia rampea lineal en ese bloque. */
    fun configure(params: MasterParams): Result<Unit>

    /** Estado de uno recién creado. La ganancia salta a la configurada (sin rampa). */
    fun reset(): Result<Unit>

    /** En el lugar: [frames] frames estéreo intercalados de [buffer] (`frames * 2` floats). */
    fun process(buffer: FloatArray, frames: Int): Result<Unit>
}
