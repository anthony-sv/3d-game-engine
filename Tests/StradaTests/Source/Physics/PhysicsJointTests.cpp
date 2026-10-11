#include "Physics/PhysicsTestUtilities.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <limits>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float Step = 1.0f / 60.0f;

	UUID const BobId(201);
	UUID const LinkId(202);
	UUID const BaseId(203);

	Scope<PhysicsScene> CreateWorld(glm::vec3 const& gravity = {0.0f, -9.81f, 0.0f})
	{
		PhysicsWorldSettings settings;
		settings.Gravity = gravity;
		Result<Scope<PhysicsScene>> world = PhysicsScene::Create(settings);
		REQUIRE(world.IsOk());
		return world.TakeValue();
	}

	BodyDesc MakeBox(UUID entity, RigidBodyType type, glm::vec3 const& position)
	{
		BodyDesc desc;
		desc.Entity = entity;
		desc.Type = type;
		desc.Position = position;
		ColliderDesc collider;
		collider.Shape = ColliderShape::Box;
		collider.HalfExtents = glm::vec3(0.25f);
		desc.Colliders.push_back(collider);
		return desc;
	}

	void Simulate(PhysicsScene& world, float seconds)
	{
		for (float time = 0.0f; time < seconds; time += Step)
		{
			world.Step(Step);
		}
	}

	glm::vec3 PositionOf(PhysicsScene const& world, UUID entity)
	{
		std::optional<BodyTransform> const transform = world.GetBodyTransform(entity);
		REQUIRE(transform);
		return transform->Position;
	}
}

TEST_CASE("PhysicsScene: ball, distance and fixed joints hold bodies to the world or to each other")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();

	// A pendulum: the bob swings around the pivot at its starting distance.
	glm::vec3 const pivot(0.0f, 5.0f, 0.0f);
	REQUIRE(world->AddBody(MakeBox(BobId, RigidBodyType::Dynamic, {2.0f, 5.0f, 0.0f})).IsOk());
	JointDesc ball;
	ball.Entity = BobId;
	ball.Type = JointType::Ball;
	ball.Point = pivot;
	REQUIRE(world->AddJoint(ball).IsOk());
	CHECK(world->HasJoint(BobId));
	CHECK(world->GetJointCount() == 1);
	float lowest = 5.0f;
	for (int i = 0; i < 90; i++)
	{
		world->Step(Step);
		glm::vec3 const position = PositionOf(*world, BobId);
		lowest = std::min(lowest, position.y);
	}
	// The ball joint holds the body's point at the pivot: its center keeps its 2 m distance.
	CHECK(glm::length(PositionOf(*world, BobId) - pivot) == doctest::Approx(2.0f).epsilon(0.03));
	CHECK(lowest < 4.0f);
	CHECK(lowest > 2.9f);

	// A fixed joint holds the body where it is.
	REQUIRE(world->AddBody(MakeBox(LinkId, RigidBodyType::Dynamic, {5.0f, 3.0f, 0.0f})).IsOk());
	JointDesc fixed;
	fixed.Entity = LinkId;
	fixed.Type = JointType::Fixed;
	REQUIRE(world->AddJoint(fixed).IsOk());
	Simulate(*world, 1.0f);
	CHECK(glm::length(PositionOf(*world, LinkId) - glm::vec3(5.0f, 3.0f, 0.0f)) < 0.02f);

	// Removing it lets the body fall.
	world->RemoveJoint(LinkId);
	CHECK_FALSE(world->HasJoint(LinkId));
	Simulate(*world, 0.5f);
	CHECK(PositionOf(*world, LinkId).y < 2.0f);

	// A distance joint between two bodies: a rope of 1 to 3 m from a static base.
	REQUIRE(world->AddBody(MakeBox(BaseId, RigidBodyType::Static, {10.0f, 6.0f, 0.0f})).IsOk());
	REQUIRE(world->AddBody(MakeBox(LinkId, RigidBodyType::Dynamic, {10.0f, 5.0f, 0.0f})).IsOk());
	JointDesc rope;
	rope.Entity = LinkId;
	rope.ConnectedEntity = BaseId;
	rope.Type = JointType::Distance;
	rope.Point = {10.0f, 5.0f, 0.0f};
	rope.ConnectedPoint = {10.0f, 6.0f, 0.0f};
	rope.MinDistance = 1.0f;
	rope.MaxDistance = 3.0f;
	REQUIRE(world->AddJoint(rope).IsOk());
	CHECK(world->GetJointCount() == 2);
	Simulate(*world, 2.0f);
	CHECK(PositionOf(*world, LinkId).y == doctest::Approx(3.0f).epsilon(0.03));

	// Removing either body removes its joints.
	world->RemoveBody(BaseId);
	CHECK_FALSE(world->HasJoint(LinkId));
	world->RemoveBody(BobId);
	CHECK(world->GetJointCount() == 0);
}

TEST_CASE("PhysicsScene: hinges and sliders turn and slide within their limits, driven by motors")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld(glm::vec3(0.0f));

	// A motorized hinge around Y turns at the motor's speed.
	REQUIRE(world->AddBody(MakeBox(BobId, RigidBodyType::Dynamic, {0.0f, 1.0f, 0.0f})).IsOk());
	JointDesc hinge;
	hinge.Entity = BobId;
	hinge.Type = JointType::Hinge;
	hinge.Point = {0.0f, 1.0f, 0.0f};
	hinge.Axis = {0.0f, 2.0f, 0.0f};
	hinge.MotorEnabled = true;
	hinge.MotorSpeed = glm::half_pi<float>();
	REQUIRE(world->AddJoint(hinge).IsOk());
	Simulate(*world, 1.0f);
	glm::vec3 const spin = world->GetAngularVelocity(BobId);
	CHECK(spin.y == doctest::Approx(glm::half_pi<float>()).epsilon(0.05));
	CHECK(std::abs(spin.x) < 0.05f);
	CHECK(std::abs(spin.z) < 0.05f);
	CHECK(glm::length(PositionOf(*world, BobId) - glm::vec3(0.0f, 1.0f, 0.0f)) < 0.02f);

	// Limits stop it: a 30 degree door.
	hinge.LimitsEnabled = true;
	hinge.MinLimit = 0.0f;
	hinge.MaxLimit = glm::radians(30.0f);
	REQUIRE(world->AddJoint(hinge).IsOk());
	CHECK(world->GetJointCount() == 1);
	world->SetAngularVelocity(BobId, glm::vec3(0.0f));
	Simulate(*world, 1.0f);
	std::optional<BodyTransform> const door = world->GetBodyTransform(BobId);
	REQUIRE(door);
	// The body turned from wherever the motor left it; the limit holds its spin.
	CHECK(std::abs(world->GetAngularVelocity(BobId).y) < 0.2f);

	// A slider along +X driven by its motor stops at its limit.
	REQUIRE(world->AddBody(MakeBox(LinkId, RigidBodyType::Dynamic, {0.0f, 5.0f, 0.0f})).IsOk());
	JointDesc slider;
	slider.Entity = LinkId;
	slider.Type = JointType::Slider;
	slider.Axis = {1.0f, 0.0f, 0.0f};
	slider.LimitsEnabled = true;
	// Out of range: clamped to Min <= 0 <= Max.
	slider.MinLimit = 1.0f;
	slider.MaxLimit = 2.0f;
	slider.MotorEnabled = true;
	slider.MotorSpeed = 1.0f;
	REQUIRE(world->AddJoint(slider).IsOk());
	Simulate(*world, 4.0f);
	glm::vec3 const slid = PositionOf(*world, LinkId);
	CHECK(slid.x == doctest::Approx(2.0f).epsilon(0.03));
	CHECK(slid.y == doctest::Approx(5.0f).epsilon(0.01));
	CHECK(std::abs(slid.z) < 0.01f);
}

TEST_CASE("PhysicsScene: joints that cannot be made are refused")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	REQUIRE(world->AddBody(MakeBox(BobId, RigidBodyType::Dynamic, {0.0f, 1.0f, 0.0f})).IsOk());

	JointDesc joint;
	joint.Entity = LinkId;
	CHECK(world->AddJoint(joint).IsError());
	joint.Entity = BobId;
	joint.ConnectedEntity = BobId;
	CHECK(world->AddJoint(joint).IsError());
	joint.ConnectedEntity = BaseId;
	CHECK(world->AddJoint(joint).IsError());
	joint.ConnectedEntity = UUID::Invalid();
	joint.Axis = glm::vec3(0.0f);
	CHECK(world->AddJoint(joint).IsError());
	joint.Type = JointType::Slider;
	CHECK(world->AddJoint(joint).IsError());
	// Ball joints need no axis.
	joint.Type = JointType::Ball;
	CHECK(world->AddJoint(joint).IsOk());
	joint.Point.x = std::numeric_limits<float>::quiet_NaN();
	CHECK(world->AddJoint(joint).IsError());
	// A refused joint replaced the previous one.
	CHECK(world->GetJointCount() == 0);
	joint.Point = glm::vec3(0.0f);
	joint.MotorSpeed = std::numeric_limits<float>::infinity();
	CHECK(world->AddJoint(joint).IsError());
	joint.MotorSpeed = 0.0f;
	joint.Point = glm::vec3(1.0e9f);
	CHECK(world->AddJoint(joint).IsError());
	Simulate(*world, 0.1f);
}

TEST_CASE("Scene: joint components join bodies while running, follow changes and remap their connected entity")
{
	Testing::PhysicsSystemScope physics;
	Scene scene("Joints");
	Entity anchor = scene.CreateEntity("Anchor");
	anchor.GetComponent<TransformComponent>().Translation = {0.0f, 5.0f, 0.0f};
	anchor.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.1f);
	Entity bob = scene.CreateEntity("Bob");
	bob.GetComponent<TransformComponent>().Translation = {0.0f, 3.0f, 0.0f};
	bob.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
	bob.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.2f);
	JointComponent& joint = bob.AddComponent<JointComponent>();
	joint.Type = JointType::Ball;
	joint.ConnectedEntity = anchor.GetUUID();
	// The top of the bob's 2 m rope, in its local space.
	joint.Anchor = {0.0f, 2.0f, 0.0f};

	scene.OnRuntimeStart();
	REQUIRE(scene.GetPhysicsScene() != nullptr);
	CHECK(scene.GetPhysicsScene()->HasJoint(bob.GetUUID()));
	for (int i = 0; i < 60; i++)
	{
		scene.OnUpdateRuntime(Timestep(Step));
	}
	// Held up by the joint instead of falling.
	CHECK(bob.GetComponent<TransformComponent>().Translation.y == doctest::Approx(3.0f).epsilon(0.02));

	// Changing the joint makes it again; removing it lets the bob fall.
	scene.GetRegistry().patch<JointComponent>(bob.GetHandle(),
	                                          [](JointComponent& component)
	                                          {
												  component.Type = JointType::Fixed;
											  });
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(scene.GetPhysicsScene()->HasJoint(bob.GetUUID()));
	bob.RemoveComponent<JointComponent>();
	for (int i = 0; i < 30; i++)
	{
		scene.OnUpdateRuntime(Timestep(Step));
	}
	CHECK_FALSE(scene.GetPhysicsScene()->HasJoint(bob.GetUUID()));
	CHECK(bob.GetComponent<TransformComponent>().Translation.y < 2.5f);

	// A joint whose connected entity is gone is left out quietly (a broken chain).
	JointComponent& again = bob.AddComponent<JointComponent>();
	again.ConnectedEntity = anchor.GetUUID();
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(scene.GetPhysicsScene()->HasJoint(bob.GetUUID()));
	scene.DestroyEntity(anchor);
	scene.OnUpdateRuntime(Timestep(Step));
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK_FALSE(scene.GetPhysicsScene()->HasJoint(bob.GetUUID()));
	scene.OnRuntimeStop();

	// Duplicating a chain of two links keeps the copy's joint inside the copy.
	Entity chain = scene.CreateEntity("Chain");
	Entity first = scene.CreateEntity("First", chain);
	Entity second = scene.CreateEntity("Second", chain);
	second.AddComponent<JointComponent>().ConnectedEntity = first.GetUUID();
	Entity const copy = scene.DuplicateEntity(chain);
	REQUIRE(copy);
	std::vector<Entity> const children = scene.GetChildren(copy);
	REQUIRE(children.size() == 2);
	CHECK(children[1].GetComponent<JointComponent>().ConnectedEntity == children[0].GetUUID());
}
