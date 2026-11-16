#include "Physics/PhysicsTestUtilities.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/MeshFactory.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Core/Hash.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float Step = 1.0f / 60.0f;
	// Jolt's GJK and EPA stop within 1e-4 m (cDefaultCollisionTolerance, cDefaultPenetrationTolerance): closed forms hold to
	// this.
	constexpr float Tolerance = 1e-3f;

	glm::quat const Upright(1.0f, 0.0f, 0.0f, 0.0f);
	glm::vec3 const Down(0.0f, -1.0f, 0.0f);
	glm::vec3 const Up(0.0f, 1.0f, 0.0f);
	glm::vec3 const Along(1.0f, 0.0f, 0.0f);

	UUID const GroundId(100);

	Scope<PhysicsScene> CreateWorld(PhysicsWorldSettings const& settings = {})
	{
		Result<Scope<PhysicsScene>> world = PhysicsScene::Create(settings);
		REQUIRE(world.IsOk());
		return world.TakeValue();
	}

	ColliderDesc Box(glm::vec3 const& halfExtents, AssetHandle material = {}, glm::vec3 const& offset = glm::vec3(0.0f))
	{
		ColliderDesc collider;
		collider.Shape = ColliderShape::Box;
		collider.HalfExtents = halfExtents;
		collider.Offset = offset;
		collider.Surface.Material = material;
		return collider;
	}

	ColliderDesc Trigger(glm::vec3 const& halfExtents, glm::vec3 const& offset = glm::vec3(0.0f))
	{
		ColliderDesc collider = Box(halfExtents, {}, offset);
		collider.IsTrigger = true;
		return collider;
	}

	ColliderDesc Sphere(float radius, AssetHandle material = {})
	{
		ColliderDesc collider;
		collider.Shape = ColliderShape::Sphere;
		collider.Radius = radius;
		collider.Surface.Material = material;
		return collider;
	}

	ColliderDesc Capsule(float radius, float halfHeight, AssetHandle material = {})
	{
		ColliderDesc collider;
		collider.Shape = ColliderShape::Capsule;
		collider.Radius = radius;
		collider.HalfHeight = halfHeight;
		collider.Surface.Material = material;
		return collider;
	}

	BodyDesc MakeBody(UUID entity, glm::vec3 const& position, std::vector<ColliderDesc> colliders,
	                  RigidBodyType type = RigidBodyType::Static, uint32_t layer = 0)
	{
		BodyDesc desc;
		desc.Entity = entity;
		desc.Type = type;
		desc.Position = position;
		desc.Colliders = std::move(colliders);
		desc.Layer = layer;
		return desc;
	}

	// A 20 x 1 x 20 static floor whose top is at y = 0.
	void AddGround(PhysicsScene& world)
	{
		REQUIRE(world.AddBody(MakeBody(GroundId, {0.0f, -0.5f, 0.0f}, {Box({10.0f, 0.5f, 10.0f})})).IsOk());
	}

	void CheckNear(glm::vec3 const& actual, glm::vec3 const& expected, float tolerance = Tolerance)
	{
		CAPTURE(actual);
		CAPTURE(expected);
		CHECK(glm::all(glm::lessThanEqual(glm::abs(actual - expected), glm::vec3(tolerance))));
	}

	std::vector<UUID> EntitiesOf(std::vector<RaycastHit> const& hits)
	{
		std::vector<UUID> entities;
		for (RaycastHit const& hit : hits)
		{
			entities.push_back(hit.Entity);
		}
		return entities;
	}

	std::vector<UUID> Ids(std::initializer_list<uint64_t> values)
	{
		std::vector<UUID> ids;
		for (uint64_t const value : values)
		{
			ids.emplace_back(value);
		}
		return ids;
	}

	// Two flat quads at y = 0 side by side, one submesh each: x from -1 to 0 and from 0 to 1, z from -1 to 1.
	Ref<MeshSource> MakeTwoQuads()
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<Submesh> submeshes;
		for (float const left : {-1.0f, 0.0f})
		{
			Submesh submesh;
			submesh.BaseVertex = static_cast<uint32_t>(vertices.size());
			submesh.VertexCount = 4;
			submesh.BaseIndex = static_cast<uint32_t>(indices.size());
			submesh.IndexCount = 6;
			for (glm::vec3 const& corner : {glm::vec3(left, 0.0f, -1.0f), glm::vec3(left, 0.0f, 1.0f), glm::vec3(left + 1.0f, 0.0f, 1.0f),
			                                glm::vec3(left + 1.0f, 0.0f, -1.0f)})
			{
				Vertex vertex;
				vertex.Position = corner;
				vertex.Normal = Up;
				vertices.push_back(vertex);
			}
			// Counter-clockwise seen from above: the front faces up.
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
}

TEST_CASE("PhysicsScene: sphere, box and capsule casts touch where their closed forms say")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	AddGround(*world);
	UUID const ballId(201);
	UUID const blockId(202);
	REQUIRE(world->AddBody(MakeBody(ballId, {0.0f, 3.0f, -6.0f}, {Sphere(1.0f)})).IsOk());
	REQUIRE(world->AddBody(MakeBody(blockId, {6.0f, 1.0f, 6.0f}, {Box(glm::vec3(0.5f))})).IsOk());

	SUBCASE("a sphere lands on a face")
	{
		std::optional<ShapeCastHit> const hit =
			world->ShapeCast(PhysicsQueryShape::MakeSphere(0.5f), {0.0f, 5.0f, 0.0f}, Upright, Down, 100.0f);
		REQUIRE(hit);
		CHECK(hit->Entity == GroundId);
		CHECK(hit->Distance == doctest::Approx(4.5f).epsilon(Tolerance));
		CheckNear(hit->Point, glm::vec3(0.0f));
		CheckNear(hit->Normal, Up);
		CHECK_FALSE(hit->Material.IsValid());
		// The center is then at origin + direction * distance; the direction need not be normalized.
		std::optional<ShapeCastHit> const scaled =
			world->ShapeCast(PhysicsQueryShape::MakeSphere(0.5f), {0.0f, 5.0f, 0.0f}, Upright, Down * 7.0f, 100.0f);
		REQUIRE(scaled);
		CHECK(scaled->Distance == doctest::Approx(4.5f).epsilon(Tolerance));
		// Too short to reach it.
		CHECK_FALSE(world->ShapeCast(PhysicsQueryShape::MakeSphere(0.5f), {0.0f, 5.0f, 0.0f}, Upright, Down, 4.4f));
	}

	SUBCASE("a sphere meets another sphere and an edge along the line between the centers")
	{
		// From 0.9 m above the other's center along x: the centers are 1.5 m apart when x is 1.2 m short of it.
		std::optional<ShapeCastHit> const side =
			world->ShapeCast(PhysicsQueryShape::MakeSphere(0.5f), {-5.0f, 3.9f, -6.0f}, Upright, Along, 100.0f);
		REQUIRE(side);
		CHECK(side->Entity == ballId);
		CHECK(side->Distance == doctest::Approx(3.8f).epsilon(Tolerance));
		CheckNear(side->Normal, {-0.8f, 0.6f, 0.0f});
		CheckNear(side->Point, {-0.8f, 3.6f, -6.0f});

		// Down onto the block's top edge (x = 6.5, y = 1.5) from 0.3 m beyond it: it touches 0.4 m above the edge's height.
		std::optional<ShapeCastHit> const edge =
			world->ShapeCast(PhysicsQueryShape::MakeSphere(0.5f), {6.8f, 10.0f, 6.0f}, Upright, Down, 100.0f);
		REQUIRE(edge);
		CHECK(edge->Entity == blockId);
		CHECK(edge->Distance == doctest::Approx(10.0f - 1.9f).epsilon(Tolerance));
		CheckNear(edge->Normal, {0.6f, 0.8f, 0.0f});
		CheckNear(edge->Point, {6.5f, 1.5f, 6.0f});
	}

	SUBCASE("a box lands flat, and on an edge when turned")
	{
		PhysicsQueryShape const box = PhysicsQueryShape::MakeBox({0.5f, 0.25f, 0.5f});
		std::optional<ShapeCastHit> const flat = world->ShapeCast(box, {2.0f, 5.0f, 2.0f}, Upright, Down, 100.0f);
		REQUIRE(flat);
		CHECK(flat->Entity == GroundId);
		CHECK(flat->Distance == doctest::Approx(4.75f).epsilon(Tolerance));
		CheckNear(flat->Normal, Up);
		CHECK(flat->Point.y == doctest::Approx(0.0f).epsilon(Tolerance));
		CHECK(std::abs(flat->Point.x - 2.0f) <= 0.5f + Tolerance);
		CHECK(std::abs(flat->Point.z - 2.0f) <= 0.5f + Tolerance);

		// Turned 45 degrees about z, its lowest edge is (0.5 + 0.25) sin 45 below its center, 0.25 sin 45 to the left: the
		// box is sharp, not rounded by a convex radius.
		glm::quat const turned = glm::angleAxis(glm::quarter_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
		float const half = std::sqrt(0.5f);
		std::optional<ShapeCastHit> const edge = world->ShapeCast(box, {2.0f, 5.0f, 2.0f}, turned, Down, 100.0f);
		REQUIRE(edge);
		CHECK(edge->Distance == doctest::Approx(5.0f - 0.75f * half).epsilon(Tolerance));
		CheckNear(edge->Normal, Up);
		CHECK(edge->Point.x == doctest::Approx(2.0f - 0.25f * half).epsilon(Tolerance));
		CHECK(edge->Point.y == doctest::Approx(0.0f).epsilon(Tolerance));

		// A zero rotation is the identity.
		std::optional<ShapeCastHit> const zero = world->ShapeCast(box, {2.0f, 5.0f, 2.0f}, glm::quat(0.0f, 0.0f, 0.0f, 0.0f), Down, 100.0f);
		REQUIRE(zero);
		CHECK(zero->Distance == flat->Distance);
	}

	SUBCASE("a capsule lands on its cap upright, and on its side lying down")
	{
		PhysicsQueryShape const capsule = PhysicsQueryShape::MakeCapsule(0.3f, 0.5f);
		std::optional<ShapeCastHit> const upright = world->ShapeCast(capsule, {-2.0f, 5.0f, -2.0f}, Upright, Down, 100.0f);
		REQUIRE(upright);
		CHECK(upright->Distance == doctest::Approx(4.2f).epsilon(Tolerance));
		CheckNear(upright->Point, {-2.0f, 0.0f, -2.0f});
		CheckNear(upright->Normal, Up);
		glm::quat const lying = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
		std::optional<ShapeCastHit> const side = world->ShapeCast(capsule, {-2.0f, 5.0f, -2.0f}, lying, Down, 100.0f);
		REQUIRE(side);
		CHECK(side->Distance == doctest::Approx(4.7f).epsilon(Tolerance));
		CheckNear(side->Normal, Up);
		// Shorter than a millimeter, a capsule is a sphere.
		std::optional<ShapeCastHit> const sphere =
			world->ShapeCast(PhysicsQueryShape::MakeCapsule(0.3f, 0.0f), {-2.0f, 5.0f, -2.0f}, lying, Down, 100.0f);
		REQUIRE(sphere);
		CHECK(sphere->Distance == doctest::Approx(4.7f).epsilon(Tolerance));
	}

	SUBCASE("a shape that starts in a collider touches it at distance 0, at the deepest point, whichever way it goes")
	{
		// 0.2 m deep in the ground.
		for (glm::vec3 const& direction : {Down, Up, Along})
		{
			CAPTURE(direction);
			std::optional<ShapeCastHit> const hit =
				world->ShapeCast(PhysicsQueryShape::MakeSphere(0.5f), {-6.0f, 0.3f, 6.0f}, Upright, direction, 10.0f);
			REQUIRE(hit);
			CHECK(hit->Entity == GroundId);
			CHECK(hit->Distance == 0.0f);
			CheckNear(hit->Normal, Up);
			CheckNear(hit->Point, {-6.0f, 0.0f, 6.0f});
		}
		// A box half in the ground, moving away from it.
		std::optional<ShapeCastHit> const box =
			world->ShapeCast(PhysicsQueryShape::MakeBox(glm::vec3(0.5f)), {-6.0f, 0.1f, -6.0f}, Upright, Up, 10.0f);
		REQUIRE(box);
		CHECK(box->Distance == 0.0f);
		CheckNear(box->Normal, Up);
	}
}

TEST_CASE("PhysicsScene: overlaps find every entity a sphere, box or capsule overlaps, by UUID")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	// Made from the largest UUID down, so that the order found is the UUID's, not the order of the bodies.
	REQUIRE(world->AddBody(MakeBody(UUID(305), glm::vec3(0.0f), {Box(glm::vec3(0.5f))})).IsOk());
	REQUIRE(world->AddBody(MakeBody(UUID(304), {1.5f, 0.0f, 0.0f}, {Sphere(0.5f)})).IsOk());
	REQUIRE(world->AddBody(MakeBody(UUID(303), {0.0f, 0.0f, 1.6f}, {Capsule(0.3f, 0.5f)})).IsOk());
	REQUIRE(world->AddBody(MakeBody(UUID(302), {5.0f, 0.0f, 3.0f}, {Box(glm::vec3(0.5f))})).IsOk());
	std::vector<UUID> entities;

	// Reaching the box (it is inside it), the sphere (1.118 m off, within 1.5) and the capsule (1.208 m from its axis, within
	// 1.3), not the far box.
	CHECK(world->Overlap(PhysicsQueryShape::MakeSphere(1.0f), {0.5f, 0.0f, 0.5f}, Upright, entities) == 3);
	CHECK(entities == Ids({303, 304, 305}));

	// A rod along z reaches the far box, and turned along x it does not.
	PhysicsQueryShape const rod = PhysicsQueryShape::MakeBox({0.1f, 0.1f, 2.0f});
	CHECK(world->Overlap(rod, {5.0f, 0.0f, 1.0f}, Upright, entities) == 1);
	CHECK(entities == Ids({302}));
	CHECK(world->Overlap(rod, {5.0f, 0.0f, 1.0f}, glm::angleAxis(glm::half_pi<float>(), Up), entities) == 0);
	CHECK(entities.empty());

	// A capsule lying along x spans the box and the sphere; upright at its center it meets neither.
	PhysicsQueryShape const capsule = PhysicsQueryShape::MakeCapsule(0.2f, 1.0f);
	glm::quat const lying = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
	CHECK(world->Overlap(capsule, {0.75f, 0.0f, 0.0f}, lying, entities) == 2);
	CHECK(entities == Ids({304, 305}));
	CHECK(world->Overlap(capsule, {0.75f, 0.0f, 0.0f}, Upright, entities) == 0);

	// The vector's earlier contents are replaced.
	entities = Ids({1, 2, 3});
	CHECK(world->Overlap(PhysicsQueryShape::MakeSphere(0.1f), {5.0f, 0.0f, 3.0f}, Upright, entities) == 1);
	CHECK(entities == Ids({302}));
}

TEST_CASE("PhysicsScene: RaycastAll reports each entity on the ray once, the nearest first, and Raycast's hit is its first")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	AssetHandle const material(UUID(7001));
	REQUIRE(world->AddBody(MakeBody(UUID(402), {6.0f, 0.0f, 0.0f}, {Box(glm::vec3(0.5f))})).IsOk());
	REQUIRE(world->AddBody(MakeBody(UUID(403), {2.0f, 0.0f, 0.0f}, {Box(glm::vec3(0.5f), material)})).IsOk());
	REQUIRE(world->AddBody(MakeBody(UUID(401), {4.0f, 0.0f, 0.0f}, {Sphere(0.5f)})).IsOk());
	// Two colliders of one entity on the ray: one hit, the nearer.
	REQUIRE(world->AddBody(MakeBody(UUID(404), {8.0f, 0.0f, 0.0f}, {Box(glm::vec3(0.5f)), Box(glm::vec3(0.25f), {}, {1.5f, 0.0f, 0.0f})}))
	            .IsOk());

	std::vector<RaycastHit> hits;
	REQUIRE(world->RaycastAll(glm::vec3(0.0f), Along * 3.0f, 100.0f, hits) == 4);
	CHECK(EntitiesOf(hits) == Ids({403, 401, 402, 404}));
	float const distances[] = {1.5f, 3.5f, 5.5f, 7.5f};
	for (size_t i = 0; i < hits.size(); i++)
	{
		CAPTURE(i);
		CHECK(hits[i].Distance == doctest::Approx(distances[i]).epsilon(Tolerance));
		CheckNear(hits[i].Point, Along * distances[i]);
		CheckNear(hits[i].Normal, -Along);
	}
	CHECK(hits[0].Material == material);

	std::optional<RaycastHit> const first = world->Raycast(glm::vec3(0.0f), Along, 100.0f);
	REQUIRE(first);
	CHECK(first->Entity == hits[0].Entity);
	CHECK(first->Point == hits[0].Point);
	CHECK(first->Normal == hits[0].Normal);
	CHECK(first->Distance == hits[0].Distance);
	CHECK(first->Material == hits[0].Material);

	// A shorter ray, and one starting inside the first box, which it passes out of.
	CHECK(world->RaycastAll(glm::vec3(0.0f), Along, 4.0f, hits) == 2);
	CHECK(world->RaycastAll({2.0f, 0.0f, 0.0f}, Along, 100.0f, hits) == 3);
	CHECK(EntitiesOf(hits) == Ids({401, 402, 404}));

	// A triangle mesh of two submeshes: each part's hit carries its submesh's material. A ray down the edge the two parts
	// share hits a triangle of each at the same distance, and the entity once, at the same triangle on every call. The
	// triangles face up, so a ray from below passes through them.
	AssetHandle const left(UUID(7002));
	AssetHandle const right(UUID(7003));
	ColliderDesc quads;
	quads.Shape = ColliderShape::TriangleMesh;
	quads.Mesh = MakeTwoQuads();
	quads.SubmeshSurfaces.resize(2);
	quads.SubmeshSurfaces[0].Material = left;
	quads.SubmeshSurfaces[1].Material = right;
	REQUIRE(world->AddBody(MakeBody(UUID(405), {0.0f, 0.0f, 20.0f}, {quads})).IsOk());
	REQUIRE(world->RaycastAll({0.5f, 5.0f, 20.0f}, Down, 100.0f, hits) == 1);
	CHECK(hits[0].Entity == UUID(405));
	CHECK(hits[0].Material == right);
	REQUIRE(world->RaycastAll({-0.5f, 5.0f, 20.0f}, Down, 100.0f, hits) == 1);
	CHECK(hits[0].Material == left);
	REQUIRE(world->RaycastAll({0.0f, 5.0f, 20.3f}, Down, 100.0f, hits) == 1);
	AssetHandle const onEdge = hits[0].Material;
	CHECK((onEdge == left || onEdge == right));
	for (int call = 0; call < 3; call++)
	{
		REQUIRE(world->RaycastAll({0.0f, 5.0f, 20.3f}, Down, 100.0f, hits) == 1);
		CHECK(hits[0].Material == onEdge);
	}
	CHECK(world->RaycastAll({-0.5f, -5.0f, 20.0f}, Up, 100.0f, hits) == 0);
}

TEST_CASE("PhysicsScene: hits at equal distances are ordered by entity UUID, whatever order the bodies were made in")
{
	Testing::PhysicsSystemScope physics;
	// Two identical boxes in the same place hit at exactly the same distance, and a third behind them; a trigger of a smaller
	// UUID in the same place, which no query here sees.
	UUID const low(900);
	UUID const high(901);
	UUID const behind(899);
	UUID const trigger(898);
	for (bool const lowFirst : {true, false})
	{
		CAPTURE(lowFirst);
		Scope<PhysicsScene> world = CreateWorld();
		REQUIRE(world->AddBody(MakeBody(trigger, {0.0f, 0.0f, 5.0f}, {Trigger(glm::vec3(0.5f))})).IsOk());
		REQUIRE(world->AddBody(MakeBody(behind, {0.0f, 0.0f, 9.0f}, {Box(glm::vec3(0.5f))})).IsOk());
		for (UUID const& entity : lowFirst ? std::vector<UUID>{low, high} : std::vector<UUID>{high, low})
		{
			REQUIRE(world->AddBody(MakeBody(entity, {0.0f, 0.0f, 5.0f}, {Box(glm::vec3(0.5f))})).IsOk());
		}

		std::vector<RaycastHit> hits;
		REQUIRE(world->RaycastAll(glm::vec3(0.0f), {0.0f, 0.0f, 1.0f}, 100.0f, hits) == 3);
		CHECK(hits[0].Distance == hits[1].Distance);
		CHECK(EntitiesOf(hits) == std::vector<UUID>{low, high, behind});
		// Raycast's hit is RaycastAll's first: the smaller UUID of the solid two, whichever Jolt lists first.
		std::optional<RaycastHit> const ray = world->Raycast(glm::vec3(0.0f), {0.0f, 0.0f, 1.0f}, 100.0f);
		REQUIRE(ray);
		CHECK(ray->Entity == low);
		CHECK(ray->Distance == hits[0].Distance);

		std::optional<ShapeCastHit> const cast =
			world->ShapeCast(PhysicsQueryShape::MakeSphere(0.25f), glm::vec3(0.0f), Upright, {0.0f, 0.0f, 1.0f}, 100.0f);
		REQUIRE(cast);
		CHECK(cast->Entity == low);

		std::vector<UUID> entities;
		CHECK(world->Overlap(PhysicsQueryShape::MakeBox(glm::vec3(1.0f)), {0.0f, 0.0f, 5.0f}, Upright, entities) == 2);
		CHECK(entities == std::vector<UUID>{low, high});
		CHECK(world->Overlap(PhysicsQueryShape::MakeSphere(20.0f), glm::vec3(0.0f), Upright, entities) == 3);
		CHECK(entities == std::vector<UUID>{behind, low, high});
	}

	// The order itself: distance first, then the UUID; a hit never goes before itself.
	CHECK(IsQueryHitBefore(1.0f, UUID(5), 1.0f, UUID(7)));
	CHECK_FALSE(IsQueryHitBefore(1.0f, UUID(7), 1.0f, UUID(5)));
	CHECK_FALSE(IsQueryHitBefore(1.0f, UUID(5), 1.0f, UUID(5)));
	CHECK(IsQueryHitBefore(0.5f, UUID(9), 1.0f, UUID(1)));
	CHECK_FALSE(IsQueryHitBefore(1.0f, UUID(1), 0.5f, UUID(9)));
	CHECK(IsQueryHitBefore(0.0f, UUID(9), 0.0f, UUID(10)));
}

TEST_CASE("PhysicsScene: every query sees only the layers of its mask, and triggers only when asked")
{
	Testing::PhysicsSystemScope physics;
	PhysicsWorldSettings settings;
	settings.LayerCount = 3;
	Scope<PhysicsScene> world = CreateWorld(settings);
	UUID const trigger(501);
	UUID const first(502);
	UUID const second(503);
	UUID const third(504);
	UUID const mixed(505);
	REQUIRE(world->AddBody(MakeBody(trigger, {1.0f, 0.0f, 0.0f}, {Trigger(glm::vec3(0.25f))})).IsOk());
	REQUIRE(world->AddBody(MakeBody(first, {2.0f, 0.0f, 0.0f}, {Box(glm::vec3(0.25f))}, RigidBodyType::Static, 0)).IsOk());
	REQUIRE(world->AddBody(MakeBody(second, {4.0f, 0.0f, 0.0f}, {Box(glm::vec3(0.25f))}, RigidBodyType::Static, 1)).IsOk());
	REQUIRE(world->AddBody(MakeBody(third, {6.0f, 0.0f, 0.0f}, {Box(glm::vec3(0.25f))}, RigidBodyType::Static, 2)).IsOk());
	// One entity with a trigger collider in front of a solid one, on its own lane (z = 3).
	REQUIRE(world->AddBody(MakeBody(mixed, {8.0f, 0.0f, 3.0f}, {Trigger(glm::vec3(0.5f)), Box(glm::vec3(0.25f), {}, {1.0f, 0.0f, 0.0f})}))
	            .IsOk());

	PhysicsQueryFilter const all;
	PhysicsQueryFilter const withTriggers{AllPhysicsLayers, true};
	auto const masked = [](uint32_t mask)
	{
		return PhysicsQueryFilter{mask, false};
	};
	std::vector<RaycastHit> hits;
	std::vector<UUID> entities;

	SUBCASE("RaycastAll")
	{
		CHECK(world->RaycastAll(glm::vec3(0.0f), Along, 100.0f, hits, all) == 3);
		CHECK(EntitiesOf(hits) == std::vector<UUID>{first, second, third});
		CHECK(world->RaycastAll(glm::vec3(0.0f), Along, 100.0f, hits, withTriggers) == 4);
		CHECK(EntitiesOf(hits) == std::vector<UUID>{trigger, first, second, third});
		CHECK(world->RaycastAll(glm::vec3(0.0f), Along, 100.0f, hits, masked(1u << 1)) == 1);
		CHECK(EntitiesOf(hits) == std::vector<UUID>{second});
		CHECK(world->RaycastAll(glm::vec3(0.0f), Along, 100.0f, hits, masked((1u << 0) | (1u << 2))) == 2);
		CHECK(EntitiesOf(hits) == std::vector<UUID>{first, third});
		CHECK(world->RaycastAll(glm::vec3(0.0f), Along, 100.0f, hits, masked(0)) == 0);
		// Bits past the 16 layers a project can have see nothing.
		CHECK(world->RaycastAll(glm::vec3(0.0f), Along, 100.0f, hits, masked(0xFFFF0000u)) == 0);

		// The trigger rule is per collider: without triggers the ray reaches the solid collider behind the trigger one.
		REQUIRE(world->RaycastAll({0.0f, 0.0f, 3.0f}, Along, 100.0f, hits) == 1);
		CHECK(hits[0].Distance == doctest::Approx(8.75f).epsilon(Tolerance));
		REQUIRE(world->RaycastAll({0.0f, 0.0f, 3.0f}, Along, 100.0f, hits, withTriggers) == 1);
		CHECK(hits[0].Entity == mixed);
		CHECK(hits[0].Distance == doctest::Approx(7.5f).epsilon(Tolerance));
		// Raycast, unchanged, ignores triggers alike.
		CHECK(world->Raycast({0.0f, 0.0f, 3.0f}, Along, 100.0f)->Distance == doctest::Approx(8.75f).epsilon(Tolerance));
	}

	SUBCASE("ShapeCast")
	{
		PhysicsQueryShape const sphere = PhysicsQueryShape::MakeSphere(0.1f);
		CHECK(world->ShapeCast(sphere, glm::vec3(0.0f), Upright, Along, 100.0f, all)->Entity == first);
		CHECK(world->ShapeCast(sphere, glm::vec3(0.0f), Upright, Along, 100.0f, withTriggers)->Entity == trigger);
		CHECK(world->ShapeCast(sphere, glm::vec3(0.0f), Upright, Along, 100.0f, masked(1u << 2))->Entity == third);
		CHECK_FALSE(world->ShapeCast(sphere, glm::vec3(0.0f), Upright, Along, 100.0f, masked(0)));
		CHECK_FALSE(world->ShapeCast(sphere, glm::vec3(0.0f), Upright, Along, 100.0f, masked(1u << 3)));
		// Starting inside the trigger: it counts only when asked, at distance 0.
		CHECK(world->ShapeCast(sphere, {1.0f, 0.0f, 0.0f}, Upright, Along, 100.0f, all)->Entity == first);
		std::optional<ShapeCastHit> const inside = world->ShapeCast(sphere, {1.0f, 0.0f, 0.0f}, Upright, Along, 100.0f, withTriggers);
		REQUIRE(inside);
		CHECK(inside->Entity == trigger);
		CHECK(inside->Distance == 0.0f);
		std::optional<ShapeCastHit> const mixedHit = world->ShapeCast(sphere, {0.0f, 0.0f, 3.0f}, Upright, Along, 100.0f, all);
		REQUIRE(mixedHit);
		CHECK(mixedHit->Entity == mixed);
		CHECK(mixedHit->Distance == doctest::Approx(8.65f).epsilon(Tolerance));
	}

	SUBCASE("Overlap")
	{
		PhysicsQueryShape const everything = PhysicsQueryShape::MakeSphere(20.0f);
		CHECK(world->Overlap(everything, glm::vec3(0.0f), Upright, entities, all) == 4);
		CHECK(entities == std::vector<UUID>{first, second, third, mixed});
		CHECK(world->Overlap(everything, glm::vec3(0.0f), Upright, entities, withTriggers) == 5);
		CHECK(entities == std::vector<UUID>{trigger, first, second, third, mixed});
		CHECK(world->Overlap(everything, glm::vec3(0.0f), Upright, entities, masked(1u << 1)) == 1);
		CHECK(entities == std::vector<UUID>{second});
		CHECK(world->Overlap(everything, glm::vec3(0.0f), Upright, entities, masked(0)) == 0);
		// Only the trigger collider of the mixed entity is here.
		CHECK(world->Overlap(PhysicsQueryShape::MakeSphere(0.2f), {8.0f, 0.0f, 3.0f}, Upright, entities, all) == 0);
		CHECK(world->Overlap(PhysicsQueryShape::MakeSphere(0.2f), {8.0f, 0.0f, 3.0f}, Upright, entities, withTriggers) == 1);
	}
}

TEST_CASE("PhysicsScene: queries hit every kind of collider, with the surface's material, and characters")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	AddGround(*world);
	struct Target
	{
		UUID Entity;
		// Where queries aim from above: x, then the top's height, then z.
		glm::vec3 Top;
		AssetHandle Material;
		// Convex hulls keep a convex radius of their own (Jolt rounds them by up to 5 cm).
		float Tolerance;
	};
	std::vector<Target> targets;
	auto const add = [&world, &targets](uint64_t id, BodyDesc desc, glm::vec3 const& top, AssetHandle material, float tolerance = Tolerance)
	{
		desc.Entity = UUID(id);
		REQUIRE(world->AddBody(desc).IsOk());
		targets.push_back({UUID(id), top, material, tolerance});
	};

	add(601, MakeBody({}, {-6.0f, 1.0f, 0.0f}, {Box(glm::vec3(0.5f), AssetHandle(UUID(7601)))}), {-6.0f, 1.5f, 0.0f},
	    AssetHandle(UUID(7601)));
	add(602, MakeBody({}, {-4.0f, 1.0f, 0.0f}, {Sphere(0.5f, AssetHandle(UUID(7602)))}), {-4.0f, 1.5f, 0.0f}, AssetHandle(UUID(7602)));
	add(603, MakeBody({}, {-2.0f, 1.0f, 0.0f}, {Capsule(0.3f, 0.5f, AssetHandle(UUID(7603)))}), {-2.0f, 1.8f, 0.0f},
	    AssetHandle(UUID(7603)));
	ColliderDesc hull;
	hull.Shape = ColliderShape::ConvexMesh;
	hull.Mesh = MeshFactory::CreateCube(AssetHandle());
	hull.Surface.Material = AssetHandle(UUID(7604));
	add(604, MakeBody({}, {0.0f, 1.0f, 0.0f}, {hull}), {0.0f, 1.5f, 0.0f}, AssetHandle(UUID(7604)), 0.01f);
	// A collider offset from its body (a decorated shape).
	add(605, MakeBody({}, {2.0f, 1.0f, 0.0f}, {Box(glm::vec3(0.5f), AssetHandle(UUID(7605)), {0.0f, 1.0f, 0.0f})}), {2.0f, 2.5f, 0.0f},
	    AssetHandle(UUID(7605)));
	// A dynamic body's triangle mesh is its convex hull, with the collider's surface; asleep, it stays where it is made.
	ColliderDesc movingMesh;
	movingMesh.Shape = ColliderShape::TriangleMesh;
	movingMesh.Mesh = MeshFactory::CreateCube(AssetHandle());
	movingMesh.Surface.Material = AssetHandle(UUID(7606));
	add(606, MakeBody({}, {4.0f, 0.5f, 0.0f}, {movingMesh}, RigidBodyType::Dynamic), {4.0f, 1.0f, 0.0f}, AssetHandle(UUID(7606)), 0.01f);
	// The parts of a triangle mesh: each quad's submesh material, raised 2 m above the ground.
	ColliderDesc quads;
	quads.Shape = ColliderShape::TriangleMesh;
	quads.Mesh = MakeTwoQuads();
	quads.SubmeshSurfaces.resize(2);
	quads.SubmeshSurfaces[0].Material = AssetHandle(UUID(7607));
	quads.SubmeshSurfaces[1].Material = AssetHandle(UUID(7608));
	add(607, MakeBody({}, {6.0f, 2.0f, 0.0f}, {quads}), {5.5f, 2.0f, 0.0f}, AssetHandle(UUID(7607)));
	targets.push_back({UUID(607), {6.5f, 2.0f, 0.0f}, AssetHandle(UUID(7608)), Tolerance});

	// A character: its inner body, a little larger than its capsule (the 2 cm it keeps from what it touches), has no
	// material. Its top is where Raycast finds it.
	CharacterDesc character;
	character.Entity = UUID(608);
	character.Position = {8.0f, 0.82f, 0.0f};
	character.Radius = 0.3f;
	character.Height = 1.6f;
	REQUIRE(world->AddCharacter(character).IsOk());
	std::optional<RaycastHit> const head = world->Raycast({8.0f, 10.0f, 0.0f}, Down, 100.0f);
	REQUIRE(head);
	REQUIRE(head->Entity == UUID(608));
	CHECK(head->Point.y == doctest::Approx(0.82f + 0.8f).epsilon(0.03));
	targets.push_back({UUID(608), head->Point, AssetHandle(), Tolerance});

	std::vector<RaycastHit> hits;
	std::vector<UUID> entities;
	for (Target const& target : targets)
	{
		uint64_t const entity = target.Entity.GetValue();
		CAPTURE(entity);
		REQUIRE(world->RaycastAll(target.Top + Up * 5.0f, Down, 100.0f, hits) == 2);
		CHECK(hits[0].Entity == target.Entity);
		CHECK(hits[0].Material == target.Material);
		CHECK(hits[0].Distance == doctest::Approx(5.0f).epsilon(target.Tolerance));
		CheckNear(hits[0].Normal, Up, target.Tolerance);
		CHECK(hits[1].Entity == GroundId);

		std::optional<ShapeCastHit> const cast =
			world->ShapeCast(PhysicsQueryShape::MakeSphere(0.1f), target.Top + Up * 5.0f, Upright, Down, 100.0f);
		REQUIRE(cast);
		CHECK(cast->Entity == target.Entity);
		CHECK(cast->Material == target.Material);
		CHECK(cast->Distance == doctest::Approx(4.9f).epsilon(target.Tolerance));
		CheckNear(cast->Normal, Up, target.Tolerance);

		// A small box just below the top overlaps the target.
		CHECK(world->Overlap(PhysicsQueryShape::MakeBox(glm::vec3(0.05f)), target.Top - Up * 0.04f, Upright, entities) >= 1);
		CHECK(std::find(entities.begin(), entities.end(), target.Entity) != entities.end());
	}

	// A triangle mesh is its surface: a shape crossing it overlaps it from either side, one beside it does not.
	CHECK(world->Overlap(PhysicsQueryShape::MakeSphere(0.1f), {6.5f, 2.05f, 0.0f}, Upright, entities) == 1);
	CHECK(world->Overlap(PhysicsQueryShape::MakeSphere(0.1f), {6.5f, 1.95f, 0.0f}, Upright, entities) == 1);
	CHECK(world->Overlap(PhysicsQueryShape::MakeSphere(0.1f), {6.5f, 2.5f, 0.0f}, Upright, entities) == 0);
	// From below, a cast passes through the mesh's back faces to nothing.
	CHECK_FALSE(world->ShapeCast(PhysicsQueryShape::MakeSphere(0.1f), {6.5f, 1.0f, 0.0f}, Upright, Up, 10.0f));
}

TEST_CASE("PhysicsScene: queries with values beyond the world's limits find nothing, and longer distances are cut")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	AddGround(*world);
	float const nan = std::numeric_limits<float>::quiet_NaN();
	float const infinity = std::numeric_limits<float>::infinity();
	glm::vec3 const origin(0.0f, 5.0f, 0.0f);
	PhysicsQueryShape const sphere = PhysicsQueryShape::MakeSphere(0.5f);
	std::vector<RaycastHit> hits;
	std::vector<UUID> entities;

	for (glm::vec3 const& bad :
	     {glm::vec3(nan, 5.0f, 0.0f), glm::vec3(infinity, 5.0f, 0.0f), glm::vec3(0.0f, 2.0f * MaxPhysicsCoordinate, 0.0f)})
	{
		CAPTURE(bad);
		CHECK(world->RaycastAll(bad, Down, 100.0f, hits) == 0);
		CHECK_FALSE(world->ShapeCast(sphere, bad, Upright, Down, 100.0f));
		CHECK(world->Overlap(sphere, bad, Upright, entities) == 0);
	}
	for (glm::vec3 const& bad : {glm::vec3(0.0f), glm::vec3(nan, -1.0f, 0.0f), glm::vec3(0.0f, -infinity, 0.0f)})
	{
		CAPTURE(bad);
		CHECK(world->RaycastAll(origin, bad, 100.0f, hits) == 0);
		CHECK_FALSE(world->ShapeCast(sphere, origin, Upright, bad, 100.0f));
	}
	for (float const bad : {0.0f, -1.0f, nan, infinity})
	{
		CAPTURE(bad);
		CHECK(world->RaycastAll(origin, Down, bad, hits) == 0);
		CHECK_FALSE(world->ShapeCast(sphere, origin, Upright, Down, bad));
	}
	for (PhysicsQueryShape const& bad :
	     {PhysicsQueryShape::MakeSphere(-1.0f), PhysicsQueryShape::MakeSphere(nan),
	      PhysicsQueryShape::MakeSphere(1.01f * MaxPhysicsQueryRadius), PhysicsQueryShape::MakeBox({0.5f, -0.5f, 0.5f}),
	      PhysicsQueryShape::MakeBox({0.5f, infinity, 0.5f}), PhysicsQueryShape::MakeBox({0.5f, 0.5f, 1.01f * MaxPhysicsQueryExtent}),
	      PhysicsQueryShape::MakeCapsule(0.5f, nan), PhysicsQueryShape::MakeCapsule(-0.5f, 0.5f),
	      PhysicsQueryShape::MakeCapsule(1.01f * MaxPhysicsQueryRadius, 0.5f),
	      PhysicsQueryShape::MakeCapsule(0.5f, 1.01f * MaxPhysicsQueryExtent)})
	{
		CAPTURE(bad.Radius);
		CAPTURE(bad.HalfExtents);
		CAPTURE(bad.HalfHeight);
		CHECK_FALSE(world->ShapeCast(bad, origin, Upright, Down, 100.0f));
		CHECK(world->Overlap(bad, glm::vec3(0.0f), Upright, entities) == 0);
	}
	PhysicsQueryShape unknown = sphere;
	unknown.Type = static_cast<PhysicsQueryShapeType>(7);
	CHECK_FALSE(world->ShapeCast(unknown, origin, Upright, Down, 100.0f));
	glm::quat const badRotation(nan, 0.0f, 0.0f, 1.0f);
	CHECK_FALSE(world->ShapeCast(PhysicsQueryShape::MakeBox(glm::vec3(0.5f)), origin, badRotation, Down, 100.0f));
	CHECK(world->Overlap(PhysicsQueryShape::MakeBox(glm::vec3(0.5f)), glm::vec3(0.0f), badRotation, entities) == 0);

	// Sizes below a millimeter count as a millimeter.
	std::optional<ShapeCastHit> const point = world->ShapeCast(PhysicsQueryShape::MakeSphere(0.0f), origin, Upright, Down, 100.0f);
	REQUIRE(point);
	CHECK(point->Distance == doctest::Approx(5.0f - 0.001f).epsilon(Tolerance));
	std::optional<ShapeCastHit> const sheet =
		world->ShapeCast(PhysicsQueryShape::MakeBox({0.5f, 0.0f, 0.5f}), origin, Upright, Down, 100.0f);
	REQUIRE(sheet);
	CHECK(sheet->Distance == doctest::Approx(5.0f - 0.001f).epsilon(Tolerance));

	// A distance beyond the farthest a query reaches is cut to it, and still finds what is there.
	CHECK(world->RaycastAll(origin, Down, std::numeric_limits<float>::max(), hits) == 1);
	CHECK(hits[0].Distance == doctest::Approx(5.0f).epsilon(Tolerance));
	std::optional<ShapeCastHit> const far = world->ShapeCast(sphere, origin, Upright, Down, std::numeric_limits<float>::max());
	REQUIRE(far);
	CHECK(far->Distance == doctest::Approx(4.5f).epsilon(Tolerance));
	// A direction too large to square is still a direction.
	CHECK(world->RaycastAll(origin, {0.0f, -3.0e38f, 0.0f}, 100.0f, hits) == 1);
}

namespace
{
	// The same numbers on every platform, in [0, 1): each draw is its own statement, since the order a call's arguments are
	// evaluated in is the compiler's.
	class Numbers
	{
	public:
		float Next()
		{
			m_State = m_State * 6364136223846793005ull + 1442695040888963407ull;
			return static_cast<float>(m_State >> 40) / static_cast<float>(1ull << 24);
		}

		float Between(float low, float high) { return low + (high - low) * Next(); }

		glm::vec3 Vector(float low, float high)
		{
			float const x = Between(low, high);
			float const y = Between(low, high);
			float const z = Between(low, high);
			return {x, y, z};
		}

		glm::vec3 Direction() { return glm::normalize(Vector(-1.0f, 1.0f) + 1e-3f); }

		glm::quat Rotation()
		{
			glm::vec3 const axis = Vector(-1.0f, 1.0f);
			return glm::normalize(glm::quat(1.0f, axis.x, axis.y, axis.z));
		}

	private:
		uint64_t m_State = 20261116;
	};

	// Where a cast first touches a body, from a function of how far the shape moved that is convex, positive while they are
	// apart and negative while they overlap: its least value by ternary search, then where it first closes by bisection.
	// None when it never closes within reach, and none at all (a pose to leave out) when it comes within the grazing
	// margin of zero at the start or at its least, where the answer depends on the last bits.
	struct ClosedForm
	{
		bool Grazing = false;
		std::optional<double> Distance;
	};

	template<typename TGap>
	ClosedForm FindFirstTouch(TGap const& gap, double reach, double grazing)
	{
		double low = 0.0;
		double high = reach;
		for (int round = 0; round < 200; round++)
		{
			double const third = (high - low) / 3.0;
			if (gap(low + third) < gap(high - third))
			{
				high -= third;
			}
			else
			{
				low += third;
			}
		}
		double const nearest = (low + high) / 2.0;
		ClosedForm form;
		if (std::abs(gap(0.0)) < grazing || std::abs(gap(nearest)) < grazing)
		{
			form.Grazing = true;
			return form;
		}
		if (gap(0.0) < 0.0)
		{
			form.Distance = 0.0;
		}
		else if (gap(nearest) < 0.0)
		{
			double open = 0.0;
			double closed = nearest;
			for (int round = 0; round < 100; round++)
			{
				double const middle = (open + closed) / 2.0;
				if (gap(middle) < 0.0)
				{
					closed = middle;
				}
				else
				{
					open = middle;
				}
			}
			form.Distance = closed;
		}
		return form;
	}

	// How far a query's sphere, box or capsule at center, turned by rotation, is from a ball (negative: they overlap), in
	// double so that the closed form is exact to far below the tolerance. Sizes below a millimeter count as a millimeter.
	double GetGapToBall(PhysicsQueryShape const& shape, glm::dvec3 const& center, glm::dquat const& rotation, glm::dvec3 const& ball,
	                    double ballRadius)
	{
		glm::dvec3 const local = glm::conjugate(rotation) * (ball - center);
		switch (shape.Type)
		{
			case PhysicsQueryShapeType::Box:
			{
				glm::dvec3 const halfExtents = glm::max(glm::dvec3(shape.HalfExtents), glm::dvec3(0.001));
				return glm::length(local - glm::clamp(local, -halfExtents, halfExtents)) - ballRadius;
			}
			case PhysicsQueryShapeType::Capsule:
			{
				double const along = std::clamp(local.y, -static_cast<double>(shape.HalfHeight), static_cast<double>(shape.HalfHeight));
				return glm::length(local - glm::dvec3(0.0, along, 0.0)) - std::max(static_cast<double>(shape.Radius), 0.001) - ballRadius;
			}
			case PhysicsQueryShapeType::Sphere:
				break;
		}
		return glm::length(local) - std::max(static_cast<double>(shape.Radius), 0.001) - ballRadius;
	}

	// The separation of two boxes along the axes that tell whether they overlap (each box's three, and their cross products):
	// positive exactly when they are apart, negative when they overlap, and convex in how far one of them moved.
	double GetBoxSeparation(glm::dvec3 const& centerA, glm::dmat3 const& axesA, glm::dvec3 const& halfA, glm::dvec3 const& centerB,
	                        glm::dvec3 const& halfB)
	{
		std::vector<glm::dvec3> axes = {axesA[0], axesA[1], axesA[2], {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
		for (glm::length_t a = 0; a < 3; a++)
		{
			for (glm::length_t b = 0; b < 3; b++)
			{
				glm::dvec3 unit(0.0);
				unit[b] = 1.0;
				axes.push_back(glm::cross(axesA[a], unit));
			}
		}
		double separation = -std::numeric_limits<double>::infinity();
		for (glm::dvec3 const& axis : axes)
		{
			double const length = glm::length(axis);
			if (length < 1e-9)
			{
				continue;
			}
			glm::dvec3 const unit = axis / length;
			double const reachA = halfA.x * std::abs(glm::dot(axesA[0], unit)) + halfA.y * std::abs(glm::dot(axesA[1], unit)) +
			                      halfA.z * std::abs(glm::dot(axesA[2], unit));
			double const reachB = glm::dot(halfB, glm::abs(unit));
			separation = std::max(separation, std::abs(glm::dot(centerB - centerA, unit)) - reachA - reachB);
		}
		return separation;
	}

	// Closer than this to touching, at the start or at its nearest, a pose is left out.
	constexpr double Grazing = 0.01;
	// How far the cast's distances at the largest sizes may be from their closed forms: about five times the farthest
	// measured over 2,000 poses of each shape (MaxPhysicsQueryExtent).
	constexpr float LargeShapeTolerance = 0.005f;
}

TEST_CASE("PhysicsScene: query shapes as large as they may be cast to where they touch, and touch nothing they pass by")
{
	Testing::PhysicsSystemScope physics;
	constexpr float Reach = 10.0f;
	constexpr float BallRadius = 0.5f;
	float const extent = MaxPhysicsQueryExtent;
	float const radius = MaxPhysicsQueryRadius;
	struct Case
	{
		PhysicsQueryShape Shape;
		std::string Name;
	};
	std::vector<Case> const cases = {
		{PhysicsQueryShape::MakeBox({extent, 0.0f, 0.0f}), "a rod of a box"},
		{PhysicsQueryShape::MakeBox({extent, extent, 0.0f}), "a sheet"},
		{PhysicsQueryShape::MakeBox(glm::vec3(extent)), "a block"},
		{PhysicsQueryShape::MakeCapsule(0.0f, extent), "a rod of a capsule"},
		{PhysicsQueryShape::MakeCapsule(radius, extent), "a wide capsule"},
		{PhysicsQueryShape::MakeSphere(radius), "a large sphere"},
	};
	// A small ball the shapes are cast at, near the world's origin and a few kilometers from it.
	for (glm::vec3 const& ball : {glm::vec3(2.0f, 10.0f, 1.0f), glm::vec3(3000.0f, 10.0f, -2000.0f)})
	{
		CAPTURE(ball);
		Scope<PhysicsScene> world = CreateWorld();
		REQUIRE(world->AddBody(MakeBody(UUID(100), ball, {Sphere(BallRadius)})).IsOk());
		Numbers numbers;
		for (Case const& tested : cases)
		{
			CAPTURE(tested.Name);
			PhysicsQueryShape const& shape = tested.Shape;
			int checked = 0;
			int startsIn = 0;
			int touches = 0;
			int misses = 0;
			float worst = 0.0f;
			for (int pose = 0; pose < 150; pose++)
			{
				CAPTURE(pose);
				glm::quat const rotation = shape.Type == PhysicsQueryShapeType::Sphere ? Upright : numbers.Rotation();
				glm::vec3 const direction = numbers.Direction();
				// The shape put so that its surface passes through a point a few meters from the ball, the ball outside or
				// inside it.
				glm::vec3 const near = ball + numbers.Vector(-3.0f, 3.0f);
				glm::vec3 const toward = numbers.Direction();
				float const side = numbers.Next() < 0.5f ? -1.0f : 1.0f;
				glm::vec3 surface(0.0f);
				switch (shape.Type)
				{
					case PhysicsQueryShapeType::Box:
					{
						// A point on the box's surface, from its center.
						glm::vec3 const magnitude = glm::abs(toward);
						glm::vec3 const scaled = toward / std::max({magnitude.x, magnitude.y, magnitude.z});
						surface = rotation * (scaled * shape.HalfExtents);
						break;
					}
					case PhysicsQueryShapeType::Capsule:
						surface = rotation * glm::vec3(0.0f, toward.y * shape.HalfHeight, 0.0f) +
						          side * glm::normalize(near - ball) * shape.Radius;
						break;
					case PhysicsQueryShapeType::Sphere:
						surface = side * glm::normalize(near - ball) * shape.Radius;
						break;
				}
				glm::vec3 const origin = near - surface;
				ClosedForm const truth = FindFirstTouch(
					[&shape, &origin, &direction, &rotation, &ball](double moved)
					{
						return GetGapToBall(shape, glm::dvec3(origin) + glm::dvec3(direction) * moved, glm::dquat(rotation),
					                        glm::dvec3(ball), BallRadius);
					},
					Reach, Grazing);
				if (truth.Grazing)
				{
					continue;
				}
				checked++;
				std::optional<ShapeCastHit> const hit = world->ShapeCast(shape, origin, rotation, direction, Reach);
				std::vector<UUID> overlapped;
				world->Overlap(shape, origin, rotation, overlapped);
				CHECK(overlapped.empty() == (truth.Distance != 0.0));
				REQUIRE(hit.has_value() == truth.Distance.has_value());
				if (!truth.Distance)
				{
					misses++;
					continue;
				}
				if (*truth.Distance == 0.0)
				{
					startsIn++;
				}
				else
				{
					touches++;
				}
				CHECK((hit->Distance == 0.0f) == (*truth.Distance == 0.0));
				worst = std::max(worst, static_cast<float>(std::abs(hit->Distance - *truth.Distance)));
			}
			MESSAGE(tested.Name << ": " << checked << " poses, " << startsIn << " start in the ball, " << touches << " touch it, " << misses
			                    << " miss it; the farthest from the closed form " << worst << " m");
			CHECK(checked >= 120);
			CHECK(startsIn > 0);
			CHECK(touches > 0);
			CHECK(misses > 0);
			CHECK(worst <= LargeShapeTolerance);
		}
	}
}

TEST_CASE("PhysicsScene: a long box cast among boxes touches the first it reaches, not those it passes by")
{
	Testing::PhysicsSystemScope physics;
	// The ground, two boxes and a pole near the shape's middle, of smaller UUIDs than the ground's, so that a false touch
	// would win over the true one: the scene that showed casts of long boxes touching bodies they passed by, at distance 0,
	// when query shapes could be ten thousand times longer than they may be now.
	struct Block
	{
		UUID Entity;
		glm::vec3 Center;
		glm::vec3 HalfExtents;
	};
	std::array<Block, 4> const blocks = {
		Block{UUID(500), {0.0f, -0.5f, 0.0f}, {50.0f, 0.5f, 50.0f}}, Block{UUID(300), {0.0f, 1.0f, 10.0f}, glm::vec3(0.5f)},
		Block{UUID(200), {10.0f, 1.0f, 0.0f}, glm::vec3(0.5f)}, Block{UUID(150), {4.0f, 6.0f, 3.0f}, {0.2f, 3.0f, 0.2f}}};
	Scope<PhysicsScene> world = CreateWorld();
	for (Block const& block : blocks)
	{
		REQUIRE(world->AddBody(MakeBody(block.Entity, block.Center, {Box(block.HalfExtents)})).IsOk());
	}
	constexpr float Reach = 30.0f;
	Numbers numbers;
	int checked = 0;
	int atZero = 0;
	int later = 0;
	float worst = 0.0f;
	for (int pose = 0; pose < 300; pose++)
	{
		CAPTURE(pose);
		glm::quat const rotation = numbers.Rotation();
		float const thickness = numbers.Between(0.0f, 1.0f);
		glm::vec3 const origin = pose % 2 == 0 ? glm::vec3(0.0f, 10.0f, 0.0f) : numbers.Vector(-6.0f, 14.0f);
		glm::vec3 const direction = pose % 2 == 0 ? glm::normalize(glm::vec3(1.0f, 0.3f, 0.0f)) : numbers.Direction();
		glm::vec3 const halfExtents(std::max(thickness, 0.001f), MaxPhysicsQueryExtent, std::max(thickness, 0.001f));
		glm::dmat3 const axes = glm::mat3_cast(glm::dquat(rotation));

		// The closed form: of the blocks, the first touched (at distance 0 the one of the smallest UUID).
		bool grazing = false;
		std::optional<double> firstDistance;
		UUID first = UUID::Invalid();
		for (Block const& block : blocks)
		{
			ClosedForm const touch = FindFirstTouch(
				[&origin, &direction, &axes, &halfExtents, &block](double moved)
				{
					return GetBoxSeparation(glm::dvec3(origin) + glm::dvec3(direction) * moved, axes, glm::dvec3(halfExtents),
				                            glm::dvec3(block.Center), glm::dvec3(block.HalfExtents));
				},
				Reach, Grazing);
			grazing = grazing || touch.Grazing;
			if (touch.Distance && (!firstDistance || IsQueryHitBefore(static_cast<float>(*touch.Distance), block.Entity,
			                                                          static_cast<float>(*firstDistance), first)))
			{
				firstDistance = touch.Distance;
				first = block.Entity;
			}
		}
		if (grazing)
		{
			continue;
		}
		checked++;
		std::optional<ShapeCastHit> const hit =
			world->ShapeCast(PhysicsQueryShape::MakeBox(halfExtents), origin, rotation, direction, Reach);
		REQUIRE(hit.has_value() == firstDistance.has_value());
		if (!hit)
		{
			continue;
		}
		CAPTURE(hit->Entity.GetValue());
		CAPTURE(first.GetValue());
		CAPTURE(*firstDistance);
		CHECK(hit->Entity == first);
		CHECK((hit->Distance == 0.0f) == (*firstDistance == 0.0));
		if (*firstDistance == 0.0)
		{
			atZero++;
		}
		else
		{
			later++;
			worst = std::max(worst, static_cast<float>(std::abs(hit->Distance - *firstDistance)));
		}
	}
	MESSAGE(checked << " poses, " << atZero << " start in a block, " << later << " touch one later; the farthest from the closed form "
	                << worst << " m");
	CHECK(checked >= 200);
	CHECK(atZero > 0);
	CHECK(later > 0);
	CHECK(worst <= LargeShapeTolerance);
}

namespace
{
	// A world of balls that hover over the ground by casting down each step, and push away what they overlap: the queries
	// decide the forces. Returns the state hash after each step, and a hash of every query's results.
	struct HoverRun
	{
		std::vector<uint64_t> States;
		uint64_t Queries = 0;
	};

	HoverRun RunHover(bool useQueries, bool applyForces, int steps)
	{
		Scope<PhysicsScene> world = CreateWorld();
		AddGround(*world);
		std::vector<UUID> balls;
		for (uint64_t i = 0; i < 6; i++)
		{
			float const x = -3.0f + 1.1f * static_cast<float>(i);
			BodyDesc ball = MakeBody(UUID(1000 + i), {x, 1.0f + 0.3f * static_cast<float>(i % 3), 0.2f * static_cast<float>(i)},
			                         {Sphere(0.3f)}, RigidBodyType::Dynamic);
			ball.LinearVelocity = {0.5f * static_cast<float>(i % 2), 0.0f, -0.3f};
			REQUIRE(world->AddBody(ball).IsOk());
			balls.push_back(ball.Entity);
		}
		// An obstacle the casts meet, and a trigger they pass unless asked.
		REQUIRE(world->AddBody(MakeBody(UUID(1100), {0.0f, 0.2f, 0.0f}, {Box({0.4f, 0.2f, 0.4f})})).IsOk());
		REQUIRE(world->AddBody(MakeBody(UUID(1101), {1.0f, 0.5f, 1.0f}, {Trigger(glm::vec3(1.0f))})).IsOk());

		HoverRun run;
		HashBuilder queries;
		std::vector<UUID> overlapped;
		std::vector<RaycastHit> hits;
		for (int step = 0; step < steps; step++)
		{
			if (useQueries)
			{
				for (UUID const& ball : balls)
				{
					glm::vec3 const position = world->GetBodyTransform(ball)->Position;
					// From below the ball, which a cast starting inside it would touch at distance 0.
					std::optional<ShapeCastHit> const below = world->ShapeCast(PhysicsQueryShape::MakeSphere(0.25f), position + Down * 0.6f,
					                                                           Upright, Down, 2.0f, {AllPhysicsLayers, false});
					queries.AddUInt64(below ? below->Entity.GetValue() : 0);
					queries.AddFloat(below ? below->Distance : -1.0f);
					world->RaycastAll(position, Along, 5.0f, hits, {AllPhysicsLayers, true});
					for (RaycastHit const& hit : hits)
					{
						queries.AddUInt64(hit.Entity.GetValue());
						queries.AddFloat(hit.Distance);
					}
					world->Overlap(PhysicsQueryShape::MakeSphere(0.8f), position, Upright, overlapped);
					for (UUID const& other : overlapped)
					{
						queries.AddUInt64(other.GetValue());
					}
					if (applyForces)
					{
						// A spring that holds the ball about 0.6 m above what is below it, and a push away from its neighbours.
						if (below && below->Distance < 0.3f)
						{
							world->AddForce(ball, Up * (0.3f - below->Distance) * 40.0f, ForceMode::Force);
						}
						for (UUID const& other : overlapped)
						{
							if (other != ball && world->GetBodyTransform(other))
							{
								glm::vec3 const away = position - world->GetBodyTransform(other)->Position;
								world->AddForce(ball, away * 2.0f, ForceMode::Force);
							}
						}
					}
				}
			}
			world->Step(Step);
			run.States.push_back(world->ComputeStateHash());
		}
		run.Queries = queries.GetHash();
		return run;
	}
}

TEST_CASE("PhysicsScene: queries between steps give the same results in worlds built and stepped alike, and change nothing themselves")
{
	Testing::PhysicsSystemScope physics;
	constexpr int Steps = 180;
	HoverRun const first = RunHover(true, true, Steps);
	HoverRun const second = RunHover(true, true, Steps);
	CHECK(first.States == second.States);
	CHECK(first.Queries == second.Queries);

	// The forces the queries decide change the simulation; the queries alone do not.
	HoverRun const queriesOnly = RunHover(true, false, Steps);
	HoverRun const without = RunHover(false, false, Steps);
	CHECK(queriesOnly.States == without.States);
	CHECK(first.States.back() != without.States.back());
}

TEST_CASE("ScenePhysics: the scene's queries find a body made since the last step at once, and the others where the step left them")
{
	Testing::PhysicsSystemScope physics;
	Scene scene("Queries");
	Entity ground = scene.CreateEntityWithUUID(GroundId, "Ground");
	ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
	ground.AddComponent<BoxColliderComponent>().HalfExtents = {10.0f, 0.5f, 10.0f};
	Entity post = scene.CreateEntityWithUUID(UUID(1201), "Post");
	post.GetComponent<TransformComponent>().Translation = {3.0f, 1.0f, 0.0f};
	post.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.5f);
	std::vector<RaycastHit> hits;
	std::vector<UUID> entities;
	PhysicsQueryShape const sphere = PhysicsQueryShape::MakeSphere(0.25f);

	// Not running: no physics, nothing found.
	CHECK_FALSE(scene.Raycast({0.0f, 5.0f, 0.0f}, Down, 100.0f));
	CHECK(scene.RaycastAll({0.0f, 5.0f, 0.0f}, Down, 100.0f, hits) == 0);
	CHECK_FALSE(scene.ShapeCast(sphere, {0.0f, 5.0f, 0.0f}, Upright, Down, 100.0f));
	CHECK(scene.Overlap(sphere, glm::vec3(0.0f), Upright, entities) == 0);

	SceneRuntimeSettings settings;
	settings.RunScripts = false;
	settings.PlayAudio = false;
	scene.OnRuntimeStart(settings);
	REQUIRE(scene.GetPhysicsScene() != nullptr);
	CHECK(scene.Raycast({0.0f, 5.0f, 0.0f}, Down, 100.0f)->Entity == GroundId);
	CHECK(scene.RaycastAll({0.0f, 5.0f, 0.0f}, Along, 100.0f, hits) == 0);
	CHECK(scene.ShapeCast(sphere, {3.0f, 5.0f, 0.0f}, Upright, Down, 100.0f)->Entity == UUID(1201));

	// A collider added this frame is found before any step.
	Entity crate = scene.CreateEntityWithUUID(UUID(1202), "Crate");
	crate.GetComponent<TransformComponent>().Translation = {0.0f, 2.0f, 0.0f};
	crate.AddComponent<SphereColliderComponent>().Radius = 0.5f;
	std::optional<ShapeCastHit> const made = scene.ShapeCast(sphere, {0.0f, 5.0f, 0.0f}, Upright, Down, 100.0f);
	REQUIRE(made);
	CHECK(made->Entity == UUID(1202));
	CHECK(made->Distance == doctest::Approx(2.25f).epsilon(Tolerance));

	// A body moved since the last step is found where the step left it, until the next step takes it there.
	post.GetComponent<TransformComponent>().Translation = {-3.0f, 1.0f, 0.0f};
	CHECK(scene.Overlap(sphere, {3.0f, 1.0f, 0.0f}, Upright, entities) == 1);
	CHECK(scene.Overlap(sphere, {-3.0f, 1.0f, 0.0f}, Upright, entities) == 0);
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(scene.Overlap(sphere, {3.0f, 1.0f, 0.0f}, Upright, entities) == 0);
	CHECK(scene.Overlap(sphere, {-3.0f, 1.0f, 0.0f}, Upright, entities) == 1);
	CHECK(entities == std::vector<UUID>{UUID(1201)});
	CHECK(scene.RaycastAll({-10.0f, 1.0f, 0.0f}, Along, 100.0f, hits) == 1);
	CHECK(hits[0].Entity == UUID(1201));

	scene.OnRuntimeStop();
	CHECK(scene.Overlap(sphere, {-3.0f, 1.0f, 0.0f}, Upright, entities) == 0);
}
