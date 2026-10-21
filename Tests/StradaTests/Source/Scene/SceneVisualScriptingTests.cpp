#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/PrefabAsset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/VisualScriptEngine.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float FrameTime = 1.0f / 60.0f;

	// Records what scripts ask of the application for as long as it lives; each check scripts report is kept with the
	// number of instances of both kinds at that moment.
	struct RecordingHost final : ScriptHost
	{
		struct Report
		{
			std::string Name;
			size_t Instances = 0;
			size_t GraphInstances = 0;
		};

		RecordingHost() { ScriptEngine::SetHost(this); }
		~RecordingHost() override { ScriptEngine::SetHost(nullptr); }

		RecordingHost(RecordingHost const&) = delete;
		RecordingHost& operator=(RecordingHost const&) = delete;

		bool IsEditor() const override { return false; }
		void RequestSceneLoad(AssetHandle) override {}
		void RequestQuit() override {}
		void ReportTestResult(ScriptTestResult const& result) override
		{
			Reports.push_back({result.Name, ScriptEngine::GetInstanceCount(), VisualScriptEngine::GetInstanceCount()});
		}
		void FinishTests() override {}
		void OnScriptException() override { Exceptions++; }

		std::vector<Report> Reports;
		uint32_t Exceptions = 0;
	};

	size_t CountLogged(uint64_t firstIndex, std::string_view text)
	{
		std::vector<LogEntry> const entries = Log::GetEntries(firstIndex);
		return static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
		                                         [text](LogEntry const& entry)
		                                         {
													 return entry.Message.find(text) != std::string::npos;
												 }));
	}

	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type, Json value = Json(), bool isPublic = true)
	{
		VisualScriptVariable variable;
		variable.ID = id;
		variable.Name = std::move(name);
		variable.Type = std::move(type);
		variable.Value = std::move(value);
		variable.Public = isPublic;
		return variable;
	}

	VisualScriptNode MakeNode(uint32_t id, std::string type, Json settings = Json::object(), Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Settings = std::move(settings);
		node.Inputs = std::move(inputs);
		return node;
	}

	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}

	// Adds Speed (public, 1) to Updates (public, 0) on every update.
	VisualScriptDocument MakeCounterDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Speed", "int", 1));
		document.Variables.push_back(MakeVariable(2, "Updates", "int", 0));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 2}})));
		document.Graph.Nodes.push_back(MakeNode(12, "Operator.Add"));
		document.Graph.Nodes.push_back(MakeNode(13, "Variable.Get", Json::object({{"Variable", 2}})));
		document.Graph.Nodes.push_back(MakeNode(14, "Variable.Get", Json::object({{"Variable", 1}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(13, "Value", 12, "A"));
		document.Graph.Links.push_back(MakeLink(14, "Value", 12, "B"));
		document.Graph.Links.push_back(MakeLink(12, "Result", 11, "Value"));
		return document;
	}

	// The counter with a node the runtime does not know: it does not compile.
	VisualScriptDocument MakeBrokenDocument()
	{
		VisualScriptDocument document = MakeCounterDocument();
		document.Graph.Nodes.push_back(MakeNode(20, "Sample.NoSuchNode"));
		return document;
	}

	// The visual-script counterpart of Strada.Tests.Lifecycle: appends ",<Label>:<call>" to the name of its Recorder for each
	// event it gets (Update and FixedUpdate only while RecordUpdates is set; contacts and clicks name the other entity).
	VisualScriptDocument MakeRecorderDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Label", "string", ""));
		document.Variables.push_back(MakeVariable(2, "Recorder", "Strada.Entity"));
		document.Variables.push_back(MakeVariable(3, "RecordUpdates", "bool", true));
		std::vector<VisualScriptNode>& nodes = document.Graph.Nodes;
		std::vector<VisualScriptLink>& links = document.Graph.Links;
		// Shared pure nodes: the recorder, the label, the recorder's name, whether the recorder exists, RecordUpdates.
		nodes.push_back(MakeNode(4, "Variable.Get", Json::object({{"Variable", 2}})));
		nodes.push_back(MakeNode(5, "Variable.Get", Json::object({{"Variable", 1}})));
		nodes.push_back(MakeNode(6, "Get:Strada.Entity.Name"));
		nodes.push_back(MakeNode(7, "IsValid"));
		nodes.push_back(MakeNode(8, "Variable.Get", Json::object({{"Variable", 3}})));
		links.push_back(MakeLink(4, "Value", 6, "Target"));
		links.push_back(MakeLink(4, "Value", 7, "Value"));

		uint32_t next = 10;
		auto const record = [&](std::string const& eventType, std::string const& call, std::string const& entityPin, bool everyFrame)
		{
			uint32_t const event = next++;
			nodes.push_back(MakeNode(event, eventType));
			VisualScriptPinReference from{event, "Then"};
			if (everyFrame)
			{
				uint32_t const enabled = next++;
				nodes.push_back(MakeNode(enabled, "Flow.Branch"));
				links.push_back(MakeLink(event, "Then", enabled, "Execute"));
				links.push_back(MakeLink(8, "Value", enabled, "Condition"));
				from = {enabled, "True"};
			}
			uint32_t const exists = next++;
			nodes.push_back(MakeNode(exists, "Flow.Branch"));
			links.push_back(MakeLink(from.Node, from.Pin, exists, "Execute"));
			links.push_back(MakeLink(7, "Result", exists, "Condition"));
			uint32_t const text = next++;
			std::string const format = "{Old},{Label}:" + call + (entityPin.empty() ? "" : "({Name})");
			nodes.push_back(MakeNode(text, "FormatString", Json::object({{"Format", format}})));
			links.push_back(MakeLink(6, "Value", text, "Old"));
			links.push_back(MakeLink(5, "Value", text, "Label"));
			if (!entityPin.empty())
			{
				uint32_t const name = next++;
				nodes.push_back(MakeNode(name, "Get:Strada.Entity.Name"));
				links.push_back(MakeLink(event, entityPin, name, "Target"));
				links.push_back(MakeLink(name, "Value", text, "Name"));
			}
			uint32_t const set = next++;
			nodes.push_back(MakeNode(set, "Set:Strada.Entity.Name"));
			links.push_back(MakeLink(exists, "True", set, "Execute"));
			links.push_back(MakeLink(4, "Value", set, "Target"));
			links.push_back(MakeLink(text, "Result", set, "Value"));
		};
		record("Event.Create", "Create", "", false);
		record("Event.Update", "Update", "", true);
		record("Event.FixedUpdate", "Fixed", "", true);
		record("Event.Destroy", "Destroy", "", false);
		record("Event.CollisionEnter", "CollisionEnter", "Other", false);
		record("Event.CollisionExit", "CollisionExit", "Other", false);
		record("Event.TriggerEnter", "TriggerEnter", "Other", false);
		record("Event.TriggerExit", "TriggerExit", "Other", false);
		record("Event.Click", "Click", "Button", false);
		return document;
	}

	// Sets Done (public) when a Delay of half a second started by Event.Create completes.
	VisualScriptDocument MakeDelayDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Done", "bool", false));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
		document.Graph.Nodes.push_back(MakeNode(11, "Flow.Delay", Json::object(), Json::object({{"Duration", 0.5}})));
		document.Graph.Nodes.push_back(MakeNode(12, "Variable.Set", Json::object({{"Variable", 1}}), Json::object({{"Value", true}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(11, "Completed", 12, "Execute"));
		return document;
	}

	// Divides 1 by a variable that is 0 on every update: an aborting error each time.
	VisualScriptDocument MakeDivisionDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Result", "int", 0));
		document.Variables.push_back(MakeVariable(2, "Zero", "int", 0, false));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 1}})));
		document.Graph.Nodes.push_back(MakeNode(12, "Operator.Divide", Json::object(), Json::object({{"A", 1}})));
		document.Graph.Nodes.push_back(MakeNode(13, "Variable.Get", Json::object({{"Variable", 2}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(13, "Value", 12, "B"));
		document.Graph.Links.push_back(MakeLink(12, "Result", 11, "Value"));
		return document;
	}

	// Loops forever when created.
	VisualScriptDocument MakeEndlessDocument()
	{
		VisualScriptDocument document;
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
		document.Graph.Nodes.push_back(MakeNode(11, "Flow.While", Json::object(), Json::object({{"Condition", true}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		return document;
	}

	AssetHandle AddGraph(VisualScriptDocument document, std::string name)
	{
		return AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(std::move(document)), std::move(name));
	}

	Entity AddVisualScript(Entity entity, AssetHandle graph, ScriptFieldMap variables = {})
	{
		VisualScriptComponent& component = entity.AddComponent<VisualScriptComponent>();
		component.Graph = graph;
		component.Variables = std::move(variables);
		return entity;
	}

	Entity AddScript(Entity entity, std::string const& className, ScriptFieldMap fields = {})
	{
		ScriptComponent& script = entity.AddComponent<ScriptComponent>();
		script.ClassName = className;
		script.Fields = std::move(fields);
		return entity;
	}

	// An entity whose name collects what Lifecycle scripts and recorder graphs record.
	Entity AddRecorder(Scene& scene)
	{
		Entity recorder = scene.CreateEntity("Recorder");
		recorder.GetComponent<TagComponent>().Tag.clear();
		return recorder;
	}

	ScriptFieldMap MakeLifecycleFields(std::string const& label, Entity recorder, bool recordUpdates, bool report = false)
	{
		return {{"Label", ScriptFieldValue::FromString(label)},
		        {"Recorder", ScriptFieldValue::FromEntity(recorder.GetUUID())},
		        {"RecordUpdates", ScriptFieldValue::FromBool(recordUpdates)},
		        {"Report", ScriptFieldValue::FromBool(report)}};
	}

	ScriptFieldMap MakeRecorderVariables(std::string const& label, Entity recorder, bool recordUpdates)
	{
		return {{"Label", ScriptFieldValue::FromString(label)},
		        {"Recorder", ScriptFieldValue::FromEntity(recorder.GetUUID())},
		        {"RecordUpdates", ScriptFieldValue::FromBool(recordUpdates)}};
	}

	// The records in the recorder's name, in order (recorder graphs start theirs with a comma).
	std::vector<std::string> GetRecords(Entity recorder)
	{
		std::vector<std::string> records;
		std::string const& text = recorder.GetName();
		size_t start = 0;
		while (start < text.size())
		{
			size_t const end = std::min(text.find(',', start), text.size());
			if (end > start)
			{
				records.push_back(text.substr(start, end - start));
			}
			start = end + 1;
		}
		return records;
	}

	ptrdiff_t IndexOf(std::vector<std::string> const& records, std::string const& record)
	{
		auto const found = std::find(records.begin(), records.end(), record);
		return found != records.end() ? found - records.begin() : -1;
	}

	// The first and the last index of records with a suffix.
	std::pair<ptrdiff_t, ptrdiff_t> FindRange(std::vector<std::string> const& records, std::vector<std::string> const& wanted)
	{
		ptrdiff_t first = static_cast<ptrdiff_t>(records.size());
		ptrdiff_t last = -1;
		for (std::string const& record : wanted)
		{
			ptrdiff_t const index = IndexOf(records, record);
			REQUIRE_MESSAGE(index >= 0, record);
			first = std::min(first, index);
			last = std::max(last, index);
		}
		return {first, last};
	}

	ScriptFieldValue ReadVariable(UUID entity, std::string const& name)
	{
		Result<ScriptFieldMap> variables = VisualScriptEngine::GetInstanceVariables(entity);
		REQUIRE_MESSAGE(variables.IsOk(), variables.GetError());
		ScriptFieldMap const& values = variables.GetValue();
		auto const variable = values.find(name);
		REQUIRE_MESSAGE(variable != values.end(), name);
		return variable->second;
	}
}

TEST_CASE("Scene: C# scripts and visual scripts start, update and end together, each event reaching the C# scripts first")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	RecordingHost host;
	AssetHandle const graph = AddGraph(MakeRecorderDocument(), "Recorder");
	Scene scene("Lifecycle");
	Entity recorder = AddRecorder(scene);
	// A C# script alone (reporting its calls), a visual script alone, both on one entity, and both on a parent and its child.
	AddScript(scene.CreateEntity("A"), "Strada.Tests.Lifecycle", MakeLifecycleFields("A", recorder, true, true));
	AddVisualScript(scene.CreateEntity("G"), graph, MakeRecorderVariables("G", recorder, true));
	Entity both = AddScript(scene.CreateEntity("Both"), "Strada.Tests.Lifecycle", MakeLifecycleFields("B", recorder, true));
	AddVisualScript(both, graph, MakeRecorderVariables("V", recorder, true));
	Entity parent = AddScript(scene.CreateEntity("Parent"), "Strada.Tests.Lifecycle", MakeLifecycleFields("P", recorder, false));
	AddVisualScript(parent, graph, MakeRecorderVariables("PV", recorder, false));
	Entity child = AddScript(scene.CreateEntity("Child", parent), "Strada.Tests.Lifecycle", MakeLifecycleFields("C", recorder, false));
	AddVisualScript(child, graph, MakeRecorderVariables("CV", recorder, false));

	// Every instance of both kinds exists before the first OnCreate; the C# scripts' OnCreate comes first.
	scene.OnRuntimeStart();
	CHECK(ScriptEngine::GetSceneContext() == &scene);
	CHECK(VisualScriptEngine::GetInstanceCount() == 4);
	CHECK(ScriptEngine::GetInstanceCount() == 8);
	REQUIRE_FALSE(host.Reports.empty());
	CHECK(host.Reports.front().Name == "A:Create");
	CHECK(host.Reports.front().GraphInstances == 4);
	CHECK(host.Reports.front().Instances == 8);
	std::vector<std::string> records = GetRecords(recorder);
	CHECK(records.size() == 8);
	auto const [csharpCreateFirst, csharpCreateLast] = FindRange(records, {"A:Create", "B:Create", "P:Create", "C:Create"});
	auto const [graphCreateFirst, graphCreateLast] = FindRange(records, {"G:Create", "V:Create", "PV:Create", "CV:Create"});
	CHECK(csharpCreateFirst == 0);
	CHECK(csharpCreateLast < graphCreateFirst);
	CHECK(graphCreateLast == 7);

	// One frame of one fixed step: every update (the C# scripts', then the visual scripts'), then every fixed update.
	recorder.GetComponent<TagComponent>().Tag.clear();
	scene.OnUpdateRuntime(Timestep(FrameTime));
	records = GetRecords(recorder);
	REQUIRE(records.size() == 8);
	auto const [csharpUpdateFirst, csharpUpdateLast] = FindRange(records, {"A:Update", "B:Update"});
	auto const [graphUpdateFirst, graphUpdateLast] = FindRange(records, {"G:Update", "V:Update"});
	auto const [csharpFixedFirst, csharpFixedLast] = FindRange(records, {"A:Fixed", "B:Fixed"});
	auto const [graphFixedFirst, graphFixedLast] = FindRange(records, {"G:Fixed", "V:Fixed"});
	CHECK(csharpUpdateFirst == 0);
	CHECK(csharpUpdateLast < graphUpdateFirst);
	CHECK(graphUpdateLast < csharpFixedFirst);
	CHECK(csharpFixedLast < graphFixedFirst);
	CHECK(graphFixedLast == 7);

	// A destroyed entity's scripts end at the end of the frame, the C# script first, before those of its children.
	recorder.GetComponent<TagComponent>().Tag.clear();
	scene.DestroyEntity(parent);
	scene.OnUpdateRuntime(Timestep(FrameTime));
	records = GetRecords(recorder);
	REQUIRE(records.size() == 12);
	CHECK(std::vector<std::string>(records.end() - 4, records.end()) ==
	      std::vector<std::string>{"P:Destroy", "PV:Destroy", "C:Destroy", "CV:Destroy"});
	CHECK(VisualScriptEngine::GetInstanceCount() == 2);
	CHECK(ScriptEngine::GetInstanceCount() == 4);

	// Stopping ends every C# script, then every visual script.
	recorder.GetComponent<TagComponent>().Tag.clear();
	scene.OnRuntimeStop();
	records = GetRecords(recorder);
	REQUIRE(records.size() == 4);
	auto const [csharpDestroyFirst, csharpDestroyLast] = FindRange(records, {"A:Destroy", "B:Destroy"});
	auto const [graphDestroyFirst, graphDestroyLast] = FindRange(records, {"G:Destroy", "V:Destroy"});
	CHECK(csharpDestroyFirst == 0);
	CHECK(csharpDestroyLast < graphDestroyFirst);
	CHECK(graphDestroyLast == 3);
	CHECK(ScriptEngine::GetInstanceCount() == 0);
	CHECK(ScriptEngine::GetSceneContext() == nullptr);
	CHECK(host.Exceptions == 0);
}

TEST_CASE("Scene: visual scripts follow VisualScript components added, changed and removed while running")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	uint64_t const logStart = Log::GetNextEntryIndex();
	AssetHandle const recorderGraph = AddGraph(MakeRecorderDocument(), "Recorder");
	AssetHandle const counterGraph = AddGraph(MakeCounterDocument(), "Counter");
	Scene scene("Changes");
	Entity recorder = AddRecorder(scene);
	Entity changing = AddVisualScript(scene.CreateEntity("Changing"), recorderGraph, MakeRecorderVariables("Changing", recorder, false));
	// Without a graph a component runs nothing, and says nothing.
	Entity const unset = AddVisualScript(scene.CreateEntity("Unset"), AssetHandle());
	scene.OnRuntimeStart();
	CHECK(VisualScriptEngine::HasInstance(changing.GetUUID()));
	CHECK_FALSE(VisualScriptEngine::HasInstance(unset.GetUUID()));
	CHECK_FALSE(Testing::WasLogged(logStart, "Entity 'Unset'"));

	// Added: created, with OnCreate, before its first update.
	Entity const added = AddVisualScript(scene.CreateEntity("Added"), counterGraph, {{"Speed", ScriptFieldValue::FromInt32(2)}});
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(ReadVariable(added.GetUUID(), "Updates") == ScriptFieldValue::FromInt32(2));

	// Another graph: the old instance ends first.
	changing.GetComponent<VisualScriptComponent>().Graph = counterGraph;
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(GetRecords(recorder).back() == "Changing:Destroy");
	CHECK(ReadVariable(changing.GetUUID(), "Updates") == ScriptFieldValue::FromInt32(1));

	// A graph that is absent is reported once and tried again when it changes; no script failed.
	AssetHandle const unregistered(UUID(424242));
	changing.GetComponent<VisualScriptComponent>().Graph = unregistered;
	scene.OnUpdateRuntime(Timestep(FrameTime));
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(CountLogged(logStart, "Entity 'Changing' runs no visual script: asset 424242 is not registered") == 1);
	CHECK_FALSE(VisualScriptEngine::HasInstance(changing.GetUUID()));
	changing.GetComponent<VisualScriptComponent>().Graph = counterGraph;
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(VisualScriptEngine::HasInstance(changing.GetUUID()));
	CHECK(host.Exceptions == 0);

	// Removed: OnDestroy runs; so does it for an entity destroyed with its component.
	changing.GetComponent<VisualScriptComponent>().Graph = recorderGraph;
	changing.GetComponent<VisualScriptComponent>().Variables = MakeRecorderVariables("Removed", recorder, false);
	scene.OnUpdateRuntime(Timestep(FrameTime));
	changing.RemoveComponent<VisualScriptComponent>();
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(GetRecords(recorder).back() == "Removed:Destroy");
	CHECK_FALSE(VisualScriptEngine::HasInstance(changing.GetUUID()));
	scene.DestroyEntity(added);
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(VisualScriptEngine::GetInstanceCount() == 0);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: prefab instances start their visual scripts before they are returned")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	AssetHandle const graph = AddGraph(MakeRecorderDocument(), "Recorder");
	// The prefab's root records into its own name: the references to it become references to each instance.
	Scene source("Source");
	Entity member = source.CreateEntity("Member");
	member.GetComponent<TagComponent>().Tag.clear();
	AddScript(member, "Strada.Tests.Lifecycle", MakeLifecycleFields("C#", member, false));
	AddVisualScript(member, graph, MakeRecorderVariables("Graph", member, false));
	Result<Ref<PrefabAsset>> prefabAsset = PrefabAsset::Create(PrefabSerializer::Serialize(source, member));
	REQUIRE(prefabAsset.IsOk());
	AssetHandle const prefab = AssetManager::AddMemoryAsset(prefabAsset.GetValue(), "Member");

	Scene scene("Prefabs");
	scene.OnRuntimeStart();
	Result<Entity> instance = scene.InstantiatePrefab(prefab, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), Entity());
	REQUIRE(instance.IsOk());
	Entity const root = instance.GetValue();
	CHECK(root.GetName() == "C#:Create,Graph:Create");
	CHECK(VisualScriptEngine::HasInstance(root.GetUUID()));
	CHECK(ScriptEngine::GetInstanceCount() == 2);
	scene.OnRuntimeStop();
	CHECK(root.GetName() == "C#:Create,Graph:Create,C#:Destroy,Graph:Destroy");
	CHECK(ScriptEngine::GetInstanceCount() == 0);
}

TEST_CASE("Scene: physics contacts reach both entities' C# and visual scripts, a C# script before the visual script")
{
	Testing::PhysicsSystemScope physics;
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	AssetHandle const graph = AddGraph(MakeRecorderDocument(), "Recorder");
	Scene scene("Contacts");
	Entity recorder = AddRecorder(scene);
	auto const addScripts = [&](Entity entity)
	{
		AddScript(entity, "Strada.Tests.Lifecycle", MakeLifecycleFields(entity.GetName(), recorder, false));
		AddVisualScript(entity, graph, MakeRecorderVariables(entity.GetName() + " graph", recorder, false));
	};

	Entity ground = scene.CreateEntity("Ground");
	ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
	ground.AddComponent<BoxColliderComponent>().HalfExtents = {10.0f, 0.5f, 10.0f};
	Entity zone = scene.CreateEntity("Zone");
	zone.GetComponent<TransformComponent>().Translation = {0.0f, 2.5f, 0.0f};
	BoxColliderComponent& trigger = zone.AddComponent<BoxColliderComponent>();
	trigger.HalfExtents = {2.0f, 1.0f, 2.0f};
	trigger.IsTrigger = true;
	addScripts(zone);
	Entity ball = scene.CreateEntity("Ball");
	ball.GetComponent<TransformComponent>().Translation = {0.0f, 6.0f, 0.0f};
	ball.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
	ball.AddComponent<SphereColliderComponent>().Radius = 0.5f;
	addScripts(ball);

	scene.OnRuntimeStart();
	for (int frame = 0; frame < 180; frame++)
	{
		scene.OnUpdateRuntime(Timestep(FrameTime));
	}
	std::vector<std::string> const records = GetRecords(recorder);
	// Per contact: the first entity's C# script, then its visual script, then the second entity's.
	ptrdiff_t const zoneEnter = IndexOf(records, "Zone:TriggerEnter(Ball scripted)");
	ptrdiff_t const ballEnter = IndexOf(records, "Ball:TriggerEnter(Zone scripted)");
	REQUIRE(zoneEnter >= 0);
	REQUIRE(ballEnter >= 0);
	CHECK(IndexOf(records, "Zone graph:TriggerEnter(Ball)") == zoneEnter + 1);
	CHECK(IndexOf(records, "Ball graph:TriggerEnter(Zone)") == ballEnter + 1);
	CHECK(std::abs(zoneEnter - ballEnter) == 2);
	ptrdiff_t const ballExit = IndexOf(records, "Ball:TriggerExit(Zone)");
	REQUIRE(ballExit >= 0);
	CHECK(IndexOf(records, "Ball graph:TriggerExit(Zone)") == ballExit + 1);
	ptrdiff_t const landed = IndexOf(records, "Ball:CollisionEnter(Ground)");
	REQUIRE(landed >= 0);
	CHECK(IndexOf(records, "Ball graph:CollisionEnter(Ground)") == landed + 1);
	CHECK(ballExit < landed);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: clicks reach the C# and visual scripts of the button and its ancestors, nearest first")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	AssetHandle const graph = AddGraph(MakeRecorderDocument(), "Recorder");
	Scene scene("Clicks");
	Entity recorder = AddRecorder(scene);
	Entity canvas = scene.CreateEntity("Canvas");
	canvas.AddComponent<UICanvasComponent>();
	Entity button = scene.CreateEntity("Button", canvas);
	button.AddComponent<UIRectComponent>();
	button.AddComponent<UIButtonComponent>();
	for (Entity entity : {canvas, button})
	{
		AddScript(entity, "Strada.Tests.Lifecycle", MakeLifecycleFields(entity.GetName(), recorder, false));
		AddVisualScript(entity, graph, MakeRecorderVariables(entity.GetName() + " graph", recorder, false));
	}

	scene.OnRuntimeStart();
	recorder.GetComponent<TagComponent>().Tag.clear();
	scene.ClickUIButton(button);
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(GetRecords(recorder) == std::vector<std::string>{"Button:Click(Button)", "Button graph:Click(Button)", "Canvas:Click(Button)",
	                                                       "Canvas graph:Click(Button)"});
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: running visual scripts take edited variables and their own values are read back")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	uint64_t const logStart = Log::GetNextEntryIndex();
	AssetHandle const graph = AddGraph(MakeCounterDocument(), "Counter");
	Scene scene("Live");
	Entity counter = AddVisualScript(scene.CreateEntity("Counter"), graph, {{"Speed", ScriptFieldValue::FromInt32(3)}});
	Entity const idle = scene.CreateEntity("Idle");

	// Before the scene runs, there is no graph instance to read.
	CHECK(scene.ReadVisualScriptVariables(counter).IsError());
	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(FrameTime));
	scene.OnUpdateRuntime(Timestep(FrameTime));

	// The values the instance holds, the ones it changed itself included, become the stored ones.
	Result<ScriptFieldMap> const read = scene.ReadVisualScriptVariables(counter);
	REQUIRE(read.IsOk());
	CHECK(read.GetValue() == ScriptFieldMap{{"Speed", ScriptFieldValue::FromInt32(3)}, {"Updates", ScriptFieldValue::FromInt32(6)}});
	CHECK(counter.GetComponent<VisualScriptComponent>().Variables == read.GetValue());
	CHECK(scene.ReadVisualScriptVariables(idle).IsError());

	// Edited stored values reach the instance at once; the ones it changed itself and that were not edited stay its own.
	scene.OnUpdateRuntime(Timestep(FrameTime));
	counter.GetComponent<VisualScriptComponent>().Variables["Speed"] = ScriptFieldValue::FromInt32(5);
	scene.ApplyVisualScriptVariableChanges();
	CHECK(ReadVariable(counter.GetUUID(), "Speed") == ScriptFieldValue::FromInt32(5));
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(ReadVariable(counter.GetUUID(), "Updates") == ScriptFieldValue::FromInt32(14));

	// Values of another type are reported and skipped; values of variables the graph does not have are skipped.
	counter.GetComponent<VisualScriptComponent>().Variables["Speed"] = ScriptFieldValue::FromString("fast");
	counter.GetComponent<VisualScriptComponent>().Variables["Removed"] = ScriptFieldValue::FromInt32(1);
	scene.ApplyVisualScriptVariableChanges();
	CHECK(ReadVariable(counter.GetUUID(), "Speed") == ScriptFieldValue::FromInt32(5));
	CHECK(
		Testing::WasLogged(logStart, "Counter on Counter (" + counter.GetUUID().ToString() + "): Speed: stored \"String\" value ignored"));

	// After stopping there is nothing to read.
	scene.OnRuntimeStop();
	CHECK(scene.ReadVisualScriptVariables(counter).IsError());
}

TEST_CASE("Scene: stored variable values reach renamed variables, and entity values follow copies of their entities")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	VisualScriptDocument document = MakeCounterDocument();
	document.Variables[0].FormerNames = {"Velocity"};
	document.Variables.push_back(MakeVariable(3, "Partner", "Strada.Entity"));
	AssetHandle const graph = AddGraph(std::move(document), "Counter");
	Scene scene("Overrides");

	// A value stored under the former name applies; of two values the one under the current name does, as the inspector
	// shows them.
	Entity const renamed = AddVisualScript(scene.CreateEntity("Renamed"), graph, {{"Velocity", ScriptFieldValue::FromInt32(4)}});
	Entity const both = AddVisualScript(scene.CreateEntity("Both"), graph,
	                                    {{"Speed", ScriptFieldValue::FromInt32(7)}, {"Velocity", ScriptFieldValue::FromInt32(4)}});
	// A parent whose graph refers to its child, copied: the copy refers to the copy of the child.
	Entity parent = scene.CreateEntity("Parent");
	Entity const child = scene.CreateEntity("Child", parent);
	AddVisualScript(parent, graph, {{"Partner", ScriptFieldValue::FromEntity(child.GetUUID())}});
	Entity const copy = scene.DuplicateEntity(parent);
	std::vector<Entity> const copyChildren = scene.GetChildren(copy);
	REQUIRE(copyChildren.size() == 1);

	scene.OnRuntimeStart();
	CHECK(ReadVariable(renamed.GetUUID(), "Speed") == ScriptFieldValue::FromInt32(4));
	CHECK(ReadVariable(both.GetUUID(), "Speed") == ScriptFieldValue::FromInt32(7));
	CHECK(ReadVariable(parent.GetUUID(), "Partner") == ScriptFieldValue::FromEntity(child.GetUUID()));
	CHECK(ReadVariable(copy.GetUUID(), "Partner") == ScriptFieldValue::FromEntity(copyChildren.front().GetUUID()));

	// Read back, the values are stored under the current names only.
	REQUIRE(scene.ReadVisualScriptVariables(both).IsOk());
	ScriptFieldMap const& stored = both.GetComponent<VisualScriptComponent>().Variables;
	CHECK_FALSE(stored.contains("Velocity"));
	CHECK(stored.at("Speed") == ScriptFieldValue::FromInt32(7));

	// An edit under the former name reaches the renamed variable.
	Entity editing = renamed;
	editing.GetComponent<VisualScriptComponent>().Variables = {{"Velocity", ScriptFieldValue::FromInt32(9)}};
	scene.ApplyVisualScriptVariableChanges();
	CHECK(ReadVariable(renamed.GetUUID(), "Speed") == ScriptFieldValue::FromInt32(9));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: visual scripts run without game scripts, which start only when loaded before the scene")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	uint64_t const logStart = Log::GetNextEntryIndex();
	AssetHandle const graph = AddGraph(MakeCounterDocument(), "Counter");

	// With a visual script the scene runs it, and its C# scripts wait for a game assembly loaded before it starts.
	Scene scene("Graphs");
	Entity const counter = AddVisualScript(scene.CreateEntity("Counter"), graph);
	Entity const mover = AddScript(scene.CreateEntity("Mover"), "Strada.Tests.Mover");
	scene.OnRuntimeStart();
	CHECK(ScriptEngine::GetSceneContext() == &scene);
	CHECK(Testing::WasLogged(logStart, "Scene 'Graphs' runs without C# scripts: no game assembly is loaded"));
	scene.OnUpdateRuntime(Timestep(0.5f));
	CHECK(ReadVariable(counter.GetUUID(), "Updates") == ScriptFieldValue::FromInt32(1));
	CHECK(mover.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
	CHECK(ScriptEngine::GetInstanceCount() == 1);
	scene.OnRuntimeStop();

	// Game scripts loaded while it runs (no instance holds the scripts) do not start its C# scripts.
	Entity graphLess = counter;
	graphLess.GetComponent<VisualScriptComponent>().Graph = AssetHandle();
	scene.OnRuntimeStart();
	CHECK(ScriptEngine::GetSceneContext() == &scene);
	REQUIRE(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
	scene.OnUpdateRuntime(Timestep(0.5f));
	CHECK_FALSE(ScriptEngine::HasInstance(mover.GetUUID()));
	CHECK(mover.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
	scene.OnRuntimeStop();
	// Loaded before the scene starts, they run.
	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(0.5f));
	CHECK(ScriptEngine::HasInstance(mover.GetUUID()));
	CHECK(mover.GetComponent<TransformComponent>().Translation.x == doctest::Approx(0.5f));
	scene.OnRuntimeStop();

	// A scene that starts with neither runs no visual script added later.
	ScriptEngine::UnloadGameAssembly();
	Scene plain("Plain");
	AddScript(plain.CreateEntity("Mover"), "Strada.Tests.Mover");
	plain.OnRuntimeStart();
	CHECK(Testing::WasLogged(logStart, "Scene 'Plain' runs without scripts: no game assembly is loaded"));
	Entity const late = AddVisualScript(plain.CreateEntity("Late"), AddGraph(MakeCounterDocument(), "Late"));
	plain.OnUpdateRuntime(Timestep(0.5f));
	CHECK_FALSE(VisualScriptEngine::HasInstance(late.GetUUID()));
	CHECK(ScriptEngine::GetSceneContext() == nullptr);
	plain.OnRuntimeStop();

	// Simulating runs no visual script.
	Entity graphAgain = counter;
	graphAgain.GetComponent<VisualScriptComponent>().Graph = graph;
	SceneRuntimeSettings simulate;
	simulate.RunScripts = false;
	scene.OnRuntimeStart(simulate);
	scene.OnUpdateRuntime(Timestep(0.5f));
	CHECK(VisualScriptEngine::GetInstanceCount() == 0);
	CHECK(ScriptEngine::GetSceneContext() == nullptr);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: running visual scripts take edits of their graphs, and a graph fixed while running starts its entities")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	uint64_t logStart = Log::GetNextEntryIndex();
	Ref<VisualScriptAsset> const delayed = CreateRef<VisualScriptAsset>(MakeDelayDocument());
	AssetHandle const delayedGraph = AssetManager::AddMemoryAsset(delayed, "Delayed");
	Ref<VisualScriptAsset> const broken = CreateRef<VisualScriptAsset>(MakeBrokenDocument());
	AssetHandle const brokenGraph = AssetManager::AddMemoryAsset(broken, "Broken");
	Scene scene("Edits");
	Entity const waiter = AddVisualScript(scene.CreateEntity("Waiter"), delayedGraph);
	Entity const fixedLater = AddVisualScript(scene.CreateEntity("Fixed later"), brokenGraph, {{"Speed", ScriptFieldValue::FromInt32(4)}});

	// A graph that does not compile starts no instance: a script exception, reported once.
	scene.OnRuntimeStart();
	CHECK(VisualScriptEngine::HasInstance(waiter.GetUUID()));
	CHECK_FALSE(VisualScriptEngine::HasInstance(fixedLater.GetUUID()));
	CHECK(host.Exceptions == 1);
	CHECK(CountLogged(logStart, "Entity 'Fixed later' runs no visual script: the visual script 'Broken' cannot run (see the log)") == 1);
	scene.OnUpdateRuntime(Timestep(0.25f));
	CHECK(host.Exceptions == 1);

	// An edit that leaves the Delay alone keeps it pending in the running instance; a variable it adds starts at its value.
	uint64_t const before = VisualScriptEngine::GetProgramGeneration(delayedGraph);
	VisualScriptDocument edited = MakeDelayDocument();
	edited.Variables.push_back(MakeVariable(2, "Extra", "int", 7));
	delayed->SetDocument(edited);
	scene.OnUpdateRuntime(Timestep(0.1f));
	CHECK(VisualScriptEngine::GetProgramGeneration(delayedGraph) != before);
	CHECK(ReadVariable(waiter.GetUUID(), "Done") == ScriptFieldValue::FromBool(false));
	CHECK(ReadVariable(waiter.GetUUID(), "Extra") == ScriptFieldValue::FromInt32(7));
	scene.OnUpdateRuntime(Timestep(0.25f));
	CHECK(ReadVariable(waiter.GetUUID(), "Done") == ScriptFieldValue::FromBool(true));

	// Fixed while running: its entities start at the next update, with their stored values.
	broken->SetDocument(MakeCounterDocument());
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(VisualScriptEngine::HasInstance(fixedLater.GetUUID()));
	CHECK(ReadVariable(fixedLater.GetUUID(), "Updates") == ScriptFieldValue::FromInt32(4));
	CHECK(host.Exceptions == 1);
	scene.OnRuntimeStop();

	// An edit that removes the pending Delay cancels it.
	scene.OnRuntimeStart();
	logStart = Log::GetNextEntryIndex();
	VisualScriptDocument withoutDelay = edited;
	withoutDelay.Graph.Nodes.erase(withoutDelay.Graph.Nodes.begin() + 1);
	withoutDelay.Graph.Links.clear();
	delayed->SetDocument(withoutDelay);
	scene.OnUpdateRuntime(Timestep(0.25f));
	scene.OnUpdateRuntime(Timestep(0.5f));
	CHECK(Testing::WasLogged(logStart, "is cancelled"));
	CHECK(ReadVariable(waiter.GetUUID(), "Done") == ScriptFieldValue::FromBool(false));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: a graph file saved and loaded again unchanged is not swapped twice in running instances")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const file = directory.GetPath() / "Delayed.sgraph";
	REQUIRE(VisualScriptSerializer::SaveToFile(MakeDelayDocument(), file).IsOk());
	Testing::AssetManagerScope assets;
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());
	AssetHandle const graph = AssetManager::FindByPath("Delayed.sgraph");
	REQUIRE(graph.IsValid());
	Testing::ScriptRuntimeScope runtime;
	Scene scene("Saved");
	Entity const waiter = AddVisualScript(scene.CreateEntity("Waiter"), graph);
	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(0.1f));

	// An editor's save: the asset changes, then the file.
	VisualScriptDocument edited = MakeDelayDocument();
	edited.Variables.push_back(MakeVariable(2, "Extra", "int", 7));
	Ref<VisualScriptAsset> const asset = AssetManager::GetAsset<VisualScriptAsset>(graph);
	REQUIRE(asset != nullptr);
	asset->SetDocument(edited);
	REQUIRE(VisualScriptSerializer::SaveToFile(edited, file).IsOk());
	scene.OnUpdateRuntime(Timestep(0.1f));
	uint64_t const saved = VisualScriptEngine::GetProgramGeneration(graph);

	// The file loaded again holds the same document: the runtime keeps the program it has.
	REQUIRE(AssetManager::ReloadAsset(graph).IsOk());
	CHECK(AssetManager::GetAsset<VisualScriptAsset>(graph) != asset);
	scene.OnUpdateRuntime(Timestep(0.1f));
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) == saved);
	scene.OnUpdateRuntime(Timestep(0.3f));
	CHECK(ReadVariable(waiter.GetUUID(), "Done") == ScriptFieldValue::FromBool(true));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: failing visual scripts are script exceptions, an endless loop one, and the scene runs on")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Failures");
	AddVisualScript(scene.CreateEntity("Divider"), AddGraph(MakeDivisionDocument(), "Division"));
	Entity const looper = AddVisualScript(scene.CreateEntity("Looper"), AddGraph(MakeEndlessDocument(), "Endless"));
	Entity const counter = AddVisualScript(scene.CreateEntity("Counter"), AddGraph(MakeCounterDocument(), "Counter"));

	// The endless loop ends at the step budget once, as one failure; its instance runs on.
	scene.OnRuntimeStart();
	CHECK(host.Exceptions == 1);
	CHECK(CountLogged(logStart, "execution exceeded 1,000,000 steps (endless loop?)") == 1);
	CHECK(VisualScriptEngine::HasInstance(looper.GetUUID()));

	// Each update the division fails again; the other graphs run on.
	scene.OnUpdateRuntime(Timestep(FrameTime));
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(host.Exceptions == 3);
	CHECK(CountLogged(logStart, "integer division by zero") == 2);
	CHECK(ReadVariable(counter.GetUUID(), "Updates") == ScriptFieldValue::FromInt32(2));
	scene.OnRuntimeStop();
}
