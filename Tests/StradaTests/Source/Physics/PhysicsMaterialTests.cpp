#include "Physics/PhysicsTestUtilities.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/MeshFactory.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Asset/PhysicsMaterial.h"

#include <doctest/doctest.h>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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

	UUID const GroundId(101);
	UUID const FirstId(102);
	UUID const SecondId(103);
	UUID const ThirdId(104);

	AssetHandle const SlopeMaterial(UUID(5001));
	AssetHandle const BoxMaterial(UUID(5002));
	AssetHandle const SlipperyMaterial(UUID(5003));
	AssetHandle const GrippyMaterial(UUID(5004));
	AssetHandle const ColliderMaterial(UUID(5005));

	Scope<PhysicsScene> CreateWorld()
	{
		Result<Scope<PhysicsScene>> world = PhysicsScene::Create({});
		REQUIRE(world.IsOk());
		return world.TakeValue();
	}

	ColliderSurface MakeSurface(AssetHandle material, float friction, PhysicsMaterialCombine frictionCombine, float restitution = 0.0f,
	                            PhysicsMaterialCombine restitutionCombine = PhysicsMaterialCombine::Maximum)
	{
		ColliderSurface surface;
		surface.Material = material;
		surface.Friction = friction;
		surface.Restitution = restitution;
		surface.FrictionCombine = frictionCombine;
		surface.RestitutionCombine = restitutionCombine;
		return surface;
	}

	ColliderDesc Box(glm::vec3 const& halfExtents, ColliderSurface const& surface, glm::vec3 const& offset = glm::vec3(0.0f))
	{
		ColliderDesc collider;
		collider.Shape = ColliderShape::Box;
		collider.HalfExtents = halfExtents;
		collider.Offset = offset;
		collider.Surface = surface;
		return collider;
	}

	BodyDesc MakeStatic(UUID entity, glm::vec3 const& position, glm::quat const& rotation, ColliderDesc collider)
	{
		BodyDesc desc;
		desc.Entity = entity;
		desc.Type = RigidBodyType::Static;
		desc.Position = position;
		desc.Rotation = rotation;
		desc.Colliders.push_back(std::move(collider));
		return desc;
	}

	// A dynamic body that only slides (no damping, no turning, never asleep), so that its motion has a closed form.
	BodyDesc MakeSlider(UUID entity, glm::vec3 const& position, glm::quat const& rotation, std::vector<ColliderDesc> colliders,
	                    glm::vec3 const& velocity = glm::vec3(0.0f))
	{
		BodyDesc desc;
		desc.Entity = entity;
		desc.Type = RigidBodyType::Dynamic;
		desc.Position = position;
		desc.Rotation = rotation;
		desc.Colliders = std::move(colliders);
		desc.LinearDamping = 0.0f;
		desc.AngularDamping = 0.0f;
		desc.LockRotation = glm::bvec3(true);
		desc.AllowSleep = false;
		desc.LinearVelocity = velocity;
		return desc;
	}

	void Simulate(PhysicsScene& world, int steps)
	{
		for (int step = 0; step < steps; step++)
		{
			world.Step(Step);
		}
	}

	// The body's acceleration along a direction over `steps` steps.
	float MeasureAcceleration(PhysicsScene& world, UUID entity, glm::vec3 const& direction, int steps)
	{
		float const before = glm::dot(world.GetLinearVelocity(entity), direction);
		Simulate(world, steps);
		float const after = glm::dot(world.GetLinearVelocity(entity), direction);
		return (after - before) / (static_cast<float>(steps) * Step);
	}

	// A flat mesh of strips along Z at y = 0, one submesh per strip: strip i covers x from x0 + 4i to x0 + 4(i + 1) and z
	// from -10 to 10.
	Ref<MeshSource> MakeStrips(uint32_t count, float x0)
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<Submesh> submeshes;
		for (uint32_t strip = 0; strip < count; strip++)
		{
			float const left = x0 + 4.0f * static_cast<float>(strip);
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
			// Counter-clockwise seen from above.
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

	AssetHandle MaterialBelow(PhysicsScene const& world, float x, float z)
	{
		std::optional<RaycastHit> const hit = world.Raycast({x, 10.0f, z}, {0.0f, -1.0f, 0.0f}, 100.0f);
		REQUIRE(hit);
		return hit->Material;
	}
}

TEST_CASE("PhysicsScene: each combine mode of a slope's and a box's friction gives the slide its closed form says")
{
	Testing::PhysicsSystemScope physics;
	// On a 30 degree slope a box slides when the combined friction is below tan 30 = 0.577, at g (sin 30 - mu cos 30).
	float const angle = glm::radians(30.0f);
	glm::quat const tilt = glm::angleAxis(angle, glm::vec3(0.0f, 0.0f, 1.0f));
	glm::vec3 const normal = tilt * glm::vec3(0.0f, 1.0f, 0.0f);
	glm::vec3 const downhill = tilt * glm::vec3(-1.0f, 0.0f, 0.0f);
	float const slope = 0.8f;
	float const box = 0.25f;
	struct Case
	{
		PhysicsMaterialCombine Mode;
		float Friction;
	};
	std::array<Case, 5> const cases = {{
		{PhysicsMaterialCombine::GeometricMean, std::sqrt(slope * box)},
		{PhysicsMaterialCombine::Average, (slope + box) * 0.5f},
		{PhysicsMaterialCombine::Minimum, box},
		{PhysicsMaterialCombine::Multiply, slope * box},
		{PhysicsMaterialCombine::Maximum, slope},
	}};
	for (Case const& entry : cases)
	{
		CAPTURE(static_cast<int>(entry.Mode));
		// The slope names the mode; the box's GeometricMean is first in the order, so the slope's mode wins. Swapped, the
		// same mode wins from the box's side, to the bit.
		auto const slide = [&](bool modeOnBox)
		{
			Scope<PhysicsScene> world = CreateWorld();
			PhysicsMaterialCombine const slopeMode = modeOnBox ? PhysicsMaterialCombine::GeometricMean : entry.Mode;
			PhysicsMaterialCombine const boxMode = modeOnBox ? entry.Mode : PhysicsMaterialCombine::GeometricMean;
			REQUIRE(world
			            ->AddBody(MakeStatic(GroundId, glm::vec3(0.0f), tilt,
			                                 Box({10.0f, 0.5f, 10.0f}, MakeSurface(SlopeMaterial, slope, slopeMode))))
			            .IsOk());
			REQUIRE(
				world->AddBody(MakeSlider(FirstId, normal * 0.75f, tilt, {Box(glm::vec3(0.25f), MakeSurface(BoxMaterial, box, boxMode))}))
					.IsOk());
			Simulate(*world, 30);
			return MeasureAcceleration(*world, FirstId, downhill, 60);
		};
		float const acceleration = slide(false);
		float const expected = Gravity * (std::sin(angle) - entry.Friction * std::cos(angle));
		if (expected > 0.0f)
		{
			CHECK(acceleration == doctest::Approx(expected).epsilon(Tolerance));
		}
		else
		{
			// Held: Maximum takes the slope's 0.8, above tan 30.
			CHECK(std::abs(acceleration) < 0.01f);
		}
		CHECK(slide(true) == acceleration);
	}
}

TEST_CASE("PhysicsScene: each combine mode of a ball's and the ground's restitution gives the bounce its closed form says")
{
	Testing::PhysicsSystemScope physics;
	float const ground = 0.9f;
	float const ball = 0.4f;
	struct Case
	{
		PhysicsMaterialCombine Mode;
		float Restitution;
	};
	std::array<Case, 5> const cases = {{
		{PhysicsMaterialCombine::GeometricMean, std::sqrt(ground * ball)},
		{PhysicsMaterialCombine::Average, (ground + ball) * 0.5f},
		{PhysicsMaterialCombine::Minimum, ball},
		{PhysicsMaterialCombine::Multiply, ground * ball},
		{PhysicsMaterialCombine::Maximum, ground},
	}};
	for (Case const& entry : cases)
	{
		CAPTURE(static_cast<int>(entry.Mode));
		Scope<PhysicsScene> world = CreateWorld();
		REQUIRE(world
		            ->AddBody(MakeStatic(GroundId, {0.0f, -0.5f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
		                                 Box({10.0f, 0.5f, 10.0f},
		                                     MakeSurface(SlopeMaterial, 0.0f, PhysicsMaterialCombine::GeometricMean, ground, entry.Mode))))
		            .IsOk());
		// Without gravity the ball meets the ground at the speed it was given and leaves at that speed times the restitution.
		ColliderDesc sphere;
		sphere.Shape = ColliderShape::Sphere;
		sphere.Radius = 0.5f;
		sphere.Surface = MakeSurface(BoxMaterial, 0.0f, PhysicsMaterialCombine::GeometricMean, ball, PhysicsMaterialCombine::GeometricMean);
		BodyDesc falling = MakeSlider(FirstId, {0.0f, 1.5f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), {sphere}, {0.0f, -5.0f, 0.0f});
		falling.GravityFactor = 0.0f;
		REQUIRE(world->AddBody(falling).IsOk());
		Simulate(*world, 60);
		CHECK(world->GetLinearVelocity(FirstId).y == doctest::Approx(5.0f * entry.Restitution).epsilon(Tolerance));
	}
}

TEST_CASE("PhysicsScene: each collider of a body brings its own surface to its contacts and raycasts")
{
	Testing::PhysicsSystemScope physics;
	float const angle = glm::radians(30.0f);
	glm::quat const tilt = glm::angleAxis(angle, glm::vec3(0.0f, 0.0f, 1.0f));
	glm::vec3 const normal = tilt * glm::vec3(0.0f, 1.0f, 0.0f);
	glm::vec3 const downhill = tilt * glm::vec3(-1.0f, 0.0f, 0.0f);
	glm::vec3 const across(0.0f, 0.0f, 1.0f);
	Scope<PhysicsScene> world = CreateWorld();
	REQUIRE(world
	            ->AddBody(MakeStatic(GroundId, glm::vec3(0.0f), tilt,
	                                 Box({10.0f, 0.5f, 10.0f}, MakeSurface(SlopeMaterial, 1.0f, PhysicsMaterialCombine::GeometricMean))))
	            .IsOk());
	// Two bodies of two colliders each, the lower touching the slope and the upper a meter above it: the first body's
	// lower collider is slippery (it slides), the second body's is grippy (it holds), whichever collider comes first.
	ColliderSurface const slippery = MakeSurface(SlipperyMaterial, 0.1f, PhysicsMaterialCombine::GeometricMean);
	ColliderSurface const grippy = MakeSurface(GrippyMaterial, 0.9f, PhysicsMaterialCombine::GeometricMean);
	glm::vec3 const up(0.0f, 1.0f, 0.0f);
	REQUIRE(world
	            ->AddBody(MakeSlider(FirstId, normal * 0.75f - across * 3.0f, tilt,
	                                 {Box(glm::vec3(0.25f), slippery), Box(glm::vec3(0.25f), grippy, up)}))
	            .IsOk());
	REQUIRE(world
	            ->AddBody(MakeSlider(SecondId, normal * 0.75f + across * 3.0f, tilt,
	                                 {Box(glm::vec3(0.25f), slippery, up), Box(glm::vec3(0.25f), grippy)}))
	            .IsOk());

	// Raycasts name the collider they hit: from above the upper one, from the side at the slope the lower one.
	auto const materialHit = [&world](glm::vec3 const& origin, glm::vec3 const& direction)
	{
		std::optional<RaycastHit> const hit = world->Raycast(origin, direction, 20.0f);
		REQUIRE(hit);
		return hit->Material;
	};
	glm::vec3 const first = normal * 0.75f - across * 3.0f;
	glm::vec3 const second = normal * 0.75f + across * 3.0f;
	CHECK(materialHit(first + normal * 5.0f, -normal) == GrippyMaterial);
	CHECK(materialHit(second + normal * 5.0f, -normal) == SlipperyMaterial);
	CHECK(materialHit(first - downhill * 5.0f, downhill) == SlipperyMaterial);
	CHECK(materialHit(second - downhill * 5.0f, downhill) == GrippyMaterial);

	Simulate(*world, 30);
	float const before = glm::dot(world->GetLinearVelocity(SecondId), downhill);
	float const acceleration = MeasureAcceleration(*world, FirstId, downhill, 60);
	CHECK(acceleration == doctest::Approx(Gravity * (std::sin(angle) - std::sqrt(0.1f) * std::cos(angle))).epsilon(Tolerance));
	CHECK(std::abs(before) < 0.01f);
	CHECK(std::abs(glm::dot(world->GetLinearVelocity(SecondId), downhill)) < 0.01f);
}

TEST_CASE("PhysicsScene: a triangle mesh's submeshes have their own surfaces for raycasts and sliding bodies")
{
	Testing::PhysicsSystemScope physics;
	// Three strips: the first two have surfaces of their own, the third (past the list) the collider's.
	Ref<MeshSource> const strips = MakeStrips(3, -6.0f);
	ColliderDesc mesh;
	mesh.Shape = ColliderShape::TriangleMesh;
	mesh.Mesh = strips;
	mesh.Surface = MakeSurface(ColliderMaterial, 0.5f, PhysicsMaterialCombine::GeometricMean);
	mesh.SubmeshSurfaces = {MakeSurface(SlipperyMaterial, 0.1f, PhysicsMaterialCombine::GeometricMean),
	                        MakeSurface(GrippyMaterial, 0.9f, PhysicsMaterialCombine::GeometricMean)};

	Scope<PhysicsScene> world = CreateWorld();
	REQUIRE(world->AddBody(MakeStatic(GroundId, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), mesh)).IsOk());
	CHECK(MaterialBelow(*world, -4.0f, 0.0f) == SlipperyMaterial);
	CHECK(MaterialBelow(*world, -4.0f, 9.0f) == SlipperyMaterial);
	CHECK(MaterialBelow(*world, 0.0f, -5.0f) == GrippyMaterial);
	CHECK(MaterialBelow(*world, 4.0f, 3.0f) == ColliderMaterial);

	// A box slides along each strip from 6 m/s, slowed by g times the combined friction (sqrt(mu * 1)).
	glm::quat const upright(1.0f, 0.0f, 0.0f, 0.0f);
	ColliderDesc const box = Box(glm::vec3(0.25f), MakeSurface(BoxMaterial, 1.0f, PhysicsMaterialCombine::GeometricMean));
	glm::vec3 const along(0.0f, 0.0f, 1.0f);
	REQUIRE(world->AddBody(MakeSlider(FirstId, {-4.0f, 0.25f, -8.0f}, upright, {box}, along * 6.0f)).IsOk());
	REQUIRE(world->AddBody(MakeSlider(SecondId, {0.0f, 0.25f, -8.0f}, upright, {box}, along * 6.0f)).IsOk());
	REQUIRE(world->AddBody(MakeSlider(ThirdId, {4.0f, 0.25f, -8.0f}, upright, {box}, along * 6.0f)).IsOk());
	Simulate(*world, 5);
	float const slippery = glm::dot(world->GetLinearVelocity(FirstId), along);
	float const grippy = glm::dot(world->GetLinearVelocity(SecondId), along);
	float const collider = glm::dot(world->GetLinearVelocity(ThirdId), along);
	Simulate(*world, 15);
	float const seconds = 15.0f * Step;
	CHECK((glm::dot(world->GetLinearVelocity(FirstId), along) - slippery) / seconds ==
	      doctest::Approx(-Gravity * std::sqrt(0.1f)).epsilon(Tolerance));
	CHECK((glm::dot(world->GetLinearVelocity(SecondId), along) - grippy) / seconds ==
	      doctest::Approx(-Gravity * std::sqrt(0.9f)).epsilon(Tolerance));
	CHECK((glm::dot(world->GetLinearVelocity(ThirdId), along) - collider) / seconds ==
	      doctest::Approx(-Gravity * std::sqrt(0.5f)).epsilon(Tolerance));

	// A box that slides across from the slippery strip onto the grippy one slows faster once it is on it.
	REQUIRE(world->AddBody(MakeSlider(UUID(105), {-4.5f, 0.25f, 5.0f}, upright, {box}, {6.0f, 0.0f, 0.0f})).IsOk());
	glm::vec3 const right(1.0f, 0.0f, 0.0f);
	Simulate(*world, 1);
	float const onSlippery = MeasureAcceleration(*world, UUID(105), right, 5);
	// While the box straddles the strips' edge, Jolt's manifold reduction keeps one sub-shape of the strips for the whole
	// contact (here the slippery one, which the contact started on); once wholly on the grippy strip it slows by its value.
	Simulate(*world, 30);
	// Wholly on the grippy strip, and still sliding.
	REQUIRE(world->GetBodyTransform(UUID(105))->Position.x > -1.7f);
	float const onGrippy = MeasureAcceleration(*world, UUID(105), right, 3);
	REQUIRE(glm::dot(world->GetLinearVelocity(UUID(105)), right) > 0.5f);
	CHECK(onSlippery == doctest::Approx(-Gravity * std::sqrt(0.1f)).epsilon(Tolerance));
	CHECK(onGrippy == doctest::Approx(-Gravity * std::sqrt(0.9f)).epsilon(Tolerance));

	// A trigger with submesh surfaces stays a trigger: rays pass through it to the ground.
	ColliderDesc trigger = mesh;
	trigger.IsTrigger = true;
	REQUIRE(world->AddBody(MakeStatic(UUID(106), {0.0f, 5.0f, 0.0f}, upright, trigger)).IsOk());
	CHECK(MaterialBelow(*world, -4.0f, 0.0f) == SlipperyMaterial);
	std::optional<RaycastHit> const throughTrigger = world->Raycast({0.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
	REQUIRE(throughTrigger);
	CHECK(throughTrigger->Entity == GroundId);
}

TEST_CASE("PhysicsScene: bodies slide and roll across the edges where a triangle mesh's submeshes meet without hitting them")
{
	Testing::PhysicsSystemScope physics;
	// Four frictionless strips, each a submesh with vertices of its own: where two meet, their positions repeat. Built over
	// the mesh's distinct positions, the shape shares the edges there, so Jolt sees flat ground. With the positions apart,
	// each edge was an open one: a ball crossing at 4 m/s leapt at 0.34 m/s and lost an eighth of its speed, and a box
	// sliding corner first at 8 m/s rose 0.32 m.
	ColliderDesc mesh;
	mesh.Shape = ColliderShape::TriangleMesh;
	mesh.Mesh = MakeStrips(4, -8.0f);
	mesh.Surface = MakeSurface(ColliderMaterial, 0.0f, PhysicsMaterialCombine::Minimum);
	mesh.SubmeshSurfaces = {MakeSurface(SlipperyMaterial, 0.0f, PhysicsMaterialCombine::Minimum),
	                        MakeSurface(GrippyMaterial, 0.0f, PhysicsMaterialCombine::Minimum)};
	ColliderDesc ball;
	ball.Shape = ColliderShape::Sphere;
	ball.Radius = 0.25f;
	ball.Surface = MakeSurface(BoxMaterial, 1.0f, PhysicsMaterialCombine::GeometricMean);
	ColliderDesc const box = Box(glm::vec3(0.25f), MakeSurface(BoxMaterial, 1.0f, PhysicsMaterialCombine::GeometricMean));
	glm::quat const upright(1.0f, 0.0f, 0.0f, 0.0f);
	glm::quat const cornerFirst = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));

	struct Crossing
	{
		char const* Name;
		ColliderDesc Collider;
		glm::quat Rotation;
		bool Rolls;
	};
	for (float const speed : {4.0f, 8.0f})
	{
		for (Crossing const& crossing : {Crossing{"a sliding ball", ball, upright, false}, Crossing{"a rolling ball", ball, upright, true},
		                                 Crossing{"a box sliding corner first", box, cornerFirst, false}})
		{
			CAPTURE(crossing.Name);
			CAPTURE(speed);
			Scope<PhysicsScene> world = CreateWorld();
			REQUIRE(world->AddBody(MakeStatic(GroundId, glm::vec3(0.0f), upright, mesh)).IsOk());
			BodyDesc body = MakeSlider(FirstId, {-7.5f, 0.25f, 0.0f}, crossing.Rotation, {crossing.Collider}, {speed, 0.0f, 0.0f});
			body.LockRotation = glm::bvec3(!crossing.Rolls);
			REQUIRE(world->AddBody(body).IsOk());
			// Across the edges at x = -4, 0 and 4.
			float rise = 0.0f;
			float fastestUp = 0.0f;
			float slowest = speed;
			for (int step = 0; step < 600 && world->GetBodyTransform(FirstId)->Position.x < 6.0f; step++)
			{
				world->Step(Step);
				rise = std::max(rise, world->GetBodyTransform(FirstId)->Position.y - 0.25f);
				fastestUp = std::max(fastestUp, std::abs(world->GetLinearVelocity(FirstId).y));
				slowest = std::min(slowest, world->GetLinearVelocity(FirstId).x);
			}
			CHECK(world->GetBodyTransform(FirstId)->Position.x >= 6.0f);
			CHECK(rise < 0.005f);
			CHECK(fastestUp < 0.01f);
			CHECK(slowest == doctest::Approx(speed).epsilon(0.001));
		}
	}
}

TEST_CASE("PhysicsScene: submesh surfaces beyond the mesh are ignored, beyond the limit refused, and hulls have one surface")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	glm::quat const upright(1.0f, 0.0f, 0.0f, 0.0f);
	ColliderDesc mesh;
	mesh.Shape = ColliderShape::TriangleMesh;
	mesh.Mesh = MakeStrips(2, -4.0f);
	mesh.Surface = MakeSurface(ColliderMaterial, 0.5f, PhysicsMaterialCombine::GeometricMean);
	// More surfaces than submeshes: those past the mesh have nothing to cover.
	mesh.SubmeshSurfaces.assign(MaxMeshColliderMaterials, MakeSurface(SlipperyMaterial, 0.1f, PhysicsMaterialCombine::GeometricMean));
	mesh.SubmeshSurfaces[1] = MakeSurface(GrippyMaterial, 0.9f, PhysicsMaterialCombine::GeometricMean);
	REQUIRE(world->AddBody(MakeStatic(GroundId, glm::vec3(0.0f), upright, mesh)).IsOk());
	CHECK(MaterialBelow(*world, -2.0f, 0.0f) == SlipperyMaterial);
	CHECK(MaterialBelow(*world, 2.0f, 0.0f) == GrippyMaterial);
	CHECK(world->SetMaterialValues(SlipperyMaterial, PhysicsMaterial{}) == 1);

	// Jolt's mesh shape holds 32 materials: the collider's and MaxMeshColliderMaterials more.
	ColliderDesc tooMany = mesh;
	tooMany.SubmeshSurfaces.push_back(tooMany.Surface);
	Result<void> const refused = world->AddBody(MakeStatic(FirstId, {20.0f, 0.0f, 0.0f}, upright, tooMany));
	REQUIRE(refused.IsError());
	CHECK(refused.GetError().find("submesh surfaces") != std::string::npos);
	CHECK_FALSE(world->HasBody(FirstId));

	// A dynamic body's triangle mesh is its convex hull, with the collider's surface alone.
	ColliderDesc cube = mesh;
	cube.Mesh = MeshFactory::CreateCube(AssetHandle());
	cube.SubmeshSurfaces.resize(1);
	BodyDesc hull = MakeSlider(SecondId, {0.0f, 20.0f, 0.0f}, upright, {cube});
	hull.GravityFactor = 0.0f;
	REQUIRE(world->AddBody(hull).IsOk());
	std::optional<RaycastHit> const onHull = world->Raycast({0.0f, 30.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
	REQUIRE(onHull);
	CHECK(onHull->Entity == SecondId);
	CHECK(onHull->Material == ColliderMaterial);
	// Kinematic, the same collider keeps its triangles and the submesh's surface.
	world->RemoveBody(SecondId);
	hull.Type = RigidBodyType::Kinematic;
	REQUIRE(world->AddBody(hull).IsOk());
	std::optional<RaycastHit> const onTriangles = world->Raycast({0.0f, 30.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
	REQUIRE(onTriangles);
	CHECK(onTriangles->Entity == SecondId);
	CHECK(onTriangles->Material == SlipperyMaterial);

	// Colliders without a material report none.
	ColliderDesc plain;
	plain.Shape = ColliderShape::Sphere;
	REQUIRE(world->AddBody(MakeStatic(ThirdId, {40.0f, 0.0f, 0.0f}, upright, plain)).IsOk());
	std::optional<RaycastHit> const onPlain = world->Raycast({40.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
	REQUIRE(onPlain);
	CHECK_FALSE(onPlain->Material.IsValid());
}

TEST_CASE("PhysicsScene: a material's new values reach the surfaces that name it from the next step, without rebuilding them")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	glm::quat const upright(1.0f, 0.0f, 0.0f, 0.0f);
	REQUIRE(world
	            ->AddBody(MakeStatic(GroundId, {0.0f, -0.5f, 0.0f}, upright,
	                                 Box({20.0f, 0.5f, 20.0f}, MakeSurface(SlopeMaterial, 0.2f, PhysicsMaterialCombine::GeometricMean))))
	            .IsOk());
	glm::vec3 const along(1.0f, 0.0f, 0.0f);
	REQUIRE(world
	            ->AddBody(MakeSlider(FirstId, {-15.0f, 0.25f, 0.0f}, upright,
	                                 {Box(glm::vec3(0.25f), MakeSurface(AssetHandle(), 1.0f, PhysicsMaterialCombine::GeometricMean))},
	                                 along * 12.0f))
	            .IsOk());
	// A body made and removed again leaves no surface behind that the edit would count.
	REQUIRE(world->AddBody(MakeStatic(SecondId, {0.0f, 10.0f, 0.0f}, upright, Box(glm::vec3(1.0f), MakeSurface(SlopeMaterial, 0.2f, {}))))
	            .IsOk());
	world->RemoveBody(SecondId);
	Simulate(*world, 5);
	float const before = MeasureAcceleration(*world, FirstId, along, 20);
	CHECK(before == doctest::Approx(-Gravity * std::sqrt(0.2f)).epsilon(Tolerance));

	uint64_t const structure = world->GetStructureVersion();
	glm::vec3 const velocity = world->GetLinearVelocity(FirstId);
	PhysicsMaterial grippier;
	grippier.Friction = 0.8f;
	CHECK(world->SetMaterialValues(SlopeMaterial, grippier) == 1);
	CHECK(world->SetMaterialValues(AssetHandle(), grippier) == 0);
	CHECK(world->SetMaterialValues(BoxMaterial, grippier) == 0);
	CHECK(world->GetStructureVersion() == structure);
	CHECK(world->GetLinearVelocity(FirstId) == velocity);
	// The contact persists, and its very next step slows the box by the new friction.
	CHECK(MeasureAcceleration(*world, FirstId, along, 1) == doctest::Approx(-Gravity * std::sqrt(0.8f)).epsilon(Tolerance));
	CHECK(MeasureAcceleration(*world, FirstId, along, 10) == doctest::Approx(-Gravity * std::sqrt(0.8f)).epsilon(Tolerance));
	CHECK(world->TakeContactEvents().size() == 1);
}

namespace
{
	// Steps until the body sleeps (at most ten seconds).
	void SimulateUntilAsleep(PhysicsScene& world, UUID entity)
	{
		for (int step = 0; step < 600 && !world.IsSleeping(entity); step++)
		{
			world.Step(Step);
		}
		REQUIRE(world.IsSleeping(entity));
	}

	// A 25 degree slope (tan 25 = 0.466) with a box resting on it, both gripping at friction 1, the box allowed to sleep; and
	// a box asleep on ground of its own, away from the slope.
	Scope<PhysicsScene> MakeRestingWorld(glm::vec3& downhill)
	{
		float const angle = glm::radians(25.0f);
		glm::quat const tilt = glm::angleAxis(angle, glm::vec3(0.0f, 0.0f, 1.0f));
		glm::vec3 const normal = tilt * glm::vec3(0.0f, 1.0f, 0.0f);
		downhill = tilt * glm::vec3(-1.0f, 0.0f, 0.0f);
		glm::quat const upright(1.0f, 0.0f, 0.0f, 0.0f);
		Scope<PhysicsScene> world = CreateWorld();
		REQUIRE(world
		            ->AddBody(MakeStatic(GroundId, glm::vec3(0.0f), tilt,
		                                 Box({5.0f, 0.5f, 5.0f}, MakeSurface(SlopeMaterial, 1.0f, PhysicsMaterialCombine::GeometricMean))))
		            .IsOk());
		REQUIRE(world
		            ->AddBody(MakeStatic(SecondId, {40.0f, -0.5f, 0.0f}, upright,
		                                 Box({5.0f, 0.5f, 5.0f}, MakeSurface(AssetHandle(), 1.0f, PhysicsMaterialCombine::GeometricMean))))
		            .IsOk());
		BodyDesc resting = MakeSlider(FirstId, normal * 0.75f, tilt,
		                              {Box(glm::vec3(0.25f), MakeSurface(BoxMaterial, 1.0f, PhysicsMaterialCombine::GeometricMean))});
		resting.AllowSleep = true;
		REQUIRE(world->AddBody(resting).IsOk());
		BodyDesc away = MakeSlider(ThirdId, {40.0f, 0.25f, 0.0f}, upright,
		                           {Box(glm::vec3(0.25f), MakeSurface(BoxMaterial, 1.0f, PhysicsMaterialCombine::GeometricMean))});
		away.AllowSleep = true;
		REQUIRE(world->AddBody(away).IsOk());
		SimulateUntilAsleep(*world, FirstId);
		SimulateUntilAsleep(*world, ThirdId);
		return world;
	}

	PhysicsMaterial Slippery()
	{
		PhysicsMaterial values;
		values.Friction = 0.0f;
		return values;
	}
}

TEST_CASE("PhysicsScene: a body asleep on a slope wakes and slides when its own material, or the slope's, loses its grip")
{
	Testing::PhysicsSystemScope physics;
	glm::vec3 downhill(0.0f);
	SUBCASE("its own material")
	{
		Scope<PhysicsScene> world = MakeRestingWorld(downhill);
		glm::vec3 const start = world->GetBodyTransform(FirstId)->Position;
		// The same values again change nothing, and wake nothing.
		PhysicsMaterial gripping;
		gripping.Friction = 1.0f;
		CHECK(world->SetMaterialValues(BoxMaterial, gripping) == 2);
		CHECK(world->IsSleeping(FirstId));
		CHECK(world->IsSleeping(ThirdId));
		// Without friction it slides at g sin 25 from the next step; the box elsewhere with the same material wakes too (and,
		// on flat ground, stays where it is).
		CHECK(world->SetMaterialValues(BoxMaterial, Slippery()) == 2);
		CHECK_FALSE(world->IsSleeping(FirstId));
		CHECK_FALSE(world->IsSleeping(ThirdId));
		Simulate(*world, 2);
		CHECK(MeasureAcceleration(*world, FirstId, downhill, 30) ==
		      doctest::Approx(Gravity * std::sin(glm::radians(25.0f))).epsilon(Tolerance));
		CHECK(glm::dot(world->GetBodyTransform(FirstId)->Position - start, downhill) > 0.5f);
	}
	SUBCASE("the slope's material")
	{
		Scope<PhysicsScene> world = MakeRestingWorld(downhill);
		// A static body wakes what rests on it; the box away from it sleeps on.
		CHECK(world->SetMaterialValues(SlopeMaterial, Slippery()) == 1);
		CHECK_FALSE(world->IsSleeping(FirstId));
		CHECK(world->IsSleeping(ThirdId));
		Simulate(*world, 2);
		CHECK(MeasureAcceleration(*world, FirstId, downhill, 30) ==
		      doctest::Approx(Gravity * std::sin(glm::radians(25.0f))).epsilon(Tolerance));
		CHECK(world->IsSleeping(ThirdId));
	}
}

TEST_CASE("PhysicsScene: a character's inner body meets the surface of the collider it touches, not the body's first")
{
	Testing::PhysicsSystemScope physics;
	glm::quat const upright(1.0f, 0.0f, 0.0f, 0.0f);
	// A ball of two colliders falls without gravity onto a standing character's head: its lower collider touches the
	// character's inner body, the upper one (first in the list) touches nothing. The inner body brings Jolt's default
	// restitution (0) at Maximum, so the bounce is the lower collider's.
	struct Fall
	{
		glm::vec3 Position;
		glm::vec3 Velocity;
	};
	auto const fall = [&upright](float firstRestitution, float touchingRestitution)
	{
		Scope<PhysicsScene> world = CreateWorld();
		REQUIRE(
			world
				->AddBody(MakeStatic(GroundId, {0.0f, -0.5f, 0.0f}, upright,
		                             Box({10.0f, 0.5f, 10.0f}, MakeSurface(AssetHandle(), 0.6f, PhysicsMaterialCombine::GeometricMean))))
				.IsOk());
		CharacterDesc character;
		character.Entity = SecondId;
		character.Position = {0.0f, 1.0f, 0.0f};
		// It pushes nothing: the ball's bounce is the contact's alone.
		character.PushForce = 0.0f;
		REQUIRE(world->AddCharacter(character).IsOk());
		Simulate(*world, 10);
		ColliderDesc upper;
		upper.Shape = ColliderShape::Sphere;
		upper.Radius = 0.3f;
		upper.Offset = {0.0f, 2.0f, 0.0f};
		upper.Surface = MakeSurface(AssetHandle(), 0.6f, PhysicsMaterialCombine::GeometricMean, firstRestitution);
		ColliderDesc lower = upper;
		lower.Offset = glm::vec3(0.0f);
		lower.Surface = MakeSurface(AssetHandle(), 0.6f, PhysicsMaterialCombine::GeometricMean, touchingRestitution);
		BodyDesc ball = MakeSlider(FirstId, {0.0f, 4.0f, 0.0f}, upright, {upper, lower}, {0.0f, -5.0f, 0.0f});
		ball.GravityFactor = 0.0f;
		REQUIRE(world->AddBody(ball).IsOk());
		Simulate(*world, 60);
		return Fall{world->GetBodyTransform(FirstId)->Position, world->GetLinearVelocity(FirstId)};
	};
	Fall const bouncing = fall(0.0f, 0.8f);
	// The first collider's surface plays no part, to the bit.
	Fall const other = fall(1.0f, 0.8f);
	CHECK(other.Position == bouncing.Position);
	CHECK(other.Velocity == bouncing.Velocity);
	CHECK(bouncing.Velocity.y == doctest::Approx(5.0f * 0.8f).epsilon(0.05));
	// The touching collider's does: without restitution the ball does not bounce back.
	Fall const dead = fall(1.0f, 0.0f);
	CHECK(dead.Velocity.y == doctest::Approx(0.0f).epsilon(0.05));
}
