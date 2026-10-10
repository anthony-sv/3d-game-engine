# Strada editor: StradaEditorCore (testable library) + StradaEditor (executable).

set(STRADA_EDITOR_CORE_SOURCES
    Source/Editor/AssetAutoRefresh.cpp
    Source/Editor/AssetAutoRefresh.h
    Source/Editor/AssetBrowsing.cpp
    Source/Editor/AssetBrowsing.h
    Source/Editor/AssetDrops.cpp
    Source/Editor/AssetDrops.h
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
    Source/Editor/Automation/ExportCommands.cpp
    Source/Editor/Automation/ExportCommands.h
    Source/Editor/Automation/JsonRpc.cpp
    Source/Editor/Automation/JsonRpc.h
    Source/Editor/Automation/JsonSchema.cpp
    Source/Editor/Automation/JsonSchema.h
    Source/Editor/Automation/PlayCommands.cpp
    Source/Editor/Automation/PlayCommands.h
    Source/Editor/Automation/ScriptCommands.cpp
    Source/Editor/Automation/ScriptCommands.h
    Source/Editor/Commands/AssetCommands.cpp
    Source/Editor/Commands/AssetCommands.h
    Source/Editor/Commands/CommandHistory.cpp
    Source/Editor/Commands/CommandHistory.h
    Source/Editor/Commands/ComponentCommands.cpp
    Source/Editor/Commands/ComponentCommands.h
    Source/Editor/Commands/CompositeCommand.cpp
    Source/Editor/Commands/CompositeCommand.h
    Source/Editor/Commands/EditorCommand.h
    Source/Editor/Commands/EntityCommands.cpp
    Source/Editor/Commands/EntityCommands.h
    Source/Editor/Commands/EntitySnapshot.cpp
    Source/Editor/Commands/EntitySnapshot.h
    Source/Editor/Commands/PrefabCommands.cpp
    Source/Editor/Commands/PrefabCommands.h
    Source/Editor/Commands/SceneCommands.cpp
    Source/Editor/Commands/SceneCommands.h
    Source/Editor/ComponentInspection.cpp
    Source/Editor/ComponentInspection.h
    Source/Editor/DefaultScene.cpp
    Source/Editor/DefaultScene.h
    Source/Editor/EditorCamera.cpp
    Source/Editor/EditorCamera.h
    Source/Editor/EditorContext.cpp
    Source/Editor/EditorContext.h
    Source/Editor/EditorExport.cpp
    Source/Editor/EditorExport.h
    Source/Editor/EditorLayer.cpp
    Source/Editor/EditorLayer.h
    Source/Editor/EditorOperations.cpp
    Source/Editor/EditorOperations.h
    Source/Editor/EditorScripts.cpp
    Source/Editor/EditorScripts.h
    Source/Editor/EntityBounds.cpp
    Source/Editor/EntityBounds.h
    Source/Editor/EntityPresets.cpp
    Source/Editor/EntityPresets.h
    Source/Editor/EntitySelection.cpp
    Source/Editor/EntitySelection.h
    Source/Editor/FileDialogs.cpp
    Source/Editor/FileDialogs.h
    Source/Editor/HierarchyEditing.cpp
    Source/Editor/HierarchyEditing.h
    Source/Editor/Panels/ConsolePanel.cpp
    Source/Editor/Panels/ConsolePanel.h
    Source/Editor/Panels/ContentBrowserPanel.cpp
    Source/Editor/Panels/ContentBrowserPanel.h
    Source/Editor/Panels/InspectorPanel.cpp
    Source/Editor/Panels/InspectorPanel.h
    Source/Editor/Panels/ProjectSettingsPanel.cpp
    Source/Editor/Panels/ProjectSettingsPanel.h
    Source/Editor/Panels/SceneHierarchyPanel.cpp
    Source/Editor/Panels/SceneHierarchyPanel.h
    Source/Editor/Panels/SceneSettingsPanel.cpp
    Source/Editor/Panels/SceneSettingsPanel.h
    Source/Editor/Panels/StatisticsPanel.cpp
    Source/Editor/Panels/StatisticsPanel.h
    Source/Editor/Panels/ViewportPanel.cpp
    Source/Editor/Panels/ViewportPanel.h
    Source/Editor/PlayMode.cpp
    Source/Editor/PlayMode.h
    Source/Editor/RecentProjects.cpp
    Source/Editor/RecentProjects.h
    Source/Editor/ScriptFieldInspection.cpp
    Source/Editor/ScriptFieldInspection.h
    Source/Editor/UI/EditorUI.cpp
    Source/Editor/UI/EditorUI.h
    Source/Editor/UI/FieldEditor.cpp
    Source/Editor/UI/FieldEditor.h
    Source/Editor/Viewport/PickingIdMap.cpp
    Source/Editor/Viewport/PickingIdMap.h
    Source/Editor/Viewport/TransformEditing.cpp
    Source/Editor/Viewport/TransformEditing.h
    Source/Editor/Viewport/ViewportOverlays.cpp
    Source/Editor/Viewport/ViewportOverlays.h
)

add_library(StradaEditorCore STATIC ${STRADA_EDITOR_CORE_SOURCES})
target_include_directories(StradaEditorCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(StradaEditorCore PUBLIC Strada PRIVATE StradaImGuizmo nfd glfw)
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
