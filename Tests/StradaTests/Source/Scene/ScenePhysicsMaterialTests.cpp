#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/MeshFactory.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Asset/PhysicsMaterial.h"
#include "Strada/Asset/PhysicsMaterialAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float Step = 1.0f / 60.0f;
	constexpr float Gravity = 9.81f;
	// The closed forms below hold to within this part of their value (Jolt solves contacts iteratively).
	constexpr double Tolerance = 0.02;

	PhysicsMaterial Values(float friction, PhysicsMaterialCombine frictionCombine = PhysicsMaterialCombine::GeometricMean,
	                       float restitution = 0.0f, PhysicsMaterialCombine restitutionCombine = PhysicsMaterialCombine::Maximum)
	{
		PhysicsMaterial material;
		material.Friction = friction;
		material.Restitution = restitution;
		material.FrictionCombine = frictionCombine;
		material.RestitutionCombine = restitutionCombine;
		return material;
	}

	Ref<TypedDataAsset<PhysicsMaterial>> MakeMaterial(PhysicsMaterial const& values)
	{
		TypedDataAssetType<PhysicsMaterial> const* type = DataAssetRegistry::Find<PhysicsMaterial>();
		REQUIRE(type != nullptr);
		Ref<TypedDataAsset<PhysicsMaterial>> const asset = std::static_pointer_cast<TypedDataAsset<PhysicsMaterial>>(type->CreateAsset());
		REQUIRE(asset->SetData(values).IsOk());
		return asset;
	}

	AssetHandle AddMaterial(PhysicsMaterial const& values, std::string name)
	{
		return AssetManager::AddMemoryAsset(MakeMaterial(values), std::move(name));
	}

	SceneRuntimeSettings PhysicsOnly(uint64_t hashStep = 0)
	{
		SceneRuntimeSettings settings;
		settings.RunScripts = false;
		settings.PlayAudio = false;
		settings.PhysicsHashStep = hashStep;
		return settings;
	}

	Entity MakeGround(Scene& scene, uint64_t id, AssetHandle material, float friction, float restitution = 0.0f)
	{
		Entity ground = scene.CreateEntityWithUUID(UUID(id), "Ground");
		ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
		BoxColliderComponent& collider = ground.AddComponent<BoxColliderComponent>();
		collider.HalfExtents = {40.0f, 0.5f, 40.0f};
		collider.Material = material;
		collider.Friction = friction;
		collider.Restitution = restitution;
		return ground;
	}

	// A box that only slides: no damping, no turning, never asleep.
	Entity MakeSlider(Scene& scene, uint64_t id, glm::vec3 const& position, glm::vec3 const& velocity, AssetHandle material, float friction)
	{
		Entity slider = scene.CreateEntityWithUUID(UUID(id), "Slider");
		slider.GetComponent<TransformComponent>().Translation = position;
		RigidBodyComponent& body = slider.AddComponent<RigidBodyComponent>();
		body.Type = RigidBodyType::Dynamic;
		body.LinearDamping = 0.0f;
		body.AngularDamping = 0.0f;
		body.LockRotation = glm::bvec3(true);
		body.AllowSleep = false;
		body.InitialLinearVelocity = velocity;
		BoxColliderComponent& collider = slider.AddComponent<BoxColliderComponent>();
		collider.HalfExtents = glm::vec3(0.25f);
		collider.Material = material;
		collider.Friction = friction;
		return slider;
	}

	// Three flat strips along Z at y = 0, one submesh each, from x = -6 to 6 (4 m each).
	Ref<MeshSource> MakeStrips()
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<Submesh> submeshes;
		for (uint32_t strip = 0; strip < 3; strip++)
		{
			float const left = -6.0f + 4.0f * static_cast<float>(strip);
			Submesh submesh;
			submesh.BaseVertex = static_cast<uint32_t>(vertices.size());
			submesh.VertexCount = 4;
			submesh.BaseIndex = static_cast<uint32_t>(indices.size());
			submesh.IndexCount = 6;
			for (glm::vec3 const corner : {glm::vec3(left, 0.0f, -10.0f), glm::vec3(left, 0.0f, 10.0f), glm::vec3(left + 4.0f, 0.0f, 10.0f),
			                               glm::vec3(left + 4.0f, 0.0f, -10.0f)})
			{
				Vertex vertex;
				vertex.Position = corner;
				vertex.Normal = {0.0f, 1.0f, 0.0f};
				vertices.push_back(vertex);
			}
			for (uint32_t const index : {0u, 1u, 2u, 0u, 2u, 3u})
			{
				indices.push_back(index);
			}
			submeshes.push_back(submesh);
		}
		Result<Ref<MeshSource>> mesh =
			MeshSource::Create(std::move(vertices), std::move(indices), std::move(submeshes), {AssetHandle(UUID(1))});
		REQUIRE_MESSAGE(mesh.IsOk(), (mesh ? std::string() : mesh.GetError()));
		return mesh.GetValue();
	}

	float SpeedAlong(Scene& scene, uint64_t id, glm::vec3 const& direction)
	{
		return glm::dot(scene.GetPhysicsScene()->GetLinearVelocity(UUID(id)), direction);
	}

	// The acceleration of a body along a direction over `frames` frames of one step each.
	float MeasureAcceleration(Scene& scene, uint64_t id, glm::vec3 const& direction, int frames)
	{
		float const before = SpeedAlong(scene, id, direction);
		for (int frame = 0; frame < frames; frame++)
		{
			scene.OnUpdateRuntime(Timestep(Step));
		}
		return (SpeedAlong(scene, id, direction) - before) / (static_cast<float>(frames) * Step);
	}

	AssetHandle MaterialBelow(Scene& scene, float x, float z)
	{
		std::optional<RaycastHit> const hit = scene.GetPhysicsScene()->Raycast({x, 10.0f, z}, {0.0f, -1.0f, 0.0f}, 100.0f);
		REQUIRE(hit);
		return hit->Material;
	}

	void WriteMaterialFile(std::filesystem::path const& path, PhysicsMaterial const& values)
	{
		REQUIRE(DataAssetSerializer::SaveToFile(*MakeMaterial(values), path).IsOk());
	}
}

TEST_CASE("ScenePhysics: colliders slide by their physics materials, and references to no material keep their own values")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	AssetHandle const ice = AddMaterial(Values(0.05f), "Ice");
	AssetHandle const rubber = AddMaterial(Values(1.0f), "Rubber");
	// A reference to an asset of another type, and to no asset at all, count as unset.
	AssetHandle const cube = AssetManager::AddMemoryAsset(MeshFactory::CreateCube(AssetHandle()), "Cube");
	AssetHandle const unknown(UUID(424242));

	Scene scene("Materials");
	// The ground's own friction (1) is replaced by the ice's.
	MakeGround(scene, 100, ice, 1.0f);
	glm::vec3 const along(1.0f, 0.0f, 0.0f);
	MakeSlider(scene, 201, {-20.0f, 0.25f, -6.0f}, along * 6.0f, rubber, 0.2f);
	MakeSlider(scene, 202, {-20.0f, 0.25f, -2.0f}, along * 6.0f, cube, 0.2f);
	MakeSlider(scene, 203, {-20.0f, 0.25f, 2.0f}, along * 6.0f, unknown, 0.2f);
	MakeSlider(scene, 204, {-20.0f, 0.25f, 6.0f}, along * 6.0f, AssetHandle(), 0.2f);

	// A triangle mesh whose first strip has a material of its own, whose second names a mesh, and whose third is past the
	// list: those two have the collider's.
	Entity track = scene.CreateEntityWithUUID(UUID(300), "Track");
	track.GetComponent<TransformComponent>().Translation = {60.0f, 0.0f, 0.0f};
	MeshColliderComponent& mesh = track.AddComponent<MeshColliderComponent>();
	mesh.Mesh = AssetManager::AddMemoryAsset(MakeStrips(), "Strips");
	mesh.Convex = false;
	mesh.Material = ice;
	mesh.Materials = {rubber, cube};

	scene.OnRuntimeStart(PhysicsOnly());
	REQUIRE(scene.GetPhysicsScene() != nullptr);
	CHECK(MaterialBelow(scene, 10.0f, 20.0f) == ice);
	CHECK(MaterialBelow(scene, -20.0f, -6.0f) == rubber);
	CHECK_FALSE(MaterialBelow(scene, -20.0f, -2.0f).IsValid());
	CHECK_FALSE(MaterialBelow(scene, -20.0f, 2.0f).IsValid());
	CHECK_FALSE(MaterialBelow(scene, -20.0f, 6.0f).IsValid());
	CHECK(MaterialBelow(scene, 56.0f, 0.0f) == rubber);
	CHECK(MaterialBelow(scene, 60.0f, 0.0f) == ice);
	CHECK(MaterialBelow(scene, 64.0f, 0.0f) == ice);

	for (int frame = 0; frame < 5; frame++)
	{
		scene.OnUpdateRuntime(Timestep(Step));
	}
	std::vector<float> before;
	for (uint64_t const id : {201u, 202u, 203u, 204u})
	{
		before.push_back(SpeedAlong(scene, id, along));
	}
	for (int frame = 0; frame < 20; frame++)
	{
		scene.OnUpdateRuntime(Timestep(Step));
	}
	float const seconds = 20.0f * Step;
	CHECK((SpeedAlong(scene, 201, along) - before[0]) / seconds == doctest::Approx(-Gravity * std::sqrt(0.05f)).epsilon(Tolerance));
	for (size_t i = 1; i < 4; i++)
	{
		CAPTURE(i);
		CHECK((SpeedAlong(scene, 201 + i, along) - before[i]) / seconds ==
		      doctest::Approx(-Gravity * std::sqrt(0.05f * 0.2f)).epsilon(Tolerance));
	}
	scene.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: a physics material edited, reloaded or broken while the scene runs reaches its bodies before the next step")
{
	Testing::TemporaryDirectory const directory;
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	std::filesystem::path const file = directory.GetPath() / "Ground.sphysmat";
	WriteMaterialFile(file, Values(0.2f));
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());
	AssetHandle const material = AssetManager::FindByPath("Ground.sphysmat");
	REQUIRE(material.IsValid());

	Scene scene("Live");
	MakeGround(scene, 100, material, 0.5f);
	glm::vec3 const along(1.0f, 0.0f, 0.0f);
	MakeSlider(scene, 200, {-35.0f, 0.25f, 0.0f}, along * 20.0f, AssetHandle(), 1.0f);
	scene.OnRuntimeStart(PhysicsOnly());
	PhysicsScene& world = *scene.GetPhysicsScene();
	for (int frame = 0; frame < 5; frame++)
	{
		scene.OnUpdateRuntime(Timestep(Step));
	}
	CHECK(MeasureAcceleration(scene, 200, along, 10) == doctest::Approx(-Gravity * std::sqrt(0.2f)).epsilon(Tolerance));
	uint64_t const structure = world.GetStructureVersion();

	// Edited in place (the inspector, physics-material.set and undo change the loaded asset): the very next step slides
	// by the new value, the body is not made again and its contact goes on.
	Ref<TypedDataAsset<PhysicsMaterial>> const loaded = AssetManager::GetAsset<TypedDataAsset<PhysicsMaterial>>(material);
	REQUIRE(loaded != nullptr);
	REQUIRE(loaded->SetData(Values(0.8f)).IsOk());
	CHECK(MeasureAcceleration(scene, 200, along, 1) == doctest::Approx(-Gravity * std::sqrt(0.8f)).epsilon(Tolerance));
	CHECK(scene.GetContactEvents().empty());
	CHECK(MeasureAcceleration(scene, 200, along, 5) == doctest::Approx(-Gravity * std::sqrt(0.8f)).epsilon(Tolerance));
	CHECK(world.GetStructureVersion() == structure);

	// Its file changed and reloaded: another asset object, whose values reach the body alike.
	WriteMaterialFile(file, Values(0.4f));
	REQUIRE(AssetManager::ReloadAsset(material).IsOk());
	CHECK(MeasureAcceleration(scene, 200, along, 1) == doctest::Approx(-Gravity * std::sqrt(0.4f)).epsilon(Tolerance));
	CHECK(world.GetStructureVersion() == structure);
	CHECK(MaterialBelow(scene, 20.0f, 20.0f) == material);

	// A file that no longer loads counts as unset: the ground is made again with its own friction, and the box keeps
	// sliding at the speed it had.
	REQUIRE(FileSystem::WriteTextFile(file, "{ not a physics material").IsOk());
	CHECK(AssetManager::ReloadAsset(material).IsError());
	float const speed = SpeedAlong(scene, 200, along);
	CHECK(MeasureAcceleration(scene, 200, along, 1) == doctest::Approx(-Gravity * std::sqrt(0.5f)).epsilon(Tolerance));
	CHECK(SpeedAlong(scene, 200, along) == doctest::Approx(speed - Gravity * std::sqrt(0.5f) * Step).epsilon(Tolerance));
	CHECK(world.GetStructureVersion() != structure);
	CHECK_FALSE(MaterialBelow(scene, 20.0f, 20.0f).IsValid());

	// Fixed again, it applies again.
	WriteMaterialFile(file, Values(0.6f));
	REQUIRE(AssetManager::ReloadAsset(material).IsOk());
	CHECK(MeasureAcceleration(scene, 200, along, 3) == doctest::Approx(-Gravity * std::sqrt(0.6f)).epsilon(Tolerance));
	CHECK(MaterialBelow(scene, 20.0f, 20.0f) == material);
	scene.OnRuntimeStop();
}

namespace
{
	struct MaterialSet
	{
		AssetHandle Slope;
		AssetHandle Average;
		AssetHandle Minimum;
		AssetHandle Bouncy;
		AssetHandle Ice;
		AssetHandle Rubber;
	};

	MaterialSet AddMaterialSet()
	{
		MaterialSet set;
		set.Slope = AddMaterial(Values(0.9f, PhysicsMaterialCombine::Multiply), "Slope");
		set.Average = AddMaterial(Values(0.5f, PhysicsMaterialCombine::Average, 0.2f, PhysicsMaterialCombine::Average), "Average");
		set.Minimum = AddMaterial(Values(0.3f, PhysicsMaterialCombine::Minimum), "Minimum");
		set.Bouncy = AddMaterial(Values(0.4f, PhysicsMaterialCombine::GeometricMean, 0.8f, PhysicsMaterialCombine::Maximum), "Bouncy");
		set.Ice = AddMaterial(Values(0.05f), "Ice");
		set.Rubber = AddMaterial(Values(1.0f, PhysicsMaterialCombine::Maximum), "Rubber");
		return set;
	}

	// Crates tumbling down a slope onto the ground, a ball bouncing, and boxes sliding across a mesh of three surfaces: every
	// mode in play, unless `materials` is false (then every collider has its own values).
	void BuildMaterialScene(Scene& scene, MaterialSet const& set, AssetHandle strips, bool materials)
	{
		auto const pick = [materials](AssetHandle material)
		{
			return materials ? material : AssetHandle();
		};
		MakeGround(scene, 100, pick(set.Ice), 0.6f, 0.1f);
		Entity slope = scene.CreateEntityWithUUID(UUID(110), "Slope");
		slope.GetComponent<TransformComponent>().Translation = {0.0f, 3.0f, 0.0f};
		slope.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::radians(25.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		BoxColliderComponent& slopeCollider = slope.AddComponent<BoxColliderComponent>();
		slopeCollider.HalfExtents = {6.0f, 0.5f, 4.0f};
		slopeCollider.Material = pick(set.Slope);
		AssetHandle const crates[] = {set.Average, set.Minimum, AssetHandle(), set.Rubber};
		for (uint64_t i = 0; i < 4; i++)
		{
			Entity crate = scene.CreateEntityWithUUID(UUID(200 + i), "Crate");
			crate.GetComponent<TransformComponent>().Translation = {2.0f, 6.0f + static_cast<float>(i) * 0.7f,
			                                                        -3.0f + 2.0f * static_cast<float>(i)};
			crate.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
			BoxColliderComponent& collider = crate.AddComponent<BoxColliderComponent>();
			collider.HalfExtents = glm::vec3(0.3f);
			collider.Material = pick(crates[i]);
			collider.Friction = 0.4f;
		}
		Entity ball = scene.CreateEntityWithUUID(UUID(300), "Ball");
		ball.GetComponent<TransformComponent>().Translation = {-10.0f, 4.0f, 6.0f};
		ball.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		SphereColliderComponent& sphere = ball.AddComponent<SphereColliderComponent>();
		sphere.Radius = 0.4f;
		sphere.Material = pick(set.Bouncy);
		Entity track = scene.CreateEntityWithUUID(UUID(400), "Track");
		track.GetComponent<TransformComponent>().Translation = {20.0f, 0.01f, 0.0f};
		MeshColliderComponent& mesh = track.AddComponent<MeshColliderComponent>();
		mesh.Mesh = strips;
		mesh.Convex = false;
		mesh.Materials = {pick(set.Ice), pick(set.Rubber)};
		for (uint64_t i = 0; i < 3; i++)
		{
			Entity box = scene.CreateEntityWithUUID(UUID(500 + i), "Box");
			box.GetComponent<TransformComponent>().Translation = {15.0f + static_cast<float>(i) * 0.5f, 0.3f,
			                                                      -8.0f + 3.0f * static_cast<float>(i)};
			RigidBodyComponent& body = box.AddComponent<RigidBodyComponent>();
			body.Type = RigidBodyType::Dynamic;
			body.InitialLinearVelocity = {7.0f, 0.0f, 2.0f};
			box.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.25f);
		}
	}

	uint64_t HashAtStep(MaterialSet const& set, AssetHandle strips, bool materials, float frameTime, uint64_t step)
	{
		Scene scene("Materials");
		BuildMaterialScene(scene, set, strips, materials);
		scene.OnRuntimeStart(PhysicsOnly(step));
		REQUIRE(scene.GetPhysicsScene() != nullptr);
		for (uint64_t frame = 0; !scene.GetRecordedPhysicsHash() && frame < 100000; frame++)
		{
			scene.OnUpdateRuntime(Timestep(frameTime));
		}
		REQUIRE(scene.GetRecordedPhysicsHash().has_value());
		uint64_t const hash = *scene.GetRecordedPhysicsHash();
		scene.OnRuntimeStop();
		return hash;
	}
}

TEST_CASE("ScenePhysics: with physics materials in play, one scene gives the same state hashes at 30, 60 and 144 frames per second")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	MaterialSet const set = AddMaterialSet();
	AssetHandle const strips = AssetManager::AddMemoryAsset(MakeStrips(), "Strips");
	for (uint64_t const step : {1u, 60u, 150u, 300u})
	{
		CAPTURE(step);
		uint64_t const at60 = HashAtStep(set, strips, true, 1.0f / 60.0f, step);
		CHECK(HashAtStep(set, strips, true, 1.0f / 30.0f, step) == at60);
		CHECK(HashAtStep(set, strips, true, 1.0f / 144.0f, step) == at60);
	}
	// The materials change what happens.
	CHECK(HashAtStep(set, strips, true, Step, 150) != HashAtStep(set, strips, false, Step, 150));
}

TEST_CASE("ScenePhysics: materials with a collider's own values at the default modes simulate bit for bit as those values")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	AssetHandle const strips = AssetManager::AddMemoryAsset(MakeStrips(), "Strips");
	// Every collider's own values, and materials holding the same with the default modes (GeometricMean, Maximum).
	AssetHandle const ground = AddMaterial(Values(0.6f, PhysicsMaterialCombine::GeometricMean, 0.1f), "Ground");
	AssetHandle const crate = AddMaterial(Values(0.4f, PhysicsMaterialCombine::GeometricMean, 0.3f), "Crate");
	AssetHandle const ball = AddMaterial(Values(0.7f, PhysicsMaterialCombine::GeometricMean, 0.5f), "Ball");
	AssetHandle const track = AddMaterial(Values(0.6f, PhysicsMaterialCombine::GeometricMean, 0.0f), "Track");
	auto const build = [&](Scene& scene, bool materials)
	{
		auto const pick = [materials](AssetHandle material)
		{
			return materials ? material : AssetHandle();
		};
		MakeGround(scene, 100, pick(ground), 0.6f, 0.1f);
		for (uint64_t i = 0; i < 4; i++)
		{
			Entity box = scene.CreateEntityWithUUID(UUID(200 + i), "Crate");
			box.GetComponent<TransformComponent>().Translation = {0.02f * static_cast<float>(i), 0.5f + static_cast<float>(i), 0.0f};
			box.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
			BoxColliderComponent& collider = box.AddComponent<BoxColliderComponent>();
			collider.HalfExtents = glm::vec3(0.5f);
			collider.Material = pick(crate);
			collider.Friction = 0.4f;
			collider.Restitution = 0.3f;
		}
		Entity sphere = scene.CreateEntityWithUUID(UUID(300), "Ball");
		sphere.GetComponent<TransformComponent>().Translation = {-6.0f, 0.5f, 0.1f};
		RigidBodyComponent& body = sphere.AddComponent<RigidBodyComponent>();
		body.Type = RigidBodyType::Dynamic;
		body.Mass = 5.0f;
		body.InitialLinearVelocity = {8.0f, 0.0f, 0.0f};
		SphereColliderComponent& collider = sphere.AddComponent<SphereColliderComponent>();
		collider.Radius = 0.5f;
		collider.Material = pick(ball);
		collider.Friction = 0.7f;
		collider.Restitution = 0.5f;
		// A mesh whose submeshes name materials with its own values: its triangles then carry material indices.
		Entity strip = scene.CreateEntityWithUUID(UUID(400), "Track");
		strip.GetComponent<TransformComponent>().Translation = {20.0f, 0.01f, 0.0f};
		MeshColliderComponent& mesh = strip.AddComponent<MeshColliderComponent>();
		mesh.Mesh = strips;
		mesh.Convex = false;
		mesh.Materials = {pick(track), pick(track), pick(track)};
		Entity slider = scene.CreateEntityWithUUID(UUID(500), "Slider");
		slider.GetComponent<TransformComponent>().Translation = {15.0f, 0.3f, 0.0f};
		RigidBodyComponent& sliderBody = slider.AddComponent<RigidBodyComponent>();
		sliderBody.Type = RigidBodyType::Dynamic;
		sliderBody.InitialLinearVelocity = {9.0f, 0.0f, 1.0f};
		slider.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.25f);
	};
	Scene own("Own values");
	Scene named("Materials");
	build(own, false);
	build(named, true);
	own.OnRuntimeStart(PhysicsOnly());
	named.OnRuntimeStart(PhysicsOnly());
	for (int frame = 0; frame < 180; frame++)
	{
		CAPTURE(frame);
		own.OnUpdateRuntime(Timestep(Step));
		named.OnUpdateRuntime(Timestep(Step));
		REQUIRE(own.ComputePhysicsStateHash() == named.ComputePhysicsStateHash());
	}
	CHECK(MaterialBelow(named, 20.0f, 0.0f) == track);
	CHECK_FALSE(MaterialBelow(own, 20.0f, 0.0f).IsValid());
	own.OnRuntimeStop();
	named.OnRuntimeStop();
}
