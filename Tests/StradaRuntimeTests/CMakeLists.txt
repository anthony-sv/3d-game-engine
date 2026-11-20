# Tests of the game player (doctest): StradaRuntime runs games written to temporary directories, as exported games and
# as projects, and its exit codes, output and window are checked.

add_executable(StradaRuntimeTests
    Source/Main.cpp
    Source/MotoRideTests.cpp
    Source/MotoSoundTests.cpp
    Source/RuntimeTestUtilities.h
    Source/RuntimeTests.cpp
    Source/SampleTests.cpp
    Source/TemplateTests.cpp
)

target_include_directories(StradaRuntimeTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
    # Shared helpers (Testing::TemporaryDirectory, the test scripts, the display and GPU switches).
    "${PROJECT_SOURCE_DIR}/Tests/StradaTests/Source"
    # The player's own pure pieces (Runtime/ScreenshotRequest.h), checked without starting it.
    "${PROJECT_SOURCE_DIR}/StradaRuntime/Source")
target_link_libraries(StradaRuntimeTests PRIVATE Strada doctest::doctest)
target_compile_definitions(StradaRuntimeTests PRIVATE
    DOCTEST_CONFIG_SUPER_FAST_ASSERTS
    STRADA_RUNTIME_PATH="$<TARGET_FILE:StradaRuntime>"
    # The repository's projects (the feature test and the Blocks and Showcase samples), run with the scripts CMake builds.
    STRADA_SAMPLE_PROJECTS_DIR="${PROJECT_SOURCE_DIR}/Projects"
    # The project templates, which the tests instantiate and play.
    STRADA_TEMPLATES_DIR="${PROJECT_SOURCE_DIR}/Templates")

# The games run the test scripts.
add_dependencies(StradaRuntimeTests StradaRuntime StradaTestScripts StradaFeatureTestScripts StradaBlocksScripts StradaCSharpStarterScripts
    StradaShowcaseScripts)

strada_configure_target(StradaRuntimeTests)
strada_precompile_third_party_headers(StradaRuntimeTests)
set_target_properties(StradaRuntimeTests PROPERTIES FOLDER "Tests")

# One ctest entry per shard (StradaRuntimeTests.<shard>) and StradaRuntimeTests.Partition, which checks that the shards
# hold every test case exactly once.
strada_add_test_shards(StradaRuntimeTests "${CMAKE_CURRENT_SOURCE_DIR}/Shards.cmake")
