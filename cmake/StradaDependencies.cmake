# Third-party dependencies. Every dependency is pinned to an exact release archive verified by SHA256.
# Keep ThirdPartyNotices.md in sync when adding, removing or updating a dependency.

include(FetchContent)

# CMake 3.30+: populate directly instead of through an ExternalProject sub-build (faster configures).
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
endif()

# Downloads <url> into the dependency cache (once) and returns the local archive path in <outputVariable>.
# FetchContent's direct population (CMP0168) ignores DOWNLOAD_DIR, so the cache is managed here. Downloads go to a
# uniquely named temporary file that is renamed into place, so concurrent configures never see partial archives.
function(strada_cache_dependency_archive name url hash outputVariable)
    file(TO_CMAKE_PATH "${STRADA_DEPENDENCY_CACHE_DIR}" cacheDirectory)
    get_filename_component(archiveName "${url}" NAME)
    set(archive "${cacheDirectory}/${name}/${archiveName}")

    if(EXISTS "${archive}")
        file(SHA256 "${archive}" existingHash)
        if(NOT existingHash STREQUAL hash)
            message(STATUS "Dependency cache: discarding ${archive} (hash mismatch)")
            file(REMOVE "${archive}")
        endif()
    endif()

    if(NOT EXISTS "${archive}")
        string(RANDOM LENGTH 12 suffix)
        set(partial "${archive}.${suffix}.part")
        message(STATUS "Dependency cache: downloading ${name}")
        file(DOWNLOAD "${url}" "${partial}" EXPECTED_HASH "SHA256=${hash}" TLS_VERIFY ON STATUS status)
        list(GET status 0 statusCode)
        if(NOT statusCode EQUAL 0)
            list(GET status 1 statusMessage)
            file(REMOVE "${partial}")
            message(FATAL_ERROR "Failed to download ${name} from ${url}: ${statusMessage}")
        endif()
        # Another configure may have finished the same download first; its archive is identical, so keep it.
        file(RENAME "${partial}" "${archive}" RESULT renameResult NO_REPLACE)
        if(NOT renameResult EQUAL 0)
            file(REMOVE "${partial}")
        endif()
    endif()

    set(${outputVariable} "${archive}" PARENT_SCOPE)
endfunction()

# strada_declare_dependency(<name> <url> <sha256> [extra FetchContent_Declare arguments...])
function(strada_declare_dependency name url hash)
    set(source "${url}")
    if(STRADA_DEPENDENCY_CACHE_DIR)
        strada_cache_dependency_archive(${name} "${url}" "${hash}" source)
    endif()
    FetchContent_Declare(${name} URL "${source}" URL_HASH "SHA256=${hash}" SYSTEM EXCLUDE_FROM_ALL ${ARGN})
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

# --- EnTT 4.0.0 (header-only ECS; requires C++20) ------------------------------------------------------------------
strada_declare_dependency(EnTT
    https://github.com/skypjack/entt/archive/refs/tags/v4.0.0.tar.gz
    32a2ff2c72cb047dfd57306006ef238820b70da7c6ce4e7e8a507ac63365212e)
set(ENTT_INSTALL OFF CACHE BOOL "" FORCE)
set(ENTT_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ENTT_BUILD_DOCS OFF CACHE BOOL "" FORCE)

# --- Vulkan-Headers 1.4.363 (matches the Vulkan SDK; must exist before NVRHI so it finds Vulkan::Headers) ----------
strada_declare_dependency(VulkanHeaders
    https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
    4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e)
set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

# --- NVRHI (main @ 2026-10-05; the project publishes no tags) -------------------------------------------------------
# NVRHI's CMake uses policy level 3.11 (CMP0077 OLD), so options must be cache variables.
strada_declare_dependency(nvrhi
    https://github.com/NVIDIA-RTX/NVRHI/archive/6b96fb03e07539f08327aea76c56d55f1de9d906.tar.gz
    f46c733ccc555fc457aaa3df510ee7df0ca089f5d1129d92903029e4fb67c560)
set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_NVAPI OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_RTXMU OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_AFTERMATH OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(NVRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glm spdlog glfw nlohmann_json EnTT VulkanHeaders nvrhi)

# --- stb (single-file libraries; pinned commit, implementation compiled in Strada/ThirdParty) -------------------------
strada_declare_dependency(stb
    https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz
    9a955b1b49a4410088a2e0ee2a9c057c3c907d0c1d75454144cb980aca0ba515
    SOURCE_SUBDIR _strada_no_cmake)
FetchContent_MakeAvailable(stb)

# --- Dear ImGui 1.92.9b (docking branch; no CMake project, compiled below) --------------------------------------------
strada_declare_dependency(imgui
    https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b-docking.tar.gz
    90ded916bd57db2e0e171b6b098940a47c6f5042725dcdc67fb19940ca8bfdcc
    SOURCE_SUBDIR _strada_no_cmake)
FetchContent_MakeAvailable(imgui)

add_library(StradaImGui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp)
target_include_directories(StradaImGui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_compile_definitions(StradaImGui PUBLIC IMGUI_DEFINE_MATH_OPERATORS IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_compile_features(StradaImGui PUBLIC cxx_std_20)
target_link_libraries(StradaImGui PUBLIC glfw)
strada_configure_third_party_target(StradaImGui)

# --- ImGuizmo (editor transform gizmos; pinned commit, only ImGuizmo.cpp is compiled) -----------------------------
strada_declare_dependency(imguizmo
    https://github.com/CedricGuillemet/ImGuizmo/archive/18cef5e031d8c6973d80284c67f60549fafd78c1.tar.gz
    6ad626f0687be12c2f3ba6542c0f1bdda9e71e395d0645e4cda37695354406d8
    SOURCE_SUBDIR _strada_no_cmake)
FetchContent_MakeAvailable(imguizmo)

add_library(StradaImGuizmo STATIC "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
target_include_directories(StradaImGuizmo SYSTEM PUBLIC "${imguizmo_SOURCE_DIR}/src")
target_link_libraries(StradaImGuizmo PUBLIC StradaImGui)
strada_configure_third_party_target(StradaImGuizmo)

# --- Model import: cgltf 1.15 (glTF 2.0), ufbx 0.23.1 (FBX and OBJ), MikkTSpace (tangent generation) ----------------
# No CMake projects; the sources are compiled by the StradaModelImport target in Strada/CMakeLists.txt.
strada_declare_dependency(cgltf
    https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.15.tar.gz
    84e352092e5cd6aab7f66de62ddb66beb5e6f18d412ca9d12950d7a55bfef25a
    SOURCE_SUBDIR _strada_no_cmake)
strada_declare_dependency(ufbx
    https://github.com/ufbx/ufbx/archive/refs/tags/v0.23.1.tar.gz
    21edd1021dfb430e37aa6214c9b3bbaa5634b1859cbbef7231e738af4f19c956
    SOURCE_SUBDIR _strada_no_cmake)
strada_declare_dependency(mikktspace
    https://github.com/mmikk/MikkTSpace/archive/3e895b49d05ea07e4c2133156cfa94369e19e409.tar.gz
    aeba65ddee85a679133d510d71d00b108f603752f90b15b9ecf7777033f6e351
    SOURCE_SUBDIR _strada_no_cmake)
FetchContent_MakeAvailable(cgltf ufbx mikktspace)

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

foreach(dependencyTarget glfw spdlog nvrhi nvrhi_vk)
    if(TARGET ${dependencyTarget})
        set_target_properties(${dependencyTarget} PROPERTIES FOLDER "ThirdParty")
    endif()
endforeach()
