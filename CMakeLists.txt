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
include(StradaEngineManifest)

add_subdirectory(Strada)
add_subdirectory(StradaEditor)
add_subdirectory(StradaRuntime)
add_subdirectory(StradaTool)

if(STRADA_BUILD_TESTS)
    # The suites the build adds (STRADA_TEST_SUITES, cmake/StradaOptions.cmake): at least one, each a suite of
    # STRADA_ALL_TEST_SUITES.
    list(JOIN STRADA_ALL_TEST_SUITES ", " allTestSuites)
    if(STRADA_TEST_SUITES STREQUAL "")
        message(FATAL_ERROR "STRADA_TEST_SUITES names no test suite: name one or more of ${allTestSuites}, separated by "
            "semicolons, or set STRADA_BUILD_TESTS=OFF to build without tests")
    endif()
    foreach(suite IN LISTS STRADA_TEST_SUITES)
        if(NOT suite IN_LIST STRADA_ALL_TEST_SUITES)
            message(FATAL_ERROR "STRADA_TEST_SUITES names '${suite}', which is not a test suite: the suites are "
                "${allTestSuites}")
        endif()
    endforeach()

    enable_testing()
    # strada_add_test_shards: the suites that run as shards, one ctest entry each, with a partition check.
    include(StradaTestShards)
    # The scripts of Projects/FeatureTest, built like a game's against this build's scripting API: ScriptCoreTests checks
    # that they use all of it and StradaRuntimeTests runs the project with them.
    strada_add_dotnet_project(StradaFeatureTestScripts
        PROJECT "${PROJECT_SOURCE_DIR}/Projects/FeatureTest/Scripts/FeatureTest.csproj"
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/FeatureTest"
        DEPENDS StradaScriptCore
        PROPERTIES "StradaScriptCoreAssembly=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Strada.ScriptCore.dll")
    # The Blocks sample's scripts: StradaRuntimeTests plays its test scene and StradaEditorTests plays the game in play mode.
    strada_add_dotnet_project(StradaBlocksScripts
        PROJECT "${PROJECT_SOURCE_DIR}/Projects/Blocks/Scripts/Blocks.csproj"
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Blocks"
        DEPENDS StradaScriptCore
        PROPERTIES "StradaScriptCoreAssembly=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Strada.ScriptCore.dll")
    # The csharp-starter template's scripts: StradaRuntimeTests plays the test scene of a project made from the template and
    # StradaEditorTests reads the graph that calls one of them.
    strada_add_dotnet_project(StradaCSharpStarterScripts
        PROJECT "${PROJECT_SOURCE_DIR}/Templates/csharp-starter/Scripts/Game.csproj"
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/CSharpStarter"
        DEPENDS StradaScriptCore
        PROPERTIES "StradaScriptCoreAssembly=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Strada.ScriptCore.dll")
    # The Showcase sample's scripts: StradaRuntimeTests plays its test scene.
    strada_add_dotnet_project(StradaShowcaseScripts
        PROJECT "${PROJECT_SOURCE_DIR}/Projects/Showcase/Scripts/Showcase.csproj"
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Showcase"
        DEPENDS StradaScriptCore
        PROPERTIES "StradaScriptCoreAssembly=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Strada.ScriptCore.dll")
    # Game scripts the scripting tests run, built against the engine's Strada.ScriptCore.dll like a game's: StradaTests
    # runs them, StradaRuntimeTests plays games with them and StradaEditorTests shows their fields in the inspector. They
    # are built here, not in one suite's directory, so that each suite has them whichever suites the build adds.
    strada_add_dotnet_project(StradaTestScripts
        PROJECT "${PROJECT_SOURCE_DIR}/Tests/TestScripts/Strada.TestScripts.csproj"
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/TestScripts"
        DEPENDS StradaScriptCore
        PROPERTIES "StradaScriptCoreDirectory=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    # In the order of STRADA_ALL_TEST_SUITES whatever the order of STRADA_TEST_SUITES, so ctest runs them in one order.
    foreach(suite IN LISTS STRADA_ALL_TEST_SUITES)
        if(suite IN_LIST STRADA_TEST_SUITES)
            add_subdirectory(Tests/${suite})
        endif()
    endforeach()
endif()

# Diagnostic only (this branch is never merged): compile one part without optimization.
set(STRADA_DIAG_NOOPT "none" CACHE STRING "Diagnostic: none, jolt, strada, tests or all")
if(MSVC)
    if(STRADA_DIAG_NOOPT STREQUAL "jolt" OR STRADA_DIAG_NOOPT STREQUAL "all")
        target_compile_options(Jolt PRIVATE /Od /Ob0)
    endif()
    if(STRADA_DIAG_NOOPT STREQUAL "strada" OR STRADA_DIAG_NOOPT STREQUAL "all")
        target_compile_options(Strada PRIVATE /Od /Ob0)
    endif()
    if((STRADA_DIAG_NOOPT STREQUAL "tests" OR STRADA_DIAG_NOOPT STREQUAL "all") AND TARGET StradaTests)
        target_compile_options(StradaTests PRIVATE /Od /Ob0)
    endif()
endif()
