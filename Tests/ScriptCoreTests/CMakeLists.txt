# Unit tests of the scripting API's managed parts (xUnit v3): an executable that runs its tests.

set(scriptCoreTestsDirectory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ScriptCoreTests")
strada_add_dotnet_project(StradaScriptCoreTests
    PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/Strada.ScriptCore.Tests.csproj"
    OUTPUT_DIRECTORY "${scriptCoreTestsDirectory}"
    DEPENDS StradaScriptCore
    PROPERTIES "StradaScriptCoreDirectory=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
# The API coverage test reads the feature-test scripts.
add_dependencies(StradaScriptCoreTests StradaFeatureTestScripts)

add_test(NAME ScriptCoreTests COMMAND "${STRADA_DOTNET_EXECUTABLE}" "${scriptCoreTestsDirectory}/Strada.ScriptCore.Tests.dll")
# Tests do not send telemetry (the test platform would by default).
set_tests_properties(ScriptCoreTests PROPERTIES TIMEOUT 600 ENVIRONMENT "TESTINGPLATFORM_TELEMETRY_OPTOUT=1")
