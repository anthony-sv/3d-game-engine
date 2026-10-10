cmake_minimum_required(VERSION 3.28)

project(Strada
    VERSION 0.1.0
    DESCRIPTION "Strada 3D game engine"
    HOMEPAGE_URL "https://github.com/anthony-sv/3d-game-engine"
    LANGUAGES C CXX)

# Objective-C sources of dependencies (GLFW's Cocoa backend, the native file dialogs) need the language enabled at the top
# level: enabling it inside one dependency's directory does not reach sibling directories.
if(APPLE)
    enable_language(OBJC)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(StradaOptions)
include(StradaCompilerSettings)
include(StradaDependencies)
include(StradaDotNet)

add_subdirectory(Strada)
add_subdirectory(StradaEditor)
add_subdirectory(StradaRuntime)

if(STRADA_BUILD_TESTS)
    enable_testing()
    # The scripts of Projects/FeatureTest, built like a game's against this build's scripting API: ScriptCoreTests checks
    # that they use all of it and StradaRuntimeTests runs the project with them.
    strada_add_dotnet_project(StradaFeatureTestScripts
        PROJECT "${PROJECT_SOURCE_DIR}/Projects/FeatureTest/Scripts/FeatureTest.csproj"
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/FeatureTest"
        DEPENDS StradaScriptCore
        PROPERTIES "StradaScriptCoreAssembly=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Strada.ScriptCore.dll")
    add_subdirectory(Tests/StradaTests)
    add_subdirectory(Tests/StradaEditorTests)
    add_subdirectory(Tests/StradaRuntimeTests)
    add_subdirectory(Tests/ScriptCoreTests)
endif()
