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
            -Wnull-dereference
            -Wmisleading-indentation)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE -Wduplicated-cond -Wlogical-op)
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
endfunction()

# Build-configuration macros (ST_DEBUG / ST_RELEASE / ST_DIST) for a target and its consumers.
function(strada_add_configuration_definitions target)
    target_compile_definitions(${target} PUBLIC
        $<$<CONFIG:Debug>:ST_DEBUG>
        $<$<CONFIG:Release>:ST_RELEASE>
        $<$<CONFIG:Dist>:ST_DIST>)
endfunction()
