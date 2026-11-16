#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/PhysicsMaterial.h"
#include "Strada/Asset/PhysicsMaterialAsset.h"
#include "Strada/Core/Log.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"

#include <doctest/doctest.h>

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

// The scripting API's physics queries (Strada.Physics: SphereCast, BoxCast, CapsuleCast, RaycastAll, OverlapSphere, OverlapBox,
// OverlapCapsule) through their bindings, against the scene's own queries (PhysicsQueryTests.cpp tests those against closed
// forms), and queries made in OnFixedUpdate at three frame rates.
namespace
{
	Entity AddScript(Scene& scene, UUID id, std::string const& name, std::string const& className, ScriptFieldMap fields = {})
	{
		Entity entity = scene.CreateEntityWithUUID(id, name);
		ScriptComponent& script = entity.AddComponent<ScriptComponent>();
		script.ClassName = className;
		script.Fields = std::move(fields);
		return entity;
	}

	AssetHandle AddPhysicsMaterial(std::string const& surface)
	{
		TypedDataAssetType<PhysicsMaterial> const* type = DataAssetRegistry::Find<PhysicsMaterial>();
		REQUIRE(type != nullptr);
		Ref<TypedDataAsset<PhysicsMaterial>> const asset = std::static_pointer_cast<TypedDataAsset<PhysicsMaterial>>(type->CreateAsset());
		PhysicsMaterial material;
		material.Surface = surface;
		REQUIRE(asset->SetData(material).IsOk());
		return AssetManager::AddMemoryAsset(asset, surface);
	}

	Entity AddBox(Scene& scene, UUID id, glm::vec3 const& position, glm::vec3 const& halfExtents)
	{
		Entity box = scene.CreateEntityWithUUID(id, "Box");
		box.GetComponent<TransformComponent>().Translation = position;
		box.AddComponent<BoxColliderComponent>().HalfExtents = halfExtents;
		return box;
	}

	glm::quat TurnedAboutY(float degrees)
	{
		return glm::angleAxis(glm::radians(degrees), glm::vec3(0.0f, 1.0f, 0.0f));
	}

	// The hovering probes' scene: a ground, a wall on layer 1, a trigger and crates sliding into the probes' casts, and two
	// Strada.Tests.QueryProbe balls that hover on what their queries find.
	void BuildHoverScene(Scene& scene, int32_t hashStep)
	{
		AddBox(scene, UUID(10), {0.0f, -0.5f, 0.0f}, {20.0f, 0.5f, 20.0f});
		Entity wall = AddBox(scene, UUID(20), {3.0f, 1.0f, 0.0f}, {0.5f, 1.0f, 3.0f});
		RigidBodyComponent& wallBody = wall.AddComponent<RigidBodyComponent>();
		wallBody.Type = RigidBodyType::Static;
		wallBody.Layer = 1;
		Entity zone = AddBox(scene, UUID(30), {-3.0f, 1.0f, 0.0f}, {0.5f, 1.0f, 2.0f});
		zone.GetComponent<BoxColliderComponent>().IsTrigger = true;
		for (uint64_t i = 0; i < 4; i++)
		{
			float const offset = static_cast<float>(i);
			Entity crate = AddBox(scene, UUID(40 + i), {-8.0f + offset, 0.3f, -1.5f + offset}, glm::vec3(0.3f));
			RigidBodyComponent& body = crate.AddComponent<RigidBodyComponent>();
			body.Type = RigidBodyType::Dynamic;
			body.InitialLinearVelocity = {5.0f - offset, 0.0f, 0.5f * offset};
		}
		for (uint64_t i = 0; i < 2; i++)
		{
			float const offset = static_cast<float>(i);
			Entity probe = AddScript(scene, UUID(50 + i), "Probe", "Strada.Tests.QueryProbe",
			                         ScriptFieldMap{{"HashStep", ScriptFieldValue::FromInt32(hashStep)},
			                                        {"HoverHeight", ScriptFieldValue::FromFloat(0.8f + 0.4f * offset)}});
			probe.GetComponent<TransformComponent>().Translation = {-1.0f + 2.0f * offset, 2.0f, 0.5f * offset};
			probe.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
			probe.AddComponent<SphereColliderComponent>().Radius = 0.5f;
		}
	}

	struct HoverRun
	{
		// After fixed step `step`, and the probes' results as their fixed updates of that step left them: the same at every
		// frame rate.
		uint64_t StateHash = 0;
		std::array<uint64_t, 2> ResultHashes = {};
		// At the end of the frame that ran the step (a frame of 1/30 s runs two).
		std::array<int32_t, 2> Found = {};
		std::array<float, 2> Heights = {};
	};

	// The hover scene run at frames of frameTime seconds up to fixed step `step`.
	HoverRun RunHover(float frameTime, int32_t step)
	{
		Scene scene("Hover");
		BuildHoverScene(scene, step);
		SceneRuntimeSettings settings;
		settings.PhysicsLayerCount = 2;
		settings.PlayAudio = false;
		settings.PhysicsHashStep = static_cast<uint64_t>(step);
		scene.OnRuntimeStart(settings);
		REQUIRE(scene.GetPhysicsScene() != nullptr);
		for (int frame = 0; !scene.GetRecordedPhysicsHash() && frame < 100000; frame++)
		{
			scene.OnUpdateRuntime(Timestep(frameTime));
		}
		REQUIRE(scene.GetRecordedPhysicsHash().has_value());
		HoverRun run;
		run.StateHash = *scene.GetRecordedPhysicsHash();
		for (size_t i = 0; i < 2; i++)
		{
			UUID const probe(50 + i);
			Result<ScriptFieldMap> const read = ScriptEngine::GetInstanceFields(probe);
			REQUIRE(read.IsOk());
			run.ResultHashes[i] = read.GetValue().at("ResultHashAtStep").GetUInt64();
			run.Found[i] = read.GetValue().at("Found").GetInt32();
			run.Heights[i] = scene.GetPhysicsScene()->GetBodyTransform(probe)->Position.y;
		}
		scene.OnRuntimeStop();
		return run;
	}
}

TEST_CASE(
	"ScenePhysics: scripts' shape casts, overlaps and RaycastAll give what the scene's queries give, and refuse values that are not valid")
{
	Testing::PhysicsSystemScope physics;
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	AssetHandle const rubber = AddPhysicsMaterial("Rubber");

	// Along +X from (0, 1, 0): a trigger whose near face is at x = 1.5, a wall of Rubber at 3.5 and a post at 7.
	Scene scene("Queries");
	Entity wall = AddBox(scene, UUID(100), {4.0f, 1.0f, 0.0f}, {0.5f, 1.0f, 2.0f});
	wall.GetComponent<BoxColliderComponent>().Material = rubber;
	Entity zone = AddBox(scene, UUID(101), {2.0f, 1.0f, 0.0f}, {0.5f, 1.0f, 2.0f});
	zone.GetComponent<BoxColliderComponent>().IsTrigger = true;
	Entity post = scene.CreateEntityWithUUID(UUID(102), "Post");
	post.GetComponent<TransformComponent>().Translation = {8.0f, 1.0f, 0.0f};
	post.AddComponent<SphereColliderComponent>().Radius = 1.0f;

	glm::vec3 const origin = {0.0f, 1.0f, 0.0f};
	glm::vec3 const direction = {1.0f, 0.0f, 0.0f};
	glm::vec3 const center = {3.0f, 1.0f, 0.0f};
	glm::vec3 const halfExtents = {1.0f, 0.5f, 0.5f};
	constexpr float Radius = 0.25f;
	constexpr float HalfHeight = 0.5f;
	constexpr float TurnDegrees = 30.0f;
	// A buffer of two of the three hits along the ray.
	Entity reader = AddScript(scene, UUID(200), "Reader", "Strada.Tests.QueryReader",
	                          ScriptFieldMap{{"Origin", ScriptFieldValue::FromVector3(origin)},
	                                         {"Direction", ScriptFieldValue::FromVector3(direction)},
	                                         {"Center", ScriptFieldValue::FromVector3(center)},
	                                         {"HalfExtents", ScriptFieldValue::FromVector3(halfExtents)},
	                                         {"Radius", ScriptFieldValue::FromFloat(Radius)},
	                                         {"HalfHeight", ScriptFieldValue::FromFloat(HalfHeight)},
	                                         {"TurnDegrees", ScriptFieldValue::FromFloat(TurnDegrees)},
	                                         {"Capacity", ScriptFieldValue::FromInt32(2)}});
	reader.GetComponent<TransformComponent>().Translation = {0.0f, 10.0f, 10.0f};

	uint64_t const logStart = Log::GetNextEntryIndex();
	scene.OnRuntimeStart();
	for (int frame = 0; frame < 3; frame++)
	{
		scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	}
	Result<ScriptFieldMap> const fields = ScriptEngine::GetInstanceFields(reader.GetUUID());
	REQUIRE(fields.IsOk());
	ScriptFieldMap const& read = fields.GetValue();
	REQUIRE(read.at("Read").GetBool());

	PhysicsQueryFilter const everything{AllPhysicsLayers, true};
	glm::quat const turned = TurnedAboutY(TurnDegrees);
	// The casts' values cross unchanged.
	std::optional<ShapeCastHit> const sphere =
		scene.ShapeCast(PhysicsQueryShape::MakeSphere(Radius), origin, glm::identity<glm::quat>(), direction, 20.0f, everything);
	REQUIRE(sphere.has_value());
	CHECK(sphere->Entity == zone.GetUUID());
	CHECK(read.at("SphereEntity").GetEntity() == sphere->Entity);
	CHECK(read.at("SpherePoint").GetVector3() == sphere->Point);
	CHECK(read.at("SphereNormal").GetVector3() == sphere->Normal);
	CHECK(read.at("SphereDistance").GetFloat() == sphere->Distance);
	CHECK(read.at("SphereDistance").GetFloat() == doctest::Approx(1.25f).epsilon(1e-4));
	CHECK_FALSE(read.at("SphereMaterial").GetAsset().IsValid());
	std::optional<ShapeCastHit> const box =
		scene.ShapeCast(PhysicsQueryShape::MakeBox(halfExtents), origin, turned, direction, 20.0f, everything);
	REQUIRE(box.has_value());
	// The script turns its box by a quaternion of its own making, a few bits away from glm's.
	CHECK(read.at("BoxEntity").GetEntity() == box->Entity);
	CHECK(glm::all(glm::epsilonEqual(read.at("BoxPoint").GetVector3(), box->Point, 1e-4f)));
	CHECK(read.at("BoxDistance").GetFloat() == doctest::Approx(box->Distance).epsilon(1e-5));
	std::optional<ShapeCastHit> const capsule =
		scene.ShapeCast(PhysicsQueryShape::MakeCapsule(Radius, HalfHeight), origin, turned, direction, 20.0f, everything);
	REQUIRE(capsule.has_value());
	CHECK(read.at("CapsuleEntity").GetEntity() == capsule->Entity);
	CHECK(read.at("CapsuleNormal").GetVector3() == capsule->Normal);
	CHECK(read.at("CapsuleDistance").GetFloat() == capsule->Distance);

	// The buffer of two keeps the nearest two of the three hits: the trigger, then the wall with its material.
	std::vector<RaycastHit> hits;
	REQUIRE(scene.RaycastAll(origin, direction, 20.0f, hits, everything) == 3);
	CHECK(read.at("AllCount").GetInt32() == 2);
	CHECK(read.at("AllFirst").GetEntity() == zone.GetUUID());
	CHECK(read.at("AllLast").GetEntity() == wall.GetUUID());
	CHECK(read.at("AllLastDistance").GetFloat() == hits[1].Distance);
	CHECK(read.at("AllLastMaterial").GetAsset() == rubber);

	// The box at (3, 1, 0), turned 30 degrees, reaches the trigger and the wall, given by UUID; the sphere and the capsule
	// there reach neither.
	std::vector<UUID> overlapped;
	REQUIRE(scene.Overlap(PhysicsQueryShape::MakeBox(halfExtents), center, turned, overlapped, everything) == 2);
	CHECK(read.at("BoxOverlaps").GetInt32() == 2);
	CHECK(read.at("BoxOverlapFirst").GetEntity() == wall.GetUUID());
	CHECK(read.at("BoxOverlapLast").GetEntity() == zone.GetUUID());
	CHECK(read.at("SphereOverlaps").GetInt32() ==
	      static_cast<int32_t>(scene.Overlap(PhysicsQueryShape::MakeSphere(Radius), center, turned, overlapped, everything)));
	CHECK(read.at("CapsuleOverlaps").GetInt32() ==
	      static_cast<int32_t>(scene.Overlap(PhysicsQueryShape::MakeCapsule(Radius, HalfHeight), center, turned, overlapped, everything)));
	CHECK(read.at("SphereOverlaps").GetInt32() == 0);

	// What the queries refuse they report as script errors, naming the member, and find nothing.
	CHECK(read.at("RefusedFound").GetInt32() == 0);
	CHECK(Testing::WasLogged(logStart, "Physics.SphereCast: a radius of -1 is not valid (it must be 0 to 1000000)"));
	CHECK(Testing::WasLogged(logStart, "Physics.BoxCast: the value is not a finite number"));
	CHECK(Testing::WasLogged(logStart, "Physics.CapsuleCast: a half height of inf is not valid"));
	CHECK(Testing::WasLogged(logStart, "Physics.RaycastAll: the value is not a finite number"));
	CHECK(Testing::WasLogged(logStart, "Physics.OverlapBox: a half extent of 2000000 is not valid"));
	CHECK(Testing::WasLogged(logStart, "Physics.OverlapCapsule: the value is not a finite number"));
	scene.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: queries from OnFixedUpdate that decide forces give the same state and the same results at 30, 60 and 144 "
          "frames per second")
{
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting;
	for (int32_t const step : {1, 60, 240})
	{
		CAPTURE(step);
		HoverRun const at60 = RunHover(1.0f / 60.0f, step);
		for (float const frameTime : {1.0f / 30.0f, 1.0f / 144.0f})
		{
			CAPTURE(frameTime);
			HoverRun const other = RunHover(frameTime, step);
			CHECK(other.StateHash == at60.StateHash);
			CHECK(other.ResultHashes == at60.ResultHashes);
		}
	}
	// The casts found something in more than half the steps, and the forces they decide hold the balls up (falling, they would
	// rest at 0.5).
	HoverRun const run = RunHover(1.0f / 60.0f, 240);
	for (size_t i = 0; i < 2; i++)
	{
		CAPTURE(i);
		CHECK(run.Found[i] > 120);
		CHECK(run.Heights[i] > 1.0f);
	}
	// The results tell steps apart.
	CHECK(RunHover(1.0f / 60.0f, 120).ResultHashes != RunHover(1.0f / 60.0f, 121).ResultHashes);
}
