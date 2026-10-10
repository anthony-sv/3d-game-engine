# .NET integration: the .NET SDK builds the C# projects, and its host pack provides the native hosting headers
# (hostfxr.h, coreclr_delegates.h). hostfxr itself is located at run time by the engine (DotNetHost.cpp).

set(STRADA_DOTNET_VERSION_MAJOR 10)

find_program(STRADA_DOTNET_EXECUTABLE dotnet
    HINTS "$ENV{DOTNET_ROOT}"
    DOC "The .NET SDK driver (dotnet)"
    REQUIRED)
# The driver is often a symbolic link (/usr/bin/dotnet, /usr/local/bin/dotnet) into the installation.
file(REAL_PATH "${STRADA_DOTNET_EXECUTABLE}" dotnetExecutable)
get_filename_component(STRADA_DOTNET_ROOT "${dotnetExecutable}" DIRECTORY)

if(WIN32)
    set(dotnetOs win)
elseif(APPLE)
    set(dotnetOs osx)
else()
    set(dotnetOs linux)
endif()
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(dotnetProcessor "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(dotnetProcessor "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${dotnetProcessor}" dotnetProcessor)
if(dotnetProcessor MATCHES "^(arm64|aarch64)$")
    set(dotnetArchitecture arm64)
elseif(dotnetProcessor MATCHES "^(x86_64|amd64|x64)$")
    set(dotnetArchitecture x64)
else()
    message(FATAL_ERROR "C# scripting supports x64 and arm64 targets only, not '${dotnetProcessor}'")
endif()
set(STRADA_DOTNET_RID "${dotnetOs}-${dotnetArchitecture}")

# The newest host pack of the required major version.
set(hostPacks "${STRADA_DOTNET_ROOT}/packs/Microsoft.NETCore.App.Host.${STRADA_DOTNET_RID}")
file(GLOB hostPackVersions LIST_DIRECTORIES true RELATIVE "${hostPacks}" "${hostPacks}/${STRADA_DOTNET_VERSION_MAJOR}.*")
if(NOT hostPackVersions)
    message(FATAL_ERROR "The .NET ${STRADA_DOTNET_VERSION_MAJOR} SDK host pack for ${STRADA_DOTNET_RID} was not found in "
        "${hostPacks}. Install the .NET ${STRADA_DOTNET_VERSION_MAJOR} SDK (https://dot.net) or point DOTNET_ROOT to it.")
endif()
list(SORT hostPackVersions COMPARE NATURAL ORDER DESCENDING)
list(GET hostPackVersions 0 hostPackVersion)
set(STRADA_DOTNET_HOSTING_INCLUDE_DIR "${hostPacks}/${hostPackVersion}/runtimes/${STRADA_DOTNET_RID}/native")
message(STATUS ".NET SDK: ${STRADA_DOTNET_ROOT} (host pack ${hostPackVersion}, ${STRADA_DOTNET_RID})")

# Headers only: the engine loads hostfxr dynamically.
add_library(StradaDotNetHosting INTERFACE)
target_include_directories(StradaDotNetHosting SYSTEM INTERFACE "${STRADA_DOTNET_HOSTING_INCLUDE_DIR}")

# Build outputs of every C# project stay in the build tree (see Directory.Build.props). The dotnet CLI misreads property
# values that end in "dotnet", so the directory has another name.
set(STRADA_DOTNET_ARTIFACTS_DIR "${CMAKE_BINARY_DIR}/csharp")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(STRADA_DOTNET_CONFIGURATION Debug)
else()
    set(STRADA_DOTNET_CONFIGURATION Release)
endif()

# strada_add_dotnet_project(<target> PROJECT <csproj> OUTPUT_DIRECTORY <dir> [DEPENDS <targets>...]
#                           [PROPERTIES <name>=<value>...])
# Builds a C# project into OUTPUT_DIRECTORY on every build: MSBuild decides what is out of date, so new and changed
# sources are always picked up without listing them here.
function(strada_add_dotnet_project target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "PROJECT;OUTPUT_DIRECTORY" "DEPENDS;PROPERTIES")
    if(NOT arg_PROJECT OR NOT arg_OUTPUT_DIRECTORY)
        message(FATAL_ERROR "strada_add_dotnet_project(${target}) needs PROJECT and OUTPUT_DIRECTORY")
    endif()

    set(properties "--property:StradaArtifactsPath=${STRADA_DOTNET_ARTIFACTS_DIR}")
    foreach(property IN LISTS arg_PROPERTIES)
        list(APPEND properties "--property:${property}")
    endforeach()

    add_custom_target(${target} ALL
        COMMAND "${STRADA_DOTNET_EXECUTABLE}" build "${arg_PROJECT}"
            --configuration ${STRADA_DOTNET_CONFIGURATION}
            --output "${arg_OUTPUT_DIRECTORY}"
            --nologo
            --verbosity quiet
            ${properties}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building ${arg_PROJECT}"
        VERBATIM
        USES_TERMINAL)
    if(arg_DEPENDS)
        add_dependencies(${target} ${arg_DEPENDS})
    endif()
    set_target_properties(${target} PROPERTIES FOLDER "Scripting")
endfunction()
