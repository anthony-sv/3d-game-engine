#include "Audio/AudioTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <charconv>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
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
}

TEST_CASE("Scene: scripts read and write every component field")
{
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Components");
	AssetHandle const mesh = GetBuiltInHandle(BuiltInAsset::CubeMesh);
	AssetHandle const clip(UUID(4242));
	AssetHandle const font = GetBuiltInHandle(BuiltInAsset::DefaultFont);
	Entity writer = AddScript(scene, "Writer", "Strada.Tests.ComponentWriter",
	                          {{"ExpectedMesh", ScriptFieldValue::FromAsset(mesh)},
	                           {"ExpectedClip", ScriptFieldValue::FromAsset(clip)},
	                           {"ExpectedFont", ScriptFieldValue::FromAsset(font)}});
	writer.AddComponent<MeshComponent>().Mesh = mesh;
	writer.AddComponent<AudioSourceComponent>().Clip = clip;
	writer.AddComponent<TextComponent>().Font = font;

	scene.OnRuntimeStart();
	CHECK_FALSE(Testing::WasLogged(logStart, "Mismatch"));
	CHECK(writer.GetName() == "Checked 87");

	CameraComponent const& camera = writer.GetComponent<CameraComponent>();
	CHECK(camera.Projection == ProjectionType::Orthographic);
	CHECK(camera.PerspectiveFOV == 50.0f);
	CHECK(camera.PerspectiveNear == 0.5f);
	CHECK(camera.PerspectiveFar == 500.0f);
	CHECK(camera.OrthographicSize == 20.0f);
	CHECK(camera.OrthographicNear == -50.0f);
	CHECK(camera.OrthographicFar == 60.0f);
	CHECK_FALSE(camera.Primary);
	CHECK(camera.FixedAspectRatio);
	CHECK(camera.AspectRatio == 2.0f);

	MeshComponent const& meshComponent = writer.GetComponent<MeshComponent>();
	CHECK(meshComponent.Mesh == mesh);
	CHECK_FALSE(meshComponent.CastShadows);
	CHECK_FALSE(meshComponent.Visible);

	DirectionalLightComponent const& sun = writer.GetComponent<DirectionalLightComponent>();
	CHECK(sun.Color == glm::vec3(1.0f, 0.5f, 0.25f));
	CHECK(sun.Intensity == 4.0f);
	CHECK_FALSE(sun.CastShadows);
	CHECK(sun.LightSize == 2.0f);

	PointLightComponent const& point = writer.GetComponent<PointLightComponent>();
	CHECK(point.Color == glm::vec3(0.5f, 0.25f, 1.0f));
	CHECK(point.Intensity == 7.0f);
	CHECK(point.Range == 12.0f);
	CHECK(point.CastShadows);

	SpotLightComponent const& spot = writer.GetComponent<SpotLightComponent>();
	CHECK(spot.Color == glm::vec3(0.25f, 1.0f, 0.5f));
	CHECK(spot.Intensity == 8.0f);
	CHECK(spot.Range == 15.0f);
	CHECK(spot.InnerConeAngle == 10.0f);
	CHECK(spot.OuterConeAngle == 25.0f);
	CHECK(spot.CastShadows);

	SkyLightComponent const& sky = writer.GetComponent<SkyLightComponent>();
	CHECK_FALSE(sky.Environment.IsValid());
	CHECK(sky.Intensity == 0.5f);
	CHECK(sky.Rotation == 90.0f);
	CHECK(sky.SkyboxBlur == 0.25f);
	CHECK_FALSE(sky.DrawSkybox);
	CHECK(sky.AmbientColor == glm::vec3(0.1f, 0.2f, 0.3f));

	RigidBodyComponent const& body = writer.GetComponent<RigidBodyComponent>();
	CHECK(body.Type == RigidBodyType::Kinematic);
	CHECK(body.Mass == 3.0f);
	CHECK(body.Layer == 2);
	CHECK(body.GravityFactor == 0.5f);
	CHECK(body.LinearDamping == 0.2f);
	CHECK(body.AngularDamping == 0.3f);
	// Without a simulated body, velocities are the initial ones.
	CHECK(body.InitialLinearVelocity == glm::vec3(1.0f, 2.0f, 3.0f));
	CHECK(body.InitialAngularVelocity == glm::vec3(4.0f, 5.0f, 6.0f));

	BoxColliderComponent const& box = writer.GetComponent<BoxColliderComponent>();
	CHECK(box.HalfExtents == glm::vec3(1.0f, 2.0f, 3.0f));
	CHECK(box.Offset == glm::vec3(0.5f, 0.0f, 0.0f));
	CHECK(box.IsTrigger);
	CHECK(box.Friction == 0.25f);
	CHECK(box.Restitution == 0.75f);

	SphereColliderComponent const& sphere = writer.GetComponent<SphereColliderComponent>();
	CHECK(sphere.Radius == 2.5f);
	CHECK(sphere.Offset == glm::vec3(0.0f, 1.0f, 0.0f));
	CHECK(sphere.IsTrigger);
	CHECK(sphere.Friction == 0.1f);
	CHECK(sphere.Restitution == 0.9f);

	CapsuleColliderComponent const& capsule = writer.GetComponent<CapsuleColliderComponent>();
	CHECK(capsule.Radius == 0.25f);
	CHECK(capsule.HalfHeight == 1.5f);
	CHECK(capsule.Offset == glm::vec3(0.0f, 0.0f, 1.0f));
	CHECK(capsule.IsTrigger);
	CHECK(capsule.Friction == 0.2f);
	CHECK(capsule.Restitution == 0.3f);

	MeshColliderComponent const& meshCollider = writer.GetComponent<MeshColliderComponent>();
	CHECK(meshCollider.Mesh == mesh);
	CHECK_FALSE(meshCollider.Convex);
	CHECK(meshCollider.IsTrigger);
	CHECK(meshCollider.Friction == 0.4f);
	CHECK(meshCollider.Restitution == 0.6f);

	AudioSourceComponent const& audio = writer.GetComponent<AudioSourceComponent>();
	CHECK_FALSE(audio.Clip.IsValid());
	CHECK(audio.Volume == 0.5f);
	CHECK(audio.Pitch == 1.5f);
	CHECK(audio.Loop);
	CHECK(audio.PlayOnStart);
	CHECK_FALSE(audio.Spatial);
	CHECK(audio.MinDistance == 2.0f);
	CHECK(audio.MaxDistance == 50.0f);

	CHECK_FALSE(writer.GetComponent<AudioListenerComponent>().Active);

	TextComponent const& text = writer.GetComponent<TextComponent>();
	CHECK(text.Text == "H\xC3\xA9llo \xE2\x9C\x93");
	CHECK_FALSE(text.Font.IsValid());
	CHECK(text.Color == glm::vec4(1.0f, 0.0f, 0.0f, 0.5f));
	CHECK(text.FontSize == 32.0f);
	CHECK(text.ScreenSpace);
	CHECK(text.Alignment == TextAlignment::Center);
	CHECK(text.LineSpacing == 1.5f);

	SpriteRendererComponent const& sprite = writer.GetComponent<SpriteRendererComponent>();
	CHECK(sprite.Color == glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
	CHECK_FALSE(sprite.Texture.IsValid());
	CHECK(sprite.Tiling == 4.0f);
	CHECK(sprite.ScreenSpace);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: scripts build hierarchies and add and remove components")
{
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Hierarchy");
	Entity const probe = AddScript(scene, "Probe", "Strada.Tests.HierarchyProbe");
	scene.OnRuntimeStart();
	CHECK(
		probe.GetName() ==
		"Children=Probe Child+Probe Second;Self=True;Added=True;Removed=True;ChildParent=Probe Parent;SecondParent=none;Transform=True;Valid=True");
	CHECK(Testing::WasLogged(logStart, "every entity keeps its Transform component"));

	Entity const parent = scene.FindEntityByName("Probe Parent");
	Entity const child = scene.FindEntityByName("Probe Child");
	Entity const second = scene.FindEntityByName("Probe Second");
	REQUIRE(parent);
	REQUIRE(child);
	REQUIRE(second);
	CHECK(child.GetComponent<RelationshipComponent>().Parent == parent.GetUUID());
	CHECK_FALSE(second.GetComponent<RelationshipComponent>().Parent.IsValid());
	CHECK_FALSE(probe.HasComponent<PointLightComponent>());
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: scripts work in world space through their transforms")
{
	Testing::ScriptEngineScope scripting;
	Scene scene("Transforms");
	Entity holder = scene.CreateEntity("Holder");
	TransformComponent& holderTransform = holder.GetComponent<TransformComponent>();
	holderTransform.Translation = {10.0f, 0.0f, 0.0f};
	holderTransform.Rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
	Entity probe = AddScript(scene, "Probe", "Strada.Tests.TransformProbe");
	REQUIRE(scene.SetParent(probe, holder, false).IsOk());
	probe.GetComponent<TransformComponent>().Translation = {1.0f, 0.0f, 0.0f};

	scene.OnRuntimeStart();
	CHECK(probe.GetName() == "World=(10, 0, -1);Matrix=(10, 0, -1);Forward=(-1, 0, 0);Right=(0, 0, -1);Up=(0, 1, 0)");
	// Moved to the world origin (in the holder's space) and turned.
	TransformComponent const& transform = probe.GetComponent<TransformComponent>();
	CHECK(transform.Translation.x == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(transform.Translation.z == doctest::Approx(-10.0f).epsilon(1e-5));
	CHECK(glm::vec3(scene.GetWorldTransform(probe)[3]).x == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(transform.GetRotationEuler().y == doctest::Approx(45.0f).epsilon(1e-4));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: scripts drive physics bodies, raycasts and gravity")
{
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Physics");
	Entity ground = scene.CreateEntity("Ground");
	ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
	ground.AddComponent<BoxColliderComponent>().HalfExtents = {10.0f, 0.5f, 10.0f};
	Entity probe = AddScript(scene, "Probe", "Strada.Tests.PhysicsProbe");
	probe.GetComponent<TransformComponent>().Translation = {0.0f, 1.0f, 0.0f};

	scene.OnRuntimeStart();
	CHECK(probe.GetName() == "Velocity=10;Hit=True;HitEntity=Ground;Distance=5;Gravity=-9.81;Sleeping=False");
	CHECK(Testing::WasLogged(logStart, "is not kinematic"));
	REQUIRE(scene.GetPhysicsScene() != nullptr);
	CHECK(scene.GetPhysicsScene()->GetGravity() == glm::vec3(0.0f, -20.0f, 0.0f));
	CHECK(probe.GetComponent<TransformComponent>().Translation == glm::vec3(3.0f, 4.0f, 5.0f));
	// The teleport reaches the body before the next step; the upward velocity survives it.
	scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	CHECK(probe.GetComponent<TransformComponent>().Translation.y > 4.0f);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: scripts read input and time and scale time")
{
	Testing::ScriptEngineScope scripting;
	InputScope input;
	Scene scene("Input");
	Entity recorder = scene.CreateEntity("Recorder");
	Entity const probe =
		AddScript(scene, "Probe", "Strada.Tests.InputProbe",
	              {{"Recorder", ScriptFieldValue::FromEntity(recorder.GetUUID())}, {"NextTimeScale", ScriptFieldValue::FromFloat(0.5f)}});
	SceneRuntimeSettings settings;
	settings.FixedTimestep = 0.125f;
	scene.OnRuntimeStart(settings);

	// The key codes scripts know are the engine's.
	std::string const& keys = recorder.GetName();
	size_t keyCount = 0;
	for (size_t start = 0; start < keys.size();)
	{
		size_t const end = std::min(keys.find(',', start), keys.size());
		std::string_view const entry(keys.data() + start, end - start);
		size_t const separator = entry.find('=');
		REQUIRE(separator != std::string_view::npos);
		uint32_t value = 0;
		std::from_chars(entry.data() + separator + 1, entry.data() + entry.size(), value);
		CHECK(std::string_view(KeyCodeToString(static_cast<KeyCode>(value))) == entry.substr(0, separator));
		keyCount++;
		start = end + 1;
	}
	CHECK(keyCount == 121);

	Input::SetKeyState(KeyCode::A, true);
	Input::SetMousePosition({12.0f, 34.0f});
	// Scripts update before the frame's physics steps: the fixed step is the scene's before the first step ran.
	scene.OnUpdateRuntime(Timestep(0.25f));
	CHECK(probe.GetName() == "Down=True;Pressed=True;Released=False;Mouse=(12, 34);Button=False;Gamepad=False;Delta=0.25;Fixed=0.125;"
	                         "Frames=1;Elapsed=0.25");
	CHECK(scene.GetTimeScale() == 0.5f);

	// Half speed from now on; pressed lasts one frame.
	Input::EndFrame();
	scene.OnUpdateRuntime(Timestep(0.25f));
	CHECK(probe.GetName() == "Down=True;Pressed=False;Released=False;Mouse=(12, 34);Button=False;Gamepad=False;Delta=0.125;Fixed=0.125;"
	                         "Frames=2;Elapsed=0.375");
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: scripts play, pause and stop sounds")
{
	Testing::AudioEngineScope engine;
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	InputScope input;
	AssetHandle const clip = AssetManager::AddMemoryAsset(Testing::MakeWavClip({{0.5f, 1.0f}}), "Tone");
	Scene scene("Audio");
	Entity probe = AddScript(scene, "Probe", "Strada.Tests.AudioProbe");
	AudioSourceComponent& source = probe.AddComponent<AudioSourceComponent>();
	source.Clip = clip;
	source.Spatial = false;

	scene.OnRuntimeStart();
	CHECK(probe.GetName() == "Before=False;Playing=True");
	REQUIRE(scene.GetAudioScene() != nullptr);
	CHECK(scene.GetAudioScene()->IsPlaying(probe.GetUUID()));

	Input::SetKeyState(KeyCode::P, true);
	scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	CHECK(probe.GetName() == "Playing=False");
	CHECK_FALSE(scene.GetAudioScene()->IsPlaying(probe.GetUUID()));
	scene.OnRuntimeStop();
}
