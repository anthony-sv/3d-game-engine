#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/InputActionMap.h"
#include "Strada/Asset/InputActionsAsset.h"
#include "Strada/Asset/PrefabAsset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/InputActions.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Script/ScriptBindings.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/VisualScriptEngine.h"

#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

// Input actions as C# scripts and graphs see them (Docs/Architecture.md section 7.4): the queries in OnUpdate and in fixed
// steps, the action events once per edge with C# first, overrides set by scripts, a debugger-free resume of step-started
// code (a latent node's continuation reads no edge), unknown names and a project without actions. The evaluation itself is
// SceneInputActionsTests.cpp's and InputActionsTests.cpp's.
namespace
{
	constexpr float Step = 1.0f / 60.0f;
	constexpr std::string_view ProbeClass = "Strada.Tests.ActionProbe";

	struct InputScope
	{
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }
		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	InputAction MakeAction(std::string name, InputActionType type, std::string controls)
	{
		InputActionBinding binding;
		binding.Controls = std::move(controls);
		InputAction action;
		action.Name = std::move(name);
		action.Type = type;
		action.Bindings.push_back(std::move(binding));
		return action;
	}

	// Jump on Space, Steer on A and D, Move on WASD.
	AssetHandle AddActions()
	{
		InputActionMap map;
		map.Actions.push_back(MakeAction("Jump", InputActionType::Button, "Key/Space"));
		map.Actions.push_back(MakeAction("Steer", InputActionType::Axis, "Key/A Key/D"));
		map.Actions.push_back(MakeAction("Move", InputActionType::Vector2, "Key/W Key/S Key/A Key/D"));
		TypedDataAssetType<InputActionMap> const* type = DataAssetRegistry::Find<InputActionMap>();
		REQUIRE(type != nullptr);
		Ref<TypedDataAsset<InputActionMap>> const asset = std::static_pointer_cast<TypedDataAsset<InputActionMap>>(type->CreateAsset());
		REQUIRE(asset->SetData(map).IsOk());
		return AssetManager::AddMemoryAsset(asset, "Actions.sinput");
	}

	SceneRuntimeSettings Settings(AssetHandle actions, uint32_t maxSteps = 8)
	{
		SceneRuntimeSettings settings;
		settings.PlayAudio = false;
		settings.FixedTimestep = Step;
		settings.MaxStepsPerFrame = maxSteps;
		settings.InputActions = actions;
		return settings;
	}

	Entity AddProbe(Scene& scene, std::string const& name, ScriptFieldMap fields = {})
	{
		Entity entity = scene.CreateEntity(name);
		ScriptComponent& script = entity.AddComponent<ScriptComponent>();
		script.ClassName = std::string(ProbeClass);
		script.Fields = std::move(fields);
		return entity;
	}

	ScriptFieldMap ReadProbe(Entity probe)
	{
		Result<ScriptFieldMap> fields = ScriptEngine::GetInstanceFields(probe.GetUUID());
		REQUIRE_MESSAGE(fields.IsOk(), (fields ? std::string() : fields.GetError()));
		return fields.TakeValue();
	}

	ScriptFieldMap ReadGraph(Entity entity)
	{
		Result<ScriptFieldMap> variables = VisualScriptEngine::GetInstanceVariables(entity.GetUUID());
		REQUIRE_MESSAGE(variables.IsOk(), (variables ? std::string() : variables.GetError()));
		return variables.TakeValue();
	}

	// One frame as the player runs it: the scene's update, then the end of the input frame.
	void RunFrame(Scene& scene, float frameTime = Step)
	{
		scene.OnUpdateRuntime(Timestep(frameTime));
		Input::EndFrame();
	}

	size_t CountLogged(uint64_t first, std::string_view text)
	{
		std::vector<LogEntry> const entries = Log::GetEntries(first);
		return static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
		                                         [text](LogEntry const& entry)
		                                         {
													 return entry.Message.find(text) != std::string::npos;
												 }));
	}

	// The messages logged since first that hold one of the texts, in their order.
	std::vector<std::string> FindLogged(uint64_t first, std::vector<std::string_view> const& texts)
	{
		std::vector<std::string> found;
		for (LogEntry const& entry : Log::GetEntries(first))
		{
			for (std::string_view const text : texts)
			{
				if (entry.Message.find(text) != std::string::npos)
				{
					found.emplace_back(text);
					break;
				}
			}
		}
		return found;
	}

	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type, Json value)
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

	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}

	// Nodes from `id` on that add 1 to the int variable when the flow leaves (from, pin); answers the node to continue from
	// (its Then).
	uint32_t AddIncrement(VisualScriptDocument& document, uint32_t from, std::string pin, uint32_t variable, uint32_t id)
	{
		document.Graph.Nodes.push_back(MakeNode(id, "Variable.Set", Json::object({{"Variable", variable}})));
		document.Graph.Nodes.push_back(MakeNode(id + 1, "Operator.Add", Json::object(), Json::object({{"B", 1}})));
		document.Graph.Nodes.push_back(MakeNode(id + 2, "Variable.Get", Json::object({{"Variable", variable}})));
		document.Graph.Links.push_back(MakeLink(from, std::move(pin), id, "Execute"));
		document.Graph.Links.push_back(MakeLink(id + 2, "Value", id + 1, "A"));
		document.Graph.Links.push_back(MakeLink(id + 1, "Result", id, "Value"));
		return id;
	}

	// Nodes from `id` on that log the message when the flow leaves (from, pin); answers the node to continue from.
	uint32_t AddLog(VisualScriptDocument& document, uint32_t from, std::string pin, std::string message, uint32_t id)
	{
		document.Graph.Nodes.push_back(
			MakeNode(id, "Call:Strada.Log.Info(string)", Json::object(), Json::object({{"Message", std::move(message)}})));
		document.Graph.Links.push_back(MakeLink(from, std::move(pin), id, "Execute"));
		return id;
	}

	// Event.ActionPressed and Event.ActionReleased of Jump, each logged ("graph pressed Jump") and counted (Presses,
	// Releases), and Event.ActionPressed of Steer, counted (SteerPresses).
	VisualScriptDocument MakeEventsDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Presses", "int", 0));
		document.Variables.push_back(MakeVariable(2, "Releases", "int", 0));
		document.Variables.push_back(MakeVariable(3, "SteerPresses", "int", 0));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.ActionPressed", Json::object({{"Action", "Jump"}})));
		AddIncrement(document, AddLog(document, 10, "Then", "graph pressed Jump", 11), "Then", 1, 12);
		document.Graph.Nodes.push_back(MakeNode(20, "Event.ActionReleased", Json::object({{"Action", "Jump"}})));
		AddIncrement(document, AddLog(document, 20, "Then", "graph released Jump", 21), "Then", 2, 22);
		document.Graph.Nodes.push_back(MakeNode(30, "Event.ActionPressed", Json::object({{"Action", "Steer"}})));
		AddIncrement(document, 30, "Then", 3, 31);
		return document;
	}

	// Event.FixedUpdate counts the steps whose IsActionPressed(Jump) is true (StepPresses); the first step that sees none
	// starts a Delay of 0 s whose continuation, in the next frame's Update dispatch, keeps what IsActionPressed(Jump) reads
	// then (FixedLateSaw). Event.Update's first run does the same from the frame (UpdateLateSaw).
	VisualScriptDocument MakeLateReadsDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "StepPresses", "int", 0));
		document.Variables.push_back(MakeVariable(2, "FixedLateSaw", "bool", false));
		document.Variables.push_back(MakeVariable(3, "UpdateLateSaw", "bool", false));
		document.Variables.push_back(MakeVariable(4, "FixedLateRan", "bool", false));
		document.Variables.push_back(MakeVariable(5, "UpdateLateRan", "bool", false));
		Json const jump = Json::object({{"Action", "Jump"}});
		std::vector<VisualScriptNode>& nodes = document.Graph.Nodes;
		std::vector<VisualScriptLink>& links = document.Graph.Links;
		nodes.push_back(MakeNode(10, "Event.FixedUpdate"));
		nodes.push_back(MakeNode(11, "Flow.Branch"));
		nodes.push_back(MakeNode(12, "Call:Strada.Input.IsActionPressed(string)", Json::object(), jump));
		links.push_back(MakeLink(10, "Then", 11, "Execute"));
		links.push_back(MakeLink(12, "Result", 11, "Condition"));
		AddIncrement(document, 11, "True", 1, 13);
		struct Late
		{
			uint32_t From;
			std::string Pin;
			uint32_t Saw;
			uint32_t Ran;
			uint32_t First;
		};
		nodes.push_back(MakeNode(40, "Event.Update"));
		for (Late const& late : {Late{11, "False", 2, 4, 20}, Late{40, "Then", 3, 5, 50}})
		{
			uint32_t const id = late.First;
			nodes.push_back(MakeNode(id, "Flow.DoOnce"));
			nodes.push_back(MakeNode(id + 1, "Flow.Delay", Json::object(), Json::object({{"Duration", 0.0}})));
			nodes.push_back(MakeNode(id + 2, "Variable.Set", Json::object({{"Variable", late.Saw}})));
			nodes.push_back(MakeNode(id + 3, "Call:Strada.Input.IsActionPressed(string)", Json::object(), jump));
			nodes.push_back(MakeNode(id + 4, "Variable.Set", Json::object({{"Variable", late.Ran}}), Json::object({{"Value", true}})));
			links.push_back(MakeLink(late.From, late.Pin, id, "Execute"));
			links.push_back(MakeLink(id, "Completed", id + 1, "Execute"));
			links.push_back(MakeLink(id + 1, "Completed", id + 2, "Execute"));
			links.push_back(MakeLink(id + 3, "Result", id + 2, "Value"));
			links.push_back(MakeLink(id + 2, "Then", id + 4, "Execute"));
		}
		return document;
	}

	AssetHandle AddGraph(VisualScriptDocument document, std::string name)
	{
		return AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(std::move(document)), std::move(name));
	}

	Entity AddGraphEntity(Scene& scene, std::string const& name, AssetHandle graph)
	{
		Entity entity = scene.CreateEntity(name);
		entity.AddComponent<VisualScriptComponent>().Graph = graph;
		return entity;
	}

	// Event.Custom Step counts its runs (SentSteps) and those in which IsActionPressed(Jump) was true (SentPresses); with
	// sendFromStep, Event.FixedUpdate raises Step on the graph itself (Event.Send).
	VisualScriptDocument MakeStepReceiverDocument(bool sendFromStep)
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "SentSteps", "int", 0));
		document.Variables.push_back(MakeVariable(2, "SentPresses", "int", 0));
		std::vector<VisualScriptNode>& nodes = document.Graph.Nodes;
		std::vector<VisualScriptLink>& links = document.Graph.Links;
		nodes.push_back(MakeNode(10, "Event.Custom", Json::object({{"Name", "Step"}, {"Parameters", Json::array()}})));
		AddIncrement(document, 10, "Then", 1, 11);
		nodes.push_back(MakeNode(20, "Flow.Branch"));
		nodes.push_back(MakeNode(21, "Call:Strada.Input.IsActionPressed(string)", Json::object(), Json::object({{"Action", "Jump"}})));
		links.push_back(MakeLink(11, "Then", 20, "Execute"));
		links.push_back(MakeLink(21, "Result", 20, "Condition"));
		AddIncrement(document, 20, "True", 2, 22);
		if (sendFromStep)
		{
			nodes.push_back(MakeNode(30, "Event.FixedUpdate"));
			nodes.push_back(MakeNode(31, "Event.Send", Json::object({{"Name", "Step"}, {"Parameters", Json::array()}})));
			links.push_back(MakeLink(30, "Then", 31, "Execute"));
		}
		return document;
	}

	// Event.FixedUpdate counts the steps (Steps) and those that see IsActionPressed(Jump) (StepPresses). With delay, the first
	// step that sees no press starts a Delay of 0 s whose continuation keeps what IsActionPressed(Jump) reads (DelaySaw,
	// DelayRan); otherwise it starts a WaitUntil of IsActionDown(Jump), whose continuation does (WaitSaw, WaitRan).
	VisualScriptDocument MakeStepLatentDocument(bool delay)
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Steps", "int", 0));
		document.Variables.push_back(MakeVariable(2, "StepPresses", "int", 0));
		document.Variables.push_back(MakeVariable(3, delay ? "DelaySaw" : "WaitSaw", "bool", false));
		document.Variables.push_back(MakeVariable(4, delay ? "DelayRan" : "WaitRan", "bool", false));
		Json const jump = Json::object({{"Action", "Jump"}});
		std::vector<VisualScriptNode>& nodes = document.Graph.Nodes;
		std::vector<VisualScriptLink>& links = document.Graph.Links;
		nodes.push_back(MakeNode(10, "Event.FixedUpdate"));
		AddIncrement(document, 10, "Then", 1, 11);
		nodes.push_back(MakeNode(20, "Flow.Branch"));
		nodes.push_back(MakeNode(21, "Call:Strada.Input.IsActionPressed(string)", Json::object(), jump));
		links.push_back(MakeLink(11, "Then", 20, "Execute"));
		links.push_back(MakeLink(21, "Result", 20, "Condition"));
		AddIncrement(document, 20, "True", 2, 22);
		nodes.push_back(MakeNode(30, "Flow.DoOnce"));
		links.push_back(MakeLink(20, "False", 30, "Execute"));
		if (delay)
		{
			nodes.push_back(MakeNode(31, "Flow.Delay", Json::object(), Json::object({{"Duration", 0.0}})));
		}
		else
		{
			nodes.push_back(MakeNode(31, "Flow.WaitUntil"));
			nodes.push_back(MakeNode(36, "Call:Strada.Input.IsActionDown(string)", Json::object(), jump));
			links.push_back(MakeLink(36, "Result", 31, "Condition"));
		}
		links.push_back(MakeLink(30, "Completed", 31, "Execute"));
		nodes.push_back(MakeNode(32, "Variable.Set", Json::object({{"Variable", 3}})));
		nodes.push_back(MakeNode(33, "Call:Strada.Input.IsActionPressed(string)", Json::object(), jump));
		nodes.push_back(MakeNode(34, "Variable.Set", Json::object({{"Variable", 4}}), Json::object({{"Value", true}})));
		links.push_back(MakeLink(31, "Completed", 32, "Execute"));
		links.push_back(MakeLink(33, "Result", 32, "Value"));
		links.push_back(MakeLink(32, "Then", 34, "Execute"));
		return document;
	}

	// Event.Custom Step, raised from fixed-step code (a C# OnFixedUpdate's SendEvent, or with sendFromStep the graph's own
	// Event.FixedUpdate through Event.Send), starts once a Delay of 0 s whose continuation keeps what IsActionPressed(Jump)
	// reads (SentLateSaw, SentLateRan).
	VisualScriptDocument MakeSentLatentDocument(bool sendFromStep)
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "SentLateSaw", "bool", false));
		document.Variables.push_back(MakeVariable(2, "SentLateRan", "bool", false));
		std::vector<VisualScriptNode>& nodes = document.Graph.Nodes;
		std::vector<VisualScriptLink>& links = document.Graph.Links;
		nodes.push_back(MakeNode(10, "Event.Custom", Json::object({{"Name", "Step"}, {"Parameters", Json::array()}})));
		nodes.push_back(MakeNode(11, "Flow.DoOnce"));
		nodes.push_back(MakeNode(12, "Flow.Delay", Json::object(), Json::object({{"Duration", 0.0}})));
		nodes.push_back(MakeNode(13, "Variable.Set", Json::object({{"Variable", 1}})));
		nodes.push_back(MakeNode(14, "Call:Strada.Input.IsActionPressed(string)", Json::object(), Json::object({{"Action", "Jump"}})));
		nodes.push_back(MakeNode(15, "Variable.Set", Json::object({{"Variable", 2}}), Json::object({{"Value", true}})));
		links.push_back(MakeLink(10, "Then", 11, "Execute"));
		links.push_back(MakeLink(11, "Completed", 12, "Execute"));
		links.push_back(MakeLink(12, "Completed", 13, "Execute"));
		links.push_back(MakeLink(14, "Result", 13, "Value"));
		links.push_back(MakeLink(13, "Then", 15, "Execute"));
		if (sendFromStep)
		{
			nodes.push_back(MakeNode(30, "Event.FixedUpdate"));
			nodes.push_back(MakeNode(31, "Event.Send", Json::object({{"Name", "Step"}, {"Parameters", Json::array()}})));
			links.push_back(MakeLink(30, "Then", 31, "Execute"));
		}
		return document;
	}

	// A native function of the scripting API, by its binding name.
	template<typename TFunction>
	TFunction FindBinding(std::string_view name)
	{
		std::span<ScriptBinding const> const bindings = GetScriptBindings();
		auto const binding = std::find_if(bindings.begin(), bindings.end(),
		                                  [name](ScriptBinding const& candidate)
		                                  {
											  return std::string_view(candidate.Name, static_cast<size_t>(candidate.NameLength)) == name;
										  });
		REQUIRE(binding != bindings.end());
		return reinterpret_cast<TFunction>(binding->Function);
	}
}

TEST_CASE("SceneInputActions: OnUpdate reads an action's value, vector and level in the frame its key went down, and each edge once")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::ScriptEngineScope const scripting;
	InputScope const input;
	AssetHandle const actions = AddActions();
	Scene scene("Queries");
	Entity const jump = AddProbe(scene, "Jump");
	Entity const steer = AddProbe(scene, "Steer", {{"Action", ScriptFieldValue::FromString("Steer")}});
	Entity const move = AddProbe(scene, "Move", {{"Action", ScriptFieldValue::FromString("Move")}});
	scene.OnRuntimeStart(Settings(actions));
	RunFrame(scene);
	CHECK(ReadProbe(jump).at("Value").GetFloat() == 0.0f);

	// Set before the frame, read in that frame: no frame of latency.
	Input::SetKeyState(KeyCode::Space, true);
	Input::SetKeyState(KeyCode::D, true);
	Input::SetKeyState(KeyCode::W, true);
	RunFrame(scene);
	ScriptFieldMap read = ReadProbe(jump);
	CHECK(read.at("Value").GetFloat() == 1.0f);
	CHECK(read.at("Vector").GetVector2() == glm::vec2(1.0f, 0.0f));
	CHECK(read.at("Down").GetBool());
	CHECK(read.at("UpdatePresses").GetInt32() == 1);
	CHECK(ReadProbe(steer).at("Value").GetFloat() == 1.0f);
	ScriptFieldMap const moved = ReadProbe(move);
	CHECK(moved.at("Vector").GetVector2().x == doctest::Approx(std::sqrt(0.5f)));
	CHECK(moved.at("Vector").GetVector2().y == doctest::Approx(std::sqrt(0.5f)));
	CHECK(moved.at("Value").GetFloat() == doctest::Approx(1.0f));

	// Held: down, no new edge.
	RunFrame(scene);
	read = ReadProbe(jump);
	CHECK(read.at("Down").GetBool());
	CHECK(read.at("UpdatePresses").GetInt32() == 1);
	Input::SetKeyState(KeyCode::Space, false);
	Input::SetKeyState(KeyCode::A, true);
	RunFrame(scene);
	read = ReadProbe(jump);
	CHECK_FALSE(read.at("Down").GetBool());
	CHECK(read.at("UpdateReleases").GetInt32() == 1);
	CHECK(read.at("Value").GetFloat() == 0.0f);
	// A and D together cancel.
	CHECK(ReadProbe(steer).at("Value").GetFloat() == 0.0f);
	scene.OnRuntimeStop();
}

TEST_CASE("SceneInputActions: OnFixedUpdate sees each press and release in exactly one step at 30, 60 and 144 frames per second")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::ScriptEngineScope const scripting;
	InputScope const input;
	AssetHandle const actions = AddActions();

	for (float const framesPerSecond : {30.0f, 60.0f, 144.0f})
	{
		CAPTURE(framesPerSecond);
		Input::Reset();
		Scene scene("Rates");
		Entity const probe = AddProbe(scene, "Probe");
		scene.OnRuntimeStart(Settings(actions));
		bool down = false;
		uint32_t const frames = static_cast<uint32_t>(framesPerSecond) * 3;
		uint32_t const period = std::max(2u, static_cast<uint32_t>(framesPerSecond * 0.05f));
		for (uint32_t frame = 0; frame < frames; frame++)
		{
			if (frame % period == 1)
			{
				down = !down;
				Input::SetKeyState(KeyCode::Space, down);
			}
			RunFrame(scene, 1.0f / framesPerSecond);
		}
		// Frames without input until steps took every edge.
		Input::SetKeyState(KeyCode::Space, false);
		for (uint32_t frame = 0; frame < 8; frame++)
		{
			RunFrame(scene, 1.0f / framesPerSecond);
		}
		ScriptFieldMap const read = ReadProbe(probe);
		int32_t const presses = read.at("UpdatePresses").GetInt32();
		CHECK(presses > 10);
		CHECK(read.at("StepPresses").GetInt32() == presses);
		CHECK(read.at("StepReleases").GetInt32() == read.at("UpdateReleases").GetInt32());
		CHECK(read.at("StepTaps").GetInt32() == 0);
		// The callbacks saw the same edges.
		CHECK(read.at("Presses").GetInt32() == presses);
		CHECK(read.at("Releases").GetInt32() == read.at("UpdateReleases").GetInt32());
		scene.OnRuntimeStop();
	}
}

TEST_CASE("SceneInputActions: OnFixedUpdate of an eight-step frame sees a press in one step, and a tap in one step as both edges")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::ScriptEngineScope const scripting;
	InputScope const input;
	AssetHandle const actions = AddActions();
	Scene scene("Steps");
	Entity const probe = AddProbe(scene, "Probe");
	scene.OnRuntimeStart(Settings(actions));
	RunFrame(scene);
	Input::SetKeyState(KeyCode::Space, true);
	RunFrame(scene, 8.0f * Step);
	ScriptFieldMap read = ReadProbe(probe);
	CHECK(read.at("Steps").GetInt32() == 9);
	CHECK(read.at("StepPresses").GetInt32() == 1);
	Input::SetKeyState(KeyCode::Space, false);
	RunFrame(scene);
	Input::SetKeyState(KeyCode::Space, true);
	Input::SetKeyState(KeyCode::Space, false);
	RunFrame(scene);
	read = ReadProbe(probe);
	CHECK(read.at("StepPresses").GetInt32() == 2);
	CHECK(read.at("StepReleases").GetInt32() == 2);
	CHECK(read.at("StepTaps").GetInt32() == 1);
	CHECK(read.at("UpdatePresses").GetInt32() == 2);
	CHECK(read.at("UpdateReleases").GetInt32() == 2);
	scene.OnRuntimeStop();
}

TEST_CASE("SceneInputActions: OnActionPressed and Event.ActionPressed run once per edge, every C# script before every graph")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::ScriptEngineScope const scripting;
	InputScope const input;
	AssetHandle const actions = AddActions();
	AssetHandle const graph = AddGraph(MakeEventsDocument(), "Events");
	Scene scene("Events");
	// The graph's entity is made first: dispatch goes by kind, not by entity.
	Entity const graphEntity = AddGraphEntity(scene, "Graph", graph);
	Entity const probe = AddProbe(scene, "Probe");
	scene.OnRuntimeStart(Settings(actions));
	RunFrame(scene);

	// A tap within one frame: its press, then its release, each to the C# script first.
	uint64_t const tapStart = Log::GetNextEntryIndex();
	Input::SetKeyState(KeyCode::Space, true);
	Input::SetKeyState(KeyCode::Space, false);
	RunFrame(scene);
	std::vector<std::string> const order = FindLogged(
		tapStart, {"ActionProbe Probe: pressed Jump", "graph pressed Jump", "ActionProbe Probe: released Jump", "graph released Jump"});
	CHECK(order == std::vector<std::string>{"ActionProbe Probe: pressed Jump", "graph pressed Jump", "ActionProbe Probe: released Jump",
	                                        "graph released Jump"});
	CHECK(ReadProbe(probe).at("Events").GetString() == "+Jump,-Jump,");

	// A press held over frames is one edge; its release is another.
	Input::SetKeyState(KeyCode::Space, true);
	for (int frame = 0; frame < 5; frame++)
	{
		RunFrame(scene);
	}
	Input::SetKeyState(KeyCode::Space, false);
	RunFrame(scene);
	RunFrame(scene);
	ScriptFieldMap const variables = ReadGraph(graphEntity);
	CHECK(variables.at("Presses").GetInt32() == 2);
	CHECK(variables.at("Releases").GetInt32() == 2);
	ScriptFieldMap const read = ReadProbe(probe);
	CHECK(read.at("Presses").GetInt32() == 2);
	CHECK(read.at("Releases").GetInt32() == 2);
	// Each event names its own action, in the actions' order (D presses Steer and Move), and each graph node runs for its own.
	Input::SetKeyState(KeyCode::D, true);
	RunFrame(scene);
	CHECK(ReadProbe(probe).at("Events").GetString() == "+Jump,-Jump,+Jump,-Jump,+Steer,+Move,");
	CHECK(ReadGraph(graphEntity).at("SteerPresses").GetInt32() == 1);
	CHECK(ReadGraph(graphEntity).at("Presses").GetInt32() == 2);
	scene.OnRuntimeStop();
}

TEST_CASE("SceneInputActions: an override a script sets presses the action from the next frame, and clearing it releases it")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::ScriptEngineScope const scripting;
	InputScope const input;
	AssetHandle const actions = AddActions();
	Scene scene("Overrides");
	Entity const probe = AddProbe(scene, "Probe",
	                              {{"OverrideAction", ScriptFieldValue::FromString("Jump")},
	                               {"OverrideValue", ScriptFieldValue::FromFloat(0.8f)},
	                               {"OverrideFrame", ScriptFieldValue::FromInt32(2)}});
	scene.OnRuntimeStart(Settings(actions));
	RunFrame(scene);
	RunFrame(scene);
	// Set in frame 2's OnUpdate: frame 2 read nothing of it.
	CHECK(ReadProbe(probe).at("UpdatePresses").GetInt32() == 0);
	REQUIRE(Input::FindActionOverride("Jump") != nullptr);
	RunFrame(scene);
	ScriptFieldMap read = ReadProbe(probe);
	CHECK(read.at("UpdatePresses").GetInt32() == 1);
	CHECK(read.at("Presses").GetInt32() == 1);
	CHECK(read.at("Value").GetFloat() == doctest::Approx(0.8f));
	CHECK(read.at("Down").GetBool());
	// Cleared in frame 3's OnUpdate: released in frame 4.
	RunFrame(scene);
	read = ReadProbe(probe);
	CHECK(read.at("UpdateReleases").GetInt32() == 1);
	CHECK(read.at("Releases").GetInt32() == 1);
	CHECK(read.at("Value").GetFloat() == 0.0f);
	CHECK(read.at("StepPresses").GetInt32() == 1);
	CHECK(read.at("StepReleases").GetInt32() == 1);
	scene.OnRuntimeStop();
}

TEST_CASE(
	"SceneInputActions: code a fixed step started reads no edge when a latent node continues it, and code a frame started reads the frame's")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::ScriptEngineScope const scripting;
	InputScope const input;
	AssetHandle const actions = AddActions();
	AssetHandle const graph = AddGraph(MakeLateReadsDocument(), "Late Reads");
	Scene scene("Late");
	Entity const entity = AddGraphEntity(scene, "Graph", graph);
	scene.OnRuntimeStart(Settings(actions));
	// Frame 1: Event.Update and the step's Event.FixedUpdate (which sees no press) each start a Delay.
	RunFrame(scene);
	ScriptFieldMap variables = ReadGraph(entity);
	CHECK_FALSE(variables.at("FixedLateRan").GetBool());
	CHECK_FALSE(variables.at("UpdateLateRan").GetBool());
	// Frame 2 presses Jump; both Delays continue in its Update dispatch, before its step.
	Input::SetKeyState(KeyCode::Space, true);
	RunFrame(scene);
	variables = ReadGraph(entity);
	REQUIRE(variables.at("FixedLateRan").GetBool());
	REQUIRE(variables.at("UpdateLateRan").GetBool());
	CHECK_FALSE(variables.at("FixedLateSaw").GetBool());
	CHECK(variables.at("UpdateLateSaw").GetBool());
	// The step itself saw the press, once.
	RunFrame(scene);
	CHECK(ReadGraph(entity).at("StepPresses").GetInt32() == 1);
	scene.OnRuntimeStop();
}

TEST_CASE("SceneInputActions: an unknown action reads zero and is logged once, and a project without actions says it names none")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::ScriptEngineScope const scripting;
	InputScope const input;
	AssetHandle const actions = AddActions();
	AssetHandle const graph = AddGraph(MakeEventsDocument(), "Events");

	SUBCASE("an unknown name")
	{
		Scene scene("Unknown");
		Entity const probe = AddProbe(scene, "Probe", {{"Action", ScriptFieldValue::FromString("jump")}});
		uint64_t const logStart = Log::GetNextEntryIndex();
		scene.OnRuntimeStart(Settings(actions));
		Input::SetKeyState(KeyCode::Space, true);
		for (int frame = 0; frame < 5; frame++)
		{
			RunFrame(scene);
		}
		ScriptFieldMap const read = ReadProbe(probe);
		CHECK(read.at("Value").GetFloat() == 0.0f);
		CHECK_FALSE(read.at("Down").GetBool());
		CHECK(read.at("UpdatePresses").GetInt32() == 0);
		CHECK(read.at("StepPresses").GetInt32() == 0);
		// Names match with their letter case; the callback still heard Jump.
		CHECK(read.at("Events").GetString() == "+Jump,");
		CHECK(CountLogged(logStart, "no action is named 'jump'") == 1);
		scene.OnRuntimeStop();
	}
	SUBCASE("a project without actions")
	{
		Scene scene("None");
		Entity const probe = AddProbe(scene, "Probe");
		Entity const graphEntity = AddGraphEntity(scene, "Graph", graph);
		uint64_t const logStart = Log::GetNextEntryIndex();
		scene.OnRuntimeStart(Settings(AssetHandle()));
		Input::SetKeyState(KeyCode::Space, true);
		Input::SetActionOverride("Jump", glm::vec2(1.0f, 0.0f));
		for (int frame = 0; frame < 5; frame++)
		{
			RunFrame(scene);
		}
		ScriptFieldMap const read = ReadProbe(probe);
		CHECK(read.at("Value").GetFloat() == 0.0f);
		CHECK(read.at("UpdatePresses").GetInt32() == 0);
		CHECK(read.at("Events").GetString().empty());
		CHECK(ReadGraph(graphEntity).at("Presses").GetInt32() == 0);
		CHECK(CountLogged(logStart, "no input actions are loaded") == 1);
		scene.OnRuntimeStop();
	}
}

TEST_CASE("SceneInputActions: a graph that a fixed step's C# script or graph raises an event on reads that step's edges, once per "
          "edge, in frames of eight steps and of none")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::ScriptEngineScope const scripting;
	InputScope const input;
	AssetHandle const actions = AddActions();
	AssetHandle const receiver = AddGraph(MakeStepReceiverDocument(false), "Receiver");
	AssetHandle const sender = AddGraph(MakeStepReceiverDocument(true), "Sender");
	Scene scene("Sent");
	// The C# script's OnFixedUpdate raises Step on its entity's graph; the other graph raises it on itself.
	Entity probe = AddProbe(scene, "Probe", {{"SendEvent", ScriptFieldValue::FromString("Step")}});
	probe.AddComponent<VisualScriptComponent>().Graph = receiver;
	Entity const graph = AddGraphEntity(scene, "Sender", sender);
	scene.OnRuntimeStart(Settings(actions));
	RunFrame(scene);
	CHECK(ReadGraph(probe).at("SentSteps").GetInt32() == 1);
	CHECK(ReadGraph(graph).at("SentSteps").GetInt32() == 1);

	// A frame of eight steps: the press reaches the first alone.
	Input::SetKeyState(KeyCode::Space, true);
	RunFrame(scene, 8.0f * Step);
	CHECK(ReadGraph(probe).at("SentSteps").GetInt32() == 9);
	for (Entity const entity : {probe, graph})
	{
		CHECK(ReadGraph(entity).at("SentPresses").GetInt32() == 1);
	}
	Input::SetKeyState(KeyCode::Space, false);
	RunFrame(scene);
	// Frames that run no step carry the press to the next step, which sees it once.
	Input::SetKeyState(KeyCode::Space, true);
	for (int frame = 0; frame < 3; frame++)
	{
		RunFrame(scene, 0.001f);
	}
	CHECK(ReadGraph(probe).at("SentSteps").GetInt32() == 10);
	for (int frame = 0; frame < 4; frame++)
	{
		RunFrame(scene);
	}
	for (Entity const entity : {probe, graph})
	{
		CHECK(ReadGraph(entity).at("SentPresses").GetInt32() == 2);
		CHECK(ReadGraph(entity).at("SentSteps").GetInt32() == 14);
	}
	CHECK(ReadProbe(probe).at("StepPresses").GetInt32() == 2);
	scene.OnRuntimeStop();
}

TEST_CASE("SceneInputActions: a Delay's and a WaitUntil's continuation of fixed-step code reads no edge at 30, 60 and 144 frames "
          "per second")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::ScriptEngineScope const scripting;
	InputScope const input;
	AssetHandle const actions = AddActions();
	AssetHandle const delayed = AddGraph(MakeStepLatentDocument(true), "Delayed");
	AssetHandle const waiting = AddGraph(MakeStepLatentDocument(false), "Waiting");
	AssetHandle const sentByScript = AddGraph(MakeSentLatentDocument(false), "Sent By Script");
	AssetHandle const sentByGraph = AddGraph(MakeSentLatentDocument(true), "Sent By Graph");
	for (float const framesPerSecond : {30.0f, 60.0f, 144.0f})
	{
		CAPTURE(framesPerSecond);
		Input::Reset();
		Scene scene("Latent");
		Entity const delay = AddGraphEntity(scene, "Delay", delayed);
		Entity const wait = AddGraphEntity(scene, "Wait", waiting);
		// Graphs that code of a step raised an event on: a C# OnFixedUpdate's, and a graph's Event.FixedUpdate.
		Entity probe = AddProbe(scene, "Probe", {{"SendEvent", ScriptFieldValue::FromString("Step")}});
		probe.AddComponent<VisualScriptComponent>().Graph = sentByScript;
		Entity const sender = AddGraphEntity(scene, "Sender", sentByGraph);
		scene.OnRuntimeStart(Settings(actions));
		float const frameTime = 1.0f / framesPerSecond;
		// Until a step ran: it started the Delays and the WaitUntil, without a press.
		int frames = 0;
		while (ReadGraph(delay).at("Steps").GetInt32() == 0 && frames < 8)
		{
			RunFrame(scene, frameTime);
			frames++;
		}
		REQUIRE(ReadGraph(delay).at("Steps").GetInt32() > 0);
		CHECK_FALSE(ReadGraph(delay).at("DelayRan").GetBool());
		CHECK_FALSE(ReadGraph(wait).at("WaitRan").GetBool());
		CHECK_FALSE(ReadGraph(probe).at("SentLateRan").GetBool());
		// The next frame presses Jump: every continuation runs in its Update dispatch, and reads no edge of it.
		Input::SetKeyState(KeyCode::Space, true);
		RunFrame(scene, frameTime);
		ScriptFieldMap const delayRead = ReadGraph(delay);
		ScriptFieldMap const waitRead = ReadGraph(wait);
		REQUIRE(delayRead.at("DelayRan").GetBool());
		REQUIRE(waitRead.at("WaitRan").GetBool());
		CHECK_FALSE(delayRead.at("DelaySaw").GetBool());
		CHECK_FALSE(waitRead.at("WaitSaw").GetBool());
		for (Entity const entity : {probe, sender})
		{
			ScriptFieldMap const sentRead = ReadGraph(entity);
			REQUIRE(sentRead.at("SentLateRan").GetBool());
			CHECK_FALSE(sentRead.at("SentLateSaw").GetBool());
		}
		// A step sees the press once, whichever frame it falls in.
		for (int frame = 0; frame < 8; frame++)
		{
			RunFrame(scene, frameTime);
		}
		CHECK(ReadGraph(delay).at("StepPresses").GetInt32() == 1);
		CHECK(ReadGraph(wait).at("StepPresses").GetInt32() == 1);
		scene.OnRuntimeStop();
	}
}

TEST_CASE("SceneInputActions: an entity that a latent continuation of fixed-step code instantiates reads the frame's edges in its "
          "Create events")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::ScriptEngineScope const scripting;
	InputScope const input;
	AssetHandle const actions = AddActions();
	// The prefab: a C# probe (OnCreate keeps CreatedPressed) and a graph whose Event.Create keeps CreatedSaw.
	VisualScriptDocument created;
	created.Variables.push_back(MakeVariable(1, "CreatedSaw", "bool", false));
	created.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
	created.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 1}})));
	created.Graph.Nodes.push_back(
		MakeNode(12, "Call:Strada.Input.IsActionPressed(string)", Json::object(), Json::object({{"Action", "Jump"}})));
	created.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
	created.Graph.Links.push_back(MakeLink(12, "Result", 11, "Value"));
	AssetHandle const createdGraph = AddGraph(created, "Created");
	Scene source("Source");
	Entity member = AddProbe(source, "Member");
	member.AddComponent<VisualScriptComponent>().Graph = createdGraph;
	Result<Ref<PrefabAsset>> prefabAsset = PrefabAsset::Create(PrefabSerializer::Serialize(source, member));
	REQUIRE(prefabAsset.IsOk());
	AssetHandle const prefab = AssetManager::AddMemoryAsset(prefabAsset.GetValue(), "Member");

	// The spawner: its first step starts a Delay of 0 s whose continuation instantiates the prefab, then keeps what
	// IsActionPressed(Jump) reads itself (SpawnerSaw).
	VisualScriptDocument spawner;
	spawner.Variables.push_back(MakeVariable(1, "SpawnerSaw", "bool", true));
	std::vector<VisualScriptNode>& nodes = spawner.Graph.Nodes;
	std::vector<VisualScriptLink>& links = spawner.Graph.Links;
	nodes.push_back(MakeNode(10, "Event.FixedUpdate"));
	nodes.push_back(MakeNode(11, "Flow.DoOnce"));
	nodes.push_back(MakeNode(12, "Flow.Delay", Json::object(), Json::object({{"Duration", 0.0}})));
	nodes.push_back(MakeNode(13, "Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)",
	                         Json::object(), Json::object({{"Prefab", prefab.ToString()}})));
	nodes.push_back(MakeNode(14, "Variable.Set", Json::object({{"Variable", 1}})));
	nodes.push_back(MakeNode(15, "Call:Strada.Input.IsActionPressed(string)", Json::object(), Json::object({{"Action", "Jump"}})));
	links.push_back(MakeLink(10, "Then", 11, "Execute"));
	links.push_back(MakeLink(11, "Completed", 12, "Execute"));
	links.push_back(MakeLink(12, "Completed", 13, "Execute"));
	links.push_back(MakeLink(13, "Then", 14, "Execute"));
	links.push_back(MakeLink(15, "Result", 14, "Value"));
	AssetHandle const spawnerGraph = AddGraph(spawner, "Spawner");

	Scene scene("Spawning");
	Entity const spawnerEntity = AddGraphEntity(scene, "Spawner", spawnerGraph);
	scene.OnRuntimeStart(Settings(actions));
	RunFrame(scene);
	CHECK_FALSE(scene.FindEntityByName("Member"));
	// The continuation runs in the frame that presses Jump: it reads no edge, and the entity it makes reads the frame's.
	Input::SetKeyState(KeyCode::Space, true);
	RunFrame(scene);
	Entity const spawned = scene.FindEntityByName("Member");
	REQUIRE(spawned);
	CHECK_FALSE(ReadGraph(spawnerEntity).at("SpawnerSaw").GetBool());
	CHECK(ReadGraph(spawned).at("CreatedSaw").GetBool());
	CHECK(ReadProbe(spawned).at("CreatedPressed").GetBool());
	scene.OnRuntimeStop();
}

TEST_CASE("SceneInputActions: a script's InputActions names its asset's actions, and none (logged) when the asset does not load")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::ScriptEngineScope const scripting;
	InputScope const input;
	AssetHandle const actions = AddActions();
	AssetHandle const mesh = GetBuiltInHandle(BuiltInAsset::CubeMesh);
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Names");
	Entity const named = AddProbe(scene, "Named", {{"Actions", ScriptFieldValue::FromAsset(actions)}});
	Entity const unloaded = AddProbe(scene, "Unloaded", {{"Actions", ScriptFieldValue::FromAsset(mesh)}});
	scene.OnRuntimeStart(Settings(actions));
	RunFrame(scene);
	CHECK(ReadProbe(named).at("ActionNameCount").GetInt32() == 3);
	CHECK(ReadProbe(unloaded).at("ActionNameCount").GetInt32() == 0);
	CHECK(CountLogged(logStart, "InputActions: ") == 1);
	scene.OnRuntimeStop();

	// The bindings themselves: a handle that names no InputActions asset reads as failed, with no name.
	using ReadFunction = int32_t (*)(uint64_t, uint64_t, uint64_t*, int32_t*);
	using NameFunction = char const* (*)(uint64_t, int32_t, int32_t*);
	auto const read = FindBinding<ReadFunction>("InputActions_Read");
	auto const name = FindBinding<NameFunction>("InputActions_GetActionName");
	uint64_t version = 7;
	int32_t count = 9;
	uint64_t const meshID = mesh.GetUUID().GetValue();
	uint64_t const actionsID = actions.GetUUID().GetValue();
	CHECK(read(meshID, 0, &version, &count) == 0);
	CHECK(version == 7);
	CHECK(count == 9);
	int32_t length = 5;
	CHECK(name(meshID, 0, &length) == nullptr);
	CHECK(length == 0);
	// Read, then unchanged at the version held; a name past the count is none.
	REQUIRE(read(actionsID, 0, &version, &count) == 2);
	CHECK(count == 3);
	CHECK(read(actionsID, version, &version, &count) == 1);
	char const* const first = name(actionsID, 0, &length);
	REQUIRE(first != nullptr);
	CHECK(std::string_view(first, static_cast<size_t>(length)) == "Jump");
	CHECK(name(actionsID, 3, &length) == nullptr);
	CHECK(name(actionsID, -1, &length) == nullptr);
}
