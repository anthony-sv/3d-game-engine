cmake_minimum_required(VERSION 3.28)

project(Strada
    VERSION 0.1.0
    DESCRIPTION "Strada 3D game engine"
    HOMEPAGE_URL "https://github.com/anthony-sv/3d-game-engine"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(StradaOptions)
include(StradaCompilerSettings)
include(StradaDependencies)

add_subdirectory(Strada)
add_subdirectory(StradaEditor)

if(STRADA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests/StradaTests)
endif()
