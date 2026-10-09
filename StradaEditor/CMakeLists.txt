# Strada editor: StradaEditorCore (testable library) + StradaEditor (executable).

set(STRADA_EDITOR_CORE_SOURCES
    Source/Editor/Automation/AutomationInstance.cpp
    Source/Editor/Automation/AutomationInstance.h
    Source/Editor/Automation/AutomationServer.cpp
    Source/Editor/Automation/AutomationServer.h
    Source/Editor/Automation/CommandRegistry.cpp
    Source/Editor/Automation/CommandRegistry.h
    Source/Editor/Automation/CommandResult.cpp
    Source/Editor/Automation/CommandResult.h
    Source/Editor/Automation/EditorCommands.cpp
    Source/Editor/Automation/EditorCommands.h
    Source/Editor/Automation/JsonRpc.cpp
    Source/Editor/Automation/JsonRpc.h
    Source/Editor/Automation/JsonSchema.cpp
    Source/Editor/Automation/JsonSchema.h
    Source/Editor/Commands/CommandHistory.cpp
    Source/Editor/Commands/CommandHistory.h
    Source/Editor/Commands/ComponentCommands.cpp
    Source/Editor/Commands/ComponentCommands.h
    Source/Editor/Commands/EditorCommand.h
    Source/Editor/Commands/EntityCommands.cpp
    Source/Editor/Commands/EntityCommands.h
    Source/Editor/Commands/EntitySnapshot.cpp
    Source/Editor/Commands/EntitySnapshot.h
    Source/Editor/Commands/SceneCommands.cpp
    Source/Editor/Commands/SceneCommands.h
    Source/Editor/EditorContext.cpp
    Source/Editor/EditorContext.h
    Source/Editor/EditorLayer.cpp
    Source/Editor/EditorLayer.h
    Source/Editor/EditorOperations.cpp
    Source/Editor/EditorOperations.h
    Source/Editor/EntitySelection.cpp
    Source/Editor/EntitySelection.h
    Source/Editor/Panels/ConsolePanel.cpp
    Source/Editor/Panels/ConsolePanel.h
    Source/Editor/Panels/StatisticsPanel.cpp
    Source/Editor/Panels/StatisticsPanel.h
)

add_library(StradaEditorCore STATIC ${STRADA_EDITOR_CORE_SOURCES})
target_include_directories(StradaEditorCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(StradaEditorCore PUBLIC Strada)
if(WIN32)
    # Winsock for the automation server.
    target_link_libraries(StradaEditorCore PRIVATE ws2_32)
endif()
strada_configure_target(StradaEditorCore)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/Source" FILES ${STRADA_EDITOR_CORE_SOURCES})
set_target_properties(StradaEditorCore PROPERTIES FOLDER "Editor")

add_executable(StradaEditor Source/EditorApplication.cpp)
target_link_libraries(StradaEditor PRIVATE StradaEditorCore)
strada_configure_target(StradaEditor)
set_target_properties(StradaEditor PROPERTIES FOLDER "Editor")
