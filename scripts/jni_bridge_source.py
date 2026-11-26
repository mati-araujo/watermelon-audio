"""Dónde vive el lado Kotlin del cruce JNI: UNA constante para todos los gates.

`check-jni-symbols`, `check-jni-signatures` y `check-doc-counts` leen
`AudioNativeBridge.kt`. Hasta REQ-055 S1 cada uno tenía la ruta de androidMain
escrita a mano; al mudarse el bridge al source set compartido Android+JVM, tres
copias de la ruta eran tres lugares donde un gate podía quedar midiendo un
archivo que ya no es el bridge. El source set es ahora un parámetro, acá.

El FQN NO cambia con el source set: los símbolos `Java_..._AudioNativeBridge_*`
salen del paquete, no de la carpeta.
"""

# El source set que compila el bridge para Android y para el target JVM `desktop`.
KOTLIN_SOURCE_SET = "jvmSharedMain"

BRIDGE_KT_REL = (
    f"audio/src/{KOTLIN_SOURCE_SET}/kotlin/com/watermellonstudios/audio"
    "/internal/bridge/AudioNativeBridge.kt"
)
