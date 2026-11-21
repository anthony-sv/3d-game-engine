#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/VisualScriptEngine.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

// The read-back of a running scene's scripts reads across the script boundary only what changed since the instance's last
// (Docs/Architecture.md §11, "Long lists"), while every reader still finds the component holding every running value.
namespace
{
	constexpr float FrameTime = 1.0f / 60.0f;
	constexpr char const* Writer = "Strada.Tests.ListWriter";

	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type, Json value = Json())
	{
		VisualScriptVariable variable;
		variable.ID = id;
		variable.Name = std::move(name);
		variable.Type = std::move(type);
		variable.Value = std::move(value);
		variable.Public = true;
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

	// A list that no update changes beside a clock every update advances: Samples (float[]), Clock (float, 1 more on every
	// update) and Steady (int).
	VisualScriptDocument MakeClockDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Samples", "float[]", Json::array()));
		document.Variables.push_back(MakeVariable(2, "Clock", "float", 0.0));
		document.Variables.push_back(MakeVariable(3, "Steady", "int", 7));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 2}})));
		document.Graph.Nodes.push_back(MakeNode(12, "Operator.Add", Json::object(), Json::object({{"B", 1.0}})));
		document.Graph.Nodes.push_back(MakeNode(13, "Variable.Get", Json::object({{"Variable", 2}})));
		document.Graph.Links.push_back({{10, "Then"}, {11, "Execute"}});
		document.Graph.Links.push_back({{13, "Value"}, {12, "A"}});
		document.Graph.Links.push_back({{12, "Result"}, {11, "Value"}});
		return document;
	}

	ScriptFieldValue MakeSamples(size_t count, float start)
	{
		std::vector<ScriptFieldValue> elements;
		elements.reserve(count);
		for (size_t i = 0; i < count; i++)
		{
			elements.push_back(ScriptFieldValue::FromFloat(start + static_cast<float>(i)));
		}
		return ScriptFieldValue::FromList(ScriptFieldType::Float, std::move(elements));
	}

	Entity AddVisualScript(Scene& scene, AssetHandle graph, ScriptFieldMap variables)
	{
		Entity entity = scene.CreateEntity("Clock");
		VisualScriptComponent& component = entity.AddComponent<VisualScriptComponent>();
		component.Graph = graph;
		component.Variables = std::move(variables);
		return entity;
	}

	Entity AddWriter(Scene& scene, ScriptFieldMap fields)
	{
		Entity entity = scene.CreateEntity("Writer");
		ScriptComponent& script = entity.AddComponent<ScriptComponent>();
		script.ClassName = Writer;
		script.Fields = std::move(fields);
		return entity;
	}

	size_t Refresh(Scene& scene, Entity entity, bool visualScript)
	{
		Result<size_t> const read = visualScript ? scene.RefreshVisualScriptVariables(entity) : scene.RefreshScriptFields(entity);
		REQUIRE_MESSAGE(read.IsOk(), read.GetError());
		return read.GetValue();
	}
}

TEST_CASE("Scene: a graph's variables are read back only where they changed since the instance's last read-back")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	Ref<VisualScriptAsset> const asset = CreateRef<VisualScriptAsset>(MakeClockDocument());
	AssetHandle const graph = AssetManager::AddMemoryAsset(asset, "Clock");
	Scene scene("ReadBack");
	ScriptFieldValue const samples = MakeSamples(MaxScriptFieldListElements, 0.0f);
	Entity clock = AddVisualScript(scene, graph, {{"Samples", samples}});
	VisualScriptComponent& component = clock.GetComponent<VisualScriptComponent>();
	CHECK(scene.RefreshVisualScriptVariables(clock).IsError());

	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(FrameTime));
	// The instance's first read-back answers every public variable; one with nothing changed answers none.
	CHECK(Refresh(scene, clock, true) == 3);
	CHECK(component.Variables.at("Clock") == ScriptFieldValue::FromFloat(1.0f));
	CHECK(component.Variables.at("Samples") == samples);
	CHECK(Refresh(scene, clock, true) == 0);
	// An update changes the clock alone.
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(Refresh(scene, clock, true) == 1);
	CHECK(component.Variables.at("Clock") == ScriptFieldValue::FromFloat(2.0f));

	// ReadVisualScriptVariables answers every variable whatever changed, and reads through the same copies.
	Result<ScriptFieldMap> const all = scene.ReadVisualScriptVariables(clock);
	REQUIRE_MESSAGE(all.IsOk(), all.GetError());
	CHECK(all.GetValue() ==
	      ScriptFieldMap{{"Clock", ScriptFieldValue::FromFloat(2.0f)}, {"Samples", samples}, {"Steady", ScriptFieldValue::FromInt32(7)}});
	CHECK(Refresh(scene, clock, true) == 0);

	// A value given is answered again, whatever became of it: a list taken, a value of another type refused (the running
	// value comes back).
	ScriptFieldValue const shorter = MakeSamples(3, 10.0f);
	component.Variables["Samples"] = shorter;
	component.Variables["Steady"] = ScriptFieldValue::FromFloat(1.5f);
	scene.ApplyVisualScriptVariableChanges();
	CHECK(Refresh(scene, clock, true) == 2);
	CHECK(component.Variables.at("Samples") == shorter);
	CHECK(component.Variables.at("Steady") == ScriptFieldValue::FromInt32(7));

	// A map replaced whole (a paste, component.set) without a variable gets it back from what was last read, which the
	// runtime does not answer again.
	component.Variables.erase("Samples");
	CHECK(Refresh(scene, clock, true) == 0);
	CHECK(component.Variables.at("Samples") == shorter);
	component.Variables.clear();
	Result<ScriptFieldMap> const restored = scene.ReadVisualScriptVariables(clock);
	REQUIRE_MESSAGE(restored.IsOk(), restored.GetError());
	CHECK(restored.GetValue().size() == 3);
	CHECK(component.Variables == restored.GetValue());

	// A live edit gives the instance another program: every variable is answered again, the new one included.
	VisualScriptDocument edited = MakeClockDocument();
	edited.Variables.push_back(MakeVariable(4, "Extra", "int", 3));
	asset->SetDocument(std::move(edited));
	scene.ApplyVisualScriptEdits();
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(Refresh(scene, clock, true) == 4);
	CHECK(component.Variables.at("Extra") == ScriptFieldValue::FromInt32(3));
	CHECK(component.Variables.at("Samples") == shorter);
	CHECK(Refresh(scene, clock, true) == 0);
	scene.OnRuntimeStop();
	CHECK(scene.RefreshVisualScriptVariables(clock).IsError());
}

TEST_CASE("Scene: a script's fields are read back only where they changed since the instance's last read-back")
{
	Testing::ScriptEngineScope scripting;
	Scene scene("ReadBack");
	// Element i holds 100 + i, so that each count an update writes changes its element.
	ScriptFieldValue const samples = MakeSamples(MaxScriptFieldListElements, 100.0f);
	Entity writer = AddWriter(scene, {{"Samples", samples}});
	ScriptComponent& script = writer.GetComponent<ScriptComponent>();
	CHECK(scene.RefreshScriptFields(writer).IsError());

	scene.OnRuntimeStart();
	// Before an update: every field at the first read-back (Samples, Updates, Length, Steady), none at the next.
	CHECK(Refresh(scene, writer, false) == 4);
	CHECK(script.Fields.at("Samples") == samples);
	CHECK(Refresh(scene, writer, false) == 0);
	// An update writes its count into Samples[1].
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(Refresh(scene, writer, false) == 2);
	CHECK(script.Fields.at("Updates") == ScriptFieldValue::FromInt32(1));
	CHECK(script.Fields.at("Samples").GetElements()[1] == ScriptFieldValue::FromFloat(1.0f));
	CHECK(Refresh(scene, writer, false) == 0);

	// ReadScriptFields answers every field, and a reader between two refreshes leaves the component holding what runs.
	scene.OnUpdateRuntime(Timestep(FrameTime));
	Result<ScriptFieldMap> const all = scene.ReadScriptFields(writer);
	REQUIRE_MESSAGE(all.IsOk(), all.GetError());
	CHECK(all.GetValue().size() == 4);
	CHECK(all.GetValue().at("Updates") == ScriptFieldValue::FromInt32(2));
	CHECK(Refresh(scene, writer, false) == 0);
	CHECK(script.Fields.at("Updates") == ScriptFieldValue::FromInt32(2));
	CHECK(script.Fields.at("Samples").GetElements()[2] == ScriptFieldValue::FromFloat(2.0f));

	// A value given is answered again, whatever became of it.
	script.Fields["Steady"] = ScriptFieldValue::FromString("seven");
	script.Fields["Length"] = ScriptFieldValue::FromInt32(5);
	scene.ApplyScriptFieldChanges();
	CHECK(Refresh(scene, writer, false) == 2);
	CHECK(script.Fields.at("Steady") == ScriptFieldValue::FromInt32(7));
	CHECK(script.Fields.at("Length") == ScriptFieldValue::FromInt32(5));
	// The game shrinks the list to five elements: one changed list, answered whole.
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(Refresh(scene, writer, false) == 2);
	CHECK(script.Fields.at("Samples").GetElements().size() == 5);

	// A map replaced whole gets back what the runtime does not answer again.
	script.Fields.clear();
	CHECK(Refresh(scene, writer, false) == 0);
	CHECK(script.Fields.size() == 4);
	CHECK(script.Fields.at("Samples").GetElements().size() == 5);

	// A new instance (another class, then this one again) answers every field again.
	script.ClassName = "Strada.Tests.FieldReporter";
	scene.OnUpdateRuntime(Timestep(FrameTime));
	script.ClassName = Writer;
	scene.OnUpdateRuntime(Timestep(FrameTime));
	CHECK(Refresh(scene, writer, false) == 4);
	scene.OnRuntimeStop();
	CHECK(scene.RefreshScriptFields(writer).IsError());
}
