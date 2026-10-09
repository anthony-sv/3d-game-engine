# Tests for the editor model and AI automation (doctest), run headless.

add_executable(StradaEditorTests
    Source/AutomationServerTests.cpp
    Source/CommandRegistryTests.cpp
    Source/EditorCommandsTests.cpp
    Source/EditorOperationsTests.cpp
    Source/JsonRpcTests.cpp
    Source/JsonSchemaTests.cpp
    Source/Main.cpp
    Source/TcpTestClient.cpp
    Source/TcpTestClient.h
)

target_include_directories(StradaEditorTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
    # Shared helpers (Testing::TemporaryDirectory).
    "${PROJECT_SOURCE_DIR}/Tests/StradaTests/Source")
target_link_libraries(StradaEditorTests PRIVATE StradaEditorCore doctest::doctest)
if(WIN32)
    target_link_libraries(StradaEditorTests PRIVATE ws2_32)
endif()
target_compile_definitions(StradaEditorTests PRIVATE
    DOCTEST_CONFIG_SUPER_FAST_ASSERTS
    # The command reference; a test checks that every registered command is documented.
    STRADA_AUTOMATION_DOC_PATH="${PROJECT_SOURCE_DIR}/Docs/Automation.md")

strada_configure_target(StradaEditorTests)
set_target_properties(StradaEditorTests PROPERTIES FOLDER "Tests")

add_test(NAME StradaEditorTests COMMAND StradaEditorTests --reporters=console --no-intro=true)
set_tests_properties(StradaEditorTests PROPERTIES TIMEOUT 600)
