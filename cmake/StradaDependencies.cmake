# Third-party dependencies. Every dependency is pinned to an exact release archive verified by SHA256.
# Keep ThirdPartyNotices.md in sync when adding, removing or updating a dependency.

include(FetchContent)

# CMake 3.30+: populate directly instead of through an ExternalProject sub-build (faster configures).
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
endif()

# strada_declare_dependency(<name> <url> <sha256> [extra FetchContent_Declare arguments...])
function(strada_declare_dependency name url hash)
    set(arguments URL "${url}" URL_HASH "SHA256=${hash}" SYSTEM EXCLUDE_FROM_ALL ${ARGN})
    if(STRADA_DEPENDENCY_CACHE_DIR)
        file(TO_CMAKE_PATH "${STRADA_DEPENDENCY_CACHE_DIR}" cacheDirectory)
        list(APPEND arguments DOWNLOAD_DIR "${cacheDirectory}/${name}")
    endif()
    FetchContent_Declare(${name} ${arguments})
endfunction()

# Third-party CMake projects must not inherit our flags or install rules.
set(CMAKE_SKIP_INSTALL_RULES ON)

# --- glm 1.0.3 (header-only) ---------------------------------------------------------------------------------------
strada_declare_dependency(glm
    https://github.com/g-truc/glm/archive/refs/tags/1.0.3.tar.gz
    6775e47231a446fd086d660ecc18bcd076531cfedd912fbd66e576b118607001)
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)

# --- spdlog 1.17.0 (compiled, bundled fmt) -------------------------------------------------------------------------
strada_declare_dependency(spdlog
    https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz
    d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "" FORCE)
if(WIN32)
    # Log file paths may contain non-ASCII characters (user profile directory).
    set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL "" FORCE)
endif()

# --- GLFW 3.5.1 ----------------------------------------------------------------------------------------------------
strada_declare_dependency(glfw
    https://github.com/glfw/glfw/archive/refs/tags/3.5.1.tar.gz
    5234f4f29473e9a06bc7847d8371858dd135d38466eeeaa652fdc9f8f9ff0c20)
set(GLFW_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

# --- nlohmann/json 3.12.0 (header-only) ----------------------------------------------------------------------------
strada_declare_dependency(nlohmann_json
    https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
set(JSON_MultipleHeaders OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glm spdlog glfw nlohmann_json)

# --- doctest 2.5.3 (tests only) ------------------------------------------------------------------------------------
if(STRADA_BUILD_TESTS)
    strada_declare_dependency(doctest
        https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
        174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331)
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
    set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(doctest)
endif()

set(CMAKE_SKIP_INSTALL_RULES OFF)

foreach(dependencyTarget glfw spdlog)
    if(TARGET ${dependencyTarget})
        set_target_properties(${dependencyTarget} PROPERTIES FOLDER "ThirdParty")
    endif()
endforeach()
