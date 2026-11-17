#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/InputActionMap.h"
#include "Strada/Asset/InputActionsAsset.h"
#include "Strada/Asset/InputBindingProfile.h"
#include "Strada/Asset/InputRebinding.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/InputActions.h"
#include "Strada/Core/InputCapture.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

// Rebinding while a scene runs (Docs/Architecture.md §7.4, "Rebinding"): a rebind reaching the evaluation, its conflicts and
// refusals, a capture through the scene (a control that binds without pressing, Escape, the time limit at a time scale of 0),
// and the profile in the game data folder (written when a binding changes, read when the actions load, stale entries
// dropped, nothing without a folder or a file). The capture alone is InputCaptureTests.cpp's, the profile's document
// InputBindingProfileTests.cpp's.
namespace
{
	constexpr float Step = 1.0f / 60.0f;

	// Input and the process's rebound bindings start and end empty.
	struct InputScope
	{
		InputScope()
		{
			Input::Reset();
			InputRebinding::Clear();
		}
		~InputScope()
		{
			InputRebinding::Clear();
			Input::Reset();
		}
		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	InputActionBinding Bound(std::string controls)
	{
		InputActionBinding binding;
		binding.Controls = std::move(controls);
		return binding;
	}

	InputAction MakeAction(std::string name, InputActionType type, std::vector<InputActionBinding> bindings)
	{
		InputAction action;
		action.Name = std::move(name);
		action.Type = type;
		action.Bindings = std::move(bindings);
		return action;
	}

	// Jump on Space and any pad's A, Steer on A and D, Move on WASD and any pad's left stick.
	InputActionMap MakeMap()
	{
		InputActionMap map;
		map.Actions.push_back(MakeAction("Jump", InputActionType::Button, {Bound("Key/Space"), Bound("Gamepad/A")}));
		map.Actions.push_back(MakeAction("Steer", InputActionType::Axis, {Bound("Key/A Key/D")}));
		map.Actions.push_back(MakeAction("Move", InputActionType::Vector2, {Bound("Key/W Key/S Key/A Key/D"), Bound("Gamepad/LeftStick")}));
		return map;
	}

	Ref<TypedDataAsset<InputActionMap>> MakeAsset(InputActionMap const& map)
	{
		TypedDataAssetType<InputActionMap> const* type = DataAssetRegistry::Find<InputActionMap>();
		REQUIRE(type != nullptr);
		Ref<TypedDataAsset<InputActionMap>> const asset = std::static_pointer_cast<TypedDataAsset<InputActionMap>>(type->CreateAsset());
		Result<void> const set = asset->SetData(map);
		REQUIRE_MESSAGE(set.IsOk(), (set ? std::string() : set.GetError()));
		return asset;
	}

	// The map as a file asset of an asset directory (Input/Game.sinput), which a profile is named after.
	AssetHandle AddFileAsset(std::filesystem::path const& assets, InputActionMap const& map)
	{
		REQUIRE(FileSystem::CreateDirectories(assets / "Input").IsOk());
		REQUIRE(AssetManager::OpenAssetDirectory(assets).IsOk());
		std::filesystem::path const file = assets / "Input" / "Game.sinput";
		REQUIRE(DataAssetSerializer::SaveToFile(*MakeAsset(map), file).IsOk());
		Result<AssetHandle> const imported = AssetManager::ImportFile(file);
		REQUIRE(imported.IsOk());
		return imported.GetValue();
	}

	SceneRuntimeSettings Settings(AssetHandle actions)
	{
		SceneRuntimeSettings settings;
		settings.RunScripts = false;
		settings.PlayAudio = false;
		settings.FixedTimestep = Step;
		settings.InputActions = actions;
		return settings;
	}

	void RunFrame(Scene& scene, float frameTime = Step)
	{
		scene.OnUpdateRuntime(Timestep(frameTime));
		Input::EndFrame();
	}

	uint32_t Find(Scene& scene, std::string_view name)
	{
		std::optional<uint32_t> const action = scene.GetInputActions().GetActions()->Find(name);
		REQUIRE(action.has_value());
		return *action;
	}

	size_t CountLogged(uint64_t first, std::string_view text)
	{
		std::vector<LogEntry> const entries = Log::GetEntries(first);
		return static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
		                                         [text](LogEntry const& entry)
		                                         {
													 return entry.Message.find(text) != std::string::npos;
												 }));
	}

	std::string ControlsOf(Scene const& scene, std::string_view action, int32_t binding)
	{
		Result<std::string> const controls = scene.GetInputBindingControls(action, binding);
		REQUIRE_MESSAGE(controls.IsOk(), (controls ? std::string() : controls.GetError()));
		return controls.GetValue();
	}

	InputBindingProfile ReadProfile(std::filesystem::path const& file)
	{
		Result<std::string> const text = FileSystem::ReadTextFile(file);
		REQUIRE_MESSAGE(text.IsOk(), FileSystem::PathToUtf8(file));
		Result<Json> const document = ParseJson(text.GetValue());
		REQUIRE(document.IsOk());
		Result<InputBindingProfile> profile = ReadInputBindingProfile(document.GetValue());
		REQUIRE_MESSAGE(profile.IsOk(), (profile ? std::string() : profile.GetError()));
		return profile.TakeValue();
	}
}

TEST_CASE("SceneInputRebinding: a rebind reaches the evaluation at the next frame, answers its conflicts and leaves the asset")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	InputScope const input;
	Ref<TypedDataAsset<InputActionMap>> const asset = MakeAsset(MakeMap());
	AssetHandle const actions = AssetManager::AddMemoryAsset(asset, "Actions.sinput");
	Scene scene("Rebind");
	scene.OnRuntimeStart(Settings(actions));
	RunFrame(scene);
	uint32_t const jump = Find(scene, "Jump");
	uint64_t const version = InputRebinding::GetVersion();

	InputRebindResult const done = scene.RebindInput("Jump", 0, "key/a");
	CHECK(done.Status == InputRebindStatus::Done);
	CHECK(done.Action == "Jump");
	CHECK(done.Binding == 0);
	CHECK(done.Part == -1);
	CHECK(done.Controls == "Key/A");
	CHECK(done.Conflicts == std::vector<std::string>{"Steer", "Move"});
	CHECK(done.Message.empty());
	CHECK(InputRebinding::GetVersion() != version);
	CHECK(ControlsOf(scene, "Jump", 0) == "Key/A");
	CHECK(asset->GetData() == MakeMap());

	// The next frame reads the new binding, and the old control presses nothing.
	Input::SetKeyState(KeyCode::Space, true);
	RunFrame(scene);
	CHECK_FALSE(scene.GetInputActions().IsDown(jump));
	Input::SetKeyState(KeyCode::Space, false);
	Input::SetKeyState(KeyCode::A, true);
	RunFrame(scene);
	CHECK(scene.GetInputActions().IsPressed(jump));
	Input::SetKeyState(KeyCode::A, false);
	RunFrame(scene);

	SUBCASE("one part of a composite")
	{
		InputRebindResult const part = scene.RebindInput("Steer", 0, "Key/L", 1);
		CHECK(part.Status == InputRebindStatus::Done);
		CHECK(part.Part == 1);
		CHECK(part.Controls == "Key/A Key/L");
		CHECK(part.Conflicts == std::vector<std::string>{"Jump", "Move"});
		Input::SetKeyState(KeyCode::L, true);
		RunFrame(scene);
		CHECK(scene.GetInputActions().GetValue(Find(scene, "Steer")) == 1.0f);
	}
	SUBCASE("refusals change nothing and say why")
	{
		struct Refusal
		{
			std::string_view Action;
			int32_t Binding;
			std::string_view Controls;
			int32_t Part;
			std::string_view Message;
		};
		for (Refusal const& refusal : {
				 Refusal{"Climb", 0, "Key/C", -1, "no action named 'Climb'"},
				 Refusal{"jump", 0, "Key/C", -1, "no action named 'jump'"},
				 Refusal{"Jump", 2, "Key/C", -1, "'Jump' has 2 bindings, so no binding 2"},
				 Refusal{"Jump", -1, "Key/C", -1, "'Jump' has 2 bindings, so no binding -1"},
				 Refusal{"Jump", 0, "Key/C", 0, "which has no parts"},
				 Refusal{"Steer", 0, "Key/C", 2, "has 2 parts, so no part 2"},
				 Refusal{"Move", 0, "Gamepad/RightStick", 0, "a part takes one control that is not a stick"},
				 Refusal{"Move", 0, "Key/C Key/V", 3, "a part takes one control that is not a stick"},
				 Refusal{"Move", 1, "Key/C", -1, "a Vector2 action takes a stick"},
				 Refusal{"Jump", 0, "Key/Nothing", -1, "'Key/Nothing' names no control"},
				 Refusal{"Jump", 0, "Key/A Key/B Key/C", -1, "not 3"},
			 })
		{
			CAPTURE(refusal.Action);
			CAPTURE(refusal.Message);
			uint64_t const before = InputRebinding::GetVersion();
			InputRebindResult const refused = scene.RebindInput(refusal.Action, refusal.Binding, refusal.Controls, refusal.Part);
			CHECK(refused.Status == InputRebindStatus::Refused);
			CHECK(refused.Message.find(refusal.Message) != std::string::npos);
			CHECK(refused.Controls.empty());
			CHECK(refused.Conflicts.empty());
			CHECK(InputRebinding::GetVersion() == before);
		}
		CHECK(ControlsOf(scene, "Jump", 0) == "Key/A");
	}
	SUBCASE("a binding left unbound, then every binding back to the asset's")
	{
		CHECK(scene.RebindInput("Jump", 1, "").Status == InputRebindStatus::Done);
		CHECK(ControlsOf(scene, "Jump", 1).empty());
		CHECK(InputRebinding::GetProfile().Bindings.size() == 2);
		InputRebindResult const reset = scene.ResetInputBindings();
		CHECK(reset.Status == InputRebindStatus::Done);
		CHECK(reset.Binding == -1);
		CHECK(InputRebinding::GetProfile().Bindings.empty());
		CHECK(ControlsOf(scene, "Jump", 0) == "Key/Space");
		CHECK(ControlsOf(scene, "Jump", 1) == "Gamepad/A");
		// Rebinding to the asset's own controls keeps nothing in the profile.
		CHECK(scene.RebindInput("Jump", 0, "Key/Space").Status == InputRebindStatus::Done);
		CHECK(InputRebinding::GetProfile().Bindings.empty());
	}
	SUBCASE("a scene that does not run, and a project without actions, refuse")
	{
		Scene idle("Idle");
		CHECK(idle.RebindInput("Jump", 0, "Key/C").Status == InputRebindStatus::Refused);
		CHECK(idle.GetInputRebindAnswer().Status == InputRebindStatus::Refused);
		CHECK(idle.GetInputRebindAnswer().Message == "the scene does not run");
		CHECK(idle.StartInputRebind("Jump", 0).Status == InputRebindStatus::Refused);
		CHECK_FALSE(idle.GetInputBindingControls("Jump", 0).IsOk());
		CHECK(InputRebinding::Rebind(AssetHandle(), "Jump", 0, "Key/C", -1).Message == "the project names no input actions");
		CHECK(InputRebinding::ResetBindings(AssetHandle()).Status == InputRebindStatus::Refused);
	}
}

TEST_CASE("SceneInputRebinding: a capture binds the next control at the frame it comes without pressing the action, and Escape or "
          "10 seconds of frames cancel it")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	InputScope const input;
	AssetHandle const actions = AssetManager::AddMemoryAsset(MakeAsset(MakeMap()), "Actions.sinput");
	Scene scene("Capture");
	scene.OnRuntimeStart(Settings(actions));
	RunFrame(scene);
	uint32_t const jump = Find(scene, "Jump");
	CHECK(scene.GetInputRebind().Status == InputRebindStatus::None);

	SUBCASE("the key pressed to bind holds the action down without a press; its release and the next press are edges")
	{
		InputRebindResult const listening = scene.StartInputRebind("Jump", 0);
		CHECK(listening.Status == InputRebindStatus::Listening);
		CHECK(listening.Controls == "Key/Space");
		CHECK(scene.GetInputRebindAnswer().Status == InputRebindStatus::Listening);
		RunFrame(scene);
		CHECK(scene.GetInputRebind().Status == InputRebindStatus::Listening);

		Input::SetKeyState(KeyCode::J, true);
		RunFrame(scene);
		InputRebindResult const& done = scene.GetInputRebind();
		CHECK(done.Status == InputRebindStatus::Done);
		CHECK(done.Controls == "Key/J");
		CHECK(done.Conflicts.empty());
		CHECK(scene.GetInputActions().IsDown(jump));
		CHECK_FALSE(scene.GetInputActions().IsPressed(jump));
		Input::SetKeyState(KeyCode::J, false);
		RunFrame(scene);
		CHECK(scene.GetInputActions().IsReleased(jump));
		Input::SetKeyState(KeyCode::J, true);
		RunFrame(scene);
		CHECK(scene.GetInputActions().IsPressed(jump));
	}
	SUBCASE("binding the control the binding already reads presses nothing either, held or tapped")
	{
		REQUIRE(scene.StartInputRebind("Jump", 0).Status == InputRebindStatus::Listening);
		uint64_t const version = InputRebinding::GetVersion();
		Input::SetKeyState(KeyCode::Space, true);
		RunFrame(scene);
		CHECK(scene.GetInputRebind().Status == InputRebindStatus::Done);
		CHECK(scene.GetInputRebind().Controls == "Key/Space");
		CHECK(InputRebinding::GetVersion() == version);
		CHECK(scene.GetInputActions().IsDown(jump));
		CHECK_FALSE(scene.GetInputActions().IsPressed(jump));
		Input::SetKeyState(KeyCode::Space, false);
		RunFrame(scene);
		CHECK(scene.GetInputActions().IsReleased(jump));

		REQUIRE(scene.StartInputRebind("Jump", 0).Status == InputRebindStatus::Listening);
		Input::SetKeyState(KeyCode::Space, true);
		Input::SetKeyState(KeyCode::Space, false);
		RunFrame(scene);
		CHECK(scene.GetInputRebind().Status == InputRebindStatus::Done);
		CHECK_FALSE(scene.GetInputActions().IsPressed(jump));
		CHECK_FALSE(scene.GetInputActions().IsReleased(jump));
		Input::SetKeyState(KeyCode::Space, true);
		RunFrame(scene);
		CHECK(scene.GetInputActions().IsPressed(jump));
	}
	SUBCASE("a part of a composite takes a key, and a Vector2 binding a stick")
	{
		CHECK(scene.StartInputRebind("Steer", 0, 0).Status == InputRebindStatus::Listening);
		Input::SetKeyState(KeyCode::Left, true);
		RunFrame(scene);
		CHECK(scene.GetInputRebind().Controls == "Key/Left Key/D");

		CHECK(scene.StartInputRebind("Move", 1).Status == InputRebindStatus::Listening);
		Input::GamepadState pad = Input::GetRestingGamepadState();
		pad.Axes[static_cast<size_t>(GamepadAxis::RightX)] = 1.0f;
		Input::InjectGamepadState(0, pad);
		RunFrame(scene);
		CHECK(scene.GetInputRebind().Status == InputRebindStatus::Done);
		CHECK(scene.GetInputRebind().Controls == "Gamepad/RightStick");
		// The stick that was pushed to bind holds Move without a press.
		CHECK(scene.GetInputActions().IsDown(Find(scene, "Move")));
		CHECK_FALSE(scene.GetInputActions().IsPressed(Find(scene, "Move")));
	}
	SUBCASE("Escape cancels, and the binding keeps its controls")
	{
		REQUIRE(scene.StartInputRebind("Jump", 0).Status == InputRebindStatus::Listening);
		Input::SetKeyState(KeyCode::Escape, true);
		RunFrame(scene);
		CHECK(scene.GetInputRebind().Status == InputRebindStatus::Cancelled);
		CHECK(scene.GetInputRebind().Message == "Escape cancelled the capture");
		CHECK(scene.GetInputRebind().Controls == "Key/Space");
		CHECK(ControlsOf(scene, "Jump", 0) == "Key/Space");
	}
	SUBCASE("10 seconds of frames end it, also at a time scale of 0")
	{
		scene.SetTimeScale(0.0f);
		REQUIRE(scene.StartInputRebind("Jump", 0).Status == InputRebindStatus::Listening);
		for (int frame = 0; frame < 599; frame++)
		{
			RunFrame(scene);
		}
		CHECK(scene.GetInputRebind().Status == InputRebindStatus::Listening);
		RunFrame(scene, 2.0f * Step);
		CHECK(scene.GetInputRebind().Status == InputRebindStatus::Cancelled);
		CHECK(scene.GetInputRebind().Message == "no control came within 10 seconds");
	}
	SUBCASE("a cancel or another start ends it; a refused start leaves none listening; the scene's stop forgets it")
	{
		REQUIRE(scene.StartInputRebind("Jump", 0).Status == InputRebindStatus::Listening);
		scene.CancelInputRebind();
		CHECK(scene.GetInputRebind().Status == InputRebindStatus::Cancelled);
		Input::SetKeyState(KeyCode::K, true);
		RunFrame(scene);
		CHECK(ControlsOf(scene, "Jump", 0) == "Key/Space");

		REQUIRE(scene.StartInputRebind("Jump", 0).Status == InputRebindStatus::Listening);
		InputRebindResult const refused = scene.StartInputRebind("Jump", 0, 1);
		CHECK(refused.Status == InputRebindStatus::Refused);
		CHECK(scene.GetInputRebind().Status == InputRebindStatus::Refused);
		Input::SetKeyState(KeyCode::K, false);
		Input::SetKeyState(KeyCode::K, true);
		RunFrame(scene);
		CHECK(ControlsOf(scene, "Jump", 0) == "Key/Space");

		REQUIRE(scene.StartInputRebind("Jump", 0).Status == InputRebindStatus::Listening);
		scene.OnRuntimeStop();
		CHECK(scene.GetInputRebind().Status == InputRebindStatus::None);
	}
}

TEST_CASE("SceneInputRebinding: the profile is written below the game data folder when a binding changes, read when the actions "
          "load, and stale entries are dropped with one line")
{
	Testing::TemporaryDirectory const directory;
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	InputScope const input;
	AssetHandle const actions = AddFileAsset(directory.GetPath() / "Assets", MakeMap());
	std::filesystem::path const data = directory.GetPath() / "Data";
	std::filesystem::path const profile = data / "Input" / "Game.sinput.json";
	GameDataScope const gameData(data, GameDataLifetime::Kept);
	CHECK(InputRebinding::GetProfilePath(actions) == profile);

	{
		Scene first("First");
		first.OnRuntimeStart(Settings(actions));
		RunFrame(first);
		CHECK_FALSE(FileSystem::Exists(profile));
		REQUIRE(first.RebindInput("Jump", 0, "Key/J").Status == InputRebindStatus::Done);
		REQUIRE(first.RebindInput("Move", 0, "Key/Up", 0).Status == InputRebindStatus::Done);
		InputBindingProfile const written = ReadProfile(profile);
		REQUIRE(written.Bindings.size() == 2);
		CHECK(written.Bindings[0] == InputBindingProfileEntry{"Jump", 0, "Key/J", "Key/Space"});
		CHECK(written.Bindings[1] == InputBindingProfileEntry{"Move", 0, "Key/Up Key/S Key/A Key/D", "Key/W Key/S Key/A Key/D"});
		first.OnRuntimeStop();

		// A scene loaded after it in the same run keeps the bindings.
		Scene second("Second");
		second.OnRuntimeStart(Settings(actions));
		CHECK(ControlsOf(second, "Jump", 0) == "Key/J");
	}

	SUBCASE("the next run reads them as its actions load")
	{
		InputRebinding::Clear();
		uint64_t const first = Log::GetNextEntryIndex();
		Scene next("Next");
		next.OnRuntimeStart(Settings(actions));
		CHECK(CountLogged(first, "Input bindings: 2 changed bindings from") == 1);
		CHECK(ControlsOf(next, "Jump", 0) == "Key/J");
		Input::SetKeyState(KeyCode::J, true);
		RunFrame(next);
		CHECK(next.GetInputActions().IsPressed(Find(next, "Jump")));

		// A reset writes the profile without entries.
		REQUIRE(next.ResetInputBindings().Status == InputRebindStatus::Done);
		CHECK(ReadProfile(profile).Bindings.empty());
	}
	SUBCASE("entries the asset no longer matches are dropped, all in one line, and the rest applies")
	{
		InputBindingProfile stale = ReadProfile(profile);
		stale.Bindings.push_back(InputBindingProfileEntry{"Climb", 0, "Key/C", "Key/Space"});
		stale.Bindings.push_back(InputBindingProfileEntry{"Jump", 1, "Gamepad/B", "Gamepad/X"});
		REQUIRE(FileSystem::WriteTextFile(profile, DumpJson(WriteInputBindingProfile(stale))).IsOk());
		InputRebinding::Clear();
		uint64_t const first = Log::GetNextEntryIndex();
		Scene next("Next");
		next.OnRuntimeStart(Settings(actions));
		CHECK(CountLogged(first, "Input bindings: 2 of 4 changed bindings of") == 1);
		CHECK(CountLogged(first, "Climb binding 0: the map has no action of that name; Jump binding 1: the asset's binding reads") == 1);
		CHECK(CountLogged(first, "no longer match") == 1);
		CHECK(ControlsOf(next, "Jump", 0) == "Key/J");
		CHECK(ControlsOf(next, "Jump", 1) == "Gamepad/A");
		CHECK(InputRebinding::GetProfile().Bindings.size() == 2);
		// The file keeps them until a binding changes, which writes what applies.
		CHECK(ReadProfile(profile).Bindings.size() == 4);
		REQUIRE(next.RebindInput("Jump", 1, "Gamepad/Y").Status == InputRebindStatus::Done);
		CHECK(ReadProfile(profile).Bindings.size() == 3);
	}
	SUBCASE("a damaged profile, or one of a newer version, is logged and the bindings are the asset's until one changes")
	{
		for (std::string_view const text :
		     {std::string_view("{ not json"),
		      std::string_view(R"({"Strada": {"Version": 9, "Type": "InputBindingProfile"}, "InputBindingProfile": {}})")})
		{
			CAPTURE(text);
			REQUIRE(FileSystem::WriteTextFile(profile, text).IsOk());
			InputRebinding::Clear();
			uint64_t const first = Log::GetNextEntryIndex();
			Scene next("Next");
			next.OnRuntimeStart(Settings(actions));
			CHECK(CountLogged(first, "is not read") == 1);
			CHECK(ControlsOf(next, "Jump", 0) == "Key/Space");
			REQUIRE(next.RebindInput("Jump", 0, "Key/H").Status == InputRebindStatus::Done);
			CHECK(ReadProfile(profile).Bindings.size() == 1);
		}
	}
}

TEST_CASE("SceneInputRebinding: without a game data folder, or for actions that are no file, nothing is read or written")
{
	Testing::TemporaryDirectory const directory;
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	InputScope const input;
	AssetHandle const file = AddFileAsset(directory.GetPath() / "Assets", MakeMap());
	AssetHandle const memory = AssetManager::AddMemoryAsset(MakeAsset(MakeMap()), "Memory.sinput");
	REQUIRE_FALSE(GameData::GetDirectory().has_value());

	SUBCASE("no game runs: the bindings hold in memory alone")
	{
		CHECK_FALSE(InputRebinding::GetProfilePath(file).has_value());
		Scene scene("Editing");
		scene.OnRuntimeStart(Settings(file));
		InputRebindResult const done = scene.RebindInput("Jump", 0, "Key/J");
		CHECK(done.Status == InputRebindStatus::Done);
		CHECK(done.Message.empty());
		CHECK(ControlsOf(scene, "Jump", 0) == "Key/J");
	}
	SUBCASE("a memory asset has no profile")
	{
		GameDataScope const gameData(directory.GetPath() / "Data", GameDataLifetime::Kept);
		CHECK_FALSE(InputRebinding::GetProfilePath(memory).has_value());
		Scene scene("Memory");
		scene.OnRuntimeStart(Settings(memory));
		CHECK(scene.RebindInput("Jump", 0, "Key/J").Status == InputRebindStatus::Done);
	}
	CHECK_FALSE(FileSystem::Exists(directory.GetPath() / "Data"));
	CHECK_FALSE(FileSystem::Exists(directory.GetPath() / "Assets" / "Input" / "Game.sinput.json"));
}

TEST_CASE("SceneInputRebinding: C# scripts rebind, capture, reset and read bindings through Input")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::ScriptEngineScope const scripting;
	InputScope const input;
	AssetHandle const actions = AssetManager::AddMemoryAsset(MakeAsset(MakeMap()), "Actions.sinput");
	SceneRuntimeSettings settings = Settings(actions);
	settings.RunScripts = true;
	Scene scene("Scripts");
	auto const addProbe = [&scene](std::string const& name, ScriptFieldMap fields)
	{
		Entity entity = scene.CreateEntity(name);
		ScriptComponent& script = entity.AddComponent<ScriptComponent>();
		script.ClassName = "Strada.Tests.ActionProbe";
		script.Fields = std::move(fields);
		return entity;
	};
	auto const read = [](Entity probe, std::string const& field)
	{
		Result<ScriptFieldMap> fields = ScriptEngine::GetInstanceFields(probe.GetUUID());
		REQUIRE_MESSAGE(fields.IsOk(), (fields ? std::string() : fields.GetError()));
		return fields.GetValue().at(field).GetString();
	};
	// Frame 1 rebinds Steer's D to L; frame 2 starts a capture of Jump; frame 6 resets; a refusal on frame 1 of another probe.
	Entity const steer = addProbe("Steer", {{"Action", ScriptFieldValue::FromString("Steer")},
	                                        {"RebindFrame", ScriptFieldValue::FromInt32(1)},
	                                        {"RebindPart", ScriptFieldValue::FromInt32(1)},
	                                        {"RebindControls", ScriptFieldValue::FromString("Key/L")},
	                                        {"ResetFrame", ScriptFieldValue::FromInt32(6)}});
	Entity const jump =
		addProbe("Jump", {{"RebindFrame", ScriptFieldValue::FromInt32(2)}, {"RebindControls", ScriptFieldValue::FromString("capture")}});
	Entity const refused = addProbe("Refused", {{"Action", ScriptFieldValue::FromString("Climb")},
	                                            {"RebindFrame", ScriptFieldValue::FromInt32(1)},
	                                            {"RebindControls", ScriptFieldValue::FromString("Key/C")}});
	scene.OnRuntimeStart(settings);

	RunFrame(scene);
	CHECK(read(steer, "Rebound") == "Done Key/A Key/L [Move] ");
	CHECK(read(steer, "Bound") == "Key/A Key/L");
	CHECK(read(steer, "Capture") == "None  [] ");
	CHECK(read(refused, "Rebound") == "Refused  [] the input actions have no action named 'Climb'");
	CHECK(read(refused, "Bound").empty());

	RunFrame(scene);
	CHECK(read(jump, "Rebound") == "Listening Key/Space [] ");
	CHECK(read(jump, "Capture") == "Listening Key/Space [] ");
	// The capture takes L at the next frame: Jump now shares it with Steer.
	Input::SetKeyState(KeyCode::L, true);
	RunFrame(scene);
	CHECK(read(jump, "Capture") == "Done Key/L [Steer] ");
	CHECK(read(jump, "Bound") == "Key/L");
	CHECK(read(steer, "Capture") == "Done Key/L [Steer] ");
	Input::SetKeyState(KeyCode::L, false);
	RunFrame(scene);
	RunFrame(scene);
	RunFrame(scene);
	// Every probe has read the reset (frame 6) once frame 7 ran.
	RunFrame(scene);
	CHECK(read(steer, "Rebound") == "Done  [] ");
	CHECK(read(steer, "Bound") == "Key/A Key/D");
	CHECK(read(jump, "Bound") == "Key/Space");
}
