# Los conteos que envejecen (historia de REQ-021)

Movido del `CLAUDE.md` raiz por MINI-044 (2026-10-09). Es la historia de por que la tabla «Conteos medidos» la escribe `scripts/check-doc-counts.py` y no la mano. Los numeros de abajo son los de cada fecha, no los de hoy.

> Los conteos del mapa (hoy en `docs/dev/arquitectura.md`) son orientativos y **driftean**. Re-medidos el 2026-08-27 tras cerrar
> REQ-013 y REQ-017: commonMain 93→**94**, androidMain **21**, iosMain **6**,
> AudioNativeBridge 3332→**3357** LOC y 306→**308** funs, JNIEXPORT 295→**297**,
> C API 272→**274**, suite de host 1131→**1154** tests. Y la tabla de **Stack** también estaba
> vieja: AGP decía 9.2.1 contra **9.3.2** y Kotlin 2.4.0 contra **2.4.10** — o sea que el drift
> no es sólo de los conteos, alcanza a cualquier número escrito a mano en este archivo.
>
> (Tandas anteriores: 2026-08-25 al cerrar REQ-014 venía de commonMain 91, AudioNativeBridge
> 3304 LOC / 303 funs, JNIEXPORT 292, C API 269 y 1011 tests; 2026-08-20 al cerrar REQ-001 S10,
> de commonMain 83, AudioNativeBridge 3229 / 291, JNIEXPORT 280, C API 253 y 883 tests.) El
> afinador entero (REQ-001) agregó `cpp/analysis/` con 14 archivos.
>
> **SEXTA tanda, el 2026-08-28 al cerrar REQ-019**: la suite de host es de **1180** tests (venía de
> 1169), `analysis/` pasó de 14 a **17** archivos (nacieron `AbsenceGate.h` y su test), y el reparto
> del baseline de llamadores es **45 sonda / 29 deuda / 1 entrada / 1 callback-externo**. Los
> conteos de JNI y de la C API **no** se movieron: MINI-007 borró un setter de C++ sin superficie.
>
> **QUINTA tanda, el 2026-08-27 al cerrar REQ-016**: la suite de host era de **1169** tests, no
> 1154 — o sea que el número de arriba envejeció **el mismo día** en que se lo re-midió. Y los
> tests de Kotlin en la JVM eran **153**, no los 69 que decía la sección de comandos; con el
> arnés JNI son **183**. Las `JNIEXPORT` de `jni/*.cpp` son **310** en total (297 del bridge + 8
> de benchmark + 3 de usb + 2 `JNI_OnLoad`/`JNI_OnUnload`), de las cuales **308** son entradas
> `Java_*` que Kotlin declara.
>
> 🔴 **Que este bloque haya quedado stale CUATRO veces seguidas es el dato, no el accidente**, y
> la cuarta agregó un eje nuevo: las VERSIONES del stack, que nadie sospechaba. Un
> conteo escrito a mano envejece en silencio: nadie lo lee como "esto puede estar viejo", se lee
> como un hecho. Le pasó también a la spec viva del afinador —decía "snapshot de 14 valores"
> cuando ya eran 16— y a un KDoc que decía "ocho floats" con quince, contra el que un consumidor
> real diseñó tres pedidos. Re-medir es barato, asi que **medir antes de citar**:
>
> ```bash
> find audio/src/commonMain -name '*.kt' | wc -l          # archivos por source set
> wc -l audio/src/androidMain/kotlin/com/watermellonstudios/audio/internal/bridge/AudioNativeBridge.kt
> grep -c 'external fun' audio/src/androidMain/kotlin/com/watermellonstudios/audio/internal/bridge/AudioNativeBridge.kt
> grep -c JNIEXPORT audio/src/main/cpp/jni/jni_audio_bridge.cpp
> grep -c WMA_API audio/src/main/cpp/api/watermelon_audio.h
> grep -nE '^agp|^kotlin ' gradle/libs.versions.toml     # la tabla de Stack tambien driftea
> python3 scripts/c-api-gap.py                            # C API + delegacion (§4b)
> ```
>
> **REQ-016 construyó el primer pedazo de esa salida**, para su propia rebanada: el conteo de
> cobertura del arnés JNI sale MEDIDO en cada corrida — el numerador se anota al cruzar la
> frontera, el denominador se cuenta del árbol, y sacar un test **baja** el número (probado por su
> propio self-test). *Al cerrarse, ese REQ dejaba 13 de 310; el valor de hoy lo imprime Gradle, no
> este archivo.* No cubrió este bloque; mostró la forma, y REQ-021 la completó.
>
> 🔴 **Siete veces stale fue la evidencia de que "medir antes de citar" NO alcanzaba**: era una
> regla que sólo vivía en prosa, y este repo ya sabe cómo terminan (WD-1.1: el callback violaba
> sus reglas escritas en 65 lugares). **REQ-021 la convirtió en un gate**:
> `scripts/check-doc-counts.py` re-mide del árbol y **falla** si la tabla «Conteos medidos» del `CLAUDE.md` raiz no coincide —
> la misma forma que `rt-safety` y `mechanism-callers`.
