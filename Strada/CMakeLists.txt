# Strada engine static library.

set(STRADA_CORE_SOURCES
    Source/Strada/Core/Application.cpp
    Source/Strada/Core/Application.h
    Source/Strada/Core/Assert.cpp
    Source/Strada/Core/Assert.h
    Source/Strada/Core/Base.h
    Source/Strada/Core/Buffer.h
    Source/Strada/Core/CommandLine.cpp
    Source/Strada/Core/CommandLine.h
    Source/Strada/Core/EntryPoint.cpp
    Source/Strada/Core/EntryPoint.h
    Source/Strada/Core/Events/ApplicationEvent.h
    Source/Strada/Core/Events/Event.h
    Source/Strada/Core/Events/KeyEvent.h
    Source/Strada/Core/Events/MouseEvent.h
    Source/Strada/Core/FileSystem.cpp
    Source/Strada/Core/FileSystem.h
    Source/Strada/Core/Hash.h
    Source/Strada/Core/Image.cpp
    Source/Strada/Core/Image.h
    Source/Strada/Core/Input.cpp
    Source/Strada/Core/Input.h
    Source/Strada/Core/KeyCodes.cpp
    Source/Strada/Core/KeyCodes.h
    Source/Strada/Core/Layer.h
    Source/Strada/Core/LayerStack.cpp
    Source/Strada/Core/LayerStack.h
    Source/Strada/Core/Log.cpp
    Source/Strada/Core/Log.h
    Source/Strada/Core/Platform.cpp
    Source/Strada/Core/Platform.h
    Source/Strada/Core/PlatformDetection.h
    Source/Strada/Core/Result.h
    Source/Strada/Core/Timer.h
    Source/Strada/Core/Timestep.h
    Source/Strada/Core/UUID.cpp
    Source/Strada/Core/UUID.h
    Source/Strada/Core/Version.h
    Source/Strada/Core/Window.cpp
    Source/Strada/Core/Window.h
)

set(STRADA_ASSET_SOURCES
    Source/Strada/Asset/AssetHandle.h
)

set(STRADA_MATH_SOURCES
    Source/Strada/Math/Math.cpp
    Source/Strada/Math/Math.h
)

set(STRADA_SCENE_SOURCES
    Source/Strada/Scene/ComponentRegistry.cpp
    Source/Strada/Scene/ComponentRegistry.h
    Source/Strada/Scene/ComponentSerialization.h
    Source/Strada/Scene/ComponentTraits.cpp
    Source/Strada/Scene/ComponentTraits.h
    Source/Strada/Scene/Components.h
    Source/Strada/Scene/Entity.h
    Source/Strada/Scene/Scene.cpp
    Source/Strada/Scene/Scene.h
    Source/Strada/Scene/SceneCamera.cpp
    Source/Strada/Scene/SceneCamera.h
    Source/Strada/Scene/SceneSerializer.cpp
    Source/Strada/Scene/SceneSerializer.h
)

set(STRADA_SCRIPT_SOURCES
    Source/Strada/Script/ScriptField.cpp
    Source/Strada/Script/ScriptField.h
)

set(STRADA_SERIALIZATION_SOURCES
    Source/Strada/Serialization/JsonSerialization.cpp
    Source/Strada/Serialization/JsonSerialization.h
)

set(STRADA_IMGUI_SOURCES
    Source/Strada/ImGui/ImGuiLayer.cpp
    Source/Strada/ImGui/ImGuiLayer.h
    Source/Strada/ImGui/ImGuiRenderer.cpp
    Source/Strada/ImGui/ImGuiRenderer.h
)

set(STRADA_RHI_SOURCES
    Source/Strada/RHI/EmbeddedShaders.h
    Source/Strada/RHI/GraphicsDevice.cpp
    Source/Strada/RHI/GraphicsDevice.h
    Source/Strada/RHI/ShaderLibrary.cpp
    Source/Strada/RHI/ShaderLibrary.h
    Source/Strada/RHI/Swapchain.cpp
    Source/Strada/RHI/Swapchain.h
    Source/Strada/RHI/TextureReadback.cpp
    Source/Strada/RHI/TextureReadback.h
    Source/Strada/RHI/VulkanContext.h
)

# Implementations of the stb single-file libraries (third-party code, warnings disabled).
add_library(StradaStb STATIC ThirdParty/StbImplementation.c)
target_include_directories(StradaStb SYSTEM PUBLIC "${stb_SOURCE_DIR}")
strada_configure_third_party_target(StradaStb)

add_library(Strada STATIC
    Source/Strada.h
    Source/stpch.h
    ${STRADA_ASSET_SOURCES}
    ${STRADA_CORE_SOURCES}
    ${STRADA_IMGUI_SOURCES}
    ${STRADA_MATH_SOURCES}
    ${STRADA_RHI_SOURCES}
    ${STRADA_SCENE_SOURCES}
    ${STRADA_SCRIPT_SOURCES}
    ${STRADA_SERIALIZATION_SOURCES}
)

target_include_directories(Strada PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_precompile_headers(Strada PRIVATE Source/stpch.h)

# nvrhi_vk must precede nvrhi for single-pass linkers; Vulkan headers are needed wherever nvrhi/vulkan.h is included.
target_link_libraries(Strada
    PUBLIC
        glm::glm
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        EnTT::EnTT
        nvrhi_vk
        nvrhi
        StradaImGui
    PRIVATE
        glfw
        StradaStb
        Vulkan::Headers
)

target_compile_definitions(Strada PUBLIC
    # glm configuration must be identical in every translation unit.
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
    GLM_FORCE_CTOR_INIT
    GLM_FORCE_SILENT_WARNINGS
    ST_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    ST_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    ST_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    ST_VERSION_STRING="${PROJECT_VERSION}"
    # Every translation unit that includes vulkan.hpp must use the dynamic dispatcher shared with NVRHI.
    VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
)

include(StradaShaders)
strada_add_shaders(Strada
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Shaders"
    SHADERS
        Fullscreen_VS:Fullscreen.hlsl:VSMain:vertex
        SolidColor_PS:Fullscreen.hlsl:PSSolidColor:pixel
        ImGui_VS:ImGui.hlsl:VSMain:vertex
        ImGui_PS:ImGui.hlsl:PSMain:pixel
)

strada_configure_target(Strada)
strada_add_configuration_definitions(Strada)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/Source" FILES
    ${STRADA_ASSET_SOURCES} ${STRADA_CORE_SOURCES} ${STRADA_IMGUI_SOURCES} ${STRADA_MATH_SOURCES} ${STRADA_RHI_SOURCES}
    ${STRADA_SCENE_SOURCES} ${STRADA_SCRIPT_SOURCES} ${STRADA_SERIALIZATION_SOURCES})
set_target_properties(Strada PROPERTIES FOLDER "Engine")
