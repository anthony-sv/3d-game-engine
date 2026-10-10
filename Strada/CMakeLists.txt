# Strada engine static library.

set(STRADA_CORE_SOURCES
    Source/Strada/Core/Application.cpp
    Source/Strada/Core/Application.h
    Source/Strada/Core/Assert.cpp
    Source/Strada/Core/Assert.h
    Source/Strada/Core/Base.h
    Source/Strada/Core/Base64.cpp
    Source/Strada/Core/Base64.h
    Source/Strada/Core/Buffer.h
    Source/Strada/Core/CommandLine.cpp
    Source/Strada/Core/CommandLine.h
    Source/Strada/Core/CrashHandler.cpp
    Source/Strada/Core/CrashHandler.h
    Source/Strada/Core/EmbeddedResources.h
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
    Source/Strada/Core/Utf8.cpp
    Source/Strada/Core/Utf8.h
    Source/Strada/Core/Version.h
    Source/Strada/Core/Window.cpp
    Source/Strada/Core/Window.h
)

set(STRADA_ASSET_SOURCES
    Source/Strada/Asset/Asset.cpp
    Source/Strada/Asset/Asset.h
    Source/Strada/Asset/AssetHandle.h
    Source/Strada/Asset/AssetManager.cpp
    Source/Strada/Asset/AssetManager.h
    Source/Strada/Asset/AssetRegistry.cpp
    Source/Strada/Asset/AssetRegistry.h
    Source/Strada/Asset/AudioClipAsset.cpp
    Source/Strada/Asset/AudioClipAsset.h
    Source/Strada/Asset/BuiltInAssets.cpp
    Source/Strada/Asset/BuiltInAssets.h
    Source/Strada/Asset/EnvironmentAsset.cpp
    Source/Strada/Asset/EnvironmentAsset.h
    Source/Strada/Asset/FontAsset.cpp
    Source/Strada/Asset/FontAsset.h
    Source/Strada/Asset/MaterialAsset.cpp
    Source/Strada/Asset/MaterialAsset.h
    Source/Strada/Asset/MeshFactory.cpp
    Source/Strada/Asset/MeshFactory.h
    Source/Strada/Asset/MeshImporter.cpp
    Source/Strada/Asset/MeshImporter.h
    Source/Strada/Asset/MeshProcessing.cpp
    Source/Strada/Asset/MeshProcessing.h
    Source/Strada/Asset/MeshSource.cpp
    Source/Strada/Asset/MeshSource.h
    Source/Strada/Asset/PrefabAsset.cpp
    Source/Strada/Asset/PrefabAsset.h
    Source/Strada/Asset/TextureAsset.cpp
    Source/Strada/Asset/TextureAsset.h
    Source/Strada/Asset/TrueTypeSanitizer.cpp
    Source/Strada/Asset/TrueTypeSanitizer.h
)

set(STRADA_MATH_SOURCES
    Source/Strada/Math/AABB.cpp
    Source/Strada/Math/AABB.h
    Source/Strada/Math/Frustum.cpp
    Source/Strada/Math/Frustum.h
    Source/Strada/Math/Math.cpp
    Source/Strada/Math/Math.h
)

set(STRADA_AUDIO_SOURCES
    Source/Strada/Audio/AudioEngine.cpp
    Source/Strada/Audio/AudioEngine.h
    Source/Strada/Audio/AudioScene.cpp
    Source/Strada/Audio/AudioScene.h
    Source/Strada/Audio/MiniaudioContext.h
)

set(STRADA_PHYSICS_SOURCES
    Source/Strada/Physics/JoltContext.h
    Source/Strada/Physics/PhysicsScene.cpp
    Source/Strada/Physics/PhysicsScene.h
    Source/Strada/Physics/PhysicsSystem.cpp
    Source/Strada/Physics/PhysicsSystem.h
    Source/Strada/Physics/PhysicsTypes.h
)

set(STRADA_PLATFORM_SOURCES
    Source/Strada/Platform/Process.cpp
    Source/Strada/Platform/Process.h
)

set(STRADA_PROJECT_SOURCES
    Source/Strada/Project/GameExporter.cpp
    Source/Strada/Project/GameExporter.h
    Source/Strada/Project/Project.cpp
    Source/Strada/Project/Project.h
    Source/Strada/Project/ProjectSettings.cpp
    Source/Strada/Project/ProjectSettings.h
    Source/Strada/Project/SceneRunner.cpp
    Source/Strada/Project/SceneRunner.h
    Source/Strada/Project/ScriptProject.cpp
    Source/Strada/Project/ScriptProject.h
)

set(STRADA_RENDERER_SOURCES
    Source/Strada/Renderer/EnvironmentMap.cpp
    Source/Strada/Renderer/EnvironmentMap.h
    Source/Strada/Renderer/FontAtlas.cpp
    Source/Strada/Renderer/FontAtlas.h
    Source/Strada/Renderer/QuadRenderer.cpp
    Source/Strada/Renderer/QuadRenderer.h
    Source/Strada/Renderer/Renderer.cpp
    Source/Strada/Renderer/Renderer.h
    Source/Strada/Renderer/SceneRenderer.cpp
    Source/Strada/Renderer/SceneRenderer.h
    Source/Strada/Renderer/SceneRendererSettings.h
    Source/Strada/Renderer/ShadowMath.cpp
    Source/Strada/Renderer/ShadowMath.h
    Source/Strada/Renderer/TextLayout.cpp
    Source/Strada/Renderer/TextLayout.h
    Source/Strada/Renderer/TextureBlitter.cpp
    Source/Strada/Renderer/TextureBlitter.h
    Source/Strada/Renderer/TextureMips.cpp
    Source/Strada/Renderer/TextureMips.h
)

set(STRADA_SCENE_SOURCES
    Source/Strada/Scene/ComponentRegistry.cpp
    Source/Strada/Scene/ComponentRegistry.h
    Source/Strada/Scene/ComponentSerialization.h
    Source/Strada/Scene/ComponentTraits.h
    Source/Strada/Scene/Components.h
    Source/Strada/Scene/Entity.h
    Source/Strada/Scene/Scene.cpp
    Source/Strada/Scene/Scene.h
    Source/Strada/Scene/SceneAudio.cpp
    Source/Strada/Scene/SceneCamera.cpp
    Source/Strada/Scene/SceneCamera.h
    Source/Strada/Scene/ScenePhysics.cpp
    Source/Strada/Scene/SceneRendering.cpp
    Source/Strada/Scene/SceneRendering.h
    Source/Strada/Scene/SceneScripting.cpp
    Source/Strada/Scene/SceneSerializer.cpp
    Source/Strada/Scene/SceneSerializer.h
)

set(STRADA_SCRIPT_SOURCES
    Source/Strada/Script/DotNetHost.cpp
    Source/Strada/Script/DotNetHost.h
    Source/Strada/Script/ScriptBindings.cpp
    Source/Strada/Script/ScriptBindings.h
    Source/Strada/Script/ScriptBindingsApplication.cpp
    Source/Strada/Script/ScriptBindingsAssets.cpp
    Source/Strada/Script/ScriptBindingsComponents.cpp
    Source/Strada/Script/ScriptBindingsEntity.cpp
    Source/Strada/Script/ScriptBindingsRuntime.cpp
    Source/Strada/Script/ScriptEngine.cpp
    Source/Strada/Script/ScriptEngine.h
    Source/Strada/Script/ScriptField.cpp
    Source/Strada/Script/ScriptField.h
    Source/Strada/Script/ScriptFieldSerialization.cpp
    Source/Strada/Script/ScriptFieldSerialization.h
    Source/Strada/Script/ScriptGlue.h
)

set(STRADA_SERIALIZATION_SOURCES
    Source/Strada/Serialization/JsonSerialization.cpp
    Source/Strada/Serialization/JsonSerialization.h
    Source/Strada/Serialization/StructSerialization.cpp
    Source/Strada/Serialization/StructSerialization.h
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

add_library(StradaModelImport STATIC
    ThirdParty/CgltfImplementation.c
    "${ufbx_SOURCE_DIR}/ufbx.c"
    "${mikktspace_SOURCE_DIR}/mikktspace.c")
target_include_directories(StradaModelImport SYSTEM PUBLIC "${cgltf_SOURCE_DIR}" "${ufbx_SOURCE_DIR}" "${mikktspace_SOURCE_DIR}")
if(UNIX)
    target_link_libraries(StradaModelImport PRIVATE m)
endif()
strada_configure_third_party_target(StradaModelImport)

# libogg and libvorbis: the Ogg container and the Vorbis decoder with vorbisfile (third-party code, warnings disabled).
# libogg's configure step only writes the integer types of config_types.h, which <stdint.h> provides everywhere.
function(strada_write_ogg_config_types directory)
    set(INCLUDE_INTTYPES_H 0)
    set(INCLUDE_STDINT_H 1)
    set(INCLUDE_SYS_TYPES_H 0)
    set(SIZE16 int16_t)
    set(USIZE16 uint16_t)
    set(SIZE32 int32_t)
    set(USIZE32 uint32_t)
    set(SIZE64 int64_t)
    set(USIZE64 uint64_t)
    configure_file("${ogg_SOURCE_DIR}/include/ogg/config_types.h.in" "${directory}/ogg/config_types.h" @ONLY)
endfunction()
set(STRADA_OGG_GENERATED_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/OggInclude")
strada_write_ogg_config_types("${STRADA_OGG_GENERATED_INCLUDE}")
add_library(StradaVorbis STATIC
    "${ogg_SOURCE_DIR}/src/bitwise.c"
    "${ogg_SOURCE_DIR}/src/framing.c"
    "${vorbis_SOURCE_DIR}/lib/analysis.c"
    "${vorbis_SOURCE_DIR}/lib/bitrate.c"
    "${vorbis_SOURCE_DIR}/lib/block.c"
    "${vorbis_SOURCE_DIR}/lib/codebook.c"
    "${vorbis_SOURCE_DIR}/lib/envelope.c"
    "${vorbis_SOURCE_DIR}/lib/floor0.c"
    "${vorbis_SOURCE_DIR}/lib/floor1.c"
    "${vorbis_SOURCE_DIR}/lib/info.c"
    "${vorbis_SOURCE_DIR}/lib/lookup.c"
    "${vorbis_SOURCE_DIR}/lib/lpc.c"
    "${vorbis_SOURCE_DIR}/lib/lsp.c"
    "${vorbis_SOURCE_DIR}/lib/mapping0.c"
    "${vorbis_SOURCE_DIR}/lib/mdct.c"
    "${vorbis_SOURCE_DIR}/lib/psy.c"
    "${vorbis_SOURCE_DIR}/lib/registry.c"
    "${vorbis_SOURCE_DIR}/lib/res0.c"
    "${vorbis_SOURCE_DIR}/lib/sharedbook.c"
    "${vorbis_SOURCE_DIR}/lib/smallft.c"
    "${vorbis_SOURCE_DIR}/lib/synthesis.c"
    "${vorbis_SOURCE_DIR}/lib/vorbisfile.c"
    "${vorbis_SOURCE_DIR}/lib/window.c")
target_include_directories(StradaVorbis SYSTEM
    PUBLIC "${ogg_SOURCE_DIR}/include" "${STRADA_OGG_GENERATED_INCLUDE}" "${vorbis_SOURCE_DIR}/include"
    PRIVATE "${vorbis_SOURCE_DIR}/lib")
if(MSVC)
    target_compile_definitions(StradaVorbis PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
elseif(UNIX)
    # libvorbis allocates on the stack with alloca; its configure step would detect <alloca.h>.
    target_compile_definitions(StradaVorbis PRIVATE HAVE_ALLOCA_H)
    target_link_libraries(StradaVorbis PRIVATE m)
endif()
strada_configure_third_party_target(StradaVorbis)

# miniaudio, with Ogg Vorbis through its libvorbis decoding backend (third-party code, warnings disabled). The feature
# switches change struct layouts, so they are public: every translation unit including miniaudio.h agrees on them.
add_library(StradaMiniaudio STATIC ThirdParty/MiniaudioImplementation.c
    "${miniaudio_SOURCE_DIR}/extras/decoders/libvorbis/miniaudio_libvorbis.c")
target_include_directories(StradaMiniaudio SYSTEM PUBLIC "${miniaudio_SOURCE_DIR}")
target_link_libraries(StradaMiniaudio PRIVATE StradaVorbis)
target_compile_definitions(StradaMiniaudio PUBLIC MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER)
if(APPLE)
    # Core Audio linked at build time instead of loaded at runtime, which Apple's notarization requires.
    target_compile_definitions(StradaMiniaudio PUBLIC MA_NO_RUNTIME_LINKING)
    target_link_libraries(StradaMiniaudio PRIVATE "-framework CoreFoundation" "-framework CoreAudio"
        "-framework AudioToolbox")
elseif(UNIX)
    # PulseAudio, ALSA and JACK are loaded at runtime, so they are not build requirements.
    find_package(Threads REQUIRED)
    target_link_libraries(StradaMiniaudio PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
strada_configure_third_party_target(StradaMiniaudio)
strada_configure_third_party_target(reproc)
strada_configure_third_party_target(reproc++)

add_library(Strada STATIC
    Source/Strada.h
    Source/stpch.h
    ${STRADA_ASSET_SOURCES}
    ${STRADA_AUDIO_SOURCES}
    ${STRADA_CORE_SOURCES}
    ${STRADA_IMGUI_SOURCES}
    ${STRADA_MATH_SOURCES}
    ${STRADA_PHYSICS_SOURCES}
    ${STRADA_PLATFORM_SOURCES}
    ${STRADA_PROJECT_SOURCES}
    ${STRADA_RENDERER_SOURCES}
    ${STRADA_RHI_SOURCES}
    ${STRADA_SCENE_SOURCES}
    ${STRADA_SCRIPT_SOURCES}
    ${STRADA_SERIALIZATION_SOURCES}
)

target_include_directories(Strada PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
    # Structures shared between C++ and HLSL (RendererInterop.h).
    "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Include")
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
        StradaModelImport
        StradaMiniaudio
        StradaDotNetHosting
        reproc++
        ${CMAKE_DL_LIBS}
        Vulkan::Headers
        Jolt
)
if(WIN32)
    # Crash minidumps (CrashHandler); DbgHelp ships with Windows.
    target_link_libraries(Strada PRIVATE Dbghelp)
endif()

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

# The scripting API, next to the executables: ScriptEngine starts .NET from its runtimeconfig.json.
strada_add_dotnet_project(StradaScriptCore
    PROJECT "${PROJECT_SOURCE_DIR}/Strada-ScriptCore/Strada.ScriptCore.csproj"
    OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
add_dependencies(Strada StradaScriptCore)

include(StradaShaders)
strada_add_shaders(Strada
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Shaders"
    INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Include/Common.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Include/IBL.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Include/RendererInterop.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Include/ShaderInterop.h"
    SHADERS
        Fullscreen_VS:Fullscreen.hlsl:VSMain:vertex
        SolidColor_PS:Fullscreen.hlsl:PSSolidColor:pixel
        Copy_PS:Fullscreen.hlsl:PSCopy:pixel
        ImGui_VS:ImGui.hlsl:VSMain:vertex
        ImGui_PS:ImGui.hlsl:PSMain:pixel
        ForwardPBR_VS:ForwardPBR.hlsl:VSMain:vertex
        ForwardPBR_PS:ForwardPBR.hlsl:PSMain:pixel
        ForwardPBR_BlendPS:ForwardPBR.hlsl:PSMainBlend:pixel
        AmbientOcclusion_CS:AmbientOcclusion.hlsl:CSMain:compute
        AmbientOcclusionDenoise_CS:AmbientOcclusion.hlsl:CSDenoise:compute
        AmbientOcclusionComposite_CS:AmbientOcclusion.hlsl:CSComposite:compute
        BloomDownsample_CS:Bloom.hlsl:CSDownsample:compute
        BloomUpsample_CS:Bloom.hlsl:CSUpsample:compute
        Fxaa_PS:Fxaa.hlsl:PSMain:pixel
        Tonemap_PS:Tonemap.hlsl:PSMain:pixel
        Shadow_VS:Shadow.hlsl:VSMain:vertex
        Shadow_PS:Shadow.hlsl:PSMain:pixel
        Sky_VS:Sky.hlsl:VSMain:vertex
        Sky_PS:Sky.hlsl:PSMain:pixel
        EnvironmentEquirectToCube_CS:Environment.hlsl:CSEquirectToCube:compute
        EnvironmentDownsample_CS:Environment.hlsl:CSDownsample:compute
        EnvironmentIrradiance_CS:Environment.hlsl:CSIrradiance:compute
        EnvironmentPrefilter_CS:Environment.hlsl:CSPrefilter:compute
        EnvironmentBrdfLut_CS:Environment.hlsl:CSBrdfLut:compute
        EntityId_VS:EntityId.hlsl:VSMain:vertex
        EntityId_PS:EntityId.hlsl:PSMain:pixel
        OverlayLine_VS:Overlay.hlsl:VSLine:vertex
        OverlayLine_PS:Overlay.hlsl:PSLine:pixel
        Grid_PS:Overlay.hlsl:PSGrid:pixel
        Outline_PS:Outline.hlsl:PSMain:pixel
        Quad_VS:Quad.hlsl:VSMain:vertex
        Quad_PS:Quad.hlsl:PSMain:pixel
        QuadEntityId_PS:Quad.hlsl:PSEntityId:pixel
)

include(StradaResources)
strada_embed_resources(Strada
    RESOURCES
        "Fonts/Roboto-Medium.ttf=${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf"
)

strada_configure_target(Strada)
strada_add_configuration_definitions(Strada)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/Source" FILES
    ${STRADA_ASSET_SOURCES} ${STRADA_AUDIO_SOURCES} ${STRADA_CORE_SOURCES} ${STRADA_IMGUI_SOURCES} ${STRADA_MATH_SOURCES} ${STRADA_PHYSICS_SOURCES} ${STRADA_PLATFORM_SOURCES}
    ${STRADA_PROJECT_SOURCES} ${STRADA_RENDERER_SOURCES} ${STRADA_RHI_SOURCES} ${STRADA_SCENE_SOURCES} ${STRADA_SCRIPT_SOURCES} ${STRADA_SERIALIZATION_SOURCES})
set_target_properties(Strada PROPERTIES FOLDER "Engine")
