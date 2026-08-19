cmake_minimum_required(VERSION 3.22.1)
project(watermelon-analysis-tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

if(NOT TARGET watermelon-dsp)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../dsp ${CMAKE_BINARY_DIR}/dsp)
endif()

# El ring se compila DENTRO del ejecutable de test, no se linkea desde
# `watermelon-analysis`: la compuerta de `WMA_TEST_HOOKS` tiene que estar
# activa, y activarla en la libreria la metería en el artefacto que se publica.
add_executable(analysis_tests
    test_analysis_ring.cpp
    test_analysis_thread.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../AnalysisRing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../AnalysisThread.cpp
)

target_compile_definitions(analysis_tests PRIVATE WMA_TEST_HOOKS=1)
target_include_directories(analysis_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
target_link_libraries(analysis_tests PRIVATE GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(analysis_tests)
