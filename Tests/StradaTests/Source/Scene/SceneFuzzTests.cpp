#include "Audio/AudioTestUtilities.h"
#include "Fuzzing.h"
#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	// A few frames of the scene's runtime, with physics and sound (scripts do not run without the script engine).
	void Run(Scene& scene)
	{
		scene.OnRuntimeStart();
		for (int frame = 0; frame < 5; frame++)
		{
			scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
		}
		scene.OnRuntimeStop();
	}
}

TEST_CASE("SceneSerializer: damaged scene files are rejected or repaired, never crash")
{
	std::vector<std::string> warnings;
	DeserializationContext const context = Testing::MakeTolerantContext(warnings);

	uint64_t seed = 1;
	for (auto const& [file, rounds] : {std::pair{"Assets/Scenes/Main.sscene", 400}, std::pair{"Assets/Scenes/Second.sscene", 150}})
	{
		Json const original = Testing::LoadFeatureTestDocument(file);
		REQUIRE(SceneSerializer::Deserialize(original, context).IsOk());

		Testing::DocumentMutator mutator(seed++);
		int loaded = 0;
		int rejected = 0;
		for (int round = 0, scaled = Testing::GetFuzzRounds(rounds); round < scaled; round++)
		{
			CAPTURE(file);
			CAPTURE(round);
			Result<Ref<Scene>> scene = SceneSerializer::Deserialize(mutator.Damage(original, round), context);
			if (!scene)
			{
				rejected++;
				continue;
			}
			loaded++;
			// What a repaired scene saves loads again.
			Json const saved = SceneSerializer::Serialize(*scene.GetValue());
			Result<Ref<Scene>> reloaded = SceneSerializer::Deserialize(saved, context);
			CHECK_MESSAGE(reloaded.IsOk(), (reloaded ? std::string() : reloaded.GetError()));
		}
		// Both outcomes happen, or the damage was too mild or too severe to tell anything.
		CHECK(loaded > 0);
		CHECK(rejected > 0);
	}
}

TEST_CASE("Scene: damaged scenes that load also run")
{
	Testing::PhysicsSystemScope physics;
	Testing::AudioEngineScope audio;
	Testing::AssetManagerScope assets;
	std::vector<std::string> warnings;
	DeserializationContext const context = Testing::MakeTolerantContext(warnings);
	Json const original = Testing::LoadFeatureTestDocument("Assets/Scenes/Main.sscene");

	Testing::DocumentMutator mutator(1000);
	int ran = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		Result<Ref<Scene>> scene = SceneSerializer::Deserialize(mutator.Damage(original, round), context);
		if (scene)
		{
			Run(*scene.GetValue());
			ran++;
		}
	}
	CHECK(ran > 0);
}

TEST_CASE("PrefabSerializer: damaged prefabs are refused or instantiate and run")
{
	Testing::PhysicsSystemScope physics;
	Testing::AudioEngineScope audio;
	Testing::AssetManagerScope assets;
	std::vector<std::string> warnings;
	DeserializationContext const context = Testing::MakeTolerantContext(warnings);
	Json const original = Testing::LoadFeatureTestDocument("Assets/Prefabs/Crate.sprefab");
	AssetHandle const prefab(UUID(1234));

	Testing::DocumentMutator mutator(2000);
	int instantiated = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		Scene scene("Prefabs");
		Entity const parent = scene.CreateEntity("Parent");
		size_t const entityCount = scene.GetEntityCount();
		Result<Entity> instance =
			PrefabSerializer::Instantiate(scene, mutator.Damage(original, round), prefab, round % 2 == 0 ? parent : Entity(), context);
		if (!instance)
		{
			// Nothing is created on failure.
			CHECK(scene.GetEntityCount() == entityCount);
			refused++;
			continue;
		}
		instantiated++;
		Json const saved = SceneSerializer::Serialize(scene);
		Result<Ref<Scene>> reloaded = SceneSerializer::Deserialize(saved, context);
		CHECK_MESSAGE(reloaded.IsOk(), (reloaded ? std::string() : reloaded.GetError()));
		Run(scene);
	}
	CHECK(instantiated > 0);
	CHECK(refused > 0);
}
