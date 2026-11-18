#include "Audio/AudioTestUtilities.h"

#include "Strada/Asset/Mixer.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Audio/AudioTypes.h"
#include "Strada/Core/Log.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

// Reverb zones (Docs/Architecture.md §9.8): the send a zone gives a sound at a position, and the sends of an AudioScene's
// spatial voices measured in the mix. The zones send to buses without effects, so that a send reads as the ratio of the
// zone's bus to the voice's own: both carry the same samples, after the voice's volume, attenuation and panning, and a
// send multiplies them by its gain, so the ratio holds on every platform (no effect's output is compared).
namespace
{
	constexpr uint32_t BlockFrames = AudioBlockFrames;

	Scope<AudioScene> CreateAudioScene()
	{
		Result<Scope<AudioScene>> scene = AudioScene::Create();
		REQUIRE(scene.IsOk());
		return scene.TakeValue();
	}

	UUID SourceId(uint64_t index)
	{
		return UUID(5000 + index);
	}

	MixerBus MakeBus(std::string name)
	{
		MixerBus bus;
		bus.Name = std::move(name);
		bus.Parent = std::string(MasterBusName);
		return bus;
	}

	// Master, Dry (where the sources play) and the zones' buses, none with effects.
	void SetZoneMixer(std::vector<std::string> const& zoneBuses)
	{
		Mixer mixer;
		mixer.Buses.push_back(MakeBus("Dry"));
		for (std::string const& bus : zoneBuses)
		{
			mixer.Buses.push_back(MakeBus(bus));
		}
		REQUIRE(ValidateMixer(mixer).IsOk());
		AudioEngine::SetMixer(mixer);
	}

	// A looping spatial source on Dry, half a meter in front of the listener at the origin, within its minimum distance.
	void AddSpatialSource(AudioScene& audio, uint64_t index, Ref<AudioClipAsset> clip, bool spatial = true)
	{
		AudioSourceDesc desc;
		desc.Entity = SourceId(index);
		desc.Clip = std::move(clip);
		desc.Loop = true;
		desc.Spatial = spatial;
		desc.Position = {0.0f, 0.0f, -0.5f};
		desc.MinDistance = 1.0f;
		desc.Bus = "Dry";
		REQUIRE(audio.AddSource(desc).IsOk());
		audio.Play(desc.Entity);
	}

	AudioReverbZoneDesc MakeSphere(glm::vec3 const& position, float radius, float fade, std::string bus, float level)
	{
		AudioReverbZoneDesc zone;
		zone.Shape = ReverbZoneShape::Sphere;
		zone.Position = position;
		zone.Radius = radius;
		zone.FadeDistance = fade;
		zone.Bus = std::move(bus);
		zone.Level = level;
		return zone;
	}

	void ReadBlocks(uint32_t blocks)
	{
		std::vector<float> samples(static_cast<size_t>(blocks) * BlockFrames * 2);
		REQUIRE(AudioEngine::ReadFrames(samples) == static_cast<uint64_t>(blocks) * BlockFrames);
	}

	// The send to bus as the mix shows it over the last block: the bus's level over Dry's.
	double MeasureSend(std::string_view bus)
	{
		float const dry = AudioEngine::GetBusRms("Dry");
		REQUIRE(dry > 0.0f);
		return static_cast<double>(AudioEngine::GetBusRms(bus)) / static_cast<double>(dry);
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

	// Blocks a gain takes to reach its target, and then one more (AudioGainSmoothingSeconds at the test rate).
	constexpr uint32_t RampBlocks =
		static_cast<uint32_t>(AudioGainSmoothingSeconds * static_cast<float>(Testing::AudioSampleRate)) / BlockFrames + 2;

	// A voice that sends to Tunnel alone (one slot heard, the other silent) moves to the two buses of zones, both new to it:
	// the send ranked first takes the silent slot and is heard from the next block, the other waits for Tunnel's to fade
	// out. Which bus is heard first depends only on the slot each send takes, so it holds on every platform.
	void CheckFirstSendHeard(std::vector<AudioReverbZoneDesc> const& zones, std::string_view first, float firstLevel,
	                         std::string_view second, float secondLevel)
	{
		Testing::AudioEngineScope engine;
		SetZoneMixer({"Tunnel", "Alpha", "Bravo", "Zeta"});
		Scope<AudioScene> audio = CreateAudioScene();
		AddSpatialSource(*audio, 0, Testing::MakeWavClip({{0.25f, 1.0f}}));
		std::vector<AudioReverbZoneDesc> const tunnel = {MakeSphere({0.0f, 0.0f, -0.5f}, 1.0f, 0.0f, "Tunnel", 0.4f)};
		audio->UpdateReverbZones(tunnel);
		ReadBlocks(RampBlocks);
		REQUIRE(MeasureSend("Tunnel") == doctest::Approx(0.4).epsilon(1e-5));

		audio->UpdateReverbZones(zones);
		ReadBlocks(1);
		CHECK(MeasureSend(first) > 0.0);
		CHECK(AudioEngine::GetBusPeak(second) == 0.0f);
		CHECK(MeasureSend("Tunnel") > 0.0);
		ReadBlocks(2 * RampBlocks);
		CHECK(MeasureSend(first) == doctest::Approx(firstLevel).epsilon(1e-5));
		CHECK(MeasureSend(second) == doctest::Approx(secondLevel).epsilon(1e-5));
		CHECK(AudioEngine::GetBusPeak("Tunnel") == 0.0f);
	}
}

TEST_CASE("AudioScene: a reverb zone sends its level inside its shape, less across its fade and nothing beyond")
{
	// A sphere of radius 2 with a fade of 4 at (1, 0, 0).
	AudioReverbZoneDesc sphere = MakeSphere({1.0f, 0.0f, 0.0f}, 2.0f, 4.0f, "Wet", 0.8f);
	CHECK(ComputeReverbZoneSend(sphere, {1.0f, 0.0f, 0.0f}) == 0.8f);
	CHECK(ComputeReverbZoneSend(sphere, {2.5f, 0.0f, 0.0f}) == 0.8f);
	CHECK(ComputeReverbZoneSend(sphere, {1.0f, 2.0f, 0.0f}) == 0.8f);
	CHECK(ComputeReverbZoneSend(sphere, {1.0f, 0.0f, 3.0f}) == doctest::Approx(0.8f * 0.75f).epsilon(1e-6));
	CHECK(ComputeReverbZoneSend(sphere, {1.0f, -4.0f, 0.0f}) == doctest::Approx(0.8f * 0.5f).epsilon(1e-6));
	CHECK(ComputeReverbZoneSend(sphere, {7.0f, 0.0f, 0.0f}) == 0.0f);
	CHECK(ComputeReverbZoneSend(sphere, {100.0f, 0.0f, 0.0f}) == 0.0f);

	// Without a fade the send stops at the surface.
	sphere.FadeDistance = 0.0f;
	CHECK(ComputeReverbZoneSend(sphere, {3.0f, 0.0f, 0.0f}) == 0.8f);
	CHECK(ComputeReverbZoneSend(sphere, {3.01f, 0.0f, 0.0f}) == 0.0f);

	// A box of half extents (1, 2, 3) turned 90 degrees about Y: its local X is world -Z and its local Z world X.
	AudioReverbZoneDesc box;
	box.Shape = ReverbZoneShape::Box;
	box.Position = {0.0f, 10.0f, 0.0f};
	box.Rotation = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f)));
	box.HalfExtents = {1.0f, 2.0f, 3.0f};
	box.FadeDistance = 2.0f;
	box.Bus = "Wet";
	box.Level = 0.5f;
	CHECK(ComputeReverbZoneSend(box, {2.9f, 10.0f, 0.0f}) == 0.5f);
	CHECK(ComputeReverbZoneSend(box, {0.0f, 10.0f, 0.9f}) == 0.5f);
	// Half the fade beyond the face at world z = 1 (local x = -1); beyond the edge at world x = 3, y = 12, the distance
	// to the edge: 0.5, 1 and 2 meters.
	CHECK(ComputeReverbZoneSend(box, {0.0f, 10.0f, 2.0f}) == doctest::Approx(0.25f).epsilon(1e-5));
	CHECK(ComputeReverbZoneSend(box, {3.3f, 12.4f, 0.0f}) == doctest::Approx(0.375f).epsilon(1e-5));
	CHECK(ComputeReverbZoneSend(box, {3.6f, 12.8f, 0.0f}) == doctest::Approx(0.25f).epsilon(1e-5));
	CHECK(ComputeReverbZoneSend(box, {4.2f, 13.6f, 0.0f}) == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(ComputeReverbZoneSend(box, {0.0f, 10.0f, 3.5f}) == 0.0f);

	// Nothing for a zone or a position that is not a number, or a level of 0.
	float const nan = std::numeric_limits<float>::quiet_NaN();
	CHECK(ComputeReverbZoneSend(box, {nan, 10.0f, 0.0f}) == 0.0f);
	box.Level = 0.0f;
	CHECK(ComputeReverbZoneSend(box, {0.0f, 10.0f, 0.0f}) == 0.0f);
	box.Level = 0.5f;
	box.Position.x = std::numeric_limits<float>::infinity();
	CHECK(ComputeReverbZoneSend(box, {0.0f, 10.0f, 0.0f}) == 0.0f);
}

TEST_CASE("AudioScene: a spatial voice inside a reverb zone sends its level to the zone's bus, across its fade and nothing outside")
{
	Testing::AudioEngineScope engine;
	SetZoneMixer({"Wet"});
	Scope<AudioScene> audio = CreateAudioScene();
	AddSpatialSource(*audio, 0, Testing::MakeWavClip({{0.25f, 1.0f}}));

	// Inside: half of the voice reaches Wet, from the block after the update on.
	std::vector<AudioReverbZoneDesc> zones = {MakeSphere({0.0f, 0.0f, -0.5f}, 1.0f, 2.0f, "Wet", 0.5f)};
	audio->UpdateReverbZones(zones);
	CHECK(audio->GetReverbSend(SourceId(0), "wet") == 0.5f);
	ReadBlocks(RampBlocks);
	CHECK(MeasureSend("Wet") == doctest::Approx(0.5).epsilon(1e-5));
	CHECK(AudioEngine::GetBusRms("Dry") == doctest::Approx(0.25f * 0.5f).epsilon(1e-3));

	// The voice's volume and attenuation come before the send: the ratio stays.
	audio->SetVolume(SourceId(0), 0.5f);
	audio->SetPosition(SourceId(0), {0.0f, 0.0f, -1.5f});
	zones[0].Position = {0.0f, 0.0f, -1.5f};
	audio->UpdateReverbZones(zones);
	ReadBlocks(RampBlocks);
	CHECK(MeasureSend("Wet") == doctest::Approx(0.5).epsilon(1e-5));

	// Halfway across the fade: a quarter.
	zones[0].Position = {0.0f, 0.0f, 0.5f};
	audio->UpdateReverbZones(zones);
	CHECK(audio->GetReverbSend(SourceId(0), "Wet") == doctest::Approx(0.25f).epsilon(1e-6));
	ReadBlocks(RampBlocks);
	CHECK(MeasureSend("Wet") == doctest::Approx(0.25).epsilon(1e-5));

	// Outside: nothing, after the send has faded out.
	zones[0].Position = {0.0f, 0.0f, 10.0f};
	audio->UpdateReverbZones(zones);
	CHECK(audio->GetReverbSend(SourceId(0), "Wet") == 0.0f);
	ReadBlocks(RampBlocks);
	CHECK(AudioEngine::GetBusPeak("Wet") == 0.0f);
	CHECK(AudioEngine::GetBusRms("Dry") > 0.0f);

	// The send follows the sound, not the listener: a listener far outside hears the voice and its send.
	zones[0].Position = {0.0f, 0.0f, -1.5f};
	audio->SetListener({0.0f, 0.0f, 50.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
	audio->UpdateReverbZones(zones);
	ReadBlocks(RampBlocks);
	CHECK(MeasureSend("Wet") == doctest::Approx(0.5).epsilon(1e-5));
}

TEST_CASE("AudioScene: of a bus's zones the largest send counts, and a voice sends to the two buses it sends most to")
{
	Testing::AudioEngineScope engine;
	SetZoneMixer({"Hall", "Tunnel", "Cave"});
	Scope<AudioScene> audio = CreateAudioScene();
	AddSpatialSource(*audio, 0, Testing::MakeWavClip({{0.25f, 1.0f}}));
	glm::vec3 const here = {0.0f, 0.0f, -0.5f};

	std::vector<AudioReverbZoneDesc> zones = {MakeSphere(here, 1.0f, 0.0f, "Hall", 0.3f), MakeSphere(here, 1.0f, 0.0f, "HALL", 0.6f),
	                                          MakeSphere(here, 1.0f, 0.0f, "Tunnel", 0.4f), MakeSphere(here, 1.0f, 0.0f, "Cave", 0.2f)};
	audio->UpdateReverbZones(zones);
	ReadBlocks(RampBlocks);
	CHECK(MeasureSend("Hall") == doctest::Approx(0.6).epsilon(1e-5));
	CHECK(MeasureSend("Tunnel") == doctest::Approx(0.4).epsilon(1e-5));
	CHECK(AudioEngine::GetBusPeak("Cave") == 0.0f);

	// Cave rises above Tunnel: Tunnel's send, heard, fades out, then plays into Cave and fades in; Hall stays where it is.
	zones[3].Level = 0.7f;
	audio->UpdateReverbZones(zones);
	CHECK(audio->GetReverbSend(SourceId(0), "Cave") == 0.7f);
	CHECK(audio->GetReverbSend(SourceId(0), "Tunnel") == 0.0f);
	ReadBlocks(1);
	double const fading = MeasureSend("Tunnel");
	CHECK(fading > 0.0);
	CHECK(fading < 0.4);
	CHECK(AudioEngine::GetBusPeak("Cave") == 0.0f);
	ReadBlocks(2 * RampBlocks);
	CHECK(MeasureSend("Hall") == doctest::Approx(0.6).epsilon(1e-5));
	CHECK(MeasureSend("Cave") == doctest::Approx(0.7).epsilon(1e-5));
	CHECK(AudioEngine::GetBusPeak("Tunnel") == 0.0f);

	// The order of the zones changes nothing.
	std::reverse(zones.begin(), zones.end());
	audio->UpdateReverbZones(zones);
	ReadBlocks(RampBlocks);
	CHECK(MeasureSend("Hall") == doctest::Approx(0.6).epsilon(1e-5));
	CHECK(MeasureSend("Cave") == doctest::Approx(0.7).epsilon(1e-5));
}

TEST_CASE("AudioScene: a voice's sends rank the larger first and, of equal ones, the bus whose key sorts first, in any zone order")
{
	glm::vec3 const here = {0.0f, 0.0f, -0.5f};
	AudioReverbZoneDesc const alpha = MakeSphere(here, 1.0f, 0.0f, "Alpha", 0.5f);
	AudioReverbZoneDesc const bravo = MakeSphere(here, 1.0f, 0.0f, "Bravo", 0.5f);
	AudioReverbZoneDesc const zeta = MakeSphere(here, 1.0f, 0.0f, "Zeta", 0.9f);

	// The larger send ranks first, although its bus's key sorts last.
	SUBCASE("the larger first, its zone last")
	{
		CheckFirstSendHeard({alpha, zeta}, "Zeta", 0.9f, "Alpha", 0.5f);
	}
	SUBCASE("the larger first, its zone first")
	{
		CheckFirstSendHeard({zeta, alpha}, "Zeta", 0.9f, "Alpha", 0.5f);
	}
	SUBCASE("of equal sends, the bus whose key sorts first, its zone last")
	{
		CheckFirstSendHeard({bravo, alpha}, "Alpha", 0.5f, "Bravo", 0.5f);
	}
	SUBCASE("of equal sends, the bus whose key sorts first, its zone first")
	{
		CheckFirstSendHeard({alpha, bravo}, "Alpha", 0.5f, "Bravo", 0.5f);
	}
	// Three buses for two slots, the two smaller sends equal: the last slot goes to the bus whose key sorts first.
	SUBCASE("the last slot to the bus whose key sorts first")
	{
		std::vector<std::vector<AudioReverbZoneDesc>> const orders = {{zeta, bravo, alpha}, {alpha, bravo, zeta}, {bravo, alpha, zeta}};
		for (std::vector<AudioReverbZoneDesc> const& zones : orders)
		{
			Testing::AudioEngineScope engine;
			SetZoneMixer({"Alpha", "Bravo", "Zeta"});
			Scope<AudioScene> audio = CreateAudioScene();
			AddSpatialSource(*audio, 0, Testing::MakeWavClip({{0.25f, 1.0f}}));
			audio->UpdateReverbZones(zones);
			CHECK(audio->GetReverbSend(SourceId(0), "Zeta") == 0.9f);
			CHECK(audio->GetReverbSend(SourceId(0), "Alpha") == 0.5f);
			CHECK(audio->GetReverbSend(SourceId(0), "Bravo") == 0.0f);
			ReadBlocks(RampBlocks);
			CHECK(MeasureSend("Zeta") == doctest::Approx(0.9).epsilon(1e-5));
			CHECK(MeasureSend("Alpha") == doctest::Approx(0.5).epsilon(1e-5));
			CHECK(AudioEngine::GetBusPeak("Bravo") == 0.0f);
		}
	}
}

TEST_CASE("AudioScene: a voice that is not spatial, and a zone without a bus or with one the mixer lacks, send nothing")
{
	Testing::AudioEngineScope engine;
	SetZoneMixer({"Wet"});
	Scope<AudioScene> audio = CreateAudioScene();
	AddSpatialSource(*audio, 0, Testing::MakeWavClip({{0.25f, 1.0f}}), false);
	glm::vec3 const here = {0.0f, 0.0f, -0.5f};

	uint64_t const logStart = Log::GetNextEntryIndex();
	std::vector<AudioReverbZoneDesc> const zones = {MakeSphere(here, 1.0f, 0.0f, "Wet", 1.0f)};
	audio->UpdateReverbZones(zones);
	ReadBlocks(RampBlocks);
	CHECK(audio->GetReverbSend(SourceId(0), "Wet") == 0.0f);
	CHECK(AudioEngine::GetBusRms("Dry") > 0.0f);
	CHECK(AudioEngine::GetBusPeak("Wet") == 0.0f);

	// Made spatial it sends; made not spatial again it stops.
	audio->SetSpatial(SourceId(0), true);
	audio->UpdateReverbZones(zones);
	ReadBlocks(RampBlocks);
	CHECK(MeasureSend("Wet") == doctest::Approx(1.0).epsilon(1e-5));
	audio->SetSpatial(SourceId(0), false);
	CHECK(audio->GetReverbSend(SourceId(0), "Wet") == 0.0f);
	ReadBlocks(RampBlocks);
	CHECK(AudioEngine::GetBusPeak("Wet") == 0.0f);

	// A spatial voice and zones that name no bus, or one the mixer lacks: nothing reaches any bus but Dry and Master, and
	// the missing bus is logged once.
	audio->SetSpatial(SourceId(0), true);
	std::vector<AudioReverbZoneDesc> const nowhere = {MakeSphere(here, 1.0f, 0.0f, "", 1.0f),
	                                                  MakeSphere(here, 1.0f, 0.0f, "Nowhere", 1.0f)};
	for (int update = 0; update < 3; update++)
	{
		audio->UpdateReverbZones(nowhere);
		ReadBlocks(RampBlocks);
	}
	CHECK(audio->GetReverbSend(SourceId(0), "Nowhere") == 0.0f);
	CHECK(AudioEngine::GetBusPeak("Wet") == 0.0f);
	CHECK(AudioEngine::GetBusRms("Master") == doctest::Approx(AudioEngine::GetBusRms("Dry")).epsilon(1e-6));
	CHECK(CountLogged(logStart, "A reverb zone sends to bus 'Nowhere', which the project's mixer lacks: it sends nothing") == 1);
}

TEST_CASE("AudioScene: one-shots and the scene's voices send from their first block, and a paused scene's sends resume in step")
{
	Testing::AudioEngineScope engine;
	SetZoneMixer({"Wet"});
	Scope<AudioScene> audio = CreateAudioScene();
	glm::vec3 const here = {0.0f, 0.0f, -0.5f};
	std::vector<AudioReverbZoneDesc> const zones = {MakeSphere(here, 1.0f, 0.0f, "Wet", 0.5f)};
	audio->UpdateReverbZones(zones);

	// A voice of the scene starts inside the zone: its send is whole in the block it starts in.
	Ref<AudioClipAsset> const impact = Testing::MakeWavClip({{0.2f, 0.5f}});
	REQUIRE(audio->PlayClipAtPoint(impact, here, 1.0f, "Dry"));
	ReadBlocks(1);
	CHECK(MeasureSend("Wet") == doctest::Approx(0.5).epsilon(1e-5));

	// So is a one-shot of a source in the zone.
	AudioSourceDesc pool;
	pool.Entity = SourceId(1);
	pool.Position = here;
	pool.Bus = "Dry";
	REQUIRE(audio->AddVoicePool(pool).IsOk());
	audio->UpdateReverbZones(zones);
	REQUIRE(audio->PlayOneShot(SourceId(1), impact, 1.0f));
	ReadBlocks(1);
	CHECK(MeasureSend("Wet") == doctest::Approx(0.5).epsilon(1e-5));

	// A source whose level steps up while the scene is paused and resumed: in every block after it resumes the send is
	// the same part of the voice, so no output of the send node lags behind the others.
	Scope<AudioScene> stepped = CreateAudioScene();
	audio.reset();
	ReadBlocks(RampBlocks);
	AddSpatialSource(*stepped, 0, Testing::MakeWavClip({{0.1f, 0.05f}, {0.4f, 0.05f}}));
	stepped->UpdateReverbZones(zones);
	ReadBlocks(3);
	CHECK(MeasureSend("Wet") == doctest::Approx(0.5).epsilon(1e-5));
	stepped->SetPaused(true);
	ReadBlocks(2);
	CHECK(AudioEngine::GetBusPeak("Dry") == 0.0f);
	CHECK(AudioEngine::GetBusPeak("Wet") == 0.0f);
	stepped->SetPaused(false);
	for (uint32_t block = 0; block < 40; block++)
	{
		ReadBlocks(1);
		CAPTURE(block);
		CHECK(MeasureSend("Wet") == doctest::Approx(0.5).epsilon(1e-5));
	}
}

TEST_CASE("AudioScene: a scene that ends with its voices sending gives back its voices, their sends and its groups")
{
	Testing::AudioEngineScope engine;
	SetZoneMixer({"Hall", "Tunnel"});
	Scope<AudioScene> audio = CreateAudioScene();
	Ref<AudioClipAsset> const clip = Testing::MakeWavClip({{0.25f, 1.0f}});
	for (uint64_t index = 0; index < 4; index++)
	{
		AddSpatialSource(*audio, index, clip);
		audio->PlayOneShot(SourceId(index), clip, 0.5f);
	}
	glm::vec3 const here = {0.0f, 0.0f, -0.5f};
	std::vector<AudioReverbZoneDesc> const zones = {MakeSphere(here, 1.0f, 0.0f, "Hall", 0.5f),
	                                                MakeSphere(here, 1.0f, 0.0f, "Tunnel", 0.3f)};
	audio->UpdateReverbZones(zones);
	ReadBlocks(RampBlocks);
	CHECK(MeasureSend("Hall") == doctest::Approx(0.5).epsilon(1e-5));
	CHECK(MeasureSend("Tunnel") == doctest::Approx(0.3).epsilon(1e-5));

	// The mixer loses Tunnel while the scene sends to it: its group moves to Master, and the zone sends nothing more.
	SetZoneMixer({"Hall"});
	audio->UpdateReverbZones(zones);
	ReadBlocks(2 * RampBlocks);
	CHECK(MeasureSend("Hall") == doctest::Approx(0.5).epsilon(1e-5));
	CHECK(AudioEngine::GetBusRms("Master") == doctest::Approx(AudioEngine::GetBusRms("Dry") * 1.5f).epsilon(1e-5));

	audio.reset();
	for (int read = 0; read < 10 && AudioEngine::GetReturningObjectCount() > 0; read++)
	{
		ReadBlocks(RampBlocks);
		AudioEngine::Update();
	}
	CHECK(AudioEngine::GetReturningObjectCount() == 0);
	CHECK(AudioEngine::GetBusPeak("Master") == 0.0f);
}
