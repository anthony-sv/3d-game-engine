#include "Audio/AudioTestUtilities.h"

#include "Editor/Automation/AudioCommands.h"
#include "Editor/Automation/CommandRegistry.h"

#include "Strada/Asset/Mixer.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioScene.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	struct AudioCommandFixture
	{
		CommandRegistry Registry;

		AudioCommandFixture() { REQUIRE(RegisterAudioCommands(Registry).IsOk()); }

		Json Meters()
		{
			std::optional<CommandResult> result;
			Registry.Execute("audio.meters", Json::object(),
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			REQUIRE(result.has_value());
			REQUIRE_MESSAGE(result->IsOk(), (result->IsError() ? result->GetError().Message : std::string()));
			return result->TakeValue();
		}
	};

	Json FindBus(Json const& meters, std::string_view name)
	{
		for (Json const& bus : meters["buses"])
		{
			if (bus["name"].get<std::string>() == name)
			{
				return bus;
			}
		}
		std::string const missing(name);
		FAIL("no bus named " << missing);
		return Json();
	}
}

TEST_CASE("AudioCommands: audio.meters is read-only and answers without an audio engine")
{
	AudioCommandFixture fixture;
	CommandDefinition const* const definition = fixture.Registry.Find("audio.meters");
	REQUIRE(definition != nullptr);
	CHECK(definition->ReadOnly);
	REQUIRE_FALSE(AudioEngine::IsInitialized());
	Json const meters = fixture.Meters();
	CHECK(meters["initialized"] == false);
	CHECK(meters["buses"] == Json::array());
	CHECK(meters["snapshot"] == "");
	CHECK(meters["activeVoices"] == 0);
	CHECK(meters["mixedVoices"] == 0);
	CHECK(meters["stolenVoices"] == 0);
	// Parameters it does not know are refused by its schema.
	std::optional<CommandResult> refused;
	fixture.Registry.Execute("audio.meters", Json::object({{"bus", "Music"}}),
	                         [&refused](CommandResult value)
	                         {
								 refused.emplace(std::move(value));
							 });
	REQUIRE(refused.has_value());
	CHECK(refused->IsError());
}

TEST_CASE("AudioCommands: audio.meters reads each bus's level over the last block, the snapshot and the voice counts")
{
	Testing::AudioEngineScope engine;
	AudioCommandFixture fixture;
	Mixer mixer;
	MixerBus music;
	music.Name = "Music";
	music.VolumeDb = -6.0f;
	mixer.Buses.push_back(music);
	mixer.Snapshots.push_back(MixerSnapshot{
		.Name = "Paused",
		.Values = {MixerSnapshotValue{.Bus = "Music",
	                                  .Target = MixerSnapshotTarget::Volume,
	                                  .Effect = 0,
	                                  .Parameter = MixerEffectParameter::Frequency,
	                                  .Value = -20.0f}},
	});
	AudioEngine::SetMixer(mixer);

	// Before anything plays the buses read silence, -80 dB rather than an infinity.
	Json silent = fixture.Meters();
	CHECK(silent["initialized"] == true);
	REQUIRE(silent["buses"].size() == 2);
	CHECK(silent["buses"][0]["name"] == "Master");
	CHECK(silent["buses"][1]["name"] == "Music");
	CHECK(silent["buses"][1]["peak"] == 0.0);
	CHECK(silent["buses"][1]["rmsDb"] == static_cast<double>(MinMixerVolumeDb));

	Result<Scope<AudioScene>> created = AudioScene::Create();
	REQUIRE(created.IsOk());
	Scope<AudioScene> audio = created.TakeValue();
	AudioSourceDesc desc;
	desc.Entity = UUID(77);
	desc.Clip = Testing::MakeWavClip({{0.5f, 1.0f}});
	desc.Spatial = false;
	desc.Loop = true;
	desc.Bus = "music";
	REQUIRE(audio->AddSource(desc).IsOk());
	audio->Play(desc.Entity);
	Testing::Skip(0.05f);

	float const level = 0.5f * std::pow(10.0f, -6.0f / 20.0f);
	Json const playing = fixture.Meters();
	Json const bus = FindBus(playing, "Music");
	CHECK(bus["peak"].get<double>() == doctest::Approx(level).epsilon(1e-4));
	CHECK(bus["rms"].get<double>() == doctest::Approx(level).epsilon(1e-4));
	CHECK(bus["peakDb"].get<double>() == doctest::Approx(-12.0206).epsilon(1e-4));
	CHECK(FindBus(playing, "Master")["rms"].get<double>() == doctest::Approx(level).epsilon(1e-4));
	CHECK(playing["snapshot"] == "");
	CHECK(playing["activeVoices"] == 1);
	CHECK(playing["mixedVoices"] == 1);
	CHECK(playing["stolenVoices"] == 0);

	REQUIRE(AudioEngine::TransitionTo("paused", 0.0f));
	Testing::Skip(0.05f);
	Json const paused = fixture.Meters();
	CHECK(paused["snapshot"] == "Paused");
	CHECK(FindBus(paused, "Music")["rmsDb"].get<double>() == doctest::Approx(-26.0206).epsilon(1e-3));
}

TEST_CASE("AudioCommands: a meter's decibels floor at the mixer's silence and are never infinite")
{
	CHECK(ToMeterDecibels(1.0f) == 0.0f);
	CHECK(ToMeterDecibels(0.5f) == doctest::Approx(-6.0206f).epsilon(1e-4));
	CHECK(ToMeterDecibels(0.0f) == MinMixerVolumeDb);
	CHECK(ToMeterDecibels(1.0e-9f) == MinMixerVolumeDb);
	CHECK(ToMeterDecibels(std::numeric_limits<float>::quiet_NaN()) == MinMixerVolumeDb);
	CHECK(ToMeterDecibels(-1.0f) == MinMixerVolumeDb);
}
