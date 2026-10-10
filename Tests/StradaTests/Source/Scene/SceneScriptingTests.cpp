#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float FrameTime = 1.0f / 60.0f;

	Entity AddScript(Scene& scene, std::string const& name, std::string const& className, ScriptFieldMap fields = {})
	{
		Entity entity = scene.CreateEntity(name);
		ScriptComponent& script = entity.AddComponent<ScriptComponent>();
		script.ClassName = className;
		script.Fields = std::move(fields);
		return entity;
	}

	// An entity whose name collects what Lifecycle scripts record.
	Entity AddRecorder(Scene& scene)
	{
		Entity recorder = scene.CreateEntity("Recorder");
		recorder.GetComponent<TagComponent>().Tag.clear();
		return recorder;
	}

	Entity AddLifecycle(Scene& scene, std::string const& label, Entity recorder, bool recordUpdates = true)
	{
		return AddScript(scene, label, "Strada.Tests.Lifecycle",
		                 {{"Label", ScriptFieldValue::FromString(label)},
		                  {"Recorder", ScriptFieldValue::FromEntity(recorder.GetUUID())},
		                  {"RecordUpdates", ScriptFieldValue::FromBool(recordUpdates)}});
	}

	std::vector<std::string> GetRecords(Entity recorder)
	{
		std::vector<std::string> records;
		std::string const& text = recorder.GetName();
		size_t start = 0;
		while (start < text.size())
		{
			size_t const end = std::min(text.find(',', start), text.size());
			records.push_back(text.substr(start, end - start));
			start = end + 1;
		}
		return records;
	}

	ptrdiff_t IndexOf(std::vector<std::string> const& records, std::string const& record)
	{
		auto const found = std::find(records.begin(), records.end(), record);
		return found != records.end() ? found - records.begin() : -1;
	}

	bool Contains(std::vector<std::string> const& records, std::string const& record)
	{
		return IndexOf(records, record) >= 0;
	}
}

TEST_CASE("Scene: scripts run with the fields stored in the scene")
{
	Testing::ScriptEngineScope scripting;
	Scene scene("Scripts");
	Entity const mover = AddScript(scene, "Mover", "Strada.Tests.Mover", {{"Speed", ScriptFieldValue::FromFloat(2.0f)}});
	Entity const defaults = AddScript(scene, "Default mover", "Strada.Tests.Mover");

	scene.OnRuntimeStart();
	CHECK(ScriptEngine::GetSceneContext() == &scene);
	CHECK(ScriptEngine::GetInstanceCount() == 2);
	scene.OnUpdateRuntime(Timestep(0.5f));
	CHECK(mover.GetComponent<TransformComponent>().Translation == glm::vec3(1.0f, 0.0f, 0.0f));
	CHECK(defaults.GetComponent<TransformComponent>().Translation == glm::vec3(0.5f, 0.0f, 0.0f));

	scene.OnRuntimeStop();
	CHECK(ScriptEngine::GetInstanceCount() == 0);
	CHECK(ScriptEngine::GetSceneContext() == nullptr);
	// Stopped scenes do not run scripts.
	scene.OnUpdateRuntime(Timestep(0.5f));
	CHECK(mover.GetComponent<TransformComponent>().Translation == glm::vec3(1.0f, 0.0f, 0.0f));
}

TEST_CASE("Scene: stored field values reach every field type and mismatches keep the defaults")
{
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Fields");
	Entity const target = scene.CreateEntity("Target");
	Entity const fields = AddScript(scene, "Fields", "Strada.Tests.FieldTypes",
	                                {
										{"Count", ScriptFieldValue::FromInt32(12)},
										{"Huge", ScriptFieldValue::FromUInt64(5)},
										{"Title", ScriptFieldValue::FromString("Stored")},
										{"Offset", ScriptFieldValue::FromVector3({4.0f, 5.0f, 6.0f})},
										{"Turn", ScriptFieldValue::FromQuaternion(glm::quat(0.0f, 1.0f, 0.0f, 0.0f))},
										{"Target", ScriptFieldValue::FromEntity(target.GetUUID())},
										{"Model", ScriptFieldValue::FromAsset(AssetHandle(UUID(77)))},
										{"Shape", ScriptFieldValue::FromAsset(AssetHandle(UUID(78)))},
										{"Level", ScriptFieldValue::FromInt32(0)},
										{"Access", ScriptFieldValue::FromUInt32(5)},
										// Out of the range of the byte enum.
										{"Small", ScriptFieldValue::FromInt32(300)},
										{"m_Secret", ScriptFieldValue::FromInt32(7)},
										// A field that changed type, and one that no longer exists.
										{"Speed", ScriptFieldValue::FromString("fast")},
										{"Removed", ScriptFieldValue::FromInt32(1)},
									});

	scene.OnRuntimeStart();
	std::string const expected = "True|12|7|-9000000000|5|2.5|0.125|Stored|(1, 2)|(4, 5, 6)|(1, 2, 3, 4)|(1, 0, 0, 0)|(0.5, 0.25, 1, 1)|" +
	                             target.GetUUID().ToString() + "|77|5|7|1|3|4|0|Mesh(78)|Easy|Read, Execute|B";
	CHECK(fields.GetName() == expected);
	CHECK(Testing::WasLogged(logStart, "Strada.Tests.FieldTypes.Speed: stored \"String\" value ignored: the field is a Float now"));
	CHECK(Testing::WasLogged(logStart, "Strada.Tests.FieldTypes.Small: stored value ignored: 300 is out of the range of Tiny (Byte)"));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: script lifecycle methods run in order and end with the entity or the scene")
{
	Testing::ScriptEngineScope scripting;
	Scene scene("Lifecycle");
	Entity const recorder = AddRecorder(scene);
	Entity const first = AddLifecycle(scene, "A", recorder);
	AddLifecycle(scene, "B", recorder);

	scene.OnRuntimeStart();
	std::vector<std::string> records = GetRecords(recorder);
	REQUIRE(records.size() == 2);
	CHECK(Contains(records, "A:Create"));
	CHECK(Contains(records, "B:Create"));

	// One frame of one fixed step: every update, then every fixed update.
	scene.OnUpdateRuntime(Timestep(FrameTime));
	records = GetRecords(recorder);
	REQUIRE(records.size() == 6);
	CHECK(std::max(IndexOf(records, "A:Update"), IndexOf(records, "B:Update")) <
	      std::min(IndexOf(records, "A:Fixed"), IndexOf(records, "B:Fixed")));

	// Destruction waits for the end of the frame; the script sees it last.
	scene.DestroyEntity(first);
	scene.OnUpdateRuntime(Timestep(FrameTime));
	records = GetRecords(recorder);
	CHECK(records.back() == "A:Destroy");
	CHECK(ScriptEngine::GetInstanceCount() == 1);

	scene.OnRuntimeStop();
	CHECK(GetRecords(recorder).back() == "B:Destroy");
}

TEST_CASE("Scene: scripts follow Script components added, changed and removed while running")
{
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Changes");
	Entity const recorder = AddRecorder(scene);
	Entity changing = AddLifecycle(scene, "Changing", recorder, false);
	scene.OnRuntimeStart();

	// Added: created with OnCreate before its first update.
	AddScript(scene, "Spawner", "Strada.Tests.Spawner", {{"SpawnName", ScriptFieldValue::FromString("Child")}});
	scene.OnUpdateRuntime(Timestep(FrameTime));
	Entity const spawned = scene.FindEntityByName("Child");
	REQUIRE(spawned);
	CHECK(spawned.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 5.0f, 0.0f));

	// Another class: the old instance is destroyed first.
	changing.GetComponent<ScriptComponent>().ClassName = "Strada.Tests.Mover";
	scene.OnUpdateRuntime(Timestep(0.5f));
	CHECK(GetRecords(recorder).back() == "Changing:Destroy");
	CHECK(changing.GetComponent<TransformComponent>().Translation.x == doctest::Approx(0.5f));

	// Unknown classes are reported once and retried when the name changes.
	changing.GetComponent<ScriptComponent>().ClassName = "Strada.Tests.Missing";
	scene.OnUpdateRuntime(Timestep(FrameTime));
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(Testing::WasLogged(logStart, "there is no script class Strada.Tests.Missing"));
	CHECK_FALSE(ScriptEngine::HasInstance(changing.GetUUID()));
	changing.GetComponent<ScriptComponent>().ClassName = "Strada.Tests.Mover";
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(ScriptEngine::HasInstance(changing.GetUUID()));

	// Removed: OnDestroy runs.
	Entity removed = AddLifecycle(scene, "Removed", recorder, false);
	scene.OnUpdateRuntime(Timestep(FrameTime));
	removed.RemoveComponent<ScriptComponent>();
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(GetRecords(recorder).back() == "Removed:Destroy");
	CHECK_FALSE(ScriptEngine::HasInstance(removed.GetUUID()));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: script exceptions are logged and the scene keeps running")
{
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Exceptions");
	AddScript(scene, "Thrower", "Strada.Tests.Thrower");
	Entity const mover = AddScript(scene, "Mover", "Strada.Tests.Mover");
	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(0.25f));
	scene.OnUpdateRuntime(Timestep(0.25f));
	CHECK(mover.GetComponent<TransformComponent>().Translation.x == doctest::Approx(0.5f));
	CHECK(Testing::WasLogged(logStart, "Strada.Tests.Thrower.OnUpdate"));
	CHECK(Testing::WasLogged(logStart, "Thrower always fails"));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: physics contacts reach the scripts of both entities")
{
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting;
	Scene scene("Contacts");
	Entity const recorder = AddRecorder(scene);

	Entity ground = scene.CreateEntity("Ground");
	ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
	ground.AddComponent<BoxColliderComponent>().HalfExtents = {10.0f, 0.5f, 10.0f};
	Entity zone = AddLifecycle(scene, "Zone", recorder, false);
	zone.GetComponent<TransformComponent>().Translation = {0.0f, 2.5f, 0.0f};
	BoxColliderComponent& trigger = zone.AddComponent<BoxColliderComponent>();
	trigger.HalfExtents = {2.0f, 1.0f, 2.0f};
	trigger.IsTrigger = true;
	Entity ball = AddLifecycle(scene, "Ball", recorder, false);
	ball.GetComponent<TransformComponent>().Translation = {0.0f, 6.0f, 0.0f};
	ball.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
	ball.AddComponent<SphereColliderComponent>().Radius = 0.5f;

	scene.OnRuntimeStart();
	for (int frame = 0; frame < 180; frame++)
	{
		scene.OnUpdateRuntime(Timestep(FrameTime));
	}
	std::vector<std::string> const records = GetRecords(recorder);
	// Scripted entities arrive as their script instances.
	CHECK(Contains(records, "Zone:TriggerEnter(Ball scripted)"));
	CHECK(Contains(records, "Ball:TriggerEnter(Zone scripted)"));
	CHECK(Contains(records, "Ball:TriggerExit(Zone)"));
	CHECK(Contains(records, "Ball:CollisionEnter(Ground)"));
	CHECK(IndexOf(records, "Ball:TriggerExit(Zone)") < IndexOf(records, "Ball:CollisionEnter(Ground)"));
	// The ball sleeps on the ground: the contact goes on.
	CHECK_FALSE(Contains(records, "Ball:CollisionExit(Ground)"));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: destroying a running scene ends its scripts, and one scene runs scripts at a time")
{
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	{
		Scene first("First");
		AddLifecycle(first, "Abandoned", first.CreateEntity("Recorder"), false);
		first.OnRuntimeStart();

		Scene second("Second");
		AddScript(second, "Mover", "Strada.Tests.Mover");
		second.OnRuntimeStart();
		CHECK(Testing::WasLogged(logStart, "Scene 'Second' runs without scripts: scene 'First' is running them"));
		CHECK(ScriptEngine::GetInstanceCount() == 1);
		second.OnRuntimeStop();
	}
	CHECK(Testing::WasLogged(logStart, "Abandoned destroyed"));
	CHECK(ScriptEngine::GetInstanceCount() == 0);
	CHECK(ScriptEngine::GetSceneContext() == nullptr);
}

TEST_CASE("Scene: without the script engine or game scripts the runtime runs without scripts")
{
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("No scripts");
	Entity const mover = AddScript(scene, "Mover", "Strada.Tests.Mover");
	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(0.5f));
	CHECK(mover.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
	CHECK(Testing::WasLogged(logStart, "Scene 'No scripts' runs without scripts: the script engine is not initialized"));
	scene.OnRuntimeStop();

	REQUIRE(ScriptEngine::Init().IsOk());
	scene.OnRuntimeStart();
	CHECK(Testing::WasLogged(logStart, "Scene 'No scripts' runs without scripts: no game assembly is loaded"));
	CHECK(ScriptEngine::GetSceneContext() == nullptr);
	scene.OnRuntimeStop();
	ScriptEngine::Shutdown();
}
