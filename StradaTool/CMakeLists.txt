# strada: the editor's command-line tool and MCP bridge (C#), built next to StradaEditor, which it starts.

strada_add_dotnet_project(StradaTool
    PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/StradaTool.csproj"
    OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
