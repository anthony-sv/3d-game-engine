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

FetchContent_MakeAvailable(glm spdlog glfw nlohmann_json VulkanHeaders nvrhi)

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
