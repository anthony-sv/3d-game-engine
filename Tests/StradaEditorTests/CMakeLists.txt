# Tests for the editor model and AI automation (doctest), run headless.

add_executable(StradaEditorTests
    Source/AssetEditingTests.cpp
    Source/AutomationServerTests.cpp
    Source/CommandRegistryTests.cpp
    Source/EditorCameraTests.cpp
    Source/EditorCommandsTests.cpp
    Source/EditorOperationsTests.cpp
    Source/EditorUITests.cpp
    Source/ExportCommandsTests.cpp
    Source/HierarchyOperationsTests.cpp
    Source/JsonRpcTests.cpp
    Source/JsonSchemaTests.cpp
    Source/Main.cpp
    Source/PanelTests.cpp
    Source/PlayModeTests.cpp
    Source/PrefabEditingTests.cpp
    Source/ProjectEditingTests.cpp
    Source/ScriptCommandsTests.cpp
    Source/ScriptFieldInspectionTests.cpp
    Source/TcpTestClient.cpp
    Source/TcpTestClient.h
    Source/ViewportTests.cpp
)

target_include_directories(StradaEditorTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
    # Shared helpers (Testing::TemporaryDirectory).
    "${PROJECT_SOURCE_DIR}/Tests/StradaTests/Source")
target_link_libraries(StradaEditorTests PRIVATE StradaEditorCore StradaImGuiNullBackend doctest::doctest)
if(WIN32)
    target_link_libraries(StradaEditorTests PRIVATE ws2_32)
endif()
target_compile_definitions(StradaEditorTests PRIVATE
    DOCTEST_CONFIG_SUPER_FAST_ASSERTS
    # The command reference; a test checks that every registered command is documented.
    STRADA_AUTOMATION_DOC_PATH="${PROJECT_SOURCE_DIR}/Docs/Automation.md")

# The inspector tests show the fields of the test scripts.
add_dependencies(StradaEditorTests StradaTestScripts)
# The export tests ship the game player.
add_dependencies(StradaEditorTests StradaRuntime)

strada_configure_target(StradaEditorTests)
set_target_properties(StradaEditorTests PROPERTIES FOLDER "Tests")

add_test(NAME StradaEditorTests COMMAND StradaEditorTests --reporters=console --no-intro=true)
set_tests_properties(StradaEditorTests PROPERTIES TIMEOUT 600)
