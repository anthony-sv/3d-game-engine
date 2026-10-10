#include "Physics/PhysicsTestUtilities.h"

#include "Strada/Asset/MeshFactory.h"

#include <doctest/doctest.h>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <limits>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float Step = 1.0f / 60.0f;

	UUID const GroundId(101);
	UUID const BallId(102);
	UUID const OtherId(103);

	Scope<PhysicsScene> CreateWorld(PhysicsWorldSettings const& settings = {})
	{
		Result<Scope<PhysicsScene>> world = PhysicsScene::Create(settings);
		REQUIRE(world.IsOk());
		return world.TakeValue();
	}

	ColliderDesc Box(glm::vec3 const& halfExtents, bool trigger = false)
	{
		ColliderDesc collider;
		collider.Shape = ColliderShape::Box;
		collider.HalfExtents = halfExtents;
		collider.IsTrigger = trigger;
		return collider;
	}

	ColliderDesc Sphere(float radius)
	{
		ColliderDesc collider;
		collider.Shape = ColliderShape::Sphere;
		collider.Radius = radius;
		return collider;
	}

	BodyDesc MakeBody(UUID entity, RigidBodyType type, glm::vec3 const& position, ColliderDesc collider)
	{
		BodyDesc desc;
		desc.Entity = entity;
		desc.Type = type;
		desc.Position = position;
		desc.Colliders.push_back(std::move(collider));
		return desc;
	}

	// A 20 x 1 x 20 static floor whose top is at y = 0.
	BodyDesc Ground(uint32_t layer = 0)
	{
		BodyDesc ground = MakeBody(GroundId, RigidBodyType::Static, {0.0f, -0.5f, 0.0f}, Box({10.0f, 0.5f, 10.0f}));
		ground.Layer = layer;
		return ground;
	}

	std::vector<ContactEvent> Simulate(PhysicsScene& world, float seconds)
	{
		std::vector<ContactEvent> events;
		for (float time = 0.0f; time < seconds; time += Step)
		{
			world.Step(Step);
			std::vector<ContactEvent> const stepEvents = world.TakeContactEvents();
			events.insert(events.end(), stepEvents.begin(), stepEvents.end());
		}
		return events;
	}

	float HeightOf(PhysicsScene const& world, UUID entity)
	{
		std::optional<BodyTransform> const transform = world.GetBodyTransform(entity);
		REQUIRE(transform);
		return transform->Position.y;
	}
}

TEST_CASE("PhysicsScene: dynamic bodies fall, land on static ground and fall asleep")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	REQUIRE(world->AddBody(Ground()).IsOk());
	REQUIRE(world->AddBody(MakeBody(BallId, RigidBodyType::Dynamic, {0.0f, 5.0f, 0.0f}, Sphere(0.5f))).IsOk());
	CHECK(world->GetBodyCount() == 2);
	CHECK(world->GetGravity().y == doctest::Approx(-9.81f));

	// Falling: the ball is reported among the moving bodies.
	world->Step(Step);
	std::vector<BodyTransform> const moving = world->GetActiveBodyTransforms();
	REQUIRE(moving.size() == 1);
	CHECK(moving[0].Entity == BallId);
	CHECK(moving[0].Position.y < 5.0f);

	std::vector<ContactEvent> const events = Simulate(*world, 3.0f);
	CHECK(HeightOf(*world, BallId) == doctest::Approx(0.5f).epsilon(0.05));
	CHECK(glm::length(world->GetLinearVelocity(BallId)) < 0.05f);
	CHECK(world->IsSleeping(BallId));
	CHECK(world->GetActiveBodyTransforms().empty());
	CHECK(Testing::HasEvent(events, ContactEventType::CollisionEnter, GroundId, BallId));
	CHECK(std::count_if(events.begin(), events.end(),
	                    [](ContactEvent const& event)
	                    {
							return event.Type == ContactEventType::CollisionEnter;
						}) == 1);
	// Falling asleep on the ground does not end the contact.
	CHECK_FALSE(Testing::HasEvent(events, ContactEventType::CollisionExit, GroundId, BallId));

	// A sleeping body wakes up when pushed; leaving the ground ends the contact and landing again starts a new one.
	world->AddForce(BallId, {0.0f, 4.0f, 0.0f}, ForceMode::VelocityChange);
	CHECK_FALSE(world->IsSleeping(BallId));
	std::vector<ContactEvent> const hop = Simulate(*world, 2.5f);
	REQUIRE(hop.size() == 2);
	CHECK(hop[0].Type == ContactEventType::CollisionExit);
	CHECK(hop[1].Type == ContactEventType::CollisionEnter);
	CHECK(world->IsSleeping(BallId));
}

TEST_CASE("PhysicsScene: ignored layer pairs pass through each other and triggers only report overlaps")
{
	Testing::PhysicsSystemScope physics;
	PhysicsWorldSettings settings;
	settings.LayerCount = 2;
	settings.IgnoredCollisions = {{0, 1}};
	Scope<PhysicsScene> world = CreateWorld(settings);
	REQUIRE(world->AddBody(Ground(0)).IsOk());
	BodyDesc ghost = MakeBody(BallId, RigidBodyType::Dynamic, {0.0f, 2.0f, 0.0f}, Sphere(0.5f));
	ghost.Layer = 1;
	REQUIRE(world->AddBody(ghost).IsOk());
	std::vector<ContactEvent> const ghostEvents = Simulate(*world, 1.5f);
	CHECK(HeightOf(*world, BallId) < -1.0f);
	CHECK(ghostEvents.empty());

	// A trigger between the ball and the ground: entered and left on the way down, then the ball lands.
	Scope<PhysicsScene> triggers = CreateWorld();
	REQUIRE(triggers->AddBody(Ground()).IsOk());
	REQUIRE(triggers->AddBody(MakeBody(OtherId, RigidBodyType::Static, {0.0f, 2.5f, 0.0f}, Box({2.0f, 1.0f, 2.0f}, true))).IsOk());
	REQUIRE(triggers->AddBody(MakeBody(BallId, RigidBodyType::Dynamic, {0.0f, 6.0f, 0.0f}, Sphere(0.5f))).IsOk());
	std::vector<ContactEvent> const events = Simulate(*triggers, 3.0f);
	CHECK(Testing::HasEvent(events, ContactEventType::TriggerEnter, OtherId, BallId));
	CHECK(Testing::HasEvent(events, ContactEventType::TriggerExit, OtherId, BallId));
	CHECK_FALSE(Testing::HasEvent(events, ContactEventType::CollisionEnter, OtherId, BallId));
	CHECK(Testing::HasEvent(events, ContactEventType::CollisionEnter, GroundId, BallId));
	CHECK(HeightOf(*triggers, BallId) == doctest::Approx(0.5f).epsilon(0.05));

	// A body that comes to rest inside a trigger stays inside it while it sleeps.
	Scope<PhysicsScene> resting = CreateWorld();
	REQUIRE(resting->AddBody(Ground()).IsOk());
	REQUIRE(resting->AddBody(MakeBody(OtherId, RigidBodyType::Static, {0.0f, 1.0f, 0.0f}, Box({2.0f, 1.5f, 2.0f}, true))).IsOk());
	REQUIRE(resting->AddBody(MakeBody(BallId, RigidBodyType::Dynamic, {0.0f, 5.0f, 0.0f}, Sphere(0.5f))).IsOk());
	std::vector<ContactEvent> const restingEvents = Simulate(*resting, 3.0f);
	CHECK(resting->IsSleeping(BallId));
	CHECK(Testing::HasEvent(restingEvents, ContactEventType::TriggerEnter, OtherId, BallId));
	CHECK_FALSE(Testing::HasEvent(restingEvents, ContactEventType::TriggerExit, OtherId, BallId));
}

TEST_CASE("PhysicsScene: kinematic bodies follow their targets and push dynamic bodies")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	REQUIRE(world->AddBody(Ground()).IsOk());
	REQUIRE(world->AddBody(MakeBody(OtherId, RigidBodyType::Kinematic, {-2.0f, 0.5f, 0.0f}, Box(glm::vec3(0.5f)))).IsOk());
	REQUIRE(world->AddBody(MakeBody(BallId, RigidBodyType::Dynamic, {0.0f, 0.5f, 0.0f}, Box(glm::vec3(0.5f)))).IsOk());

	glm::vec3 target(-2.0f, 0.5f, 0.0f);
	for (int step = 0; step < 240; step++)
	{
		target.x += Step;
		world->MoveKinematic(OtherId, target, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), Step);
		world->Step(Step);
	}
	std::optional<BodyTransform> const pusher = world->GetBodyTransform(OtherId);
	REQUIRE(pusher);
	CHECK(pusher->Position.x == doctest::Approx(target.x).epsilon(0.01));
	std::optional<BodyTransform> const pushed = world->GetBodyTransform(BallId);
	REQUIRE(pushed);
	CHECK(pushed->Position.x > target.x + 0.9f);
	CHECK(world->GetMass(OtherId) == 0.0f);
}

TEST_CASE("PhysicsScene: forces, impulses and velocities respect mass and locked axes")
{
	Testing::PhysicsSystemScope physics;
	PhysicsWorldSettings settings;
	settings.Gravity = glm::vec3(0.0f);
	Scope<PhysicsScene> world = CreateWorld(settings);
	BodyDesc ball = MakeBody(BallId, RigidBodyType::Dynamic, glm::vec3(0.0f), Sphere(0.5f));
	ball.Mass = 2.0f;
	ball.LinearDamping = 0.0f;
	ball.AngularDamping = 0.0f;
	ball.LockTranslation = {false, true, false};
	ball.LockRotation = {true, true, true};
	REQUIRE(world->AddBody(ball).IsOk());
	CHECK(world->GetMass(BallId) == doctest::Approx(2.0f));

	auto const velocityX = [&world]
	{
		return world->GetLinearVelocity(BallId).x;
	};
	world->AddForce(BallId, {2.0f, 0.0f, 0.0f}, ForceMode::Impulse);
	world->Step(Step);
	CHECK(velocityX() == doctest::Approx(1.0f).epsilon(0.001));
	world->AddForce(BallId, {1.0f, 0.0f, 0.0f}, ForceMode::VelocityChange);
	world->Step(Step);
	CHECK(velocityX() == doctest::Approx(2.0f).epsilon(0.001));
	// One step of force F changes the velocity by F / m * dt.
	world->AddForce(BallId, {120.0f, 0.0f, 0.0f}, ForceMode::Force);
	world->Step(Step);
	CHECK(velocityX() == doctest::Approx(3.0f).epsilon(0.001));
	world->AddForce(BallId, {60.0f, 0.0f, 0.0f}, ForceMode::Acceleration);
	world->Step(Step);
	CHECK(velocityX() == doctest::Approx(4.0f).epsilon(0.001));

	// Locked axes do not move.
	world->AddForce(BallId, {0.0f, 10.0f, 0.0f}, ForceMode::Impulse);
	world->AddTorque(BallId, {5.0f, 5.0f, 5.0f}, ForceMode::Impulse);
	world->Step(Step);
	CHECK(world->GetLinearVelocity(BallId).y == doctest::Approx(0.0f));
	CHECK(glm::length(world->GetAngularVelocity(BallId)) == doctest::Approx(0.0f));
	CHECK(HeightOf(*world, BallId) == doctest::Approx(0.0f));

	world->SetLinearVelocity(BallId, {0.0f, 0.0f, -3.0f});
	CHECK(world->GetLinearVelocity(BallId).z == doctest::Approx(-3.0f));

	// Torques turn free bodies; velocity changes ignore the inertia.
	BodyDesc spinner = MakeBody(OtherId, RigidBodyType::Dynamic, {5.0f, 0.0f, 0.0f}, Box(glm::vec3(0.5f)));
	spinner.AngularDamping = 0.0f;
	REQUIRE(world->AddBody(spinner).IsOk());
	world->AddTorque(OtherId, {0.0f, 2.0f, 0.0f}, ForceMode::VelocityChange);
	world->Step(Step);
	CHECK(world->GetAngularVelocity(OtherId).y == doctest::Approx(2.0f).epsilon(0.001));

	// Static bodies have no velocity or mass.
	REQUIRE(world->AddBody(Ground()).IsOk());
	world->SetLinearVelocity(GroundId, {1.0f, 0.0f, 0.0f});
	CHECK(world->GetLinearVelocity(GroundId) == glm::vec3(0.0f));
	CHECK(world->GetMass(GroundId) == 0.0f);
	CHECK_FALSE(world->IsSleeping(GroundId));
}

TEST_CASE("PhysicsScene: raycasts hit the closest solid collider on the masked layers")
{
	Testing::PhysicsSystemScope physics;
	PhysicsWorldSettings settings;
	settings.LayerCount = 2;
	Scope<PhysicsScene> world = CreateWorld(settings);
	REQUIRE(world->AddBody(MakeBody(GroundId, RigidBodyType::Static, {5.0f, 0.0f, 0.0f}, Box(glm::vec3(0.5f)))).IsOk());
	BodyDesc far = MakeBody(OtherId, RigidBodyType::Static, {10.0f, 0.0f, 0.0f}, Box(glm::vec3(0.5f)));
	far.Layer = 1;
	REQUIRE(world->AddBody(far).IsOk());
	REQUIRE(world->AddBody(MakeBody(BallId, RigidBodyType::Static, {2.0f, 0.0f, 0.0f}, Box(glm::vec3(0.5f), true))).IsOk());

	std::optional<RaycastHit> const hit = world->Raycast(glm::vec3(0.0f), {3.0f, 0.0f, 0.0f}, 100.0f);
	REQUIRE(hit);
	// The trigger in front is ignored.
	CHECK(hit->Entity == GroundId);
	CHECK(hit->Distance == doctest::Approx(4.5f).epsilon(0.001));
	CHECK(hit->Point.x == doctest::Approx(4.5f).epsilon(0.001));
	CHECK(hit->Normal.x == doctest::Approx(-1.0f).epsilon(0.001));

	std::optional<RaycastHit> const masked = world->Raycast(glm::vec3(0.0f), {1.0f, 0.0f, 0.0f}, 100.0f, 1u << 1);
	REQUIRE(masked);
	CHECK(masked->Entity == OtherId);
	CHECK(masked->Distance == doctest::Approx(9.5f).epsilon(0.001));

	// Rays starting inside a collider ignore it.
	std::optional<RaycastHit> const inside = world->Raycast({5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f);
	REQUIRE(inside);
	CHECK(inside->Entity == OtherId);

	CHECK_FALSE(world->Raycast(glm::vec3(0.0f), {1.0f, 0.0f, 0.0f}, 3.0f));
	CHECK_FALSE(world->Raycast(glm::vec3(0.0f), glm::vec3(0.0f), 100.0f));
	CHECK_FALSE(world->Raycast(glm::vec3(0.0f), {0.0f, 1.0f, 0.0f}, 100.0f));
}

TEST_CASE("PhysicsScene: shapes take the body scale, offsets and meshes")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();

	// Scaled box: its faces move out with the scale.
	BodyDesc scaled = MakeBody(GroundId, RigidBodyType::Static, {5.0f, 0.0f, 0.0f}, Box(glm::vec3(0.5f)));
	scaled.Scale = glm::vec3(2.0f);
	REQUIRE(world->AddBody(scaled).IsOk());
	CHECK(world->Raycast(glm::vec3(0.0f), {1.0f, 0.0f, 0.0f}, 100.0f)->Distance == doctest::Approx(4.0f).epsilon(0.001));

	// A collider offset from the body's origin.
	BodyDesc offset = MakeBody(OtherId, RigidBodyType::Static, {0.0f, 0.0f, 20.0f}, Box(glm::vec3(0.5f)));
	offset.Colliders[0].Offset = {0.0f, 3.0f, 0.0f};
	REQUIRE(world->AddBody(offset).IsOk());
	std::optional<RaycastHit> const top = world->Raycast({0.0f, 10.0f, 20.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
	REQUIRE(top);
	CHECK(top->Entity == OtherId);
	CHECK(top->Point.y == doctest::Approx(3.5f).epsilon(0.001));

	// Two colliders form one compound body.
	BodyDesc compound = MakeBody(BallId, RigidBodyType::Static, {0.0f, 0.0f, -20.0f}, Box(glm::vec3(0.5f)));
	ColliderDesc second = Sphere(0.5f);
	second.Offset = {0.0f, 2.0f, 0.0f};
	compound.Colliders.push_back(second);
	REQUIRE(world->AddBody(compound).IsOk());
	CHECK(world->Raycast({0.0f, 10.0f, -20.0f}, {0.0f, -1.0f, 0.0f}, 100.0f)->Point.y == doctest::Approx(2.5f).epsilon(0.001));

	// Convex hull of a cube mesh, and a triangle mesh floor that a dynamic ball rests on.
	ColliderDesc hull;
	hull.Shape = ColliderShape::ConvexMesh;
	hull.Mesh = MeshFactory::CreateCube(AssetHandle());
	REQUIRE(world->AddBody(MakeBody(UUID(201), RigidBodyType::Static, {-5.0f, 0.0f, 0.0f}, hull)).IsOk());
	CHECK(world->Raycast(glm::vec3(0.0f), {-1.0f, 0.0f, 0.0f}, 100.0f)->Distance == doctest::Approx(4.5f).epsilon(0.01));

	Scope<PhysicsScene> meshWorld = CreateWorld();
	ColliderDesc floor;
	floor.Shape = ColliderShape::TriangleMesh;
	floor.Mesh = MeshFactory::CreatePlane(AssetHandle());
	BodyDesc plane = MakeBody(GroundId, RigidBodyType::Static, glm::vec3(0.0f), floor);
	plane.Scale = glm::vec3(20.0f);
	REQUIRE(meshWorld->AddBody(plane).IsOk());
	REQUIRE(meshWorld->AddBody(MakeBody(BallId, RigidBodyType::Dynamic, {0.0f, 3.0f, 0.0f}, Sphere(0.5f))).IsOk());
	Simulate(*meshWorld, 2.5f);
	CHECK(HeightOf(*meshWorld, BallId) == doctest::Approx(0.5f).epsilon(0.05));

	// Invalid descriptions fail without changing the world.
	BodyDesc empty;
	empty.Entity = UUID(300);
	CHECK(world->AddBody(empty).IsError());
	ColliderDesc noMesh;
	noMesh.Shape = ColliderShape::ConvexMesh;
	CHECK(world->AddBody(MakeBody(UUID(301), RigidBodyType::Static, glm::vec3(0.0f), noMesh)).IsError());
	CHECK(world->GetBodyCount() == 4);
}

TEST_CASE("PhysicsScene: bodies are replaced and removed, ending their contacts")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	REQUIRE(world->AddBody(Ground()).IsOk());
	REQUIRE(world->AddBody(MakeBody(BallId, RigidBodyType::Dynamic, {0.0f, 1.0f, 0.0f}, Sphere(0.5f))).IsOk());
	Simulate(*world, 1.0f);

	// Replacing keeps one body per entity.
	REQUIRE(world->AddBody(MakeBody(BallId, RigidBodyType::Dynamic, {0.0f, 0.5f, 0.0f}, Sphere(0.5f))).IsOk());
	CHECK(world->GetBodyCount() == 2);
	Simulate(*world, 0.5f);

	world->RemoveBody(GroundId);
	CHECK_FALSE(world->HasBody(GroundId));
	CHECK(world->GetBodyCount() == 1);
	CHECK(Testing::HasEvent(world->TakeContactEvents(), ContactEventType::CollisionExit, GroundId, BallId));
	world->RemoveBody(GroundId);
	CHECK_FALSE(world->GetBodyTransform(GroundId).has_value());

	// Teleporting moves the body without simulating the way there.
	world->SetBodyTransform(BallId, {7.0f, 9.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
	CHECK(world->GetBodyTransform(BallId)->Position == glm::vec3(7.0f, 9.0f, 0.0f));
}

TEST_CASE("PhysicsScene: worlds need the physics system")
{
	CHECK_FALSE(PhysicsSystem::IsInitialized());
	CHECK(PhysicsScene::Create({}).IsError());
	{
		Testing::PhysicsSystemScope physics;
		CHECK(PhysicsSystem::IsInitialized());
		CHECK(PhysicsScene::Create({}).IsOk());
	}
	CHECK_FALSE(PhysicsSystem::IsInitialized());
}

TEST_CASE("PhysicsScene: values beyond the physics world's limits are clamped or refused")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	auto const isFinite = [](glm::vec3 const& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	};
	glm::quat const identity(1.0f, 0.0f, 0.0f, 0.0f);

	// Colliders too large, offsets too far and transforms that are not finite make no body.
	CHECK(world->AddBody(MakeBody(BallId, RigidBodyType::Static, glm::vec3(0.0f), Box(glm::vec3(1.0e30f)))).IsError());
	CHECK(world->AddBody(MakeBody(BallId, RigidBodyType::Dynamic, glm::vec3(0.0f), Sphere(2.0f * MaxPhysicsExtent))).IsError());
	BodyDesc offset = MakeBody(BallId, RigidBodyType::Static, glm::vec3(0.0f), Box(glm::vec3(1.0f)));
	offset.Colliders[0].Offset = glm::vec3(0.0f, 2.0f * MaxPhysicsExtent, 0.0f);
	CHECK(world->AddBody(offset).IsError());
	float const infinity = std::numeric_limits<float>::infinity();
	CHECK(world->AddBody(MakeBody(BallId, RigidBodyType::Dynamic, {infinity, 0.0f, 0.0f}, Sphere(0.5f))).IsError());
	CHECK(world->GetBodyCount() == 0);

	// A body beyond the edge stays at it; its mass and initial velocities are clamped.
	BodyDesc far = MakeBody(BallId, RigidBodyType::Dynamic, {1.0e30f, 0.0f, 0.0f}, Sphere(0.5f));
	far.Mass = 1.0e30f;
	far.GravityFactor = 1.0e30f;
	far.LinearVelocity = {1.0e20f, 0.0f, 0.0f};
	far.AngularVelocity = {0.0f, 1.0e20f, 0.0f};
	REQUIRE(world->AddBody(far).IsOk());
	CHECK(world->GetBodyTransform(BallId)->Position.x == doctest::Approx(MaxPhysicsCoordinate));
	CHECK(world->GetMass(BallId) == doctest::Approx(MaxPhysicsMass));
	CHECK(glm::length(world->GetLinearVelocity(BallId)) <= 500.0f * 1.001f);

	// Teleports are clamped too, and ignored when not finite.
	world->SetBodyTransform(BallId, {0.0f, -1.0e30f, 0.0f}, identity);
	CHECK(world->GetBodyTransform(BallId)->Position.y == doctest::Approx(-MaxPhysicsCoordinate));
	world->SetBodyTransform(BallId, glm::vec3(std::numeric_limits<float>::quiet_NaN()), identity);
	CHECK(world->GetBodyTransform(BallId)->Position.y == doctest::Approx(-MaxPhysicsCoordinate));

	// Rotations with components near the float range keep their direction (normalizing them naively gives zero).
	glm::quat const quarterTurn = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
	auto const isQuarterTurn = [&](UUID entity)
	{
		return std::abs(glm::dot(world->GetBodyTransform(entity)->Rotation, quarterTurn)) > 0.9999f;
	};
	world->SetBodyTransform(BallId, glm::vec3(0.0f), quarterTurn * 1.0e30f);
	CHECK(isQuarterTurn(BallId));
	BodyDesc turned = MakeBody(GroundId, RigidBodyType::Kinematic, {0.0f, 50.0f, 0.0f}, Box(glm::vec3(1.0f)));
	turned.Rotation = quarterTurn * 1.0e30f;
	REQUIRE(world->AddBody(turned).IsOk());
	CHECK(isQuarterTurn(GroundId));
	world->MoveKinematic(GroundId, {0.0f, 50.0f, 0.0f}, quarterTurn * -1.0e30f, Step);
	world->Step(Step);
	CHECK(isQuarterTurn(GroundId));

	// Extreme gravity, forces and torques, even on the lightest and smallest body, leave the simulation finite.
	world->SetGravity({0.0f, -1.0e30f, 0.0f});
	CHECK(world->GetGravity().y == doctest::Approx(-MaxPhysicsGravity));
	BodyDesc speck = MakeBody(OtherId, RigidBodyType::Dynamic, {0.0f, 10.0f, 0.0f}, Sphere(1.0e-3f));
	speck.Mass = 0.0f;
	REQUIRE(world->AddBody(speck).IsOk());
	for (int step = 0; step < 10; step++)
	{
		for (ForceMode const mode : {ForceMode::Force, ForceMode::Impulse, ForceMode::Acceleration, ForceMode::VelocityChange})
		{
			world->AddForce(OtherId, glm::vec3(1.0e30f), mode);
			world->AddTorque(OtherId, glm::vec3(1.0e30f), mode);
		}
		world->Step(Step);
	}
	for (UUID const entity : {BallId, OtherId})
	{
		CAPTURE(entity);
		CHECK(isFinite(world->GetLinearVelocity(entity)));
		CHECK(isFinite(world->GetAngularVelocity(entity)));
		CHECK(isFinite(world->GetBodyTransform(entity)->Position));
	}
}

TEST_CASE("PhysicsScene: velocities beyond Jolt's limits are clamped in every direction")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	// Jolt's default limits, which bodies keep.
	float const maxLinearVelocity = 500.0f;
	float const maxAngularVelocity = 0.25f * glm::pi<float>() * 60.0f;
	auto const checkClamped = [](glm::vec3 const& velocity, glm::vec3 const& direction, float limit)
	{
		CHECK(glm::length(velocity) <= limit * 1.0001f);
		CHECK(glm::length(velocity) >= limit * 0.9999f);
		CHECK(glm::dot(glm::normalize(velocity), direction) > 0.9999f);
	};

	// Directions spread over the sphere (a Fibonacci lattice), and speeds from above the limits up to the largest float:
	// clamped velocities must be within the limits to the last bit, and squaring huge ones must not overflow.
	constexpr int DirectionCount = 64;
	for (int index = 0; index < DirectionCount; index++)
	{
		float const y = 1.0f - 2.0f * (static_cast<float>(index) + 0.5f) / static_cast<float>(DirectionCount);
		float const radius = std::sqrt(1.0f - y * y);
		float const angle = 2.39996323f * static_cast<float>(index);
		glm::vec3 const direction(radius * std::cos(angle), y, radius * std::sin(angle));
		for (float const speed : {1.0e3f, 1.0e30f, std::numeric_limits<float>::max()})
		{
			CAPTURE(index);
			CAPTURE(speed);
			BodyDesc ball = MakeBody(BallId, RigidBodyType::Dynamic, glm::vec3(0.0f), Sphere(0.5f));
			ball.LinearVelocity = direction * speed;
			ball.AngularVelocity = direction * speed;
			REQUIRE(world->AddBody(ball).IsOk());
			checkClamped(world->GetLinearVelocity(BallId), direction, maxLinearVelocity);
			checkClamped(world->GetAngularVelocity(BallId), direction, maxAngularVelocity);

			world->SetLinearVelocity(BallId, -direction * speed);
			world->SetAngularVelocity(BallId, -direction * speed);
			checkClamped(world->GetLinearVelocity(BallId), -direction, maxLinearVelocity);
			checkClamped(world->GetAngularVelocity(BallId), -direction, maxAngularVelocity);
		}
	}
}
