#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/PlayCommands.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/PlayMode.h"
#include "Editor/Viewport/ViewportOverlays.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/InputActionMap.h"
#include "Strada/Asset/InputActionsAsset.h"
#include "Strada/Asset/InputBindingProfile.h"
#include "Strada/Asset/InputRebinding.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Buffer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/InputActions.h"
#include "Strada/Core/InputCapture.h"
#include "Strada/Core/Log.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	// Input is global: tests that set it restore it.
	class InputScope
	{
	public:
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }

		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	// The editor model with play mode and its commands, as the editor layer sets them up.
	struct PlayFixture
	{
		EditorContext Context;
		EditorOperations Operations{Context};
		EditorScripts Scripts{Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode Play{Context, Scripts};
		CommandRegistry Registry;

		PlayFixture()
		{
			REQUIRE(RegisterPlayCommands(Registry, Play, Context,
			                             []
			                             {
											 return glm::uvec2(640, 480);
										 })
			            .IsOk());
		}

		UUID Create(std::string name, Json components = Json::object())
		{
			EntityCreateInfo info;
			info.Name = std::move(name);
			info.Components = std::move(components);
			Result<UUID> entity = Operations.CreateEntity(info);
			REQUIRE(entity.IsOk());
			return entity.GetValue();
		}

		// Commands complete at once here: scripts never need building without a project.
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

		AutomationErrorCode Fails(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE(result.IsError());
			return result.GetError().Code;
		}
	};

	// A project open in the editor model with play mode, whose asset files the steps under test edit.
	struct ProjectPlayFixture
	{
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		EditorScripts Scripts{Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode Play{Context, Scripts};

		ProjectPlayFixture() { REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk()); }

		UUID Create(std::string name, Json components = Json::object())
		{
			EntityCreateInfo info;
			info.Name = std::move(name);
			info.Components = std::move(components);
			Result<UUID> entity = Operations.CreateEntity(info);
			REQUIRE(entity.IsOk());
			return entity.GetValue();
		}

		AssetHandle CreateGraph(std::string_view path)
		{
			Result<AssetHandle> const created = Operations.CreateVisualScript(path);
			REQUIRE(created.IsOk());
			return created.GetValue();
		}

		AssetHandle CreateMaterial(std::string_view path)
		{
			Result<AssetHandle> const created = Operations.CreateMaterial(path);
			REQUIRE(created.IsOk());
			return created.GetValue();
		}

		std::string ReadFile(AssetHandle asset) const
		{
			Result<Buffer> const bytes = FileSystem::ReadBinaryFile(AssetManager::GetAbsolutePath(asset));
			REQUIRE(bytes.IsOk());
			return std::string(bytes.GetValue().As<char>(), static_cast<size_t>(bytes.GetValue().GetSize()));
		}

		// Gives the graph another description, committed as a graph window commits its edits.
		void Describe(AssetHandle graph, std::string const& description)
		{
			Ref<VisualScriptAsset> const asset = AssetManager::GetAsset<VisualScriptAsset>(graph);
			REQUIRE(asset != nullptr);
			VisualScriptDocument document = asset->GetDocument();
			document.Description = description;
			Result<void> const edited =
				Operations.EditVisualScript(graph, CreateRef<VisualScriptDocument const>(std::move(document)), "Describe Graph");
			REQUIRE_MESSAGE(edited.IsOk(), (edited ? std::string() : edited.GetError()));
		}

		void SetRoughness(AssetHandle material, double roughness, uint64_t mergeKey = 0)
		{
			REQUIRE(Operations.SetMaterialFields(material, Json::object({{"Roughness", roughness}}), mergeKey).IsOk());
		}

		size_t GetUndoCount() const { return Context.GetHistory().GetUndoCount(); }
	};

	// A scene whose SceneSwitcher loads nextScene on L and quits on Q.
	void WriteSwitcherScene(std::filesystem::path const& path, std::string const& name, std::string const& nextScene)
	{
		Scene scene(name);
		Entity switcher = scene.CreateEntity("Switcher");
		ScriptComponent& script = switcher.AddComponent<ScriptComponent>();
		script.ClassName = "Strada.Tests.SceneSwitcher";
		script.Fields["NextScene"] = ScriptFieldValue::FromString(nextScene);
		REQUIRE(SceneSerializer::SaveToFile(scene, path).IsOk());
	}

	void AdvanceWithKey(PlayMode& play, KeyCode key)
	{
		Input::SetKeyState(key, true);
		play.Advance(1, Timestep(1.0f / 60.0f));
		Input::SetKeyState(key, false);
	}

	// Records what play mode tells it, with the running scene's frame and pause at each call, and holds play mode back as
	// a test sets it to: pauses after a frame, refuses frames, keeps the scene paused on a resume.
	class RecordingObserver final : public PlayModeObserver
	{
	public:
		struct Call
		{
			std::string Kind;
			uint64_t Frame = 0;
			float Seconds = 0.0f;
			bool Paused = false;

			bool operator==(Call const& other) const = default;
		};

		explicit RecordingObserver(EditorContext& context)
			: m_Context(context)
		{
		}

		void OnPlayStarting(EditorPlayState state, PlayStartOptions const& options) override
		{
			// The scene does not run yet: the context still shows the edited one.
			StartedPlaying = m_Context.IsPlaying();
			Started = std::make_pair(state, options);
			Calls.push_back({"Starting", 0, 0.0f, false});
		}

		bool OnPlayFrame(Timestep timestep) override
		{
			Scene const& scene = m_Context.GetScene();
			Calls.push_back({"Frame", scene.GetRuntimeFrame(), timestep.GetSeconds(), scene.IsPaused()});
			return PauseAfterFrame == scene.GetRuntimeFrame();
		}

		void OnPlayStopping() override { Calls.push_back({"Stopping", m_Context.GetScene().GetRuntimeFrame(), 0.0f, false}); }

		bool OnPlayResuming() override
		{
			Calls.push_back({"Resuming", m_Context.GetScene().GetRuntimeFrame(), 0.0f, m_Context.GetScene().IsPaused()});
			// Continuing ends the refusal, unless an execution stops again.
			if (!StopAgainOnResume)
			{
				Refusal.reset();
			}
			return StopAgainOnResume;
		}

		void Refresh() override { Refreshes++; }
		std::optional<std::string> GetFrameStepRefusal() const override { return Refusal; }
		PlayDebugSummary GetDebugSummary() const override { return Summary; }

		VisualScriptStop const* FindStop(uint64_t) const override { return nullptr; }

		std::vector<Call> Calls;
		std::optional<std::pair<EditorPlayState, PlayStartOptions>> Started;
		bool StartedPlaying = true;
		std::optional<uint64_t> PauseAfterFrame;
		bool StopAgainOnResume = false;
		std::optional<std::string> Refusal;
		PlayDebugSummary Summary;
		uint32_t Refreshes = 0;

	private:
		EditorContext& m_Context;
	};

	// Declared right after an observer: stops playing and clears the observer before it is destroyed, also when a failed
	// assertion unwinds the case (the fixture's play mode, destroyed after it, would tell a destroyed observer otherwise).
	class ObserverGuard final
	{
	public:
		explicit ObserverGuard(PlayMode& play)
			: m_Play(play)
		{
		}

		~ObserverGuard()
		{
			m_Play.Stop();
			m_Play.SetObserver(nullptr);
		}

		ObserverGuard(ObserverGuard const&) = delete;
		ObserverGuard& operator=(ObserverGuard const&) = delete;

	private:
		PlayMode& m_Play;
	};

	// Writes Scenes/Second.sscene (an entity, no scripts) and Scripts/Loader.sgraph, whose update loads the second scene, into
	// the directory and opens it as the asset directory; returns the loader.
	AssetHandle WriteSceneLoader(std::filesystem::path const& directory)
	{
		REQUIRE(FileSystem::CreateDirectories(directory / "Scenes").IsOk());
		REQUIRE(FileSystem::CreateDirectories(directory / "Scripts").IsOk());
		Scene second("Second");
		second.CreateEntity("Arrival");
		REQUIRE(SceneSerializer::SaveToFile(second, directory / "Scenes" / "Second.sscene").IsOk());
		VisualScriptDocument loader;
		VisualScriptNode update;
		update.ID = 10;
		update.Type = "Event.Update";
		VisualScriptNode load;
		load.ID = 11;
		load.Type = "Call:Strada.SceneManager.LoadScene(string)";
		load.Inputs = Json::object({{"Path", "Scenes/Second.sscene"}});
		loader.Graph.Nodes = {update, load};
		loader.Graph.Links.push_back({{10, "Then"}, {11, "Execute"}});
		REQUIRE(VisualScriptSerializer::SaveToFile(loader, directory / "Scripts" / "Loader.sgraph").IsOk());
		REQUIRE(AssetManager::OpenAssetDirectory(directory).IsOk());
		AssetHandle const graph = AssetManager::FindByPath("Scripts/Loader.sgraph");
		REQUIRE(graph.IsValid());
		return graph;
	}

	// The Prism Break sample as a player gets it, its scenes and assets and no C#, copied and opened in the editor model with
	// play mode (spec-slice4 section 18.10), with what its cases do and read while it plays. The match's random numbers are
	// seeded, so every run plays the same frames.
	class PrismBreakPlay
	{
	public:
		PrismBreakPlay()
		{
			std::filesystem::path const sample = FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR) / "PrismBreak";
			REQUIRE(FileSystem::CopyDirectory(sample / "Assets", m_Directory.GetPath() / "Assets").IsOk());
			REQUIRE(FileSystem::Copy(sample / "PrismBreak.sproj", m_Directory.GetPath() / "PrismBreak.sproj", false).IsOk());
			REQUIRE(m_Fixture.Operations.OpenProject(m_Directory.GetPath() / "PrismBreak.sproj").IsOk());
			Entity const game = Find("Game");
			VisualScriptComponent match = game.GetComponent<VisualScriptComponent>();
			match.Variables["Seed"] = ScriptFieldValue::FromInt32(7);
			REQUIRE(m_Fixture.Operations.SetComponent(game.GetUUID(), match).IsOk());
		}

		PrismBreakPlay(PrismBreakPlay const&) = delete;
		PrismBreakPlay& operator=(PrismBreakPlay const&) = delete;

		// Starts playing, paused: the frames run as the case advances them.
		void Start()
		{
			CHECK(m_Fixture.Ok("play.start")["state"] == "play");
			m_Fixture.Ok("play.pause");
		}

		void Stop() { CHECK(m_Fixture.Ok("play.stop")["state"] == "edit"); }

		void Advance(uint32_t frames) { m_Fixture.Ok("play.advance", Json::object({{"frames", frames}})); }

		void Hold(std::string const& key, bool down) { m_Fixture.Ok("input.set", Json::object({{"keys", Json::object({{key, down}})}})); }

		// A key goes down for a frame, then up.
		void Press(std::string const& key)
		{
			Hold(key, true);
			Advance(1);
			Hold(key, false);
			Advance(1);
		}

		// The mouse at a point of the 640-pixel-wide game view, its height's middle.
		void PointAt(float x) { m_Fixture.Ok("input.set", Json::object({{"mousePosition", Json::array({x, 240.0})}})); }

		Entity Find(std::string_view name)
		{
			Entity const entity = m_Fixture.Context.GetScene().FindEntityByName(name);
			REQUIRE(entity);
			return entity;
		}

		// A public variable of the graph an entity runs (the match's read-outs on the Game), as its instance holds it.
		ScriptFieldValue ReadOut(std::string const& name, std::string_view entity = "Game")
		{
			Result<ScriptFieldMap> const variables = VisualScriptEngine::GetInstanceVariables(Find(entity).GetUUID());
			REQUIRE_MESSAGE(variables.IsOk(), (variables ? std::string() : variables.GetError()));
			auto const variable = variables.GetValue().find(name);
			REQUIRE(variable != variables.GetValue().end());
			return variable->second;
		}

		// Sets a public variable of the graph an entity runs, in its running instance.
		void SetVariable(std::string_view entity, std::string const& name, ScriptFieldValue value)
		{
			ScriptFieldMap const variables = {{name, std::move(value)}};
			Result<void> const set = VisualScriptEngine::SetInstanceVariables(Find(entity).GetUUID(), variables);
			REQUIRE_MESSAGE(set.IsOk(), (set ? std::string() : set.GetError()));
		}

		std::string GetState() { return ReadOut("State").GetString(); }

		bool IsShown(std::string_view element) { return Find(element).GetComponent<UIRectComponent>().Visible; }

		std::string GetText(std::string_view element) { return Find(element).GetComponent<UITextComponent>().Text; }

		glm::vec3 GetPlace(std::string_view name) { return Find(name).GetComponent<TransformComponent>().Translation; }

		// The children of a container that exist: the level's prisms in Bricks, the balls in play in Balls.
		std::vector<Entity> GetChildren(std::string_view container)
		{
			std::vector<Entity> found;
			for (UUID const child : Find(container).GetComponent<RelationshipComponent>().Children)
			{
				if (Entity const entity = m_Fixture.Context.GetScene().GetEntityByUUID(child))
				{
					found.push_back(entity);
				}
			}
			return found;
		}

		glm::vec3 GetBallPlace()
		{
			std::vector<Entity> const balls = GetChildren("Balls");
			REQUIRE(balls.size() == 1);
			return balls.front().GetComponent<TransformComponent>().Translation;
		}

	private:
		Testing::AssetManagerScope m_Assets;
		Testing::PhysicsSystemScope m_Physics;
		Testing::ScriptRuntimeScope m_Scripting;
		InputScope m_Input;
		Testing::TemporaryDirectory m_Directory;
		PlayFixture m_Fixture;
	};
}

TEST_CASE("PlayMode: playing shows and edits a copy that stopping discards")
{
	PlayFixture fixture;
	UUID const crate = fixture.Create("Crate");
	std::array<UUID, 1> const selection = {crate};
	REQUIRE(fixture.Operations.Select(selection).IsOk());
	Scene const* const edited = &fixture.Context.GetScene();
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();
	uint64_t const version = fixture.Context.GetSceneVersion();

	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	CHECK(fixture.Play.IsPlaying());
	CHECK(fixture.Context.GetPlayState() == EditorPlayState::Play);
	CHECK(&fixture.Context.GetScene() != edited);
	CHECK(fixture.Context.GetScene().IsRunning());
	CHECK(fixture.Context.GetScene().GetViewportWidth() == 640);
	CHECK(fixture.Context.GetSceneVersion() != version);
	// The copy has the same entities, so the selection carries over; its history starts empty.
	CHECK(fixture.Context.GetSelection().Contains(crate));
	CHECK_FALSE(fixture.Context.GetHistory().CanUndo());
	CHECK(fixture.Context.IsDirty());
	CHECK(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsError());

	// Edits while playing reach the copy and can be undone there.
	REQUIRE(fixture.Operations.RenameEntity(crate, "Moved").IsOk());
	UUID const spawned = fixture.Create("Spawned");
	std::array<UUID, 2> const both = {crate, spawned};
	REQUIRE(fixture.Operations.Select(both).IsOk());
	CHECK(fixture.Context.GetHistory().CanUndo());

	// Scene and project files wait until playing stops.
	CHECK(fixture.Operations.SaveScene("Scene.sscene").IsError());
	CHECK(fixture.Operations.NewScene().IsError());
	CHECK(fixture.Operations.OpenScene("Scene.sscene").IsError());
	CHECK(fixture.Operations.CloseProject().IsError());

	fixture.Play.Stop();
	CHECK_FALSE(fixture.Play.IsPlaying());
	CHECK(fixture.Context.GetPlayState() == EditorPlayState::Edit);
	CHECK(&fixture.Context.GetScene() == edited);
	CHECK(fixture.Context.GetScene().GetEntityByUUID(crate).GetName() == "Crate");
	CHECK_FALSE(fixture.Context.GetScene().HasEntity(spawned));
	CHECK_FALSE(fixture.Context.GetScene().IsRunning());
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
	CHECK(fixture.Context.GetSelection().GetEntities() == std::vector<UUID>{crate});
}

TEST_CASE(
	"PlayMode: a project plays with its play data as the game's data folder, a test run with a temporary one, and stopping empties it")
{
	{
		// Without a project there is no game data folder.
		PlayFixture fixture;
		REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
		CHECK_FALSE(GameData::GetDirectory().has_value());
		fixture.Play.Stop();
	}

	ProjectPlayFixture fixture;
	REQUIRE_FALSE(GameData::GetDirectory().has_value());
	std::filesystem::path const playData = fixture.Context.GetProject()->GetPlayDataDirectory();
	CHECK(playData == (fixture.Directory.GetPath() / "Game" / ".strada" / "PlayData").lexically_normal());
	for (EditorPlayState const state : {EditorPlayState::Play, EditorPlayState::Simulate})
	{
		CAPTURE(static_cast<int>(state));
		REQUIRE(fixture.Play.Start(state, 640, 480).IsOk());
		CHECK(GameData::GetDirectory() == playData);
		CHECK_FALSE(GameData::IsTemporary());
		fixture.Play.Stop();
		CHECK_FALSE(GameData::GetDirectory().has_value());
	}
	// Playing names the folder and creates nothing: the game's first write does.
	CHECK_FALSE(FileSystem::Exists(fixture.Directory.GetPath() / "Game" / ".strada"));

	// A test run starts from no data and leaves none.
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480, nullptr, PlayStartOptions{.TestRun = true}).IsOk());
	std::optional<std::filesystem::path> const temporary = GameData::GetDirectory();
	REQUIRE(temporary.has_value());
	CHECK(GameData::IsTemporary());
	CHECK(*temporary != playData);
	CHECK_FALSE(FileSystem::IsInside(*temporary, fixture.Directory.GetPath()));
	// What the run writes there goes with the folder.
	REQUIRE(FileSystem::WriteTextFile(*temporary / "Saves" / "Slot1.json", "{}").IsOk());
	fixture.Play.Stop();
	CHECK_FALSE(GameData::GetDirectory().has_value());
	CHECK_FALSE(FileSystem::Exists(*temporary));

	// Stopping by destroying play mode empties it too, and what playing wrote into the play data stays.
	{
		PlayMode play(fixture.Context, fixture.Scripts);
		REQUIRE(play.Start(EditorPlayState::Play, 640, 480).IsOk());
		CHECK(GameData::GetDirectory() == playData);
		REQUIRE(FileSystem::WriteTextFile(playData / "Saves" / "Slot1.json", "{}").IsOk());
	}
	CHECK_FALSE(GameData::GetDirectory().has_value());
	CHECK(FileSystem::IsRegularFile(playData / "Saves" / "Slot1.json"));
}

TEST_CASE("PlayMode: script fields edited while playing reach the running script at once, and edits start from its values")
{
	Testing::ScriptEngineScope scripting;
	PlayFixture fixture;
	UUID const reporter = fixture.Create(
		"Reporter",
		Json::object(
			{{"Script", Json::object({{"ClassName", "Strada.Tests.FieldReporter"},
	                                  {"Fields", Json::object({{"Visible", Json::object({{"Type", "Bool"}, {"Value", false}})}})}})}}));
	auto const field = [&](std::string const& name)
	{
		Result<ScriptFieldMap> const fields = ScriptEngine::GetInstanceFields(reporter);
		REQUIRE(fields.IsOk());
		return fields.GetValue().at(name);
	};
	auto const setField = [&](std::string const& name, char const* type, Json value)
	{
		Json const patch = Json::object({{"Fields", Json::object({{name, Json::object({{"Type", type}, {"Value", std::move(value)}})}})}});
		REQUIRE(fixture.Operations.SetComponentFields(reporter, "Script", patch).IsOk());
	};

	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	fixture.Play.Advance(2, Timestep(1.0f / 60.0f));
	Entity const entity = fixture.Context.GetScene().GetEntityByUUID(reporter);
	CHECK(entity.GetName() == "Speed 1 Visible False");

	// An edit reaches the script at once, also while paused (the next frame shows it), and undoing it gives back the value
	// the script had.
	fixture.Play.SetPaused(true);
	setField("Visible", "Bool", true);
	CHECK(field("Visible") == ScriptFieldValue::FromBool(true));
	fixture.Play.Advance(1, Timestep(1.0f / 60.0f));
	CHECK(entity.GetName() == "Speed 1 Visible True");
	REQUIRE(fixture.Context.Undo().IsOk());
	CHECK(field("Visible") == ScriptFieldValue::FromBool(false));
	REQUIRE(fixture.Context.Redo().IsOk());
	CHECK(field("Visible") == ScriptFieldValue::FromBool(true));
	// Fields the edit did not set keep the values the script gave them.
	CHECK(field("Updates") == ScriptFieldValue::FromInt32(3));

	// Edits start from what the script holds: once it changed a value itself (its update count), setting the value the
	// component still stores reaches it, and undoing gives back the value the script had.
	fixture.Play.SetPaused(false);
	setField("Updates", "Int32", 0);
	CHECK(field("Updates") == ScriptFieldValue::FromInt32(0));
	fixture.Play.Advance(2, Timestep(1.0f / 60.0f));
	CHECK(field("Updates") == ScriptFieldValue::FromInt32(2));
	setField("Updates", "Int32", 0);
	CHECK(field("Updates") == ScriptFieldValue::FromInt32(0));
	REQUIRE(fixture.Context.Undo().IsOk());
	CHECK(field("Updates") == ScriptFieldValue::FromInt32(2));
	CHECK(entity.GetComponent<ScriptComponent>().Fields.at("Speed") == ScriptFieldValue::FromInt32(1));

	// Stopping discards what was edited.
	fixture.Play.Stop();
	Entity const edited = fixture.Context.GetScene().GetEntityByUUID(reporter);
	CHECK(edited.GetComponent<ScriptComponent>().Fields == ScriptFieldMap{{"Visible", ScriptFieldValue::FromBool(false)}});
}

TEST_CASE("PlayMode: entities deleted while playing go at once, also while paused")
{
	PlayFixture fixture;
	UUID const crate = fixture.Create("Crate");
	UUID const child = fixture.Create("Child");
	REQUIRE(fixture.Operations.ReparentEntity(child, crate).IsOk());
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());

	// A running scene defers destruction to the end of the frame, which never comes while it is paused; edits do not
	// wait, so undo restores the entities rather than a second copy next to the doomed ones.
	for (bool const paused : {false, true})
	{
		CAPTURE(paused);
		fixture.Play.SetPaused(paused);
		std::array<UUID, 1> const doomed = {crate};
		REQUIRE(fixture.Operations.DeleteEntities(doomed).IsOk());
		CHECK_FALSE(fixture.Context.GetScene().HasEntity(crate));
		CHECK_FALSE(fixture.Context.GetScene().HasEntity(child));
		REQUIRE(fixture.Context.Undo().IsOk());
		CHECK(fixture.Context.GetScene().GetEntityByUUID(child).GetComponent<RelationshipComponent>().Parent == crate);
		REQUIRE(fixture.Context.Redo().IsOk());
		CHECK_FALSE(fixture.Context.GetScene().HasEntity(crate));
		REQUIRE(fixture.Context.Undo().IsOk());
		fixture.Play.Update(Timestep(1.0f / 60.0f));
		CHECK(fixture.Context.GetScene().HasEntity(crate));
		CHECK(fixture.Context.GetScene().HasEntity(child));
	}
	fixture.Play.Stop();
	CHECK(fixture.Context.GetScene().HasEntity(child));
}

TEST_CASE("PlayMode: scenes the scripts load replace the copy and keep the steps of asset files, and quitting stops playing")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	InputScope input;
	Testing::TemporaryDirectory directory;
	std::filesystem::path const scenes = directory.GetPath() / "Scenes";
	REQUIRE(FileSystem::CreateDirectories(scenes).IsOk());
	WriteSwitcherScene(scenes / "First.sscene", "First", "Scenes/Second.sscene");
	WriteSwitcherScene(scenes / "Second.sscene", "Second", "");
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());

	PlayFixture fixture;
	REQUIRE(fixture.Operations.OpenScene(scenes / "First.sscene").IsOk());
	Result<AssetHandle> const created = fixture.Operations.CreateMaterial("Materials/Rock.smat");
	REQUIRE(created.IsOk());
	AssetHandle const material = created.GetValue();
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	// An edit of the material and one of the running scene.
	REQUIRE(fixture.Operations.SetMaterialFields(material, Json::object({{"Roughness", 0.75}})).IsOk());
	UUID const switcher = fixture.Context.GetScene().FindEntityByName("Switcher").GetUUID();
	REQUIRE(fixture.Operations.RenameEntity(switcher, "Renamed").IsOk());
	CHECK(fixture.Context.GetHistory().GetUndoCount() == 2);

	// The scene loaded replaces the copy and its steps; the material's step stays.
	uint64_t const version = fixture.Context.GetSceneVersion();
	AdvanceWithKey(fixture.Play, KeyCode::L);
	CHECK(fixture.Context.GetScene().GetName() == "Second");
	CHECK(fixture.Context.GetSceneVersion() != version);
	CHECK(fixture.Context.GetScene().IsRunning());
	CHECK(fixture.Context.GetHistory().GetUndoCount() == 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Edit Material");

	// Quitting stops playing; the edited scene's history takes the material's step, which undoes.
	AdvanceWithKey(fixture.Play, KeyCode::Q);
	CHECK_FALSE(fixture.Play.IsPlaying());
	CHECK(fixture.Context.GetScene().GetName() == "First");
	CHECK(fixture.Context.GetHistory().GetUndoCount() == 1);
	CHECK(fixture.Context.GetHistory().GetUndoEditedAsset() == material);
	CHECK_FALSE(fixture.Context.IsDirty());
	REQUIRE(fixture.Operations.Undo().IsOk());
	Ref<MaterialAsset> const rock = AssetManager::GetAsset<MaterialAsset>(material);
	REQUIRE(rock != nullptr);
	CHECK(rock->GetData().Roughness == doctest::Approx(0.5f));
	TestRunReport const* const report = fixture.Play.GetTestReport();
	REQUIRE(report != nullptr);
	REQUIRE(report->Results.size() == 4);
	CHECK(report->Results[1].Name == "First stopped");
	CHECK(report->Results[3].Name == "Second stopped");
}

TEST_CASE("PlayMode: simulating runs physics only, and pausing holds the scene")
{
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting;
	PlayFixture fixture;
	UUID const ball = fixture.Create("Ball", Json::object({{"Transform", Json::object({{"Translation", {0.0, 5.0, 0.0}}})},
	                                                       {"RigidBody", Json::object({{"Type", "Dynamic"}})},
	                                                       {"SphereCollider", Json::object()},
	                                                       {"Script", Json::object({{"ClassName", "Strada.Tests.Mover"}})}}));
	REQUIRE(fixture.Play.Start(EditorPlayState::Simulate, 640, 480).IsOk());
	CHECK(fixture.Context.GetPlayState() == EditorPlayState::Simulate);
	CHECK(ScriptEngine::GetInstanceCount() == 0);
	CHECK(fixture.Context.GetScene().GetAudioScene() == nullptr);

	auto const height = [&fixture, ball]
	{
		return fixture.Context.GetScene().GetEntityByUUID(ball).GetComponent<TransformComponent>().Translation.y;
	};
	CHECK(fixture.Play.Advance(30, Timestep(1.0f / 60.0f)) == 30);
	float const fallen = height();
	CHECK(fallen < 5.0f);

	// Paused: the editor's frames hold the scene, steps advance it, and Advance runs frames anyway.
	fixture.Play.SetPaused(true);
	CHECK(fixture.Play.IsPaused());
	uint64_t const frame = fixture.Context.GetScene().GetRuntimeFrame();
	fixture.Play.Update(Timestep(1.0f / 60.0f));
	CHECK(fixture.Context.GetScene().GetRuntimeFrame() == frame);
	fixture.Play.Step(2);
	fixture.Play.Update(Timestep(1.0f / 60.0f));
	fixture.Play.Update(Timestep(1.0f / 60.0f));
	fixture.Play.Update(Timestep(1.0f / 60.0f));
	CHECK(fixture.Context.GetScene().GetRuntimeFrame() == frame + 2);
	CHECK(fixture.Play.Advance(3, Timestep(1.0f / 60.0f)) == 3);
	CHECK(fixture.Context.GetScene().GetRuntimeFrame() == frame + 5);
	CHECK(height() < fallen);

	fixture.Play.Stop();
	CHECK(height() == 5.0f);
}

TEST_CASE("PlayMode: the contacts overlay draws the playing copy's contacts of the last frame, and keeps them while paused")
{
	Testing::PhysicsSystemScope physics;
	PlayFixture fixture;
	fixture.Create("Ground", Json::object({{"Transform", Json::object({{"Translation", {0.0, -0.5, 0.0}}})},
	                                       {"BoxCollider", Json::object({{"HalfExtents", {10.0, 0.5, 10.0}}})}}));
	fixture.Create("Ball", Json::object({{"Transform", Json::object({{"Translation", {0.0, 2.0, 0.0}}})},
	                                     {"RigidBody", Json::object({{"Type", "Dynamic"}})},
	                                     {"SphereCollider", Json::object()}}));
	SceneViewOverlays overlays;
	overlays.Contacts = true;
	auto const contactLines = [&fixture, &overlays]
	{
		std::vector<OverlayLine> lines;
		ViewportOverlays::AppendSceneShapes(fixture.Context.GetScene(), {}, overlays, 1.0f, glm::vec3(0.0f, 0.0f, 10.0f), lines);
		return lines.size();
	};
	CHECK(contactLines() == 0);

	REQUIRE(fixture.Play.Start(EditorPlayState::Simulate, 640, 480).IsOk());
	bool landed = false;
	for (int frame = 0; frame < 120 && !landed; frame++)
	{
		fixture.Play.Advance(1, Timestep(1.0f / 60.0f));
		landed = !fixture.Context.GetScene().GetContactEvents().empty();
	}
	REQUIRE(landed);
	size_t const drawn = contactLines();
	CHECK(drawn == 4 * fixture.Context.GetScene().GetContactEvents().front().PointCount);
	CHECK(drawn > 0);

	// Paused, the editor's frames hold the scene and its last frame's contacts with it.
	fixture.Play.SetPaused(true);
	fixture.Play.Update(Timestep(1.0f / 60.0f));
	fixture.Play.Update(Timestep(1.0f / 60.0f));
	CHECK(contactLines() == drawn);
	// A frame that begins no contact draws none.
	fixture.Play.SetPaused(false);
	fixture.Play.Advance(1, Timestep(1.0f / 60.0f));
	CHECK(contactLines() == 0);

	// The edited scene does not run: it has no contacts.
	fixture.Play.Stop();
	CHECK(contactLines() == 0);
}

TEST_CASE("PlayMode: graph and material edits made while playing stay undoable after Stop, and a saved scene stays clean")
{
	ProjectPlayFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Player.sgraph");
	AssetHandle const material = fixture.CreateMaterial("Materials/Rock.smat");
	UUID const crate = fixture.Create("Crate");
	REQUIRE(fixture.Operations.SaveScene().IsOk());
	CHECK_FALSE(fixture.Context.IsDirty());
	size_t const steps = fixture.GetUndoCount();
	std::string const graphBytes = fixture.ReadFile(graph);
	std::string const materialBytes = fixture.ReadFile(material);
	constexpr uint64_t Key = 0x5445535400000007ull;

	// While playing: an edit of the running copy, a graph edit, a material drag, and a graph edit undone again.
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	REQUIRE(fixture.Operations.RenameEntity(crate, "Moved").IsOk());
	fixture.Describe(graph, "Played");
	std::string const playedGraph = fixture.ReadFile(graph);
	fixture.SetRoughness(material, 0.75, Key);
	std::string const playedMaterial = fixture.ReadFile(material);
	fixture.Describe(graph, "Undone");
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadFile(graph) == playedGraph);
	CHECK(fixture.GetUndoCount() == 3);
	CHECK_FALSE(fixture.Context.IsDirty());

	// Stopped: the copy and its edit are gone, the steps of the files stay (not the one undone), and the scene is clean.
	fixture.Play.Stop();
	CommandHistory const& history = fixture.Context.GetHistory();
	CHECK(fixture.Context.GetScene().GetEntityByUUID(crate).GetName() == "Crate");
	CHECK(history.GetUndoCount() == steps + 2);
	CHECK(history.GetUndoDescription() == "Edit Material");
	CHECK(history.GetUndoEditedAsset() == material);
	CHECK_FALSE(history.CanRedo());
	CHECK_FALSE(fixture.Context.IsDirty());

	// An edit after Stop is a step of its own, even under the merge key of the last edit made while playing.
	fixture.SetRoughness(material, 0.25, Key);
	CHECK(history.GetUndoCount() == steps + 3);
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadFile(material) == playedMaterial);

	// Undo gives the files the bytes they had before playing, and redo the ones playing gave them; the scene stays clean.
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadFile(material) == materialBytes);
	CHECK_FALSE(fixture.Context.IsDirty());
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadFile(graph) == graphBytes);
	CHECK(history.GetUndoCount() == steps);
	CHECK_FALSE(fixture.Context.IsDirty());
	REQUIRE(fixture.Operations.Redo().IsOk());
	REQUIRE(fixture.Operations.Redo().IsOk());
	CHECK(fixture.ReadFile(graph) == playedGraph);
	CHECK(fixture.ReadFile(material) == playedMaterial);
	CHECK_FALSE(fixture.Context.IsDirty());
}

TEST_CASE("PlayMode: another scene of the project keeps the steps of asset files, and another project drops them")
{
	ProjectPlayFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Player.sgraph");
	std::filesystem::path const mainScene = fixture.Context.GetScenePath();
	REQUIRE_FALSE(mainScene.empty());
	std::string const original = fixture.ReadFile(graph);
	fixture.Create("Crate");
	fixture.Describe(graph, "Kept");
	std::string const kept = fixture.ReadFile(graph);
	fixture.Create("Barrel");
	CHECK(fixture.GetUndoCount() == 3);
	CHECK(fixture.Context.IsDirty());

	// A new scene: the steps of the scene go with it, the graph's step stays, and the new scene is clean.
	REQUIRE(fixture.Operations.NewScene("Level").IsOk());
	CHECK(fixture.GetUndoCount() == 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Describe Graph");
	CHECK_FALSE(fixture.Context.IsDirty());
	// A scene file opened: the same.
	REQUIRE(fixture.Operations.OpenScene(mainScene).IsOk());
	CHECK(fixture.GetUndoCount() == 1);
	CHECK_FALSE(fixture.Context.IsDirty());
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadFile(graph) == original);
	CHECK_FALSE(fixture.Context.IsDirty());
	REQUIRE(fixture.Operations.Redo().IsOk());
	CHECK(fixture.ReadFile(graph) == kept);

	// Another project created, opened or closed: the steps name the assets of the project before, so none stays.
	std::filesystem::path const game = fixture.Context.GetProject()->GetFilePath();
	REQUIRE(fixture.Operations.CreateProject(fixture.Directory.GetPath() / "Other", "Other", Scene("Main")).IsOk());
	CHECK_FALSE(fixture.Context.GetHistory().CanUndo());
	AssetHandle const other = fixture.CreateGraph("Scripts/Enemy.sgraph");
	fixture.Describe(other, "Other");
	CHECK(fixture.GetUndoCount() == 1);
	REQUIRE(fixture.Operations.OpenProject(game).IsOk());
	CHECK_FALSE(fixture.Context.GetHistory().CanUndo());
	fixture.Describe(graph, "Again");
	CHECK(fixture.GetUndoCount() == 1);
	REQUIRE(fixture.Operations.CloseProject().IsOk());
	CHECK_FALSE(fixture.Context.GetHistory().CanUndo());
	CHECK_FALSE(fixture.Context.GetHistory().CanRedo());
}

TEST_CASE("PlayMode: deleting an edited graph or material discards its steps, and undo goes on with the steps before them")
{
	ProjectPlayFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Player.sgraph");
	AssetHandle const material = fixture.CreateMaterial("Materials/Rock.smat");
	UUID const crate = fixture.Create("Crate");
	fixture.Describe(graph, "First");
	fixture.SetRoughness(material, 0.75);
	REQUIRE(fixture.Operations.RenameEntity(crate, "Box").IsOk());
	fixture.Describe(graph, "Second");
	fixture.Describe(graph, "Third");
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.GetUndoCount() == 5);

	// The graph's steps leave both stacks, and undo goes on with the steps before them.
	REQUIRE(fixture.Operations.DeleteAsset(graph).IsOk());
	CommandHistory const& history = fixture.Context.GetHistory();
	CHECK(history.GetUndoCount() == 3);
	CHECK_FALSE(history.CanRedo());
	CHECK(history.GetUndoDescription() == "Rename Entity");
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.Context.GetScene().GetEntityByUUID(crate).GetName() == "Crate");
	REQUIRE(fixture.Operations.Undo().IsOk());
	Ref<MaterialAsset> const rock = AssetManager::GetAsset<MaterialAsset>(material);
	REQUIRE(rock != nullptr);
	CHECK(rock->GetData().Roughness == doctest::Approx(0.5f));
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK_FALSE(fixture.Context.GetScene().HasEntity(crate));
	CHECK_FALSE(history.CanUndo());
	for (int step = 0; step < 3; step++)
	{
		REQUIRE(fixture.Operations.Redo().IsOk());
	}
	CHECK(rock->GetData().Roughness == doctest::Approx(0.75f));

	// Deleted while playing, with its folder: the material's steps leave the running scene's history and the edited one's.
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	fixture.SetRoughness(material, 0.25);
	CHECK(fixture.GetUndoCount() == 1);
	REQUIRE(fixture.Operations.DeleteAssetFolder("Materials").IsOk());
	CHECK_FALSE(AssetManager::IsValid(material));
	CHECK_FALSE(history.CanUndo());
	fixture.Play.Stop();
	CHECK(history.GetUndoCount() == 2);
	CHECK(history.GetUndoDescription() == "Rename Entity");
	REQUIRE(fixture.Operations.Undo().IsOk());
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK_FALSE(history.CanUndo());
	CHECK_FALSE(fixture.Context.GetScene().HasEntity(crate));
}

TEST_CASE("PlayMode: after an entity drop, deleting its graph keeps the step, whose undo gives the component its value back")
{
	ProjectPlayFixture fixture;
	AssetHandle const graph = fixture.CreateGraph("Scripts/Player.sgraph");
	UUID const door = fixture.Create("Door");
	UUID const player = fixture.Create("Player", Json::object({{"VisualScript", Json::object({{"Graph", graph.ToString()}})}}));
	size_t const steps = fixture.GetUndoCount();
	auto const variables = [&]
	{
		Entity const entity = fixture.Operations.FindEntity(player);
		REQUIRE(entity);
		return entity.GetComponent<VisualScriptComponent>().Variables;
	};

	// The door dropped into the graph: a variable of the graph, and the player's value of it, in one step.
	Ref<VisualScriptAsset> const asset = AssetManager::GetAsset<VisualScriptAsset>(graph);
	REQUIRE(asset != nullptr);
	VisualScriptDocument referencing = asset->GetDocument();
	VisualScriptVariable variable;
	variable.ID = 1000;
	variable.Name = "Door";
	variable.Type = "Strada.Entity";
	variable.Public = true;
	referencing.Variables.push_back(variable);
	Json const value = Json::object({{"Type", "Entity"}, {"Value", std::to_string(door.GetValue())}});
	std::vector<ComponentEdit> edits = {{player, "VisualScript", Json::object({{"Variables", Json::object({{"Door", value}})}})}};
	REQUIRE(fixture.Operations
	            .EditVisualScriptAndComponents(graph, CreateRef<VisualScriptDocument const>(std::move(referencing)),
	                                           "Reference Door in Player", std::move(edits))
	            .IsOk());
	CommandHistory const& history = fixture.Context.GetHistory();
	CHECK(history.GetUndoCount() == steps + 1);
	CHECK(history.GetUndoEditedAsset() == graph);
	CHECK(variables() == ScriptFieldMap{{"Door", ScriptFieldValue::FromEntity(door)}});

	// The step changed the scene: it stays, edits the components alone, and the steps before it still undo.
	REQUIRE(fixture.Operations.DeleteAsset(graph).IsOk());
	CHECK(history.GetUndoCount() == steps + 1);
	CHECK(history.GetUndoDescription() == "Reference Door in Player");
	CHECK_FALSE(history.GetUndoEditedAsset().IsValid());
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(variables().empty());
	REQUIRE(fixture.Operations.Undo().IsOk());
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK_FALSE(history.CanUndo());
	CHECK_FALSE(fixture.Context.GetScene().HasEntity(door));
	for (int step = 0; step < 3; step++)
	{
		REQUIRE(fixture.Operations.Redo().IsOk());
	}
	CHECK(variables() == ScriptFieldMap{{"Door", ScriptFieldValue::FromEntity(door)}});
}

TEST_CASE("PlayCommands: every play, input, physics and test command is documented in Docs/Automation.md")
{
	PlayFixture fixture;
	Result<std::string> reference = FileSystem::ReadTextFile(STRADA_AUTOMATION_DOC_PATH);
	REQUIRE(reference.IsOk());
	CHECK(fixture.Registry.GetCommandCount() == 11);
	for (CommandDefinition const* command : fixture.Registry.GetCommands())
	{
		CAPTURE(command->Name);
		CHECK(reference.GetValue().find("`" + command->Name + "`") != std::string::npos);
	}
}

TEST_CASE("PlayCommands: automation plays, steps, injects input and stops")
{
	Testing::ScriptEngineScope scripting;
	InputScope input;
	PlayFixture fixture;
	fixture.Create("Crate");

	CHECK(fixture.Ok("play.state")["state"] == "edit");
	CHECK(fixture.Fails("play.stop") == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("play.advance", Json::object({{"frames", 1}})) == AutomationErrorCode::InvalidOperation);

	Json const started = fixture.Ok("play.start");
	CHECK(started["state"] == "play");
	CHECK(started["frame"] == 0);
	CHECK(fixture.Fails("play.start") == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Fails("play.step") == AutomationErrorCode::InvalidOperation);
	CHECK(fixture.Ok("play.pause")["paused"] == true);
	CHECK(fixture.Ok("play.step", Json::object({{"frames", 2}}))["paused"] == true);
	Json const advanced = fixture.Ok("play.advance", Json::object({{"frames", 5}, {"timestep", 0.5}}));
	CHECK(advanced["framesRun"] == 5);
	CHECK(advanced["frame"] == 5);
	CHECK(advanced["time"] == doctest::Approx(2.5));

	Json const keys = fixture.Ok("input.set", Json::object({{"keys", Json::object({{"A", true}, {"Space", true}})},
	                                                        {"mouseButtons", Json::object({{"Left", true}})},
	                                                        {"mousePosition", {12.0, 34.0}}}));
	CHECK(keys["keys"] == Json::array({"Space", "A"}));
	CHECK(keys["mouseButtons"] == Json::array({"Left"}));
	CHECK(keys["mousePosition"] == Json::array({12.0, 34.0}));
	CHECK(Input::IsKeyDown(KeyCode::A));
	CHECK(fixture.Fails("input.set", Json::object({{"keys", Json::object({{"Nope", true}})}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Ok("input.release")["keys"].empty());

	Json const stopped = fixture.Ok("play.stop");
	CHECK(stopped["state"] == "edit");
	CHECK(fixture.Ok("play.start", Json::object({{"mode", "simulate"}}))["state"] == "simulate");
	CHECK(fixture.Ok("play.stop")["state"] == "edit");
}

TEST_CASE("PlayCommands: automation finds the UI with ui.layout and clicks its buttons through input.set")
{
	Testing::ScriptEngineScope scripting;
	InputScope input;
	PlayFixture fixture;
	// The game view is 640 x 480 pixels: the 200 x 60 button in the middle of the canvas spans (220, 210) to (420, 270).
	UUID const canvas = fixture.Create("Canvas", Json::object({{"UICanvas", Json::object({{"ScaleMode", "ConstantPixelSize"}})}}));
	EntityCreateInfo info;
	info.Name = "Button";
	info.Parent = canvas;
	info.Components = Json::object(
		{{"UIRect", Json::object({{"Size", Json::array({200.0, 60.0})}})}, {"UIImage", Json::object()}, {"UIButton", Json::object()}});
	Result<UUID> const button = fixture.Operations.CreateEntity(info);
	REQUIRE(button.IsOk());
	std::string const buttonId = std::to_string(button.GetValue().GetValue());
	// The canvas's script hears the clicks of the buttons in it and names its entity after them each update.
	Json const watched = Json::object({{"Type", "Entity"}, {"Value", buttonId}});
	REQUIRE(fixture.Operations
	            .AddComponent(canvas, "Script",
	                          Json::object({{"ClassName", "Strada.Tests.ClickRecorder"}, {"Fields", Json::object({{"Watched", watched}})}}))
	            .IsOk());

	// Where the button is in the game view, before playing at the size the game view would have.
	Json const layout = fixture.Ok("ui.layout");
	CHECK(layout["width"] == 640);
	CHECK(layout["height"] == 480);
	CHECK(layout["pointerOverUI"] == false);
	REQUIRE(layout["elements"].size() == 2);
	Json const& canvasElement = layout["elements"][0];
	CHECK(canvasElement["id"] == std::to_string(canvas.GetValue()));
	CHECK(canvasElement["name"] == "Canvas");
	CHECK(canvasElement["hitTarget"] == false);
	CHECK(canvasElement["button"].is_null());
	CHECK_FALSE(canvasElement.contains("state"));
	Json const& buttonElement = layout["elements"][1];
	CHECK(buttonElement["id"] == buttonId);
	CHECK(buttonElement["canvas"] == std::to_string(canvas.GetValue()));
	CHECK(buttonElement["min"] == Json::array({220.0, 210.0}));
	CHECK(buttonElement["max"] == Json::array({420.0, 270.0}));
	CHECK(buttonElement["center"] == Json::array({320.0, 240.0}));
	CHECK(buttonElement["opacity"] == 1.0);
	CHECK(buttonElement["hitTarget"] == true);
	CHECK(buttonElement["button"] == buttonId);
	CHECK(buttonElement["state"] == "normal");
	CHECK(fixture.Ok("ui.layout", Json::object({{"entity", buttonId}}))["elements"] == Json::array({buttonElement}));
	CHECK(fixture.Fails("ui.layout", Json::object({{"entity", "not an id"}})) == AutomationErrorCode::InvalidParams);
	CHECK(fixture.Fails("ui.layout", Json::object({{"entity", "12345"}})) == AutomationErrorCode::EntityNotFound);
	UUID const outside = fixture.Create("Outside");
	CHECK(fixture.Fails("ui.layout", Json::object({{"entity", std::to_string(outside.GetValue())}})) == AutomationErrorCode::InvalidParams);

	REQUIRE(fixture.Ok("play.start")["state"] == "play");
	fixture.Ok("play.pause");
	auto const advance = [&fixture, canvas](Json const& devices)
	{
		fixture.Ok("input.set", devices);
		// The editor's frame ends between the two requests; the paused game keeps its input for its next frame.
		Input::EndApplicationFrame();
		fixture.Ok("play.advance", Json::object({{"frames", 1}}));
		Entity const recorder = fixture.Context.GetScene().GetEntityByUUID(canvas);
		REQUIRE(recorder);
		return recorder.GetName();
	};
	// The mouse at the center ui.layout gave.
	CHECK(advance(Json::object({{"mousePosition", buttonElement["center"]}})) ==
	      "Clicks=0;Last=0;InClick=False;Polled=False;State=Hovered;Over=True");
	Json const hovered = fixture.Ok("ui.layout", Json::object({{"entity", buttonId}}));
	CHECK(hovered["pointerOverUI"] == true);
	CHECK(hovered["elements"][0]["state"] == "hovered");
	CHECK(advance(Json::object({{"mouseButtons", Json::object({{"Left", true}})}})) ==
	      "Clicks=0;Last=0;InClick=False;Polled=False;State=Pressed;Over=True");
	CHECK(fixture.Ok("ui.layout", Json::object({{"entity", buttonId}}))["elements"][0]["state"] == "pressed");
	CHECK(advance(Json::object({{"mouseButtons", Json::object({{"Left", false}})}})) ==
	      "Clicks=1;Last=" + buttonId + ";InClick=True;Polled=True;State=Hovered;Over=True");
	// Outside the button, a press and a release click nothing.
	CHECK(advance(Json::object({{"mousePosition", Json::array({100.0, 100.0})}, {"mouseButtons", Json::object({{"Left", true}})}})) ==
	      "Clicks=1;Last=" + buttonId + ";InClick=True;Polled=False;State=Normal;Over=False");
	CHECK(advance(Json::object({{"mouseButtons", Json::object({{"Left", false}})}})) ==
	      "Clicks=1;Last=" + buttonId + ";InClick=True;Polled=False;State=Normal;Over=False");
	// A press that input.release ends (the window losing the focus) is canceled.
	advance(Json::object({{"mousePosition", buttonElement["center"]}, {"mouseButtons", Json::object({{"Left", true}})}}));
	fixture.Ok("input.release");
	Input::EndApplicationFrame();
	fixture.Ok("play.advance", Json::object({{"frames", 1}}));
	CHECK(fixture.Context.GetScene().GetEntityByUUID(canvas).GetName() ==
	      "Clicks=1;Last=" + buttonId + ";InClick=True;Polled=False;State=Hovered;Over=True");
	CHECK(fixture.Ok("play.stop")["state"] == "edit");
}

TEST_CASE("PlayCommands: a paused game keeps what input.set gave it for its next frame, whatever editor frames come between")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	InputScope input;
	// Logs when Space is pressed and when it is released.
	VisualScriptDocument keys;
	VisualScriptNode pressed;
	pressed.ID = 10;
	pressed.Type = "Event.KeyPressed";
	pressed.Settings = Json::object({{"Key", "Space"}});
	VisualScriptNode logPress;
	logPress.ID = 11;
	logPress.Type = "Call:Strada.Log.Info(string)";
	logPress.Inputs = Json::object({{"Message", "Space pressed in the graph"}});
	VisualScriptNode released = pressed;
	released.ID = 12;
	released.Type = "Event.KeyReleased";
	VisualScriptNode logRelease = logPress;
	logRelease.ID = 13;
	logRelease.Inputs = Json::object({{"Message", "Space released in the graph"}});
	keys.Graph.Nodes = {pressed, logPress, released, logRelease};
	keys.Graph.Links = {{{10, "Then"}, {11, "Execute"}}, {{12, "Then"}, {13, "Execute"}}};
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(std::move(keys)), "Keys");
	PlayFixture fixture;
	fixture.Create("Keys", Json::object({{"VisualScript", Json::object({{"Graph", graph.ToString()}})}}));
	UUID const probe = fixture.Create("Probe", Json::object({{"Script", Json::object({{"ClassName", "Strada.Tests.InputFrameProbe"}})}}));

	uint64_t const firstEntry = Log::GetNextEntryIndex();
	auto const countLogged = [firstEntry](std::string_view text)
	{
		std::vector<LogEntry> const entries = Log::GetEntries(firstEntry);
		return std::count_if(entries.begin(), entries.end(),
		                     [text](LogEntry const& entry)
		                     {
								 return entry.Message.find(text) != std::string::npos;
							 });
	};
	auto const probed = [&fixture, probe]
	{
		Entity const entity = fixture.Context.GetScene().GetEntityByUUID(probe);
		REQUIRE(entity);
		return entity.GetName();
	};
	// An editor frame between two requests, as EditorLayer::OnUpdate runs it after automation: play mode's update, then the
	// application's end of frame.
	constexpr float Frame = 1.0f / 60.0f;
	auto const editorFrames = [&fixture](int count)
	{
		for (int frame = 0; frame < count; frame++)
		{
			fixture.Play.Update(Timestep(Frame));
			Input::EndApplicationFrame();
		}
	};

	// The mouse's first known position is no movement.
	fixture.Ok("input.set", Json::object({{"mousePosition", Json::array({100.0, 100.0})}}));
	REQUIRE(fixture.Ok("play.start", Json::object({{"paused", true}}))["paused"] == true);
	CHECK(Input::IsFrameOwned());
	editorFrames(2);

	// A press, movement and scroll given to the paused game reach the next play.advance frame, which sees them once.
	fixture.Ok("input.set", Json::object({{"keys", Json::object({{"Space", true}})},
	                                      {"mouseButtons", Json::object({{"Left", true}})},
	                                      {"mousePosition", Json::array({130.0, 90.0})},
	                                      {"scroll", Json::array({0.0, 2.0})}}));
	editorFrames(3);
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 1}}))["framesRun"] == 1);
	CHECK(countLogged("Space pressed in the graph") == 1);
	CHECK(probed() == "Pressed=1;Released=0;Clicked=1;Moved=1;Delta=30,-10;Scrolled=1;Scroll=0,2");
	editorFrames(1);
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 1}}))["framesRun"] == 1);
	CHECK(countLogged("Space pressed in the graph") == 1);
	CHECK(probed() == "Pressed=1;Released=0;Clicked=1;Moved=1;Delta=0,0;Scrolled=1;Scroll=0,0");

	// A release reaches the frame play.step runs at the next editor frame; the frames after it run none and see nothing.
	fixture.Ok("input.set", Json::object({{"keys", Json::object({{"Space", false}})}}));
	editorFrames(2);
	fixture.Ok("play.step");
	editorFrames(3);
	CHECK(countLogged("Space released in the graph") == 1);
	CHECK(probed() == "Pressed=1;Released=1;Clicked=1;Moved=1;Delta=0,0;Scrolled=1;Scroll=0,0");
	CHECK(fixture.Context.GetScene().GetRuntimeFrame() == 3);

	// A running game sees a press in the next editor frame, once.
	REQUIRE(fixture.Ok("play.pause", Json::object({{"paused", false}}))["paused"] == false);
	fixture.Ok("input.set", Json::object({{"keys", Json::object({{"Space", true}})}}));
	editorFrames(2);
	CHECK(countLogged("Space pressed in the graph") == 2);
	CHECK(probed() == "Pressed=2;Released=1;Clicked=1;Moved=1;Delta=0,0;Scrolled=1;Scroll=0,0");
	CHECK(fixture.Context.GetScene().GetRuntimeFrame() == 5);

	// Stopping gives the input frame back to the application, whose frames end it again in edit mode.
	CHECK(fixture.Ok("play.stop")["state"] == "edit");
	CHECK_FALSE(Input::IsFrameOwned());
	CHECK_FALSE(Input::IsKeyDown(KeyCode::Space));
	fixture.Ok("input.set", Json::object({{"keys", Json::object({{"A", true}})}, {"scroll", Json::array({0.0, 1.0})}}));
	CHECK(Input::IsKeyPressed(KeyCode::A));
	editorFrames(1);
	CHECK_FALSE(Input::IsKeyPressed(KeyCode::A));
	CHECK(Input::IsKeyDown(KeyCode::A));
	CHECK(Input::GetScrollDelta() == glm::vec2(0.0f));
}

TEST_CASE("PlayMode: a binding the game changes is kept below the project's play data, the next play reads it, and stopping "
          "forgets it")
{
	Testing::PhysicsSystemScope physics;
	InputScope input;
	ProjectPlayFixture fixture;
	DataAssetType const* const type = DataAssetRegistry::FindByName("InputActions");
	REQUIRE(type != nullptr);
	Result<AssetHandle> const actions = fixture.Operations.CreateDataAsset(
		type->GetAssetType(), "Input/Game.sinput",
		Json::parse(R"({ "Actions": [ { "Name": "Jump", "Bindings": [ { "Controls": "Key/Space" } ] } ] })"));
	REQUIRE(actions.IsOk());
	REQUIRE(fixture.Operations.ApplyProjectSettings(Json::object({{"InputActions", actions.GetValue().ToString()}})).IsOk());
	std::filesystem::path const profile = fixture.Context.GetProject()->GetPlayDataDirectory() / "Input" / "Game.sinput.json";

	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	CHECK(fixture.Context.GetScene().RebindInput("Jump", 0, "Key/J").Status == InputRebindStatus::Done);
	Result<std::string> const text = FileSystem::ReadTextFile(profile);
	REQUIRE(text.IsOk());
	Result<Json> const document = ParseJson(text.GetValue());
	REQUIRE(document.IsOk());
	Result<InputBindingProfile> const written = ReadInputBindingProfile(document.GetValue());
	REQUIRE(written.IsOk());
	CHECK(written.GetValue().Bindings == std::vector<InputBindingProfileEntry>{{"Jump", 0, "Key/J", "Key/Space"}});
	fixture.Play.Stop();
	CHECK_FALSE(InputRebinding::GetActions().IsValid());
	CHECK(InputRebinding::GetProfile().Bindings.empty());

	// The next play reads the profile as its actions load; the asset is as it was.
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	Result<std::string> const bound = fixture.Context.GetScene().GetInputBindingControls("Jump", 0);
	REQUIRE(bound.IsOk());
	CHECK(bound.GetValue() == "Key/J");
	Ref<TypedDataAsset<InputActionMap>> const asset = AssetManager::GetAsset<TypedDataAsset<InputActionMap>>(actions.GetValue());
	REQUIRE(asset != nullptr);
	CHECK(asset->GetData().Actions[0].Bindings[0].Controls == "Key/Space");
	fixture.Play.Stop();
	CHECK_FALSE(InputRebinding::GetActions().IsValid());
}

TEST_CASE("PlayCommands: the project's input actions are evaluated in the game's input frame, and a paused game keeps their taps")
{
	Testing::PhysicsSystemScope physics;
	InputScope input;
	ProjectPlayFixture fixture;
	CommandRegistry registry;
	REQUIRE(RegisterPlayCommands(registry, fixture.Play, fixture.Context,
	                             []
	                             {
									 return glm::uvec2(640, 480);
								 })
	            .IsOk());
	auto const ok = [&registry](std::string_view name, Json params = Json::object())
	{
		std::optional<CommandResult> result;
		registry.Execute(name, std::move(params),
		                 [&result](CommandResult value)
		                 {
							 result.emplace(std::move(value));
						 });
		REQUIRE(result.has_value());
		REQUIRE_MESSAGE(result->IsOk(), (result->IsError() ? result->GetError().Message : std::string()));
		return result->TakeValue();
	};
	DataAssetType const* const type = DataAssetRegistry::FindByName("InputActions");
	REQUIRE(type != nullptr);
	Result<AssetHandle> const actions = fixture.Operations.CreateDataAsset(
		type->GetAssetType(), "Input/Game.sinput",
		Json::parse(R"({ "Actions": [ { "Name": "Jump", "Bindings": [ { "Controls": "Key/Space" }, { "Controls": "Gamepad/A" } ] } ] })"));
	REQUIRE(actions.IsOk());
	REQUIRE(fixture.Operations.ApplyProjectSettings(Json::object({{"InputActions", actions.GetValue().ToString()}})).IsOk());

	constexpr float Frame = 1.0f / 60.0f;
	auto const editorFrames = [&fixture](int count)
	{
		for (int frame = 0; frame < count; frame++)
		{
			fixture.Play.Update(Timestep(Frame));
			Input::EndApplicationFrame();
		}
	};
	auto const advance = [&ok]
	{
		CHECK(ok("play.advance", Json::object({{"frames", 1}}))["framesRun"] == 1);
	};
	auto const edges = [&fixture]
	{
		std::span<InputActionEdge const> const listed = fixture.Context.GetScene().GetInputActions().GetEdges();
		return std::vector<InputActionEdge>(listed.begin(), listed.end());
	};

	REQUIRE(ok("play.start", Json::object({{"paused", true}}))["paused"] == true);
	InputActionState const& state = fixture.Context.GetScene().GetInputActions();
	REQUIRE(state.GetActions() != nullptr);
	editorFrames(2);

	// A key given before a frame is the action's value in that frame.
	ok("input.set", Json::object({{"keys", Json::object({{"Space", true}})}}));
	editorFrames(3);
	advance();
	CHECK(fixture.Context.GetScene().GetInputActions().GetValue(0) == 1.0f);
	CHECK(edges() == std::vector<InputActionEdge>{{0, true}});
	advance();
	CHECK(edges().empty());

	// Released and pressed again while paused, with editor frames between: the next frame reports both, once.
	ok("input.set", Json::object({{"keys", Json::object({{"Space", false}})}}));
	editorFrames(2);
	ok("input.set", Json::object({{"keys", Json::object({{"Space", true}})}}));
	editorFrames(2);
	advance();
	CHECK(edges() == std::vector<InputActionEdge>{{0, false}, {0, true}});
	ok("input.set", Json::object({{"keys", Json::object({{"Space", false}})}}));
	advance();
	CHECK(edges() == std::vector<InputActionEdge>{{0, false}});

	// A tap of an injected gamepad's button, and an override set and cleared, given while paused: play.step's frame
	// reports each as a tap.
	Input::GamepadState pad = Input::GetRestingGamepadState();
	pad.Buttons[static_cast<size_t>(GamepadButton::A)] = true;
	Input::InjectGamepadState(0, pad);
	editorFrames(2);
	pad.Buttons[static_cast<size_t>(GamepadButton::A)] = false;
	Input::InjectGamepadState(0, pad);
	editorFrames(1);
	ok("play.step");
	editorFrames(2);
	CHECK(edges() == std::vector<InputActionEdge>{{0, true}, {0, false}});
	REQUIRE(Input::SetActionOverride("Jump", glm::vec2(1.0f, 0.0f)));
	editorFrames(2);
	Input::ClearActionOverride("Jump");
	advance();
	CHECK(edges() == std::vector<InputActionEdge>{{0, true}, {0, false}});

	// Stopping ends what was put in place of devices.
	REQUIRE(Input::SetActionOverride("Jump", glm::vec2(1.0f, 0.0f)));
	CHECK(ok("play.stop")["state"] == "edit");
	CHECK(Input::GetActionOverrides().empty());
	CHECK_FALSE(Input::IsGamepadInjected(0));
	CHECK(fixture.Context.GetScene().GetInputActions().GetActions() == nullptr);
}

TEST_CASE("PlayCommands: input.set holds the project's action overrides and injected gamepads, checks every entry before it "
          "changes anything, and input.release and play.stop end them")
{
	Testing::PhysicsSystemScope physics;
	InputScope input;
	ProjectPlayFixture fixture;
	CommandRegistry registry;
	REQUIRE(RegisterPlayCommands(registry, fixture.Play, fixture.Context,
	                             []
	                             {
									 return glm::uvec2(640, 480);
								 })
	            .IsOk());
	auto const run = [&registry](std::string_view name, Json params)
	{
		std::optional<CommandResult> result;
		registry.Execute(name, std::move(params),
		                 [&result](CommandResult value)
		                 {
							 result.emplace(std::move(value));
						 });
		REQUIRE(result.has_value());
		return std::move(*result);
	};
	auto const ok = [&run](std::string_view name, Json params = Json::object())
	{
		CommandResult result = run(name, std::move(params));
		REQUIRE_MESSAGE(result.IsOk(), (result.IsError() ? result.GetError().Message : std::string()));
		return result.TakeValue();
	};
	// A refusal changes nothing: no key, override or injection.
	auto const refused = [&run](Json params)
	{
		uint64_t const overridesBefore = Input::GetActionOverridesVersion();
		std::vector<std::optional<Input::GamepadState>> padsBefore;
		for (uint32_t slot = 0; slot < MaxGamepads; slot++)
		{
			padsBefore.push_back(Input::GetInjectedGamepadState(slot));
		}
		CommandResult const answer = run("input.set", std::move(params));
		REQUIRE(answer.IsError());
		CHECK(Input::GetActionOverridesVersion() == overridesBefore);
		for (uint32_t slot = 0; slot < MaxGamepads; slot++)
		{
			std::optional<Input::GamepadState> const now = Input::GetInjectedGamepadState(slot);
			REQUIRE(now.has_value() == padsBefore[slot].has_value());
			if (now)
			{
				CHECK(now->Buttons == padsBefore[slot]->Buttons);
				CHECK(now->Axes == padsBefore[slot]->Axes);
			}
		}
		CHECK_FALSE(Input::IsKeyDown(KeyCode::A));
		return answer.GetError();
	};
	constexpr float Frame = 1.0f / 60.0f;
	auto const editorFrames = [&fixture](int count)
	{
		for (int frame = 0; frame < count; frame++)
		{
			fixture.Play.Update(Timestep(Frame));
			Input::EndApplicationFrame();
		}
	};
	auto const advance = [&ok, &fixture]
	{
		CHECK(ok("play.advance", Json::object({{"frames", 1}}))["framesRun"] == 1);
		std::span<InputActionEdge const> const listed = fixture.Context.GetScene().GetInputActions().GetEdges();
		return std::vector<InputActionEdge>(listed.begin(), listed.end());
	};

	// A project that names no actions refuses every name, saying so; an empty map asks for nothing.
	CommandError const unnamed =
		refused(Json::object({{"keys", Json::object({{"A", true}})}, {"actions", Json::object({{"Jump", true}})}}));
	CHECK(unnamed.Code == AutomationErrorCode::InvalidOperation);
	CHECK(unnamed.Message.find("the project names no input actions") != std::string::npos);
	CHECK(ok("input.set", Json::object({{"actions", Json::object()}}))["actions"] == Json::object());

	DataAssetType const* const type = DataAssetRegistry::FindByName("InputActions");
	REQUIRE(type != nullptr);
	Result<AssetHandle> const actions =
		fixture.Operations.CreateDataAsset(type->GetAssetType(), "Input/Game.sinput", Json::parse(R"({ "Actions": [
		{ "Name": "Jump", "Bindings": [ { "Controls": "Key/Space" }, { "Controls": "Gamepad/A" } ] },
		{ "Name": "Steer", "Type": "Axis", "Bindings": [ { "Controls": "Gamepad/LeftX" } ] },
		{ "Name": "Move", "Type": "Vector2", "Bindings": [ { "Controls": "Gamepad/LeftStick", "Gamepad": 1 } ] } ] })"));
	REQUIRE(actions.IsOk());
	// A reference to an asset of another type is no actions either.
	Result<AssetHandle> const material = fixture.Operations.CreateMaterial("Materials/Paint.smat");
	REQUIRE(material.IsOk());
	REQUIRE(fixture.Operations.ApplyProjectSettings(Json::object({{"InputActions", material.GetValue().ToString()}})).IsOk());
	CHECK(refused(Json::object({{"actions", Json::object({{"Jump", true}})}})).Message.find("does not load as an InputActions asset") !=
	      std::string::npos);
	REQUIRE(fixture.Operations.ApplyProjectSettings(Json::object({{"InputActions", actions.GetValue().ToString()}})).IsOk());

	// Overrides by name: a bool or a number for Button and Axis actions, [x, y] for Vector2; the answer lists those held.
	Json const set =
		ok("input.set", Json::object({{"actions", Json::object({{"Jump", true}, {"Steer", -0.5}, {"Move", Json::array({0.6, 0.8})}})}}));
	CHECK(set["actions"] ==
	      Json::object({{"Jump", Json::array({1.0f, 0.0f})}, {"Move", Json::array({0.6f, 0.8f})}, {"Steer", Json::array({-0.5f, 0.0f})}}));
	REQUIRE(Input::FindActionOverride("Move") != nullptr);
	CHECK(Input::FindActionOverride("Move")->Value == glm::vec2(0.6f, 0.8f));
	CHECK(Input::FindActionOverride("Move")->Held);

	// Each of these is refused, and nothing of the request changes, the valid entries beside it included.
	for (Json const& actionEntries :
	     {Json::object({{"Missing", 1}}), Json::object({{"jump", 1}}), Json::object({{"Jump", Json::array({1.0, 0.0})}}),
	      Json::object({{"Move", 1}}), Json::object({{"Move", Json::array({1.0})}}), Json::object({{"Move", Json::array({1.0, "up"})}}),
	      Json::object({{"Steer", "left"}}), Json::object({{"Steer", 1.0e308}}),
	      Json::object({{"Move", Json::array({std::numeric_limits<double>::quiet_NaN(), 0.0})}}),
	      Json::object({{"Jump", Json()}, {"Missing", 1}})})
	{
		CAPTURE(actionEntries.dump());
		CHECK(refused(Json::object({{"keys", Json::object({{"A", true}})}, {"actions", actionEntries}})).Code ==
		      AutomationErrorCode::InvalidParams);
	}
	CHECK(refused(Json::object({{"actions", Json::object({{"Missing", 1}})}})).Message.find("Jump, Steer, Move") != std::string::npos);
	CHECK(Input::FindActionOverride("Jump")->Held);

	// Null clears an override: the action's record stays, not held.
	CHECK(ok("input.set", Json::object({{"actions", Json::object({{"Jump", Json()}, {"Steer", Json()}})}}))["actions"] ==
	      Json::object({{"Move", Json::array({0.6f, 0.8f})}}));
	CHECK_FALSE(Input::FindActionOverride("Jump")->Held);

	// Gamepads by slot: names and values checked first, as actions are.
	for (Json const& padEntries :
	     {Json::object({{"16", Json::object()}}), Json::object({{"01", Json::object()}}), Json::object({{"-1", Json::object()}}),
	      Json::object({{" 1", Json::object()}}), Json::object({{"one", Json::object()}}),
	      Json::object({{"0", Json::object({{"buttons", Json::object({{"Nope", true}})}})}}),
	      Json::object({{"0", Json::object({{"axes", Json::object({{"Nope", 0.0}})}})}}),
	      Json::object({{"0", Json::object({{"axes", Json::object({{"LeftX", 2.0}})}})}}),
	      Json::object({{"0", Json::object({{"axes", Json::object({{"LeftX", std::numeric_limits<double>::quiet_NaN()}})}})}}),
	      Json::object({{"0", Json::object({{"buttons", Json::object({{"A", true}})}})},
	                    {"1", Json::object({{"buttons", Json::object({{"B", 1}})}})}})})
	{
		CAPTURE(padEntries.dump());
		CHECK(refused(Json::object({{"keys", Json::object({{"A", true}})}, {"gamepads", padEntries}})).Code ==
		      AutomationErrorCode::InvalidParams);
	}

	// A slot's first injection starts connected and at rest, triggers at -1; a later one changes only what it names.
	Json const injected =
		ok("input.set", Json::object({{"gamepads", Json::object({{"0", Json::object({{"buttons", Json::object({{"A", true}})}})}})}}));
	CHECK(injected["gamepads"]["0"] ==
	      Json::object(
			  {{"connected", true},
	           {"buttons", Json::array({"A"})},
	           {"axes",
	            Json::object(
					{{"LeftX", 0.0}, {"LeftY", 0.0}, {"RightX", 0.0}, {"RightY", 0.0}, {"LeftTrigger", -1.0}, {"RightTrigger", -1.0}})}}));
	CHECK(injected["gamepads"].size() == 1);
	ok("input.set",
	   Json::object(
		   {{"gamepads", Json::object({{"0", Json::object({{"axes", Json::object({{"LeftX", 0.5}, {"RightTrigger", 1.0}})}})}})}}));
	CHECK(Input::IsGamepadButtonDown(0, GamepadButton::A));
	CHECK(Input::GetGamepadAxis(0, GamepadAxis::LeftX) == 0.5f);
	CHECK(Input::GetGamepadAxis(0, GamepadAxis::RightTrigger) == 1.0f);
	CHECK(Input::GetGamepadAxis(0, GamepadAxis::LeftTrigger) == -1.0f);

	// A window's poll writes every slot (Window::PollGamepads): the injected one keeps its state, the others take the device's.
	for (uint32_t slot = 0; slot < MaxGamepads; slot++)
	{
		Input::SetGamepadState(slot, Input::GamepadState{});
	}
	CHECK(Input::IsGamepadConnected(0));
	CHECK(Input::IsGamepadButtonDown(0, GamepadButton::A));
	CHECK(Input::GetGamepadAxis(0, GamepadAxis::LeftX) == 0.5f);
	CHECK_FALSE(Input::IsGamepadConnected(1));
	// Null gives the slot back to its device: the state the poll gave it, a pad that is not connected.
	CHECK(ok("input.set", Json::object({{"gamepads", Json::object({{"0", Json()}})}}))["gamepads"] == Json::object());
	CHECK_FALSE(Input::IsGamepadInjected(0));
	CHECK_FALSE(Input::IsGamepadConnected(0));
	ok("input.set", Json::object({{"actions", Json::object({{"Move", Json()}})}}));

	// While the game is paused, what input.set gives reaches the next frame that runs, once, whatever editor frames come
	// between, and polls between them leave the injected pad alone.
	REQUIRE(ok("play.start", Json::object({{"paused", true}}))["paused"] == true);
	editorFrames(2);
	ok("input.set", Json::object({{"actions", Json::object({{"Jump", true}})}}));
	editorFrames(3);
	CHECK(advance() == std::vector<InputActionEdge>{{0, true}});
	CHECK(advance().empty());
	ok("input.set", Json::object({{"actions", Json::object({{"Jump", Json()}})}}));
	editorFrames(1);
	CHECK(advance() == std::vector<InputActionEdge>{{0, false}});
	// Pad 1's button A presses Jump, and its left stick moves Steer (bound on any pad's LeftX) and Move (pad 1's stick).
	ok("input.set", Json::object({{"gamepads", Json::object({{"1", Json::object({{"buttons", Json::object({{"A", true}})},
	                                                                             {"axes", Json::object({{"LeftX", 1.0}})}})}})}}));
	for (int poll = 0; poll < 3; poll++)
	{
		for (uint32_t slot = 0; slot < MaxGamepads; slot++)
		{
			Input::SetGamepadState(slot, Input::GamepadState{});
		}
		editorFrames(1);
	}
	CHECK(advance() == std::vector<InputActionEdge>{{0, true}, {1, true}, {2, true}});
	CHECK(fixture.Context.GetScene().GetInputActions().GetVector(2) == glm::vec2(1.0f, 0.0f));
	// A tap given while paused, set and cleared between two frames, reaches the next frame as both edges.
	ok("input.set", Json::object({{"gamepads", Json::object({{"1", Json()}})}}));
	editorFrames(1);
	CHECK(advance() == std::vector<InputActionEdge>{{0, false}, {1, false}, {2, false}});
	ok("input.set", Json::object({{"actions", Json::object({{"Steer", 1.0}})}}));
	editorFrames(1);
	ok("input.set", Json::object({{"actions", Json::object({{"Steer", Json()}})}}));
	editorFrames(1);
	CHECK(advance() == std::vector<InputActionEdge>{{1, true}, {1, false}});

	// input.release ends both; so does play.stop.
	Json pad = Json::object({{"buttons", Json::object({{"Start", true}})}});
	ok("input.set", Json::object({{"actions", Json::object({{"Jump", 1}})}, {"gamepads", Json::object({{"3", pad}})}}));
	Json const released = ok("input.release");
	CHECK(released["actions"] == Json::object());
	CHECK(released["gamepads"] == Json::object());
	CHECK_FALSE(Input::IsGamepadInjected(3));
	CHECK(advance() == std::vector<InputActionEdge>{{0, true}, {0, false}});
	ok("input.set", Json::object({{"actions", Json::object({{"Jump", 1}})}, {"gamepads", Json::object({{"3", pad}})}}));
	CHECK(ok("play.stop")["state"] == "edit");
	CHECK(Input::GetActionOverrides().empty());
	CHECK_FALSE(Input::IsGamepadInjected(3));

	// Overrides are global, so they are set without a game playing too (held until input.release, a null or play.stop).
	CHECK(ok("input.set", Json::object({{"actions", Json::object({{"Jump", 1}})}}))["actions"] ==
	      Json::object({{"Jump", Json::array({1.0f, 0.0f})}}));
}

TEST_CASE("PlayCommands: input.set refuses an override of a new name once the input keeps the most names it can, still sets the "
          "names that have one, and has room again once the game stops")
{
	InputScope input;
	ProjectPlayFixture fixture;
	CommandRegistry registry;
	REQUIRE(RegisterPlayCommands(registry, fixture.Play, fixture.Context,
	                             []
	                             {
									 return glm::uvec2(640, 480);
								 })
	            .IsOk());
	auto const run = [&registry](Json params)
	{
		std::optional<CommandResult> result;
		registry.Execute("input.set", std::move(params),
		                 [&result](CommandResult value)
		                 {
							 result.emplace(std::move(value));
						 });
		REQUIRE(result.has_value());
		return std::move(*result);
	};
	DataAssetType const* const type = DataAssetRegistry::FindByName("InputActions");
	REQUIRE(type != nullptr);
	Result<AssetHandle> const actions =
		fixture.Operations.CreateDataAsset(type->GetAssetType(), "Input/Game.sinput",
	                                       Json::parse(R"({ "Actions": [ { "Name": "Jump" }, { "Name": "Steer", "Type": "Axis" } ] })"));
	REQUIRE(actions.IsOk());
	REQUIRE(fixture.Operations.ApplyProjectSettings(Json::object({{"InputActions", actions.GetValue().ToString()}})).IsOk());

	// Jump has a record; the names scripts overrode fill the rest, cleared or not (a cleared override keeps its record).
	REQUIRE(run(Json::object({{"actions", Json::object({{"Jump", true}})}})).IsOk());
	for (size_t index = 1; index < Input::MaxActionOverrides; index++)
	{
		REQUIRE(Input::SetActionOverride("Script" + std::to_string(index), glm::vec2(1.0f, 0.0f)));
		Input::ClearActionOverride("Script" + std::to_string(index));
	}

	// A new name is refused before anything of the request changes; Jump, and a clear of a name without one, are not.
	uint64_t const version = Input::GetActionOverridesVersion();
	CommandResult const full = run(Json::object({{"actions", Json::object({{"Jump", 0.5}, {"Steer", 1.0}})}}));
	REQUIRE(full.IsError());
	CHECK(full.GetError().Code == AutomationErrorCode::InvalidOperation);
	CHECK(full.GetError().Message.find("names that have one can still be set") != std::string::npos);
	CHECK(Input::GetActionOverridesVersion() == version);
	CHECK(Input::FindActionOverride("Steer") == nullptr);
	CHECK(Input::FindActionOverride("Jump")->Value == glm::vec2(1.0f, 0.0f));
	CommandResult const kept = run(Json::object({{"actions", Json::object({{"Jump", 0.5}, {"Steer", Json()}})}}));
	REQUIRE(kept.IsOk());
	CHECK(kept.GetValue()["actions"] == Json::object({{"Jump", Json::array({0.5f, 0.0f})}}));

	// The end of a game ends its overrides, and their records with them: a new name has room again.
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	fixture.Play.Stop();
	CHECK_FALSE(Input::HasActionOverrides());
	CommandResult const room = run(Json::object({{"actions", Json::object({{"Steer", 1.0}})}}));
	REQUIRE(room.IsOk());
	CHECK(room.GetValue()["actions"] == Json::object({{"Steer", Json::array({1.0f, 0.0f})}}));
}

TEST_CASE("PlayMode: frames refused at a stop keep the game's input for the first frame that runs, and Start and Stop begin a "
          "clean input frame")
{
	InputScope input;
	PlayFixture fixture;
	RecordingObserver observer(fixture.Context);
	ObserverGuard const guard(fixture.Play);
	fixture.Play.SetObserver(&observer);
	constexpr float Frame = 1.0f / 60.0f;

	// What came before playing is not the game's; a key held stays down.
	Input::SetKeyState(KeyCode::W, true);
	Input::AddScrollDelta({0.0f, 3.0f});
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	CHECK(Input::IsFrameOwned());
	CHECK_FALSE(Input::IsKeyPressed(KeyCode::W));
	CHECK(Input::IsKeyDown(KeyCode::W));
	CHECK(Input::GetScrollDelta() == glm::vec2(0.0f));

	// While an execution is stopped no frame runs, and the editor's frames keep the press for the first frame that does.
	Scene& scene = fixture.Context.GetScene();
	observer.Refusal = "an execution is stopped at a breakpoint (stop 1): graph.continue or graph.step it first";
	Input::SetKeyState(KeyCode::Space, true);
	for (int frame = 0; frame < 3; frame++)
	{
		fixture.Play.Update(Timestep(Frame));
		Input::EndApplicationFrame();
	}
	CHECK(scene.GetRuntimeFrame() == 0);
	CHECK(Input::IsKeyPressed(KeyCode::Space));
	observer.Refusal.reset();
	fixture.Play.Update(Timestep(Frame));
	CHECK(scene.GetRuntimeFrame() == 1);
	CHECK_FALSE(Input::IsKeyPressed(KeyCode::Space));
	CHECK(Input::IsKeyDown(KeyCode::Space));

	// Stopping releases the keys held and begins a clean frame of the application's.
	REQUIRE(fixture.Play.SetPaused(true));
	Input::SetMouseButtonState(MouseButton::Left, true);
	fixture.Play.Stop();
	CHECK_FALSE(Input::IsFrameOwned());
	CHECK_FALSE(Input::IsKeyDown(KeyCode::Space));
	CHECK_FALSE(Input::IsKeyReleased(KeyCode::Space));
	CHECK_FALSE(Input::IsMouseButtonPressed(MouseButton::Left));
	CHECK_FALSE(Input::IsMouseButtonDown(MouseButton::Left));
	fixture.Play.SetObserver(nullptr);
}

TEST_CASE("PlayCommands: test runs report the scripts' checks and time out")
{
	Testing::ScriptEngineScope scripting;
	PlayFixture fixture;
	UUID const tester = fixture.Create("Tester", Json::object({{"Script", Json::object({{"ClassName", "Strada.Tests.SelfTest"}})}}));

	Json const passed = fixture.Ok("test.run");
	CAPTURE(passed.dump());
	CHECK(passed["finished"] == true);
	CHECK(passed["timedOut"] == false);
	CHECK(passed["frames"] == 1);
	CHECK(passed["passed"] == 2);
	CHECK(passed["failures"] == 0);
	CHECK_FALSE(fixture.Play.IsPlaying());

	REQUIRE(fixture.Operations
	            .SetComponentFields(tester, "Script",
	                                Json::object({{"Fields", Json::object({{"Fail", Json::object({{"Type", "Bool"}, {"Value", true}})}})}}))
	            .IsOk());
	Json const failed = fixture.Ok("test.run");
	CHECK(failed["failed"] == 1);
	CHECK(failed["results"][1]["message"] == "expected false: Fail is set");

	REQUIRE(fixture.Operations.DeleteEntities(std::array<UUID, 1>{tester}).IsOk());
	Json const timedOut = fixture.Ok("test.run", Json::object({{"timeout", 0.5}, {"timestep", 0.1}}));
	CHECK(timedOut["finished"] == false);
	CHECK(timedOut["timedOut"] == true);
	CHECK(timedOut["frames"] == 5);
	CHECK(fixture.Fails("test.run", Json::object({{"scene", "Scenes/Missing.sscene"}})) == AutomationErrorCode::AssetNotFound);
}

TEST_CASE("PlayCommands: input.set plays the Blocks sample: clicks and keys in its menus, Escape and P pause, R starts over")
{
	// The sample as a player gets it: its scenes and assets, with the scripts this build made of its sources.
	Testing::AssetManagerScope assets;
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting(FileSystem::GetExecutableDirectory() / "Blocks" / "Blocks.dll");
	InputScope input;
	Testing::TemporaryDirectory directory;
	std::filesystem::path const sample = FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR) / "Blocks";
	REQUIRE(FileSystem::CopyDirectory(sample / "Assets", directory.GetPath() / "Assets").IsOk());
	REQUIRE(FileSystem::Copy(sample / "Blocks.sproj", directory.GetPath() / "Blocks.sproj", false).IsOk());

	PlayFixture fixture;
	REQUIRE(fixture.Operations.OpenProject(directory.GetPath() / "Blocks.sproj").IsOk());
	// The best score stays out of the user's data folder, and the game-over screen soon goes back to the title; the game keeps
	// its other fields (its camera, its screen and its sounds).
	Entity const game = fixture.Context.GetScene().FindEntityByName("Game");
	REQUIRE(game);
	ScriptComponent script = game.GetComponent<ScriptComponent>();
	REQUIRE(script.Fields.contains("HudCanvas"));
	script.Fields["SaveBestScore"] = ScriptFieldValue::FromBool(false);
	script.Fields["TitleReturnDelay"] = ScriptFieldValue::FromFloat(2.0f);
	REQUIRE(fixture.Operations.SetComponent(game.GetUUID(), script).IsOk());

	CHECK(fixture.Ok("play.start")["state"] == "play");
	fixture.Ok("play.pause");
	auto const advance = [&fixture](uint32_t frames)
	{
		fixture.Ok("play.advance", Json::object({{"frames", frames}}));
	};
	// A key or button goes down for a frame, then up.
	auto const press = [&fixture, &advance](std::string const& device, std::string const& name)
	{
		fixture.Ok("input.set", Json::object({{device, Json::object({{name, true}})}}));
		advance(1);
		fixture.Ok("input.set", Json::object({{device, Json::object({{name, false}})}}));
		advance(1);
	};
	auto const find = [&fixture](std::string_view name)
	{
		Entity const entity = fixture.Context.GetScene().FindEntityByName(name);
		REQUIRE(entity);
		return entity;
	};
	// What a text of the game's screen says.
	auto const text = [&find](std::string_view name)
	{
		return find(name).GetComponent<UITextComponent>().Text;
	};
	// Whether a menu's button takes clicks: the menu of the game's state is open, the others are closed.
	auto const open = [&find](std::string_view button)
	{
		return find(button).GetComponent<UIButtonComponent>().Interactable;
	};
	// The left mouse button pressed and released over a button, where ui.layout says the game view shows it.
	auto const click = [&fixture, &find, &advance](std::string_view button)
	{
		Json const layout = fixture.Ok("ui.layout", Json::object({{"entity", std::to_string(find(button).GetUUID().GetValue())}}));
		fixture.Ok("input.set",
		           Json::object({{"mousePosition", layout["elements"][0]["center"]}, {"mouseButtons", Json::object({{"Left", true}})}}));
		advance(1);
		fixture.Ok("input.set", Json::object({{"mouseButtons", Json::object({{"Left", false}})}}));
		advance(1);
	};
	// Hard drops pieces until the stack reaches the top.
	auto const topOut = [&press, &open]
	{
		for (int piece = 0; piece < 40 && !open("Play Again Button"); piece++)
		{
			press("keys", "Space");
		}
		REQUIRE(open("Play Again Button"));
	};
	// Space pressed five times a second (each press a frame long) until the stack reaches the top, and on for a second and a
	// half: a player mashing hard drops as the game ends.
	auto const mashThroughTopOut = [&fixture, &advance, &open]
	{
		auto const tap = [&fixture, &advance]
		{
			fixture.Ok("input.set", Json::object({{"keys", Json::object({{"Space", true}})}}));
			advance(1);
			fixture.Ok("input.set", Json::object({{"keys", Json::object({{"Space", false}})}}));
			advance(11);
		};
		for (int piece = 0; piece < 40 && !open("Play Again Button"); piece++)
		{
			tap();
		}
		REQUIRE(open("Play Again Button"));
		for (int presses = 0; presses < 8; presses++)
		{
			tap();
		}
	};
	// The game-over menu takes keys half a second after it opens.
	auto const awaitKeys = [&advance]
	{
		advance(30);
	};

	// The game starts on its title with the camera already at the title's view, so the title's sign shows from the first
	// frame, over the board rather than on it. The game view (4:3) is narrower than the 16:10 the view was made for: the field
	// of view widens to show as much across.
	advance(1);
	glm::vec3 const cameraPlace = find("Main Camera").GetComponent<TransformComponent>().Translation;
	glm::vec3 const attractPlace = find("Attract View").GetComponent<TransformComponent>().Translation;
	CHECK(glm::distance(cameraPlace, attractPlace) < 0.05f);
	CHECK(find("Title Sign").GetComponent<UIRectComponent>().Opacity == 1.0f);
	float const fieldOfView = 2.0f * glm::degrees(std::atan(std::tan(glm::radians(21.0f)) * 1.6f / (640.0f / 480.0f)));
	CHECK(find("Main Camera").GetComponent<CameraComponent>().PerspectiveFOV == doctest::Approx(fieldOfView).epsilon(1e-4));

	// The title screen, where the board plays itself, starts a game with a click on Play.
	advance(59);
	CHECK(open("Play Button"));
	click("Play Button");
	CHECK_FALSE(open("Play Button"));
	CHECK(text("Score Value") == "0");

	// Escape pauses and Enter clicks Resume, which the pause menu focuses; P pauses and Escape goes on.
	press("keys", "Escape");
	CHECK(open("Resume Button"));
	press("keys", "Enter");
	CHECK_FALSE(open("Resume Button"));
	press("keys", "P");
	CHECK(open("Resume Button"));
	press("keys", "Escape");
	CHECK_FALSE(open("Resume Button"));

	// In the pause menu Down moves to Restart and Enter clicks it: the points of a hard drop go.
	press("keys", "Space");
	CHECK(text("Score Value") != "0");
	press("keys", "P");
	press("keys", "Down");
	press("keys", "Enter");
	CHECK_FALSE(open("Resume Button"));
	CHECK(text("Score Value") == "0");

	// R starts the game over.
	press("keys", "Space");
	CHECK(text("Score Value") != "0");
	press("keys", "R");
	CHECK(text("Score Value") == "0");

	// The game-over screen plays again with a click on Play Again (once the screen has faded in), with Enter (Play Again has
	// the focus) and with R, which plays again at once.
	topOut();
	advance(20);
	click("Play Again Button");
	CHECK_FALSE(open("Play Again Button"));
	CHECK(text("Score Value") == "0");
	// Space, the hard drop, stays the game's: mashed through the end of a game and on, it leaves the game-over screen up with
	// the game's score. Enter plays again.
	mashThroughTopOut();
	CHECK(open("Play Again Button"));
	CHECK(text("Final Score") != "0");
	press("keys", "Enter");
	CHECK_FALSE(open("Play Again Button"));
	CHECK(text("Score Value") == "0");
	// Keys pressed for the game as it ends do not act on the game-over menu: Down (a soft drop) at once leaves Play Again
	// focused, which Enter clicks once the menu takes keys.
	topOut();
	press("keys", "Down");
	awaitKeys();
	press("keys", "Enter");
	CHECK_FALSE(open("Play Again Button"));
	CHECK_FALSE(open("Play Button"));
	CHECK(text("Score Value") == "0");
	topOut();
	press("keys", "R");
	CHECK_FALSE(open("Play Again Button"));
	CHECK(text("Score Value") == "0");

	// Down and Enter click its Quit, though the mouse rests on Play Again: back to the title, where Enter clicks Play.
	topOut();
	awaitKeys();
	press("keys", "Down");
	press("keys", "Enter");
	CHECK(open("Play Button"));
	press("keys", "Enter");
	CHECK_FALSE(open("Play Button"));
	CHECK(text("Score Value") == "0");

	// Left alone, the game-over screen goes back to the title.
	topOut();
	advance(130);
	CHECK(open("Play Button"));

	// The title's Quit (Right, then Enter) ends the game, which ends play mode.
	press("keys", "Right");
	press("keys", "Enter");
	CHECK(fixture.Ok("play.state")["state"] == "edit");
}

TEST_CASE("PlayCommands: input.set plays the Prism Break sample, made of visual scripts alone: the title plays itself, Space starts "
          "and serves, the mouse moves the paddle, Escape pauses, a lost ball costs a life")
{
	PrismBreakPlay sample;
	sample.Start();

	// The title shows, and the game plays itself: the autopilot steers the paddle, and its ball breaks prisms. The mouse rests
	// over the game view meanwhile (a first position is no movement).
	sample.PointAt(320.0f);
	sample.Advance(1);
	CHECK(sample.GetState() == "Title");
	CHECK(sample.IsShown("Title Screen"));
	CHECK_FALSE(sample.IsShown("Game Bar"));
	sample.Advance(900);
	CHECK(sample.GetState() == "Title");
	CHECK(sample.IsShown("Title Screen"));
	CHECK(sample.ReadOut("Score").GetInt32() > 0);
	CHECK(sample.ReadOut("Lives").GetInt32() == 3);
	CHECK(sample.GetPlace("Paddle").x != 0.0f);

	// Space starts a game: the score and lives start fresh, the level builds prism by prism, and the ball waits on the paddle.
	sample.Press("Space");
	CHECK(sample.GetState() == "Serve");
	CHECK_FALSE(sample.IsShown("Title Screen"));
	CHECK(sample.ReadOut("Score").GetInt32() == 0);
	CHECK(sample.ReadOut("Lives").GetInt32() == 3);
	sample.Advance(60);
	CHECK(sample.ReadOut("BricksLeft").GetInt32() == 33);
	CHECK(sample.GetState() == "Serve");
	CHECK(sample.IsShown("Serve Prompt"));
	CHECK(sample.IsShown("Game Bar"));

	// The mouse moves the paddle across the arena, and the waiting ball rides it.
	sample.PointAt(600.0f);
	sample.Advance(30);
	CHECK(sample.GetPlace("Paddle").x > 5.0f);
	CHECK(sample.GetBallPlace().x == doctest::Approx(sample.GetPlace("Paddle").x).epsilon(0.02));
	sample.PointAt(40.0f);
	sample.Advance(30);
	CHECK(sample.GetPlace("Paddle").x < -5.0f);
	CHECK(sample.GetBallPlace().x == doctest::Approx(sample.GetPlace("Paddle").x).epsilon(0.02));
	sample.PointAt(320.0f);
	sample.Advance(30);

	// Space serves: the ball leaves the paddle for the prisms.
	float const served = sample.GetBallPlace().z;
	sample.Press("Space");
	CHECK(sample.GetState() == "Play");
	CHECK_FALSE(sample.IsShown("Serve Prompt"));
	sample.Advance(10);
	CHECK(sample.GetBallPlace().z < served - 1.0f);

	// Escape pauses: the pause screen shows and the ball holds still; Escape again plays on.
	sample.Press("Escape");
	CHECK(sample.IsShown("Pause Screen"));
	glm::vec3 const held = sample.GetBallPlace();
	sample.Advance(30);
	CHECK(sample.GetBallPlace() == held);
	sample.Press("Escape");
	CHECK_FALSE(sample.IsShown("Pause Screen"));
	sample.Advance(5);
	CHECK(sample.GetBallPlace() != held);

	// The paddle kept away from the ball lets it fall into the drain: a life goes, its icon with it, and the next ball waits
	// on the paddle.
	for (int frame = 0; frame < 1200 && sample.ReadOut("Lives").GetInt32() == 3; frame++)
	{
		std::vector<Entity> const inPlay = sample.GetChildren("Balls");
		if (!inPlay.empty())
		{
			sample.PointAt(inPlay.front().GetComponent<TransformComponent>().Translation.x > 0.0f ? 40.0f : 600.0f);
		}
		sample.Advance(1);
	}
	CHECK(sample.ReadOut("Lives").GetInt32() == 2);
	CHECK(sample.IsShown("Life 2"));
	CHECK_FALSE(sample.IsShown("Life 3"));
	sample.Advance(90);
	CHECK(sample.GetState() == "Serve");
	CHECK(sample.GetChildren("Balls").size() == 1);
	sample.Stop();
}

TEST_CASE("PlayCommands: input.set starts a Prism Break game while its title plays itself: a start as the title clears a level or "
          "loses its ball keeps the player's game, the title's points are no best, and the keys turn the paddle at a wall")
{
	PrismBreakPlay sample;
	sample.Start();
	sample.PointAt(320.0f);
	sample.Advance(900);
	REQUIRE(sample.GetState() == "Title");
	CHECK(sample.ReadOut("Attract").GetBool());
	CHECK(sample.ReadOut("Score").GetInt32() > 0);
	CHECK(sample.GetText("Best Text") == "0");

	// The title's next break clears its level: the title stays up, without the game bar or the level banner.
	sample.SetVariable("Game", "BricksLeft", ScriptFieldValue::FromInt32(1));
	for (int frame = 0; frame < 1800 && sample.ReadOut("LastBroadcastCount").GetInt32() == 0; frame++)
	{
		sample.Advance(1);
	}
	REQUIRE(sample.ReadOut("LastBroadcastCount").GetInt32() == 8);
	CHECK(sample.GetState() == "Title");
	CHECK(sample.IsShown("Title Screen"));
	CHECK_FALSE(sample.IsShown("Game Bar"));
	CHECK_FALSE(sample.IsShown("Banner"));

	// Space, while the title waits to build its next level, starts a game on the first level, which the title's clear
	// leaves alone once its wait is over.
	sample.Advance(30);
	sample.Press("Space");
	CHECK(sample.GetState() == "Serve");
	CHECK_FALSE(sample.ReadOut("Attract").GetBool());
	sample.Advance(240);
	CHECK(sample.GetState() == "Serve");
	CHECK(sample.ReadOut("Level").GetInt32() == 1);
	CHECK(sample.ReadOut("BricksLeft").GetInt32() == 33);
	CHECK(sample.GetChildren("Bricks").size() == 33);
	CHECK_FALSE(sample.IsShown("Title Screen"));
	CHECK(sample.IsShown("Game Bar"));
	CHECK(sample.GetText("Score Text") == "0");
	CHECK(sample.GetText("Best Text") == "0");

	// D carries the paddle to the right wall; A turns it round at once, as the keys' target never passes the wall.
	sample.Hold("D", true);
	sample.Advance(60);
	sample.Hold("D", false);
	float const atWall = sample.GetPlace("Paddle").x;
	CHECK(atWall == doctest::Approx(8.5f - 3.2f * 0.5f).epsilon(0.01));
	sample.Hold("A", true);
	sample.Advance(5);
	sample.Hold("A", false);
	CHECK(sample.GetPlace("Paddle").x < atWall - 0.3f);
	sample.Stop();

	// Played again, the title's autopilot lets its ball go into the drain, and Space starts a game before the title serves
	// itself again: the player's ball waits on the paddle, which nothing steers but the player.
	sample.Start();
	sample.Advance(120);
	REQUIRE(sample.GetState() == "Title");
	sample.SetVariable("Paddle", "Autopilot", ScriptFieldValue::FromBool(false));
	bool drained = false;
	for (int frame = 0; frame < 1800 && !drained; frame++)
	{
		sample.Advance(1);
		drained = sample.GetChildren("Balls").empty();
	}
	REQUIRE(drained);
	CHECK(sample.GetState() == "Title");
	sample.Press("Space");
	CHECK(sample.GetState() == "Serve");
	sample.Advance(240);
	CHECK(sample.GetState() == "Serve");
	CHECK_FALSE(sample.ReadOut("Attract").GetBool());
	CHECK_FALSE(sample.ReadOut("Autopilot", "Paddle").GetBool());
	CHECK_FALSE(sample.IsShown("Title Screen"));
	CHECK(sample.ReadOut("Lives").GetInt32() == 3);
	CHECK(sample.GetChildren("Balls").size() == 1);
	sample.Stop();
}

TEST_CASE("PlayCommands: play mode flies the Moto sample's camera tour, a graph's timeline, from the home view and stopping brings "
          "the camera back; with PlayOnStart off the camera stays home")
{
	Testing::AssetManagerScope assets;
	Testing::PhysicsSystemScope physics;
	Testing::ScriptRuntimeScope scripting;
	InputScope input;
	Testing::TemporaryDirectory directory;
	PlayFixture fixture;
	std::filesystem::path const sample = FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR) / "Moto";
	REQUIRE(FileSystem::CopyDirectory(sample / "Assets", directory.GetPath() / "Assets").IsOk());
	REQUIRE(FileSystem::Copy(sample / "Moto.sproj", directory.GetPath() / "Moto.sproj", false).IsOk());
	REQUIRE(fixture.Operations.OpenProject(directory.GetPath() / "Moto.sproj").IsOk());

	auto const findCamera = [&fixture]
	{
		Entity const found = fixture.Context.GetScene().FindEntityByName("Main Camera");
		REQUIRE(found);
		return found;
	};
	auto const readTours = [&findCamera]
	{
		Result<ScriptFieldMap> const variables = VisualScriptEngine::GetInstanceVariables(findCamera().GetUUID());
		REQUIRE_MESSAGE(variables.IsOk(), (variables ? std::string() : variables.GetError()));
		auto const tours = variables.GetValue().find("Tours");
		REQUIRE(tours != variables.GetValue().end());
		return tours->second.GetInt32();
	};
	glm::vec3 const home = findCamera().GetComponent<TransformComponent>().Translation;

	// Ten seconds in, the tour has flown the camera hundreds of meters down the circuit (Tools/Tour.py puts it 473 m from
	// the home view), and no tour has come home yet.
	CHECK(fixture.Ok("play.start")["state"] == "play");
	fixture.Ok("play.pause");
	fixture.Ok("play.advance", Json::object({{"frames", 600}}));
	CHECK(glm::distance(findCamera().GetComponent<TransformComponent>().Translation, home) > 100.0f);
	CHECK(readTours() == 0);
	CHECK(fixture.Ok("play.stop")["state"] == "edit");
	CHECK(findCamera().GetComponent<TransformComponent>().Translation == home);

	// The camera's PlayOnStart off, as a graph that starts the tour itself would set it: the camera waits at the home view.
	Entity const camera = findCamera();
	VisualScriptComponent tour = camera.GetComponent<VisualScriptComponent>();
	tour.Variables["PlayOnStart"] = ScriptFieldValue::FromBool(false);
	REQUIRE(fixture.Operations.SetComponent(camera.GetUUID(), tour).IsOk());
	CHECK(fixture.Ok("play.start")["state"] == "play");
	fixture.Ok("play.pause");
	fixture.Ok("play.advance", Json::object({{"frames", 120}}));
	CHECK(findCamera().GetComponent<TransformComponent>().Translation == home);
	CHECK(readTours() == 0);
	CHECK(fixture.Ok("play.stop")["state"] == "edit");
}

TEST_CASE("PlayMode: the observer is told of the start, of each frame that ran with its time step and of the stop, and may pause "
          "after a frame")
{
	PlayFixture fixture;
	fixture.Create("Crate");
	RecordingObserver observer(fixture.Context);
	ObserverGuard const guard(fixture.Play);
	fixture.Play.SetObserver(&observer);
	using Call = RecordingObserver::Call;
	constexpr float Frame = 1.0f / 60.0f;

	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	CHECK_FALSE(observer.StartedPlaying);
	REQUIRE(observer.Started.has_value());
	CHECK(observer.Started->first == EditorPlayState::Play);
	CHECK_FALSE(observer.Started->second.TestRun);
	CHECK_FALSE(observer.Started->second.Paused);
	// After the start, the scene started and ran no frame (a zero time step).
	CHECK(observer.Calls == std::vector<Call>{{"Starting", 0, 0.0f, false}, {"Frame", 0, 0.0f, false}});

	// Editor frames: one call per frame that ran, with the time step before the scene's time scale.
	fixture.Context.GetScene().SetTimeScale(0.5f);
	fixture.Play.Update(Timestep(Frame));
	CHECK(observer.Calls.back() == Call{"Frame", 1, Frame, false});
	CHECK(fixture.Context.GetScene().GetRuntimeTime() == doctest::Approx(Frame * 0.5f));
	fixture.Context.GetScene().SetTimeScale(1.0f);
	REQUIRE(fixture.Play.SetPaused(true));
	size_t const calls = observer.Calls.size();
	fixture.Play.Update(Timestep(Frame));
	CHECK(observer.Calls.size() == calls);
	fixture.Play.Step(2);
	for (int frame = 0; frame < 3; frame++)
	{
		fixture.Play.Update(Timestep(Frame));
	}
	REQUIRE(observer.Calls.size() == calls + 2);
	CHECK(observer.Calls[calls + 1] == Call{"Frame", 3, Frame, true});

	// Advance tells it of each frame, and stops early when it pauses the scene after one.
	CHECK(fixture.Play.Advance(2, Timestep(0.5f)) == 2);
	CHECK(observer.Calls.back() == Call{"Frame", 5, 0.5f, true});
	REQUIRE(fixture.Play.SetPaused(false));
	observer.PauseAfterFrame = 7;
	CHECK(fixture.Play.Advance(5, Timestep(0.25f)) == 2);
	CHECK(observer.Calls.back() == Call{"Frame", 7, 0.25f, false});
	CHECK(fixture.Play.IsPaused());
	// An editor frame after which it pauses the scene holds it there.
	REQUIRE(fixture.Play.SetPaused(false));
	observer.PauseAfterFrame = 8;
	fixture.Play.Update(Timestep(Frame));
	CHECK(fixture.Play.IsPaused());
	fixture.Play.Update(Timestep(Frame));
	CHECK(fixture.Context.GetScene().GetRuntimeFrame() == 8);

	// Before the scene stops, while it still runs.
	fixture.Play.Stop();
	CHECK(observer.Calls.back() == Call{"Stopping", 8, 0.0f, false});
	CHECK(std::count_if(observer.Calls.begin(), observer.Calls.end(),
	                    [](Call const& call)
	                    {
							return call.Kind == "Stopping";
						}) == 1);

	// A pause the observer asks for after the start holds the scene before its first frame.
	observer.Calls.clear();
	observer.PauseAfterFrame = 0;
	REQUIRE(fixture.Play.Start(EditorPlayState::Simulate, 640, 480).IsOk());
	CHECK(observer.Started->first == EditorPlayState::Simulate);
	CHECK(fixture.Play.IsPaused());
	fixture.Play.Update(Timestep(Frame));
	CHECK(fixture.Context.GetScene().GetRuntimeFrame() == 0);
	fixture.Play.Stop();

	// Without an observer play mode runs as before, and reports the debugger detached.
	fixture.Play.SetObserver(nullptr);
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	CHECK(fixture.Play.Advance(3, Timestep(Frame)) == 3);
	CHECK_FALSE(fixture.Play.GetFrameStepRefusal().has_value());
	CHECK(fixture.Play.GetDebugSummary().State == "Detached");
	fixture.Play.Stop();
	CHECK(observer.Calls.size() == 3);
}

TEST_CASE("PlayMode: PlayStartOptions start the scene paused before its first frame, and tell the observer of test runs")
{
	Testing::ScriptEngineScope scripting;
	PlayFixture fixture;
	UUID const reporter =
		fixture.Create("Reporter", Json::object({{"Script", Json::object({{"ClassName", "Strada.Tests.FieldReporter"}})}}));
	RecordingObserver observer(fixture.Context);
	ObserverGuard const guard(fixture.Play);
	fixture.Play.SetObserver(&observer);
	auto const updates = [reporter]
	{
		Result<ScriptFieldMap> const fields = ScriptEngine::GetInstanceFields(reporter);
		REQUIRE(fields.IsOk());
		return fields.GetValue().at("Updates");
	};

	PlayStartOptions paused;
	paused.Paused = true;
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480, nullptr, paused).IsOk());
	REQUIRE(observer.Started.has_value());
	CHECK(observer.Started->second.Paused);
	// Its scripts were created; none updated, and editor frames hold it.
	CHECK(fixture.Play.IsPaused());
	CHECK(ScriptEngine::HasInstance(reporter));
	CHECK(observer.Calls.back() == RecordingObserver::Call{"Frame", 0, 0.0f, true});
	fixture.Play.Update(Timestep(1.0f / 60.0f));
	CHECK(fixture.Context.GetScene().GetRuntimeFrame() == 0);
	CHECK(updates() == ScriptFieldValue::FromInt32(0));
	CHECK(fixture.Play.Advance(1, Timestep(1.0f / 60.0f)) == 1);
	CHECK(updates() == ScriptFieldValue::FromInt32(1));
	fixture.Play.Stop();

	// play.start's paused does the same; test.run's runs are test runs.
	Json const started = fixture.Ok("play.start", Json::object({{"paused", true}}));
	CHECK(started["paused"] == true);
	CHECK(started["frame"] == 0);
	CHECK(observer.Started->second.Paused);
	CHECK_FALSE(observer.Started->second.TestRun);
	fixture.Ok("play.stop");
	CHECK_FALSE(fixture.Ok("play.start")["paused"].get<bool>());
	fixture.Ok("play.stop");
	fixture.Ok("test.run", Json::object({{"timeout", 0.05}}));
	CHECK(observer.Started->second.TestRun);
	CHECK_FALSE(observer.Started->second.Paused);
	fixture.Play.SetObserver(nullptr);
}

TEST_CASE("PlayMode: while the observer refuses frames, unpausing, steps and frames are refused, and Resume continues through it")
{
	PlayFixture fixture;
	RecordingObserver observer(fixture.Context);
	ObserverGuard const guard(fixture.Play);
	fixture.Play.SetObserver(&observer);
	constexpr float Frame = 1.0f / 60.0f;
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	fixture.Play.Advance(2, Timestep(Frame));
	Scene& scene = fixture.Context.GetScene();
	REQUIRE(fixture.Play.SetPaused(true));

	// Steps and frames bring the observer up to date first: what it refuses may have changed since an editor frame.
	uint32_t const refreshes = observer.Refreshes;
	fixture.Play.Step(1);
	fixture.Play.CancelSteps();
	CHECK(observer.Refreshes == refreshes + 1);
	CHECK(fixture.Play.Advance(0, Timestep(Frame)) == 0);
	CHECK(observer.Refreshes == refreshes + 2);

	// Steps queued before the refusal never run while it holds, and the refusal refuses new ones.
	fixture.Play.Step(5);
	observer.Refusal = "an execution is stopped at a breakpoint (stop 3): graph.continue or graph.step it first";
	CHECK(fixture.Play.GetFrameStepRefusal() == observer.Refusal);
	for (int frame = 0; frame < 3; frame++)
	{
		fixture.Play.Update(Timestep(Frame));
	}
	CHECK(scene.GetRuntimeFrame() == 2);
	CHECK(fixture.Play.Advance(4, Timestep(Frame)) == 0);
	CHECK(scene.GetRuntimeFrame() == 2);
	fixture.Play.Step(1);
	// Unpausing is refused; pausing is not.
	CHECK_FALSE(fixture.Play.SetPaused(false));
	CHECK(fixture.Play.IsPaused());
	CHECK(fixture.Play.SetPaused(true));

	// Cancelled steps are gone once the refusal ends.
	fixture.Play.CancelSteps();
	observer.Refusal.reset();
	fixture.Play.Update(Timestep(Frame));
	CHECK(scene.GetRuntimeFrame() == 2);
	fixture.Play.Step(1);
	fixture.Play.Update(Timestep(Frame));
	CHECK(scene.GetRuntimeFrame() == 3);

	// Resume continues the stopped executions first: one that stops again keeps the scene paused; once none does, it runs.
	observer.Refusal = "stopped";
	observer.StopAgainOnResume = true;
	CHECK_FALSE(fixture.Play.Resume());
	CHECK(fixture.Play.IsPaused());
	CHECK(observer.Calls.back() == RecordingObserver::Call{"Resuming", 3, 0.0f, true});
	observer.StopAgainOnResume = false;
	CHECK(fixture.Play.Resume());
	CHECK_FALSE(fixture.Play.IsPaused());
	CHECK_FALSE(fixture.Play.GetFrameStepRefusal().has_value());
	fixture.Play.Update(Timestep(Frame));
	CHECK(scene.GetRuntimeFrame() == 4);

	// The debugger's summary comes from the observer.
	observer.Summary.State = "Stopped";
	observer.Summary.Stops = 2;
	observer.Summary.Current = 5;
	CHECK(fixture.Play.GetDebugSummary().Current == std::optional<uint64_t>(5));
	fixture.Play.Stop();
	CHECK_FALSE(fixture.Play.Resume());
	CHECK_FALSE(fixture.Play.SetPaused(true));
	fixture.Play.SetObserver(nullptr);
}

TEST_CASE("PlayMode: the scene the views show begins one frame of its world-transform cache per editor frame, also while no "
          "game frame runs")
{
	PlayFixture fixture;
	RecordingObserver observer(fixture.Context);
	ObserverGuard const guard(fixture.Play);
	fixture.Play.SetObserver(&observer);
	constexpr float Frame = 1.0f / 60.0f;

	// Nothing plays: the edited scene's frames are play mode's to begin.
	Scene& edited = fixture.Context.GetScene();
	uint64_t const editedStart = edited.GetTransformFrame();
	for (int frame = 0; frame < 3; frame++)
	{
		fixture.Play.Update(Timestep(Frame));
	}
	CHECK(edited.GetTransformFrame() == editedStart + 3);

	// Playing: the running scene begins its own in each update, paused or not, and each frame Advance runs begins one.
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	Scene& running = fixture.Context.GetScene();
	REQUIRE(&running != &edited);
	uint64_t const runningStart = running.GetTransformFrame();
	fixture.Play.Update(Timestep(Frame));
	fixture.Play.Update(Timestep(Frame));
	CHECK(running.GetTransformFrame() == runningStart + 2);
	CHECK(fixture.Play.Advance(3, Timestep(Frame)) == 3);
	CHECK(running.GetTransformFrame() == runningStart + 5);
	REQUIRE(fixture.Play.SetPaused(true));
	uint64_t const pausedFrame = running.GetRuntimeFrame();
	fixture.Play.Update(Timestep(Frame));
	CHECK(running.GetTransformFrame() == runningStart + 6);
	CHECK(running.GetRuntimeFrame() == pausedFrame);

	// The debugger holds the game: no game frame runs, and play mode begins the frame the views draw.
	observer.Refusal = "an execution is stopped at a breakpoint";
	fixture.Play.Update(Timestep(Frame));
	fixture.Play.Update(Timestep(Frame));
	CHECK(running.GetTransformFrame() == runningStart + 8);
	CHECK(running.GetRuntimeFrame() == pausedFrame);
	CHECK(edited.GetTransformFrame() == editedStart + 3);
	observer.Refusal.reset();
	fixture.Play.Stop();

	// Stopped: the edited scene's frames go on.
	fixture.Play.Update(Timestep(Frame));
	CHECK(edited.GetTransformFrame() == editedStart + 4);
	fixture.Play.SetObserver(nullptr);
}

TEST_CASE("PlayMode: FollowScripts loads the scene script code asked for between frames, and stops when it quit")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	Testing::TemporaryDirectory directory;
	std::filesystem::path const scenes = directory.GetPath() / "Scenes";
	REQUIRE(FileSystem::CreateDirectories(scenes).IsOk());
	WriteSwitcherScene(scenes / "First.sscene", "First", "Scenes/Second.sscene");
	WriteSwitcherScene(scenes / "Second.sscene", "Second", "");
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());
	PlayFixture fixture;
	REQUIRE(fixture.Operations.OpenScene(scenes / "First.sscene").IsOk());
	RecordingObserver observer(fixture.Context);
	ObserverGuard const guard(fixture.Play);
	fixture.Play.SetObserver(&observer);
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	fixture.Play.Advance(1, Timestep(1.0f / 60.0f));

	// What the scripts ask between frames (as a resumed execution does) waits for FollowScripts, which acts on it at once.
	fixture.Play.FollowScripts();
	CHECK(fixture.Context.GetScene().GetName() == "First");
	ScriptHost* const host = ScriptEngine::GetHost();
	REQUIRE(host != nullptr);
	host->RequestSceneLoad(AssetManager::FindByPath("Scenes/Second.sscene"));
	CHECK(fixture.Context.GetScene().GetName() == "First");
	uint64_t const version = fixture.Context.GetSceneVersion();
	fixture.Play.FollowScripts();
	CHECK(fixture.Context.GetScene().GetName() == "Second");
	CHECK(fixture.Context.GetScene().IsRunning());
	CHECK(fixture.Context.GetSceneVersion() != version);

	host->RequestQuit();
	fixture.Play.FollowScripts();
	CHECK_FALSE(fixture.Play.IsPlaying());
	CHECK(observer.Calls.back().Kind == "Stopping");
	CHECK(fixture.Context.GetScene().GetName() == "First");
	fixture.Play.FollowScripts();
	fixture.Play.SetObserver(nullptr);
}

TEST_CASE("PlayCommands: frames are refused with the debugger's reason, play.pause resumes through it, and play.state reports "
          "the debugger")
{
	PlayFixture fixture;
	RecordingObserver observer(fixture.Context);
	ObserverGuard const guard(fixture.Play);
	fixture.Play.SetObserver(&observer);
	Json const started = fixture.Ok("play.start", Json::object({{"paused", true}}));
	CHECK(started["debug"] == Json::object({{"state", "Detached"}, {"stops", 0}, {"current", nullptr}, {"pauseHeld", false}}));

	std::string const reason = "an execution is stopped at a breakpoint (stop 3): graph.continue or graph.step it first";
	observer.Refusal = reason;
	observer.Summary = {"Stopped", 1, 3, true};
	for (std::string_view const command : {"play.step", "play.advance"})
	{
		CAPTURE(command);
		CommandResult const refused = fixture.Run(command, Json::object({{"frames", 2}}));
		REQUIRE(refused.IsError());
		CHECK(refused.GetError().Code == AutomationErrorCode::InvalidOperation);
		CHECK(refused.GetError().Message == reason);
	}
	CHECK(fixture.Context.GetScene().GetRuntimeFrame() == 0);
	// play.state brings the debugger up to date before it reports it.
	uint32_t const refreshes = observer.Refreshes;
	Json const state = fixture.Ok("play.state");
	CHECK(observer.Refreshes > refreshes);
	CHECK(state["debug"] == Json::object({{"state", "Stopped"}, {"stops", 1}, {"current", 3}, {"pauseHeld", true}}));

	// Resuming continues the stopped executions first; the scene stays paused while one stops again.
	observer.StopAgainOnResume = true;
	CHECK(fixture.Ok("play.pause", Json::object({{"paused", false}}))["paused"] == true);
	CHECK(observer.Calls.back().Kind == "Resuming");
	observer.StopAgainOnResume = false;
	observer.Summary = {};
	Json const resumed = fixture.Ok("play.pause", Json::object({{"paused", false}}));
	CHECK(resumed["paused"] == false);
	CHECK(resumed["debug"]["state"] == "Detached");
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 2}}))["framesRun"] == 2);
	fixture.Ok("play.stop");
	fixture.Play.SetObserver(nullptr);
}

TEST_CASE("PlayMode: a scene that a frame of a paused game loads stays paused, the frames still asked for run, and a running "
          "game's load runs")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	InputScope input;
	Testing::TemporaryDirectory directory;
	AssetHandle const loader = WriteSceneLoader(directory.GetPath());
	PlayFixture fixture;
	fixture.Create("Loader", Json::object({{"VisualScript", Json::object({{"Graph", loader.ToString()}})}}));
	Timestep const frame(1.0f / 60.0f);

	SUBCASE("play.advance runs its frames, the first loading the scene, and leaves play paused")
	{
		fixture.Ok("play.start", Json::object({{"paused", true}}));
		CHECK(fixture.Ok("play.advance", Json::object({{"frames", 3}}))["framesRun"] == 3);
		CHECK(fixture.Context.GetScene().GetName() == "Second");
		CHECK(fixture.Context.GetScene().GetRuntimeFrame() == 2);
		CHECK(fixture.Ok("play.state")["paused"] == true);
		for (int editorFrame = 0; editorFrame < 5; editorFrame++)
		{
			fixture.Play.Update(frame);
		}
		CHECK(fixture.Context.GetScene().GetRuntimeFrame() == 2);
		CHECK(fixture.Play.IsPaused());
	}

	SUBCASE("play.step's frames carry over to the scene the first of them loads, one per editor frame, and play stays paused")
	{
		fixture.Ok("play.start", Json::object({{"paused", true}}));
		fixture.Ok("play.step", Json::object({{"frames", 3}}));
		fixture.Play.Update(frame);
		CHECK(fixture.Context.GetScene().GetName() == "Second");
		CHECK(fixture.Context.GetScene().GetRuntimeFrame() == 0);
		CHECK(fixture.Context.GetScene().GetQueuedSteps() == 2);
		for (int editorFrame = 0; editorFrame < 6; editorFrame++)
		{
			fixture.Play.Update(frame);
		}
		CHECK(fixture.Context.GetScene().GetRuntimeFrame() == 2);
		CHECK(fixture.Context.GetScene().GetQueuedSteps() == 0);
		CHECK(fixture.Ok("play.state")["paused"] == true);
	}

	SUBCASE("a running game's load runs the scene it loads")
	{
		fixture.Ok("play.start");
		for (int editorFrame = 0; editorFrame < 3; editorFrame++)
		{
			fixture.Play.Update(frame);
		}
		CHECK(fixture.Context.GetScene().GetName() == "Second");
		CHECK(fixture.Context.GetScene().GetRuntimeFrame() == 2);
		CHECK_FALSE(fixture.Play.IsPaused());
		CHECK(fixture.Ok("play.advance", Json::object({{"frames", 2}}))["framesRun"] == 2);
		CHECK_FALSE(fixture.Play.IsPaused());
	}
	fixture.Ok("play.stop");
}

TEST_CASE("PlayMode: playing clears the live log first, so a run's problems are its own, those of its OnCreate included, and "
          "stay after Stop")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	// Sets Result to 1 divided by Zero (0) when created: an aborting error at node 12.
	VisualScriptDocument division;
	VisualScriptVariable result;
	result.ID = 1;
	result.Name = "Result";
	result.Type = "int";
	result.Value = 0;
	VisualScriptVariable zero = result;
	zero.ID = 2;
	zero.Name = "Zero";
	division.Variables = {result, zero};
	VisualScriptNode create;
	create.ID = 10;
	create.Type = "Event.Create";
	VisualScriptNode set;
	set.ID = 11;
	set.Type = "Variable.Set";
	set.Settings = Json::object({{"Variable", 1}});
	VisualScriptNode divide;
	divide.ID = 12;
	divide.Type = "Operator.Divide";
	divide.Inputs = Json::object({{"A", 1}});
	VisualScriptNode get;
	get.ID = 13;
	get.Type = "Variable.Get";
	get.Settings = Json::object({{"Variable", 2}});
	division.Graph.Nodes = {create, set, divide, get};
	division.Graph.Links = {{{10, "Then"}, {11, "Execute"}}, {{13, "Value"}, {12, "B"}}, {{12, "Result"}, {11, "Value"}}};
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(std::move(division)), "Division");
	PlayFixture fixture;
	fixture.Create("Divider", Json::object({{"VisualScript", Json::object({{"Graph", graph.ToString()}})}}));

	auto const countProblems = []
	{
		Ref<VisualScriptLiveLog const> const live = VisualScriptEngine::GetLiveLog();
		REQUIRE(live->Problems.size() <= 1);
		return live->Problems.empty() ? 0u : live->Problems[0]->Count;
	};
	for (int run = 0; run < 2; run++)
	{
		CAPTURE(run);
		fixture.Ok("play.start");
		CHECK(countProblems() == 1);
		fixture.Ok("play.stop");
		// The last run's problems stay until the next one starts.
		CHECK(countProblems() == 1);
	}
	// Simulating runs no graph and keeps them.
	fixture.Ok("play.start", Json::object({{"mode", "simulate"}}));
	CHECK(countProblems() == 1);
	fixture.Ok("play.stop");
}

namespace
{
	// A row of boxes along -Z from the origin at y = 0.5, on a ground whose top is y = 0: a trigger whose face is 1.25 m
	// away, Near (2.5 m) and Far (5.5 m, a static rigid body). Everything is static, so the world is the same at every step.
	// Without a project the world has one layer, 0, which every body is on.
	struct QueryScene
	{
		UUID Ground;
		UUID Trigger;
		UUID Near;
		UUID Far;

		explicit QueryScene(PlayFixture& fixture)
		{
			Ground = fixture.Create("Ground", Json::object({{"Transform", Json::object({{"Translation", {0.0, -0.5, 0.0}}})},
			                                                {"BoxCollider", Json::object({{"HalfExtents", {10.0, 0.5, 10.0}}})}}));
			Trigger = fixture.Create(
				"Trigger", Json::object({{"Transform", Json::object({{"Translation", {0.0, 0.5, -1.5}}})},
			                             {"BoxCollider", Json::object({{"HalfExtents", {0.25, 0.25, 0.25}}, {"IsTrigger", true}})}}));
			Near = fixture.Create(
				"Near", Json::object({{"Transform", Json::object({{"Translation", {0.0, 0.5, -3.0}}})}, {"BoxCollider", Json::object()}}));
			Far = fixture.Create("Far", Json::object({{"Transform", Json::object({{"Translation", {0.0, 0.5, -6.0}}})},
			                                          {"RigidBody", Json::object({{"Type", "Static"}})},
			                                          {"BoxCollider", Json::object()}}));
		}
	};

	// A query along the row: from (0, 0.5, 0) towards -Z, with a direction that is not of unit length.
	Json AlongTheRow(std::string_view kind, Json const& extra = Json::object())
	{
		Json params = Json::object({{"kind", kind}, {"origin", {0.0, 0.5, 0.0}}, {"direction", {0.0, 0.0, -2.0}}});
		params.update(extra);
		return params;
	}

	std::vector<std::string> HitEntities(Json const& result)
	{
		std::vector<std::string> entities;
		for (Json const& hit : result["hits"])
		{
			entities.push_back(hit["entity"].get<std::string>());
		}
		return entities;
	}

	void CheckVector(Json const& value, glm::vec3 const& expected)
	{
		REQUIRE(value.is_array());
		REQUIRE(value.size() == 3);
		for (glm::length_t index = 0; index < 3; index++)
		{
			CHECK(value[static_cast<size_t>(index)].get<double>() == doctest::Approx(expected[index]).epsilon(1.0e-4));
		}
	}
}

TEST_CASE("PlayCommands: physics.query answers each kind from the playing scene's physics world, as the scene's queries do")
{
	Testing::PhysicsSystemScope physics;
	PlayFixture fixture;
	QueryScene const row(fixture);
	CommandDefinition const* const command = fixture.Registry.Find("physics.query");
	REQUIRE(command != nullptr);
	CHECK(command->ReadOnly);

	// Paused before its first frame: the world has run no step, and its bodies are already where the scene puts them.
	Json const started = fixture.Ok("play.start", Json::object({{"mode", "simulate"}, {"paused", true}}));
	CHECK(started["state"] == "simulate");
	Scene& scene = fixture.Context.GetScene();
	size_t const steps = fixture.Context.GetHistory().GetUndoCount();
	bool const dirty = fixture.Context.IsDirty();

	// raycast: the first solid hit; the trigger before it is passed through.
	Json const ray = fixture.Ok("physics.query", AlongTheRow("raycast"));
	CHECK(ray["kind"] == "raycast");
	CHECK(ray["state"] == "simulate");
	CHECK(ray["step"] == 0);
	CHECK(ray["count"] == 1);
	CHECK(ray["truncated"] == false);
	REQUIRE(ray["hits"].size() == 1);
	Json const& first = ray["hits"][0];
	CHECK(first["entity"] == row.Near.ToString());
	CHECK(first["name"] == "Near");
	CHECK(first["distance"].get<double>() == doctest::Approx(2.5).epsilon(1.0e-4));
	CheckVector(first["point"], glm::vec3(0.0f, 0.5f, -2.5f));
	CheckVector(first["normal"], glm::vec3(0.0f, 0.0f, 1.0f));
	CHECK(first["material"].is_null());
	std::optional<RaycastHit> const expected = scene.Raycast(glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), 100.0f);
	REQUIRE(expected.has_value());
	CHECK(first["distance"].get<double>() == doctest::Approx(expected->Distance));
	// Too short a ray meets nothing.
	Json const shortRay = fixture.Ok("physics.query", AlongTheRow("raycast", Json::object({{"distance", 2.0}})));
	CHECK(shortRay["count"] == 0);
	CHECK(shortRay["hits"].empty());

	// raycastAll: every entity, nearest first; triggers when asked; masks by layer; maxHits keeps the nearest.
	CHECK(HitEntities(fixture.Ok("physics.query", AlongTheRow("raycastAll"))) ==
	      std::vector<std::string>{row.Near.ToString(), row.Far.ToString()});
	Json const all = fixture.Ok("physics.query", AlongTheRow("raycastAll", Json::object({{"triggers", true}})));
	CHECK(HitEntities(all) == std::vector<std::string>{row.Trigger.ToString(), row.Near.ToString(), row.Far.ToString()});
	CHECK(all["hits"][0]["distance"].get<double>() == doctest::Approx(1.25).epsilon(1.0e-4));
	CHECK(all["hits"][2]["distance"].get<double>() == doctest::Approx(5.5).epsilon(1.0e-4));
	std::vector<RaycastHit> sceneHits;
	PhysicsQueryFilter withTriggers;
	withTriggers.IncludeTriggers = true;
	REQUIRE(scene.RaycastAll(glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), MaxPhysicsQueryDistance, sceneHits, withTriggers) ==
	        3);
	for (size_t index = 0; index < sceneHits.size(); index++)
	{
		CHECK(all["hits"][index]["entity"] == sceneHits[index].Entity.ToString());
		CHECK(all["hits"][index]["distance"].get<double>() == doctest::Approx(sceneHits[index].Distance));
	}
	// The mask: bit 0 sees layer 0, where everything is; a mask without it sees nothing, for each kind.
	CHECK(HitEntities(fixture.Ok("physics.query", AlongTheRow("raycastAll", Json::object({{"mask", 1}})))) ==
	      std::vector<std::string>{row.Near.ToString(), row.Far.ToString()});
	for (std::string_view const kind : {"raycast", "raycastAll", "shapeCast", "overlap"})
	{
		CAPTURE(kind);
		Json params = AlongTheRow(kind, Json::object({{"mask", 4294967294ull}}));
		if (kind == "overlap")
		{
			params.erase("direction");
			params["origin"] = Json::array({0.0, 0.5, -3.0});
		}
		if (kind == "shapeCast" || kind == "overlap")
		{
			params["shape"] = "sphere";
		}
		Json const masked = fixture.Ok("physics.query", params);
		CHECK(masked["count"] == 0);
		params["mask"] = 1;
		CHECK(fixture.Ok("physics.query", params)["count"].get<size_t>() > 0);
	}
	Json const truncated = fixture.Ok("physics.query", AlongTheRow("raycastAll", Json::object({{"triggers", true}, {"maxHits", 2}})));
	CHECK(truncated["count"] == 3);
	CHECK(truncated["truncated"] == true);
	CHECK(HitEntities(truncated) == std::vector<std::string>{row.Trigger.ToString(), row.Near.ToString()});

	// shapeCast: the distance the shape travels until it touches Near's face at z = -2.5.
	struct Cast
	{
		Json Shape;
		double Distance = 0.0;
	};
	double const halfTurn = std::sqrt(0.5);
	std::array<Cast, 5> const casts = {
		Cast{Json::object({{"shape", "sphere"}, {"radius", 0.25}}), 2.25},
		Cast{Json::object({{"shape", "box"}, {"halfExtents", {0.25, 0.25, 0.25}}}), 2.25},
		// Upright, along its local Y axis: its side touches first.
		Cast{Json::object({{"shape", "capsule"}, {"radius", 0.2}, {"halfHeight", 0.1}}), 2.3},
		// Lying along Z, turned by a pitch of 90 degrees or by the same quaternion: its cap reaches 0.3 m ahead.
		Cast{Json::object({{"shape", "capsule"}, {"radius", 0.2}, {"halfHeight", 0.1}, {"rotationEuler", {90.0, 0.0, 0.0}}}), 2.2},
		Cast{Json::object({{"shape", "capsule"}, {"radius", 0.2}, {"halfHeight", 0.1}, {"rotation", {halfTurn, 0.0, 0.0, halfTurn}}}), 2.2},
	};
	for (Cast const& cast : casts)
	{
		CAPTURE(cast.Shape.dump());
		Json const result = fixture.Ok("physics.query", AlongTheRow("shapeCast", cast.Shape));
		CHECK(result["count"] == 1);
		REQUIRE(result["hits"].size() == 1);
		CHECK(result["hits"][0]["entity"] == row.Near.ToString());
		CHECK(result["hits"][0]["distance"].get<double>() == doctest::Approx(cast.Distance).epsilon(1.0e-3));
		CheckVector(result["hits"][0]["normal"], glm::vec3(0.0f, 0.0f, 1.0f));
		CHECK(result["hits"][0]["point"][2].get<double>() == doctest::Approx(-2.5).epsilon(1.0e-3));
	}
	std::optional<ShapeCastHit> const sphereHit = scene.ShapeCast(PhysicsQueryShape::MakeSphere(0.25f), glm::vec3(0.0f, 0.5f, 0.0f),
	                                                              glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), 100.0f);
	REQUIRE(sphereHit.has_value());
	CHECK(fixture.Ok("physics.query", AlongTheRow("shapeCast", casts[0].Shape))["hits"][0]["distance"].get<double>() ==
	      doctest::Approx(sphereHit->Distance));
	// With triggers the cast stops at the trigger's face.
	Json const castTriggers =
		fixture.Ok("physics.query", AlongTheRow("shapeCast", Json::object({{"shape", "sphere"}, {"radius", 0.1}, {"triggers", true}})));
	REQUIRE(castTriggers["hits"].size() == 1);
	CHECK(castTriggers["hits"][0]["entity"] == row.Trigger.ToString());

	// overlap: by entity ID, triggers when asked; a box over Near and Far, above the ground.
	Json const overlapBox =
		Json::object({{"kind", "overlap"}, {"origin", {0.0, 1.0, -4.5}}, {"shape", "box"}, {"halfExtents", {0.5, 0.4, 1.5}}});
	std::vector<std::string> both = {row.Near.ToString(), row.Far.ToString()};
	std::ranges::sort(both,
	                  [](std::string const& left, std::string const& right)
	                  {
						  return std::stoull(left) < std::stoull(right);
					  });
	Json const overlapped = fixture.Ok("physics.query", overlapBox);
	CHECK(overlapped["count"] == 2);
	CHECK(HitEntities(overlapped) == both);
	REQUIRE_FALSE(overlapped["hits"].empty());
	CHECK(overlapped["hits"][0]["name"].is_string());
	CHECK_FALSE(overlapped["hits"][0].contains("distance"));
	Json overlapOne = overlapBox;
	overlapOne["maxHits"] = 1;
	Json const one = fixture.Ok("physics.query", overlapOne);
	CHECK(one["count"] == 2);
	CHECK(one["truncated"] == true);
	CHECK(HitEntities(one) == std::vector<std::string>{both[0]});
	Json const sphereOverTrigger = Json::object({{"kind", "overlap"}, {"origin", {0.0, 0.5, -1.5}}, {"shape", "sphere"}, {"radius", 0.1}});
	CHECK(fixture.Ok("physics.query", sphereOverTrigger)["count"] == 0);
	Json sphereWithTriggers = sphereOverTrigger;
	sphereWithTriggers["triggers"] = true;
	CHECK(HitEntities(fixture.Ok("physics.query", sphereWithTriggers)) == std::vector<std::string>{row.Trigger.ToString()});
	Json const capsuleOverlap =
		Json::object({{"kind", "overlap"}, {"origin", {0.0, 0.5, -3.0}}, {"shape", "capsule"}, {"radius", 0.1}, {"halfHeight", 0.1}});
	CHECK(HitEntities(fixture.Ok("physics.query", capsuleOverlap)) == std::vector<std::string>{row.Near.ToString()});

	// Queries are no step of the playing copy's history and change nothing.
	CHECK(fixture.Context.GetHistory().GetUndoCount() == steps);
	CHECK(fixture.Context.IsDirty() == dirty);

	// A body made while playing is found before the next step, and still after steps.
	UUID const added = fixture.Create("Added", Json::object({{"Transform", Json::object({{"Translation", {0.0, 0.5, -2.0}}})},
	                                                         {"BoxCollider", Json::object({{"HalfExtents", {0.1, 0.1, 0.1}}})}}));
	CHECK(fixture.Ok("physics.query", AlongTheRow("raycast"))["hits"][0]["entity"] == added.ToString());
	CHECK(fixture.Ok("play.advance", Json::object({{"frames", 2}}))["framesRun"] == 2);
	Json const later = fixture.Ok("physics.query", AlongTheRow("raycast"));
	CHECK(later["step"].get<uint64_t>() > 0);
	CHECK(later["frame"] == scene.GetRuntimeFrame());
	CHECK(later["hits"][0]["entity"] == added.ToString());

	// Stopped, nothing plays.
	fixture.Ok("play.stop");
	CHECK(fixture.Fails("physics.query", AlongTheRow("raycast")) == AutomationErrorCode::Unavailable);
}

TEST_CASE("PlayCommands: physics.query is Unavailable while nothing plays or without physics, and refuses what a kind has no use for")
{
	PlayFixture fixture;
	QueryScene const row(fixture);
	// While editing: Unavailable, whatever the parameters the schema lets through.
	CommandResult const editing = fixture.Run("physics.query", AlongTheRow("raycast"));
	REQUIRE(editing.IsError());
	CHECK(editing.GetError().Code == AutomationErrorCode::Unavailable);
	CHECK(editing.GetError().Message.find("play.start") != std::string::npos);
	CHECK(fixture.Fails("physics.query", AlongTheRow("overlap")) == AutomationErrorCode::Unavailable);
	// The schema refuses before the state is read.
	CHECK(fixture.Fails("physics.query", Json::object({{"origin", {0.0, 0.0, 0.0}}})) == AutomationErrorCode::InvalidParams);

	SUBCASE("without the physics system the playing scene has no world")
	{
		fixture.Ok("play.start", Json::object({{"mode", "simulate"}, {"paused", true}}));
		CommandResult const result = fixture.Run("physics.query", AlongTheRow("raycast"));
		REQUIRE(result.IsError());
		CHECK(result.GetError().Code == AutomationErrorCode::Unavailable);
		CHECK(result.GetError().Message.find("no physics world") != std::string::npos);
		fixture.Ok("play.stop");
	}

	SUBCASE("while playing, each parameter is checked")
	{
		Testing::PhysicsSystemScope physics;
		fixture.Ok("play.start", Json::object({{"mode", "simulate"}}));
		double const nan = std::numeric_limits<double>::quiet_NaN();
		struct Refusal
		{
			Json Params;
			std::string_view Path;
		};
		std::vector<Refusal> const refusals = {
			{AlongTheRow("raycast", Json::object({{"shape", "sphere"}})), "shape"},
			{AlongTheRow("raycastAll", Json::object({{"radius", 1.0}})), "radius"},
			{AlongTheRow("raycast", Json::object({{"rotationEuler", {0.0, 0.0, 0.0}}})), "rotationEuler"},
			{AlongTheRow("shapeCast"), "shape"},
			{Json::object({{"kind", "raycast"}, {"origin", {0.0, 0.0, 0.0}}}), "direction"},
			{Json::object({{"kind", "overlap"}, {"origin", {0.0, 0.0, 0.0}}, {"shape", "sphere"}, {"direction", {1.0, 0.0, 0.0}}}),
		     "direction"},
			{Json::object({{"kind", "overlap"}, {"origin", {0.0, 0.0, 0.0}}, {"shape", "sphere"}, {"distance", 1.0}}), "distance"},
			{AlongTheRow("raycast", Json::object({{"maxHits", 2}})), "maxHits"},
			{AlongTheRow("shapeCast", Json::object({{"shape", "sphere"}, {"maxHits", 2}})), "maxHits"},
			{AlongTheRow("raycast", Json::object({{"triggers", true}})), "triggers"},
			{AlongTheRow("shapeCast", Json::object({{"shape", "box"}, {"radius", 1.0}})), "radius"},
			{AlongTheRow("shapeCast", Json::object({{"shape", "sphere"}, {"halfExtents", {1.0, 1.0, 1.0}}})), "halfExtents"},
			{AlongTheRow("shapeCast", Json::object({{"shape", "box"}, {"halfHeight", 1.0}})), "halfHeight"},
			{AlongTheRow("shapeCast", Json::object({{"shape", "sphere"}, {"halfHeight", 1.0}})), "halfHeight"},
			{AlongTheRow("shapeCast", Json::object({{"shape", "sphere"}, {"rotation", {0.0, 0.0, 0.0, 1.0}}})), "rotation"},
			{Json::object({{"kind", "overlap"}, {"origin", {0.0, 0.0, 0.0}}, {"shape", "sphere"}, {"rotationEuler", {0.0, 0.0, 0.0}}}),
		     "rotationEuler"},
			{AlongTheRow("shapeCast",
		                 Json::object({{"shape", "box"}, {"rotation", {0.0, 0.0, 0.0, 1.0}}, {"rotationEuler", {0.0, 0.0, 0.0}}})),
		     "rotationEuler"},
			{AlongTheRow("raycast", Json::object({{"direction", {0.0, 0.0, 0.0}}})), "direction"},
			{AlongTheRow("shapeCast", Json::object({{"shape", "box"}, {"rotation", {0.0, 0.0, 0.0, 0.0}}})), "rotation"},
			// Values no JSON text holds, which a request built in memory can.
			{AlongTheRow("raycast", Json::object({{"origin", {0.0, nan, 0.0}}})), "origin"},
			{AlongTheRow("raycast", Json::object({{"direction", {nan, 0.0, -1.0}}})), "direction"},
			{AlongTheRow("raycast", Json::object({{"distance", nan}})), "distance"},
			// A query of no length would find nothing, not even what a shape starts in.
			{AlongTheRow("raycast", Json::object({{"distance", 0.0}})), "distance"},
			{AlongTheRow("shapeCast", Json::object({{"shape", "sphere"}, {"distance", 0.0}})), "distance"},
			{AlongTheRow("shapeCast", Json::object({{"shape", "sphere"}, {"radius", nan}})), "radius"},
			{AlongTheRow("shapeCast", Json::object({{"shape", "capsule"}, {"halfHeight", nan}})), "halfHeight"},
			{AlongTheRow("shapeCast", Json::object({{"shape", "box"}, {"halfExtents", {1.0, nan, 1.0}}})), "halfExtents"},
			{AlongTheRow("shapeCast", Json::object({{"shape", "box"}, {"rotation", {0.0, nan, 0.0, 1.0}}})), "rotation"},
			{AlongTheRow("shapeCast", Json::object({{"shape", "box"}, {"rotationEuler", {0.0, nan, 0.0}}})), "rotationEuler"},
		};
		for (Refusal const& refusal : refusals)
		{
			CAPTURE(refusal.Params.dump());
			CommandResult const result = fixture.Run("physics.query", refusal.Params);
			REQUIRE(result.IsError());
			CHECK(result.GetError().Code == AutomationErrorCode::InvalidParams);
			CHECK(result.GetError().Data.value("path", std::string()) == refusal.Path);
			CHECK_FALSE(result.GetError().Message.empty());
		}
		// The schema's limits: beyond the world, a negative size or distance, a size past the largest a query's shape may have,
		// a mask past 32 bits, an unknown kind, shape or parameter, a vector of two numbers.
		std::array<Json, 12> const outOfRange = {
			AlongTheRow("raycast", Json::object({{"origin", {2.0e7, 0.0, 0.0}}})),
			AlongTheRow("raycast", Json::object({{"distance", -1.0}})),
			AlongTheRow("raycast", Json::object({{"distance", 1.0e8}})),
			AlongTheRow("raycast", Json::object({{"mask", 4294967296ull}})),
			AlongTheRow("shapeCast", Json::object({{"shape", "sphere"}, {"radius", -1.0}})),
			AlongTheRow("shapeCast", Json::object({{"shape", "sphere"}, {"radius", 1.01 * MaxPhysicsQueryRadius}})),
			AlongTheRow("shapeCast", Json::object({{"shape", "box"}, {"halfExtents", {0.5, 1.01 * MaxPhysicsQueryExtent, 0.5}}})),
			Json::object(
				{{"kind", "overlap"}, {"origin", {0.0, 0.0, 0.0}}, {"shape", "capsule"}, {"halfHeight", 1.01 * MaxPhysicsQueryExtent}}),
			AlongTheRow("shapeCast", Json::object({{"shape", "cone"}})),
			AlongTheRow("sweep"),
			AlongTheRow("raycast", Json::object({{"direction", {0.0, 1.0}}})),
			AlongTheRow("raycast", Json::object({{"unknown", 1}})),
		};
		for (Json const& params : outOfRange)
		{
			CAPTURE(params.dump());
			CHECK(fixture.Fails("physics.query", params) == AutomationErrorCode::InvalidParams);
		}
		// The valid extremes answer, and right: the world's edge with the longest distance, the largest shapes beside bodies
		// they do not touch, a direction of any length.
		auto const query = [&fixture](Json const& asked)
		{
			CAPTURE(asked.dump());
			return fixture.Ok("physics.query", asked);
		};
		Json const fromTheEdge = query(Json::object({{"kind", "raycastAll"},
		                                             {"origin", {MaxPhysicsCoordinate, MaxPhysicsCoordinate, -MaxPhysicsCoordinate}},
		                                             {"direction", {-1.0, -1.0, 1.0}},
		                                             {"distance", MaxPhysicsQueryDistance}}));
		CHECK(HitEntities(fromTheEdge) == std::vector<std::string>{row.Ground.ToString()});
		// A sheet as wide as a query's box may be, through Near and Far where it starts: both at distance 0, Near or Far by
		// entity ID, as the overlap of the same box says; the ground half a meter below is not touched.
		Json const sheet = Json::object({{"shape", "box"}, {"halfExtents", {MaxPhysicsQueryExtent, 0.0, MaxPhysicsQueryExtent}}});
		Json sheetOverlap = Json::object({{"kind", "overlap"}, {"origin", {0.0, 0.5, 0.0}}});
		sheetOverlap.update(sheet);
		std::vector<std::string> const sheetEntities = HitEntities(query(sheetOverlap));
		std::vector<std::string> both = {row.Near.ToString(), row.Far.ToString()};
		std::ranges::sort(both,
		                  [](std::string const& left, std::string const& right)
		                  {
							  return std::stoull(left) < std::stoull(right);
						  });
		CHECK(sheetEntities == both);
		Json const sheetCast = query(AlongTheRow("shapeCast", sheet));
		REQUIRE(sheetCast["hits"].size() == 1);
		CHECK(sheetCast["hits"][0]["entity"] == both[0]);
		CHECK(sheetCast["hits"][0]["distance"] == 0.0);
		// A rod as long as a query's capsule may be: upright it starts in the ground; lying across the row it meets Near's face.
		Json const upright =
			query(AlongTheRow("shapeCast", Json::object({{"shape", "capsule"}, {"radius", 0.0}, {"halfHeight", MaxPhysicsQueryExtent}})));
		REQUIRE(upright["hits"].size() == 1);
		CHECK(upright["hits"][0]["entity"] == row.Ground.ToString());
		CHECK(upright["hits"][0]["distance"] == 0.0);
		Json const lying = query(AlongTheRow(
			"shapeCast",
			Json::object(
				{{"shape", "capsule"}, {"radius", 0.0}, {"halfHeight", MaxPhysicsQueryExtent}, {"rotationEuler", {0.0, 0.0, 90.0}}})));
		REQUIRE(lying["hits"].size() == 1);
		CHECK(lying["hits"][0]["entity"] == row.Near.ToString());
		CHECK(lying["hits"][0]["distance"].get<double>() == doctest::Approx(2.5 - 0.001).epsilon(1.0e-3));
		// Every entity of the scene lies in the largest sphere around the origin; the trigger counts only when asked.
		Json const everything =
			query(Json::object({{"kind", "overlap"}, {"origin", {0.0, 0.0, 0.0}}, {"shape", "sphere"}, {"radius", MaxPhysicsQueryRadius}}));
		CHECK(everything["count"] == 3);
		CHECK(everything["state"] == "simulate");
		// A direction of any length but zero is a direction: down onto the ground, a meter's worth of diagonal away.
		Json const diagonal = query(AlongTheRow("raycast", Json::object({{"direction", {1.0e6, -1.0e6, 1.0e6}}})));
		REQUIRE(diagonal["hits"].size() == 1);
		CHECK(diagonal["hits"][0]["entity"] == row.Ground.ToString());
		CHECK(diagonal["hits"][0]["distance"].get<double>() == doctest::Approx(0.5 * std::sqrt(3.0)).epsilon(1.0e-4));
		fixture.Ok("play.stop");
	}
	CHECK(row.Ground.IsValid());
}
