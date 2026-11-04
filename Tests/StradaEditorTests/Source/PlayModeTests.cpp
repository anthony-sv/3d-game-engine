#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/PlayCommands.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/PlayMode.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/Buffer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Input.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Script/VisualScriptEngine.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
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

TEST_CASE("PlayCommands: every play, input and test command is documented in Docs/Automation.md")
{
	PlayFixture fixture;
	Result<std::string> reference = FileSystem::ReadTextFile(STRADA_AUTOMATION_DOC_PATH);
	REQUIRE(reference.IsOk());
	CHECK(fixture.Registry.GetCommandCount() == 10);
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
		// The editor's frame ends between the two requests, and Input's one-frame pressed and released flags with it.
		Input::EndFrame();
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
	Input::EndFrame();
	fixture.Ok("play.advance", Json::object({{"frames", 1}}));
	CHECK(fixture.Context.GetScene().GetEntityByUUID(canvas).GetName() ==
	      "Clicks=1;Last=" + buttonId + ";InClick=True;Polled=False;State=Hovered;Over=True");
	CHECK(fixture.Ok("play.stop")["state"] == "edit");
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
	std::filesystem::path const sample = FileSystem::PathFromUtf8(STRADA_PROJECTS_DIR) / "Blocks";
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
	// The sample as a player gets it: its scenes and assets, and no C# (spec-slice4 section 18.10).
	Testing::AssetManagerScope assets;
	Testing::PhysicsSystemScope physics;
	Testing::ScriptRuntimeScope scripting;
	InputScope input;
	Testing::TemporaryDirectory directory;
	std::filesystem::path const sample = FileSystem::PathFromUtf8(STRADA_PROJECTS_DIR) / "PrismBreak";
	REQUIRE(FileSystem::CopyDirectory(sample / "Assets", directory.GetPath() / "Assets").IsOk());
	REQUIRE(FileSystem::Copy(sample / "PrismBreak.sproj", directory.GetPath() / "PrismBreak.sproj", false).IsOk());

	PlayFixture fixture;
	REQUIRE(fixture.Operations.OpenProject(directory.GetPath() / "PrismBreak.sproj").IsOk());
	auto const find = [&fixture](std::string_view name)
	{
		Entity const entity = fixture.Context.GetScene().FindEntityByName(name);
		REQUIRE(entity);
		return entity;
	};
	// The match's random numbers are seeded, so every run plays the same frames.
	Entity const game = find("Game");
	VisualScriptComponent match = game.GetComponent<VisualScriptComponent>();
	match.Variables["Seed"] = ScriptFieldValue::FromInt32(7);
	REQUIRE(fixture.Operations.SetComponent(game.GetUUID(), match).IsOk());
	UUID const gameId = game.GetUUID();

	CHECK(fixture.Ok("play.start")["state"] == "play");
	fixture.Ok("play.pause");
	auto const advance = [&fixture](uint32_t frames)
	{
		fixture.Ok("play.advance", Json::object({{"frames", frames}}));
	};
	// A key goes down for a frame, then up.
	auto const press = [&fixture, &advance](std::string const& key)
	{
		fixture.Ok("input.set", Json::object({{"keys", Json::object({{key, true}})}}));
		advance(1);
		fixture.Ok("input.set", Json::object({{"keys", Json::object({{key, false}})}}));
		advance(1);
	};
	// What the match's read-outs say, through the public variables of its graph.
	auto const readOut = [gameId](std::string const& name)
	{
		Result<ScriptFieldMap> const variables = VisualScriptEngine::GetInstanceVariables(gameId);
		REQUIRE_MESSAGE(variables.IsOk(), (variables ? std::string() : variables.GetError()));
		auto const variable = variables.GetValue().find(name);
		REQUIRE(variable != variables.GetValue().end());
		return variable->second;
	};
	auto const state = [&readOut]
	{
		return readOut("State").GetString();
	};
	auto const shown = [&find](std::string_view element)
	{
		return find(element).GetComponent<UIRectComponent>().Visible;
	};
	auto const placeOf = [&find](std::string_view name)
	{
		return find(name).GetComponent<TransformComponent>().Translation;
	};
	// The balls in play: the children of the Balls container.
	auto const balls = [&fixture, &find]
	{
		std::vector<Entity> found;
		for (UUID const child : find("Balls").GetComponent<RelationshipComponent>().Children)
		{
			if (Entity const ball = fixture.Context.GetScene().GetEntityByUUID(child))
			{
				found.push_back(ball);
			}
		}
		return found;
	};
	auto const ballPlace = [&balls]
	{
		std::vector<Entity> const found = balls();
		REQUIRE(found.size() == 1);
		return found.front().GetComponent<TransformComponent>().Translation;
	};
	// The mouse at a point of the 640-pixel-wide game view, its height's middle.
	auto const pointAt = [&fixture](float x)
	{
		fixture.Ok("input.set", Json::object({{"mousePosition", Json::array({x, 240.0})}}));
	};

	// The title shows, and the game plays itself: the autopilot steers the paddle, and its ball breaks prisms. The mouse rests
	// over the game view meanwhile (a first position is no movement).
	pointAt(320.0f);
	advance(1);
	CHECK(state() == "Title");
	CHECK(shown("Title Screen"));
	CHECK_FALSE(shown("Game Bar"));
	advance(900);
	CHECK(state() == "Title");
	CHECK(shown("Title Screen"));
	CHECK(readOut("Score").GetInt32() > 0);
	CHECK(readOut("Lives").GetInt32() == 3);
	CHECK(placeOf("Paddle").x != 0.0f);

	// Space starts a game: the score and lives start fresh, the level builds prism by prism, and the ball waits on the paddle.
	press("Space");
	CHECK(state() == "Serve");
	CHECK_FALSE(shown("Title Screen"));
	CHECK(readOut("Score").GetInt32() == 0);
	CHECK(readOut("Lives").GetInt32() == 3);
	advance(60);
	CHECK(readOut("BricksLeft").GetInt32() == 33);
	CHECK(state() == "Serve");
	CHECK(shown("Serve Prompt"));
	CHECK(shown("Game Bar"));

	// The mouse moves the paddle across the arena, and the waiting ball rides it.
	pointAt(600.0f);
	advance(30);
	CHECK(placeOf("Paddle").x > 5.0f);
	CHECK(ballPlace().x == doctest::Approx(placeOf("Paddle").x).epsilon(0.02));
	pointAt(40.0f);
	advance(30);
	CHECK(placeOf("Paddle").x < -5.0f);
	CHECK(ballPlace().x == doctest::Approx(placeOf("Paddle").x).epsilon(0.02));
	pointAt(320.0f);
	advance(30);

	// Space serves: the ball leaves the paddle for the prisms.
	float const served = ballPlace().z;
	press("Space");
	CHECK(state() == "Play");
	CHECK_FALSE(shown("Serve Prompt"));
	advance(10);
	CHECK(ballPlace().z < served - 1.0f);

	// Escape pauses: the pause screen shows and the ball holds still; Escape again plays on.
	press("Escape");
	CHECK(shown("Pause Screen"));
	glm::vec3 const held = ballPlace();
	advance(30);
	CHECK(ballPlace() == held);
	press("Escape");
	CHECK_FALSE(shown("Pause Screen"));
	advance(5);
	CHECK(ballPlace() != held);

	// The paddle kept away from the ball lets it fall into the drain: a life goes, its icon with it, and the next ball waits
	// on the paddle.
	for (int frame = 0; frame < 1200 && readOut("Lives").GetInt32() == 3; frame++)
	{
		std::vector<Entity> const inPlay = balls();
		if (!inPlay.empty())
		{
			pointAt(inPlay.front().GetComponent<TransformComponent>().Translation.x > 0.0f ? 40.0f : 600.0f);
		}
		advance(1);
	}
	CHECK(readOut("Lives").GetInt32() == 2);
	CHECK(shown("Life 2"));
	CHECK_FALSE(shown("Life 3"));
	advance(90);
	CHECK(state() == "Serve");
	CHECK(balls().size() == 1);
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
