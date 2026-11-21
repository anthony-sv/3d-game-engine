# The shards of StradaEditorTests: one ctest entry each, so that no entry runs into ctest's timeout as the suite grows.
#
# cmake/StradaTestShards.cmake says how a shard selects its test cases by source file and how patterns match.
# StradaEditorTests.Partition checks that the shards are a partition of the suite: every test case in exactly one shard,
# no shard empty. A shard that passes 450 s on CI's Windows MSVC Debug job that runs this suite (editor and player
# tests) is split by source-file patterns: add the new shard here and take its files out of the shard that held them.

# The visual script editor's panels, panes, windows and canvases, drawn through HeadlessImGui.
set(STRADA_EDITOR_TEST_PANEL_FILES
    "*StradaEditorTests?Source?VisualScript*Panel*Tests.cpp,*VisualScriptCanvasPerformanceTests.cpp,*VisualScriptEditorsTests.cpp,*VisualScriptHeadlessCanvasTests.cpp,*VisualScriptPanesTests.cpp,*VisualScriptRenderTests.cpp,*VisualScriptWindowTests.cpp")
set(STRADA_EDITOR_TEST_GRAPH_FILES "*GraphCommandsTests.cpp,*GraphDebugCommandsTests.cpp,*GraphEditFuzzTests.cpp")

# The Moto sample played in the editor: its debugger, telemetry and menus cases and M1's agent steps, each opening the
# sample's copy.
set(STRADA_EDITOR_TEST_MOTO_FILES "*MotoPlayTests.cpp,*MotoAgentTests.cpp")

set(STRADA_TEST_SHARDS AutomationFuzz Graph VisualScriptPanels VisualScriptModel Moto Core)

set(STRADA_TEST_SHARD_AutomationFuzz "--source-file=*AutomationFuzzTests.cpp")
set(STRADA_TEST_SHARD_Graph "--source-file=${STRADA_EDITOR_TEST_GRAPH_FILES}")
set(STRADA_TEST_SHARD_VisualScriptPanels "--source-file=${STRADA_EDITOR_TEST_PANEL_FILES}")
# The rest of the visual script editor: its models, controllers, documents and commits.
set(STRADA_TEST_SHARD_VisualScriptModel
    "--source-file=*StradaEditorTests?Source?VisualScript*Tests.cpp" "--source-file-exclude=${STRADA_EDITOR_TEST_PANEL_FILES}")
set(STRADA_TEST_SHARD_Moto "--source-file=${STRADA_EDITOR_TEST_MOTO_FILES}")
# Everything the shards above do not select.
set(STRADA_TEST_SHARD_Core
    "--source-file-exclude=*AutomationFuzzTests.cpp,${STRADA_EDITOR_TEST_GRAPH_FILES},*StradaEditorTests?Source?VisualScript*Tests.cpp,${STRADA_EDITOR_TEST_MOTO_FILES}")
