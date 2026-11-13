# The shards of StradaEditorTests: one ctest entry each, so that no entry runs into ctest's timeout as the suite grows.
#
# A shard is a name and the doctest arguments that select its test cases by source file: --source-file= and
# --source-file-exclude=, each with comma-separated wildcard patterns. doctest runs a test case when its __FILE__ matches
# a --source-file pattern (or none is given) and no --source-file-exclude pattern, without regard to case. __FILE__ is a
# full path with the platform's separators, so a pattern never names a separator: `*Source?PanelTests.cpp`, not
# `*/PanelTests.cpp`, selects PanelTests.cpp alone. `*` matches separators too, so a `*` inside a pattern can reach into the
# checkout's path (a clone in D:\VisualScriptLab would match `*VisualScript*Tests.cpp` in every file): a pattern with an
# inner `*` is anchored to the suite's directory, `*StradaEditorTests?Source?VisualScript*Tests.cpp`.
#
# The shards must be a partition of the suite: every test case in exactly one shard, no shard empty.
# StradaEditorTests.Partition checks that by listing the test cases unfiltered and through each shard's arguments
# (CheckShardPartition.cmake, which includes this file). A shard that passes 450 s on CI's Windows MSVC Debug job is
# split by source-file patterns: add the new shard here and take its files out of the shard that held them.

# The visual script editor's panels, panes, windows and canvases, drawn through HeadlessImGui.
set(STRADA_EDITOR_TEST_PANEL_FILES
    "*StradaEditorTests?Source?VisualScript*Panel*Tests.cpp,*VisualScriptCanvasPerformanceTests.cpp,*VisualScriptEditorsTests.cpp,*VisualScriptHeadlessCanvasTests.cpp,*VisualScriptPanesTests.cpp,*VisualScriptRenderTests.cpp,*VisualScriptWindowTests.cpp")
set(STRADA_EDITOR_TEST_GRAPH_FILES "*GraphCommandsTests.cpp,*GraphDebugCommandsTests.cpp,*GraphEditFuzzTests.cpp")

set(STRADA_EDITOR_TEST_SHARDS AutomationFuzz Graph VisualScriptPanels VisualScriptModel Core)

set(STRADA_EDITOR_TEST_SHARD_AutomationFuzz "--source-file=*AutomationFuzzTests.cpp")
set(STRADA_EDITOR_TEST_SHARD_Graph "--source-file=${STRADA_EDITOR_TEST_GRAPH_FILES}")
set(STRADA_EDITOR_TEST_SHARD_VisualScriptPanels "--source-file=${STRADA_EDITOR_TEST_PANEL_FILES}")
# The rest of the visual script editor: its models, controllers, documents and commits.
set(STRADA_EDITOR_TEST_SHARD_VisualScriptModel
    "--source-file=*StradaEditorTests?Source?VisualScript*Tests.cpp" "--source-file-exclude=${STRADA_EDITOR_TEST_PANEL_FILES}")
# Everything the shards above do not select.
set(STRADA_EDITOR_TEST_SHARD_Core
    "--source-file-exclude=*AutomationFuzzTests.cpp,${STRADA_EDITOR_TEST_GRAPH_FILES},*StradaEditorTests?Source?VisualScript*Tests.cpp")

# Registers <target>.<shard> for every shard and <target>.Partition, which fails when the shards are not a partition.
function(strada_add_editor_test_shards target)
    add_test(NAME ${target}.Partition
        COMMAND "${CMAKE_COMMAND}" "-DTEST_EXECUTABLE=$<TARGET_FILE:${target}>"
            "-DSHARDS_FILE=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Shards.cmake"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CheckShardPartition.cmake")
    set_tests_properties(${target}.Partition PROPERTIES TIMEOUT 120)
    foreach(shard IN LISTS STRADA_EDITOR_TEST_SHARDS)
        add_test(NAME ${target}.${shard}
            COMMAND ${target} --reporters=console --no-intro=true ${STRADA_EDITOR_TEST_SHARD_${shard}})
        set_tests_properties(${target}.${shard} PROPERTIES TIMEOUT 600)
    endforeach()
endfunction()
