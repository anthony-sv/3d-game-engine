#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/PhysicsMaterial.h"
#include "Strada/Asset/PhysicsMaterialAsset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/VisualScriptEngine.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
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

TEST_CASE("Scene: a list stored for a field of one value is logged and skipped, keeping the default")
{
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Lists");
	Entity const fields = AddScript(scene, "Fields", "Strada.Tests.FieldTypes",
	                                {
										{"Speed", ScriptFieldValue::FromList(ScriptFieldType::Float, {ScriptFieldValue::FromFloat(9.0f)})},
										{"Count", ScriptFieldValue::FromList(ScriptFieldType::Int32, {})},
									});

	scene.OnRuntimeStart();
	Result<ScriptFieldMap> const read = scene.ReadScriptFields(fields);
	REQUIRE_MESSAGE(read.IsOk(), read.GetError());
	CHECK(read.GetValue().at("Speed") == ScriptFieldValue::FromFloat(2.5f));
	CHECK(read.GetValue().at("Count") == ScriptFieldValue::FromInt32(-3));
	CHECK(Testing::WasLogged(logStart, "Strada.Tests.FieldTypes.Speed: stored \"Float[]\" value ignored: the field is a Float now"));
	CHECK(Testing::WasLogged(logStart, "Strada.Tests.FieldTypes.Count: stored \"Int32[]\" value ignored: the field is a Int32 now"));
	scene.OnRuntimeStop();
}

namespace
{
	ScriptFieldValue MakeList(ScriptFieldType elementType, std::vector<ScriptFieldValue> elements)
	{
		return ScriptFieldValue::FromList(elementType, std::move(elements));
	}

	// A list of every element type for each array and list field of Strada.Tests.FieldLists, none of them its default.
	ScriptFieldMap MakeStoredLists(UUID target)
	{
		using V = ScriptFieldValue;
		using T = ScriptFieldType;
		return {
			{"Flags", MakeList(T::Bool, {V::FromBool(false), V::FromBool(true), V::FromBool(true)})},
			{"Counts", MakeList(T::Int32, {V::FromInt32(7), V::FromInt32(8)})},
			{"Masks", MakeList(T::UInt32, {V::FromUInt32(1), V::FromUInt32(4000000000u)})},
			{"Bigs", MakeList(T::Int64, {V::FromInt64(-5)})},
			{"Huges", MakeList(T::UInt64, {V::FromUInt64(18000000000000000001ULL)})},
			{"Speeds", MakeList(T::Float, {V::FromFloat(0.25f), V::FromFloat(9.0f)})},
			{"Precises", MakeList(T::Double, {V::FromDouble(1.5)})},
			{"Titles", MakeList(T::String, {V::FromString("a"), V::FromString("")})},
			{"Sizes", MakeList(T::Vector2, {V::FromVector2({3.0f, 4.0f})})},
			{"Offsets", MakeList(T::Vector3, {V::FromVector3({1.0f, 1.0f, 1.0f}), V::FromVector3({2.0f, 2.0f, 2.0f})})},
			{"Weights", MakeList(T::Vector4, {})},
			{"Turns", MakeList(T::Quaternion, {V::FromQuaternion(glm::quat(0.0f, 1.0f, 0.0f, 0.0f))})},
			{"Tints", MakeList(T::Color, {V::FromColor({1.0f, 0.0f, 0.0f, 1.0f})})},
			{"Targets", MakeList(T::Entity, {V::FromEntity(target), V::FromEntity(UUID::Invalid())})},
			{"Crates", MakeList(T::Prefab, {V::FromPrefab(AssetHandle(UUID(41)))})},
			{"Models", MakeList(T::Asset, {V::FromAsset(AssetHandle(UUID(42)))})},
			{"Shapes", MakeList(T::Asset, {V::FromAsset(AssetHandle(UUID(43))), V::FromAsset(AssetHandle())})},
			{"Levels", MakeList(T::Int32, {V::FromInt32(0), V::FromInt32(5), V::FromInt32(1)})},
			{"Accesses", MakeList(T::UInt32, {V::FromUInt32(5)})},
			{"Smalls", MakeList(T::Int32, {V::FromInt32(1), V::FromInt32(200)})},
			{"FlagList", MakeList(T::Bool, {V::FromBool(false)})},
			{"CountList", MakeList(T::Int32, {})},
			{"MaskList", MakeList(T::UInt32, {V::FromUInt32(3)})},
			{"BigList", MakeList(T::Int64, {V::FromInt64(1), V::FromInt64(2)})},
			{"HugeList", MakeList(T::UInt64, {V::FromUInt64(3)})},
			{"SpeedList", MakeList(T::Float, {V::FromFloat(1.5f)})},
			{"PreciseList", MakeList(T::Double, {V::FromDouble(0.5), V::FromDouble(0.25)})},
			{"TitleList", MakeList(T::String, {V::FromString("x")})},
			{"SizeList", MakeList(T::Vector2, {V::FromVector2({1.0f, 1.0f})})},
			{"OffsetList", MakeList(T::Vector3, {})},
			{"WeightList", MakeList(T::Vector4, {V::FromVector4({1.0f, 2.0f, 3.0f, 4.0f})})},
			{"TurnList", MakeList(T::Quaternion, {V::FromQuaternion(glm::quat(1.0f, 0.0f, 0.0f, 0.0f))})},
			{"TintList", MakeList(T::Color, {V::FromColor({0.0f, 1.0f, 0.0f, 1.0f})})},
			{"TargetList", MakeList(T::Entity, {V::FromEntity(target)})},
			{"CrateList", MakeList(T::Prefab, {V::FromPrefab(AssetHandle())})},
			{"ModelList", MakeList(T::Asset, {V::FromAsset(AssetHandle(UUID(44)))})},
			{"ShapeList", MakeList(T::Asset, {V::FromAsset(AssetHandle(UUID(45)))})},
			{"LevelList", MakeList(T::Int32, {V::FromInt32(5)})},
			{"AccessList", MakeList(T::UInt32, {V::FromUInt32(7)})},
			{"SmallList", MakeList(T::Int32, {V::FromInt32(1)})},
			{"Missing", MakeList(T::Float, {V::FromFloat(1.0f)})},
			{"UnsetTurn", V::FromQuaternion(glm::quat(0.0f, 0.0f, 1.0f, 0.0f))},
			{"UnsetTurns", MakeList(T::Quaternion, {})},
		};
	}
}

TEST_CASE("Scene: stored lists reach C# arrays and lists of every element type, and are read back as stored")
{
	Testing::ScriptEngineScope scripting;
	Scene scene("Lists");
	Entity const target = scene.CreateEntity("Target");
	ScriptFieldMap const stored = MakeStoredLists(target.GetUUID());
	Entity const lists = AddScript(scene, "Lists", "Strada.Tests.FieldLists", stored);
	Entity const defaults = AddScript(scene, "Defaults", "Strada.Tests.FieldLists");

	scene.OnRuntimeStart();
	// The instance holds them as arrays and lists of the fields' C# types: enums by their names, entities and meshes as
	// references (0 for none).
	CHECK(lists.GetName() == "7,8||0.25,9|x|Easy,Hard,Normal|Hard|" + target.GetUUID().ToString() + ",0|43,0|1|5000");
	CHECK(defaults.GetName() == "1,-2,3|4,5|2.5|first|Hard,Easy|Normal|0||-1|5000");
	Result<ScriptFieldMap> const read = scene.ReadScriptFields(lists);
	REQUIRE_MESSAGE(read.IsOk(), read.GetError());
	CHECK(read.GetValue().size() == stored.size());
	for (auto const& [name, value] : stored)
	{
		CAPTURE(name);
		CHECK(read.GetValue().at(name) == value);
	}
	// Read from a null array: the empty list.
	Result<ScriptFieldMap> const readDefaults = scene.ReadScriptFields(defaults);
	REQUIRE(readDefaults.IsOk());
	CHECK(readDefaults.GetValue().at("Missing") == MakeList(ScriptFieldType::Float, {}));
	// The instance's zero quaternions, which the engine refuses, are read back as the identity.
	ScriptFieldValue const identity = ScriptFieldValue::FromQuaternion(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
	CHECK(readDefaults.GetValue().at("UnsetTurn") == identity);
	CHECK(readDefaults.GetValue().at("UnsetTurns") == MakeList(ScriptFieldType::Quaternion, {identity, identity}));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: a list of another element type or with an element out of its enum's range is logged and keeps the default")
{
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Lists");
	Entity const lists =
		AddScript(scene, "Lists", "Strada.Tests.FieldLists",
	              {
					  {"Counts", MakeList(ScriptFieldType::Float, {ScriptFieldValue::FromFloat(1.0f)})},
					  {"Smalls", MakeList(ScriptFieldType::Int32, {ScriptFieldValue::FromInt32(1), ScriptFieldValue::FromInt32(300)})},
					  {"Speeds", ScriptFieldValue::FromFloat(1.0f)},
				  });

	scene.OnRuntimeStart();
	Result<ScriptFieldMap> const read = scene.ReadScriptFields(lists);
	REQUIRE_MESSAGE(read.IsOk(), read.GetError());
	CHECK(read.GetValue().at("Counts") == MakeList(ScriptFieldType::Int32, {ScriptFieldValue::FromInt32(1), ScriptFieldValue::FromInt32(-2),
	                                                                        ScriptFieldValue::FromInt32(3)}));
	CHECK(read.GetValue().at("Smalls") == MakeList(ScriptFieldType::Int32, {}));
	CHECK(read.GetValue().at("Speeds") == MakeList(ScriptFieldType::Float, {ScriptFieldValue::FromFloat(2.5f)}));
	CHECK(Testing::WasLogged(logStart, "Strada.Tests.FieldLists.Counts: stored \"Float[]\" value ignored: the field is a Int32[] now"));
	CHECK(Testing::WasLogged(logStart,
	                         "Strada.Tests.FieldLists.Smalls: stored value ignored: element 1: 300 is out of the range of Tiny (Byte)"));
	CHECK(Testing::WasLogged(logStart, "Strada.Tests.FieldLists.Speeds: stored \"Float\" value ignored: the field is a Float[] now"));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: a list an instance holds beyond the most elements a list holds is not read back, and the stored list stays")
{
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Lists");
	// Counts of [-1] makes the script fill CountList with 5000 elements on its updates.
	ScriptFieldValue const storedList = MakeList(ScriptFieldType::Int32, {ScriptFieldValue::FromInt32(4)});
	Entity const lists =
		AddScript(scene, "Lists", "Strada.Tests.FieldLists",
	              {{"Counts", MakeList(ScriptFieldType::Int32, {ScriptFieldValue::FromInt32(-1)})}, {"CountList", storedList}});

	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	for (int read = 0; read < 2; read++)
	{
		Result<ScriptFieldMap> const fields = scene.ReadScriptFields(lists);
		REQUIRE_MESSAGE(fields.IsOk(), fields.GetError());
		CHECK_FALSE(fields.GetValue().contains("CountList"));
		CHECK(fields.GetValue().contains("Counts"));
	}
	CHECK(lists.GetComponent<ScriptComponent>().Fields.at("CountList") == storedList);
	CHECK(Testing::CountLogged(logStart, "Strada.Tests.FieldLists.CountList: holds 5000 elements, more than a list's 4096: its value is "
	                                     "not read back") == 1);
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

TEST_CASE("Scene: running scripts take edited fields and their own values are read back")
{
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Live");
	Entity reporter = AddScript(scene, "Reporter", "Strada.Tests.FieldReporter", {{"Speed", ScriptFieldValue::FromInt32(3)}});
	Entity const idle = scene.CreateEntity("Idle");

	// Before the scene runs, there is no script to read.
	CHECK(scene.ReadScriptFields(reporter).IsError());
	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(reporter.GetName() == "Speed 3 Visible False");

	// The values the script holds, the ones it changed itself included, become the stored ones.
	scene.OnUpdateRuntime(Timestep(FrameTime));
	Result<ScriptFieldMap> const read = scene.ReadScriptFields(reporter);
	REQUIRE(read.IsOk());
	CHECK(read.GetValue() == ScriptFieldMap{{"Speed", ScriptFieldValue::FromInt32(3)},
	                                        {"Visible", ScriptFieldValue::FromBool(false)},
	                                        {"Updates", ScriptFieldValue::FromInt32(2)}});
	CHECK(reporter.GetComponent<ScriptComponent>().Fields == read.GetValue());
	CHECK(scene.ReadScriptFields(idle).IsError());

	// Edited stored values reach the script at once; the ones it changed itself and that were not edited stay its own.
	ScriptComponent& script = reporter.GetComponent<ScriptComponent>();
	script.Fields["Speed"] = ScriptFieldValue::FromInt32(5);
	script.Fields["Visible"] = ScriptFieldValue::FromBool(true);
	scene.ApplyScriptFieldChanges();
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(reporter.GetName() == "Speed 5 Visible True");
	Result<ScriptFieldMap> const updated = scene.ReadScriptFields(reporter);
	REQUIRE(updated.IsOk());
	CHECK(updated.GetValue().at("Updates") == ScriptFieldValue::FromInt32(3));

	// Values of another type, or of fields the script does not have, are reported and skipped.
	script.Fields["Speed"] = ScriptFieldValue::FromString("fast");
	script.Fields["Removed"] = ScriptFieldValue::FromInt32(1);
	scene.ApplyScriptFieldChanges();
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(reporter.GetName() == "Speed 5 Visible True");
	CHECK(Testing::WasLogged(logStart, "Strada.Tests.FieldReporter.Speed: stored \"String\" value ignored: the field is a Int32 now"));

	// Without an instance, the script engine refuses to read or set fields.
	CHECK(ScriptEngine::GetInstanceFields(idle.GetUUID()).IsError());
	CHECK(ScriptEngine::SetInstanceFields(idle.GetUUID(), {{"Speed", ScriptFieldValue::FromInt32(1)}}).IsError());
	scene.OnRuntimeStop();
	CHECK(scene.ReadScriptFields(reporter).IsError());
	CHECK(ScriptEngine::GetInstanceFields(reporter.GetUUID()).IsError());
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

namespace
{
	AssetHandle AddPhysicsMaterial(std::string const& surface)
	{
		TypedDataAssetType<PhysicsMaterial> const* type = DataAssetRegistry::Find<PhysicsMaterial>();
		REQUIRE(type != nullptr);
		Ref<TypedDataAsset<PhysicsMaterial>> const asset = std::static_pointer_cast<TypedDataAsset<PhysicsMaterial>>(type->CreateAsset());
		PhysicsMaterial material;
		material.Surface = surface;
		REQUIRE(asset->SetData(material).IsOk());
		return AssetManager::AddMemoryAsset(asset, surface);
	}

	// The first event of that type between the two entities.
	ContactEvent const& FindEvent(std::vector<ContactEvent> const& events, ContactEventType type, UUID a, UUID b)
	{
		auto const found = std::find_if(events.begin(), events.end(),
		                                [&](ContactEvent const& event)
		                                {
											return event.Type == type &&
			                                       ((event.First == a && event.Second == b) || (event.First == b && event.Second == a));
										});
		REQUIRE(found != events.end());
		return *found;
	}

	// What Strada.Tests.ContactReader kept of the first collision that began is the event as its entity sees it.
	void CheckCollisionRead(ScriptFieldMap const& read, ContactEvent const& event, UUID entity, AssetHandle material, AssetHandle other)
	{
		HandledContactEvent const handled{&event, event.Second == entity};
		REQUIRE(event.PointCount > 0);
		CHECK(read.at("CollisionsEntered").GetInt32() >= 1);
		CHECK(read.at("EnterEntity").GetEntity() == entity);
		CHECK(read.at("EnterOther").GetEntity() == handled.GetOther());
		CHECK(read.at("EnterStep").GetUInt64() == event.Step);
		CHECK(read.at("EnterPointCount").GetInt32() == static_cast<int32_t>(event.PointCount));
		ContactPoint const first = handled.GetPoint(0);
		ContactPoint const last = handled.GetPoint(event.PointCount - 1);
		// The values cross unchanged.
		CHECK(read.at("FirstPoint").GetVector3() == first.Point);
		CHECK(read.at("FirstNormal").GetVector3() == first.Normal);
		CHECK(read.at("FirstImpulse").GetFloat() == first.Impulse);
		CHECK(read.at("FirstRelativeVelocity").GetVector3() == first.RelativeVelocity);
		CHECK(read.at("FirstMaterial").GetAsset() == material);
		CHECK(read.at("FirstOtherMaterial").GetAsset() == other);
		CHECK(read.at("LastPoint").GetVector3() == last.Point);
		CHECK(read.at("LastImpulse").GetFloat() == last.Impulse);
		CHECK(read.at("EmptyOutsideHandlers").GetBool());
	}
}

TEST_CASE("Scene: contact handlers read the event's entities, step and points through Strada.Collision, seen from their entity")
{
	Testing::PhysicsSystemScope physics;
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	AssetHandle const asphalt = AddPhysicsMaterial("Asphalt");
	AssetHandle const rubber = AddPhysicsMaterial("Rubber");
	struct Run
	{
		uint64_t EnterStep = 0;
		float FirstImpulse = 0.0f;
	};
	std::vector<Run> runs;
	// The steps are of 1/60 s at every frame rate: the contact is the same, and is read the same.
	for (float const frameTime : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f})
	{
		CAPTURE(frameTime);
		Scene scene("Contacts");
		Entity ground = AddScript(scene, "Ground", "Strada.Tests.ContactReader");
		ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
		BoxColliderComponent& floor = ground.AddComponent<BoxColliderComponent>();
		floor.HalfExtents = {10.0f, 0.5f, 10.0f};
		floor.Material = asphalt;
		Entity zone = scene.CreateEntity("Zone");
		zone.GetComponent<TransformComponent>().Translation = {0.0f, 2.5f, 0.0f};
		BoxColliderComponent& trigger = zone.AddComponent<BoxColliderComponent>();
		trigger.HalfExtents = {2.0f, 1.0f, 2.0f};
		trigger.IsTrigger = true;
		// A box that lands flat: four points.
		Entity box = AddScript(scene, "Box", "Strada.Tests.ContactReader");
		box.GetComponent<TransformComponent>().Translation = {0.0f, 6.0f, 0.0f};
		box.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		BoxColliderComponent& crate = box.AddComponent<BoxColliderComponent>();
		crate.HalfExtents = {0.5f, 0.5f, 0.5f};
		crate.Material = rubber;

		scene.OnRuntimeStart();
		std::vector<ContactEvent> events;
		int const frames = static_cast<int>(std::lround(3.0f / frameTime));
		for (int frame = 0; frame < frames; frame++)
		{
			scene.OnUpdateRuntime(Timestep(frameTime));
			events.insert(events.end(), scene.GetContactEvents().begin(), scene.GetContactEvents().end());
		}
		ContactEvent const& landed = FindEvent(events, ContactEventType::CollisionEnter, box.GetUUID(), ground.GetUUID());
		CHECK(landed.PointCount == MaxContactPoints);
		Result<ScriptFieldMap> const boxRead = ScriptEngine::GetInstanceFields(box.GetUUID());
		Result<ScriptFieldMap> const groundRead = ScriptEngine::GetInstanceFields(ground.GetUUID());
		REQUIRE(boxRead.IsOk());
		REQUIRE(groundRead.IsOk());
		ScriptFieldMap const& read = boxRead.GetValue();
		CheckCollisionRead(read, landed, box.GetUUID(), rubber, asphalt);
		CheckCollisionRead(groundRead.GetValue(), landed, ground.GetUUID(), asphalt, rubber);
		// The ground pushes the box up and the box the ground down; the strongest point comes first.
		CHECK(read.at("FirstNormal").GetVector3().y > 0.99f);
		CHECK(groundRead.GetValue().at("FirstNormal").GetVector3().y < -0.99f);
		CHECK(read.at("FirstImpulse").GetFloat() > 0.0f);
		CHECK(read.at("FirstImpulse").GetFloat() >= read.at("LastImpulse").GetFloat());

		// A trigger's points carry no impulse, and a contact that ends has none.
		ContactEvent const& entered = FindEvent(events, ContactEventType::TriggerEnter, box.GetUUID(), zone.GetUUID());
		ContactEvent const& left = FindEvent(events, ContactEventType::TriggerExit, box.GetUUID(), zone.GetUUID());
		CHECK(read.at("TriggersEntered").GetInt32() == 1);
		CHECK(read.at("TriggerEnterStep").GetUInt64() == entered.Step);
		CHECK(read.at("TriggerEnterPointCount").GetInt32() == static_cast<int32_t>(entered.PointCount));
		CHECK(read.at("TriggerEnterPointCount").GetInt32() > 0);
		CHECK(read.at("TriggerEnterImpulse").GetFloat() == 0.0f);
		CHECK(read.at("TriggersExited").GetInt32() == 1);
		CHECK(read.at("TriggerExitStep").GetUInt64() == left.Step);
		CHECK(read.at("TriggerExitPointCount").GetInt32() == 0);
		CHECK(read.at("TriggerExitOther").GetEntity() == zone.GetUUID());
		CHECK(entered.Step < left.Step);
		CHECK(left.Step < landed.Step);
		runs.push_back({landed.Step, read.at("FirstImpulse").GetFloat()});
		scene.OnRuntimeStop();
	}
	REQUIRE(runs.size() == 3);
	for (Run const& run : runs)
	{
		CHECK(run.EnterStep == runs.front().EnterStep);
		CHECK(run.FirstImpulse == runs.front().FirstImpulse);
	}
	// Outside the scene's dispatch no contact is being handled.
	CHECK(ScriptEngine::GetHandledContactEvent() == nullptr);
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

TEST_CASE("Scene: without the script engine visual scripts do not run, and without game scripts they run alone")
{
	Testing::AssetManagerScope assets;
	uint64_t const logStart = Log::GetNextEntryIndex();
	// Counts its updates in a public variable.
	VisualScriptDocument document;
	VisualScriptVariable updates;
	updates.ID = 1;
	updates.Name = "Updates";
	updates.Type = "int";
	updates.Public = true;
	document.Variables.push_back(updates);
	VisualScriptNode update;
	update.ID = 10;
	update.Type = "Event.Update";
	VisualScriptNode set;
	set.ID = 11;
	set.Type = "Variable.Set";
	set.Settings = Json::object({{"Variable", 1}});
	VisualScriptNode add;
	add.ID = 12;
	add.Type = "Operator.Add";
	add.Inputs = Json::object({{"B", 1}});
	VisualScriptNode get;
	get.ID = 13;
	get.Type = "Variable.Get";
	get.Settings = Json::object({{"Variable", 1}});
	document.Graph.Nodes = {update, set, add, get};
	document.Graph.Links = {{{10, "Then"}, {11, "Execute"}}, {{13, "Value"}, {12, "A"}}, {{12, "Result"}, {11, "Value"}}};
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(document), "Counter");

	Scene scene("Graphs only");
	Entity counter = scene.CreateEntity("Counter");
	counter.AddComponent<VisualScriptComponent>().Graph = graph;
	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(0.5f));
	CHECK(Testing::WasLogged(logStart, "Scene 'Graphs only' runs without scripts: the script engine is not initialized"));
	CHECK(VisualScriptEngine::GetInstanceCount() == 0);
	scene.OnRuntimeStop();

	Testing::ScriptRuntimeScope runtime;
	scene.OnRuntimeStart();
	CHECK(ScriptEngine::GetSceneContext() == &scene);
	CHECK(VisualScriptEngine::HasInstance(counter.GetUUID()));
	scene.OnUpdateRuntime(Timestep(0.5f));
	Result<ScriptFieldMap> const read = scene.ReadVisualScriptVariables(counter);
	REQUIRE(read.IsOk());
	CHECK(read.GetValue() == ScriptFieldMap{{"Updates", ScriptFieldValue::FromInt32(1)}});
	CHECK_FALSE(Testing::WasLogged(logStart, "no game assembly is loaded"));
	scene.OnRuntimeStop();
	CHECK(ScriptEngine::GetSceneContext() == nullptr);
	CHECK(VisualScriptEngine::GetInstanceCount() == 0);
}
