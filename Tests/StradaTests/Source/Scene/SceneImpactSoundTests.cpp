#include "Audio/AudioTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/Mixer.h"
#include "Strada/Asset/PhysicsMaterial.h"
#include "Strada/Asset/PhysicsMaterialAsset.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

using namespace Strada;

// Impact sounds (Mixer::Impacts, Docs/Architecture.md §9.8): balls dropped on a ground, each side's physics material
// naming the sound it makes, played where a collision begins on the mixer's Impacts bus. Sounds are counted by the voices
// that start and measured on the bus, never heard.
namespace
{
	constexpr float FrameTime = 1.0f / 60.0f;
	constexpr uint32_t MaxFrames = 120;

	Ref<TypedDataAsset<PhysicsMaterial>> MakeMaterial(AssetHandle impactSound)
	{
		TypedDataAssetType<PhysicsMaterial> const* type = DataAssetRegistry::Find<PhysicsMaterial>();
		REQUIRE(type != nullptr);
		Ref<TypedDataAsset<PhysicsMaterial>> const asset = std::static_pointer_cast<TypedDataAsset<PhysicsMaterial>>(type->CreateAsset());
		PhysicsMaterial values;
		values.ImpactSound = impactSound;
		REQUIRE(asset->SetData(values).IsOk());
		return asset;
	}

	struct ImpactSetup
	{
		// The mixer's Impacts settings; the bus Impacts exists in every case.
		MixerImpacts Impacts{.Bus = "Impacts", .MinImpulse = 0.5f, .FullImpulse = 20.0f, .MaxPerFrame = 8};
		uint32_t BallCount = 1;
		// Whether the ground sounds too, and with the balls' sound or another one.
		bool GroundSounds = false;
		bool GroundSameSound = false;
	};

	struct ImpactResult
	{
		// The first frame with collisions: the strongest point's impulse, the voices started in it and the Impacts bus's
		// peak over the block after it.
		float Impulse = 0.0f;
		uint32_t CollisionCount = 0;
		uint32_t Voices = 0;
		float Peak = 0.0f;
		// Voices started in the frames after it, which run at 144 per second (some with no physics step), while the
		// balls rest on the ground and the first sounds still play.
		uint32_t LaterVoices = 0;
	};

	ImpactResult DropBalls(ImpactSetup const& setup)
	{
		Testing::AudioEngineScope const engine;
		Testing::AssetManagerScope const assets;
		Testing::PhysicsSystemScope const physics;
		AssetHandle const knock = AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.5f, 0.5f}}), "Knock");
		AssetHandle const thud = AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.3f, 0.5f}}), "Thud");
		AssetHandle const rubber = AssetManager::AddMemoryAsset(MakeMaterial(knock), "Rubber");
		AssetHandle const groundMaterial = AssetManager::AddMemoryAsset(
			MakeMaterial(setup.GroundSounds ? (setup.GroundSameSound ? knock : thud) : AssetHandle()), "Stone");

		Mixer mixer;
		MixerBus bus;
		bus.Name = "Impacts";
		bus.Parent = std::string(MasterBusName);
		mixer.Buses.push_back(bus);
		mixer.Impacts = setup.Impacts;
		REQUIRE(ValidateMixer(mixer).IsOk());
		AudioEngine::SetMixer(mixer);

		Scene scene("Impacts");
		scene.CreateEntity("Listener").AddComponent<AudioListenerComponent>();
		Entity ground = scene.CreateEntityWithUUID(UUID(100), "Ground");
		ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
		BoxColliderComponent& groundCollider = ground.AddComponent<BoxColliderComponent>();
		groundCollider.HalfExtents = {20.0f, 0.5f, 20.0f};
		groundCollider.Material = groundMaterial;
		for (uint32_t index = 0; index < setup.BallCount; index++)
		{
			// Side by side at one height: every ball lands in the same step.
			Entity ball = scene.CreateEntityWithUUID(UUID(200 + index), "Ball");
			ball.GetComponent<TransformComponent>().Translation = {-3.0f + 2.0f * static_cast<float>(index), 1.25f, -2.0f};
			RigidBodyComponent& body = ball.AddComponent<RigidBodyComponent>();
			body.Type = RigidBodyType::Dynamic;
			SphereColliderComponent& collider = ball.AddComponent<SphereColliderComponent>();
			collider.Radius = 0.25f;
			collider.Material = rubber;
		}

		SceneRuntimeSettings settings;
		settings.RunScripts = false;
		scene.OnRuntimeStart(settings);
		ImpactResult result;
		for (uint32_t frame = 0; frame < MaxFrames && result.CollisionCount == 0; frame++)
		{
			uint32_t const before = AudioEngine::GetActiveVoiceCount();
			scene.OnUpdateRuntime(Timestep(FrameTime));
			for (ContactEvent const& event : scene.GetContactEvents())
			{
				if (event.Type == ContactEventType::CollisionEnter && event.PointCount > 0)
				{
					result.Impulse = std::max(result.Impulse, event.Points[0].Impulse);
					result.CollisionCount++;
				}
			}
			result.Voices = AudioEngine::GetActiveVoiceCount() - before;
			Testing::Skip(FrameTime);
		}
		result.Peak = AudioEngine::GetBusPeak("Impacts");
		for (uint32_t frame = 0; frame < 30; frame++)
		{
			uint32_t const before = AudioEngine::GetActiveVoiceCount();
			scene.OnUpdateRuntime(Timestep(1.0f / 144.0f));
			uint32_t const after = AudioEngine::GetActiveVoiceCount();
			result.LaterVoices += after > before ? after - before : 0;
		}
		scene.OnRuntimeStop();
		return result;
	}
}

TEST_CASE("Scene: a collision that begins plays its sides' impact sounds once, at a volume of its impulse over the full one")
{
	ImpactSetup setup;
	ImpactResult const first = DropBalls(setup);
	REQUIRE(first.CollisionCount == 1);
	CHECK(first.Impulse > 2.0f);
	CHECK(first.Voices == 1);
	CHECK(first.Peak > 0.0f);
	// The contact goes on, and sounds no more.
	CHECK(first.LaterVoices == 0);

	// The same drop at two full impulses: the sound is in proportion, so the peaks are as the volumes.
	setup.Impacts.FullImpulse = first.Impulse * 2.0f;
	ImpactResult const half = DropBalls(setup);
	setup.Impacts.FullImpulse = first.Impulse * 4.0f;
	ImpactResult const quarter = DropBalls(setup);
	REQUIRE(half.Impulse == first.Impulse);
	REQUIRE(quarter.Impulse == first.Impulse);
	CHECK(half.Peak / quarter.Peak == doctest::Approx(2.0f).epsilon(1e-4));
	// Stronger than the full impulse plays at full volume, no louder.
	setup.Impacts.FullImpulse = first.Impulse * 0.5f;
	ImpactResult const full = DropBalls(setup);
	setup.Impacts.FullImpulse = first.Impulse;
	ImpactResult const exact = DropBalls(setup);
	CHECK(full.Peak == doctest::Approx(exact.Peak).epsilon(1e-6));
	CHECK(exact.Peak / half.Peak == doctest::Approx(2.0f).epsilon(1e-4));

	// Each side sounds: two sounds when the ground has another, one when both name the same.
	setup.GroundSounds = true;
	CHECK(DropBalls(setup).Voices == 2);
	setup.GroundSameSound = true;
	CHECK(DropBalls(setup).Voices == 1);
}

TEST_CASE("Scene: impact sounds play above the least impulse, at most a frame's limit of them, and none without a bus")
{
	ImpactSetup setup;
	setup.BallCount = 4;
	ImpactResult const all = DropBalls(setup);
	REQUIRE(all.CollisionCount == 4);
	CHECK(all.Voices == 4);
	CHECK(all.LaterVoices == 0);

	setup.Impacts.MaxPerFrame = 3;
	ImpactResult const limited = DropBalls(setup);
	CHECK(limited.CollisionCount == 4);
	CHECK(limited.Voices == 3);

	// Below the least impulse, nothing.
	setup.Impacts.MaxPerFrame = 8;
	setup.Impacts.MinImpulse = all.Impulse * 1.01f;
	ImpactResult const weak = DropBalls(setup);
	CHECK(weak.CollisionCount == 4);
	CHECK(weak.Voices == 0);
	CHECK(weak.Peak == 0.0f);

	// A mixer without an Impacts bus plays none: a new mixer's.
	setup.Impacts = MixerImpacts{};
	CHECK(setup.Impacts.Bus.empty());
	ImpactResult const silent = DropBalls(setup);
	CHECK(silent.CollisionCount == 4);
	CHECK(silent.Voices == 0);
	CHECK(silent.Peak == 0.0f);
}
