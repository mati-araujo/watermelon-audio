# googletest — EL pin unico de la suite de host (REQ-047 S4).
#
# No esta vendorizado: FetchContent lo baja al configurar. Hasta REQ-047 S4 la
# version estaba escrita en NUEVE CMakeLists.txt (el raiz de tests/ y cada suite
# hija, que lo re-declaraba para poder configurarse suelta), y subirla era editar
# nueve lugares y confiar en no olvidarse uno. Ahora todos incluyen este archivo, y
# `scripts/check-dep-pins.py` falla si un `FetchContent_Declare(googletest` o un
# `GIT_TAG` de googletest aparece en cualquier otro lado.
#
#   version  1.18.0
#   commit   063de7e9578f82b369302001269680b4b1553359  (el tag v1.18.0, liviano:
#            apunta al commit, medido con `gh api repos/google/googletest/git/ref/tags/v1.18.0`)
#
# Un bump de googletest se verifica igual que el de REQ-047 S4 (AC-047.12): el
# conjunto de NOMBRES de `ctest -N` antes y despues tiene que ser identico. Un test
# que deja de listarse es un verde con menos tests, y nada mas lo ve.
#
# Dependabot NO ve esta dependencia: la cubre el inventario de
# thirdparty/VENDORED.md.
#
# Usage: include() este archivo. Deja los targets GTest::gtest / GTest::gtest_main
# (y gtest / gtest_main). `include_guard(GLOBAL)`: el raiz de tests/ lo incluye
# primero y las suites hijas, al entrar por add_subdirectory, no lo repiten.

include_guard(GLOBAL)

set(WMA_GOOGLETEST_VERSION "1.18.0")

include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v${WMA_GOOGLETEST_VERSION}
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
