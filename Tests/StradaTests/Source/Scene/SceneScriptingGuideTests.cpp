#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <optional>
#include <string>
#include <string_view>

using namespace Strada;

namespace
{
	constexpr float Step = 1.0f / 60.0f;
	glm::vec3 const Up(0.0f, 1.0f, 0.0f);
	glm::vec3 const Right(1.0f, 0.0f, 0.0f);

	Entity AddScript(Scene& scene, std::string const& name, std::string const& className, ScriptFieldMap fields = {})
	{
		Entity entity = scene.CreateEntity(name);
		ScriptComponent& script = entity.AddComponent<ScriptComponent>();
		script.ClassName = className;
		script.Fields = std::move(fields);
		return entity;
	}

	// Input is global: tests that set it restore it.
	class InputScope
	{
	public:
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }

		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	// A static box from its lowest to its highest corner.
	void AddBlock(Scene& scene, std::string const& name, glm::vec3 const& lowest, glm::vec3 const& highest)
	{
		Entity block = scene.CreateEntity(name);
		block.GetComponent<TransformComponent>().Translation = 0.5f * (lowest + highest);
		block.AddComponent<BoxColliderComponent>().HalfExtents = 0.5f * (highest - lowest);
	}

	// Whether a rotation turns by the angle (in degrees) around the axis; q and -q are the same rotation.
	bool IsTurn(glm::quat const& rotation, float degrees, glm::vec3 const& axis)
	{
		return std::abs(glm::dot(rotation, glm::angleAxis(glm::radians(degrees), axis))) > 0.99999f;
	}

	std::string ReadText(char const* path)
	{
		Result<std::string> text = FileSystem::ReadTextFile(path);
		REQUIRE_MESSAGE(text.IsOk(), path);
		std::string result = text.TakeValue();
		std::erase(result, '\r');
		return result;
	}

	// The text of an example class, from "public class <name>" to its closing brace; empty when there is none.
	std::string FindClass(std::string const& text, std::string const& name)
	{
		size_t const begin = text.find("public class " + name + " : Script\n{\n");
		size_t const end = begin != std::string::npos ? text.find("\n}\n", begin) : std::string::npos;
		return end != std::string::npos ? text.substr(begin, end + 3 - begin) : std::string();
	}

	// The jumping test player: a 1.8 m capsule with a radius of 0.4 m (its center 0.9 m above its feet), and the controller's
	// default jump speed.
	constexpr float FeetToCenter = 0.9f;
	constexpr float JumpSpeed = 5.0f;

	// A deck 1.6 m high on the ground, with seven stairs, 20 cm high and 30 cm deep, down along -Z from its edge at z = 0 and
	// a plain ledge at x = 3; on it, a player with the guide's first-person controller walking at the speed. Returns the
	// player.
	Entity BuildDeck(Scene& scene, float speed)
	{
		AddBlock(scene, "Ground", {-20.0f, -1.0f, -20.0f}, {20.0f, 0.0f, 20.0f});
		AddBlock(scene, "Deck", {-3.0f, 0.0f, 0.0f}, {3.0f, 1.6f, 6.0f});
		for (int stair = 1; stair <= 7; stair++)
		{
			float const index = static_cast<float>(stair);
			AddBlock(scene, "Stair", {-3.0f, 0.0f, -0.3f * index}, {3.0f, 1.6f - 0.2f * index, -0.3f * (index - 1.0f)});
		}
		Entity player = AddScript(scene, "Player", "Strada.Tests.FirstPersonController", {{"Speed", ScriptFieldValue::FromFloat(speed)}});
		player.GetComponent<TransformComponent>().Translation = {0.0f, 1.6f + FeetToCenter, 3.0f};
		CharacterControllerComponent& character = player.AddComponent<CharacterControllerComponent>();
		character.Radius = 0.4f;
		character.Height = 2.0f * FeetToCenter;
		Entity head = scene.CreateEntity("Head", player);
		head.GetComponent<TransformComponent>().Translation = {0.0f, 0.7f, 0.0f};
		player.GetComponent<ScriptComponent>().Fields["Head"] = ScriptFieldValue::FromEntity(head.GetUUID());
		return player;
	}

	// Plays the player of a running deck scene with the keyboard, frame by frame, and reads what its character does.
	class PlayerDriver
	{
	public:
		PlayerDriver(Scene& scene, Entity player)
			: m_Scene(scene),
			  m_Player(player)
		{
		}

		void Frame()
		{
			m_Scene.OnUpdateRuntime(Timestep(Step));
			Input::EndFrame();
		}

		CharacterState State() const
		{
			std::optional<CharacterState> const state = m_Scene.GetPhysicsScene()->GetCharacterState(m_Player.GetUUID());
			REQUIRE(state);
			return *state;
		}

		float GetFeetHeight() const { return State().Position.y - FeetToCenter; }

		// Lets go of every key and places the player's feet at a spot, at rest; it settles there.
		void Place(glm::vec3 const& feet)
		{
			Input::ReleaseAll();
			Frame();
			m_Scene.TeleportCharacter(m_Player, feet + glm::vec3(0.0f, FeetToCenter, 0.0f));
			for (int i = 0; i < 5; i++)
			{
				Frame();
			}
			REQUIRE(State().IsGrounded);
		}

		// Space, pressed for a frame.
		void PressJump()
		{
			Input::SetKeyState(KeyCode::Space, true);
			Frame();
			Input::SetKeyState(KeyCode::Space, false);
		}

		// Presses Space; true when the character jumps (rises at about the jump speed) within the frames, this one included.
		bool JumpsWithin(int frames)
		{
			for (int i = 0; i < frames; i++)
			{
				if (i == 0)
				{
					PressJump();
				}
				else
				{
					Frame();
				}
				if (State().Velocity.y > 0.8f * JumpSpeed)
				{
					return true;
				}
			}
			return false;
		}

		// Holds the key until the character leaves the ground; false when it does not within the frames.
		bool WalkUntilInAir(KeyCode key, int frames)
		{
			Input::SetKeyState(key, true);
			for (int i = 0; i < frames; i++)
			{
				Frame();
				if (!State().IsGrounded)
				{
					return true;
				}
			}
			return false;
		}

	private:
		Scene& m_Scene;
		Entity m_Player;
	};
}

TEST_CASE("Scene: the scripting guide shows the example scripts that these tests run")
{
	std::string const guide = ReadText(STRADA_SCRIPTING_GUIDE_PATH);
	std::string const examples = ReadText(STRADA_GUIDE_EXAMPLES_PATH);
	for (char const* name : {"Spinner", "FirstPersonController"})
	{
		CAPTURE(name);
		std::string const shown = FindClass(guide, name);
		REQUIRE_FALSE(shown.empty());
		CHECK(examples.find(shown) != std::string::npos);
	}
}

TEST_CASE("Scene: the scripting guide's Spinner turns all the way around")
{
	Testing::ScriptEngineScope scripting;
	Scene scene("Spinner");
	Entity const spinner = AddScript(scene, "Spinner", "Strada.Tests.Spinner");
	TransformComponent const& transform = spinner.GetComponent<TransformComponent>();

	// 90 degrees a second, 1.5 a frame: past a quarter turn, then on around and past a whole turn.
	scene.OnRuntimeStart();
	int frames = 0;
	for (float const degrees : {60.0f, 135.0f, 180.0f, 270.0f, 360.0f, 450.0f})
	{
		for (; static_cast<float>(frames) * 1.5f < degrees; frames++)
		{
			scene.OnUpdateRuntime(Timestep(Step));
		}
		CAPTURE(degrees);
		CHECK(IsTurn(transform.Rotation, degrees, Up));
	}
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: the scripting guide's first-person controller looks all the way around")
{
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting;
	InputScope input;
	Scene scene("First person");
	AddBlock(scene, "Ground", {-20.0f, -1.0f, -20.0f}, {20.0f, 0.0f, 20.0f});
	Entity player = AddScript(scene, "Player", "Strada.Tests.FirstPersonController");
	TransformComponent& transform = player.GetComponent<TransformComponent>();
	// Standing on the ground (the 2 m capsule's center is 1 m up), facing 30 degrees to the left of -Z.
	transform.Translation = {0.0f, 1.0f, 0.0f};
	transform.Rotation = glm::angleAxis(glm::radians(30.0f), Up);
	player.AddComponent<CharacterControllerComponent>();
	Entity head = scene.CreateEntity("Head", player);
	head.GetComponent<TransformComponent>().Translation = {0.0f, 0.7f, 0.0f};
	head.AddComponent<CameraComponent>();
	player.GetComponent<ScriptComponent>().Fields["Head"] = ScriptFieldValue::FromEntity(head.GetUUID());
	TransformComponent const& headTransform = head.GetComponent<TransformComponent>();
	glm::vec2 mouse(0.0f);
	Input::SetMousePosition(mouse);
	auto const frame = [&scene, &mouse](glm::vec2 const& move)
	{
		mouse += move;
		Input::SetMousePosition(mouse);
		scene.OnUpdateRuntime(Timestep(Step));
		Input::EndFrame();
	};

	scene.OnRuntimeStart();
	CHECK(Input::GetCursorMode() == CursorMode::Locked);
	// It starts looking where the entity faces.
	frame(glm::vec2(0.0f));
	CHECK(IsTurn(transform.Rotation, 30.0f, Up));

	// Moving the mouse to the right turns the body to the right, steadily, past every quarter turn and all the way around: 10
	// pixels a frame are 1.5 degrees at the default sensitivity.
	int firstWrongFrame = 0;
	for (int i = 1; i <= 300; i++)
	{
		frame({10.0f, 0.0f});
		if (firstWrongFrame == 0 && !IsTurn(transform.Rotation, 30.0f - 1.5f * static_cast<float>(i), Up))
		{
			firstWrongFrame = i;
		}
	}
	CHECK(firstWrongFrame == 0);
	CHECK(IsTurn(headTransform.Rotation, 0.0f, Right));
	// Turning does not move it.
	CHECK(glm::length(transform.Translation - glm::vec3(0.0f, 1.0f, 0.0f)) < 0.01f);

	// Moving it down tilts the head down, up tilts it up, 85 degrees at most; the body does not tilt.
	frame({0.0f, 1000.0f});
	CHECK(IsTurn(headTransform.Rotation, -85.0f, Right));
	frame({0.0f, -2000.0f});
	CHECK(IsTurn(headTransform.Rotation, 85.0f, Right));
	CHECK(IsTurn(transform.Rotation, 30.0f - 450.0f, Up));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: the scripting guide's first-person controller jumps at any moment of walking down stairs")
{
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting;
	InputScope input;
	// Slowly and at the controller's default speed: the stairs' edges meet the capsule in different places.
	for (float const speed : {2.0f, 5.0f})
	{
		CAPTURE(speed);
		Scene scene("Stairs");
		PlayerDriver driver(scene, BuildDeck(scene, speed));
		scene.OnRuntimeStart();

		// From 1.5 m before the edge down the stairs and on along the ground (4 m). On the way, the capsule's round bottom rolls
		// over the edges of some stairs and leaves the ground for a step.
		int const walkFrames = static_cast<int>(std::ceil(4.0f / speed / Step));
		driver.Place({0.0f, 1.6f, 1.5f});
		Input::SetKeyState(KeyCode::W, true);
		int framesInAir = 0;
		for (int i = 0; i < walkFrames; i++)
		{
			driver.Frame();
			framesInAir += driver.State().IsGrounded ? 0 : 1;
		}
		CHECK(framesInAir > 0);
		CHECK(driver.State().IsGrounded);
		CHECK(std::abs(driver.GetFeetHeight()) < 0.01f);
		CHECK(driver.State().Position.z < -2.1f);

		// A jump pressed at any frame of the way happens: on the deck, at its edge, on and between the stairs and below them.
		int droppedJumps = 0;
		for (int pressFrame = 0; pressFrame < walkFrames; pressFrame++)
		{
			driver.Place({0.0f, 1.6f, 1.5f});
			Input::SetKeyState(KeyCode::W, true);
			for (int i = 0; i < pressFrame; i++)
			{
				driver.Frame();
			}
			if (!driver.JumpsWithin(8))
			{
				droppedJumps++;
				MESSAGE("the jump pressed after " << pressFrame << " frames of walking was dropped");
			}
		}
		CHECK(droppedJumps == 0);
		scene.OnRuntimeStop();
	}
}

TEST_CASE("Scene: the scripting guide's first-person controller jumps just after leaving the ground and just before landing")
{
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting;
	InputScope input;
	Scene scene("Ledge");
	PlayerDriver driver(scene, BuildDeck(scene, 5.0f));
	scene.OnRuntimeStart();

	// Walking off the side of the deck (1.6 m high): pressed three frames after leaving the ground, it still jumps (coyote
	// time).
	driver.Place({2.0f, 1.6f, 3.0f});
	REQUIRE(driver.WalkUntilInAir(KeyCode::D, 60));
	driver.Frame();
	driver.Frame();
	REQUIRE_FALSE(driver.State().IsGrounded);
	CHECK(driver.JumpsWithin(2));

	// Pressed later in the fall, it neither jumps in the air nor when it lands, well after the press.
	driver.Place({2.0f, 1.6f, 3.0f});
	REQUIRE(driver.WalkUntilInAir(KeyCode::D, 60));
	for (int i = 0; i < 9; i++)
	{
		driver.Frame();
	}
	REQUIRE_FALSE(driver.State().IsGrounded);
	CHECK_FALSE(driver.JumpsWithin(60));
	CHECK(driver.State().IsGrounded);

	// Pressed just before it lands, it jumps as it lands (the jump buffer): from the ground, not in the air.
	driver.Place({2.0f, 1.6f, 3.0f});
	REQUIRE(driver.WalkUntilInAir(KeyCode::D, 60));
	Input::ReleaseAll();
	for (int i = 0; i < 120 && driver.GetFeetHeight() > 0.2f; i++)
	{
		driver.Frame();
	}
	REQUIRE(driver.GetFeetHeight() <= 0.2f);
	REQUIRE_FALSE(driver.State().IsGrounded);
	bool landed = false;
	bool jumped = false;
	for (int i = 0; i < 8 && !jumped; i++)
	{
		if (i == 0)
		{
			driver.PressJump();
		}
		else
		{
			driver.Frame();
		}
		CharacterState const now = driver.State();
		jumped = now.Velocity.y > 0.8f * JumpSpeed;
		landed = landed || now.IsGrounded;
	}
	CHECK(jumped);
	CHECK(landed);
	CHECK(driver.GetFeetHeight() < 0.15f);
	scene.OnRuntimeStop();
}
