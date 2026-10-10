#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/PrefabAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Math/Math.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Script/ScriptEngine.h"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <string>
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

	// Records what scripts ask of the application for as long as it lives.
	struct RecordingHost final : ScriptHost
	{
		explicit RecordingHost(bool editor)
			: Editor(editor)
		{
			ScriptEngine::SetHost(this);
		}

		~RecordingHost() override { ScriptEngine::SetHost(nullptr); }

		RecordingHost(RecordingHost const&) = delete;
		RecordingHost& operator=(RecordingHost const&) = delete;

		bool IsEditor() const override { return Editor; }
		void RequestSceneLoad(AssetHandle scene) override { SceneLoads.push_back(scene); }
		void RequestQuit() override { QuitRequests++; }
		void ReportTestResult(ScriptTestResult const& result) override { Results.push_back(result); }
		void FinishTests() override { FinishCalls++; }
		void OnScriptException() override { Exceptions++; }

		bool Editor = false;
		std::vector<AssetHandle> SceneLoads;
		uint32_t QuitRequests = 0;
		std::vector<ScriptTestResult> Results;
		uint32_t FinishCalls = 0;
		uint32_t Exceptions = 0;
	};

	// Input is global: tests that set it restore it.
	class InputScope
	{
	public:
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }

		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	// One frame with the key pressed.
	void UpdateWithKey(Scene& scene, KeyCode key, float deltaTime)
	{
		Input::SetKeyState(key, true);
		scene.OnUpdateRuntime(Timestep(deltaTime));
		Input::EndFrame();
		Input::SetKeyState(key, false);
		Input::EndFrame();
	}

	// An asset directory with the scene Scenes/Next.sscene.
	struct SceneAssets
	{
		SceneAssets()
		{
			REQUIRE(FileSystem::CreateDirectories(Directory.GetPath() / "Scenes").IsOk());
			Scene const next("Next");
			REQUIRE(SceneSerializer::SaveToFile(next, Directory.GetPath() / "Scenes" / "Next.sscene").IsOk());
			REQUIRE(AssetManager::OpenAssetDirectory(Directory.GetPath()).IsOk());
			NextScene = AssetManager::FindByPath("Scenes/Next.sscene");
			REQUIRE(NextScene.IsValid());
		}

		Testing::TemporaryDirectory Directory;
		AssetHandle NextScene;
	};
}

TEST_CASE("Scene: scripts load assets and change the materials they make")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	AssetHandle const white = GetBuiltInHandle(BuiltInAsset::WhiteTexture);
	AssetHandle const defaultMaterial = GetBuiltInHandle(BuiltInAsset::DefaultMaterial);
	MaterialData const defaults = AssetManager::GetAsset<MaterialAsset>(defaultMaterial)->GetData();
	Scene scene("Assets");
	Entity probe = AddScript(scene, "Probe", "Strada.Tests.AssetProbe");
	probe.AddComponent<MeshComponent>().Mesh = GetBuiltInHandle(BuiltInAsset::CubeMesh);

	scene.OnRuntimeStart();
	CHECK(probe.GetName() ==
	      "Cube=True;Missing=True;WrongType=True;SharedEditable=False;CreatedEditable=True;CloneEditable=True;SharedUnchanged=True;"
	      "Metallic=0.75;Blend=True;Texture=True;Emissive=(0, 1, 0, 1)");
	CHECK(Testing::WasLogged(logStart, "Assets.Load: there is no asset at 'Meshes/Missing.glb'"));
	CHECK(Testing::WasLogged(logStart, "Assets.Load: 'builtin://Cube' is a Mesh, not a Material"));
	CHECK(Testing::WasLogged(logStart, "Material.Metallic: the value is not a finite number"));
	CHECK(Testing::WasLogged(logStart, "Material.AlphaMode: 7 is not a MaterialAlphaMode"));
	CHECK(Testing::WasLogged(logStart, "Material.Roughness: material 100 belongs to the project"));

	MeshComponent const& mesh = probe.GetComponent<MeshComponent>();
	REQUIRE(mesh.Materials.size() == 2);
	AssetHandle const created = mesh.Materials[0];
	AssetHandle const clone = mesh.Materials[1];
	CHECK(scene.IsRuntimeAsset(created));
	CHECK(scene.IsRuntimeAsset(clone));
	Ref<MaterialAsset> const createdAsset = AssetManager::GetAsset<MaterialAsset>(created);
	REQUIRE(createdAsset != nullptr);
	MaterialData const& data = createdAsset->GetData();
	CHECK(data.BaseColor == glm::vec4(1.0f, 0.0f, 0.0f, 0.5f));
	CHECK(data.Metallic == 0.75f);
	CHECK(data.Roughness == 0.25f);
	CHECK(data.EmissiveColor == glm::vec3(0.0f, 1.0f, 0.0f));
	CHECK(data.EmissiveIntensity == 3.0f);
	CHECK(data.NormalStrength == 0.5f);
	CHECK(data.OcclusionStrength == 0.25f);
	CHECK(data.BaseColorTexture == white);
	CHECK(data.NormalTexture == white);
	CHECK(data.MetallicRoughnessTexture == white);
	CHECK(data.OcclusionTexture == white);
	CHECK(data.EmissiveTexture == white);
	CHECK(data.AlphaMode == MaterialAlphaMode::Blend);
	CHECK(data.AlphaCutoff == 0.3f);
	CHECK(data.DoubleSided);
	CHECK(data.UVTiling == glm::vec2(2.0f, 3.0f));
	CHECK(data.UVOffset == glm::vec2(0.5f, 0.25f));

	Ref<MaterialAsset> const cloneAsset = AssetManager::GetAsset<MaterialAsset>(clone);
	REQUIRE(cloneAsset != nullptr);
	CHECK(cloneAsset->GetData() == defaults);
	CHECK(AssetManager::GetMetadata(clone)->Name == "DefaultMaterial (Clone)");
	CHECK(AssetManager::GetAsset<MaterialAsset>(defaultMaterial)->GetData() == defaults);

	// The run's materials go with it.
	scene.OnRuntimeStop();
	CHECK_FALSE(AssetManager::IsValid(created));
	CHECK_FALSE(AssetManager::IsValid(clone));
	CHECK_FALSE(scene.IsRuntimeAsset(created));
}

TEST_CASE("Scene: scripts instantiate prefabs whose scripts start at once")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	Scene source("Source");
	Entity member = AddScript(source, "Member", "Strada.Tests.PrefabMember", {{"Value", ScriptFieldValue::FromInt32(7)}});
	member.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f);
	source.CreateEntity("Member Child", member);
	Result<Ref<PrefabAsset>> prefabAsset = PrefabAsset::Create(PrefabSerializer::Serialize(source, member));
	REQUIRE(prefabAsset.IsOk());
	AssetHandle const prefab = AssetManager::AddMemoryAsset(prefabAsset.GetValue(), "Member");

	Scene scene("Prefabs");
	Entity probe = AddScript(scene, "Probe", "Strada.Tests.PrefabProbe", {{"Template", ScriptFieldValue::FromPrefab(prefab)}});
	probe.GetComponent<TransformComponent>().Translation = {10.0f, 0.0f, 0.0f};
	scene.OnRuntimeStart();
	CHECK(probe.GetName() == "Started=True;Value=7;Parent=True;Children=1;AtRoot=True;Distinct=True");

	std::vector<Entity> const children = scene.GetChildren(probe);
	REQUIRE(children.size() == 1);
	Entity const instance = children.front();
	CHECK(instance.GetName() == "Member");
	CHECK(instance.GetComponent<PrefabComponent>().Prefab == prefab);
	CHECK(scene.GetChildren(instance).size() == 1);
	CHECK(ScriptEngine::HasInstance(instance.GetUUID()));
	// The probe and both instances' roots run scripts.
	CHECK(ScriptEngine::GetInstanceCount() == 3);

	// Placed in world space, keeping the prefab's scale.
	glm::vec3 position;
	glm::quat rotation;
	glm::vec3 scale;
	REQUIRE(Math::DecomposeTransform(scene.GetWorldTransform(instance), position, rotation, scale));
	CHECK(glm::length(position - glm::vec3(1.0f, 2.0f, 3.0f)) < 1e-5f);
	CHECK(glm::abs(glm::dot(rotation, glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f)))) > 1.0f - 1e-5f);
	CHECK(glm::length(scale - glm::vec3(2.0f)) < 1e-5f);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: scripts talk to the application running them")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	InputScope input;
	SceneAssets const sceneAssets;
	RecordingHost host(true);
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Host");
	scene.OnViewportResize(640, 480);
	Entity const probe = AddScript(scene, "Probe", "Strada.Tests.HostProbe");

	scene.OnRuntimeStart();
	CHECK(probe.GetName() == "Editor=True;Size=640x480;Scene=Host;Load=True;Missing=False;Wrong=False");
	CHECK(host.SceneLoads == std::vector<AssetHandle>{sceneAssets.NextScene});
	CHECK(Testing::WasLogged(logStart, "SceneManager.LoadScene: there is no asset at 'Scenes/Missing.sscene'"));
	CHECK(Testing::WasLogged(logStart, "SceneManager.LoadScene: 'builtin://Cube' is a Mesh, not a Scene"));

	REQUIRE(host.Results.size() == 5);
	CHECK(host.Results[0].Name == "passes");
	CHECK(host.Results[0].Passed);
	CHECK(host.Results[1].Name == "fails");
	CHECK_FALSE(host.Results[1].Passed);
	CHECK(host.Results[1].Message == "on purpose");
	CHECK(host.Results[2].Name == "asserts");
	CHECK(host.Results[2].Message == "expected 1, got 2");
	CHECK(host.Results[3].Name == "throws");
	CHECK(host.Results[3].Message.find("System.InvalidOperationException: boom") != std::string::npos);
	CHECK(host.Results[4].Name == "checks");
	CHECK(host.Results[4].Passed);
	CHECK(Testing::WasLogged(logStart, "Test failed: fails: on purpose"));

	// Debug lines: the invalid ones are rejected; one lasts a frame, the other a second.
	CHECK(Testing::WasLogged(logStart, "Debug.DrawLine: the value is not a finite number"));
	CHECK(Testing::WasLogged(logStart, "Debug.DrawLine: the duration -1 is negative"));
	REQUIRE(scene.GetDebugLines().size() == 2);
	CHECK(scene.GetDebugLines()[0].Color == glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
	CHECK(scene.GetDebugLines()[1].To == glm::vec3(1.0f, 0.0f, 0.0f));
	CHECK(scene.GetDebugLines()[1].Color == glm::vec4(0.0f, 0.5f, 0.0f, 1.0f));

	UpdateWithKey(scene, KeyCode::Q, 0.5f);
	CHECK(host.QuitRequests == 1);
	CHECK(scene.GetDebugLines().size() == 1);
	UpdateWithKey(scene, KeyCode::F, 0.5f);
	CHECK(host.FinishCalls == 1);
	CHECK(scene.GetDebugLines().empty());
	CHECK(host.Exceptions == 0);
	UpdateWithKey(scene, KeyCode::T, 0.5f);
	CHECK(host.Exceptions == 1);
	CHECK(Testing::WasLogged(logStart, "thrown on purpose"));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: without an application, scripts' requests are logged")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	InputScope input;
	SceneAssets const sceneAssets;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Alone");
	Entity const probe = AddScript(scene, "Probe", "Strada.Tests.HostProbe");
	scene.OnRuntimeStart();
	CHECK(probe.GetName() == "Editor=False;Size=0x0;Scene=Alone;Load=False;Missing=False;Wrong=False");
	CHECK(Testing::WasLogged(logStart, "SceneManager.LoadScene: no application runs the scene"));
	CHECK(Testing::WasLogged(logStart, "Test passed: passes"));
	UpdateWithKey(scene, KeyCode::Q, 0.1f);
	CHECK(Testing::WasLogged(logStart, "Application.Quit: no application runs the scene"));
	// Debug lines go when the runtime stops.
	CHECK_FALSE(scene.GetDebugLines().empty());
	scene.OnRuntimeStop();
	CHECK(scene.GetDebugLines().empty());
}

TEST_CASE("Scene: script values that are not finite or not enumerators are rejected")
{
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Guard");
	Entity probe = AddScript(scene, "Probe", "Strada.Tests.ValueGuard");
	scene.OnRuntimeStart();
	CHECK(probe.GetName() == "Translation=(1, 2, 3);Rotation=(0, 0, 0, 1);Hit=False");
	// The rotation set last, with components near the float range, kept its direction.
	glm::quat const quarterTurn = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
	CHECK(std::abs(glm::dot(probe.GetComponent<TransformComponent>().Rotation, quarterTurn)) == doctest::Approx(1.0f));

	CameraComponent const& camera = probe.GetComponent<CameraComponent>();
	CHECK(camera.OrthographicSize == 5.0f);
	CHECK(camera.Projection == ProjectionType::Perspective);
	RigidBodyComponent const& body = probe.GetComponent<RigidBodyComponent>();
	CHECK(body.Mass == 2.0f);
	CHECK(body.InitialLinearVelocity == glm::vec3(0.0f));
	for (char const* const message :
	     {"TransformComponent.Translation: the value is not valid", "TransformComponent.EulerAngles: the value is not a finite number",
	      "CameraComponent.OrthographicSize: the value is not valid", "CameraComponent.Projection: the value is not valid",
	      "RigidBodyComponent.Mass: the value is not valid", "RigidBodyComponent.LinearVelocity: the value is not a finite number",
	      "RigidBodyComponent.AddForce: the value is not a finite number", "RigidBodyComponent.Teleport: the value is not a finite number",
	      "Physics.Gravity: the value is not a finite number", "Physics.Raycast: the value is not a finite number"})
	{
		CAPTURE(message);
		CHECK(Testing::WasLogged(logStart, message));
	}
	scene.OnRuntimeStop();
}
