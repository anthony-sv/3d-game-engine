# Unit tests for the Strada engine library (doctest).

add_executable(StradaTests
    Source/Main.cpp
    Source/Core/ApplicationTests.cpp
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
    Source/RHI/GraphicsDeviceTests.cpp
    Source/RHI/SwapchainTests.cpp
    Source/RHI/TextureReadbackTests.cpp
    Source/TestUtilities.h
)

target_include_directories(StradaTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(StradaTests PRIVATE Strada doctest::doctest)
target_compile_definitions(StradaTests PRIVATE DOCTEST_CONFIG_SUPER_FAST_ASSERTS)

strada_configure_target(StradaTests)
set_target_properties(StradaTests PROPERTIES FOLDER "Tests")

add_test(NAME StradaTests COMMAND StradaTests --reporters=console --no-intro=true)
set_tests_properties(StradaTests PROPERTIES TIMEOUT 600)
