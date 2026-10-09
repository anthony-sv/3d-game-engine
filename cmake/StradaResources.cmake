# Binary files compiled into a target (the default font, ...), found at run time with EmbeddedResources::Get(<name>).

set(STRADA_EMBED_RESOURCES_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/StradaEmbedResources.cmake")

# strada_embed_resources(<target>
#     RESOURCES <name>=<file> ...      names are what EmbeddedResources::Get looks up
# )
# Generates <build>/Resources/EmbeddedResources.cpp (implementing Strada/Core/EmbeddedResources.h) and adds it to
# <target>. The source is regenerated when a file changes.
function(strada_embed_resources target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "RESOURCES")

    set(outputDirectory "${CMAKE_CURRENT_BINARY_DIR}/Resources")
    file(MAKE_DIRECTORY "${outputDirectory}")

    set(files "")
    set(manifestEntries "")
    foreach(resource IN LISTS arg_RESOURCES)
        string(FIND "${resource}" "=" separator)
        if(separator LESS 1)
            message(FATAL_ERROR "Invalid resource '${resource}' (expected <name>=<file>)")
        endif()
        string(SUBSTRING "${resource}" 0 ${separator} name)
        math(EXPR fileStart "${separator} + 1")
        string(SUBSTRING "${resource}" ${fileStart} -1 file)
        if(NOT EXISTS "${file}")
            message(FATAL_ERROR "Resource file '${file}' (${name}) does not exist")
        endif()
        list(APPEND files "${file}")
        list(APPEND manifestEntries "${name}|${file}")
    endforeach()

    set(manifest "${outputDirectory}/ResourceManifest.txt")
    string(REPLACE ";" "\n" manifestText "${manifestEntries}")
    file(CONFIGURE OUTPUT "${manifest}" CONTENT "${manifestText}\n")

    set(generatedSource "${outputDirectory}/EmbeddedResources.cpp")
    add_custom_command(
        OUTPUT "${generatedSource}"
        COMMAND "${CMAKE_COMMAND}" -DMANIFEST=${manifest} -DOUTPUT=${generatedSource} -P "${STRADA_EMBED_RESOURCES_SCRIPT}"
        DEPENDS ${files} "${manifest}" "${STRADA_EMBED_RESOURCES_SCRIPT}"
        COMMENT "Embedding ${target} resources"
        VERBATIM)

    target_sources(${target} PRIVATE "${generatedSource}")
    set_source_files_properties("${generatedSource}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endfunction()
