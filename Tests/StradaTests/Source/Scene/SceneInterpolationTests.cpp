#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Math/Math.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float Step = 1.0f / 60.0f;
	constexpr float Frame144 = 1.0f / 144.0f;

	SceneRuntimeSettings PhysicsOnly()
	{
		SceneRuntimeSettings settings;
		settings.RunScripts = false;
		settings.PlayAudio = false;
		return settings;
	}

	void AddGround(Scene& scene)
	{
		Entity ground = scene.CreateEntity("Ground");
		ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
		ground.AddComponent<BoxColliderComponent>().HalfExtents = {20.0f, 0.5f, 20.0f};
	}

	// A box that nothing slows down or pulls: it keeps the velocities it starts with.
	Entity AddFloatingBody(Scene& scene, std::string const& name, glm::vec3 const& position, glm::vec3 const& velocity,
	                       glm::vec3 const& angularVelocity, RigidBodyInterpolation interpolation)
	{
		Entity entity = scene.CreateEntity(name);
		entity.GetComponent<TransformComponent>().Translation = position;
		RigidBodyComponent& body = entity.AddComponent<RigidBodyComponent>();
		body.Type = RigidBodyType::Dynamic;
		body.GravityFactor = 0.0f;
		body.LinearDamping = 0.0f;
		body.AngularDamping = 0.0f;
		body.AllowSleep = false;
		body.InitialLinearVelocity = velocity;
		body.InitialAngularVelocity = angularVelocity;
		body.Interpolation = interpolation;
		entity.AddComponent<BoxColliderComponent>().HalfExtents = {0.5f, 0.5f, 0.5f};
		return entity;
	}

	TransformComponent const& GetTransform(Entity entity)
	{
		return entity.GetComponent<TransformComponent>();
	}

	BodyTransform GetSimulated(Scene& scene, Entity entity)
	{
		std::optional<BodyTransform> const transform = scene.GetPhysicsScene()->GetBodyTransform(entity.GetUUID());
		REQUIRE(transform.has_value());
		return *transform;
	}

	float GetAngle(glm::quat const& a, glm::quat const& b)
	{
		return 2.0f * std::acos(std::clamp(std::abs(glm::dot(a, b)), 0.0f, 1.0f));
	}

	// Where a body was after each step: the poses its entity is drawn between.
	struct StepLog
	{
		std::vector<BodyTransform> Poses;

		// After a frame of at most one step: the pose after the steps run so far, recorded once per step.
		void Record(Scene& scene, Entity entity)
		{
			while (Poses.size() <= scene.GetFixedStepCount())
			{
				Poses.push_back(GetSimulated(scene, entity));
			}
		}
	};
}

TEST_CASE("ScenePhysics: a body at rest is drawn exactly where it is, frame after frame")
{
	Testing::PhysicsSystemScope physics;
	Scene scene("Rest");
	AddGround(scene);
	Entity const floating =
		AddFloatingBody(scene, "Floating", {3.0f, 2.0f, 0.0f}, glm::vec3(0.0f), glm::vec3(0.0f), RigidBodyInterpolation::Interpolate);
	Entity crate = scene.CreateEntity("Crate");
	crate.GetComponent<TransformComponent>().Translation = {0.0f, 1.5f, 0.0f};
	crate.GetComponent<TransformComponent>().Rotation = glm::angleAxis(0.3f, glm::vec3(0.0f, 1.0f, 0.0f));
	crate.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
	crate.AddComponent<BoxColliderComponent>().HalfExtents = {0.5f, 0.5f, 0.5f};
	scene.OnRuntimeStart(PhysicsOnly());
	REQUIRE(scene.GetPhysicsScene() != nullptr);
	TransformComponent const floatingStart = GetTransform(floating);

	// Frames of uneven lengths: every frame is drawn at another point between two steps.
	constexpr std::array<float, 5> FrameTimes = {1.0f / 144.0f, 1.0f / 97.0f, 1.0f / 200.0f, 1.0f / 60.0f, 1.0f / 31.0f};
	bool floatingStill = true;
	int frame = 0;
	for (; frame < 3000 && !scene.GetPhysicsScene()->IsSleeping(crate.GetUUID()); frame++)
	{
		scene.OnUpdateRuntime(Timestep(FrameTimes[static_cast<size_t>(frame) % FrameTimes.size()]));
		floatingStill = floatingStill && GetTransform(floating).Translation == floatingStart.Translation &&
		                GetTransform(floating).Rotation == floatingStart.Rotation;
	}
	// A body that is awake and does not move is never drawn anywhere else.
	CHECK(floatingStill);
	CHECK(GetTransform(floating).Translation == GetSimulated(scene, floating).Position);
	// The crate landed and fell asleep: from then on it is drawn where it lies, bit for bit, whatever the frame.
	REQUIRE(scene.GetPhysicsScene()->IsSleeping(crate.GetUUID()));
	CHECK(std::abs(GetTransform(crate).Translation.y - 0.5f) < 0.05f);
	TransformComponent const resting = GetTransform(crate);
	bool crateStill = true;
	for (int rest = 0; rest < 200; rest++, frame++)
	{
		scene.OnUpdateRuntime(Timestep(FrameTimes[static_cast<size_t>(frame) % FrameTimes.size()]));
		crateStill = crateStill && GetTransform(crate).Translation == resting.Translation &&
		             GetTransform(crate).Rotation == resting.Rotation && GetTransform(floating).Translation == floatingStart.Translation;
	}
	CHECK(crateStill);
	scene.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: a moving body is drawn between its last two steps, evenly at any frame rate")
{
	Testing::PhysicsSystemScope physics;
	Scene scene("Moving");
	constexpr glm::vec3 Velocity = {3.0f, 0.0f, 0.0f};
	Entity const smooth =
		AddFloatingBody(scene, "Smooth", {0.0f, 1.0f, 0.0f}, Velocity, {0.0f, 2.0f, 0.0f}, RigidBodyInterpolation::Interpolate);
	Entity const stepped =
		AddFloatingBody(scene, "Stepped", {0.0f, 1.0f, 5.0f}, Velocity, {0.0f, 2.0f, 0.0f}, RigidBodyInterpolation::None);
	scene.OnRuntimeStart(PhysicsOnly());
	REQUIRE(scene.GetPhysicsScene() != nullptr);

	StepLog steps;
	steps.Record(scene, smooth);
	std::vector<float> smoothX;
	std::vector<float> steppedX;
	bool between = true;
	bool turnedBetween = true;
	bool steppedAtSteps = true;
	for (int frame = 0; frame < 288; frame++)
	{
		scene.OnUpdateRuntime(Timestep(Frame144));
		steps.Record(scene, smooth);
		uint64_t const count = scene.GetFixedStepCount();
		TransformComponent const& drawn = GetTransform(smooth);
		smoothX.push_back(drawn.Translation.x);
		steppedX.push_back(GetTransform(stepped).Translation.x);
		steppedAtSteps = steppedAtSteps && GetTransform(stepped).Translation == GetSimulated(scene, stepped).Position;
		if (count == 0)
		{
			continue;
		}
		BodyTransform const& from = steps.Poses[count - 1];
		BodyTransform const& to = steps.Poses[count];
		// On the segment between its last two steps (a step behind the body), turned part of the way between them.
		between = between && drawn.Translation.x >= from.Position.x - 1e-5f && drawn.Translation.x <= to.Position.x + 1e-5f &&
		          std::abs(drawn.Translation.y - 1.0f) < 1e-5f && std::abs(drawn.Translation.z) < 1e-5f;
		turnedBetween = turnedBetween && std::abs(GetAngle(from.Rotation, drawn.Rotation) + GetAngle(drawn.Rotation, to.Rotation) -
		                                          GetAngle(from.Rotation, to.Rotation)) < 1e-3f;
	}
	CHECK(scene.GetFixedStepCount() >= 119);
	CHECK(between);
	CHECK(turnedBetween);
	// None draws the body where each step leaves it.
	CHECK(steppedAtSteps);

	// Drawn between steps, it moves as far in every frame as the frame's time makes up; drawn at its steps, it stands still
	// in frames without a step and jumps a whole step in the others.
	bool even = true;
	int stills = 0;
	int jumps = 0;
	// From the frames after the first step on (the first steps start from where the body was made).
	for (size_t frame = 4; frame < smoothX.size(); frame++)
	{
		even = even && std::abs(smoothX[frame] - smoothX[frame - 1] - Velocity.x * Frame144) < 1e-4f;
		float const steppedMove = steppedX[frame] - steppedX[frame - 1];
		stills += steppedMove == 0.0f ? 1 : 0;
		jumps += std::abs(steppedMove - Velocity.x * Step) < 1e-4f ? 1 : 0;
	}
	CHECK(even);
	CHECK(stills > 100);
	CHECK(jumps > 100);
	scene.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: a teleported body is drawn from where it was put, never on its way there")
{
	Testing::PhysicsSystemScope physics;
	Scene scene("Teleport");
	constexpr glm::vec3 Velocity = {6.0f, 0.0f, 0.0f};
	Entity body = AddFloatingBody(scene, "Body", {0.0f, 1.0f, 0.0f}, Velocity, glm::vec3(0.0f), RigidBodyInterpolation::Interpolate);
	scene.OnRuntimeStart(PhysicsOnly());
	REQUIRE(scene.GetPhysicsScene() != nullptr);

	// Teleported between frames (as by a script's OnUpdate or the editor), in frames that run a step and in frames that do
	// not, each time far from where it was drawn.
	bool inFrameWithoutStep = false;
	bool inFrameWithStep = false;
	for (int teleport = 0; teleport < 6; teleport++)
	{
		for (int frame = 0; frame < 30 + teleport; frame++)
		{
			scene.OnUpdateRuntime(Timestep(Frame144));
		}
		CAPTURE(teleport);
		glm::vec3 const target = {-20.0f * static_cast<float>(teleport % 2 == 0 ? 1 : -1), 3.0f + static_cast<float>(teleport), 0.0f};
		body.GetComponent<TransformComponent>().Translation = target;
		uint64_t const before = scene.GetFixedStepCount();
		scene.OnUpdateRuntime(Timestep(Frame144));
		if (scene.GetFixedStepCount() == before)
		{
			// No step took it yet: it is shown exactly where it was put.
			inFrameWithoutStep = true;
			CHECK(GetTransform(body).Translation == target);
		}
		else
		{
			inFrameWithStep = true;
		}
		// From then on it is drawn on its way from there, never between there and where it was.
		float elapsed = Frame144;
		bool fromTarget = true;
		for (int frame = 0; frame < 40; frame++)
		{
			glm::vec3 const drawn = GetTransform(body).Translation;
			fromTarget = fromTarget && drawn.x >= target.x - 1e-4f && drawn.x <= target.x + Velocity.x * (elapsed + Step) + 1e-4f &&
			             drawn.y == doctest::Approx(target.y);
			scene.OnUpdateRuntime(Timestep(Frame144));
			elapsed += Frame144;
		}
		CHECK(fromTarget);
	}
	CHECK(inFrameWithoutStep);
	CHECK(inFrameWithStep);
	scene.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: a body moved in OnUpdate is shown where it was put, and its drawing goes back exactly to its step")
{
	Testing::PhysicsSystemScope physics;
	Scene scene("Return");
	Entity body =
		AddFloatingBody(scene, "Body", {0.0f, 1.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, RigidBodyInterpolation::Interpolate);
	Entity other = AddFloatingBody(scene, "Other", {0.0f, 1.0f, 4.0f}, glm::vec3(0.0f), glm::vec3(0.0f), RigidBodyInterpolation::None);
	scene.OnRuntimeStart(PhysicsOnly());
	REQUIRE(scene.GetPhysicsScene() != nullptr);
	for (int frame = 0; frame < 25; frame++)
	{
		scene.OnUpdateRuntime(Timestep(Frame144));
	}
	// Drawn between two steps now; a change of a physics component (applied at once by a script's call, as by the next frame)
	// first takes it back to exactly where the last step left it, which is where its body is.
	BodyTransform const simulated = GetSimulated(scene, body);
	CHECK(GetTransform(body).Translation != simulated.Position);
	other.GetComponent<RigidBodyComponent>().Mass = 2.0f;
	scene.GetRegistry().patch<RigidBodyComponent>(other.GetHandle());
	scene.ApplyPhysicsChanges();
	CHECK(GetTransform(body).Translation == simulated.Position);
	CHECK(GetTransform(body).Rotation == Math::NormalizeRotation(simulated.Rotation));

	// Put somewhere by a frame's script: frames without a step leave it there, and the next step takes its body there.
	scene.OnUpdateRuntime(Timestep(Frame144));
	glm::vec3 const put = {7.0f, 2.0f, 1.0f};
	body.GetComponent<TransformComponent>().Translation = put;
	uint64_t const steps = scene.GetFixedStepCount();
	while (scene.GetFixedStepCount() == steps)
	{
		CHECK(GetTransform(body).Translation == put);
		scene.OnUpdateRuntime(Timestep(Frame144));
	}
	CHECK(GetSimulated(scene, body).Position.x == doctest::Approx(put.x + 2.0f * Step));
	scene.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: a kinematic body moved every frame is shown exactly where it was put, never behind")
{
	Testing::PhysicsSystemScope physics;
	Scene scene("Per frame");
	Entity platform = scene.CreateEntity("Platform");
	RigidBodyComponent& body = platform.AddComponent<RigidBodyComponent>();
	body.Type = RigidBodyType::Kinematic;
	body.Interpolation = RigidBodyInterpolation::Interpolate;
	platform.AddComponent<BoxColliderComponent>().HalfExtents = {1.0f, 0.1f, 1.0f};
	scene.OnRuntimeStart(PhysicsOnly());
	REQUIRE(scene.GetPhysicsScene() != nullptr);

	// Moved before every frame, as a script's OnUpdate or an animation moves it: each frame shows exactly that pose, in
	// frames that run a step and in frames that do not, and the steps take its body there.
	bool exact = true;
	int framesWithStep = 0;
	int framesWithoutStep = 0;
	for (int frame = 1; frame <= 144; frame++)
	{
		TransformComponent& transform = platform.GetComponent<TransformComponent>();
		glm::vec3 const put = {0.02f * static_cast<float>(frame), 1.0f + 0.01f * static_cast<float>(frame), 0.0f};
		glm::quat const turned = glm::angleAxis(0.01f * static_cast<float>(frame), glm::vec3(0.0f, 1.0f, 0.0f));
		transform.Translation = put;
		transform.Rotation = turned;
		uint64_t const before = scene.GetFixedStepCount();
		scene.OnUpdateRuntime(Timestep(Frame144));
		(scene.GetFixedStepCount() == before ? framesWithoutStep : framesWithStep)++;
		exact = exact && GetTransform(platform).Translation == put && GetTransform(platform).Rotation == turned;
	}
	CHECK(exact);
	CHECK(framesWithStep > 50);
	CHECK(framesWithoutStep > 50);
	CHECK(GetSimulated(scene, platform).Position.x > 2.5f);
	scene.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: bodies moved by fixed steps are drawn between them with their riders, and OnFixedUpdate reads the steps")
{
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting;
	Scene scene("Lift");
	AddGround(scene);
	auto const addLift = [&scene](std::string const& name, glm::vec3 const& position, RigidBodyInterpolation interpolation)
	{
		Entity made = scene.CreateEntity(name);
		made.GetComponent<TransformComponent>().Translation = position;
		RigidBodyComponent& madeBody = made.AddComponent<RigidBodyComponent>();
		madeBody.Type = RigidBodyType::Kinematic;
		madeBody.Interpolation = interpolation;
		made.AddComponent<BoxColliderComponent>().HalfExtents = {1.0f, 0.1f, 1.0f};
		ScriptComponent& mover = made.AddComponent<ScriptComponent>();
		mover.ClassName = "Strada.Tests.FixedStepMover";
		mover.Fields = ScriptFieldMap{{"Velocity", ScriptFieldValue::FromVector3({0.0f, 1.0f, 0.0f})}};
		return made;
	};
	Entity const lift = addLift("Lift", {0.0f, 0.5f, 0.0f}, RigidBodyInterpolation::Interpolate);
	Entity const steppedLift = addLift("Stepped Lift", {6.0f, 0.5f, 0.0f}, RigidBodyInterpolation::None);
	// A character stands on the lift, its feet at its entity.
	Entity rider = scene.CreateEntity("Rider");
	rider.GetComponent<TransformComponent>().Translation = {0.0f, 0.65f, 0.0f};
	CharacterControllerComponent& character = rider.AddComponent<CharacterControllerComponent>();
	character.Height = 1.6f;
	character.Radius = 0.3f;
	character.Offset = {0.0f, 0.8f, 0.0f};
	// A floating body reads where it is in its fixed updates.
	Entity probe =
		AddFloatingBody(scene, "Probe", {0.0f, 3.0f, 6.0f}, {2.0f, 0.0f, 0.0f}, glm::vec3(0.0f), RigidBodyInterpolation::Interpolate);
	ScriptComponent& probeScript = probe.AddComponent<ScriptComponent>();
	probeScript.ClassName = "Strada.Tests.FixedStepMover";

	scene.OnRuntimeStart();
	REQUIRE(scene.GetPhysicsScene() != nullptr);
	// The rider lands on the lift, which waits until then (its script moves it from the first fixed update: it starts
	// rising at once, so the rider lands on a rising lift).
	for (int frame = 0; frame < 72; frame++)
	{
		scene.OnUpdateRuntime(Timestep(Frame144));
	}
	StepLog probeSteps;
	probeSteps.Record(scene, probe);
	uint64_t const firstStep = scene.GetFixedStepCount();
	std::vector<float> liftY;
	std::vector<float> steppedY;
	float lowest = 1e9f;
	float highest = -1e9f;
	bool readTheSteps = true;
	int checkedReads = 0;
	for (int frame = 0; frame < 288; frame++)
	{
		uint64_t const before = scene.GetFixedStepCount();
		scene.OnUpdateRuntime(Timestep(Frame144));
		uint64_t const after = scene.GetFixedStepCount();
		probeSteps.Record(scene, probe);
		liftY.push_back(GetTransform(lift).Translation.y);
		steppedY.push_back(GetTransform(steppedLift).Translation.y);
		// The rider's feet stay on the lift's top as both are drawn between their steps.
		float const gap = GetTransform(rider).Translation.y - (GetTransform(lift).Translation.y + 0.1f);
		lowest = std::min(lowest, gap);
		highest = std::max(highest, gap);
		// In a frame of one step, its fixed update read exactly where the step before left the body.
		if (after == before + 1)
		{
			Result<ScriptFieldMap> const fields = scene.ReadScriptFields(probe);
			REQUIRE(fields.IsOk());
			readTheSteps = readTheSteps && fields.GetValue().at("FixedRead").GetVector3() == probeSteps.Poses[before].Position;
			checkedReads++;
		}
	}
	CHECK(readTheSteps);
	CHECK(checkedReads == static_cast<int>(scene.GetFixedStepCount() - firstStep));
	CHECK(checkedReads >= 119);
	CHECK(highest - lowest < 2e-3f);
	CHECK(lowest > -0.05f);
	CHECK(highest < 0.1f);

	bool even = true;
	int stills = 0;
	for (size_t frame = 1; frame < liftY.size(); frame++)
	{
		even = even && std::abs(liftY[frame] - liftY[frame - 1] - Frame144) < 1e-4f;
		stills += steppedY[frame] == steppedY[frame - 1] ? 1 : 0;
	}
	CHECK(even);
	CHECK(stills > 100);
	scene.OnRuntimeStop();
}
