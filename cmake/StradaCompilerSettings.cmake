# Compiler settings applied to Strada's own targets only (never to third-party targets).

# strada_configure_target(<target>)
# Applies the C++ standard, warning level, platform definitions and build-configuration macros.
function(strada_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)

    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # clang-cl: MSVC-compatible driver, Clang diagnostics. MSVC-only conformance switches are not accepted.
        target_compile_options(${target} PRIVATE
            /W4
            /utf-8
            /bigobj
            -Wshadow
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wimplicit-fallthrough)
        if(STRADA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    elseif(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /Zc:inline
            /utf-8
            /bigobj
            /external:W0
            /external:anglebrackets)
        if(STRADA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wcast-align
            -Wformat=2
            -Wimplicit-fallthrough
            -Wmisleading-indentation)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE -Wduplicated-cond -Wlogical-op)
        else()
            # GCC's -Wnull-dereference is a flow-sensitive optimizer warning that reports false positives inside
            # inlined third-party code (EnTT sparse sets at -O2), so it is only enabled for Clang's syntactic check.
            target_compile_options(${target} PRIVATE -Wnull-dereference)
        endif()
        if(STRADA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()

    if(WIN32)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    endif()

    if(STRADA_ENABLE_SANITIZERS)
        if(MSVC)
            target_compile_options(${target} PRIVATE /fsanitize=address)
        else()
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
    strada_add_thread_sanitizer(${target})
    # Tells the sources that a sanitizer's runtime is linked in. Such a runtime defines the global allocation functions
    # itself, so a source that would replace them (StradaTests' Audio/MixAllocationHook.cpp) leaves them to it.
    if(STRADA_ENABLE_SANITIZERS OR STRADA_ENABLE_THREAD_SANITIZER)
        target_compile_definitions(${target} PRIVATE ST_SANITIZER_BUILD)
    endif()
endfunction()

# strada_add_thread_sanitizer(<target>)
# With STRADA_ENABLE_THREAD_SANITIZER, compiles the target with ThreadSanitizer, and links an executable or shared
# library with its runtime. It is applied to Strada's targets and to every third-party target the build compiles:
# synchronization done by code that is not instrumented (a job system's atomics, a ring buffer's) is invisible to the
# sanitizer, which then reports races that are none. Jolt also compiles different inline code when it sees the sanitizer
# (JPH_TSAN_ENABLED in Jolt/Core/Core.h), so Jolt and the engine sources that include it must agree.
function(strada_add_thread_sanitizer target)
    if(NOT STRADA_ENABLE_THREAD_SANITIZER)
        return()
    endif()
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY")
        return()
    endif()
    target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=thread)
endfunction()

# strada_configure_third_party_target(<target>)
# For third-party sources compiled inside the Strada build (single-header implementations, ImGui, ...):
# same standard and CRT, warnings silenced.
function(strada_configure_third_party_target target)
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY")
        return()
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /W0 /utf-8 /bigobj)
    else()
        target_compile_options(${target} PRIVATE -w)
    endif()

    if(WIN32)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    endif()

    set_target_properties(${target} PROPERTIES FOLDER "ThirdParty")
    strada_add_thread_sanitizer(${target})
endfunction()

# strada_precompile_third_party_headers(<target>)
# Precompiles the third-party and standard headers that nearly every source of a target parses, for the targets whose
# sources include no precompiled header of their own (the editor core and the C++ test executables; the engine library
# has Strada/Source/stpch.h). CMake forces the header into every C++ source of the target, so no source includes it,
# and each source still includes everything it uses. STRADA_PRECOMPILE_THIRD_PARTY_HEADERS=OFF builds these targets
# without it, as CI's Linux Clang Debug job does, so a missing include fails there. GCC always builds them without it.
# The list was measured (MSVC Release, /Bt+, eight sources of each target): it halves the front end's time. EnTT,
# Dear ImGui, NVRHI and doctest saved about 1 s more over eight sources at most, within noise, and stay out. Only
# third-party and standard headers belong here, never a Strada header: an edit to one would recompile every source of
# these targets, and a source that defines a configuration macro before its first include
# (Core/ProfilerDistFormTests.cpp) would no longer see it first.
set(STRADA_PRECOMPILED_THIRD_PARTY_HEADERS
    <algorithm>
    <array>
    <atomic>
    <chrono>
    <cmath>
    <cstddef>
    <cstdint>
    <cstring>
    <filesystem>
    <functional>
    <limits>
    <map>
    <memory>
    <optional>
    <set>
    <span>
    <string>
    <string_view>
    <system_error>
    <tuple>
    <unordered_map>
    <unordered_set>
    <utility>
    <variant>
    <vector>
    # Strada/Core/Log.h's: spdlog with its bundled fmt (the larger half of what these headers cost), and glm.
    <glm/glm.hpp>
    <glm/gtc/quaternion.hpp>
    <spdlog/fmt/std.h>
    <spdlog/spdlog.h>
    # Strada/Serialization/JsonSerialization.h's, which most sources reach through the serializers and assets.
    <nlohmann/json.hpp>)

function(strada_precompile_third_party_headers target)
    # Not with GCC, in any version or configuration: with the header, CI's Linux GCC Release job (GCC 13) failed with
    # -Warray-bounds errors inside libstdc++'s string concatenation at six places in five test sources, and without it
    # that job reported none of them. The cause is not established.
    if(STRADA_PRECOMPILE_THIRD_PARTY_HEADERS AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_precompile_headers(${target} PRIVATE ${STRADA_PRECOMPILED_THIRD_PARTY_HEADERS})
    endif()
endfunction()

# Build-configuration macros (ST_DEBUG / ST_RELEASE / ST_DIST) for a target and its consumers.
function(strada_add_configuration_definitions target)
    target_compile_definitions(${target} PUBLIC
        $<$<CONFIG:Debug>:ST_DEBUG>
        $<$<CONFIG:Release>:ST_RELEASE>
        $<$<CONFIG:Dist>:ST_DIST>)
endfunction()
