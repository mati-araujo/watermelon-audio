# TinyMidiLoader — parser de MIDI minimalista, header-only (MIT)
# https://github.com/schellingb/TinySoundFont  (tml.h vive en el MISMO repo que tsf.h)
#
# Vendoreado por REQ-039 S1. Del mismo autor que TinySoundFont, un archivo, sin
# dependencias: es lo que deja tocar `sf_spec_test.mid` a traves del motor para
# medir si reproduce el font como fue programado.
#
# 🔴 EL PIN, porque el repo NO tiene tags (igual que el del SoundFont-Spec-Test y
# el de GeneralUser GS, y por la misma razon):
#
#   commit  853a0a171759f1ddba0de1442133a75912bbeffa   (2026-07-19)
#   archivo tml.h  —  TinyMidiLoader v0.7, 531 lineas
#   sha256  93257db259e0efb2ea2037d7157841bec8cb4a2d7986286e43c8090705326546
#
# A diferencia del material del spec-test —que se BAJA y no se versiona porque son
# datos—, esto es CODIGO que se compila adentro del test, asi que se versiona: un
# test que no compila sin red no es un test. Actualizarlo es un diff que se revisa.
#
# Usage: include() este archivo y linkear el target `tinymidiloader`.
# Es INTERFACE: `tml.h` es header-only y quien lo use define `TML_IMPLEMENTATION`
# en UNA sola TU, igual que `tsf.h`.

set(TML_DIR ${CMAKE_CURRENT_LIST_DIR}/tinymidiloader)

add_library(tinymidiloader INTERFACE)

# SYSTEM: codigo de terceros: sus warnings no son deuda nuestra y no pueden
# voltear un build con -Werror. Es la misma postura que `tinysoundfont.cmake`
# toma con su `-w`, pero por la via PORTABLE: `-w`/`-Wno-everything` son de un
# compilador y este target lo consumen los dos —Apple clang en el gate local y
# gcc/libstdc++ en los tres jobs de ubuntu, que nunca se saltean—. Un flag que
# solo entiende clang habria roto el CI y no el gate, que es el peor orden.
target_include_directories(tinymidiloader SYSTEM INTERFACE ${TML_DIR})
