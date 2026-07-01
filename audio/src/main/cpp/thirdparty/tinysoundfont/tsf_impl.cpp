/**
 * @file tsf_impl.cpp
 * @brief TinySoundFont implementation compilation unit
 *
 * This file includes the tsf.h header with TSF_IMPLEMENTATION defined,
 * creating the actual function implementations. Only include this in
 * ONE compilation unit (handled by CMakeLists.txt).
 *
 * stb_vorbis is included BEFORE tsf.h so TinySoundFont enables its SF3
 * code path: SoundFont3 (.sf3) stores samples as Ogg/Vorbis, and tsf gates
 * all Ogg decoding behind `#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H`. Without
 * this include, .sf3 fonts still load but their compressed samples are read
 * as raw PCM — producing loud garbage (as if the sample rate were wrong).
 * Decoding happens at load time (off the audio thread), so it is RT-safe.
 *
 * STB_VORBIS_NO_STDIO: tsf only uses the in-memory decoder
 * (stb_vorbis_open_memory), so the file-based API and its stdio dependency
 * are dropped. Third-party warnings for this whole TU are already silenced
 * via `-w` in tinysoundfont.cmake.
 */
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"

#define TSF_IMPLEMENTATION
#include "tsf.h"
