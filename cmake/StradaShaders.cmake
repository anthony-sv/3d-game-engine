# HLSL -> SPIR-V shader compilation with DXC (from the Vulkan SDK) and embedding into a C++ source file.

if(DEFINED ENV{VULKAN_SDK})
    file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" STRADA_VULKAN_SDK_DIR)
endif()

find_program(STRADA_DXC_EXECUTABLE
    NAMES dxc
    HINTS "${STRADA_VULKAN_SDK_DIR}/bin" "${STRADA_VULKAN_SDK_DIR}/Bin"
    DOC "DirectX Shader Compiler used to compile HLSL to SPIR-V")
if(NOT STRADA_DXC_EXECUTABLE)
    message(FATAL_ERROR "dxc not found. Install the Vulkan SDK 1.4.x and set VULKAN_SDK (Tools/build.py does this automatically "
        "for default install locations).")
endif()

# Register shifts matching NVRHI's default VulkanBindingOffsets (t: 0, s: 128, b: 256, u: 384) for spaces 0-7.
set(STRADA_DXC_SPIRV_FLAGS -spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout -HV 2021 -Zpc -WX -O3)
foreach(space RANGE 0 7)
    list(APPEND STRADA_DXC_SPIRV_FLAGS
        -fvk-t-shift 0 ${space}
        -fvk-s-shift 128 ${space}
        -fvk-b-shift 256 ${space}
        -fvk-u-shift 384 ${space})
endforeach()

set(STRADA_EMBED_SHADERS_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/StradaEmbedShaders.cmake")

# strada_add_shaders(<target>
#     SOURCE_DIR <dir>                  directory containing the .hlsl files
#     INCLUDES <files...>               shared .hlsli/.h headers (every shader recompiles when one changes)
#     SHADERS <name>:<file>:<entry>:<stage> ...
# )
# Stages: vertex, pixel, compute. Each shader compiles to <build>/Shaders/<name>.spv; all blobs are embedded in a
# generated source file added to <target>, where ShaderLibrary finds them by name.
function(strada_add_shaders target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "SOURCE_DIR" "INCLUDES;SHADERS")

    set(outputDirectory "${CMAKE_CURRENT_BINARY_DIR}/Shaders")
    file(MAKE_DIRECTORY "${outputDirectory}")

    set(spirvFiles "")
    set(manifestEntries "")
    foreach(shader IN LISTS arg_SHADERS)
        string(REPLACE ":" ";" parts "${shader}")
        list(LENGTH parts partCount)
        if(NOT partCount EQUAL 4)
            message(FATAL_ERROR "Invalid shader specification '${shader}' (expected <name>:<file>:<entry>:<stage>)")
        endif()
        list(GET parts 0 name)
        list(GET parts 1 file)
        list(GET parts 2 entry)
        list(GET parts 3 stage)

        if(stage STREQUAL "vertex")
            set(profile vs_6_6)
        elseif(stage STREQUAL "pixel")
            set(profile ps_6_6)
        elseif(stage STREQUAL "compute")
            set(profile cs_6_6)
        else()
            message(FATAL_ERROR "Unknown shader stage '${stage}' in '${shader}'")
        endif()

        set(source "${arg_SOURCE_DIR}/${file}")
        set(output "${outputDirectory}/${name}.spv")
        add_custom_command(
            OUTPUT "${output}"
            COMMAND "${STRADA_DXC_EXECUTABLE}" ${STRADA_DXC_SPIRV_FLAGS} -T ${profile} -E ${entry}
                    -I "${arg_SOURCE_DIR}" -Fo "${output}" "${source}"
            DEPENDS "${source}" ${arg_INCLUDES}
            COMMENT "Compiling shader ${name} (${file}:${entry})"
            VERBATIM)

        list(APPEND spirvFiles "${output}")
        list(APPEND manifestEntries "${name}|${stage}|${entry}|${output}")
    endforeach()

    set(manifest "${outputDirectory}/ShaderManifest.txt")
    string(REPLACE ";" "\n" manifestText "${manifestEntries}")
    file(CONFIGURE OUTPUT "${manifest}" CONTENT "${manifestText}\n")

    set(generatedSource "${outputDirectory}/EmbeddedShaders.cpp")
    add_custom_command(
        OUTPUT "${generatedSource}"
        COMMAND "${CMAKE_COMMAND}" -DMANIFEST=${manifest} -DOUTPUT=${generatedSource} -P "${STRADA_EMBED_SHADERS_SCRIPT}"
        DEPENDS ${spirvFiles} "${manifest}" "${STRADA_EMBED_SHADERS_SCRIPT}"
        COMMENT "Embedding ${target} shaders"
        VERBATIM)

    target_sources(${target} PRIVATE "${generatedSource}")
    set_source_files_properties("${generatedSource}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endfunction()
