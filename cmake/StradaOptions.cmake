# Build options and build configurations for Strada.

option(STRADA_BUILD_TESTS "Build the Strada test suites" ON)
option(STRADA_WARNINGS_AS_ERRORS "Treat compiler warnings in Strada targets as errors" ON)
option(STRADA_ENABLE_SANITIZERS "Build Strada targets with AddressSanitizer (and UndefinedBehaviorSanitizer where supported)" OFF)
set(STRADA_DEPENDENCY_CACHE_DIR "" CACHE PATH
    "Optional directory where dependency archives are cached between build trees (speeds up fresh configures)")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use a preset, e.g. 'cmake --preset windows-debug'.")
endif()

# Build configurations: Debug (no optimization, asserts), Release (optimized, debug info, asserts) and
# Dist (optimized, no asserts or dev tooling; used for shipped games).
get_property(STRADA_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(STRADA_IS_MULTI_CONFIG)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Dist" CACHE STRING "Available build configurations" FORCE)
else()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build configuration" FORCE)
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release Dist)
    if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release|Dist)$")
        message(FATAL_ERROR "Unsupported CMAKE_BUILD_TYPE '${CMAKE_BUILD_TYPE}' (expected Debug, Release or Dist)")
    endif()
endif()

# Release keeps debug information for profiling and crash analysis; Dist uses the same optimization flags.
if(MSVC)
    set(STRADA_RELEASE_COMPILE_FLAGS "/O2 /Ob2 /DNDEBUG")
    set(STRADA_RELEASE_LINK_FLAGS "/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF")
else()
    set(STRADA_RELEASE_COMPILE_FLAGS "-O2 -g -DNDEBUG")
    set(STRADA_RELEASE_LINK_FLAGS "")
endif()

foreach(config RELEASE DIST)
    foreach(language C CXX)
        set(CMAKE_${language}_FLAGS_${config} "${STRADA_RELEASE_COMPILE_FLAGS}" CACHE STRING "" FORCE)
    endforeach()
    foreach(type EXE SHARED MODULE)
        set(CMAKE_${type}_LINKER_FLAGS_${config} "${STRADA_RELEASE_LINK_FLAGS}" CACHE STRING "" FORCE)
    endforeach()
    set(CMAKE_STATIC_LINKER_FLAGS_${config} "" CACHE STRING "" FORCE)
endforeach()

# Debug information embedded in object files (/Z7): no PDB contention in parallel builds and compiler-cache friendly.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")
# Dynamic CRT everywhere (/MD, /MDd) so every library agrees.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# All executables and shared libraries land in build/<preset>/bin (per configuration for multi-config generators).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
