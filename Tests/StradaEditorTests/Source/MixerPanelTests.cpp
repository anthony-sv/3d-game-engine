#include "Audio/AudioTestUtilities.h"
#include "HeadlessImGui.h"
#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/MixerEditing.h"
#include "Editor/Panels/MixerPanel.h"
#include "Editor/PlayMode.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/Mixer.h"
#include "Strada/Asset/MixerAsset.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Project/Project.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;
using Strada::Testing::HeadlessImGui;

namespace
{
	using Record = MixerPanel::DrawRecord;

	ImVec2 ToImVec2(Record::Point point)
	{
		return ImVec2(point[0], point[1]);
	}

	// Master over Music (ducked by Effects, a low-pass and a reverb), Effects over Voices; the snapshot Quiet lowers Music,
	// sets its reverb's wet level and mutes Effects; impact sounds play on Effects.
	constexpr char const* GameMixer = R"({
		"Buses": [
			{ "Name": "Master" },
			{ "Name": "Music", "Parent": "Master", "VolumeDb": -6.0, "DuckedBy": "Effects",
			  "Effects": [ { "Kind": "LowPass", "Frequency": 8000.0 }, { "Kind": "Reverb" } ] },
			{ "Name": "Effects", "Parent": "Master" },
			{ "Name": "Voices", "Parent": "Effects" }
		],
		"Snapshots": [
			{ "Name": "Quiet", "Values": [
				{ "Bus": "Music", "Target": "Volume", "Value": -20.0 },
				{ "Bus": "Music", "Target": "Effect", "Effect": 1, "Parameter": "WetDb", "Value": -3.0 },
				{ "Bus": "Effects", "Target": "Muted", "Value": 1.0 }
			] }
		],
		"Impacts": { "Bus": "Effects" }
	})";

	// A project whose Project Settings name a mixer (Audio/Game.smixer), shown by a Mixer panel placed at the top left of the
	// display.
	struct MixerPanelFixture
	{
		Testing::AssetManagerScope Assets;
		HeadlessImGui Gui;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		MixerPanel Panel;
		AssetHandle Asset;
		std::filesystem::path File;
		bool Open = true;
		bool Draw = true;

		explicit MixerPanelFixture(bool nameMixer = true)
		{
			REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
			Asset = CreateMixer("Audio/Game.smixer", GameMixer);
			File = Context.GetProject()->GetAssetDirectory() / "Audio" / "Game.smixer";
			if (nameMixer)
			{
				REQUIRE(Operations.ApplyProjectSettings(Json::object({{"Mixer", Asset.ToString()}})).IsOk());
			}
			Frames(3);
		}

		AssetHandle CreateMixer(std::string_view path, char const* fields)
		{
			DataAssetType const* const type = DataAssetRegistry::FindByName("Mixer");
			REQUIRE(type != nullptr);
			Result<AssetHandle> const created = Operations.CreateDataAsset(type->GetAssetType(), path, Json::parse(fields));
			REQUIRE_MESSAGE(created.IsOk(), (created ? std::string() : created.GetError()));
			return created.GetValue();
		}

		std::function<void()> Drawer()
		{
			return [this]
			{
				if (!Draw)
				{
					return;
				}
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
				ImGui::SetNextWindowSize(ImVec2(1500.0f, 1000.0f));
				Panel.OnImGuiRender(Operations, Open);
			};
		}

		void Frames(int count)
		{
			for (int frame = 0; frame < count; frame++)
			{
				Gui.Frame(Drawer());
			}
		}

		Record const& GetRecord() const { return Panel.GetLastDraw(); }

		Record::Strip const& GetStrip(std::string_view name) const
		{
			for (Record::Strip const& strip : GetRecord().Strips)
			{
				if (strip.Name == name)
				{
					return strip;
				}
			}
			FAIL("no strip for the bus ", name);
			return GetRecord().Strips.front();
		}

		Mixer ReadMixer() const
		{
			Ref<TypedDataAsset<Mixer>> const asset = AssetManager::GetAsset<TypedDataAsset<Mixer>>(Asset);
			REQUIRE(asset != nullptr);
			return asset->GetData();
		}

		std::string ReadFile() const
		{
			Result<std::string> const text = FileSystem::ReadTextFile(File);
			REQUIRE(text.IsOk());
			return text.GetValue();
		}

		size_t GetUndoCount() const { return Context.GetHistory().GetUndoCount(); }

		// Clicks a widget of the panel, then draws a frame that records what the click did.
		void Click(Record::Point point)
		{
			Gui.Click(ToImVec2(point), Drawer());
			Frames(1);
		}

		// Replaces the text of the selected bus's name field and presses Enter.
		void TypeName(char const* name)
		{
			Click(GetRecord().NameField);
			Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, Drawer());
			Gui.Type(name);
			Frames(1);
			Gui.Press(ImGuiKey_Enter, Drawer());
			Frames(2);
		}
	};

	MixerBus const* FindBus(Mixer const& mixer, std::string_view name)
	{
		for (MixerBus const& bus : mixer.Buses)
		{
			if (bus.Name == name)
			{
				return &bus;
			}
		}
		return nullptr;
	}
}

TEST_CASE("MixerPanel: the project's mixer shows a strip per bus with its volume and mute, and drawing changes nothing")
{
	MixerPanelFixture fixture;
	std::string const bytes = fixture.ReadFile();
	size_t const undo = fixture.GetUndoCount();
	fixture.Frames(2);
	Record const& record = fixture.GetRecord();
	CHECK(record.Drawn);
	CHECK(record.Asset == fixture.Asset);
	CHECK(record.FollowsProject);
	CHECK(record.Editable);
	CHECK_FALSE(record.Live);
	CHECK(record.Status == "Play the game to see the meters and audition snapshots.");
	REQUIRE(record.Strips.size() == 4);
	CHECK(record.Strips[0].Name == "Master");
	CHECK(record.Strips[1].Name == "Music");
	CHECK(record.Strips[1].VolumeDb == -6.0f);
	CHECK_FALSE(record.Strips[1].Muted);
	CHECK(record.Strips[3].Name == "Voices");
	// Nothing plays: the meters are silent.
	CHECK(record.Strips[1].Peak == 0.0f);
	CHECK(record.Strips[1].Rms == 0.0f);
	// Master is selected until another bus is.
	REQUIRE(record.Selected.has_value());
	CHECK(*record.Selected == 0);
	CHECK(record.Strips[0].Selected);
	REQUIRE(record.Snapshots.size() == 1);
	CHECK(record.Snapshots[0].Name == "Quiet");
	CHECK(record.Snapshots[0].ValueCount == 3);
	CHECK_FALSE(record.Snapshots[0].CanAudition);
	CHECK_FALSE(record.Snapshots[0].Auditioning);
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(fixture.ReadFile() == bytes);

	// Another mixer opened in the panel is not what the game mixes through.
	AssetHandle const other = fixture.CreateMixer("Audio/Other.smixer", R"({ "Buses": [ { "Name": "Master" } ] })");
	fixture.Panel.OpenAsset(other);
	fixture.Frames(2);
	CHECK(record.Asset == other);
	CHECK_FALSE(record.FollowsProject);
	CHECK(record.Strips.size() == 1);
	CHECK(record.Status == "The game mixes through the project's mixer: this one shows no meters and auditions nothing.");
	fixture.Panel.OpenAsset(AssetHandle());
	fixture.Frames(2);
	CHECK(record.Asset == fixture.Asset);
}

TEST_CASE("MixerPanel: without a project or a mixer named, the panel says so")
{
	{
		Testing::AssetManagerScope assets;
		HeadlessImGui gui;
		EditorContext context;
		EditorOperations operations{context};
		MixerPanel panel;
		bool open = true;
		gui.Frame(
			[&panel, &operations, &open]
			{
				panel.OnImGuiRender(operations, open);
			});
		CHECK(panel.GetLastDraw().Status == "Open a project to edit its mixer.");
		CHECK(panel.GetLastDraw().Strips.empty());
	}
	MixerPanelFixture fixture(false);
	CHECK_FALSE(fixture.GetRecord().Asset.IsValid());
	CHECK(fixture.GetRecord().Strips.empty());
	CHECK(fixture.GetRecord().Status == "The project names no mixer: choose a Mixer asset above, or name one in Project Settings (Mixer).");
}

TEST_CASE("MixerPanel: a fader's drag is one undo step of the asset, and a mute is a step of its own")
{
	MixerPanelFixture fixture;
	size_t const undo = fixture.GetUndoCount();
	Record::Strip const music = fixture.GetStrip("Music");
	fixture.Gui.DragPath(ToImVec2(music.FaderBottom), ToImVec2(music.FaderTop), fixture.Drawer(), ImGuiMouseButton_Left, 8);
	fixture.Frames(2);
	float const raised = fixture.ReadMixer().Buses[1].VolumeDb;
	CHECK(raised > 20.0f);
	CHECK(raised <= MaxMixerVolumeDb);
	CHECK(fixture.GetStrip("Music").VolumeDb == raised);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Set the Volume of Music in Game");
	CHECK(fixture.ReadFile().find("\"VolumeDb\": -6.0") == std::string::npos);

	fixture.Click(fixture.GetStrip("Voices").Mute);
	CHECK(fixture.ReadMixer().Buses[3].Muted);
	CHECK(fixture.GetStrip("Voices").Muted);
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Mute Voices in Game");

	// Undone, each step returns the asset as it was.
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK_FALSE(fixture.ReadMixer().Buses[3].Muted);
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadMixer().Buses[1].VolumeDb == -6.0f);
	fixture.Frames(2);
	CHECK(fixture.GetStrip("Music").VolumeDb == -6.0f);
}

TEST_CASE("MixerPanel: buses are added below the one selected, renamed with what names them, and removed with what only they named")
{
	MixerPanelFixture fixture;
	size_t const undo = fixture.GetUndoCount();

	// A click on a strip's name selects its bus; Add Bus adds one that feeds it, and selects the new bus.
	fixture.Click(fixture.GetStrip("Music").Label);
	REQUIRE(fixture.GetRecord().Selected.has_value());
	CHECK(*fixture.GetRecord().Selected == 1);
	fixture.Click(fixture.GetRecord().AddBus);
	fixture.Frames(1);
	Mixer added = fixture.ReadMixer();
	REQUIRE(added.Buses.size() == 5);
	CHECK(added.Buses[4].Name == "Bus");
	CHECK(added.Buses[4].Parent == "Music");
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Add Bus Bus to Game");
	REQUIRE(fixture.GetRecord().Selected.has_value());
	CHECK(*fixture.GetRecord().Selected == 4);

	// A rename reaches the buses it feeds, the bus it ducks, the snapshots' values and the impact sounds.
	fixture.Click(fixture.GetStrip("Effects").Label);
	fixture.TypeName("Sounds");
	Mixer renamed = fixture.ReadMixer();
	REQUIRE(FindBus(renamed, "Sounds") != nullptr);
	CHECK(FindBus(renamed, "Effects") == nullptr);
	CHECK(FindBus(renamed, "Voices")->Parent == "Sounds");
	CHECK(FindBus(renamed, "Music")->DuckedBy == "Sounds");
	CHECK(renamed.Snapshots[0].Values[2].Bus == "Sounds");
	CHECK(renamed.Impacts.Bus == "Sounds");
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Rename Bus Effects to Sounds in Game");

	// A name another bus has is refused by validation: nothing changes, and the panel says why.
	std::string const bytes = fixture.ReadFile();
	fixture.TypeName("music");
	CHECK(fixture.GetUndoCount() == undo + 2);
	CHECK(fixture.ReadFile() == bytes);
	CHECK(FindBus(fixture.ReadMixer(), "Sounds") != nullptr);
	CHECK_FALSE(fixture.GetRecord().Status.empty());
	CHECK(fixture.GetRecord().Status.find("Play the game") == std::string::npos);

	// Removed, its buses feed its parent, the bus it ducked is ducked no more, its snapshot values and impacts go.
	fixture.Click(fixture.GetStrip("Sounds").Label);
	fixture.Click(fixture.GetRecord().RemoveBus);
	Mixer removed = fixture.ReadMixer();
	CHECK(FindBus(removed, "Sounds") == nullptr);
	CHECK(FindBus(removed, "Voices")->Parent == "Master");
	CHECK(FindBus(removed, "Music")->DuckedBy.empty());
	CHECK(removed.Snapshots[0].Values.size() == 2);
	CHECK(removed.Impacts.Bus.empty());
	CHECK(fixture.GetUndoCount() == undo + 3);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Remove Bus Sounds from Game");
	// Each is one step back.
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadMixer() == renamed);
	REQUIRE(fixture.Operations.Undo().IsOk());
	CHECK(fixture.ReadMixer() == added);

	// Master keeps its name and its place.
	fixture.Click(fixture.GetStrip("Master").Label);
	size_t const steps = fixture.GetUndoCount();
	fixture.Click(fixture.GetRecord().RemoveBus);
	fixture.TypeName("Main");
	CHECK(fixture.GetUndoCount() == steps);
	CHECK(fixture.ReadMixer().Buses[0].Name == "Master");
}

TEST_CASE("MixerPanel: a snapshot is added as a step of its own, and a read-only mixer is shown disabled")
{
	MixerPanelFixture fixture;
	size_t const undo = fixture.GetUndoCount();
	fixture.Click(fixture.GetRecord().AddSnapshot);
	fixture.Frames(1);
	REQUIRE(fixture.ReadMixer().Snapshots.size() == 2);
	CHECK(fixture.ReadMixer().Snapshots[1].Name == "Snapshot");
	CHECK(fixture.GetRecord().Snapshots.size() == 2);
	CHECK(fixture.GetUndoCount() == undo + 1);
	CHECK(fixture.Context.GetHistory().GetUndoDescription() == "Add Snapshot Snapshot to Game");

	std::string const bytes = fixture.ReadFile();
	size_t const steps = fixture.GetUndoCount();
	Testing::ReadOnlyFileScope const readOnly(fixture.File);
	fixture.Frames(2);
	CHECK_FALSE(fixture.GetRecord().Editable);
	CHECK_FALSE(fixture.GetRecord().Status.empty());
	CHECK(fixture.GetRecord().Strips.size() == 4);
	fixture.Click(fixture.GetStrip("Music").Mute);
	fixture.Click(fixture.GetRecord().AddBus);
	fixture.Click(fixture.GetRecord().AddSnapshot);
	Record::Strip const music = fixture.GetStrip("Music");
	fixture.Gui.DragPath(ToImVec2(music.FaderBottom), ToImVec2(music.FaderTop), fixture.Drawer());
	fixture.Frames(1);
	CHECK(fixture.GetUndoCount() == steps);
	CHECK(fixture.ReadFile() == bytes);
}

TEST_CASE("MixerEditing: names are free ones, and snapshot values follow their bus's effects when the list changes")
{
	Mixer mixer;
	MixerBus music;
	music.Name = "Music";
	music.Parent = "Master";
	music.Effects.resize(3);
	mixer.Buses.push_back(music);
	MixerBus bus;
	bus.Name = "bus";
	mixer.Buses.push_back(bus);
	CHECK(MakeMixerBusName(mixer) == "Bus2");
	CHECK(MakeMixerSnapshotName(mixer) == "Snapshot");
	CHECK(AddMixerBus(mixer, "") == 3);
	CHECK(mixer.Buses[3].Name == "Bus2");
	CHECK(mixer.Buses[3].Parent == "Master");
	CHECK_FALSE(RemoveMixerBus(mixer, 0));
	CHECK_FALSE(RemoveMixerBus(mixer, 9));

	auto const makeValue = [](std::string busName, uint32_t effect)
	{
		MixerSnapshotValue value;
		value.Bus = std::move(busName);
		value.Target = MixerSnapshotTarget::Effect;
		value.Effect = effect;
		return value;
	};
	MixerSnapshot snapshot;
	snapshot.Name = "Edit";
	snapshot.Values = {makeValue("music", 0), makeValue("Music", 1), makeValue("Music", 2), makeValue("Bus2", 1)};
	mixer.Snapshots.push_back(snapshot);

	// The second effect removed: its value goes, the third's moves down, another bus's stays.
	std::array<std::optional<uint32_t>, 2> const removed = {0u, 2u};
	RemapMixerSnapshotEffects(mixer, "MUSIC", removed);
	REQUIRE(mixer.Snapshots[0].Values.size() == 3);
	CHECK(mixer.Snapshots[0].Values[0].Effect == 0);
	CHECK(mixer.Snapshots[0].Values[1].Effect == 1);
	CHECK(mixer.Snapshots[0].Values[2].Bus == "Bus2");
	CHECK(mixer.Snapshots[0].Values[2].Effect == 1);

	// Moved first to last, an effect inserted before them, one duplicated: values follow their originals.
	std::array<std::optional<uint32_t>, 4> const moved = {std::nullopt, 1u, 0u, 0u};
	RemapMixerSnapshotEffects(mixer, "Music", moved);
	CHECK(mixer.Snapshots[0].Values[0].Effect == 2);
	CHECK(mixer.Snapshots[0].Values[1].Effect == 1);
	// Not an effect's value: left as it is.
	MixerSnapshotValue volume;
	volume.Bus = "Music";
	volume.Target = MixerSnapshotTarget::Volume;
	volume.Effect = 7;
	mixer.Snapshots[0].Values.push_back(volume);
	RemapMixerSnapshotEffects(mixer, "Music", std::array<std::optional<uint32_t>, 0>{});
	REQUIRE(mixer.Snapshots[0].Values.size() == 2);
	CHECK(mixer.Snapshots[0].Values[1].Target == MixerSnapshotTarget::Volume);
	CHECK(mixer.Snapshots[0].Values[1].Effect == 7);
}

TEST_CASE("MixerPanel: while the game plays through the mixer, the strips meter its buses and Audition blends the mix")
{
	Testing::PhysicsSystemScope physics;
	Testing::AudioEngineScope engine;
	MixerPanelFixture fixture;
	// A tone on Music, playing from the start.
	AssetHandle const tone = AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.5f, 2.0f}}), "Tone");
	Entity speaker = fixture.Context.GetScene().CreateEntity("Speaker");
	AudioSourceComponent& source = speaker.AddComponent<AudioSourceComponent>();
	source.Clip = tone;
	source.PlayOnStart = true;
	source.Loop = true;
	source.Spatial = false;
	source.Bus = "Music";
	EditorScripts scripts{fixture.Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
	PlayMode play{fixture.Context, scripts};
	REQUIRE(play.Start(EditorPlayState::Play, 640, 480).IsOk());
	std::vector<float> samples(AudioBlockFrames * 4 * AudioEngine::GetChannelCount());
	auto const step = [&play, &samples, &fixture]
	{
		play.Update(Timestep(1.0f / 60.0f));
		REQUIRE(AudioEngine::ReadFrames(samples) == AudioBlockFrames * 4);
		fixture.Frames(1);
	};
	step();
	step();
	Record const& record = fixture.GetRecord();
	REQUIRE(record.Live);
	CHECK(record.CurrentSnapshot.empty());
	CHECK(fixture.GetStrip("Music").Peak > 0.0f);
	CHECK(fixture.GetStrip("Music").Rms > 0.0f);
	CHECK(fixture.GetStrip("Master").Peak > 0.0f);
	CHECK(fixture.GetStrip("Voices").Peak == 0.0f);
	REQUIRE(record.Snapshots.size() == 1);
	CHECK(record.Snapshots[0].CanAudition);

	// Audition blends the running mix to the snapshot, never as an undo step; pressed again, back.
	size_t const undo = fixture.GetUndoCount();
	std::string const bytes = fixture.ReadFile();
	fixture.Click(record.Snapshots[0].Audition);
	CHECK(AudioEngine::GetCurrentSnapshot() == "Quiet");
	CHECK(record.Snapshots[0].Auditioning);
	CHECK(record.CurrentSnapshot == "Quiet");
	step();
	fixture.Click(record.Snapshots[0].Audition);
	CHECK(AudioEngine::GetCurrentSnapshot().empty());
	CHECK_FALSE(record.Snapshots[0].Auditioning);

	// Hidden, the panel's audition ends and the mix blends back.
	fixture.Click(record.Snapshots[0].Audition);
	REQUIRE(AudioEngine::GetCurrentSnapshot() == "Quiet");
	fixture.Draw = false;
	fixture.Panel.OnHidden();
	CHECK(AudioEngine::GetCurrentSnapshot().empty());
	fixture.Draw = true;
	step();

	// The game moving the mix itself ends the audition, and the panel leaves the mix where the game put it.
	fixture.Click(record.Snapshots[0].Audition);
	REQUIRE(AudioEngine::GetCurrentSnapshot() == "Quiet");
	REQUIRE(AudioEngine::TransitionTo("", 0.0f));
	step();
	CHECK_FALSE(record.Snapshots[0].Auditioning);
	REQUIRE(AudioEngine::TransitionTo("Quiet", 0.0f));
	fixture.Panel.OnHidden();
	CHECK(AudioEngine::GetCurrentSnapshot() == "Quiet");
	REQUIRE(AudioEngine::TransitionTo("", 0.0f));
	step();
	CHECK(fixture.GetUndoCount() == undo);
	CHECK(fixture.ReadFile() == bytes);

	// An edit made while the game plays is heard: the fader reaches the running mix through the scene runner.
	Record::Strip const music = fixture.GetStrip("Music");
	fixture.Gui.DragPath(ToImVec2(music.FaderTop), ToImVec2(music.FaderBottom), fixture.Drawer());
	step();
	CHECK(AudioEngine::GetMixer().Buses[1].VolumeDb == MinMixerVolumeDb);

	// Stopped, the panel is live no more.
	fixture.Click(record.Snapshots[0].Audition);
	play.Stop();
	fixture.Frames(1);
	CHECK_FALSE(record.Live);
	CHECK_FALSE(record.Snapshots[0].CanAudition);
	CHECK_FALSE(record.Snapshots[0].Auditioning);
	CHECK(fixture.GetStrip("Music").Peak == 0.0f);
}
