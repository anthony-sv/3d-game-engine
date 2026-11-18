#include "Audio/AudioTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/Mixer.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Audio/AudioTypes.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <utility>

using namespace Strada;

// ReverbZone (Docs/Architecture.md §9.8) as a running scene applies it: each zone placed, turned and sized by its entity's
// world transform, and the scene's spatial sounds sending to the zones' buses.
namespace
{
	constexpr float FrameTime = 1.0f / 60.0f;

	MixerBus MakeBus(std::string name)
	{
		MixerBus bus;
		bus.Name = std::move(name);
		bus.Parent = std::string(MasterBusName);
		return bus;
	}

	struct ZoneScene
	{
		Testing::AudioEngineScope Engine;
		Testing::AssetManagerScope Assets;
		Scene World{"Zones"};
		Entity Speaker;
		Entity Zone;

		ZoneScene()
		{
			Mixer mixer;
			mixer.Buses.push_back(MakeBus("Dry"));
			mixer.Buses.push_back(MakeBus("Wet"));
			AudioEngine::SetMixer(mixer);

			World.CreateEntity("Listener").AddComponent<AudioListenerComponent>();
			Speaker = World.CreateEntity("Speaker");
			Speaker.GetComponent<TransformComponent>().Translation = {0.0f, 0.0f, -0.5f};
			AudioSourceComponent& source = Speaker.AddComponent<AudioSourceComponent>();
			source.Clip = AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.25f, 1.0f}}), "Hum");
			source.Loop = true;
			source.PlayOnStart = true;
			source.Bus = "Dry";

			// A box of 1 m half extents 2.5 m beyond the speaker: the speaker is outside it, and outside its fade of 0.
			Zone = World.CreateEntity("Underpass");
			Zone.GetComponent<TransformComponent>().Translation = {0.0f, 0.0f, -3.0f};
			ReverbZoneComponent& zone = Zone.AddComponent<ReverbZoneComponent>();
			zone.Shape = ReverbZoneShape::Box;
			zone.HalfExtents = glm::vec3(1.0f);
			zone.FadeDistance = 0.0f;
			zone.Bus = "wet";
			zone.SendLevel = 0.5f;
		}

		AudioScene& GetAudio()
		{
			AudioScene* const audio = World.GetAudioScene();
			REQUIRE(audio != nullptr);
			return *audio;
		}

		// The send after a frame, as the scene computes it and as the mix shows it.
		float Update()
		{
			World.OnUpdateRuntime(Timestep(FrameTime));
			Testing::Skip(0.02f);
			float const send = GetAudio().GetReverbSend(Speaker.GetUUID(), "Wet");
			float const dry = AudioEngine::GetBusRms("Dry");
			REQUIRE(dry > 0.0f);
			CHECK(AudioEngine::GetBusRms("Wet") / dry == doctest::Approx(send).epsilon(1e-4));
			return send;
		}
	};
}

TEST_CASE("Scene: a ReverbZone sends the spatial sounds inside it to its bus, placed, turned and sized by its entity")
{
	ZoneScene scene;
	scene.World.OnRuntimeStart();
	CHECK(scene.Update() == 0.0f);

	TransformComponent& transform = scene.Zone.GetComponent<TransformComponent>();
	ReverbZoneComponent& zone = scene.Zone.GetComponent<ReverbZoneComponent>();
	SUBCASE("sized by its entity's scale")
	{
		// Three times as deep, the box reaches 3 m towards the speaker.
		transform.Scale = {1.0f, 1.0f, 3.0f};
		CHECK(scene.Update() == 0.5f);
		transform.Scale = {3.0f, 3.0f, 1.0f};
		CHECK(scene.Update() == 0.0f);
	}
	SUBCASE("turned by its entity's rotation")
	{
		zone.HalfExtents = {3.0f, 1.0f, 1.0f};
		CHECK(scene.Update() == 0.0f);
		transform.Rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
		CHECK(scene.Update() == 0.5f);
	}
	SUBCASE("a sphere sized by its entity's largest scale")
	{
		zone.Shape = ReverbZoneShape::Sphere;
		zone.Radius = 1.0f;
		CHECK(scene.Update() == 0.0f);
		transform.Scale = {3.0f, 1.0f, 1.0f};
		CHECK(scene.Update() == 0.5f);
	}
	SUBCASE("across its fade, and placed by its parent")
	{
		// 1.5 m outside the box, halfway across a fade of 3 m.
		zone.FadeDistance = 3.0f;
		CHECK(scene.Update() == doctest::Approx(0.25f).epsilon(1e-5));
		Entity parent = scene.World.CreateEntity("Bridge");
		REQUIRE(scene.World.SetParent(scene.Zone, parent, false).IsOk());
		parent.GetComponent<TransformComponent>().Translation = {0.0f, 0.0f, 2.5f};
		CHECK(scene.Update() == 0.5f);
		parent.GetComponent<TransformComponent>().Translation = {0.0f, 0.0f, -10.0f};
		CHECK(scene.Update() == 0.0f);
	}
	SUBCASE("its level and bus as they change, until it goes")
	{
		transform.Translation = {0.0f, 0.0f, -0.5f};
		CHECK(scene.Update() == 0.5f);
		// Scripts write fields without their ranges: the zone takes them within.
		zone.SendLevel = 3.0f;
		CHECK(scene.Update() == 1.0f);
		zone.SendLevel = 0.2f;
		CHECK(scene.Update() == 0.2f);
		zone.Bus = "Dry";
		CHECK(scene.Update() == 0.0f);
		CHECK(scene.GetAudio().GetReverbSend(scene.Speaker.GetUUID(), "Dry") == 0.2f);
		zone.Bus = "Wet";
		CHECK(scene.Update() == 0.2f);
		scene.Speaker.GetComponent<AudioSourceComponent>().Spatial = false;
		CHECK(scene.Update() == 0.0f);
		scene.Speaker.GetComponent<AudioSourceComponent>().Spatial = true;
		CHECK(scene.Update() == 0.2f);
		scene.Zone.RemoveComponent<ReverbZoneComponent>();
		CHECK(scene.Update() == 0.0f);
	}
	scene.World.OnRuntimeStop();
}
