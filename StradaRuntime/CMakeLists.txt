# The game player: runs the exported game next to it (Game.sgame) or a project, in a window or headless (test runs).
# Exported games ship it renamed after the game.

set(STRADA_RUNTIME_SOURCES
    Source/Runtime/RuntimeLayer.cpp
    Source/Runtime/RuntimeLayer.h
    Source/RuntimeApplication.cpp
)

add_executable(StradaRuntime ${STRADA_RUNTIME_SOURCES})
target_include_directories(StradaRuntime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(StradaRuntime PRIVATE Strada)
if(MSVC)
    # Players of Dist builds open no console window; main stays the entry point.
    target_link_options(StradaRuntime PRIVATE "$<$<CONFIG:Dist>:/SUBSYSTEM:WINDOWS;/ENTRY:mainCRTStartup>")
endif()
strada_configure_target(StradaRuntime)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/Source" FILES ${STRADA_RUNTIME_SOURCES})
set_target_properties(StradaRuntime PROPERTIES FOLDER "Runtime")

# The editor exports games with this player.
add_dependencies(StradaEditor StradaRuntime)

# What exported games ship besides the player, placed next to it for the exporter (Project/GameExporter.h): the
# third-party notices; the MSVC runtime on Windows (not in Debug builds, whose runtime is not redistributable); and for
# macOS app bundles the Vulkan loader, MoltenVK and its driver manifest from the Vulkan SDK.
set(strada_runtime_directory "$<TARGET_FILE_DIR:StradaRuntime>")
add_custom_command(TARGET StradaRuntime POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/ThirdPartyNotices.md"
        "${strada_runtime_directory}/ThirdPartyNotices.md"
    VERBATIM)

if(MSVC AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        add_custom_command(TARGET StradaRuntime POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${strada_runtime_directory}/Redist"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} "${strada_runtime_directory}/Redist"
            VERBATIM)
    else()
        message(WARNING "The MSVC runtime DLLs were not found: exported games will need the Visual C++ Redistributable")
    endif()
endif()

if(APPLE)
    set(strada_vulkan_copy_commands)
    foreach(file IN ITEMS "lib/libvulkan.1.dylib" "lib/libMoltenVK.dylib" "share/vulkan/icd.d/MoltenVK_icd.json")
        set(source "$ENV{VULKAN_SDK}/${file}")
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR "${source} was not found: macOS builds need the Vulkan SDK (VULKAN_SDK) for exported games")
        endif()
        # The SDK's libraries are symbolic links to versioned files; the bundle gets the files under the linked names.
        file(REAL_PATH "${source}" resolved)
        get_filename_component(name "${file}" NAME)
        list(APPEND strada_vulkan_copy_commands
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${resolved}" "${strada_runtime_directory}/Vulkan/${name}")
    endforeach()
    add_custom_command(TARGET StradaRuntime POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${strada_runtime_directory}/Vulkan"
        ${strada_vulkan_copy_commands}
        VERBATIM)
endif()
