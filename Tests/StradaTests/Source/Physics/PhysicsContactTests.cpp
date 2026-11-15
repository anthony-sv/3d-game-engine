#include "Physics/PhysicsTestUtilities.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/MeshSource.h"

#include <doctest/doctest.h>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float Gravity = 9.81f;
	// Jolt finds a contact once the bodies are this close (PhysicsSettings::mSpeculativeContactDistance).
	constexpr float SpeculativeDistance = 0.02f;

	UUID const GroundId(201);
	UUID const BallId(202);
	UUID const SensorId(203);
	UUID const HeroId(204);
	UUID const BoxId(205);

	AssetHandle const BallMaterial(UUID(5001));
	AssetHandle const GroundMaterial(UUID(5002));
	AssetHandle const LeftMaterial(UUID(5003));
	AssetHandle const RightMaterial(UUID(5004));
	AssetHandle const SensorMaterial(UUID(5005));

	Scope<PhysicsScene> CreateWorld()
	{
		Result<Scope<PhysicsScene>> world = PhysicsScene::Create({});
		REQUIRE(world.IsOk());
		return world.TakeValue();
	}

	ColliderDesc Box(glm::vec3 const& halfExtents, AssetHandle material = {})
	{
		ColliderDesc collider;
		collider.Shape = ColliderShape::Box;
		collider.HalfExtents = halfExtents;
		collider.Surface.Material = material;
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
	BodyDesc Ground(AssetHandle material = {})
	{
		return MakeBody(GroundId, RigidBodyType::Static, {0.0f, -0.5f, 0.0f}, Box({10.0f, 0.5f, 10.0f}, material));
	}

	// A ball of 2 kg and radius 0.5 whose fall nothing but gravity changes.
	BodyDesc Ball(glm::vec3 const& position, AssetHandle material = {})
	{
		BodyDesc ball = MakeBody(BallId, RigidBodyType::Dynamic, position, Sphere(0.5f, material));
		ball.Mass = 2.0f;
		ball.LinearDamping = 0.0f;
		ball.AngularDamping = 0.0f;
		return ball;
	}

	// The first event of a type for the pair (either order) among the events, if any.
	std::optional<ContactEvent> FindEvent(std::vector<ContactEvent> const& events, ContactEventType type, UUID a, UUID b)
	{
		auto const it = std::find_if(events.begin(), events.end(),
		                             [&](ContactEvent const& event)
		                             {
										 return event.Type == type &&
			                                    ((event.First == a && event.Second == b) || (event.First == b && event.Second == a));
									 });
		return it != events.end() ? std::optional<ContactEvent>(*it) : std::nullopt;
	}

	// An event's points as the entity sees them.
	std::vector<ContactPoint> PointsSeenBy(ContactEvent const& event, UUID entity)
	{
		REQUIRE((event.First == entity || event.Second == entity));
		std::vector<ContactPoint> points;
		for (uint32_t index = 0; index < event.PointCount; index++)
		{
			points.push_back(event.First == entity ? event.Points[index] : ReverseContactPoint(event.Points[index]));
		}
		return points;
	}

	// The stated order of an event's points: the larger impulse first, then the smaller x, y and z.
	bool AreSorted(ContactEvent const& event)
	{
		for (uint32_t index = 1; index < event.PointCount; index++)
		{
			ContactPoint const& before = event.Points[index - 1];
			ContactPoint const& after = event.Points[index];
			bool const inOrder = before.Impulse > after.Impulse ||
			                     (before.Impulse == after.Impulse && std::tuple(before.Point.x, before.Point.y, before.Point.z) <
			                                                             std::tuple(after.Point.x, after.Point.y, after.Point.z));
			if (!inOrder)
			{
				return false;
			}
		}
		return true;
	}

	std::vector<ContactEvent> Simulate(PhysicsScene& world, float step, int steps)
	{
		std::vector<ContactEvent> events;
		for (int index = 0; index < steps; index++)
		{
			world.Step(step);
			std::vector<ContactEvent> const stepEvents = world.TakeContactEvents();
			events.insert(events.end(), stepEvents.begin(), stepEvents.end());
		}
		return events;
	}

	// A static V of two faces sloping up at 30 degrees to each side of x = 0 (along Z, from z = -2 to 2), one submesh each:
	// the left face (x < 0) first.
	Ref<MeshSource> MakeGroove()
	{
		float const height = 2.0f * std::tan(glm::radians(30.0f));
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<Submesh> submeshes;
		std::vector<std::vector<glm::vec3>> const faces = {
			{{-2.0f, height, -2.0f}, {-2.0f, height, 2.0f}, {0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, -2.0f}},
			{{0.0f, 0.0f, -2.0f}, {0.0f, 0.0f, 2.0f}, {2.0f, height, 2.0f}, {2.0f, height, -2.0f}},
		};
		for (std::vector<glm::vec3> const& face : faces)
		{
			Submesh submesh;
			submesh.BaseVertex = static_cast<uint32_t>(vertices.size());
			submesh.VertexCount = 4;
			submesh.BaseIndex = static_cast<uint32_t>(indices.size());
			submesh.IndexCount = 6;
			for (glm::vec3 const& corner : face)
			{
				Vertex vertex;
				vertex.Position = corner;
				vertex.Normal = {0.0f, 1.0f, 0.0f};
				vertices.push_back(vertex);
			}
			// Facing up (the faces' front side is the one their corners turn counter-clockwise around).
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

TEST_CASE("PhysicsScene: a ball dropped on the ground reports where it lands, along which normal and how hard, with steps of 1/30, "
          "1/60 and 1/144 s")
{
	Testing::PhysicsSystemScope physics;
	// The ball's bottom starts 2 m above the ground: it lands at sqrt(2 g h).
	float const drop = 2.0f;
	float const landingSpeed = std::sqrt(2.0f * Gravity * drop);
	glm::vec3 const start(1.5f, drop + 0.5f, -0.75f);
	bool sawBallFirst = false;
	bool sawGroundFirst = false;
	for (float const step : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f})
	{
		for (float const restitution : {0.0f, 0.5f})
		{
			for (bool const ballFirst : {false, true})
			{
				CAPTURE(step);
				CAPTURE(restitution);
				CAPTURE(ballFirst);
				Scope<PhysicsScene> world = CreateWorld();
				BodyDesc ball = Ball(start);
				ball.Colliders[0].Surface.Restitution = restitution;
				// Jolt numbers bodies in the order they are added and reports the lower number first.
				if (ballFirst)
				{
					REQUIRE(world->AddBody(ball).IsOk());
					REQUIRE(world->AddBody(Ground()).IsOk());
				}
				else
				{
					REQUIRE(world->AddBody(Ground()).IsOk());
					REQUIRE(world->AddBody(ball).IsOk());
				}

				std::optional<ContactEvent> landing;
				float velocityBefore = 0.0f;
				for (int index = 0; index < 300 && !landing; index++)
				{
					velocityBefore = world->GetLinearVelocity(BallId).y;
					world->Step(step);
					landing = FindEvent(world->TakeContactEvents(), ContactEventType::CollisionEnter, BallId, GroundId);
					CHECK(world->GetStepCount() == static_cast<uint64_t>(index + 1));
				}
				REQUIRE(landing);
				CHECK(landing->Step == world->GetStepCount());
				CHECK((landing->First == BallId) == ballFirst);
				sawBallFirst = sawBallFirst || landing->First == BallId;
				sawGroundFirst = sawGroundFirst || landing->First == GroundId;
				REQUIRE(landing->PointCount == 1);

				// Gravity acts on the step's velocity before Jolt looks for contacts, so the ball comes in at the velocity it
				// had after the last step plus one step of gravity: within a step of gravity of the closed form.
				float const speed = -(velocityBefore - Gravity * step);
				CHECK(std::abs(speed - landingSpeed) <= Gravity * step);

				ContactPoint const point = PointsSeenBy(*landing, BallId)[0];
				// Below the ball's center, halfway between the surfaces: in the gap of up to 2 cm in which Jolt finds the
				// contact, or inside, as deep as half of what its last step took the ball below the ground.
				CHECK(point.Point.x == doctest::Approx(start.x).epsilon(1e-5));
				CHECK(point.Point.z == doctest::Approx(start.z).epsilon(1e-5));
				CHECK(point.Point.y <= 0.5f * SpeculativeDistance + 1e-4f);
				CHECK(point.Point.y >= -0.5f * speed * step - 1e-4f);
				// The ground pushes the ball up.
				CHECK(point.Normal.x == doctest::Approx(0.0f));
				CHECK(point.Normal.y == doctest::Approx(1.0f).epsilon(1e-6));
				CHECK(point.Normal.z == doctest::Approx(0.0f));
				CHECK(point.RelativeVelocity.x == doctest::Approx(0.0f));
				CHECK(point.RelativeVelocity.y == doctest::Approx(-speed).epsilon(1e-5));
				CHECK(point.RelativeVelocity.z == doctest::Approx(0.0f));
				// What stops a 2 kg ball and, with restitution, sends it back at that part of its speed (above Jolt's
				// 1 m/s threshold for restitution).
				CHECK(point.Impulse == doctest::Approx(2.0f * speed * (1.0f + restitution)).epsilon(1e-4));
				CHECK(point.Material == AssetHandle());
				CHECK(point.OtherMaterial == AssetHandle());

				// The ground sees the same point the other way round.
				ContactPoint const seenByGround = PointsSeenBy(*landing, GroundId)[0];
				CHECK(seenByGround.Point == point.Point);
				CHECK(seenByGround.Normal == -point.Normal);
				CHECK(seenByGround.RelativeVelocity == -point.RelativeVelocity);
				CHECK(seenByGround.Impulse == point.Impulse);

				// One landing; a bounce that leaves the ground ends with an exit, which carries no points.
				std::vector<ContactEvent> const after = Simulate(*world, step, static_cast<int>(1.0f / step));
				for (ContactEvent const& event : after)
				{
					CHECK(event.Step > landing->Step);
					CHECK(event.Step <= world->GetStepCount());
					if (event.Type == ContactEventType::CollisionExit)
					{
						CHECK(event.PointCount == 0);
					}
				}
				if (restitution == 0.0f)
				{
					CHECK(after.empty());
				}
				else
				{
					CHECK(FindEvent(after, ContactEventType::CollisionExit, BallId, GroundId).has_value());
				}
			}
		}
	}
	CHECK(sawBallFirst);
	CHECK(sawGroundFirst);
}

TEST_CASE("PhysicsScene: contact points name both sides' physics materials, a submesh's on a triangle mesh, and triggers' points "
          "carry no impulse")
{
	Testing::PhysicsSystemScope physics;

	SUBCASE("A ball's material and the ground's")
	{
		Scope<PhysicsScene> world = CreateWorld();
		REQUIRE(world->AddBody(Ground(GroundMaterial)).IsOk());
		REQUIRE(world->AddBody(Ball({0.0f, 1.5f, 0.0f}, BallMaterial)).IsOk());
		std::optional<ContactEvent> const landing =
			FindEvent(Simulate(*world, 1.0f / 60.0f, 60), ContactEventType::CollisionEnter, BallId, GroundId);
		REQUIRE(landing);
		REQUIRE(landing->PointCount == 1);
		ContactPoint const ball = PointsSeenBy(*landing, BallId)[0];
		CHECK(ball.Material == BallMaterial);
		CHECK(ball.OtherMaterial == GroundMaterial);
		ContactPoint const ground = PointsSeenBy(*landing, GroundId)[0];
		CHECK(ground.Material == GroundMaterial);
		CHECK(ground.OtherMaterial == BallMaterial);
	}

	SUBCASE("A ball in a groove touches both faces in one step: one event, a point on each face with its normal and material")
	{
		ColliderDesc groove;
		groove.Shape = ColliderShape::TriangleMesh;
		groove.Mesh = MakeGroove();
		groove.Surface.Material = GroundMaterial;
		groove.SubmeshSurfaces.resize(2);
		groove.SubmeshSurfaces[0].Material = LeftMaterial;
		groove.SubmeshSurfaces[1].Material = RightMaterial;
		Scope<PhysicsScene> world = CreateWorld();
		REQUIRE(world->AddBody(MakeBody(GroundId, RigidBodyType::Static, glm::vec3(0.0f), groove)).IsOk());
		REQUIRE(world->AddBody(Ball({0.0f, 3.0f, 0.0f}, BallMaterial)).IsOk());
		std::vector<ContactEvent> const events = Simulate(*world, 1.0f / 60.0f, 90);
		CHECK(std::count_if(events.begin(), events.end(),
		                    [](ContactEvent const& event)
		                    {
								return event.Type == ContactEventType::CollisionEnter;
							}) == 1);
		std::optional<ContactEvent> const landing = FindEvent(events, ContactEventType::CollisionEnter, BallId, GroundId);
		REQUIRE(landing);
		REQUIRE(landing->PointCount == 2);
		CHECK(AreSorted(*landing));
		std::vector<ContactPoint> const points = PointsSeenBy(*landing, BallId);
		// Each face pushes the ball up and away from itself, 30 degrees from the vertical.
		ContactPoint const& left = points[0].Normal.x > 0.0f ? points[0] : points[1];
		ContactPoint const& right = points[0].Normal.x > 0.0f ? points[1] : points[0];
		CHECK(left.Normal.x == doctest::Approx(0.5f).epsilon(1e-4));
		CHECK(left.Normal.y == doctest::Approx(std::sqrt(0.75f)).epsilon(1e-4));
		CHECK(right.Normal.x == doctest::Approx(-0.5f).epsilon(1e-4));
		CHECK(right.Normal.y == doctest::Approx(std::sqrt(0.75f)).epsilon(1e-4));
		CHECK(left.Point.x < 0.0f);
		CHECK(right.Point.x > 0.0f);
		CHECK(left.OtherMaterial == LeftMaterial);
		CHECK(right.OtherMaterial == RightMaterial);
		CHECK(left.Material == BallMaterial);
		CHECK(right.Material == BallMaterial);
		// The two faces share the landing: each takes a part of what stops the ball.
		CHECK(left.Impulse > 0.0f);
		CHECK(right.Impulse > 0.0f);
	}

	SUBCASE("A trigger's points have no impulse, and its exit no points")
	{
		Scope<PhysicsScene> world = CreateWorld();
		REQUIRE(world->AddBody(Ground()).IsOk());
		ColliderDesc sensor = Box({2.0f, 0.5f, 2.0f}, SensorMaterial);
		sensor.IsTrigger = true;
		REQUIRE(world->AddBody(MakeBody(SensorId, RigidBodyType::Static, {0.0f, 2.0f, 0.0f}, sensor)).IsOk());
		REQUIRE(world->AddBody(Ball({0.0f, 4.0f, 0.0f}, BallMaterial)).IsOk());
		std::vector<ContactEvent> const events = Simulate(*world, 1.0f / 60.0f, 120);
		std::optional<ContactEvent> const entered = FindEvent(events, ContactEventType::TriggerEnter, BallId, SensorId);
		std::optional<ContactEvent> const left = FindEvent(events, ContactEventType::TriggerExit, BallId, SensorId);
		REQUIRE(entered);
		REQUIRE(left);
		REQUIRE(entered->PointCount >= 1);
		for (ContactPoint const& point : PointsSeenBy(*entered, BallId))
		{
			CHECK(point.Impulse == 0.0f);
			CHECK(point.Material == BallMaterial);
			CHECK(point.OtherMaterial == SensorMaterial);
			CHECK(point.RelativeVelocity.y < 0.0f);
		}
		CHECK(left->PointCount == 0);
		CHECK(left->Step > entered->Step);
		CHECK(FindEvent(events, ContactEventType::CollisionEnter, BallId, GroundId).has_value());
	}

	SUBCASE("A character landing on static ground: no dynamic body, so no impulse; its inner body has no material")
	{
		Scope<PhysicsScene> world = CreateWorld();
		REQUIRE(world->AddBody(Ground(GroundMaterial)).IsOk());
		CharacterDesc hero;
		hero.Entity = HeroId;
		hero.Position = {0.0f, 2.0f, 0.0f};
		hero.Radius = 0.3f;
		hero.Height = 1.6f;
		REQUIRE(world->AddCharacter(hero).IsOk());
		std::optional<ContactEvent> const landing =
			FindEvent(Simulate(*world, 1.0f / 60.0f, 120), ContactEventType::CollisionEnter, HeroId, GroundId);
		REQUIRE(landing);
		REQUIRE(landing->PointCount >= 1);
		for (ContactPoint const& point : PointsSeenBy(*landing, HeroId))
		{
			CHECK(point.Impulse == 0.0f);
			CHECK(std::isfinite(point.Point.x));
			CHECK(std::isfinite(point.Point.y));
			CHECK(std::isfinite(point.Point.z));
			CHECK(point.Normal.y > 0.99f);
			CHECK(point.Material == AssetHandle());
			CHECK(point.OtherMaterial == GroundMaterial);
		}
	}
}

TEST_CASE("PhysicsScene: a box landing flat reports four points, strongest first, which stop it together")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	REQUIRE(world->AddBody(Ground()).IsOk());
	// Tipped a little (its corners a millimeter or two apart in height, all within Jolt's manifold tolerance), so they do
	// not land alike.
	BodyDesc box = MakeBody(BoxId, RigidBodyType::Dynamic, {0.3f, 1.5f, -0.2f}, Box(glm::vec3(0.5f)));
	box.Rotation = glm::angleAxis(0.002f, glm::normalize(glm::vec3(1.0f, 0.0f, 0.5f)));
	box.Mass = 3.0f;
	box.LinearDamping = 0.0f;
	box.AngularDamping = 0.0f;
	REQUIRE(world->AddBody(box).IsOk());
	std::optional<ContactEvent> landing;
	float speed = 0.0f;
	for (int index = 0; index < 120 && !landing; index++)
	{
		speed = -(world->GetLinearVelocity(BoxId).y - Gravity / 60.0f);
		world->Step(1.0f / 60.0f);
		landing = FindEvent(world->TakeContactEvents(), ContactEventType::CollisionEnter, BoxId, GroundId);
	}
	REQUIRE(landing);
	REQUIRE(landing->PointCount == MaxContactPoints);
	CHECK(AreSorted(*landing));
	float total = 0.0f;
	for (ContactPoint const& point : PointsSeenBy(*landing, BoxId))
	{
		CHECK(point.Normal.y == doctest::Approx(1.0f).epsilon(1e-5));
		CHECK(std::abs(point.Point.y) <= 0.5f * SpeculativeDistance + 0.01f);
		CHECK(point.Impulse > 0.0f);
		total += point.Impulse;
	}
	// Together they stop the 3 kg box (Jolt's estimate iterates, as its solver does: within 2 %).
	CHECK(total == doctest::Approx(3.0f * speed).epsilon(0.02));
}

TEST_CASE("PhysicsScene: two worlds built and stepped alike report the same events with the same points, in step order")
{
	Testing::PhysicsSystemScope physics;
	auto const run = []
	{
		Scope<PhysicsScene> world = CreateWorld();
		REQUIRE(world->AddBody(Ground(GroundMaterial)).IsOk());
		ColliderDesc sensor = Box({3.0f, 0.25f, 3.0f}, SensorMaterial);
		sensor.IsTrigger = true;
		REQUIRE(world->AddBody(MakeBody(SensorId, RigidBodyType::Static, {0.0f, 1.0f, 0.0f}, sensor)).IsOk());
		// A pile of balls and boxes that fall onto each other and the ground, bounce and roll.
		for (uint64_t index = 0; index < 12; index++)
		{
			float const x = static_cast<float>(index % 3) * 0.7f - 0.7f;
			float const z = static_cast<float>((index / 3) % 2) * 0.6f - 0.3f;
			float const y = 2.0f + static_cast<float>(index) * 0.8f;
			ColliderDesc collider = index % 2 == 0 ? Sphere(0.35f, BallMaterial) : Box(glm::vec3(0.3f), LeftMaterial);
			collider.Surface.Restitution = 0.3f;
			BodyDesc body = MakeBody(UUID(1000 + index), RigidBodyType::Dynamic, {x, y, z}, collider);
			body.Rotation = glm::angleAxis(0.2f * static_cast<float>(index), glm::normalize(glm::vec3(1.0f, 2.0f, 0.5f)));
			REQUIRE(world->AddBody(body).IsOk());
		}
		return Simulate(*world, 1.0f / 60.0f, 240);
	};
	std::vector<ContactEvent> const first = run();
	std::vector<ContactEvent> const second = run();
	CHECK(first.size() > 20);
	CHECK(first == second);

	size_t withPoints = 0;
	for (size_t index = 0; index < first.size(); index++)
	{
		ContactEvent const& event = first[index];
		bool const enter = event.Type == ContactEventType::CollisionEnter || event.Type == ContactEventType::TriggerEnter;
		CHECK((enter ? event.PointCount >= 1 : event.PointCount == 0));
		CHECK(event.PointCount <= MaxContactPoints);
		CHECK(AreSorted(event));
		withPoints += event.PointCount > 1 ? 1 : 0;
		if (index > 0)
		{
			// In step order, and within a step by First, Second and Type.
			ContactEvent const& previous = first[index - 1];
			CHECK(previous.Step <= event.Step);
			if (previous.Step == event.Step)
			{
				CHECK(std::tuple(previous.First.GetValue(), previous.Second.GetValue(), previous.Type) <
				      std::tuple(event.First.GetValue(), event.Second.GetValue(), event.Type));
			}
		}
	}
	// The boxes land on faces and edges.
	CHECK(withPoints > 0);
}
