# Tests of strada, the editor's command-line tool and MCP bridge (xUnit v3): an executable that runs its tests. Some
# start the editor (headless) that STRADA_EDITOR names.

set(toolTestsDirectory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/StradaToolTests")
strada_add_dotnet_project(StradaToolTests
    PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/Strada.Tool.Tests.csproj"
    OUTPUT_DIRECTORY "${toolTestsDirectory}"
    DEPENDS StradaTool
    PROPERTIES "StradaToolDirectory=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
add_dependencies(StradaToolTests StradaEditor)

add_test(NAME StradaToolTests COMMAND "${STRADA_DOTNET_EXECUTABLE}" "${toolTestsDirectory}/Strada.Tool.Tests.dll")
# Tests do not send telemetry (the test platform would by default).
set_tests_properties(StradaToolTests PROPERTIES TIMEOUT 600
    ENVIRONMENT "TESTINGPLATFORM_TELEMETRY_OPTOUT=1;STRADA_EDITOR=$<TARGET_FILE:StradaEditor>")
