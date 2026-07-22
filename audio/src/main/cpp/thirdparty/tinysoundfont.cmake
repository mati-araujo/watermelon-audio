# TinySoundFont - Header-only SoundFont2 synthesizer (MIT License)
# https://github.com/schellingb/TinySoundFont
#
# Usage: include() this file, then link tinysoundfont target.
# The implementation is compiled via tsf_impl.cpp (one TU only).

# CMAKE_CURRENT_LIST_DIR (this file's own directory), not
# CMAKE_CURRENT_SOURCE_DIR (the includer's): lets the iOS build under ios/
# include this without pretending to be cpp/. Same path when included from
# cpp/CMakeLists.txt, so the Android build is unaffected.
set(TSF_DIR ${CMAKE_CURRENT_LIST_DIR}/tinysoundfont)

add_library(tinysoundfont STATIC
    ${TSF_DIR}/tsf_impl.cpp
)

target_include_directories(tinysoundfont PUBLIC
    ${TSF_DIR}
)

# Suppress warnings in third-party code
target_compile_options(tinysoundfont PRIVATE -w)
