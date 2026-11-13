# StradaEditorTests.Partition: fails unless the shards of Shards.cmake are a partition of StradaEditorTests, that is
# unless every test case is in exactly one shard and no shard is empty.
#
#   cmake -DTEST_EXECUTABLE=<StradaEditorTests> [-DSHARDS_FILE=<Shards.cmake>] -P CheckShardPartition.cmake
#
# SHARDS_FILE defaults to the Shards.cmake beside this script; naming another file checks a changed definition before
# it replaces the real one.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED TEST_EXECUTABLE)
    message(FATAL_ERROR "Pass -DTEST_EXECUTABLE=<path of StradaEditorTests>.")
endif()
if(NOT DEFINED SHARDS_FILE)
    set(SHARDS_FILE "${CMAKE_CURRENT_LIST_DIR}/Shards.cmake")
endif()
include("${SHARDS_FILE}")

# Lists the test cases the doctest arguments select, one XML element per test case: its name, file and line identify it.
function(strada_list_test_cases result)
    execute_process(COMMAND "${TEST_EXECUTABLE}" --list-test-cases --reporters=xml ${ARGN}
        OUTPUT_VARIABLE output ERROR_VARIABLE errors RESULT_VARIABLE exitCode)
    if(NOT exitCode EQUAL 0)
        message(FATAL_ERROR "Listing the test cases of ${TEST_EXECUTABLE} ${ARGN} failed (${exitCode}):\n${errors}")
    endif()
    # The listing holds ';' (in names and in XML references such as &quot;), '[' and ']', which split or group CMake list
    # elements: replace them with tokens that hold none of them.
    string(REPLACE ";" "@SEMICOLON@" output "${output}")
    string(REPLACE "[" "@LEFT_BRACKET@" output "${output}")
    string(REPLACE "]" "@RIGHT_BRACKET@" output "${output}")
    string(REGEX MATCHALL "<TestCase [^\n]*/>" testCases "${output}")
    set(${result} "${testCases}" PARENT_SCOPE)
endfunction()

# "<name>" in <file>, for messages.
function(strada_describe_test_case testCase result)
    string(REGEX MATCH " name=\"([^\"]*)\"" ignored "${testCase}")
    set(name "${CMAKE_MATCH_1}")
    string(REGEX MATCH " filename=\"([^\"]*)\"" ignored "${testCase}")
    set(${result} "\"${name}\" in ${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

strada_list_test_cases(allTestCases)
list(LENGTH allTestCases allCount)
if(allCount EQUAL 0)
    message(FATAL_ERROR "${TEST_EXECUTABLE} lists no test cases.")
endif()

set(problems "")
set(shardCounts "")
foreach(shard IN LISTS STRADA_EDITOR_TEST_SHARDS)
    if(NOT DEFINED STRADA_EDITOR_TEST_SHARD_${shard})
        list(APPEND problems "Shard ${shard} has no filter (STRADA_EDITOR_TEST_SHARD_${shard}).")
        continue()
    endif()
    strada_list_test_cases(shardTestCases ${STRADA_EDITOR_TEST_SHARD_${shard}})
    list(LENGTH shardTestCases shardCount)
    list(APPEND shardCounts "${shard} ${shardCount}")
    if(shardCount EQUAL 0)
        string(REPLACE ";" " " filters "${STRADA_EDITOR_TEST_SHARD_${shard}}")
        list(APPEND problems "Shard ${shard} selects no test case (${filters}).")
    endif()
    foreach(testCase IN LISTS shardTestCases)
        string(MD5 key "${testCase}")
        if(DEFINED shardOf_${key})
            strada_describe_test_case("${testCase}" description)
            list(APPEND problems "${description} is in two shards: ${shardOf_${key}} and ${shard}.")
        else()
            set(shardOf_${key} ${shard})
        endif()
    endforeach()
endforeach()

foreach(testCase IN LISTS allTestCases)
    string(MD5 key "${testCase}")
    if(NOT DEFINED shardOf_${key})
        strada_describe_test_case("${testCase}" description)
        list(APPEND problems "${description} is in no shard.")
    endif()
endforeach()

string(REPLACE ";" ", " shardSummary "${shardCounts}")
list(LENGTH problems problemCount)
if(problemCount GREATER 0)
    string(REPLACE ";" "\n  " problemText "${problems}")
    string(REPLACE "@SEMICOLON@" ";" problemText "${problemText}")
    string(REPLACE "@LEFT_BRACKET@" "[" problemText "${problemText}")
    string(REPLACE "@RIGHT_BRACKET@" "]" problemText "${problemText}")
    message(FATAL_ERROR "The shards are not a partition of the ${allCount} test cases "
        "(${shardSummary}): ${problemCount} problem(s).\n  ${problemText}\n"
        "Change the filters in ${SHARDS_FILE} so that every test case is in exactly one shard.")
endif()
message(STATUS "The ${allCount} test cases are a partition into the shards ${shardSummary}.")
