#pragma once

#include "Core/UserDataTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Platform/Process.h"
#include "Strada/Project/ProjectTemplate.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptField.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What the game player's tests share (RuntimeTests.cpp, SampleTests.cpp): running the player and other programs with a
// per-user root of their own, reading their output, a scene the player shows, and copies of the repository's projects
// and templates.
namespace Strada::RuntimeTesting
{
	// Runs a program to completion with the per-user root given, where it writes its log, its crash reports and its game's
	// data: never into this user's folders.
	inline ProcessResult RunProgram(std::filesystem::path const& program, std::vector<std::string> arguments,
	                                Testing::IsolatedUserRoot const& userRoot)
	{
		ProcessSpecification specification;
		specification.Executable = program;
		specification.Arguments = std::move(arguments);
		specification.Environment = userRoot.GetEnvironment();
		specification.Timeout = std::chrono::minutes(2);
		Result<ProcessResult> result = Process::Run(specification);
		REQUIRE_MESSAGE(result.IsOk(), (result ? std::string() : result.GetError()));
		REQUIRE_FALSE(result.GetValue().TimedOut);
		return result.TakeValue();
	}

	// Runs a program to completion with a per-user root of its own, removed after the run.
	inline ProcessResult RunProgram(std::filesystem::path const& program, std::vector<std::string> arguments)
	{
		Testing::IsolatedUserRoot const userRoot;
		return RunProgram(program, std::move(arguments), userRoot);
	}

	// Runs the player of this build to completion, as RunProgram does.
	inline ProcessResult RunPlayer(std::vector<std::string> arguments, Testing::IsolatedUserRoot const& userRoot)
	{
		return RunProgram(FileSystem::PathFromUtf8(STRADA_RUNTIME_PATH), std::move(arguments), userRoot);
	}

	inline ProcessResult RunPlayer(std::vector<std::string> arguments)
	{
		return RunProgram(FileSystem::PathFromUtf8(STRADA_RUNTIME_PATH), std::move(arguments));
	}

	inline bool Contains(std::string_view text, std::string_view part)
	{
		return text.find(part) != std::string_view::npos;
	}

	// The test run's summary in a player's output ("Tests passed: <checks> checks in <frames> frames"); empty without one.
	inline std::string_view TestSummary(std::string_view output)
	{
		size_t const start = output.find("Tests passed: ");
		if (start == std::string_view::npos)
		{
			return {};
		}
		size_t const end = output.find_first_of("\r\n", start);
		return output.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
	}

	// A scene seen by a primary camera against a red sky; with fields, an entity runs Strada.Tests.SelfTest with them.
	inline Ref<Scene> MakeScene(std::string const& name, std::optional<ScriptFieldMap> selfTest)
	{
		Ref<Scene> scene = CreateRef<Scene>(name);
		scene->CreateEntity("Camera").AddComponent<CameraComponent>();
		scene->CreateEntity("Sky").AddComponent<SkyLightComponent>().AmbientColor = glm::vec3(1.0f, 0.0f, 0.0f);
		scene->GetSettings().Renderer.Tonemapper = TonemapOperator::None;
		scene->GetSettings().Renderer.Dithering = false;
		if (selfTest)
		{
			ScriptComponent& script = scene->CreateEntity("Tester").AddComponent<ScriptComponent>();
			script.ClassName = "Strada.Tests.SelfTest";
			script.Fields = std::move(*selfTest);
		}
		return scene;
	}

	inline std::string Native(std::filesystem::path const& path)
	{
		return FileSystem::PathToNativeUtf8(path);
	}

	// A copy of Projects/<name> (its project file named after it) in the directory. A project with C# scripts
	// (Scripts/<name>.csproj, the rule the engine tests' ProjectScriptsScope follows) gets the scripts CMake built for it
	// (bin/<name>) where the editor's build puts them; a project made of visual scripts alone has none to copy. Returns the
	// copy's project file.
	inline std::filesystem::path CopyRepositoryProject(std::string const& name, std::filesystem::path const& directory)
	{
		std::filesystem::path const source = FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR) / FileSystem::PathFromUtf8(name);
		std::filesystem::path const project = directory / FileSystem::PathFromUtf8(name);
		std::filesystem::path const file = FileSystem::PathFromUtf8(name + ".sproj");
		REQUIRE(FileSystem::CopyDirectory(source / "Assets", project / "Assets").IsOk());
		REQUIRE(FileSystem::Copy(source / file, project / file, false).IsOk());
		if (FileSystem::Exists(source / "Scripts" / FileSystem::PathFromUtf8(name + ".csproj")))
		{
			REQUIRE(FileSystem::CopyDirectory(FileSystem::GetExecutableDirectory() / FileSystem::PathFromUtf8(name),
			                                  project / "Scripts" / "Binaries")
			            .IsOk());
		}
		return project / file;
	}

	// A new project in the directory made from a template of the repository's Templates, as the editor makes one (a
	// default-scene template starts with startScene); returns its project file.
	inline std::filesystem::path InstantiateRepositoryTemplate(std::string_view id, std::filesystem::path const& directory,
	                                                           std::string const& name, Scene const* startScene = nullptr)
	{
		Result<ProjectTemplate> loaded =
			ProjectTemplate::Load(FileSystem::PathFromUtf8(STRADA_TEMPLATES_DIR) / FileSystem::PathFromUtf8(id));
		REQUIRE_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
		Result<std::filesystem::path> created =
			ProjectTemplate::Instantiate(loaded.GetValue(), directory, name, TemplateOptions{}, startScene);
		REQUIRE_MESSAGE(created.IsOk(), (created ? std::string() : created.GetError()));
		return created.TakeValue();
	}
}
