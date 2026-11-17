#include "Audio/AudioTestUtilities.h"
#include "Fuzzing.h"
#include "TestUtilities.h"

#include "Strada/Asset/Asset.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/Mixer.h"
#include "Strada/Asset/MixerAsset.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	DataAssetType const& GetMixerType()
	{
		DataAssetType const* type = DataAssetRegistry::FindByName("Mixer");
		REQUIRE(type != nullptr);
		return *type;
	}

	Json MakeMixerDocument(Json content)
	{
		Json document = Json::object();
		document["Strada"] = MakeFileHeader("Mixer", 1);
		document["Mixer"] = std::move(content);
		return document;
	}

	MixerBus MakeBus(std::string name, std::string parent)
	{
		MixerBus bus;
		bus.Name = std::move(name);
		bus.Parent = std::move(parent);
		return bus;
	}

	Mixer MakeMixer(std::vector<MixerBus> buses)
	{
		Mixer mixer;
		mixer.Buses = std::move(buses);
		return mixer;
	}

	std::string ValidationError(Mixer const& mixer)
	{
		Result<void> const result = ValidateMixer(mixer);
		REQUIRE(result.IsError());
		return result.GetError();
	}

	// A mixer that sets every value away from its default, as the FeatureTest seed does.
	Mixer MakeFullMixer()
	{
		MixerEffect limiter;
		limiter.Kind = MixerEffectKind::Limiter;
		limiter.CeilingDb = -2.0f;
		limiter.ReleaseSeconds = 0.2f;
		MixerEffect reverb;
		reverb.Kind = MixerEffectKind::Reverb;
		reverb.RoomSize = 0.7f;
		reverb.Damping = 0.3f;
		reverb.Width = 0.8f;
		reverb.WetDb = -9.0f;
		reverb.DryDb = -1.0f;
		MixerEffect compressor;
		compressor.Kind = MixerEffectKind::Compressor;
		compressor.ThresholdDb = -18.0f;
		compressor.Ratio = 3.0f;
		compressor.KneeDb = 4.0f;
		compressor.AttackSeconds = 0.01f;
		compressor.MakeupDb = 2.0f;
		MixerEffect delay;
		delay.Kind = MixerEffectKind::Delay;
		delay.DelaySeconds = 0.3f;
		delay.Feedback = 0.4f;
		MixerEffect peaking;
		peaking.Kind = MixerEffectKind::Peaking;
		peaking.Frequency = 2500.0f;
		peaking.Q = 1.5f;
		peaking.GainDb = -4.0f;
		Mixer mixer;
		mixer.Buses = {MakeBus("Master", ""), MakeBus("Music", "Master"), MakeBus("Effects", "Master"), MakeBus("Voices", "Effects")};
		mixer.Buses[0].VolumeDb = -1.0f;
		mixer.Buses[0].Effects = {limiter};
		mixer.Buses[1].Effects = {peaking, reverb};
		mixer.Buses[2].Muted = true;
		mixer.Buses[2].Effects = {delay, compressor};
		mixer.Buses[3].VolumeDb = 3.0f;
		// The voices duck the music; a snapshot lowers the effects, mutes the voices and moves the music's peak down.
		mixer.Buses[1].DuckedBy = "Voices";
		mixer.Buses[1].DuckAmountDb = -9.0f;
		mixer.Buses[1].DuckThresholdDb = -30.0f;
		mixer.Buses[1].DuckAttackSeconds = 0.1f;
		mixer.Buses[1].DuckReleaseSeconds = 0.8f;
		MixerSnapshot paused;
		paused.Name = "Paused";
		paused.Values = {
			MixerSnapshotValue{.Bus = "Effects",
		                       .Target = MixerSnapshotTarget::Volume,
		                       .Effect = 0,
		                       .Parameter = MixerEffectParameter::Frequency,
		                       .Value = -12.0f},
			MixerSnapshotValue{.Bus = "Voices",
		                       .Target = MixerSnapshotTarget::Muted,
		                       .Effect = 0,
		                       .Parameter = MixerEffectParameter::Frequency,
		                       .Value = 1.0f},
			MixerSnapshotValue{.Bus = "Music",
		                       .Target = MixerSnapshotTarget::Effect,
		                       .Effect = 0,
		                       .Parameter = MixerEffectParameter::Frequency,
		                       .Value = 800.0f},
		};
		mixer.Snapshots = {paused, MixerSnapshot{.Name = "Quiet", .Values = {}}};
		return mixer;
	}

	MixerSnapshotValue SetVolume(std::string bus, float decibels)
	{
		return MixerSnapshotValue{.Bus = std::move(bus),
		                          .Target = MixerSnapshotTarget::Volume,
		                          .Effect = 0,
		                          .Parameter = MixerEffectParameter::Frequency,
		                          .Value = decibels};
	}

	MixerSnapshotValue SetParameter(std::string bus, uint32_t effect, MixerEffectParameter parameter, float value)
	{
		return MixerSnapshotValue{
			.Bus = std::move(bus), .Target = MixerSnapshotTarget::Effect, .Effect = effect, .Parameter = parameter, .Value = value};
	}
}

TEST_CASE("MixerAsset: the engine registers Mixer, its .smixer files and its commands' names")
{
	DataAssetType const& type = GetMixerType();
	CHECK(type.IsEngineType());
	CHECK(DataAssetRegistry::Find<Mixer>() == &type);
	CHECK(TypedDataAsset<Mixer>::GetStaticType() == type.GetAssetType());
	CHECK(type.GetExtension() == ".smixer");
	CHECK(type.GetFormatVersion() == 1);
	CHECK(DataAssetRegistry::FindByExtension(".SMIXER") == &type);
	CHECK(GetAssetTypeForExtension(".smixer") == type.GetAssetType());
	CHECK(std::string_view(AssetTypeToString(type.GetAssetType())) == "Mixer");
	CHECK(type.GetCommandDomain() == "mixer");
	CHECK(type.GetCommandParameter() == "mixer");
	REQUIRE(type.GetFields().size() == 2);
	CHECK(type.GetFields()[0].Name == "Buses");
	CHECK(type.GetFields()[1].Name == "Snapshots");
	CHECK_FALSE(type.GetFields()[0].Hints.Description.empty());
	CHECK_FALSE(type.GetFields()[1].Hints.Description.empty());
	// A new mixer is Master alone, at 0 dB without effects or ducking, and has no snapshot.
	Json const defaults = type.GetDefaultFields();
	REQUIRE(defaults["Buses"].size() == 1);
	Json const& master = defaults["Buses"][0];
	CHECK(master["Name"] == "Master");
	CHECK(master["Parent"] == "");
	CHECK(master["VolumeDb"] == 0.0);
	CHECK(master["Muted"] == false);
	CHECK(master["Effects"] == Json::array());
	CHECK(master["DuckedBy"] == "");
	CHECK(master["DuckAmountDb"] == -12.0);
	CHECK(defaults["Snapshots"] == Json::array());
}

TEST_CASE("MixerAsset: files round trip, buses read from a file replace the default Master, and missing fields keep defaults")
{
	Mixer const mixer = MakeFullMixer();
	Ref<DataAsset> const asset = GetMixerType().CreateAsset();
	auto const typed = std::static_pointer_cast<TypedDataAsset<Mixer>>(asset);
	REQUIRE(typed->SetData(mixer).IsOk());
	Json const document = DataAssetSerializer::Serialize(*asset);
	CHECK(document["Strada"] == Json::parse(R"({ "Version": 1, "Type": "Mixer" })"));
	CHECK(document["Mixer"]["Buses"].size() == 4);
	CHECK(document["Mixer"]["Buses"][1]["Effects"][1]["Kind"] == "Reverb");
	CHECK(document["Mixer"]["Buses"][1]["DuckedBy"] == "Voices");
	CHECK(document["Mixer"]["Snapshots"][0]["Values"][1]["Target"] == "Muted");
	CHECK(document["Mixer"]["Snapshots"][0]["Values"][2]["Parameter"] == "Frequency");

	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / "Game.smixer";
	REQUIRE(DataAssetSerializer::SaveToFile(*asset, path).IsOk());
	Result<Ref<DataAsset>> loaded = DataAssetSerializer::LoadFromFile(GetMixerType(), path, {});
	REQUIRE_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
	CHECK(std::static_pointer_cast<TypedDataAsset<Mixer>>(loaded.GetValue())->GetData() == mixer);

	// The file's buses are the mixer's, not appended to the default Master; a bus's and an effect's missing fields take
	// their defaults.
	Result<Ref<DataAsset>> sparse = DataAssetSerializer::Deserialize(
		GetMixerType(),
		MakeMixerDocument(
			Json::parse(R"({ "Buses": [ { "Name": "Master" }, { "Name": "Music", "Effects": [ { "Kind": "HighPass" } ] } ] })")),
		{});
	REQUIRE_MESSAGE(sparse.IsOk(), (sparse ? std::string() : sparse.GetError()));
	Mixer const& read = std::static_pointer_cast<TypedDataAsset<Mixer>>(sparse.GetValue())->GetData();
	REQUIRE(read.Buses.size() == 2);
	CHECK(read.Buses[0] == MakeBus("Master", ""));
	CHECK(read.Buses[1].Name == "Music");
	CHECK(read.Buses[1].Parent.empty());
	REQUIRE(read.Buses[1].Effects.size() == 1);
	MixerEffect highPass;
	highPass.Kind = MixerEffectKind::HighPass;
	CHECK(read.Buses[1].Effects[0] == highPass);
	CHECK(read.Snapshots.empty());
	// A snapshot value's missing fields take theirs too: Master's volume at 0 dB.
	Result<Ref<DataAsset>> sparseSnapshot = DataAssetSerializer::Deserialize(
		GetMixerType(),
		MakeMixerDocument(Json::parse(
			R"({ "Buses": [ { "Name": "Master" } ], "Snapshots": [ { "Name": "Calm", "Values": [ { "Bus": "Master" } ] } ] })")),
		{});
	REQUIRE_MESSAGE(sparseSnapshot.IsOk(), (sparseSnapshot ? std::string() : sparseSnapshot.GetError()));
	Mixer const& readSnapshot = std::static_pointer_cast<TypedDataAsset<Mixer>>(sparseSnapshot.GetValue())->GetData();
	REQUIRE(readSnapshot.Snapshots.size() == 1);
	CHECK(readSnapshot.Snapshots[0].Values == std::vector<MixerSnapshotValue>{SetVolume("Master", 0.0f)});

	// Values outside their ranges are refused when read.
	CHECK(DataAssetSerializer::Deserialize(GetMixerType(),
	                                       MakeMixerDocument(Json::parse(R"({ "Buses": [ { "Name": "Master", "VolumeDb": 30 } ] })")), {})
	          .IsError());
	CHECK(DataAssetSerializer::Deserialize(
			  GetMixerType(),
			  MakeMixerDocument(Json::parse(R"({ "Buses": [ { "Name": "Master", "Effects": [ { "Kind": "LowPass", "Q": 0 } ] } ] })")), {})
	          .IsError());
	CHECK(DataAssetSerializer::Deserialize(
			  GetMixerType(),
			  MakeMixerDocument(Json::parse(R"({ "Buses": [ { "Name": "Master", "Effects": [ { "Kind": "Chorus" } ] } ] })")), {})
	          .IsError());
}

TEST_CASE("MixerAsset: validation refuses mixers without one root Master, with names or parents at fault, cycles and too much")
{
	CHECK(ValidateMixer(Mixer{}).IsOk());
	CHECK(ValidateMixer(MakeFullMixer()).IsOk());
	// Master is the bus named exactly so; another bus that names no parent feeds it.
	CHECK(ValidateMixer(MakeMixer({MakeBus("Music", ""), MakeBus("Master", "")})).IsOk());

	CHECK(ValidationError(MakeMixer({})).find("a mixer has a bus named Master") != std::string::npos);
	CHECK(ValidationError(MakeMixer({MakeBus("Music", "")})).find("a mixer has a bus named Master") != std::string::npos);
	CHECK(ValidationError(MakeMixer({MakeBus("master", "")})).find("a mixer has a bus named Master") != std::string::npos);
	CHECK(ValidationError(MakeMixer({MakeBus("Master", ""), MakeBus("Master", "")})) ==
	      "Buses[1].Name: 'Master' is the name of Buses[0] ('Master'): names are unique, ignoring letter case");
	CHECK(ValidationError(MakeMixer({MakeBus("Master", ""), MakeBus("Music", ""), MakeBus("MUSIC", "")})) ==
	      "Buses[2].Name: 'MUSIC' is the name of Buses[1] ('Music'): names are unique, ignoring letter case");
	CHECK(ValidationError(MakeMixer({MakeBus("Master", "Music"), MakeBus("Music", "")})) ==
	      "Buses[0].Parent: Master is the root and feeds no bus, not 'Music'");
	CHECK(ValidationError(MakeMixer({MakeBus("Master", ""), MakeBus("Music", "Musik")})) ==
	      "Buses[1].Parent: the parent of 'Music', 'Musik', is not a bus of the mixer");
	CHECK(ValidationError(MakeMixer({MakeBus("Master", ""), MakeBus("Music", "music")})) ==
	      "Buses[1].Parent: 'Music' feeds itself through its parents, which never reach Master");
	CHECK(ValidationError(MakeMixer({MakeBus("Master", ""), MakeBus("A", "B"), MakeBus("B", "C"), MakeBus("C", "A")})) ==
	      "Buses[1].Parent: 'A' feeds itself through its parents, which never reach Master");
	CHECK(ValidationError(MakeMixer({MakeBus("Master", ""), MakeBus("", "")})) == "Buses[1].Name: a bus has a name");
	CHECK(ValidationError(MakeMixer({MakeBus("Master", ""), MakeBus(std::string(65, 'a'), "")})) ==
	      "Buses[1].Name: a bus's name is at most 64 bytes, not 65");
	CHECK(ValidateMixer(MakeMixer({MakeBus("Master", ""), MakeBus(std::string(64, 'a'), "")})).IsOk());

	Mixer tooMany = MakeMixer({MakeBus("Master", "")});
	for (uint32_t index = 1; index < MaxMixerBuses; index++)
	{
		tooMany.Buses.push_back(MakeBus("Bus" + std::to_string(index), ""));
	}
	CHECK(ValidateMixer(tooMany).IsOk());
	tooMany.Buses.push_back(MakeBus("Bus32", ""));
	CHECK(ValidationError(tooMany) == "Buses: a mixer has at most 32 buses, not 33");

	Mixer crowded = MakeMixer({MakeBus("Master", "")});
	crowded.Buses[0].Effects.resize(MaxBusEffects);
	CHECK(ValidateMixer(crowded).IsOk());
	crowded.Buses[0].Effects.resize(MaxBusEffects + 1);
	CHECK(ValidationError(crowded) == "Buses[0].Effects: 'Master' has at most 8 effects, not 9");

	// The registry refuses what validation refuses, naming the type.
	Result<Ref<DataAsset>> const cycle = DataAssetSerializer::Deserialize(
		GetMixerType(),
		MakeMixerDocument(
			Json::parse(R"({ "Buses": [ { "Name": "Master" }, { "Name": "A", "Parent": "B" }, { "Name": "B", "Parent": "A" } ] })")),
		{});
	REQUIRE(cycle.IsError());
	CHECK(cycle.GetError().find("'A' feeds itself through its parents") != std::string::npos);
}

TEST_CASE("MixerAsset: validation refuses ducking by a bus the mixer lacks, by the bus itself, or by a level that depends on it")
{
	auto const ducked = [](std::string_view follower, std::string_view ducker)
	{
		Mixer mixer =
			MakeMixer({MakeBus("Master", ""), MakeBus("Music", "Master"), MakeBus("Voices", "Master"), MakeBus("Score", "Music")});
		for (MixerBus& bus : mixer.Buses)
		{
			if (bus.Name == follower)
			{
				bus.DuckedBy = std::string(ducker);
			}
		}
		return mixer;
	};
	// A sibling, a bus below it (whose level does not depend on the buses above it), and names in any letter case.
	CHECK(ValidateMixer(ducked("Music", "Voices")).IsOk());
	CHECK(ValidateMixer(ducked("Music", "voices")).IsOk());
	CHECK(ValidateMixer(ducked("Music", "Score")).IsOk());
	CHECK(ValidateMixer(ducked("Master", "Voices")).IsOk());

	CHECK(ValidationError(ducked("Music", "Dialogue")) ==
	      "Buses[1].DuckedBy: 'Music' is ducked by 'Dialogue', which is not a bus of the mixer");
	CHECK(ValidationError(ducked("Music", "MUSIC")) == "Buses[1].DuckedBy: 'Music' cannot be ducked by its own level");
	// Master's level, and Music's, hold what Score feeds them.
	CHECK(ValidationError(ducked("Score", "Master")) ==
	      "Buses[3].DuckedBy: 'Score' is ducked by 'Master', whose level depends on 'Score' itself");
	CHECK(ValidationError(ducked("Score", "Music")) ==
	      "Buses[3].DuckedBy: 'Score' is ducked by 'Music', whose level depends on 'Score' itself");
	// Two buses ducking each other, and a chain of ducking that comes back.
	Mixer pair = ducked("Music", "Voices");
	pair.Buses[2].DuckedBy = "Music";
	CHECK(ValidationError(pair).find("whose level depends on") != std::string::npos);
	Mixer chain = MakeMixer({MakeBus("Master", ""), MakeBus("A", ""), MakeBus("B", ""), MakeBus("C", "")});
	chain.Buses[1].DuckedBy = "B";
	chain.Buses[2].DuckedBy = "C";
	CHECK(ValidateMixer(chain).IsOk());
	chain.Buses[3].DuckedBy = "A";
	CHECK(ValidationError(chain) == "Buses[1].DuckedBy: 'A' is ducked by 'B', whose level depends on 'A' itself");
	// Ducked by a bus that a bus below it ducks is no loop: Score's level does not depend on Music's output.
	Mixer below = ducked("Music", "Voices");
	below.Buses[2].DuckedBy = "Score";
	CHECK(ValidateMixer(below).IsOk());

	// Ranges are the field table's.
	CHECK(DataAssetSerializer::Deserialize(
			  GetMixerType(), MakeMixerDocument(Json::parse(R"({ "Buses": [ { "Name": "Master", "DuckAmountDb": 3 } ] })")), {})
	          .IsError());
	CHECK(DataAssetSerializer::Deserialize(
			  GetMixerType(), MakeMixerDocument(Json::parse(R"({ "Buses": [ { "Name": "Master", "DuckAttackSeconds": 0 } ] })")), {})
	          .IsError());
}

TEST_CASE("MixerAsset: validation refuses snapshots with names, buses, effects, parameters or values at fault")
{
	auto const withSnapshot = [](std::vector<MixerSnapshotValue> values, std::string name = "Paused")
	{
		Mixer mixer = MakeFullMixer();
		mixer.Snapshots = {MixerSnapshot{.Name = std::move(name), .Values = std::move(values)}};
		return mixer;
	};
	CHECK(ValidateMixer(withSnapshot({SetVolume("music", -80.0f), SetVolume("Effects", 24.0f)})).IsOk());
	// Music's effects are a peaking filter, then a reverb.
	CHECK(ValidateMixer(withSnapshot({SetParameter("Music", 0, MixerEffectParameter::GainDb, -24.0f),
	                                  SetParameter("Music", 1, MixerEffectParameter::WetDb, 0.0f),
	                                  SetParameter("Music", 0, MixerEffectParameter::Q, 20.0f)}))
	          .IsOk());

	CHECK(ValidationError(withSnapshot({}, "")) == "Snapshots[0].Name: a snapshot has a name");
	CHECK(ValidationError(withSnapshot({}, std::string(65, 's'))) == "Snapshots[0].Name: a snapshot's name is at most 64 bytes, not 65");
	Mixer twice = withSnapshot({});
	twice.Snapshots.push_back(MixerSnapshot{.Name = "PAUSED", .Values = {}});
	CHECK(ValidationError(twice) ==
	      "Snapshots[1].Name: 'PAUSED' is the name of Snapshots[0] ('Paused'): names are unique, ignoring letter case");
	CHECK(ValidationError(withSnapshot({SetVolume("Musik", 0.0f)})) ==
	      "Snapshots[0].Values[0].Bus: 'Musik' of snapshot 'Paused' is not a bus of the mixer");
	CHECK(ValidationError(withSnapshot({SetVolume("Music", 25.0f)})) == "Snapshots[0].Values[0].Value: a volume is -80 to 24 dB, not 25");
	CHECK(ValidationError(withSnapshot({MixerSnapshotValue{.Bus = "Music",
	                                                       .Target = MixerSnapshotTarget::Muted,
	                                                       .Effect = 0,
	                                                       .Parameter = MixerEffectParameter::Frequency,
	                                                       .Value = 0.5f}})) ==
	      "Snapshots[0].Values[0].Value: a mute is 1 (muted) or 0, not 0.5");
	CHECK(ValidationError(withSnapshot({SetParameter("Music", 2, MixerEffectParameter::GainDb, 0.0f)})) ==
	      "Snapshots[0].Values[0].Effect: 'Music' has 2 effects, so no effect 2");
	CHECK(ValidationError(withSnapshot({SetParameter("Music", 1, MixerEffectParameter::Frequency, 500.0f)})) ==
	      "Snapshots[0].Values[0].Parameter: effect 1 of 'Music' is a Reverb, which has no Frequency");
	CHECK(ValidationError(withSnapshot({SetParameter("Music", 0, MixerEffectParameter::Frequency, 5.0f)})) ==
	      "Snapshots[0].Values[0].Value: Frequency is 10 to 24000, not 5");
	CHECK(ValidationError(withSnapshot({SetVolume("Music", -6.0f), SetVolume("MUSIC", -3.0f)})) ==
	      "Snapshots[0].Values[1]: snapshot 'Paused' already sets what this value sets on 'MUSIC'");
	CHECK(ValidationError(withSnapshot({SetParameter("Music", 0, MixerEffectParameter::GainDb, -3.0f),
	                                    SetParameter("Music", 0, MixerEffectParameter::GainDb, -6.0f)}))
	          .find("already sets what this value sets") != std::string::npos);
	// The effect and parameter of a volume do not tell two volumes apart.
	MixerSnapshotValue volume = SetVolume("Music", -3.0f);
	volume.Effect = 1;
	CHECK(ValidationError(withSnapshot({SetVolume("Music", -6.0f), volume})).find("already sets") != std::string::npos);

	// The counts are the field table's.
	Ref<DataAsset> const asset = GetMixerType().CreateAsset();
	REQUIRE(std::static_pointer_cast<TypedDataAsset<Mixer>>(asset)->SetData(MakeFullMixer()).IsOk());
	Json document = DataAssetSerializer::Serialize(*asset);
	Json& values = document["Mixer"]["Snapshots"][0]["Values"];
	while (values.size() <= MaxMixerSnapshotValues)
	{
		values.push_back(values[0]);
	}
	CHECK(DataAssetSerializer::Deserialize(GetMixerType(), document, {}).IsError());
}

TEST_CASE("MixerAsset: a snapshot is found by its name in any letter case, and puts its values in place of the mixer's own")
{
	Mixer const mixer = MakeFullMixer();
	CHECK(FindMixerSnapshot(mixer, "paused") == &mixer.Snapshots[0]);
	CHECK(FindMixerSnapshot(mixer, "QUIET") == &mixer.Snapshots[1]);
	CHECK(FindMixerSnapshot(mixer, "") == nullptr);
	CHECK(FindMixerSnapshot(mixer, "Loud") == nullptr);

	CHECK(ApplyMixerSnapshot(mixer, nullptr) == mixer);
	CHECK(ApplyMixerSnapshot(mixer, &mixer.Snapshots[1]) == mixer);
	Mixer expected = mixer;
	expected.Buses[2].VolumeDb = -12.0f;
	expected.Buses[3].Muted = true;
	expected.Buses[1].Effects[0].Frequency = 800.0f;
	CHECK(ApplyMixerSnapshot(mixer, &mixer.Snapshots[0]) == expected);

	MixerEffect effect;
	for (uint32_t index = 0; index < MixerEffectParameterCount; index++)
	{
		MixerEffectParameter const parameter = static_cast<MixerEffectParameter>(index);
		SetMixerEffectParameter(effect, parameter, 0.25f + static_cast<float>(index));
		CHECK(GetMixerEffectParameter(effect, parameter) == 0.25f + static_cast<float>(index));
	}
}

TEST_CASE("MixerAsset: an effect's parameters are read by name, and bus names are matched by their keys")
{
	MixerEffect effect;
	effect.CeilingDb = -3.0f;
	effect.ThresholdDb = -20.0f;
	CHECK(GetMixerEffectParameter(effect, MixerEffectParameter::Frequency) == effect.Frequency);
	CHECK(GetMixerEffectParameter(effect, MixerEffectParameter::ThresholdDb) == -20.0f);
	CHECK(GetMixerEffectParameter(effect, MixerEffectParameter::CeilingDb) == -3.0f);
	CHECK(MixerEffectParameterCount == 17);
	CHECK(MakeMixerBusKey("") == "master");
	CHECK(MakeMixerBusKey("Engine-2 ÜBER") == "engine-2 Über");
}

TEST_CASE("MixerAsset: damaged mixers are refused, and what loads mixes")
{
	// Every damaged copy of a full mixer that still loads is set on the engine and mixed for a block, with a sound on each
	// of its buses: whatever loads must be something the mix can run.
	Testing::AudioEngineScope engine;
	Result<Scope<AudioScene>> created = AudioScene::Create();
	REQUIRE(created.IsOk());
	Scope<AudioScene> scene = created.TakeValue();
	Ref<AudioClipAsset> const clip = Testing::MakeWavClip({{0.1f, 0.2f}});
	uint64_t entity = 1;
	for (char const* bus : {"", "Music", "Effects", "Voices", "Elsewhere"})
	{
		AudioSourceDesc desc;
		desc.Entity = UUID(entity++);
		desc.Clip = clip;
		desc.Loop = true;
		desc.Spatial = false;
		desc.Bus = bus;
		REQUIRE(scene->AddSource(desc).IsOk());
		scene->Play(desc.Entity);
	}
	Ref<DataAsset> const asset = GetMixerType().CreateAsset();
	REQUIRE(std::static_pointer_cast<TypedDataAsset<Mixer>>(asset)->SetData(MakeFullMixer()).IsOk());
	Json const original = DataAssetSerializer::Serialize(*asset);
	Testing::DocumentMutator mutator(0x6D6978657273ull);
	std::vector<float> block(static_cast<size_t>(AudioBlockFrames) * 2);
	int loadedCount = 0;
	int const rounds = Testing::GetFuzzRounds(200);
	for (int round = 0; round < rounds; round++)
	{
		Json document = original;
		mutator.Mutate(document);
		if (round % 2 == 0)
		{
			mutator.Mutate(document);
		}
		Result<Ref<DataAsset>> loaded = DataAssetSerializer::Deserialize(GetMixerType(), document, {});
		if (!loaded)
		{
			CHECK_FALSE(loaded.GetError().empty());
			continue;
		}
		loadedCount++;
		Mixer const& mixer = std::static_pointer_cast<TypedDataAsset<Mixer>>(loaded.GetValue())->GetData();
		CHECK(ValidateMixer(mixer).IsOk());
		AudioEngine::SetMixer(mixer);
		CHECK(AudioEngine::GetMixer() == mixer);
		// What loads saves and loads again unchanged.
		Result<Ref<DataAsset>> again =
			DataAssetSerializer::Deserialize(GetMixerType(), DataAssetSerializer::Serialize(*loaded.GetValue()), {});
		REQUIRE(again.IsOk());
		CHECK(std::static_pointer_cast<TypedDataAsset<Mixer>>(again.GetValue())->GetData() == mixer);
		REQUIRE(AudioEngine::ReadFrames(block) == AudioBlockFrames);
		// Every snapshot that loads can be blended to and mixed.
		for (MixerSnapshot const& snapshot : mixer.Snapshots)
		{
			CHECK(AudioEngine::TransitionTo(snapshot.Name, 0.01f));
			CHECK(AudioEngine::GetCurrentSnapshot() == snapshot.Name);
			REQUIRE(AudioEngine::ReadFrames(block) == AudioBlockFrames);
		}
		CHECK(AudioEngine::TransitionTo("", 0.0f));
	}
	CHECK(loadedCount > 0);
	scene.reset();
	AudioEngine::SetMixer(Mixer{});
	REQUIRE(AudioEngine::ReadFrames(block) == AudioBlockFrames);
	AudioEngine::Update();
}
