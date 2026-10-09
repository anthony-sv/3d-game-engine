# Unit tests for the Strada engine library (doctest).

add_executable(StradaTests
    Source/Main.cpp
    Source/Asset/AssetManagerTests.cpp
    Source/Asset/AssetRegistryTests.cpp
    Source/Asset/AssetTypeTests.cpp
    Source/Asset/GltfTestBuilder.h
    Source/Asset/MeshFactoryTests.cpp
    Source/Asset/MeshImporterTests.cpp
    Source/Asset/MeshSourceTests.cpp
    Source/Core/ApplicationTests.cpp
    Source/Core/Base64Tests.cpp
    Source/Core/BufferTests.cpp
    Source/Core/CommandLineTests.cpp
    Source/Core/EventTests.cpp
    Source/Core/FileSystemTests.cpp
    Source/Core/HashTests.cpp
    Source/Core/ImageTests.cpp
    Source/Core/InputTests.cpp
    Source/Core/KeyCodeTests.cpp
    Source/Core/LayerStackTests.cpp
    Source/Core/LogTests.cpp
    Source/Core/ResultTests.cpp
    Source/Core/UUIDTests.cpp
    Source/GpuTestUtilities.h
    Source/Math/AABBTests.cpp
    Source/Math/MathTests.cpp
    Source/Project/ProjectTests.cpp
    Source/RHI/GraphicsDeviceTests.cpp
    Source/RHI/SwapchainTests.cpp
    Source/RHI/TextureReadbackTests.cpp
    Source/Renderer/RenderTestUtilities.h
    Source/Renderer/EditorOverlayTests.cpp
    Source/Renderer/PostProcessingTests.cpp
    Source/Renderer/SceneRendererTests.cpp
    Source/Renderer/ShadowMathTests.cpp
    Source/Renderer/ShadowRenderingTests.cpp
    Source/Scene/ComponentRegistryTests.cpp
    Source/Scene/SceneCameraTests.cpp
    Source/Scene/SceneSerializerTests.cpp
    Source/Scene/SceneTests.cpp
    Source/Script/ScriptFieldTests.cpp
    Source/Serialization/JsonSerializationTests.cpp
    Source/Serialization/StructSerializationTests.cpp
    Source/TestUtilities.h
)

target_include_directories(StradaTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(StradaTests PRIVATE Strada doctest::doctest)
# A permissively licensed TrueType font from the pinned Dear ImGui sources (OFL), used by font loading tests.
target_compile_definitions(StradaTests PRIVATE
    DOCTEST_CONFIG_SUPER_FAST_ASSERTS
    STRADA_TEST_FONT_PATH="${imgui_SOURCE_DIR}/misc/fonts/Karla-Regular.ttf"
    # Golden images and other test data.
    STRADA_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/Tests/Data")

strada_configure_target(StradaTests)
set_target_properties(StradaTests PROPERTIES FOLDER "Tests")

add_test(NAME StradaTests COMMAND StradaTests --reporters=console --no-intro=true)
set_tests_properties(StradaTests PROPERTIES TIMEOUT 600)
