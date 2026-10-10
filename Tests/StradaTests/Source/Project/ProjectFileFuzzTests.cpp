#include "Fuzzing.h"
#include "Physics/PhysicsTestUtilities.h"

#include "Strada/Project/Project.h"
#include "Strada/Project/ProjectSettings.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace Strada;

TEST_CASE("Project: damaged project files are refused, or load, save again and run scenes")
{
	Testing::PhysicsSystemScope physics;
	std::vector<std::string> warnings;
	DeserializationContext const context = Testing::MakeTolerantContext(warnings);
	Json const original = Testing::LoadFeatureTestDocument("FeatureTest.sproj");
	REQUIRE(Project::Deserialize(original, context).IsOk());

	Testing::DocumentMutator mutator(5000);
	int loaded = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(300); round < rounds; round++)
	{
		CAPTURE(round);
		Result<ProjectSettings> settings = Project::Deserialize(mutator.Damage(original, round), context);
		if (!settings)
		{
			refused++;
			continue;
		}
		loaded++;
		Result<ProjectSettings> reloaded = Project::Deserialize(Project::Serialize(settings.GetValue()), context);
		REQUIRE_MESSAGE(reloaded.IsOk(), (reloaded ? std::string() : reloaded.GetError()));
		CHECK(reloaded.GetValue() == settings.GetValue());

		// Its runtime settings (physics only) simulate a scene: a ball falling onto the ground.
		Scene scene("Project");
		Entity ground = scene.CreateEntity("Ground");
		ground.AddComponent<BoxColliderComponent>().HalfExtents = {5.0f, 0.5f, 5.0f};
		Entity ball = scene.CreateEntity("Ball");
		ball.GetComponent<TransformComponent>().Translation = {0.0f, 3.0f, 0.0f};
		ball.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		ball.AddComponent<SphereColliderComponent>();
		SceneRuntimeSettings runtime = MakeSceneRuntimeSettings(settings.GetValue());
		runtime.RunScripts = false;
		runtime.PlayAudio = false;
		scene.OnRuntimeStart(runtime);
		for (int frame = 0; frame < 5; frame++)
		{
			scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
		}
		scene.OnRuntimeStop();
	}
	CHECK(loaded > 0);
	CHECK(refused > 0);
}
