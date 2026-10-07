# Strada editor: StradaEditorCore (testable library) + StradaEditor (executable).

set(STRADA_EDITOR_CORE_SOURCES
    Source/Editor/EditorLayer.cpp
    Source/Editor/EditorLayer.h
    Source/Editor/Panels/ConsolePanel.cpp
    Source/Editor/Panels/ConsolePanel.h
    Source/Editor/Panels/StatisticsPanel.cpp
    Source/Editor/Panels/StatisticsPanel.h
)

add_library(StradaEditorCore STATIC ${STRADA_EDITOR_CORE_SOURCES})
target_include_directories(StradaEditorCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(StradaEditorCore PUBLIC Strada)
strada_configure_target(StradaEditorCore)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/Source" FILES ${STRADA_EDITOR_CORE_SOURCES})
set_target_properties(StradaEditorCore PROPERTIES FOLDER "Editor")

add_executable(StradaEditor Source/EditorApplication.cpp)
target_link_libraries(StradaEditor PRIVATE StradaEditorCore)
strada_configure_target(StradaEditor)
set_target_properties(StradaEditor PROPERTIES FOLDER "Editor")
