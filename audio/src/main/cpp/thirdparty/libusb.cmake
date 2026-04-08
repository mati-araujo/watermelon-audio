# libusb.cmake
#
# CMake configuration for libusb on Android.
#
# libusb is used for direct USB communication with audio devices,
# bypassing Android's audio HAL for lower latency.
#
# Usage: include(thirdparty/libusb.cmake)
#
# This provides: libusb::libusb target for linking

set(LIBUSB_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libusb)

# Verify libusb exists
if(NOT EXISTS "${LIBUSB_SOURCE_DIR}/libusb/libusb.h")
    message(FATAL_ERROR "libusb not found! Run: git submodule update --init --recursive")
endif()

message(STATUS "Configuring libusb from: ${LIBUSB_SOURCE_DIR}")

# ============================================================================
# Create libusb library target manually (libusb doesn't have good CMake support)
# ============================================================================

# Core source files for all platforms
set(LIBUSB_SOURCES
    ${LIBUSB_SOURCE_DIR}/libusb/core.c
    ${LIBUSB_SOURCE_DIR}/libusb/descriptor.c
    ${LIBUSB_SOURCE_DIR}/libusb/hotplug.c
    ${LIBUSB_SOURCE_DIR}/libusb/io.c
    ${LIBUSB_SOURCE_DIR}/libusb/strerror.c
    ${LIBUSB_SOURCE_DIR}/libusb/sync.c
)

# Android/Linux specific sources
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND LIBUSB_SOURCES
        ${LIBUSB_SOURCE_DIR}/libusb/os/events_posix.c
        ${LIBUSB_SOURCE_DIR}/libusb/os/threads_posix.c
        ${LIBUSB_SOURCE_DIR}/libusb/os/linux_usbfs.c
        ${LIBUSB_SOURCE_DIR}/libusb/os/linux_netlink.c
    )
endif()

# Create static library
add_library(libusb_static STATIC ${LIBUSB_SOURCES})

# Include directories
target_include_directories(libusb_static
    PUBLIC
        ${LIBUSB_SOURCE_DIR}/libusb
    PRIVATE
        ${LIBUSB_SOURCE_DIR}/android      # For config.h (Android-specific)
        ${LIBUSB_SOURCE_DIR}/libusb/os
)

# ============================================================================
# Platform-specific configuration
# ============================================================================

if(ANDROID)
    # Android-specific defines
    target_compile_definitions(libusb_static PRIVATE
        # Core config
        _GNU_SOURCE
        HAVE_SYS_TIME_H
        HAVE_SYS_TYPES_H
        HAVE_UNISTD_H
        HAVE_STDINT_H
        HAVE_STDLIB_H
        HAVE_STRING_H
        HAVE_MEMORY_H
        HAVE_STRINGS_H
        HAVE_INTTYPES_H
        HAVE_STRUCT_TIMESPEC
        HAVE_CLOCK_GETTIME

        # Threading
        THREADS_POSIX
        HAVE_PTHREAD_H

        # Platform
        OS_LINUX
        HAVE_LINUX_NETLINK_H
        HAVE_LINUX_FILTER_H

        # Disable features not available on Android
        # (no udev, no systemd, no dbus)

        # Logging disabled for now (causes compilation issues)
        # Can be re-enabled once libusb is fully integrated

        # Note: DEFAULT_VISIBILITY is defined in config.h, not needed here
    )

    # Link against Android log library
    target_link_libraries(libusb_static PRIVATE log)

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Desktop Linux config
    target_compile_definitions(libusb_static PRIVATE
        _GNU_SOURCE
        HAVE_SYS_TIME_H
        HAVE_SYS_TYPES_H
        HAVE_UNISTD_H
        HAVE_STDINT_H
        HAVE_STDLIB_H
        HAVE_STRING_H
        HAVE_MEMORY_H
        HAVE_STRINGS_H
        HAVE_INTTYPES_H
        HAVE_STRUCT_TIMESPEC
        HAVE_CLOCK_GETTIME
        THREADS_POSIX
        HAVE_PTHREAD_H
        OS_LINUX
        HAVE_LINUX_NETLINK_H
        HAVE_LINUX_FILTER_H
        DEFAULT_VISIBILITY=
    )

    find_package(Threads REQUIRED)
    target_link_libraries(libusb_static PRIVATE Threads::Threads)
endif()

# Compiler flags
target_compile_options(libusb_static PRIVATE
    -Wall
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-pointer-sign
    -fvisibility=hidden
)

# ============================================================================
# Create alias target for cleaner usage
# ============================================================================

add_library(libusb::libusb ALIAS libusb_static)

# Export version for consumers
set(LIBUSB_FOUND TRUE)
set(LIBUSB_VERSION "1.0.27")

message(STATUS "libusb configured successfully (from git submodule)")
