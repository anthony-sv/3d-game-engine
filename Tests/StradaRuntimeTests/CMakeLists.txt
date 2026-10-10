# Tests of the game player (doctest): StradaRuntime runs games written to temporary directories, as exported games and
# as projects, and its exit codes, output and window are checked.

add_executable(StradaRuntimeTests
    Source/Main.cpp
    Source/RuntimeTests.cpp
)

target_include_directories(StradaRuntimeTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
    # Shared helpers (Testing::TemporaryDirectory, the test scripts, the display and GPU switches).
    "${PROJECT_SOURCE_DIR}/Tests/StradaTests/Source")
target_link_libraries(StradaRuntimeTests PRIVATE Strada doctest::doctest)
target_compile_definitions(StradaRuntimeTests PRIVATE
    DOCTEST_CONFIG_SUPER_FAST_ASSERTS
    STRADA_RUNTIME_PATH="$<TARGET_FILE:StradaRuntime>"
    # The repository's projects (the feature test and the Blocks sample), run with the scripts CMake builds.
    STRADA_PROJECTS_DIR="${PROJECT_SOURCE_DIR}/Projects")

# The games run the test scripts.
add_dependencies(StradaRuntimeTests StradaRuntime StradaTestScripts StradaFeatureTestScripts StradaBlocksScripts)

strada_configure_target(StradaRuntimeTests)
set_target_properties(StradaRuntimeTests PROPERTIES FOLDER "Tests")

add_test(NAME StradaRuntimeTests COMMAND StradaRuntimeTests --reporters=console --no-intro=true)
set_tests_properties(StradaRuntimeTests PROPERTIES TIMEOUT 600)
