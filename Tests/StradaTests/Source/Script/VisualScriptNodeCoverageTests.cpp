#include "Audio/AudioTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "Script/VisualScriptTestGraphs.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace Strada;

// Every node of the node library analyzes and runs in a generated graph (spec-slice3 section 13.1): the built-in kinds, every
// member of the scripting API and the game's [VisualNode] members (TestScripts' Mover). A new node is covered without a
// change when its defaults run it; otherwise this test fails naming it, and the change that adds it adds a recipe or a
// checked exclusion below.
namespace
{
	// What a node needs beyond what the generator gives it.
	struct NodeCaseRecipe
	{
		// The exact node type.
		std::string_view Type;
		// JSON object text merged over the generated settings.
		std::string_view Settings;
		// JSON object text: literals by pin, given instead of what the generator links.
		std::string_view Inputs;
		// JSON object text: components the case's entity has from the start, by registry name, with their fields as scenes
		// store them ({"RigidBody": {"Type": "Kinematic"}, "BoxCollider": {}}); added to those of MemberTypeRecipes.
		std::string_view Components;
		// The node's Target is the case's child entity rather than Self: members that destroy or replace what they act on.
		bool TargetsChild = false;
		// Keys, mouse buttons and buttons of gamepad 0 pressed and released before the first update, so that each reports
		// both while the case runs (input events). The InputScope around every scene restores the input state.
		std::span<KeyCode const> HeldKeys;
		std::span<MouseButton const> HeldMouseButtons;
		std::span<GamepadButton const> HeldGamepadButtons;
		// The exec output the Ran chain starts from; empty: the pass-through exit.
		std::string_view RanFrom;
		int Frames = 30;
		// Run in a scene of its own (beside GlobalEffectTypes and static setters).
		bool Alone = false;
		// Empty: Ran must be true; else a substring of the aborting error the node must stop with.
		std::string_view ExpectedError;
		// A substring of a Warning or Error the case may log; empty for none.
		std::string_view ExpectedLog;

		// The recipe with one field changed: tables name only what a node needs (designated initializers that leave fields out
		// are refused by -Wmissing-designated-field-initializers).
		template<typename T>
		constexpr NodeCaseRecipe With(T NodeCaseRecipe::* field, T value) const
		{
			NodeCaseRecipe recipe = *this;
			recipe.*field = value;
			return recipe;
		}
		constexpr NodeCaseRecipe WithSettings(std::string_view value) const { return With(&NodeCaseRecipe::Settings, value); }
		constexpr NodeCaseRecipe WithInputs(std::string_view value) const { return With(&NodeCaseRecipe::Inputs, value); }
		constexpr NodeCaseRecipe WithComponents(std::string_view value) const { return With(&NodeCaseRecipe::Components, value); }
		constexpr NodeCaseRecipe WithTargetsChild() const { return With(&NodeCaseRecipe::TargetsChild, true); }
		constexpr NodeCaseRecipe WithHeldKeys(std::span<KeyCode const> value) const { return With(&NodeCaseRecipe::HeldKeys, value); }
		constexpr NodeCaseRecipe WithHeldMouseButtons(std::span<MouseButton const> value) const
		{
			return With(&NodeCaseRecipe::HeldMouseButtons, value);
		}
		constexpr NodeCaseRecipe WithHeldGamepadButtons(std::span<GamepadButton const> value) const
		{
			return With(&NodeCaseRecipe::HeldGamepadButtons, value);
		}
		constexpr NodeCaseRecipe WithRanFrom(std::string_view value) const { return With(&NodeCaseRecipe::RanFrom, value); }
		constexpr NodeCaseRecipe WithFrames(int value) const { return With(&NodeCaseRecipe::Frames, value); }
		constexpr NodeCaseRecipe WithAlone() const { return With(&NodeCaseRecipe::Alone, true); }
		constexpr NodeCaseRecipe WithExpectedError(std::string_view value) const { return With(&NodeCaseRecipe::ExpectedError, value); }
		constexpr NodeCaseRecipe WithExpectedLog(std::string_view value) const { return With(&NodeCaseRecipe::ExpectedLog, value); }
	};

	// The recipe of a node with nothing changed yet.
	constexpr NodeCaseRecipe Recipe(std::string_view type)
	{
		NodeCaseRecipe recipe;
		recipe.Type = type;
		return recipe;
	}

	enum class NodeCaseRunner : uint8_t
	{
		// A graph attached in a FeatureTest scene, or a library such a graph calls: StradaRuntimeTests runs it (--test).
		FeatureTestRun = 0,
		// One of the shared fixture documents VisualScriptEngineTests.cpp runs on entities (Testing::SharedVisualScriptFixtures).
		SharedFixtureRun
	};

	// A node a generated case cannot run, and the checked-in graph that runs it instead.
	struct NodeCaseExclusion
	{
		std::string_view Type;
		// Why a generated case cannot run it.
		std::string_view Reason;
		// The graph that holds the node: a path of Projects/FeatureTest/Assets for FeatureTestRun, a file name of
		// Tests/Data/VisualScripting/Documents for SharedFixtureRun.
		std::string_view CoveredBy;
		NodeCaseRunner RunBy = NodeCaseRunner::FeatureTestRun;
	};

	constexpr std::array<KeyCode, 1> SpaceKey = {KeyCode::Space};
	constexpr std::array<MouseButton, 1> LeftMouseButton = {MouseButton::Left};
	constexpr std::array<GamepadButton, 1> GamepadButtonA = {GamepadButton::A};

	constexpr auto Recipes = std::to_array<NodeCaseRecipe>({
		// Flow whose defaults never reach the exit the Ran chain starts from, or wait longer than a case runs.
		Recipe("Flow.Branch").WithInputs(R"({"Condition": true})"),
		Recipe("Flow.WaitUntil").WithInputs(R"({"Condition": true})"),
		Recipe("Flow.Interval").WithInputs(R"({"Interval": 0.05, "Count": 1})"),
		Recipe("Flow.Tween").WithInputs(R"({"Duration": 0.1})"),
		Recipe("Flow.Timeline")
			.WithSettings(R"({"Timeline": {"Length": 0.1, "Loop": false, "Tracks": [{"Name": "Value", "Type": "float", "Keys": [
			{"Time": 0.0, "Value": 0.0}, {"Time": 0.1, "Value": 1.0}]}]}})")
			.WithRanFrom("Finished"),
		// Input events, with the input they wait for.
		Recipe("Event.KeyPressed").WithSettings(R"({"Key": "Space"})").WithHeldKeys(SpaceKey),
		Recipe("Event.KeyReleased").WithSettings(R"({"Key": "Space"})").WithHeldKeys(SpaceKey),
		Recipe("Event.MouseButtonPressed").WithSettings(R"({"Button": "Left"})").WithHeldMouseButtons(LeftMouseButton),
		Recipe("Event.MouseButtonReleased").WithSettings(R"({"Button": "Left"})").WithHeldMouseButtons(LeftMouseButton),
		Recipe("Event.GamepadButtonPressed").WithSettings(R"({"Button": "A"})").WithHeldGamepadButtons(GamepadButtonA),
		Recipe("Event.GamepadButtonReleased").WithSettings(R"({"Button": "A"})").WithHeldGamepadButtons(GamepadButtonA),
		// Neutral values for members with global effects, which run alone: the defaults would stop the case's own scene.
		Recipe("Set:Strada.Time.TimeScale").WithInputs(R"({"Value": 1.0})"),
		Recipe("Set:Strada.Physics.Gravity").WithInputs(R"({"Value": [0.0, -9.81, 0.0]})"),
		// Members that end the test run or replace the scene ask the application, which the test's host records. Each runs
		// alone (SceneManager's and Application's by their types), so a batch that runs again case by case to attribute a
		// message never asks the host twice.
		Recipe("Call:Strada.SceneManager.LoadScene(string)").WithInputs(R"({"Path": "Scenes/Second.sscene"})"),
		Recipe("Call:Strada.Testing.TestReporter.Finish()").WithAlone(),
		// Members that log by design, naming no entity: each runs alone, so what it logs is its own.
		Recipe("Call:Strada.Log.Warn(string)")
			.WithInputs(R"({"Message": "Coverage warning"})")
			.WithAlone()
			.WithExpectedLog("Coverage warning"),
		Recipe("Call:Strada.Log.Error(string)")
			.WithInputs(R"({"Message": "Coverage error"})")
			.WithAlone()
			.WithExpectedLog("Coverage error"),
		Recipe("Call:Strada.Testing.TestReporter.Fail(string,string)")
			.WithInputs(R"({"Name": "Coverage", "Message": "reported"})")
			.WithAlone()
			.WithExpectedLog("Test failed: Coverage: reported"),
		// Assertions with inputs that pass, and the one that always fails.
		Recipe("Call:Strada.Testing.Assert.IsTrue(bool,string)").WithInputs(R"({"Condition": true})"),
		Recipe("Call:Strada.Testing.Assert.AreNotEqual<T>(T,T,string)").WithInputs(R"({"NotExpected": 2.0})"),
		Recipe("Call:Strada.Testing.Assert.Fail(string)").WithInputs(R"({"Message": "always"})").WithExpectedError("always"),
		// Objects are the entity, which is no null.
		Recipe("Call:Strada.Testing.Assert.IsNull(System.Object,string)").WithExpectedError("expected null, got Entity"),
		// Members that need an asset of the project the test opens (Projects/FeatureTest's), or a body of a kind.
		Recipe("Call:Strada.Assets.Load<T>(string)")
			.WithSettings(R"({"TypeArguments": ["Strada.AudioClip"]})")
			.WithInputs(R"({"Path": "Audio/Beep.wav"})"),
		Recipe("Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)")
			.WithInputs(R"({"Prefab": "17000004240335447843"})"),
		Recipe("Call:Strada.RigidBodyComponent.MoveKinematic(Strada.Vector3,Strada.Quaternion)")
			.WithComponents(R"({"RigidBody": {"Type": "Kinematic"}, "BoxCollider": {}})"),
		// Members that take their target away, or would make it its own parent: they act on the case's child.
		Recipe("Call:Strada.Entity.Destroy()").WithTargetsChild(),
		Recipe("Set:Strada.VisualScriptComponent.Graph").WithTargetsChild(),
		Recipe("Set:Strada.Entity.Parent").WithTargetsChild(),
		// Values a joint and a mesh collider refuse to be without; a joint cannot join its own body, which it says.
		Recipe("Set:Strada.JointComponent.Axis").WithInputs(R"({"Value": [0.0, 0.0, 1.0]})"),
		Recipe("Set:Strada.JointComponent.ConnectedEntity").WithExpectedLog("cannot be joined to itself"),
		Recipe("Set:Strada.MeshColliderComponent.Mesh").WithInputs(R"({"Value": "1"})"),
	});

	// What every member of a type needs beyond its own component: components added before a recipe's own.
	struct MemberTypeRecipe
	{
		// The declaring type.
		std::string_view Type;
		// As NodeCaseRecipe::Components.
		std::string_view Components;
	};

	constexpr auto MemberTypeRecipes = std::to_array<MemberTypeRecipe>({
		// A body is simulated with a collider; a joint joins the body of its entity; a mesh collider needs a mesh (the
		// built-in cube).
		{"Strada.RigidBodyComponent", R"({"RigidBody": {}, "BoxCollider": {}})"},
		{"Strada.JointComponent", R"({"RigidBody": {}, "BoxCollider": {}, "Joint": {}})"},
		{"Strada.MeshColliderComponent", R"({"RigidBody": {"Type": "Static"}, "MeshCollider": {"Mesh": "1", "Convex": true}})"},
	});

	constexpr auto Exclusions = std::to_array<NodeCaseExclusion>({
		{"Event.CollisionEnter", "two bodies of a simulated scene that touch start it", "Scripts/Contacts.sgraph",
	     NodeCaseRunner::FeatureTestRun},
		{"Event.CollisionExit", "two bodies of a simulated scene that part start it", "Scripts/Contacts.sgraph",
	     NodeCaseRunner::FeatureTestRun},
		{"Event.TriggerEnter", "a body entering a trigger starts it", "Scripts/Contacts.sgraph", NodeCaseRunner::FeatureTestRun},
		{"Event.TriggerExit", "a body leaving a trigger starts it", "Scripts/Contacts.sgraph", NodeCaseRunner::FeatureTestRun},
		{"Event.Click", "a click on a UI button starts it", "Scripts/Clicks.sgraph", NodeCaseRunner::FeatureTestRun},
		{"Event.Destroy", "it runs when the scene stops, after Ran is read", "Scripts/Features.sgraph", NodeCaseRunner::FeatureTestRun},
	});

	// Types whose members change what every case of a scene sees (time, gravity, the application, the scene): their cases
	// run alone, as static setters do (Input's among them).
	constexpr std::array<std::string_view, 4> GlobalEffectTypes = {"Strada.Application", "Strada.Physics", "Strada.SceneManager",
	                                                               "Strada.Time"};

	constexpr float FrameTime = 1.0f / 60.0f;
	constexpr size_t BatchSize = 200;

	// IDs of the generated documents: variables, the function and its parameter, then nodes.
	constexpr uint32_t RanVariable = 1;
	constexpr uint32_t ValueVariable = 2;
	constexpr uint32_t SinkVariable = 3;
	constexpr uint32_t CaseFunction = 4;
	constexpr uint32_t FirstNode = 100;

	// Records what scripts ask of the application for as long as it lives.
	struct RecordingHost final : ScriptHost
	{
		RecordingHost() { ScriptEngine::SetHost(this); }
		~RecordingHost() override { ScriptEngine::SetHost(nullptr); }

		RecordingHost(RecordingHost const&) = delete;
		RecordingHost& operator=(RecordingHost const&) = delete;

		bool IsEditor() const override { return false; }
		void RequestSceneLoad(AssetHandle) override { SceneLoads++; }
		void RequestQuit() override { Quits++; }
		void ReportTestResult(ScriptTestResult const&) override {}
		void FinishTests() override { Finishes++; }
		void OnScriptException() override {}

		uint32_t SceneLoads = 0;
		uint32_t Quits = 0;
		uint32_t Finishes = 0;
	};

	// Input is global: every scene of cases starts and ends with none.
	class InputScope
	{
	public:
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }

		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	template<typename T>
	T const* FindByType(std::span<T const> entries, std::string_view type)
	{
		auto const found = std::find_if(entries.begin(), entries.end(),
		                                [type](T const& entry)
		                                {
											return entry.Type == type;
										});
		return found != entries.end() ? &*found : nullptr;
	}

	// The type that declares a member's node ("Call:Strada.Entity.AddComponent<T>()" → "Strada.Entity"); empty for the
	// built-in kinds.
	std::string GetDeclaringType(std::string_view type)
	{
		size_t const colon = type.find(':');
		if (colon == std::string_view::npos)
		{
			return {};
		}
		std::string_view member = type.substr(colon + 1);
		if (type.starts_with("New:"))
		{
			return std::string(member.substr(0, member.find('(')));
		}
		member = member.substr(0, std::min(member.find('('), member.find('<')));
		size_t const dot = member.rfind('.');
		return dot != std::string_view::npos ? std::string(member.substr(0, dot)) : std::string();
	}

	// Whether a Type or TypeList setting of that constraint takes the type (the runtime's TypeConstraint).
	bool SatisfiesTypeConstraint(VisualScriptTypeInfo const& type, std::string_view constraint)
	{
		if (type.Kind == VisualScriptTypeKind::Exec)
		{
			return false;
		}
		if (constraint == "Literal")
		{
			return type.HasLiteral;
		}
		if (constraint == "Exposable")
		{
			return type.FieldType != ScriptFieldType::None && type.Kind != VisualScriptTypeKind::Array;
		}
		if (constraint == "Struct")
		{
			return type.Kind == VisualScriptTypeKind::Struct;
		}
		if (constraint == "Makeable")
		{
			return type.Makeable;
		}
		if (constraint == "Breakable")
		{
			return type.Breakable;
		}
		if (constraint == "Enum")
		{
			return type.Kind == VisualScriptTypeKind::Enum;
		}
		if (constraint == "Switch")
		{
			return type.ID == "int" || type.ID == "string" || type.Kind == VisualScriptTypeKind::Enum;
		}
		if (constraint == "Component")
		{
			return type.Kind == VisualScriptTypeKind::Component;
		}
		if (constraint == "Asset")
		{
			return type.Kind == VisualScriptTypeKind::Asset && type.AcceptedAssetType != AssetType::None;
		}
		if (constraint == "Script")
		{
			return type.Kind == VisualScriptTypeKind::Entity && type.ID != "Strada.Entity" && type.ID != "Strada.Script";
		}
		return true;
	}

	// The first library type the constraint takes, in the order float, int, Strada.Vector3, the first component, the first
	// asset class, the first script class, then any; empty when none does.
	std::string PickType(VisualScriptLibrary const& library, std::string_view constraint)
	{
		for (std::string_view const preferred : {"float", "int", "Strada.Vector3"})
		{
			if (VisualScriptTypeInfo const* const type = library.FindType(preferred);
			    type != nullptr && SatisfiesTypeConstraint(*type, constraint))
			{
				return type->ID;
			}
		}
		auto const first = [&](auto const& accept) -> std::string
		{
			for (VisualScriptTypeInfo const& type : library.GetTypes())
			{
				if (accept(type) && SatisfiesTypeConstraint(type, constraint))
				{
					return type.ID;
				}
			}
			return {};
		};
		for (std::string_view const kind : {"Component", "Asset", "Script"})
		{
			std::string const type = first(
				[kind](VisualScriptTypeInfo const& candidate)
				{
					return SatisfiesTypeConstraint(candidate, kind);
				});
			if (!type.empty())
			{
				return type;
			}
		}
		return first(
			[](VisualScriptTypeInfo const& candidate)
			{
				return candidate.Kind != VisualScriptTypeKind::Array;
			});
	}

	// A generated case: the document that runs one node of the library, and how it runs and is judged.
	struct NodeCase
	{
		VisualScriptNodeTemplate const* Node = nullptr;
		NodeCaseRecipe const* Recipe = nullptr;
		VisualScriptDocument Document;
		// The node under test.
		uint32_t NodeID = 0;
		// The script class the entity runs, for members of script classes.
		std::string ScriptClass;
		// The components the member's type needs (MemberTypeRecipes); empty for none.
		std::string_view MemberTypeComponents;
		bool Alone = false;
		int Frames = 30;
		// Why the case could not be made, analyzed or run; empty while it passes.
		std::string Failure;
	};

	// Builds the documents of cases (spec-slice3 section 13.1, step 1).
	class NodeCaseBuilder
	{
	public:
		NodeCaseBuilder(VisualScriptLibrary const& library, AssetHandle targetScript)
			: m_Library(library),
			  m_TargetScript(targetScript)
		{
		}

		NodeCase Build(VisualScriptNodeTemplate const& node, NodeCaseRecipe const* recipe)
		{
			NodeCase result;
			result.Node = &node;
			result.Recipe = recipe;
			result.Frames = recipe != nullptr ? recipe->Frames : 30;
			m_Document = {};
			m_NextNode = FirstNode;
			m_Prelude.clear();
			m_Failure.clear();

			std::string const declaringType = GetDeclaringType(node.Type);
			VisualScriptTypeInfo const* const declaring = declaringType.empty() ? nullptr : m_Library.FindType(declaringType);
			bool const isStatic = node.FindPin("Target", VisualScriptPinDirection::In) == nullptr;
			result.Alone = (recipe != nullptr && recipe->Alone) || (node.Type.starts_with("Set:") && isStatic) ||
			               std::find(GlobalEffectTypes.begin(), GlobalEffectTypes.end(), declaringType) != GlobalEffectTypes.end();
			if (declaring != nullptr && declaring->Kind == VisualScriptTypeKind::Entity && declaring->ID != "Strada.Entity")
			{
				result.ScriptClass = declaring->ID;
			}
			if (MemberTypeRecipe const* const memberType = FindByType<MemberTypeRecipe>(MemberTypeRecipes, declaringType))
			{
				result.MemberTypeComponents = memberType->Components;
			}

			m_Document.Variables.push_back(MakeVariable(RanVariable, "Ran", "bool", false, true));
			VisualScriptNode tested;
			tested.Type = node.Type;
			tested.Settings = MakeSettings(node, recipe);
			if (recipe != nullptr && !recipe->Inputs.empty())
			{
				tested.Inputs = ParseRecipe(recipe->Inputs, "Inputs");
			}

			// Function.Entry and Function.Return are those of the case's function, which a call in the event graph runs.
			bool const inFunction = node.Type == "Function.Entry" || node.Type == "Function.Return";
			uint32_t testedID = 0;
			if (inFunction)
			{
				uint32_t const function = AddFunction();
				testedID = node.Type == "Function.Entry" ? m_EntryID : m_ReturnID;
				tested = MakeNodeOf("Function.Call", Json::object({{"Function", function}}));
			}
			uint32_t const nodeID = AddNode(m_Document.Graph, std::move(tested));
			result.NodeID = inFunction ? testedID : nodeID;
			if (!m_Failure.empty())
			{
				result.Failure = m_Failure;
				return result;
			}

			// The pins under these settings, as the runtime shapes them.
			VisualScriptAnalyzedNode const* const shaped = Shape(nodeID);
			if (shaped == nullptr)
			{
				result.Failure = m_Failure;
				return result;
			}
			std::vector<VisualScriptPin> const pins = shaped->Pins;
			uint32_t const child = recipe != nullptr && recipe->TargetsChild ? AddChild() : 0;
			LinkInputs(nodeID, pins, child);

			// A Send or Broadcast raises the case's own custom event, and a custom event is raised by a Send.
			if (node.Type == "Event.Send" || node.Type == "Event.Broadcast")
			{
				AddNode(m_Document.Graph, MakeNodeOf("Event.Custom", Json::object({{"Name", "Case"}})));
			}

			uint32_t const ranSet = AddNode(
				m_Document.Graph, MakeNodeOf("Variable.Set", Json::object({{"Variable", RanVariable}}), Json::object({{"Value", true}})));
			bool const isEvent = node.Kind == VisualScriptNodeKind::Event;
			if (isEvent)
			{
				Link(nodeID, ExitOf(pins, recipe), ranSet, "Execute");
				if (node.Type == "Event.Custom")
				{
					uint32_t const create = AddNode(m_Document.Graph, MakeNodeOf("Event.Create"));
					uint32_t const send = AddNode(m_Document.Graph, MakeNodeOf("Event.Send", Json::object({{"Name", "Case"}})));
					Link(create, "Then", send, "Execute");
				}
			}
			else
			{
				uint32_t const create = AddNode(m_Document.Graph, MakeNodeOf("Event.Create"));
				// Impure producers of the node's inputs run first.
				uint32_t start = create;
				for (uint32_t const prelude : m_Prelude)
				{
					Link(start, "Then", prelude, "Execute");
					start = prelude;
				}
				std::string const entry = EntryOf(pins);
				uint32_t first = nodeID;
				std::string firstPin = entry;
				if (entry.empty())
				{
					// A pure node: its first data output is stored in a variable of its type, then Ran is set.
					auto const output =
						std::find_if(pins.begin(), pins.end(),
					                 [](VisualScriptPin const& pin)
					                 {
										 return pin.Direction == VisualScriptPinDirection::Out && pin.Type != VisualScriptExecType;
									 });
					if (output == pins.end())
					{
						result.Failure = "the pure node has no data output to read";
						return result;
					}
					uint32_t const sink = AddSink(nodeID, *output);
					if (sink == 0)
					{
						result.Failure = m_Failure;
						return result;
					}
					Link(sink, "Then", ranSet, "Execute");
					first = sink;
					firstPin = "Execute";
				}
				else
				{
					Link(nodeID, ExitOf(pins, recipe), ranSet, "Execute");
				}
				if (declaring != nullptr && declaring->Kind == VisualScriptTypeKind::Component)
				{
					// The entity gets the component first unless it has it (core components it always has).
					uint32_t const has =
						AddNode(m_Document.Graph, MakeNodeOf("Call:Strada.Entity.HasComponent<T>()",
					                                         Json::object({{"TypeArguments", Json::array({declaring->ID})}})));
					uint32_t const branch = AddNode(m_Document.Graph, MakeNodeOf("Flow.Branch"));
					uint32_t const add =
						AddNode(m_Document.Graph, MakeNodeOf("Call:Strada.Entity.AddComponent<T>()",
					                                         Json::object({{"TypeArguments", Json::array({declaring->ID})}})));
					if (child != 0)
					{
						Link(child, "Element", has, "Target");
						Link(child, "Element", add, "Target");
					}
					Link(start, "Then", branch, "Execute");
					Link(has, "Result", branch, "Condition");
					Link(branch, "True", first, firstPin);
					Link(branch, "False", add, "Execute");
					Link(add, "Then", first, firstPin);
				}
				else
				{
					Link(start, "Then", first, firstPin);
				}
			}
			result.Failure = m_Failure;
			result.Document = std::move(m_Document);
			return result;
		}

	private:
		static VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type, Json value, bool isPublic)
		{
			VisualScriptVariable variable;
			variable.ID = id;
			variable.Name = std::move(name);
			variable.Type = std::move(type);
			variable.Value = std::move(value);
			variable.Public = isPublic;
			return variable;
		}

		static VisualScriptNode MakeNodeOf(std::string type, Json settings = Json::object(), Json inputs = Json::object())
		{
			VisualScriptNode node;
			node.Type = std::move(type);
			node.Settings = std::move(settings);
			node.Inputs = std::move(inputs);
			return node;
		}

		Json ParseRecipe(std::string_view text, std::string_view what)
		{
			Result<Json> parsed = ParseJson(text);
			if (!parsed || !parsed.GetValue().is_object())
			{
				m_Failure = fmt::format("the recipe's {} are no JSON object", what);
				return Json::object();
			}
			return parsed.TakeValue();
		}

		uint32_t AddNode(VisualScriptGraph& graph, VisualScriptNode node)
		{
			node.ID = m_NextNode++;
			if (node.Settings.is_null())
			{
				node.Settings = Json::object();
			}
			if (node.Inputs.is_null())
			{
				node.Inputs = Json::object();
			}
			graph.Nodes.push_back(std::move(node));
			return graph.Nodes.back().ID;
		}

		void Link(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
		{
			m_Document.Graph.Links.push_back({{from, std::move(fromPin)}, {to, std::move(toPin)}});
		}

		// The required settings' representative values, with the recipe's merged over them.
		Json MakeSettings(VisualScriptNodeTemplate const& node, NodeCaseRecipe const* recipe)
		{
			Json settings = Json::object();
			for (VisualScriptSetting const& setting : node.Settings)
			{
				if (!setting.Required)
				{
					continue;
				}
				if (std::optional<Json> value = MakeSetting(setting))
				{
					settings[setting.Name] = std::move(*value);
				}
				else if (m_Failure.empty())
				{
					m_Failure = fmt::format("no value for its required setting {} of kind {}", setting.Name, setting.Kind);
				}
			}
			if (recipe != nullptr && !recipe->Settings.empty())
			{
				settings.update(ParseRecipe(recipe->Settings, "Settings"));
			}
			return settings;
		}

		std::optional<Json> MakeSetting(VisualScriptSetting const& setting)
		{
			std::string_view const kind = setting.Kind;
			if (kind == "Int")
			{
				return !setting.Default.is_null() ? setting.Default : Json(setting.Min.value_or(0));
			}
			if (kind == "String")
			{
				return Json("Case");
			}
			if (kind == "Bool")
			{
				return !setting.Default.is_null() ? setting.Default : Json(false);
			}
			if (kind == "Type" || kind == "TypeList")
			{
				std::string const type = PickType(m_Library, setting.Constraint);
				if (type.empty())
				{
					return std::nullopt;
				}
				return kind == "Type" ? Json(type) : Json::array({type});
			}
			if (kind == "Variable")
			{
				m_Document.Variables.push_back(MakeVariable(ValueVariable, "Value", "float", 0.5, false));
				return Json(ValueVariable);
			}
			if (kind == "Function")
			{
				return Json(AddFunction());
			}
			if (kind == "Parameters")
			{
				return Json::array({Json::object({{"Name", "X"}, {"Type", "float"}})});
			}
			if (kind == "Cases")
			{
				return Json::array({1});
			}
			if (kind == "Enum")
			{
				VisualScriptTypeInfo const* const type = m_Library.FindType(setting.Type);
				if (type == nullptr || type->Enumerators.empty())
				{
					return std::nullopt;
				}
				return Json(type->Enumerators.front().Name);
			}
			if (kind == "Choice")
			{
				return setting.Choices.empty() ? std::nullopt : std::optional<Json>(Json(setting.Choices.front()));
			}
			if (kind == "Expression")
			{
				return Json("a + 1");
			}
			if (kind == "Timeline")
			{
				return Json::object(
					{{"Length", 0.1},
				     {"Tracks", Json::array({Json::object({{"Name", "Value"},
				                                           {"Type", "float"},
				                                           {"Keys", Json::array({Json::object({{"Time", 0.0}, {"Value", 0.0}}),
				                                                                 Json::object({{"Time", 0.1}, {"Value", 1.0}})})}})})}});
			}
			if (kind == "Graph" && setting.Constraint == "Script")
			{
				return Json(m_TargetScript.ToString());
			}
			return std::nullopt;
		}

		// The case's function: impure, X: float → Y: float, its Entry linked to its Return.
		uint32_t AddFunction()
		{
			VisualScriptFunction function;
			function.ID = CaseFunction;
			function.Name = "Case";
			function.Inputs.emplace_back("X", "float");
			function.Outputs.emplace_back("Y", "float");
			VisualScriptNode entry = MakeNodeOf("Function.Entry");
			VisualScriptNode exit = MakeNodeOf("Function.Return");
			m_EntryID = AddNode(function.Graph, std::move(entry));
			m_ReturnID = AddNode(function.Graph, std::move(exit));
			function.Graph.Links.push_back({{m_EntryID, "Then"}, {m_ReturnID, "Execute"}});
			function.Graph.Links.push_back({{m_EntryID, "X"}, {m_ReturnID, "Y"}});
			m_Document.Functions.push_back(std::move(function));
			return CaseFunction;
		}

		// Analyzes the document as it is: the tested node's pins under its settings. Null (with the failure) when the
		// runtime cannot.
		VisualScriptAnalyzedNode const* Shape(uint32_t nodeID)
		{
			Result<VisualScriptAnalysis> analysis = VisualScriptEngine::Analyze(m_Document);
			if (!analysis)
			{
				m_Failure = "the runtime did not analyze it: " + analysis.GetError();
				return nullptr;
			}
			m_Shape = analysis.TakeValue();
			VisualScriptAnalyzedNode const* const node = m_Shape.FindNode(nodeID);
			if (node == nullptr)
			{
				m_Failure = "the analysis does not have it";
			}
			return node;
		}

		// A node that gives a value of the type; 0 when the type has a literal (or none of these), so the input takes its
		// type default.
		uint32_t MakeProducer(std::string_view type, VisualScriptPinConstraint constraint, bool arrayElements, std::string& pin)
		{
			if (type == VisualScriptWildcardType)
			{
				switch (constraint)
				{
					case VisualScriptPinConstraint::Reference:
						pin = "Self";
						return AddNode(m_Document.Graph, MakeNodeOf("Self"));
					case VisualScriptPinConstraint::Array:
						pin = "Value";
						return AddNode(m_Document.Graph, MakeNodeOf("Get:Strada.Entity.Children"));
					case VisualScriptPinConstraint::None:
					case VisualScriptPinConstraint::Any:
					case VisualScriptPinConstraint::Numeric:
					case VisualScriptPinConstraint::Expression:
						break;
				}
				// Beside an array of the entity's children, a value of their type.
				if (arrayElements && constraint == VisualScriptPinConstraint::Any)
				{
					pin = "Self";
					return AddNode(m_Document.Graph, MakeNodeOf("Self"));
				}
				pin = "Value";
				return AddNode(m_Document.Graph, MakeNodeOf("Literal", Json::object({{"Type", "float"}, {"Value", 1.0}})));
			}
			if (type == "Strada.Entity" || type == "System.Object")
			{
				pin = "Self";
				return AddNode(m_Document.Graph, MakeNodeOf("Self"));
			}
			VisualScriptTypeInfo const* const info = m_Library.FindType(type);
			if (info == nullptr || info->HasLiteral)
			{
				return 0;
			}
			if (info->Kind == VisualScriptTypeKind::Component)
			{
				pin = "Result";
				return AddNode(m_Document.Graph, MakeNodeOf("Call:Strada.Entity.GetComponent<T>()",
				                                            Json::object({{"TypeArguments", Json::array({info->ID})}})));
			}
			std::string const constructor = fmt::format("New:{}()", type);
			if (m_Library.FindNode(constructor) != nullptr)
			{
				pin = "Value";
				return AddNode(m_Document.Graph, MakeNodeOf(constructor));
			}
			return 0;
		}

		// The case's child entity: element 0 of Self's children.
		uint32_t AddChild()
		{
			uint32_t const children = AddNode(m_Document.Graph, MakeNodeOf("Get:Strada.Entity.Children"));
			uint32_t const child = AddNode(m_Document.Graph, MakeNodeOf("Array.Get", Json::object(), Json::object({{"Index", 0}})));
			Link(children, "Value", child, "Array");
			return child;
		}

		VisualScriptNode& FindNode(uint32_t id)
		{
			auto const node = std::find_if(m_Document.Graph.Nodes.begin(), m_Document.Graph.Nodes.end(),
			                               [id](VisualScriptNode const& candidate)
			                               {
											   return candidate.ID == id;
										   });
			REQUIRE(node != m_Document.Graph.Nodes.end());
			return *node;
		}

		// Links each data input the recipe gives no literal for and that has no default from a producer of its type. The
		// Target of a member is the child when given, and for a member of an asset class the first asset of that class the
		// asset manager has (a project's, the built-in ones first).
		void LinkInputs(uint32_t nodeID, std::vector<VisualScriptPin> const& pins, uint32_t child)
		{
			Json const literals = FindNode(nodeID).Inputs;
			bool const hasArray =
				std::any_of(pins.begin(), pins.end(),
			                [](VisualScriptPin const& pin)
			                {
								return pin.Direction == VisualScriptPinDirection::In && pin.Constraint == VisualScriptPinConstraint::Array;
							});
			for (VisualScriptPin const& pin : pins)
			{
				if (pin.Direction != VisualScriptPinDirection::In || pin.Type == VisualScriptExecType || literals.contains(pin.Name))
				{
					continue;
				}
				if (pin.Name == "Target" && child != 0)
				{
					Link(child, "Element", nodeID, pin.Name);
					continue;
				}
				if (pin.Name == "Target" && LinkAssetTarget(nodeID, pin.Type))
				{
					continue;
				}
				if (pin.DefaultsToSelf || !pin.Default.is_null())
				{
					continue;
				}
				std::string output;
				if (uint32_t const producer = MakeProducer(pin.Type, pin.Constraint, hasArray, output); producer != 0)
				{
					Link(producer, output, nodeID, pin.Name);
				}
			}
		}

		// Gives the Target of a member of an asset class an asset: a new one where the class creates them (members that change
		// an asset change only copies), else the first asset of the class the asset manager has (the built-in ones first),
		// through a literal of its concrete class for an abstract one. False for other types.
		bool LinkAssetTarget(uint32_t nodeID, std::string_view type)
		{
			VisualScriptTypeInfo const* const info = m_Library.FindType(type);
			if (info == nullptr || info->Kind != VisualScriptTypeKind::Asset)
			{
				return false;
			}
			std::string const create = fmt::format("Call:{}.Create()", type);
			if (m_Library.FindNode(create) != nullptr)
			{
				uint32_t const created = AddNode(m_Document.Graph, MakeNodeOf(create));
				m_Prelude.push_back(created);
				Link(created, "Result", nodeID, "Target");
				return true;
			}
			for (AssetMetadata const& asset : AssetManager::GetAssets(info->AcceptedAssetType))
			{
				auto const concrete = std::find_if(m_Library.GetTypes().begin(), m_Library.GetTypes().end(),
				                                   [&asset](VisualScriptTypeInfo const& candidate)
				                                   {
													   return candidate.Kind == VisualScriptTypeKind::Asset && candidate.HasLiteral &&
					                                          candidate.AcceptedAssetType == asset.Type;
												   });
				if (concrete == m_Library.GetTypes().end())
				{
					continue;
				}
				if (concrete->ID == type)
				{
					FindNode(nodeID).Inputs["Target"] = asset.Handle.ToString();
				}
				else
				{
					uint32_t const literal =
						AddNode(m_Document.Graph,
					            MakeNodeOf("Literal", Json::object({{"Type", concrete->ID}, {"Value", asset.Handle.ToString()}})));
					Link(literal, "Value", nodeID, "Target");
				}
				return true;
			}
			return false;
		}

		// Stores the output in a private variable of its type, which the analysis resolves when the pin is a wildcard; the
		// Variable.Set, or 0 with the failure.
		uint32_t AddSink(uint32_t nodeID, VisualScriptPin const& output)
		{
			std::string type = output.Type;
			if (type == VisualScriptWildcardType)
			{
				Result<VisualScriptAnalysis> analysis = VisualScriptEngine::Analyze(m_Document);
				VisualScriptAnalyzedNode const* const node = analysis ? analysis.GetValue().FindNode(nodeID) : nullptr;
				VisualScriptPin const* const resolved =
					node != nullptr ? node->FindPin(output.Name, VisualScriptPinDirection::Out) : nullptr;
				if (resolved == nullptr || resolved->Type == VisualScriptWildcardType)
				{
					m_Failure = fmt::format("its output {} keeps no type with the generated inputs{}", output.Name,
					                        analysis ? "; " + DescribeVisualScriptErrors(analysis.GetValue().Diagnostics) : std::string());
					return 0;
				}
				type = resolved->Type;
			}
			m_Document.Variables.push_back(MakeVariable(SinkVariable, "Sink", type, Json(), false));
			uint32_t const sink = AddNode(m_Document.Graph, MakeNodeOf("Variable.Set", Json::object({{"Variable", SinkVariable}})));
			Link(nodeID, output.Name, sink, "Value");
			return sink;
		}

		// The exec input flow enters by: Execute, else the first.
		static std::string EntryOf(std::vector<VisualScriptPin> const& pins)
		{
			std::string first;
			for (VisualScriptPin const& pin : pins)
			{
				if (pin.Direction == VisualScriptPinDirection::In && pin.Type == VisualScriptExecType)
				{
					if (pin.Name == "Execute")
					{
						return pin.Name;
					}
					if (first.empty())
					{
						first = pin.Name;
					}
				}
			}
			return first;
		}

		// The exit the Ran chain starts from: the recipe's, else Then, else Completed, else the first exec output that is
		// no Tick (s2 section 3.2 rule 1's order).
		static std::string ExitOf(std::vector<VisualScriptPin> const& pins, NodeCaseRecipe const* recipe)
		{
			if (recipe != nullptr && !recipe->RanFrom.empty())
			{
				return std::string(recipe->RanFrom);
			}
			auto const has = [&pins](std::string_view name)
			{
				return std::any_of(pins.begin(), pins.end(),
				                   [name](VisualScriptPin const& pin)
				                   {
									   return pin.Direction == VisualScriptPinDirection::Out && pin.Type == VisualScriptExecType &&
					                          pin.Name == name;
								   });
			};
			for (std::string_view const name : {"Then", "Completed"})
			{
				if (has(name))
				{
					return std::string(name);
				}
			}
			for (VisualScriptPin const& pin : pins)
			{
				if (pin.Direction == VisualScriptPinDirection::Out && pin.Type == VisualScriptExecType &&
				    pin.Role != VisualScriptPinRole::Tick)
				{
					return pin.Name;
				}
			}
			return "Then";
		}

		VisualScriptLibrary const& m_Library;
		AssetHandle m_TargetScript;
		VisualScriptDocument m_Document;
		VisualScriptAnalysis m_Shape;
		uint32_t m_NextNode = FirstNode;
		// The impure producers that run before the node, in order.
		std::vector<uint32_t> m_Prelude;
		uint32_t m_EntryID = 0;
		uint32_t m_ReturnID = 0;
		std::string m_Failure;
	};

	// A case in a scene: its graph asset and entity, and the token its names carry.
	struct RunningCase
	{
		NodeCase* Case = nullptr;
		std::string Token;
		AssetHandle Graph;
		UUID Entity = UUID::Invalid();
	};

	// Runs cases in scenes (spec-slice3 section 13.1, steps 3 and 4) and judges them.
	class NodeCaseScenes
	{
	public:
		// Runs the cases together; a message that names none of them makes each run alone, to attribute it.
		void RunBatch(std::span<NodeCase*> cases)
		{
			std::vector<std::string> unattributed = Run(cases);
			if (unattributed.empty())
			{
				return;
			}
			if (cases.size() == 1)
			{
				// Alone, what it logged is its own.
				return;
			}
			m_Reruns++;
			if (m_FirstUnattributed.empty())
			{
				m_FirstUnattributed = unattributed.front();
			}
			for (NodeCase* nodeCase : cases)
			{
				nodeCase->Failure.clear();
				std::array<NodeCase*, 1> alone = {nodeCase};
				Run(alone);
			}
		}

		// How many batches ran again one case at a time, and the first message that made one.
		size_t GetReruns() const { return m_Reruns; }
		std::string const& GetFirstUnattributed() const { return m_FirstUnattributed; }

	private:
		// Runs the cases in one scene; the Warning and Error messages that named none of them (always none for one case).
		std::vector<std::string> Run(std::span<NodeCase*> cases)
		{
			InputScope input;
			std::vector<RunningCase> running;
			running.reserve(cases.size());
			Scene scene("Coverage");
			int frames = 0;
			for (NodeCase* nodeCase : cases)
			{
				size_t const index = m_NextToken++;
				std::string token = fmt::format("Case[{:04}]", index);
				AssetHandle const graph =
					AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(nodeCase->Document), token + " " + nodeCase->Node->Type);
				Entity entity = scene.CreateEntity(token);
				scene.CreateEntity(token + " Child", entity);
				AddComponents(scene, entity, nodeCase->MemberTypeComponents);
				if (nodeCase->Recipe != nullptr)
				{
					AddComponents(scene, entity, nodeCase->Recipe->Components);
					PressAndRelease(*nodeCase->Recipe);
				}
				if (!nodeCase->ScriptClass.empty())
				{
					entity.AddComponent<ScriptComponent>().ClassName = nodeCase->ScriptClass;
				}
				entity.AddComponent<VisualScriptComponent>().Graph = graph;
				running.push_back({nodeCase, std::move(token), graph, entity.GetUUID()});
				frames = std::max(frames, nodeCase->Frames);
			}

			// A viewport, as in the player and the editor: cameras map between the screen and the world.
			scene.OnViewportResize(1280, 720);
			uint64_t const logStart = Log::GetNextEntryIndex();
			scene.OnRuntimeStart();
			for (int frame = 0; frame < frames; frame++)
			{
				scene.OnUpdateRuntime(Timestep(FrameTime));
			}
			// Instances go when the scene stops: Ran is read before.
			std::vector<std::optional<bool>> ran;
			for (RunningCase const& runningCase : running)
			{
				ran.push_back(ReadRan(runningCase.Entity));
			}
			std::vector<std::string> leaks = FindLeaks(scene);
			scene.OnRuntimeStop();
			std::vector<LogEntry> const entries = Log::GetEntries(logStart);
			for (RunningCase const& runningCase : running)
			{
				AssetManager::RemoveMemoryAsset(runningCase.Graph);
			}

			// What each case logged, and what named none.
			std::vector<std::vector<std::string>> logged(running.size());
			std::vector<std::string> unattributed = std::move(leaks);
			for (LogEntry const& entry : entries)
			{
				if (entry.Severity < LogLevel::Warn)
				{
					continue;
				}
				bool named = false;
				for (size_t index = 0; index < running.size(); index++)
				{
					if (running.size() == 1 || entry.Message.find(running[index].Token) != std::string::npos ||
					    entry.Message.find(running[index].Entity.ToString()) != std::string::npos)
					{
						logged[index].push_back(entry.Message);
						named = true;
					}
				}
				if (!named)
				{
					unattributed.push_back(entry.Message);
				}
			}

			for (size_t index = 0; index < running.size(); index++)
			{
				Judge(running[index], ran[index], logged[index]);
				if (running.size() == 1 && !unattributed.empty() && running[index].Case->Failure.empty())
				{
					running[index].Case->Failure = "it changed what other cases see: " + unattributed.front();
				}
			}
			return running.size() == 1 ? std::vector<std::string>() : unattributed;
		}

		// The global state a case may change and must leave as it found it: the scene's time scale and gravity, the cursor.
		std::vector<std::string> FindLeaks(Scene const& scene) const
		{
			std::vector<std::string> leaks;
			if (scene.GetTimeScale() != 1.0f)
			{
				leaks.push_back(fmt::format("the time scale is {}", scene.GetTimeScale()));
			}
			if (scene.GetSettings().Physics.Gravity != m_DefaultGravity)
			{
				glm::vec3 const gravity = scene.GetSettings().Physics.Gravity;
				leaks.push_back(fmt::format("the gravity is ({}, {}, {})", gravity.x, gravity.y, gravity.z));
			}
			if (Input::GetCursorMode() != CursorMode::Normal)
			{
				leaks.push_back("the cursor mode is not Normal");
			}
			return leaks;
		}

		// The value of the entity's Ran variable; nothing when it runs no visual script.
		static std::optional<bool> ReadRan(UUID entity)
		{
			Result<ScriptFieldMap> variables = VisualScriptEngine::GetInstanceVariables(entity);
			if (!variables)
			{
				return std::nullopt;
			}
			auto const value = variables.GetValue().find("Ran");
			return value != variables.GetValue().end() && value->second.GetType() == ScriptFieldType::Bool && value->second.GetBool();
		}

		// Adds the components of a recipe's JSON text ({"RigidBody": {...}, ...}) to the entity.
		static void AddComponents(Scene& scene, Entity entity, std::string_view components)
		{
			if (components.empty())
			{
				return;
			}
			Result<Json> parsed = ParseJson(components);
			REQUIRE_MESSAGE(parsed.IsOk(), components);
			REQUIRE(parsed.GetValue().is_object());
			for (auto const& [name, fields] : parsed.GetValue().items())
			{
				ComponentInfo const* const component = ComponentRegistry::Find(name);
				REQUIRE_MESSAGE(component != nullptr, name);
				Result<void> const added =
					component->Deserialize(scene.GetRegistry(), entity.GetHandle(), fields, AssetManager::CreateDeserializationContext());
				REQUIRE_MESSAGE(added.IsOk(), (added ? std::string() : name + ": " + added.GetError()));
			}
		}

		static void PressAndRelease(NodeCaseRecipe const& recipe)
		{
			for (KeyCode const key : recipe.HeldKeys)
			{
				Input::SetKeyState(key, true);
				Input::SetKeyState(key, false);
			}
			for (MouseButton const button : recipe.HeldMouseButtons)
			{
				Input::SetMouseButtonState(button, true);
				Input::SetMouseButtonState(button, false);
			}
			for (GamepadButton const button : recipe.HeldGamepadButtons)
			{
				Input::GamepadState state;
				state.Connected = true;
				state.Buttons[static_cast<size_t>(button)] = true;
				Input::SetGamepadState(0, state);
				state.Buttons[static_cast<size_t>(button)] = false;
				Input::SetGamepadState(0, state);
			}
		}

		// Passes when Ran is true and the case logged no Warning or Error but the one its recipe allows, or when its recipe
		// expects an aborting error and the case stopped with it at the node.
		static void Judge(RunningCase const& runningCase, std::optional<bool> ran, std::vector<std::string> const& logged)
		{
			NodeCase& nodeCase = *runningCase.Case;
			NodeCaseRecipe const* const recipe = nodeCase.Recipe;
			if (recipe != nullptr && !recipe->ExpectedError.empty())
			{
				std::string const place = fmt::format("(node {})", nodeCase.NodeID);
				bool const stopped = std::any_of(logged.begin(), logged.end(),
				                                 [&](std::string const& message)
				                                 {
													 return message.find(place) != std::string::npos &&
					                                        message.find(recipe->ExpectedError) != std::string::npos;
												 });
				if (!stopped)
				{
					nodeCase.Failure = fmt::format("it did not stop with the error \"{}\"{}", recipe->ExpectedError,
					                               logged.empty() ? std::string() : "; it logged: " + logged.front());
				}
				return;
			}
			for (std::string const& message : logged)
			{
				if (recipe == nullptr || recipe->ExpectedLog.empty() || message.find(recipe->ExpectedLog) == std::string::npos)
				{
					nodeCase.Failure = "it logged: " + message;
					return;
				}
			}
			if (!ran.has_value())
			{
				nodeCase.Failure = "its entity runs no visual script";
			}
			else if (!*ran)
			{
				nodeCase.Failure = "the flow never set Ran after it";
			}
		}

		glm::vec3 m_DefaultGravity = Scene().GetSettings().Physics.Gravity;
		size_t m_NextToken = 1;
		size_t m_Reruns = 0;
		std::string m_FirstUnattributed;
	};

	// Whether a node of the graph runs: an event with a linked exit, a node an event's chain reaches, or a pure node such a
	// node reads (directly or through other pure nodes). Disabled nodes run nothing.
	bool IsLinkedIntoFlow(VisualScriptGraph const& graph, uint32_t target, VisualScriptLibrary const& library)
	{
		std::unordered_map<uint32_t, VisualScriptNode const*> nodes;
		for (VisualScriptNode const& node : graph.Nodes)
		{
			if (node.Enabled == VisualScriptNodeEnabled::Enabled)
			{
				nodes.emplace(node.ID, &node);
			}
		}
		// Whether a link carries the flow: by its input's pin, else its output's (pins a node's settings make are not in its
		// kind's default pins); a link of pins neither has carries a value.
		auto const pinType = [&](VisualScriptPinReference const& end, VisualScriptPinDirection direction) -> std::string_view
		{
			auto const node = nodes.find(end.Node);
			VisualScriptNodeTemplate const* const kind = node != nodes.end() ? library.FindNode(node->second->Type) : nullptr;
			VisualScriptPin const* const pin = kind != nullptr ? kind->FindPin(end.Pin, direction) : nullptr;
			return pin != nullptr ? std::string_view(pin->Type) : std::string_view();
		};
		auto const isExec = [&](VisualScriptLink const& link)
		{
			std::string_view const input = pinType(link.To, VisualScriptPinDirection::In);
			return !input.empty() ? input == VisualScriptExecType
			                      : pinType(link.From, VisualScriptPinDirection::Out) == VisualScriptExecType;
		};
		// Reached: events with a linked exec exit, then what exec links from reached nodes lead to.
		std::unordered_set<uint32_t> reached;
		for (VisualScriptLink const& link : graph.Links)
		{
			auto const from = nodes.find(link.From.Node);
			VisualScriptNodeTemplate const* const kind = from != nodes.end() ? library.FindNode(from->second->Type) : nullptr;
			if (kind != nullptr && kind->Kind == VisualScriptNodeKind::Event && nodes.contains(link.To.Node) && isExec(link))
			{
				reached.insert(link.From.Node);
			}
		}
		for (bool grew = true; grew;)
		{
			grew = false;
			for (VisualScriptLink const& link : graph.Links)
			{
				bool const sourceRuns = reached.contains(link.From.Node) && isExec(link);
				bool const readByRunning = reached.contains(link.To.Node) && !isExec(link);
				uint32_t const next = sourceRuns ? link.To.Node : link.From.Node;
				if ((sourceRuns || readByRunning) && nodes.contains(next) && reached.insert(next).second)
				{
					grew = true;
				}
			}
		}
		return reached.contains(target);
	}

	// What an exclusion's graph is not, or empty when it holds an enabled node of the type linked into flow and is run.
	std::string CheckExclusion(NodeCaseExclusion const& exclusion, VisualScriptLibrary const& library,
	                           std::vector<Testing::RunSetGraph> const& featureTestRunSet)
	{
		VisualScriptDocument document;
		if (exclusion.RunBy == NodeCaseRunner::FeatureTestRun)
		{
			auto const graph = std::find_if(featureTestRunSet.begin(), featureTestRunSet.end(),
			                                [&](Testing::RunSetGraph const& candidate)
			                                {
												return candidate.Path == exclusion.CoveredBy;
											});
			if (graph == featureTestRunSet.end())
			{
				return fmt::format("{} is no graph a FeatureTest scene runs, nor a library one calls", exclusion.CoveredBy);
			}
			document = graph->Document;
		}
		else
		{
			if (std::find(Testing::SharedVisualScriptFixtures.begin(), Testing::SharedVisualScriptFixtures.end(), exclusion.CoveredBy) ==
			    Testing::SharedVisualScriptFixtures.end())
			{
				return fmt::format("{} is no shared fixture document VisualScriptEngineTests.cpp runs", exclusion.CoveredBy);
			}
			std::filesystem::path const path = FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Documents" /
			                                   FileSystem::PathFromUtf8(exclusion.CoveredBy);
			Result<std::string> text = FileSystem::ReadTextFile(path);
			if (!text)
			{
				return fmt::format("{} cannot be read: {}", exclusion.CoveredBy, text.GetError());
			}
			Result<Json> json = ParseJson(text.GetValue());
			if (!json)
			{
				return fmt::format("{} is no JSON: {}", exclusion.CoveredBy, json.GetError());
			}
			Result<VisualScriptDocument> read = VisualScriptSerializer::DeserializeDocument(json.GetValue(), Testing::MakeStrictContext());
			if (!read)
			{
				return fmt::format("{} does not load: {}", exclusion.CoveredBy, read.GetError());
			}
			document = read.TakeValue();
		}
		auto const runs = [&](VisualScriptGraph const& graph)
		{
			return std::any_of(graph.Nodes.begin(), graph.Nodes.end(),
			                   [&](VisualScriptNode const& node)
			                   {
								   return node.Type == exclusion.Type && node.Enabled == VisualScriptNodeEnabled::Enabled &&
				                          IsLinkedIntoFlow(graph, node.ID, library);
							   });
		};
		bool covered = runs(document.Graph);
		for (VisualScriptFunction const& function : document.Functions)
		{
			covered = covered || runs(function.Graph);
		}
		return covered ? std::string() : fmt::format("{} has no enabled node of the type linked into flow", exclusion.CoveredBy);
	}
}

TEST_CASE("VisualScript: every node of the library analyzes and runs in a generated graph")
{
	auto const started = std::chrono::steady_clock::now();
	Testing::PhysicsSystemScope physics;
	Testing::AudioEngineScope audio;
	// The feature-test project's assets: scenes, sounds and graphs that members load by path, and the graphs exclusions name.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const featureTestAssets = FileSystem::PathFromUtf8(STRADA_FEATURE_TEST_DIR) / "Assets";
	REQUIRE(FileSystem::CopyDirectory(featureTestAssets, directory.GetPath() / "Assets").IsOk());
	Testing::AssetManagerScope assets;
	Result<AssetRefreshResult> const opened = AssetManager::OpenAssetDirectory(directory.GetPath() / "Assets");
	REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
	std::vector<Testing::RunSetGraph> const featureTestRunSet = Testing::FindVisualScriptRunSet(featureTestAssets);
	// TestScripts, whose Mover has [VisualNode] members.
	Testing::ScriptEngineScope scripting;
	RecordingHost host;
	Ref<VisualScriptLibrary const> const library = VisualScriptEngine::GetLibrary();
	REQUIRE(library != nullptr);

	// The tables name only what the library has.
	std::vector<std::string> failures;
	for (NodeCaseRecipe const& recipe : Recipes)
	{
		if (library->FindNode(recipe.Type) == nullptr)
		{
			failures.push_back(fmt::format("{}: a recipe of a node the library does not have: remove it", recipe.Type));
		}
	}
	for (NodeCaseExclusion const& exclusion : Exclusions)
	{
		if (library->FindNode(exclusion.Type) == nullptr)
		{
			failures.push_back(fmt::format("{}: an exclusion of a node the library does not have: remove it", exclusion.Type));
		}
		else if (std::string const problem = CheckExclusion(exclusion, *library, featureTestRunSet); !problem.empty())
		{
			failures.push_back(fmt::format("{}: excluded ({}), but {}", exclusion.Type, exclusion.Reason, problem));
		}
	}
	// Static classes are no types of graphs: a type is known by the members it declares.
	auto const declaresMembers = [&library](std::string_view type)
	{
		return std::any_of(library->GetNodes().begin(), library->GetNodes().end(),
		                   [type](VisualScriptNodeTemplate const& node)
		                   {
							   return GetDeclaringType(node.Type) == type;
						   });
	};
	for (std::string_view const type : GlobalEffectTypes)
	{
		if (!declaresMembers(type))
		{
			failures.push_back(fmt::format("{}: a type of GlobalEffectTypes whose members the library does not have: remove it", type));
		}
	}
	for (MemberTypeRecipe const& recipe : MemberTypeRecipes)
	{
		if (!declaresMembers(recipe.Type))
		{
			failures.push_back(fmt::format("{}: a recipe of a type whose members the library does not have: remove it", recipe.Type));
		}
	}

	// Graph settings of scripts name a graph of their own.
	VisualScriptDocument target;
	target.Description = "What Graph settings of the coverage cases name.";
	AssetHandle const targetScript = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(std::move(target)), "Coverage target");

	// Generate and analyze: no Error, and the node resolves exactly.
	NodeCaseBuilder builder(*library, targetScript);
	std::vector<NodeCase> cases;
	for (VisualScriptNodeTemplate const& node : library->GetNodes())
	{
		if (FindByType<NodeCaseExclusion>(Exclusions, node.Type) != nullptr)
		{
			continue;
		}
		NodeCase nodeCase = builder.Build(node, FindByType<NodeCaseRecipe>(Recipes, node.Type));
		if (nodeCase.Failure.empty())
		{
			Result<VisualScriptAnalysis> analysis = VisualScriptEngine::Analyze(nodeCase.Document);
			if (!analysis)
			{
				nodeCase.Failure = "the runtime did not analyze it: " + analysis.GetError();
			}
			else if (CountVisualScriptErrors(analysis.GetValue().Diagnostics) > 0)
			{
				nodeCase.Failure = "its graph has " + DescribeVisualScriptErrors(analysis.GetValue().Diagnostics);
			}
			else if (VisualScriptAnalyzedNode const* const analyzed = analysis.GetValue().FindNode(nodeCase.NodeID);
			         analyzed == nullptr || analyzed->Resolved != VisualScriptResolution::Exact)
			{
				nodeCase.Failure = "it does not resolve exactly";
			}
		}
		cases.push_back(std::move(nodeCase));
	}

	// Run: together in batches, and alone where a case changes what the others see.
	NodeCaseScenes runner;
	std::vector<NodeCase*> batch;
	size_t alone = 0;
	for (NodeCase& nodeCase : cases)
	{
		if (!nodeCase.Failure.empty())
		{
			continue;
		}
		if (nodeCase.Alone)
		{
			std::array<NodeCase*, 1> single = {&nodeCase};
			runner.RunBatch(single);
			alone++;
			continue;
		}
		batch.push_back(&nodeCase);
		if (batch.size() == BatchSize)
		{
			runner.RunBatch(batch);
			batch.clear();
		}
	}
	if (!batch.empty())
	{
		runner.RunBatch(batch);
	}
	// What the members that end the test run or replace the scene asked of the application.
	CHECK(host.SceneLoads == 1);
	CHECK(host.Quits == 1);
	CHECK(host.Finishes == 1);
	AssetManager::RemoveMemoryAsset(targetScript);

	for (NodeCase const& nodeCase : cases)
	{
		if (!nodeCase.Failure.empty())
		{
			failures.push_back(fmt::format("{}: {}: add a recipe or a checked exclusion in VisualScriptNodeCoverageTests.cpp",
			                               nodeCase.Node->Type, nodeCase.Failure));
		}
	}
	std::sort(failures.begin(), failures.end());
	std::string described = fmt::format("{} of {} nodes fail:", failures.size(), cases.size() + Exclusions.size());
	for (std::string const& failure : failures)
	{
		described += "\n  " + failure;
	}
	CHECK_MESSAGE(failures.empty(), described);
	double const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	MESSAGE(fmt::format("{} nodes: {} cases ({} alone), {} recipes, {} member-type recipes, {} exclusions, {} batches run again{}, in "
	                    "{:.1f} s",
	                    library->GetNodes().size(), cases.size(), alone, Recipes.size(), MemberTypeRecipes.size(), Exclusions.size(),
	                    runner.GetReruns(), runner.GetFirstUnattributed().empty() ? "" : " (first: " + runner.GetFirstUnattributed() + ")",
	                    seconds));
}
