# The build-tree marker engine.json beside the executables (Strada/Core/EngineLayout.h): it names this source checkout, from
# which the editor reads the engine content (templates, examples, guides, the agent kit) while it runs from the build tree.
# Written when CMake generates, rewritten only when its text changes; the game exporter never ships it.

file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}" strada_manifest_content)
# A JSON string: backslashes (possible in POSIX paths) and quotes are escaped.
string(REPLACE "\\" "\\\\" strada_manifest_content "${strada_manifest_content}")
string(REPLACE "\"" "\\\"" strada_manifest_content "${strada_manifest_content}")

# The engine's JSON form (tabs, LF line ends, a trailing newline), as Strada writes its files on every platform.
string(CONCAT strada_manifest_text
    "{\n"
    "\t\"Strada\": {\n"
    "\t\t\"Version\": 1,\n"
    "\t\t\"Type\": \"EngineManifest\"\n"
    "\t},\n"
    "\t\"Content\": \"${strada_manifest_content}\"\n"
    "}\n")

# Multi-config generators (Visual Studio, Xcode) put each configuration's executables in its own folder below bin/.
if(STRADA_IS_MULTI_CONFIG)
    set(strada_manifest_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>")
else()
    set(strada_manifest_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()
file(GENERATE OUTPUT "${strada_manifest_directory}/engine.json" CONTENT "${strada_manifest_text}" NEWLINE_STYLE UNIX)
