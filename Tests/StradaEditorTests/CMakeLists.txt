# Tests for the editor model and AI automation (doctest), run headless.

add_executable(StradaEditorTests
    Source/AgentCommandsTests.cpp
    Source/AgentKitTests.cpp
    Source/AgentSetupDialogTests.cpp
    Source/AssetEditingTests.cpp
    Source/AssetThumbnailsTests.cpp
    Source/AutomationFuzzTests.cpp
    Source/AutomationInstanceFileTests.cpp
    Source/AutomationServerTests.cpp
    Source/CommandRegistryTests.cpp
    Source/ContentBrowsingTests.cpp
    Source/CurveAssetEditorTests.cpp
    Source/CurveEditorTests.cpp
    Source/DataAssetCommandsTests.cpp
    Source/DataAssetEditingTests.cpp
    Source/EditorCameraTests.cpp
    Source/EditorCommandsTests.cpp
    Source/EditorLocationsTests.cpp
    Source/EditorLocationsTestUtilities.h
    Source/EditorOperationsTests.cpp
    Source/EditorProfilerTests.cpp
    Source/EditorUITests.cpp
    Source/EditorWindowOptionsTests.cpp
    Source/ExportCommandsTests.cpp
    Source/FieldEditorArrayTests.cpp
    Source/GameInputTests.cpp
    Source/GraphCommandsTests.cpp
    Source/GraphDebugCommandsTests.cpp
    Source/GraphEditFuzzTests.cpp
    Source/HeadlessImGui.h
    Source/HeadlessImGuiTests.cpp
    Source/HierarchyOperationsTests.cpp
    Source/JsonRpcTests.cpp
    Source/JsonSchemaTests.cpp
    Source/Main.cpp
    Source/PanelTests.cpp
    Source/PlayModeTests.cpp
    Source/PrefabEditingTests.cpp
    Source/ProjectContentCommandsTests.cpp
    Source/ProjectCreateParametersTests.cpp
    Source/ProjectEditingTests.cpp
    Source/SampleGraphsTests.cpp
    Source/ScriptCommandsTests.cpp
    Source/ScriptFieldInspectionTests.cpp
    Source/StartCommandsTests.cpp
    Source/StartScreenModelTests.cpp
    Source/StartScreenPanelTests.cpp
    Source/TcpTestClient.cpp
    Source/TcpTestClient.h
    Source/UIEditingTests.cpp
    Source/ViewportTests.cpp
    Source/VisualScriptArrangeTests.cpp
    Source/VisualScriptBatchTests.cpp
    Source/VisualScriptBreakpointsTests.cpp
    Source/VisualScriptCanvasModelTests.cpp
    Source/VisualScriptCanvasPerformanceTests.cpp
    Source/VisualScriptClipboardTests.cpp
    Source/VisualScriptCommitTests.cpp
    Source/VisualScriptControllerTests.cpp
    Source/VisualScriptCrossGraphSweepTests.cpp
    Source/VisualScriptCrossGraphTests.cpp
    Source/VisualScriptCurveViewTests.cpp
    Source/VisualScriptDebuggerPanelTests.cpp
    Source/VisualScriptDebugSessionTests.cpp
    Source/VisualScriptDebugValuesTests.cpp
    Source/VisualScriptEditingTests.cpp
    Source/VisualScriptEditorFoundationTests.cpp
    Source/VisualScriptEditorModelTests.cpp
    Source/VisualScriptEditorsTests.cpp
    Source/VisualScriptFindTests.cpp
    Source/VisualScriptFlowTests.cpp
    Source/VisualScriptGeometryTests.cpp
    Source/VisualScriptHeadlessCanvasTests.cpp
    Source/VisualScriptHitTestTests.cpp
    Source/VisualScriptLanguageEditingTests.cpp
    Source/VisualScriptNodeTitlesTests.cpp
    Source/VisualScriptNormalizationTests.cpp
    Source/VisualScriptPaletteTests.cpp
    Source/VisualScriptPanelTests.cpp
    Source/VisualScriptPanelTestUtilities.h
    Source/VisualScriptPanesTests.cpp
    Source/VisualScriptProjectIndexTests.cpp
    Source/VisualScriptQuickFixTests.cpp
    Source/VisualScriptRenderTests.cpp
    Source/VisualScriptRuntimeEditorTests.cpp
    Source/VisualScriptSemanticsTests.cpp
    Source/VisualScriptShortcutsTests.cpp
    Source/VisualScriptSpatialIndexTests.cpp
    Source/VisualScriptTestDocuments.h
    Source/VisualScriptTestUtilities.h
    Source/VisualScriptTextTests.cpp
    Source/VisualScriptThemeStyleTests.cpp
    Source/VisualScriptThemeTests.cpp
    Source/VisualScriptTimelineTests.cpp
    Source/VisualScriptViewSettingsTests.cpp
    Source/VisualScriptViewTests.cpp
    Source/VisualScriptWindowTests.cpp
)

target_include_directories(StradaEditorTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
    # Shared helpers (Testing::TemporaryDirectory, Fuzzing.h).
    "${PROJECT_SOURCE_DIR}/Tests/StradaTests/Source")
target_link_libraries(StradaEditorTests PRIVATE StradaEditorCore StradaImGuiNullBackend doctest::doctest)
if(WIN32)
    target_link_libraries(StradaEditorTests PRIVATE ws2_32)
endif()
target_compile_definitions(StradaEditorTests PRIVATE
    DOCTEST_CONFIG_SUPER_FAST_ASSERTS
    # The command reference; a test checks that every registered command is documented.
    STRADA_AUTOMATION_DOC_PATH="${PROJECT_SOURCE_DIR}/Docs/Automation.md"
    # The architecture; a test checks that section 11 lists every key and mouse gesture of the visual script editor.
    STRADA_ARCHITECTURE_DOC_PATH="${PROJECT_SOURCE_DIR}/Docs/Architecture.md"
    # The visual scripting guide and the agents' game-making skill: tests check that they show graph.edit's example batch
    # as the graph command tests run it, and the documented debugging session step by step.
    STRADA_VISUAL_SCRIPTING_DOC_PATH="${PROJECT_SOURCE_DIR}/Docs/VisualScripting.md"
    STRADA_MAKE_GAME_SKILL_PATH="${PROJECT_SOURCE_DIR}/.claude/skills/strada-make-game/SKILL.md"
    # The shared fuzzing helpers (Fuzzing.h) take their seed documents from the FeatureTest project.
    STRADA_FEATURE_TEST_DIR="${PROJECT_SOURCE_DIR}/Projects/FeatureTest"
    # The repository's projects: the play mode tests play the Blocks sample with the scripts CMake builds and the Prism Break
    # sample, and a test reads the sample projects' graphs.
    STRADA_SAMPLE_PROJECTS_DIR="${PROJECT_SOURCE_DIR}/Projects"
    # The project templates: the sample graphs' test reads the graphs of the graph-starter and csharp-starter templates too.
    STRADA_TEMPLATES_DIR="${PROJECT_SOURCE_DIR}/Templates"
    # Shared test data: the visual script editor's tests read their node library and documents from it.
    STRADA_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/Tests/Data")

# The inspector tests show the fields of the test scripts; the play mode tests play the Blocks sample; the sample graphs'
# test reads the Showcase's and the csharp-starter template's graphs with their scripts.
add_dependencies(StradaEditorTests StradaTestScripts StradaBlocksScripts StradaCSharpStarterScripts StradaShowcaseScripts)
# The export tests ship the game player.
add_dependencies(StradaEditorTests StradaRuntime)

strada_configure_target(StradaEditorTests)
set_target_properties(StradaEditorTests PROPERTIES FOLDER "Tests")

# One ctest entry per shard (StradaEditorTests.<shard>) and StradaEditorTests.Partition, which checks that the shards
# hold every test case exactly once.
include("${CMAKE_CURRENT_SOURCE_DIR}/Shards.cmake")
strada_add_editor_test_shards(StradaEditorTests)
