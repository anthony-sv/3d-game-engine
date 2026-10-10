#include "Audio/AudioTestUtilities.h"
#include "Fuzzing.h"
#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/EditorCommands.h"
#include "Editor/Automation/PlayCommands.h"
#include "Editor/Automation/ScriptCommands.h"
#include "Editor/EditorCamera.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/PlayMode.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/Input.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	// Commands the fuzzer leaves out: they replace the open project (project.create, project.open, project.close) or
	// need .NET builds (script.create makes the project's scripts need one, script.build runs it), which their own tests
	// cover with real projects.
	constexpr std::array<std::string_view, 5> ExcludedCommands = {"project.create", "project.open", "project.close", "script.create",
	                                                              "script.build"};

	// Input is global: the fuzzer presses keys through input.set.
	class InputScope
	{
	public:
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }

		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	using FileSnapshot = std::map<std::filesystem::path, std::pair<uintmax_t, std::filesystem::file_time_type>>;

	// Every file below a directory (but for the excluded ones) with its size and modification time, to tell which files
	// a run created, changed or removed.
	FileSnapshot SnapshotFiles(std::filesystem::path const& directory, std::vector<std::filesystem::path> const& excluded)
	{
		FileSnapshot files;
		std::error_code errorCode;
		for (auto it = std::filesystem::recursive_directory_iterator(directory, errorCode);
		     !errorCode && it != std::filesystem::recursive_directory_iterator(); it.increment(errorCode))
		{
			if (std::ranges::find(excluded, it->path()) != excluded.end())
			{
				it.disable_recursion_pending();
				continue;
			}
			if (it->is_regular_file(errorCode))
			{
				files[it->path()] = {it->file_size(errorCode), it->last_write_time(errorCode)};
			}
		}
		REQUIRE_FALSE(static_cast<bool>(errorCode));
		return files;
	}

	// What the generator knows about the editor, so parameters name entities, components and assets that exist.
	struct EditorState
	{
		std::vector<std::string> Entities;
		std::vector<std::string> EntityNames;
		// Entity ID -> the names of its components.
		std::map<std::string, std::vector<std::string>> EntityComponents;
		std::vector<std::string> Components;
		// Component name -> { field name: default value }.
		std::map<std::string, Json> ComponentFields;
		// Asset IDs and references (asset://<path>, builtin://<name>), all of them and by asset type.
		std::vector<std::string> Assets;
		std::map<std::string, std::vector<std::string>> AssetsByType;
		// Asset ID or reference -> the asset's path, for assets with a file.
		std::map<std::string, std::string> AssetPaths;
		std::vector<std::string> ScenePaths;
		std::vector<std::string> Folders;
		Json MaterialFields = Json::object();
		Json ProjectSettings = Json::object();
		Json SceneSettings = Json::object();
	};

	// Values for a string parameter: ones that name what exists or would be accepted, and edge cases to be refused.
	struct StringChoices
	{
		std::vector<std::string> Good;
		std::vector<std::string> Edge;
	};

	// Builds parameters from a command's schema. Most name what exists, so commands get past their lookups and do their
	// work; one value in ten breaks the schema, and a quarter of the strings are edge cases, so validation is exercised
	// too. Ranges of parameters that only make commands slower (frames run at once, game seconds) are narrowed so rounds
	// stay fast.
	class ParameterGenerator
	{
	public:
		ParameterGenerator(uint64_t seed, EditorState const& state, std::filesystem::path outside, std::vector<std::string> importFiles)
			: m_Random(seed),
			  m_Mutator(seed ^ 0xA5A5A5A5A5A5A5A5ull),
			  m_State(state),
			  m_Outside(std::move(outside)),
			  m_ImportFiles(std::move(importFiles))
		{
		}

		Json Generate(std::string_view command, Json const& schema)
		{
			m_Command = command;
			if (schema.is_null())
			{
				// Commands without parameters must refuse any.
				return m_Random.Pick(10) == 0 ? Json::object({{"unexpected", 1}}) : Json::object();
			}
			return Value(schema, "", Json::object(), 0);
		}

	private:
		Json Value(Json const& schema, std::string_view property, Json const& siblings, int depth)
		{
			if (depth > 0 && m_Random.Pick(10) == 0)
			{
				return InvalidValue();
			}
			std::string const type = PickType(schema);
			if (type == "object")
			{
				return ObjectValue(schema, property, siblings, depth);
			}
			if (type == "array")
			{
				return ArrayValue(schema, property, depth);
			}
			if (type == "string")
			{
				return StringValue(schema, property, siblings);
			}
			if (type == "integer")
			{
				return IntegerValue(schema, property);
			}
			if (type == "number")
			{
				return NumberValue(schema, property);
			}
			if (type == "boolean")
			{
				return m_Random.Pick(2) == 0;
			}
			if (type == "null")
			{
				return Json();
			}
			return InvalidValue();
		}

		std::string PickType(Json const& schema)
		{
			auto const type = schema.find("type");
			if (type == schema.end())
			{
				return {};
			}
			if (type->is_array())
			{
				return (*type)[m_Random.Pick(type->size())].get<std::string>();
			}
			return type->get<std::string>();
		}

		Json ObjectValue(Json const& schema, std::string_view property, Json const& siblings, int depth)
		{
			if (std::optional<Json> patch = Patch(property, siblings))
			{
				return *patch;
			}
			Json object = Json::object();
			std::set<std::string> required;
			if (auto const list = schema.find("required"); list != schema.end())
			{
				for (Json const& name : *list)
				{
					required.insert(name.get<std::string>());
				}
			}
			if (auto const properties = schema.find("properties"); properties != schema.end())
			{
				for (auto const& [name, propertySchema] : properties->items())
				{
					// Required properties are left out now and then, to exercise validation.
					bool const include = required.contains(name) ? m_Random.Pick(20) != 0 : m_Random.Pick(2) == 0;
					if (include)
					{
						object[name] = Value(propertySchema, name, object, depth + 1);
					}
				}
			}
			if (auto const additional = schema.find("additionalProperties"); additional != schema.end() && *additional != false)
			{
				for (size_t count = m_Random.Pick(3); count > 0; count--)
				{
					object[Key(property)] = Value(additional->is_object() ? *additional : Json::object(), property, object, depth + 1);
				}
			}
			return object;
		}

		Json ArrayValue(Json const& schema, std::string_view property, int depth)
		{
			uint64_t const minimum = schema.value("minItems", uint64_t(0));
			uint64_t const maximum = std::min(schema.value("maxItems", uint64_t(4)), minimum + 4);
			Json const items = schema.value("items", Json::object());
			Json array = Json::array();
			for (uint64_t count = minimum + m_Random.Pick(maximum - minimum + 1); count > 0; count--)
			{
				array.push_back(Value(items, property, Json::object(), depth + 1));
			}
			return array;
		}

		Json StringValue(Json const& schema, std::string_view property, Json const& siblings)
		{
			if (auto const values = schema.find("enum"); values != schema.end() && m_Random.Pick(10) != 0)
			{
				return (*values)[m_Random.Pick(values->size())];
			}
			StringChoices const choices = Strings(property, siblings);
			bool const good = !choices.Good.empty() && m_Random.Pick(4) != 0;
			return PickFrom(good ? choices.Good : choices.Edge);
		}

		Json IntegerValue(Json const& schema, std::string_view property)
		{
			double const minimum = schema.value("minimum", -1000.0);
			double maximum = schema.value("maximum", 1000.0);
			if (property == "frames")
			{
				maximum = std::min(maximum, 5.0);
			}
			std::array<double, 6> const candidates = {minimum, minimum + 1.0, maximum,
			                                          0.0,     1.0,           minimum + static_cast<double>(m_Random.Pick(100))};
			return static_cast<int64_t>(std::clamp(candidates[m_Random.Pick(candidates.size())], minimum, maximum));
		}

		Json NumberValue(Json const& schema, std::string_view property)
		{
			double minimum = schema.value("minimum", -1.0e30);
			double maximum = schema.value("maximum", 1.0e30);
			if (property == "timeout")
			{
				maximum = std::min(maximum, 0.5);
			}
			if (property == "timestep")
			{
				minimum = std::max(minimum, 0.01);
			}
			std::array<double, 8> const candidates = {minimum, maximum, 0.0, 0.5, -1.0, 1.0e6, -1.0e6, (minimum + maximum) / 2.0};
			return std::clamp(candidates[m_Random.Pick(candidates.size())], minimum, maximum);
		}

		// A value of any kind, which the schema most likely rejects.
		Json InvalidValue()
		{
			std::array<Json, 11> const values = {Json(),
			                                     Json(true),
			                                     Json(-1),
			                                     Json(1.0e308),
			                                     Json(1.5),
			                                     Json(""),
			                                     Json("text"),
			                                     Json::array(),
			                                     Json::array({1, 2, 3}),
			                                     Json::object(),
			                                     Json::object({{"unexpected", 1}})};
			return values[m_Random.Pick(values.size())];
		}

		// Patches of component, material, scene and project fields: real ones, damaged now and then.
		std::optional<Json> Patch(std::string_view property, Json const& siblings)
		{
			if (property == "fields")
			{
				if (m_Command.starts_with("material."))
				{
					return Damage(m_State.MaterialFields);
				}
				auto const component = siblings.find("component");
				std::string const name =
					component != siblings.end() && component->is_string() ? component->get<std::string>() : PickFrom(m_State.Components);
				auto const fields = m_State.ComponentFields.find(name);
				return Damage(fields != m_State.ComponentFields.end() ? fields->second : Json::object());
			}
			if (property == "components")
			{
				Json components = Json::object();
				for (size_t count = 1 + m_Random.Pick(2); count > 0; count--)
				{
					std::string const name = PickFrom(m_State.Components);
					auto const fields = m_State.ComponentFields.find(name);
					components[name] = Damage(fields != m_State.ComponentFields.end() ? fields->second : Json::object());
				}
				return components;
			}
			if (property == "settings")
			{
				return Damage(m_Command == "project.settings" ? m_State.ProjectSettings : m_State.SceneSettings);
			}
			return std::nullopt;
		}

		// Some of the members of a document, damaged half the time.
		Json Damage(Json const& document)
		{
			Json patch = Json::object();
			for (auto const& [name, value] : document.items())
			{
				if (m_Random.Pick(3) == 0)
				{
					patch[name] = value;
				}
			}
			if (!patch.empty() && m_Random.Pick(2) == 0)
			{
				m_Mutator.Mutate(patch);
			}
			return patch;
		}

		std::string Key(std::string_view property)
		{
			if (property == "keys")
			{
				return PickFrom(KeyNames);
			}
			if (property == "mouseButtons")
			{
				return PickFrom(MouseButtonNames);
			}
			return m_Random.Pick(2) == 0 ? PickFrom(m_State.Components) : PickFrom(GenericStrings);
		}

		StringChoices Strings(std::string_view property, Json const& siblings)
		{
			std::vector<std::string> const invalidIds = {"", "0", "not-an-id", "18446744073709551615", "-1"};
			std::vector<std::string> const invalidAssets = {"", "0", "asset://Missing.smat", "builtin://Nothing", "asset://../Escape.smat"};
			std::vector<std::string> const invalidFolders = {"", ".", "..", "../Escape", "../../Escape", "Materials/../..", "C:Escape"};
			if (property == "entity" || property == "parent" || property == "primary" || property == "entities")
			{
				return {m_State.Entities, invalidIds};
			}
			if (property == "component")
			{
				std::vector<std::string> const unknown = {"Nope", "", "transform"};
				// The components of the entity the command names (except for adding one).
				auto const entity = siblings.find("entity");
				if (entity != siblings.end() && entity->is_string() && m_Command != "component.add")
				{
					auto const components = m_State.EntityComponents.find(entity->get<std::string>());
					if (components != m_State.EntityComponents.end())
					{
						return {components->second, unknown};
					}
				}
				return {m_State.Components, unknown};
			}
			if (property == "asset")
			{
				return {m_State.Assets, invalidAssets};
			}
			if (property == "material" || property == "prefab")
			{
				auto const typed = m_State.AssetsByType.find(property == "material" ? "Material" : "Prefab");
				return {typed != m_State.AssetsByType.end() ? typed->second : std::vector<std::string>(), invalidAssets};
			}
			if (property == "scene")
			{
				return {m_State.ScenePaths, {"Missing.sscene", "../Escape.sscene", ""}};
			}
			if (property == "folder")
			{
				return {m_State.Folders, invalidFolders};
			}
			if (property == "newFolder")
			{
				return {{"New", "New/Deeper", "Moved", "Materials/Sub"}, invalidFolders};
			}
			if (property == "files")
			{
				return {{m_ImportFiles[0], m_ImportFiles[1]}, {m_ImportFiles[2], "Relative.png", ""}};
			}
			if (property == "path")
			{
				return Paths(siblings);
			}
			if (property == "name")
			{
				std::vector<std::string> names = m_State.EntityNames;
				names.push_back("Fuzz");
				return {names, GenericStrings};
			}
			return {{"Fuzz"}, GenericStrings};
		}

		// Paths the command at hand accepts (in the asset directory with the right extension, or absolute where that is
		// allowed), and ones it must refuse.
		StringChoices Paths(Json const& siblings)
		{
			std::vector<std::string> const refused = {"../Escape.sscene",       "../../Escape.smat", "../../../Escape.sprefab",
			                                          "../../../../Escape.png", "C:Escape.sscene",   "Fuzz.txt"};
			std::string const outside = FileSystem::PathToUtf8(m_Outside / "Fuzz.sscene");
			if (m_Command == "scene.open")
			{
				std::vector<std::string> scenes = m_State.ScenePaths;
				scenes.push_back(outside);
				return {scenes, refused};
			}
			if (m_Command == "scene.save")
			{
				return {{"Scenes/Fuzz.sscene", "Fuzz.sscene", outside}, refused};
			}
			if (m_Command == "material.create")
			{
				return {{"Materials/Fuzz.smat", "Fuzz.smat"}, refused};
			}
			if (m_Command == "prefab.create")
			{
				return {{"Prefabs/Fuzz.sprefab", "Fuzz.sprefab"}, refused};
			}
			// asset.move: a new place for the asset, under the same name.
			auto const asset = siblings.find("asset");
			if (asset != siblings.end() && asset->is_string())
			{
				if (auto const path = m_State.AssetPaths.find(asset->get<std::string>()); path != m_State.AssetPaths.end())
				{
					std::string const name = path->second.substr(path->second.rfind('/') + 1);
					return {{"Moved/" + name, "Moved/Deeper/" + name, name}, refused};
				}
			}
			return {{}, refused};
		}

		template<typename T>
		T const& PickFrom(std::vector<T> const& values)
		{
			REQUIRE_FALSE(values.empty());
			return values[m_Random.Pick(values.size())];
		}

		static inline std::vector<std::string> const GenericStrings = {
			"", "x", "Fuzz", "  ", "名前", std::string(300, 'a'), std::string("nul\0inside", 10), "18446744073709551615", "../.."};
		static inline std::vector<std::string> const KeyNames = {"A", "Space", "Escape", "Left", "F12", "NotAKey", ""};
		static inline std::vector<std::string> const MouseButtonNames = {"Left", "Right", "Middle", "Button8", ""};

		Testing::FuzzRandom m_Random;
		Testing::DocumentMutator m_Mutator;
		EditorState const& m_State;
		std::filesystem::path m_Outside;
		std::vector<std::string> m_ImportFiles;
		std::string_view m_Command;
	};

	// A project with something of everything, open in an editor model with the commands the editor layer registers.
	struct FuzzFixture
	{
		Testing::PhysicsSystemScope Physics;
		Testing::AudioEngineScope Audio;
		Testing::AssetManagerScope Assets;
		InputScope Inputs;
		Testing::TemporaryDirectory Directory;
		// Nested, so paths that climb out of the project with "../" still land in the temporary directory.
		std::filesystem::path ProjectDirectory = Directory.GetPath() / "One" / "Two" / "Project";
		// Where absolute scene paths point: scenes may be saved anywhere.
		std::filesystem::path Outside = Directory.GetPath() / "Outside";
		std::filesystem::path Imports = Directory.GetPath() / "Imports";
		EditorContext Context;
		EditorOperations Operations{Context};
		EditorScripts Scripts{Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode Play{Context, Scripts};
		EditorCamera Camera;
		CommandRegistry Registry;
		bool QuitRequested = false;

		FuzzFixture()
		{
			EditorCommandEnvironment environment;
			environment.RequestQuit = [this]
			{
				QuitRequested = true;
			};
			environment.ViewportCamera = [this]() -> EditorCamera&
			{
				return Camera;
			};
			environment.ViewportStatistics = []() -> SceneRendererStatistics const*
			{
				return nullptr;
			};
			REQUIRE(RegisterEditorCommands(Registry, Operations, std::move(environment)).IsOk());
			REQUIRE(RegisterPlayCommands(Registry, Play, Context,
			                             []
			                             {
											 return glm::uvec2(640, 480);
										 })
			            .IsOk());
			REQUIRE(RegisterScriptCommands(Registry, Scripts, Context).IsOk());

			REQUIRE(FileSystem::CreateDirectories(Outside).IsOk());
			REQUIRE(FileSystem::CreateDirectories(Imports).IsOk());
			REQUIRE(Image(2, 2, 4).WritePNG(Imports / "Texture.png").IsOk());
			std::array<float, 64> const silence{};
			REQUIRE(FileSystem::WriteBinaryFile(Imports / "Sound.wav", Testing::MakeWav(silence).GetSpan()).IsOk());
			REQUIRE(Operations.CreateProject(ProjectDirectory, "Fuzz", Scene("Main")).IsOk());

			// An entity for each component, in one hierarchy, a material, a prefab, a folder and imported files.
			std::string parent;
			Json const types = Ok("component.types");
			for (Json const& type : types["components"])
			{
				std::string const name = type["Name"].get<std::string>();
				if (type["Core"] == true)
				{
					continue;
				}
				std::string const id =
					Ok("entity.create", Json::object({{"name", name}, {"components", {{name, Json::object()}}}}))["id"].get<std::string>();
				if (parent.empty())
				{
					parent = id;
				}
				else
				{
					Ok("entity.reparent", Json::object({{"entity", id}, {"parent", parent}}));
				}
			}
			Ok("material.create", Json::object({{"path", "Materials/Painted.smat"}, {"fields", {{"Roughness", 0.25}}}}));
			Ok("prefab.create", Json::object({{"entity", parent}, {"path", "Prefabs/Everything.sprefab"}}));
			Ok("asset.create-folder", Json::object({{"folder", "Empty"}}));
			Ok("asset.import", Json::object({{"files", Json::array({ImportFiles()[0], ImportFiles()[1]})}, {"folder", "Imported"}}));
			Ok("scene.save");
		}

		// The files asset.import is given: two that exist and one that does not.
		std::vector<std::string> ImportFiles() const
		{
			return {FileSystem::PathToUtf8(Imports / "Texture.png"), FileSystem::PathToUtf8(Imports / "Sound.wav"),
			        FileSystem::PathToUtf8(Imports / "Missing.png")};
		}

		// Runs a synchronous command.
		CommandResult Run(std::string_view name, Json params = Json::object())
		{
			std::optional<CommandResult> result;
			Registry.Execute(name, params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			REQUIRE(result.has_value());
			return std::move(*result);
		}

		Json Ok(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE_MESSAGE(result.IsOk(), (result.IsError() ? result.GetError().Message : std::string()));
			return result.TakeValue();
		}

		// Puts entities with components back into a scene the fuzzer emptied: an instance of the prefab of everything, or
		// a few entities when the prefab was deleted or moved.
		void Replenish()
		{
			if (Run("prefab.instantiate", Json::object({{"prefab", "asset://Prefabs/Everything.sprefab"}})).IsOk())
			{
				return;
			}
			for (char const* const component : {"RigidBody", "PointLight", "AudioSource"})
			{
				static_cast<void>(Run("entity.create", Json::object({{"name", component}, {"components", {{component, Json::object()}}}})));
			}
		}

		// One editor frame.
		void Tick()
		{
			Scripts.Update();
			Play.Update(Timestep(1.0f / 60.0f));
		}

		// The edited scene (or the running copy) saves and loads again.
		void CheckSceneLoads()
		{
			Json const dump = Ok("scene.dump");
			Result<Ref<Scene>> loaded = SceneSerializer::Deserialize(dump, AssetManager::CreateDeserializationContext());
			CHECK_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
		}

		// Undoing every step, then redoing them all, brings the scene back (once what was undone before is redone).
		bool CheckHistory()
		{
			while (Run("editor.redo").IsOk())
			{
			}
			Json const edited = Ok("scene.dump");
			int undone = 0;
			while (Run("editor.undo").IsOk())
			{
				undone++;
			}
			CheckSceneLoads();
			int redone = 0;
			while (Run("editor.redo").IsOk())
			{
				redone++;
			}
			Json const restored = Ok("scene.dump");
			CHECK(redone == undone);
			CHECK(restored == edited);
			return redone == undone && restored == edited;
		}

		EditorState ReadState()
		{
			EditorState state;
			std::vector<Json> nodes = Ok("scene.hierarchy")["entities"].get<std::vector<Json>>();
			while (!nodes.empty())
			{
				Json const node = std::move(nodes.back());
				nodes.pop_back();
				std::string const id = node["id"].get<std::string>();
				state.Entities.push_back(id);
				state.EntityNames.push_back(node["name"].get<std::string>());
				state.EntityComponents[id] = node["components"].get<std::vector<std::string>>();
				for (Json const& child : node["children"])
				{
					nodes.push_back(child);
				}
			}
			Json const types = Ok("component.types");
			for (Json const& type : types["components"])
			{
				Json fields = Json::object();
				for (Json const& field : type["Fields"])
				{
					fields[field["Name"].get<std::string>()] = field["Default"];
				}
				state.Components.push_back(type["Name"].get<std::string>());
				state.ComponentFields[type["Name"].get<std::string>()] = std::move(fields);
			}
			std::set<std::string> folders;
			Json const assets = Ok("asset.list", Json::object({{"builtIn", true}}));
			for (Json const& asset : assets["assets"])
			{
				std::vector<std::string> names = {asset["id"].is_string() ? asset["id"].get<std::string>() : asset["id"].dump()};
				if (asset["reference"].is_string())
				{
					names.push_back(asset["reference"].get<std::string>());
				}
				std::string const type = asset["type"].get<std::string>();
				for (std::string const& name : names)
				{
					state.Assets.push_back(name);
					state.AssetsByType[type].push_back(name);
				}
				if (!asset["path"].is_string())
				{
					continue;
				}
				std::string const path = asset["path"].get<std::string>();
				for (std::string const& name : names)
				{
					state.AssetPaths[name] = path;
				}
				if (type == "Scene")
				{
					state.ScenePaths.push_back(path);
				}
				if (type == "Material" && state.MaterialFields.empty())
				{
					CommandResult material = Run("material.get", Json::object({{"material", names.front()}}));
					if (material.IsOk())
					{
						state.MaterialFields = material.GetValue()["fields"];
					}
				}
				for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1))
				{
					folders.insert(path.substr(0, slash));
				}
			}
			state.Folders.assign(folders.begin(), folders.end());
			state.ProjectSettings = Ok("project.info")["settings"];
			state.SceneSettings = Ok("scene.settings")["settings"];
			return state;
		}
	};
}

TEST_CASE("Automation: commands with any parameters answer once, keep the editor consistent and write only in the project")
{
	FuzzFixture fixture;
	std::vector<std::filesystem::path> const writable = {fixture.ProjectDirectory, fixture.Outside};
	FileSnapshot const filesBefore = SnapshotFiles(fixture.Directory.GetPath(), writable);

	std::vector<CommandDefinition const*> commands;
	for (CommandDefinition const* command : fixture.Registry.GetCommands())
	{
		if (std::ranges::find(ExcludedCommands, command->Name) == ExcludedCommands.end())
		{
			commands.push_back(command);
		}
	}

	EditorState state = fixture.ReadState();
	ParameterGenerator generator(7, state, fixture.Outside, fixture.ImportFiles());
	Testing::FuzzRandom random(11);
	std::set<int> unanswered;
	// Command name -> how often it succeeded and failed.
	std::map<std::string, std::pair<int, int>> outcomes;
	// The requests since the last history check, listed when it fails, to find the step that breaks the history.
	std::vector<std::string> recent;
	for (int round = 0, rounds = Testing::GetFuzzRounds(1500); round < rounds; round++)
	{
		if (round % 5 == 0)
		{
			state = fixture.ReadState();
			if (state.Entities.size() < 3)
			{
				fixture.Replenish();
				state = fixture.ReadState();
			}
		}
		CommandDefinition const& command = *commands[random.Pick(commands.size())];
		Json const params = generator.Generate(command.Name, command.Parameters);
		std::string const request = command.Name + " " + params.dump();
		CAPTURE(round);
		CAPTURE(request);
		unanswered.insert(round);
		fixture.Registry.Execute(command.Name, params,
		                         [&unanswered, &outcomes, &recent, name = command.Name, round, request](CommandResult result)
		                         {
									 CHECK_MESSAGE(unanswered.erase(round) == 1, "answered twice: " << request);
									 recent.push_back(request + (result.IsOk() ? " -> ok" : " -> " + result.GetError().Message));
									 if (result.IsOk())
									 {
										 outcomes[name].first++;
										 return;
									 }
									 outcomes[name].second++;
									 // InternalError means a handler threw: it relied on something it did not check.
									 CHECK_MESSAGE(result.GetError().Code != AutomationErrorCode::InternalError,
			                                       request << ": " << result.GetError().Message);
									 CHECK_FALSE(result.GetError().Message.empty());
								 });
		if (!command.IsAsync())
		{
			CHECK_MESSAGE(!unanswered.contains(round), "a synchronous command did not answer");
		}
		fixture.Tick();
		if (round % 50 == 49)
		{
			fixture.CheckSceneLoads();
		}
		if (round % 100 == 99)
		{
			if (!fixture.CheckHistory())
			{
				for (std::string const& line : recent)
				{
					MESSAGE(line);
				}
			}
			recent.clear();
		}
	}

	// Every command answers once playing stops.
	if (fixture.Play.IsPlaying())
	{
		fixture.Ok("play.stop");
	}
	for (int tick = 0; tick < 100 && !unanswered.empty(); tick++)
	{
		fixture.Tick();
	}
	CHECK(unanswered.empty());
	fixture.CheckSceneLoads();
	fixture.CheckHistory();

	// The parameters reach the commands' work, not only their validation: most commands succeed at least once (a few
	// cannot without a window, a GPU or script classes).
	size_t const exercised = static_cast<size_t>(std::ranges::count_if(outcomes,
	                                                                   [](auto const& outcome)
	                                                                   {
																		   return outcome.second.first > 0;
																	   }));
	CHECK_MESSAGE(exercised * 4 >= commands.size() * 3, exercised << " of " << commands.size() << " commands succeeded");

	// Nothing outside the project changed, but where absolute scene paths pointed.
	CHECK(SnapshotFiles(fixture.Directory.GetPath(), writable) == filesBefore);
}
