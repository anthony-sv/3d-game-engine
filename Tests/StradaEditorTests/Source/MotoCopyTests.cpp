#include "Audio/AudioTestUtilities.h"
#include "Core/TelemetryTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/Automation/EditorCommands.h"
#include "Editor/Automation/PlayCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/PlayMode.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Input.h"
#include "Strada/Project/ProjectCopy.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

using namespace Strada;

// M1's first step (Docs/Roadmap.md, M1): the editor opens a copy of Moto made as the start screen makes an example's (the
// project copy's rules: no Tools/, no build output or caches, no root .gitignore) and plays it with no SDK, since a game of
// graphs alone has no script project to build (Docs/Architecture.md section 15, "Moto shipped").

namespace
{
	// Input is global: the case restores it.
	class InputScope
	{
	public:
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }

		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	// The names of a directory's entries.
	std::set<std::string> ListNames(std::filesystem::path const& directory)
	{
		std::set<std::string> names;
		std::error_code error;
		for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(directory, error))
		{
			names.insert(FileSystem::PathToUtf8(entry.path().filename()));
		}
		REQUIRE_MESSAGE(!error, error.message());
		return names;
	}

	// An editor model with the editor's and play mode's commands, as the editor layer registers them for automation.
	class EditorModel
	{
	public:
		EditorModel()
		{
			m_Context.SetAssetReferenceResolver(
				[](std::string_view reference)
				{
					return AssetManager::ResolveReference(reference);
				});
			REQUIRE(RegisterEditorCommands(m_Registry, m_Operations, EditorCommandEnvironment()).IsOk());
			REQUIRE(RegisterPlayCommands(m_Registry, m_Play, m_Context,
			                             []
			                             {
											 return glm::uvec2(640, 480);
										 })
			            .IsOk());
		}

		~EditorModel() { m_Play.Stop(); }

		EditorModel(EditorModel const&) = delete;
		EditorModel& operator=(EditorModel const&) = delete;

		Json Ok(std::string_view name, Json params = Json::object())
		{
			std::optional<CommandResult> result;
			m_Registry.Execute(name, params,
			                   [&result](CommandResult value)
			                   {
								   result.emplace(std::move(value));
							   });
			REQUIRE_MESSAGE(result.has_value(), name);
			REQUIRE_MESSAGE(result->IsOk(), name, ": ", (result->IsError() ? result->GetError().Message : std::string()));
			return result->TakeValue();
		}

		EditorOperations& GetOperations() { return m_Operations; }

		Entity Find(std::string_view name)
		{
			Entity const entity = m_Context.GetScene().FindEntityByName(name);
			REQUIRE_MESSAGE(entity, name);
			return entity;
		}

		// A public variable of the graph an entity runs, as its instance holds it.
		ScriptFieldValue ReadOut(std::string_view entity, std::string const& name)
		{
			Result<ScriptFieldMap> const variables = VisualScriptEngine::GetInstanceVariables(Find(entity).GetUUID());
			REQUIRE_MESSAGE(variables.IsOk(), (variables ? std::string() : variables.GetError()));
			auto const variable = variables.GetValue().find(name);
			REQUIRE_MESSAGE(variable != variables.GetValue().end(), name);
			return variable->second;
		}

	private:
		Testing::AudioEngineScope m_Audio;
		Testing::AssetManagerScope m_Assets;
		Testing::PhysicsSystemScope m_Physics;
		Testing::ScriptRuntimeScope m_Scripting;
		Testing::TelemetryScope m_Telemetry;
		InputScope m_Input;
		EditorContext m_Context;
		EditorOperations m_Operations{m_Context};
		EditorScripts m_Scripts{m_Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode m_Play{m_Context, m_Scripts};
		CommandRegistry m_Registry;
	};
}

TEST_CASE("MotoCopy: a copy of Moto made by the project copy's rules holds its assets and project file alone, opens in the editor "
          "and plays its title over the attract mode")
{
	// The repository's Moto holds its authoring tools, which a copy leaves out.
	std::filesystem::path const sample = FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR) / "Moto";
	REQUIRE(FileSystem::IsDirectory(sample / "Tools"));
	Testing::TemporaryDirectory directory;
	std::filesystem::path const copy = directory.GetPath() / "Moto";
	// As the start screen copies an example (EditorOperations::CopyExample): the example's root .gitignore stays behind.
	ProjectCopyOptions options;
	options.ExcludedFiles.push_back(".gitignore");
	Result<ProjectCopyResult> const copied = ProjectCopy::Copy(sample, copy, options);
	REQUIRE_MESSAGE(copied.IsOk(), (copied ? std::string() : copied.GetError()));
	CHECK(copied.GetValue().Warnings.empty());
	// What Moto's sample case copies by hand, and nothing else: no Tools/, and no Scripts/, so no script project for an SDK
	// to build.
	CHECK(ListNames(copy) == std::set<std::string>{"Assets", "Moto.sproj"});

	EditorModel editor;
	REQUIRE(editor.GetOperations().OpenProject(copy / "Moto.sproj").IsOk());
	CHECK(editor.Ok("play.start")["state"] == "play");
	editor.Ok("play.pause");
	editor.Ok("play.advance", Json::object({{"frames", 3}}));
	// The main scene opens on its title over the attract mode, where the ghost rides.
	CHECK(editor.ReadOut("Menus", "Screen").GetString() == "Title");
	CHECK(editor.Find("Title Screen").GetComponent<UIRectComponent>().Visible);
	glm::vec3 const ghostAt = editor.Find("Ghost").GetComponent<TransformComponent>().Translation;
	editor.Ok("play.advance", Json::object({{"frames", 60}}));
	CHECK(editor.Find("Ghost").GetComponent<TransformComponent>().Translation != ghostAt);
	CHECK(editor.ReadOut("Menus", "Screen").GetString() == "Title");
	CHECK(editor.Ok("play.stop")["state"] == "edit");
}
