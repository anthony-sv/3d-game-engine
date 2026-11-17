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
		return mixer;
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
	REQUIRE(type.GetFields().size() == 1);
	CHECK(type.GetFields()[0].Name == "Buses");
	CHECK_FALSE(type.GetFields()[0].Hints.Description.empty());
	// A new mixer is Master alone, at 0 dB without effects.
	CHECK(type.GetDefaultFields() ==
	      Json::parse(R"({ "Buses": [ { "Name": "Master", "Parent": "", "VolumeDb": 0.0, "Muted": false, "Effects": [] } ] })"));
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
	}
	CHECK(loadedCount > 0);
	scene.reset();
	AudioEngine::SetMixer(Mixer{});
	REQUIRE(AudioEngine::ReadFrames(block) == AudioBlockFrames);
	AudioEngine::Update();
}
