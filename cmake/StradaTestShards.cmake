# Test shards: a doctest suite runs as one ctest entry per shard, so that no entry runs into ctest's timeout as the suite
# grows. StradaTests, StradaEditorTests and StradaRuntimeTests use them.
#
# A suite's Shards.cmake (beside its CMakeLists.txt) defines its shards and nothing else, because the partition check
# includes it in script mode too:
#
#   STRADA_TEST_SHARDS                the shards' names, in the order ctest runs them;
#   STRADA_TEST_SHARD_<shard>         the doctest arguments that select the shard's test cases by source file.
#
# The arguments are --source-file= and --source-file-exclude=, each with comma-separated wildcard patterns. doctest runs a
# test case when its __FILE__ matches a --source-file pattern (or none is given) and no --source-file-exclude pattern,
# without regard to case. __FILE__ is a full path with the platform's separators, so a pattern never names a separator:
# `*Source?PanelTests.cpp`, not `*/PanelTests.cpp`, selects PanelTests.cpp alone. `*` matches separators too, so a `*`
# inside a pattern can reach into the checkout's path (a clone in D:\VisualScriptLab would match `*VisualScript*Tests.cpp`
# in every file): a pattern with an inner `*` is anchored to the suite's directory,
# `*StradaEditorTests?Source?VisualScript*Tests.cpp`.
#
# The shards must be a partition of the suite: every test case in exactly one shard, no shard empty. <suite>.Partition
# checks that by listing the test cases unfiltered and through each shard's arguments (CheckShardPartition.cmake).

# Registers <target>.Partition, which fails when the shards of shardsFile are not a partition of target's test cases, and
# <target>.<shard> for every shard. The shards file is included here, in the function's scope, so that the variables of
# one suite's shards never reach another's.
function(strada_add_test_shards target shardsFile)
    include("${shardsFile}")
    add_test(NAME ${target}.Partition
        COMMAND "${CMAKE_COMMAND}" "-DTEST_EXECUTABLE=$<TARGET_FILE:${target}>" "-DSHARDS_FILE=${shardsFile}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CheckShardPartition.cmake")
    set_tests_properties(${target}.Partition PROPERTIES TIMEOUT 120)
    foreach(shard IN LISTS STRADA_TEST_SHARDS)
        add_test(NAME ${target}.${shard} COMMAND ${target} --reporters=console --no-intro=true ${STRADA_TEST_SHARD_${shard}})
        set_tests_properties(${target}.${shard} PROPERTIES TIMEOUT 600)
    endforeach()
endfunction()
