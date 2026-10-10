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
