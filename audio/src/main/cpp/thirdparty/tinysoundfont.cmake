# TinySoundFont - Header-only SoundFont2 synthesizer (MIT License)
# https://github.com/schellingb/TinySoundFont
#
# Usage: include() this file, then link tinysoundfont target.
# The implementation is compiled via tsf_impl.cpp (one TU only).

set(TSF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tinysoundfont)

add_library(tinysoundfont STATIC
    ${TSF_DIR}/tsf_impl.cpp
)

target_include_directories(tinysoundfont PUBLIC
    ${TSF_DIR}
)

# Suppress warnings in third-party code
target_compile_options(tinysoundfont PRIVATE -w)
