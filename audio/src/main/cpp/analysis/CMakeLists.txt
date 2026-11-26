# ============================================================================
# watermelon-analysis: el camino de analisis del afinador (REQ-001).
#
# CERO dependencias fuera de `watermelon-dsp`. Hoy ni siquiera esa hace falta —
# el ring tiene almacenamiento propio, ver la nota de AnalysisRing.h sobre por
# que no envuelve a LockFreeRingBuffer — pero se declara igual para que el
# LIMITE quede escrito y una dependencia nueva sea visible en el diff.
#
# El motor NO sabe que existe un afinador: la dependencia va en una sola
# direccion, de `core/` hacia aca.
# ============================================================================

cmake_minimum_required(VERSION 3.22.1)

# Las primitivas de disco del volcado dependen del SO (REQ-061 AC-061.4): Win32 o POSIX.
if(WIN32)
    set(WMA_DUMP_FILE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/../platform/DumpFileWindows.cpp)
else()
    set(WMA_DUMP_FILE_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/../platform/DumpFilePosix.cpp)
endif()

add_library(watermelon-analysis STATIC
    AnalysisRing.cpp
    AnalysisThread.cpp
    TunerInputDump.cpp
    WavFileDumpSink.cpp
    ${WMA_DUMP_FILE_SOURCE}
    SpectralSupportProbe.cpp
    OfflineAnalysis.cpp
    PhaseSlopeEstimator.cpp
    StrobeTracker.cpp
    InharmonicityEstimator.cpp
    FastModeTracker.cpp
)

target_include_directories(watermelon-analysis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(watermelon-analysis PUBLIC cxx_std_20)
target_link_libraries(watermelon-analysis PUBLIC watermelon-dsp)
