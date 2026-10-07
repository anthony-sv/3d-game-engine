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

add_library(Strada STATIC
    Source/Strada.h
    Source/stpch.h
    ${STRADA_CORE_SOURCES}
)

target_include_directories(Strada PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_precompile_headers(Strada PRIVATE Source/stpch.h)

target_link_libraries(Strada
    PUBLIC
        glm::glm
        spdlog::spdlog
        nlohmann_json::nlohmann_json
    PRIVATE
        glfw
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
)

strada_configure_target(Strada)
strada_add_configuration_definitions(Strada)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/Source" FILES ${STRADA_CORE_SOURCES})
set_target_properties(Strada PROPERTIES FOLDER "Engine")
