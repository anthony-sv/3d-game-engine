#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/AssetRegistry.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/Log.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/ScriptEngineInternal.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	// Records what scripts ask of the application for as long as it lives.
	struct RecordingHost final : ScriptHost
	{
		RecordingHost() { ScriptEngine::SetHost(this); }
		~RecordingHost() override { ScriptEngine::SetHost(nullptr); }

		RecordingHost(RecordingHost const&) = delete;
		RecordingHost& operator=(RecordingHost const&) = delete;

		bool IsEditor() const override { return false; }
		void RequestSceneLoad(AssetHandle) override {}
		void RequestQuit() override {}
		void ReportTestResult(ScriptTestResult const& result) override { Results.push_back(result); }
		void FinishTests() override {}
		void OnScriptException() override { Exceptions++; }

		std::vector<ScriptTestResult> Results;
		uint32_t Exceptions = 0;
	};

	// Lets the scripting bindings act on a scene that is not running, for as long as it lives.
	class SceneContextScope
	{
	public:
		explicit SceneContextScope(Scene& scene) { ScriptEngine::SetSceneContext(&scene); }
		~SceneContextScope() { ScriptEngine::SetSceneContext(nullptr); }

		SceneContextScope(SceneContextScope const&) = delete;
		SceneContextScope& operator=(SceneContextScope const&) = delete;
	};

	// Input is global: graphs poll it on every update.
	class InputScope
	{
	public:
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }

		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
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

	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type, Json value, bool isPublic)
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

	VisualScriptPin MakePin(std::string type, VisualScriptPinDirection direction)
	{
		VisualScriptPin pin;
		pin.Name = "Value";
		pin.Direction = direction;
		pin.Type = std::move(type);
		return pin;
	}

	// Sets the public variable Count to a number when created.
	VisualScriptDocument MakeCountDocument(int32_t count)
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Count", "int", 0, true));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 1}}), Json::object({{"Value", count}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		return document;
	}

	// The count document with a node the library does not have: one error, at node 12.
	VisualScriptDocument MakeBrokenDocument()
	{
		VisualScriptDocument document = MakeCountDocument(2);
		document.Graph.Nodes.push_back(MakeNode(12, "Sample.NoSuchNode"));
		return document;
	}

	// Counts the events of its instance in public variables, and logs when it is destroyed.
	VisualScriptDocument MakeEventCounterDocument()
	{
		VisualScriptDocument document;
		auto const addCounter = [&document](uint32_t variable, std::string name, uint32_t firstNode, std::vector<std::string> const& events)
		{
			document.Variables.push_back(MakeVariable(variable, std::move(name), "int", 0, true));
			uint32_t const set = firstNode;
			uint32_t const add = firstNode + 1;
			uint32_t const get = firstNode + 2;
			document.Graph.Nodes.push_back(MakeNode(set, "Variable.Set", Json::object({{"Variable", variable}})));
			document.Graph.Nodes.push_back(MakeNode(add, "Operator.Add", Json::object(), Json::object({{"B", 1}})));
			document.Graph.Nodes.push_back(MakeNode(get, "Variable.Get", Json::object({{"Variable", variable}})));
			document.Graph.Links.push_back(MakeLink(get, "Value", add, "A"));
			document.Graph.Links.push_back(MakeLink(add, "Result", set, "Value"));
			uint32_t node = firstNode + 3;
			for (std::string const& event : events)
			{
				document.Graph.Nodes.push_back(MakeNode(node, event));
				document.Graph.Links.push_back(MakeLink(node, "Then", set, "Execute"));
				node++;
			}
		};
		addCounter(1, "Creates", 10, {"Event.Create"});
		addCounter(2, "Updates", 20, {"Event.Update"});
		addCounter(3, "FixedUpdates", 30, {"Event.FixedUpdate"});
		addCounter(4, "Contacts", 40, {"Event.CollisionEnter", "Event.CollisionExit", "Event.TriggerEnter", "Event.TriggerExit"});
		addCounter(5, "Clicks", 50, {"Event.Click"});
		document.Graph.Nodes.push_back(MakeNode(60, "Event.Destroy"));
		document.Graph.Nodes.push_back(
			MakeNode(61, "Call:Strada.Log.Info(string)", Json::object(), Json::object({{"Message", "the counting graph is destroyed"}})));
		document.Graph.Links.push_back(MakeLink(60, "Then", 61, "Execute"));
		return document;
	}

	// Divides 1 by a variable that is 0 when created: an aborting error.
	VisualScriptDocument MakeDivisionDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Result", "int", 0, true));
		document.Variables.push_back(MakeVariable(2, "Zero", "int", 0, false));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 1}})));
		document.Graph.Nodes.push_back(MakeNode(12, "Operator.Divide", Json::object(), Json::object({{"A", 1}})));
		document.Graph.Nodes.push_back(MakeNode(13, "Variable.Get", Json::object({{"Variable", 2}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(13, "Value", 12, "B"));
		document.Graph.Links.push_back(MakeLink(12, "Result", 11, "Value"));
		return document;
	}

	// Converts its entity to the game's Mover script (a variable of a game type) and says whether it was one.
	VisualScriptDocument MakeMoverDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Target", "Strada.Tests.Mover", Json(), false));
		document.Variables.push_back(MakeVariable(2, "IsMover", "bool", false, true));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 1}})));
		document.Graph.Nodes.push_back(MakeNode(12, "Convert", Json::object({{"To", "Strada.Tests.Mover"}})));
		document.Graph.Nodes.push_back(MakeNode(13, "Self"));
		document.Graph.Nodes.push_back(MakeNode(14, "Variable.Set", Json::object({{"Variable", 2}})));
		document.Graph.Nodes.push_back(MakeNode(15, "IsValid"));
		document.Graph.Nodes.push_back(MakeNode(16, "Variable.Get", Json::object({{"Variable", 1}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(13, "Self", 12, "Value"));
		document.Graph.Links.push_back(MakeLink(12, "Result", 11, "Value"));
		document.Graph.Links.push_back(MakeLink(11, "Then", 14, "Execute"));
		document.Graph.Links.push_back(MakeLink(16, "Value", 15, "Value"));
		document.Graph.Links.push_back(MakeLink(15, "Result", 14, "Value"));
		return document;
	}

	DeserializationContext MakeStrictContext()
	{
		DeserializationContext context;
		context.UnknownFields = UnknownFieldPolicy::Warn;
		return context;
	}

	std::string DescribeIssues(std::vector<VisualScriptIssue> const& issues)
	{
		std::string described;
		for (VisualScriptIssue const& issue : issues)
		{
			described += (described.empty() ? "" : "; ") + issue.Path + ": " + issue.Message;
		}
		return described;
	}

	// A shared fixture document (Tests/Data/VisualScripting/Documents), loaded as automation loads documents.
	VisualScriptDocument ReadFixture(std::string_view name)
	{
		std::filesystem::path const path =
			FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Documents" / FileSystem::PathFromUtf8(name);
		Result<std::string> text = FileSystem::ReadTextFile(path);
		REQUIRE_MESSAGE(text.IsOk(), FileSystem::PathToUtf8(path));
		Result<Json> json = ParseJson(text.GetValue());
		REQUIRE_MESSAGE(json.IsOk(), json.GetError());
		std::vector<VisualScriptIssue> issues;
		Result<VisualScriptDocument> document = VisualScriptSerializer::DeserializeDocument(json.GetValue(), MakeStrictContext(), &issues);
		REQUIRE_MESSAGE(document.IsOk(), document.GetError());
		CHECK_MESSAGE(issues.empty(), DescribeIssues(issues));
		return document.TakeValue();
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

	// Gives the graph's asset the document and loads the graph: whether that sent the document to the runtime (the
	// graph's generation changed).
	bool SendsDocument(VisualScriptAsset& asset, AssetHandle graph, VisualScriptDocument document)
	{
		uint64_t const generation = VisualScriptEngine::GetProgramGeneration(graph);
		asset.SetDocument(std::move(document));
		Result<void> const loaded = VisualScriptEngine::EnsureLoaded(graph);
		REQUIRE_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
		return VisualScriptEngine::GetProgramGeneration(graph) != generation;
	}

	// Records each document the engine sends to the runtime while it lives, with the references sent beside it, and passes
	// both on (the runtime compiles the document as ever). Lives within a Testing::ScriptRuntimeScope, one at a time.
	class SentDocuments
	{
	public:
		SentDocuments()
		{
			ScriptEngineInternal::ScriptEngineData* const data = ScriptEngineInternal::GetData();
			REQUIRE(data != nullptr);
			REQUIRE(s_Load == nullptr);
			s_Sent.clear();
			s_Load = std::exchange(data->Host.VisualScriptLoad, &Load);
		}

		~SentDocuments()
		{
			if (ScriptEngineInternal::ScriptEngineData* const data = ScriptEngineInternal::GetData(); data != nullptr)
			{
				data->Host.VisualScriptLoad = s_Load;
			}
			s_Load = nullptr;
			s_Sent.clear();
		}

		SentDocuments(SentDocuments const&) = delete;
		SentDocuments& operator=(SentDocuments const&) = delete;

		size_t GetCount() const { return s_Sent.size(); }

		// The document of a send, read from the content form the runtime received.
		VisualScriptDocument Get(size_t index) const
		{
			REQUIRE(index < s_Sent.size());
			Result<Json> json = ParseJson(s_Sent[index].Document);
			REQUIRE_MESSAGE(json.IsOk(), json.GetError());
			std::vector<VisualScriptIssue> issues;
			Result<VisualScriptDocument> document =
				VisualScriptSerializer::DeserializeDocument(json.GetValue(), MakeStrictContext(), &issues);
			REQUIRE_MESSAGE(document.IsOk(), document.GetError());
			CHECK_MESSAGE(issues.empty(), DescribeIssues(issues));
			return document.TakeValue();
		}

		// The references text of a send, as the runtime received it; empty for none.
		std::string const& GetReferences(size_t index) const
		{
			REQUIRE(index < s_Sent.size());
			return s_Sent[index].References;
		}

	private:
		struct Send
		{
			std::string Document;
			std::string References;
		};

		static std::string ToText(char const* text, int32_t length)
		{
			return text != nullptr && length > 0 ? std::string(text, static_cast<size_t>(length)) : std::string();
		}

		static int32_t Load(uint64_t graph, char const* name, int32_t nameLength, char const* document, int32_t documentLength,
		                    char const* references, int32_t referencesLength, void* context, ScriptEngineInternal::ReceiveFunction receive)
		{
			s_Sent.push_back({ToText(document, documentLength), ToText(references, referencesLength)});
			return s_Load(graph, name, nameLength, document, documentLength, references, referencesLength, context, receive);
		}

		static inline decltype(ScriptEngineInternal::HostFunctions::VisualScriptLoad) s_Load = nullptr;
		static inline std::vector<Send> s_Sent;
	};

	// receive for the entry points: counts its calls and keeps the last text.
	struct Received
	{
		int32_t Calls = 0;
		std::string Text;
	};

	void Receive(void* context, char const* text, int32_t length)
	{
		Received& received = *static_cast<Received*>(context);
		received.Calls++;
		received.Text = text != nullptr && length > 0 ? std::string(text, static_cast<size_t>(length)) : std::string();
	}

	// One end of the link of a two-node document: a node and its pin.
	struct LinkEnd
	{
		// The pin's type, or the wildcard's node and pin, in messages.
		std::string Name;
		std::string NodeType;
		Json Settings;
		std::string Pin;
		// A private variable of this type, which Settings name; empty for none.
		std::string VariableType;
		// A wildcard: CanConnect is asked about the pin as the library describes it, before analysis resolves it.
		bool IsWildcard = false;
	};

	constexpr uint32_t OutputNode = 10;
	constexpr uint32_t InputNode = 11;

	// Variables of every kind of type: numbers, text, a struct, an enum, entities, components, assets, objects.
	constexpr std::array<std::string_view, 17> ParityTypes = {"bool",
	                                                          "int",
	                                                          "uint",
	                                                          "long",
	                                                          "ulong",
	                                                          "float",
	                                                          "double",
	                                                          "string",
	                                                          "Strada.Vector3",
	                                                          "Strada.KeyCode",
	                                                          "Strada.Entity",
	                                                          "Strada.RigidBodyComponent",
	                                                          "Strada.Component",
	                                                          "Strada.Material",
	                                                          "Strada.Asset",
	                                                          "Strada.AssetHandle",
	                                                          "System.Object"};

	LinkEnd MakeVariableOutput(std::string_view type)
	{
		return {std::string(type), "Variable.Get", Json::object({{"Variable", 1}}), "Value", std::string(type), false};
	}

	LinkEnd MakeVariableInput(std::string_view type)
	{
		return {std::string(type), "Variable.Set", Json::object({{"Variable", 2}}), "Value", std::string(type), false};
	}

	VisualScriptPin const* FindLinkPin(VisualScriptLibrary const& library, VisualScriptAnalysis const& analysis, LinkEnd const& end,
	                                   uint32_t node, VisualScriptPinDirection direction)
	{
		if (end.IsWildcard)
		{
			VisualScriptNodeTemplate const* found = library.FindNode(end.NodeType);
			return found != nullptr ? found->FindPin(end.Pin, direction) : nullptr;
		}
		VisualScriptAnalyzedNode const* found = analysis.FindNode(node);
		return found != nullptr ? found->FindPin(end.Pin, direction) : nullptr;
	}

	// A small library in the runtime's form, its types out of order.
	constexpr std::string_view LibraryJson = R"json({
	"Types": [
		{ "ID": "string", "Title": "String", "Kind": "String", "FieldType": "String", "Literal": true, "ImplicitTo": ["System.Object"] },
		{ "ID": "exec", "Title": "Exec", "Kind": "Exec", "ImplicitTo": [] },
		{ "ID": "int", "Title": "Integer", "Kind": "Integer", "FieldType": "Int32", "Literal": true, "ImplicitTo": ["long", "float", "System.Object"] },
		{ "ID": "long", "Title": "Long", "Kind": "Integer", "FieldType": "Int64", "Literal": true, "ImplicitTo": ["float", "System.Object"] },
		{ "ID": "float", "Title": "Float", "Kind": "Float", "FieldType": "Float", "Literal": true, "ImplicitTo": ["System.Object"] },
		{ "ID": "Game.Layers", "Title": "Layers", "Kind": "Enum", "FieldType": "UInt64", "Literal": true,
		  "Enumerators": [ { "Name": "None", "Value": "0" }, { "Name": "All", "Value": "18446744073709551615" } ], "Flags": true,
		  "ImplicitTo": ["System.Object"] },
		{ "ID": "Strada.KeyCode", "Title": "Key Code", "Kind": "Enum", "FieldType": "Int32", "Literal": true,
		  "Enumerators": [ { "Name": "Space", "Value": 32 }, { "Name": "Escape", "Value": 256 } ], "Flags": false, "ImplicitTo": ["System.Object"] },
		{ "ID": "Strada.Vector3", "Title": "Vector3", "Kind": "Vector", "FieldType": "Vector3", "Literal": true, "Makeable": true,
		  "Breakable": true, "Fields": [ { "Name": "X", "Type": "float" }, { "Name": "Y", "Type": "float" }, { "Name": "Z", "Type": "float" } ],
		  "ImplicitTo": ["System.Object"] },
		{ "ID": "Strada.Entity", "Title": "Entity", "Kind": "Entity", "FieldType": "Entity", "Literal": true, "ImplicitTo": ["System.Object"] },
		{ "ID": "Strada.Entity[]", "Title": "Entity Array", "Kind": "Array", "Element": "Strada.Entity", "ImplicitTo": ["System.Object"] },
		{ "ID": "Strada.Material", "Title": "Material", "Kind": "Asset", "FieldType": "Asset", "AssetType": "Material", "Literal": true,
		  "Base": "Strada.Asset", "ImplicitTo": ["Strada.Asset", "System.Object"] },
		{ "ID": "Strada.Asset", "Title": "Asset", "Kind": "Asset", "ImplicitTo": ["System.Object"] },
		{ "ID": "System.Object", "Title": "Object", "Kind": "Object", "ImplicitTo": [] }
	],
	"Nodes": [
		{ "Type": "Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3)", "Title": "Add Force", "Subtitle": "Target is Rigid Body",
		  "Category": "Components/Rigid Body", "Description": "Pushes the body.", "Keywords": ["push"], "Kind": "Call", "Pure": false,
		  "Latent": false, "Pins": [
			{ "Name": "Execute", "Direction": "In", "Type": "exec" },
			{ "Name": "Target", "Direction": "In", "Type": "Strada.Entity", "DefaultsToSelf": true },
			{ "Name": "Force", "Direction": "In", "Type": "Strada.Vector3", "Label": "Force (N)", "Default": [0.0, 1.0, 0.0] },
			{ "Name": "Then", "Direction": "Out", "Type": "exec" } ],
		  "Settings": [] },
		{ "Type": "Flow.Delay", "Title": "Delay", "Category": "Flow", "Keywords": ["wait"], "Kind": "Latent", "Pure": false, "Latent": true,
		  "Pins": [
			{ "Name": "Execute", "Direction": "In", "Type": "exec" },
			{ "Name": "Duration", "Direction": "In", "Type": "float", "Default": 0.2 },
			{ "Name": "Completed", "Direction": "Out", "Type": "exec", "Role": "Continuation" } ],
		  "Settings": [] },
		{ "Type": "Flow.Tween", "Title": "Tween", "Category": "Flow", "Keywords": [], "Kind": "Latent", "Pure": false, "Latent": true,
		  "Pins": [
			{ "Name": "Execute", "Direction": "In", "Type": "exec" },
			{ "Name": "Tick", "Direction": "Out", "Type": "exec", "Role": "Tick" } ],
		  "Settings": [ { "Name": "Easing", "Kind": "Choice", "Default": "Linear", "Choices": ["Linear", "EaseIn"] } ] },
		{ "Type": "Flow.Sequence", "Title": "Sequence", "Category": "Flow", "Keywords": [], "Kind": "Flow", "Pure": false, "Latent": false,
		  "Pins": [ { "Name": "Execute", "Direction": "In", "Type": "exec" } ],
		  "Settings": [ { "Name": "Count", "Kind": "Int", "Default": 2, "Min": 2, "Max": 64 },
		                { "Name": "Of", "Kind": "Type", "Required": true, "Constraint": "Enum", "Type": "Strada.KeyCode" } ] },
		{ "Type": "Operator.Add", "Title": "Add", "Category": "Math/Operators", "Keywords": ["plus"], "Kind": "Operator", "Pure": true,
		  "Latent": false, "Pins": [
			{ "Name": "A", "Direction": "In", "Type": "*", "Constraint": "Any" },
			{ "Name": "B", "Direction": "In", "Type": "*", "Constraint": "Any" },
			{ "Name": "Result", "Direction": "Out", "Type": "*", "Constraint": "Any" } ],
		  "Settings": [] },
		{ "Type": "IsValid", "Title": "Is Valid", "Category": "Utilities", "Keywords": [], "Kind": "Data", "Pure": true, "Latent": false,
		  "Pins": [
			{ "Name": "Value", "Direction": "In", "Type": "*", "Constraint": "Reference" },
			{ "Name": "Result", "Direction": "Out", "Type": "bool" } ],
		  "Settings": [] },
		{ "Type": "Array.Get", "Title": "Get", "Category": "Utilities/Array", "Keywords": [], "Kind": "Data", "Pure": true, "Latent": false,
		  "Pins": [
			{ "Name": "Array", "Direction": "In", "Type": "*", "Constraint": "Array" },
			{ "Name": "Element", "Direction": "Out", "Type": "*", "Constraint": "Any", "ElementOf": "Array" } ],
		  "Settings": [] },
		{ "Type": "Reroute", "Title": "Reroute", "Category": "Utilities", "Keywords": ["knot"], "Kind": "Reroute", "Pure": true, "Latent": false,
		  "Pins": [
			{ "Name": "In", "Direction": "In", "Type": "*", "Constraint": "Any", "Group": "T" },
			{ "Name": "Out", "Direction": "Out", "Type": "*", "Constraint": "Any", "Group": "T" } ],
		  "Settings": [] },
		{ "Type": "Note", "Title": "Note", "Category": "", "Keywords": [], "Kind": "Data", "Pure": true, "Latent": false, "Pins": [],
		  "Settings": [] }
	]
})json";

	VisualScriptLibrary const& RequireLibrary(Result<Scope<VisualScriptLibrary>> const& library)
	{
		REQUIRE_MESSAGE(library.IsOk(), library.GetError());
		return *library.GetValue();
	}

	VisualScriptCategory const* FindChild(VisualScriptCategory const& category, std::string_view name)
	{
		auto const child = std::find_if(category.Children.begin(), category.Children.end(),
		                                [name](VisualScriptCategory const& candidate)
		                                {
											return candidate.Name == name;
										});
		return child != category.Children.end() ? &*child : nullptr;
	}

	std::vector<std::string> GetTypes(std::vector<VisualScriptNodeTemplate const*> const& nodes)
	{
		std::vector<std::string> types;
		for (VisualScriptNodeTemplate const* node : nodes)
		{
			types.push_back(node->Type);
		}
		return types;
	}

	size_t CountNodes(VisualScriptCategory const& root)
	{
		size_t count = 0;
		std::vector<VisualScriptCategory const*> pending = {&root};
		while (!pending.empty())
		{
			VisualScriptCategory const* category = pending.back();
			pending.pop_back();
			count += category->Nodes.size();
			for (VisualScriptCategory const& child : category->Children)
			{
				pending.push_back(&child);
			}
		}
		return count;
	}
}

TEST_CASE("VisualScriptLibrary: reads the runtime's types, node kinds, pins and settings, and finds them")
{
	Result<Scope<VisualScriptLibrary>> parsed = VisualScriptLibrary::Parse(LibraryJson);
	VisualScriptLibrary const& library = RequireLibrary(parsed);

	// Types are sorted by ID.
	std::vector<std::string> ids;
	for (VisualScriptTypeInfo const& type : library.GetTypes())
	{
		ids.push_back(type.ID);
	}
	CHECK(ids == std::vector<std::string>{"Game.Layers", "Strada.Asset", "Strada.Entity", "Strada.Entity[]", "Strada.KeyCode",
	                                      "Strada.Material", "Strada.Vector3", "System.Object", "exec", "float", "int", "long", "string"});
	CHECK(library.FindType("Strada.Missing") == nullptr);
	CHECK(library.FindType("") == nullptr);

	VisualScriptTypeInfo const* integer = library.FindType("int");
	REQUIRE(integer != nullptr);
	CHECK(integer->Title == "Integer");
	CHECK(integer->Kind == VisualScriptTypeKind::Integer);
	CHECK(integer->FieldType == ScriptFieldType::Int32);
	CHECK(integer->HasLiteral);
	CHECK(integer->ImplicitTo == std::vector<std::string>{"long", "float", "System.Object"});
	CHECK(integer->Source.at("ID") == "int");

	VisualScriptTypeInfo const* exec = library.FindType("exec");
	REQUIRE(exec != nullptr);
	CHECK(exec->Kind == VisualScriptTypeKind::Exec);
	CHECK(exec->FieldType == ScriptFieldType::None);
	CHECK_FALSE(exec->HasLiteral);
	CHECK(exec->ImplicitTo.empty());

	VisualScriptTypeInfo const* vector = library.FindType("Strada.Vector3");
	REQUIRE(vector != nullptr);
	CHECK(vector->Kind == VisualScriptTypeKind::Vector);
	CHECK(vector->Makeable);
	CHECK(vector->Breakable);
	REQUIRE(vector->Fields.size() == 3);
	CHECK(vector->Fields[2].Name == "Z");
	CHECK(vector->Fields[2].Type == "float");

	VisualScriptTypeInfo const* material = library.FindType("Strada.Material");
	REQUIRE(material != nullptr);
	CHECK(material->Kind == VisualScriptTypeKind::Asset);
	CHECK(material->FieldType == ScriptFieldType::Asset);
	CHECK(material->AcceptedAssetType == AssetType::Material);
	CHECK(material->Base == "Strada.Asset");
	VisualScriptTypeInfo const* asset = library.FindType("Strada.Asset");
	REQUIRE(asset != nullptr);
	CHECK(asset->AcceptedAssetType == AssetType::None);
	VisualScriptTypeInfo const* children = library.FindType("Strada.Entity[]");
	REQUIRE(children != nullptr);
	CHECK(children->Element == "Strada.Entity");

	// Enums keep their enumerators as values of their field type: 64-bit ones arrive as decimal strings.
	VisualScriptTypeInfo const* layers = library.FindType("Game.Layers");
	REQUIRE(layers != nullptr);
	CHECK(layers->Kind == VisualScriptTypeKind::Enum);
	CHECK(layers->IsFlags);
	REQUIRE(layers->Enumerators.size() == 2);
	CHECK(layers->Enumerators[1].Name == "All");
	CHECK(layers->Enumerators[1].Value == ScriptFieldValue::FromUInt64(18446744073709551615ull));

	std::optional<VisualScriptEnumInfo> const layersEnum = library.FindEnum("Game.Layers");
	REQUIRE(layersEnum.has_value());
	CHECK(layersEnum->UnderlyingType == ScriptFieldType::UInt64);
	CHECK(layersEnum->IsFlags);
	CHECK(layersEnum->Enumerators.size() == 2);
	std::optional<VisualScriptEnumInfo> const keys = library.FindEnum("Strada.KeyCode");
	REQUIRE(keys.has_value());
	CHECK(keys->UnderlyingType == ScriptFieldType::Int32);
	CHECK_FALSE(keys->IsFlags);
	REQUIRE(keys->Enumerators.size() == 2);
	CHECK(keys->Enumerators[0].Name == "Space");
	CHECK(keys->Enumerators[0].Value == ScriptFieldValue::FromInt32(32));
	CHECK_FALSE(library.FindEnum("int").has_value());
	CHECK_FALSE(library.FindEnum("Game.Missing").has_value());

	// Node kinds stay in library order, with their pins and settings.
	CHECK(library.GetNodes().size() == 9);
	CHECK(library.GetNodes().front().Type == "Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3)");
	CHECK(library.FindNode("Flow.Missing") == nullptr);

	VisualScriptNodeTemplate const* addForce = library.FindNode("Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3)");
	REQUIRE(addForce != nullptr);
	CHECK(addForce->Title == "Add Force");
	CHECK(addForce->Subtitle == "Target is Rigid Body");
	CHECK(addForce->Category == "Components/Rigid Body");
	CHECK(addForce->Description == "Pushes the body.");
	CHECK(addForce->Keywords == std::vector<std::string>{"push"});
	CHECK(addForce->Kind == VisualScriptNodeKind::Call);
	CHECK_FALSE(addForce->Pure);
	CHECK_FALSE(addForce->Latent);
	CHECK(addForce->Source.at("Title") == "Add Force");
	REQUIRE(addForce->Pins.size() == 4);
	VisualScriptPin const* target = addForce->FindPin("Target", VisualScriptPinDirection::In);
	REQUIRE(target != nullptr);
	CHECK(target->Type == "Strada.Entity");
	CHECK(target->DefaultsToSelf);
	CHECK(target->Default.is_null());
	VisualScriptPin const* force = addForce->FindPin("Force", VisualScriptPinDirection::In);
	REQUIRE(force != nullptr);
	CHECK(force->Label == "Force (N)");
	CHECK(force->Default == Json::array({0.0, 1.0, 0.0}));
	CHECK_FALSE(force->DefaultsToSelf);
	CHECK(addForce->FindPin("Then", VisualScriptPinDirection::In) == nullptr);
	CHECK(addForce->FindPin("Then", VisualScriptPinDirection::Out) != nullptr);

	VisualScriptNodeTemplate const* delay = library.FindNode("Flow.Delay");
	REQUIRE(delay != nullptr);
	CHECK(delay->Kind == VisualScriptNodeKind::Latent);
	CHECK(delay->Latent);
	CHECK(delay->FindPin("Completed", VisualScriptPinDirection::Out)->Role == VisualScriptPinRole::Continuation);
	CHECK(delay->FindPin("Duration", VisualScriptPinDirection::In)->Default == 0.2);
	VisualScriptNodeTemplate const* tween = library.FindNode("Flow.Tween");
	REQUIRE(tween != nullptr);
	CHECK(tween->FindPin("Tick", VisualScriptPinDirection::Out)->Role == VisualScriptPinRole::Tick);
	REQUIRE(tween->Settings.size() == 1);
	CHECK(tween->Settings[0].Kind == "Choice");
	CHECK(tween->Settings[0].Default == "Linear");
	CHECK(tween->Settings[0].Choices == std::vector<std::string>{"Linear", "EaseIn"});

	VisualScriptNodeTemplate const* sequence = library.FindNode("Flow.Sequence");
	REQUIRE(sequence != nullptr);
	CHECK(sequence->Kind == VisualScriptNodeKind::Flow);
	REQUIRE(sequence->Settings.size() == 2);
	VisualScriptSetting const& count = sequence->Settings[0];
	CHECK(count.Name == "Count");
	CHECK(count.Kind == "Int");
	CHECK(count.Default == 2);
	CHECK(count.Min == std::optional<int64_t>(2));
	CHECK(count.Max == std::optional<int64_t>(64));
	CHECK_FALSE(count.Required);
	VisualScriptSetting const& of = sequence->Settings[1];
	CHECK(of.Required);
	CHECK(of.Default.is_null());
	CHECK_FALSE(of.Min.has_value());
	CHECK(of.Constraint == "Enum");
	CHECK(of.Type == "Strada.KeyCode");

	VisualScriptNodeTemplate const* add = library.FindNode("Operator.Add");
	REQUIRE(add != nullptr);
	CHECK(add->Pure);
	VisualScriptPin const* a = add->FindPin("A", VisualScriptPinDirection::In);
	REQUIRE(a != nullptr);
	CHECK(a->Type == VisualScriptWildcardType);
	CHECK(a->Constraint == VisualScriptPinConstraint::Any);
	CHECK_FALSE(a->CarriesExec);
	VisualScriptNodeTemplate const* arrayGet = library.FindNode("Array.Get");
	REQUIRE(arrayGet != nullptr);
	CHECK(arrayGet->FindPin("Element", VisualScriptPinDirection::Out)->ElementOf == "Array");
	CHECK(arrayGet->FindPin("Array", VisualScriptPinDirection::In)->Constraint == VisualScriptPinConstraint::Array);
	// A Reroute's wildcards pass the flow too.
	VisualScriptNodeTemplate const* reroute = library.FindNode("Reroute");
	REQUIRE(reroute != nullptr);
	CHECK(reroute->Kind == VisualScriptNodeKind::Reroute);
	CHECK(reroute->FindPin("In", VisualScriptPinDirection::In)->CarriesExec);
	CHECK(reroute->FindPin("Out", VisualScriptPinDirection::Out)->CarriesExec);
	CHECK(reroute->FindPin("Out", VisualScriptPinDirection::Out)->Group == "T");

	// The category tree: children by name, nodes in library order, uncategorized nodes at the root.
	VisualScriptCategory const& root = library.GetCategories();
	CHECK(root.Path.empty());
	std::vector<std::string> names;
	for (VisualScriptCategory const& child : root.Children)
	{
		names.push_back(child.Name);
	}
	CHECK(names == std::vector<std::string>{"Components", "Flow", "Math", "Utilities"});
	CHECK(GetTypes(root.Nodes) == std::vector<std::string>{"Note"});
	VisualScriptCategory const* components = FindChild(root, "Components");
	REQUIRE(components != nullptr);
	CHECK(components->Nodes.empty());
	VisualScriptCategory const* rigidBody = FindChild(*components, "Rigid Body");
	REQUIRE(rigidBody != nullptr);
	CHECK(rigidBody->Path == "Components/Rigid Body");
	CHECK(GetTypes(rigidBody->Nodes) == std::vector<std::string>{addForce->Type});
	VisualScriptCategory const* flow = FindChild(root, "Flow");
	REQUIRE(flow != nullptr);
	CHECK(GetTypes(flow->Nodes) == std::vector<std::string>{"Flow.Delay", "Flow.Tween", "Flow.Sequence"});
	VisualScriptCategory const* utilities = FindChild(root, "Utilities");
	REQUIRE(utilities != nullptr);
	CHECK(GetTypes(utilities->Nodes) == std::vector<std::string>{"IsValid", "Reroute"});
	REQUIRE(FindChild(*utilities, "Array") != nullptr);
	CHECK(FindChild(*utilities, "Array")->Path == "Utilities/Array");
	CHECK(CountNodes(root) == library.GetNodes().size());

	// Search: title, keywords, category and type, ignoring the case of ASCII letters, in library order.
	CHECK(GetTypes(library.Search("ADD FORCE")) == std::vector<std::string>{addForce->Type});
	CHECK(GetTypes(library.Search("Knot")) == std::vector<std::string>{"Reroute"});
	CHECK(GetTypes(library.Search("rigid body")) == std::vector<std::string>{addForce->Type});
	CHECK(GetTypes(library.Search("flow.d")) == std::vector<std::string>{"Flow.Delay"});
	CHECK(GetTypes(library.Search("utilities/")) == std::vector<std::string>{"Array.Get"});
	CHECK(GetTypes(library.Search("add")) == std::vector<std::string>{addForce->Type, "Operator.Add"});
	CHECK(library.Search("").size() == library.GetNodes().size());
	CHECK(library.Search("no such node").empty());
}

TEST_CASE("VisualScriptLibrary: refuses JSON that is not a library, naming what is wrong")
{
	struct Case
	{
		std::string_view Json;
		std::string_view Error;
	};
	constexpr std::string_view Type = R"("ID": "int", "Title": "Integer", "Kind": "Integer", "ImplicitTo": [])";
	std::vector<Case> const cases = {
		{R"([])", "the document: expected an object"},
		{R"({ "Types": {}, "Nodes": [] })", "Types: expected an array"},
		{R"({ "Types": [] })", "Nodes: expected an array"},
		{R"({ "Types": [ 3 ], "Nodes": [] })", "Types[0]: expected an object"},
		{R"({ "Types": [ { "Title": "Integer", "Kind": "Integer", "ImplicitTo": [] } ], "Nodes": [] })", "Types[0].ID: expected a string"},
		{R"({ "Types": [ { "ID": "", "Title": "Integer", "Kind": "Integer", "ImplicitTo": [] } ], "Nodes": [] })",
	     "Types[0].ID: expected a string that is not empty"},
		{R"({ "Types": [ { "ID": "int", "Title": "Integer", "Kind": "Number", "ImplicitTo": [] } ], "Nodes": [] })",
	     "Types[0].Kind: expected one of Exec, Bool, Integer, Float, String, Vector, Quaternion, Color, Matrix, Entity, Component, Asset, "
	     "Enum, Struct, Class, Array, Object"},
		{R"({ "Types": [ { "ID": "int", "Title": "Integer", "Kind": "Integer" } ], "Nodes": [] })",
	     "Types[0].ImplicitTo: expected an array"},
		{R"({ "Types": [ { "ID": "int", "Title": "Integer", "Kind": "Integer", "ImplicitTo": [ 1 ] } ], "Nodes": [] })",
	     "Types[0].ImplicitTo: expected an array of strings"},
		{R"({ "Types": [ { "ID": "int", "Title": "Integer", "Kind": "Integer", "FieldType": "Number", "ImplicitTo": [] } ], "Nodes": [] })",
	     "Types[0].FieldType: Number is not a script field type"},
		{R"({ "Types": [ { "ID": "int", "Title": "Integer", "Kind": "Integer", "AssetType": "Picture", "ImplicitTo": [] } ], "Nodes": [] })",
	     "Types[0].AssetType: Picture is not an asset type"},
		{R"({ "Types": [ { "ID": "int", "Title": "Integer", "Kind": "Integer", "Literal": 1, "ImplicitTo": [] } ], "Nodes": [] })",
	     "Types[0].Literal: expected true or false"},
		{R"({ "Types": [ { "ID": "int", "Title": "Integer", "Kind": "Integer", "Enumerators": [], "ImplicitTo": [] } ], "Nodes": [] })",
	     "Types[0].Enumerators: only enums have enumerators"},
		{R"({ "Types": [ { "ID": "E", "Title": "E", "Kind": "Enum", "FieldType": "Float", "Enumerators": [], "Flags": false, "ImplicitTo": [] } ],
		     "Nodes": [] })",
	     "Types[0].FieldType: an enum is stored as Int32, UInt32, Int64 or UInt64"},
		{R"({ "Types": [ { "ID": "E", "Title": "E", "Kind": "Enum", "FieldType": "Int32", "Flags": false, "ImplicitTo": [] } ], "Nodes": [] })",
	     "Types[0].Enumerators: expected an array"},
		{R"({ "Types": [ { "ID": "E", "Title": "E", "Kind": "Enum", "FieldType": "Int32", "Enumerators": [ { "Name": "A", "Value": 3000000000 } ],
		     "Flags": false, "ImplicitTo": [] } ], "Nodes": [] })",
	     "Types[0].Enumerators[0].Value: "},
		{R"({ "Types": [ { "ID": "E", "Title": "E", "Kind": "Enum", "FieldType": "Int32", "Enumerators": [ { "Name": "A" } ], "Flags": false,
		     "ImplicitTo": [] } ], "Nodes": [] })",
	     "Types[0].Enumerators[0].Value: expected the enumerator's value"},
		{R"({ "Types": [ { "ID": "E", "Title": "E", "Kind": "Enum", "FieldType": "Int32", "Enumerators": [], "ImplicitTo": [] } ], "Nodes": [] })",
	     "Types[0].Flags: expected true or false"},
		{R"({ "Types": [ { "ID": "V", "Title": "V", "Kind": "Vector", "Fields": [ { "Name": "X" } ], "ImplicitTo": [] } ], "Nodes": [] })",
	     "Types[0].Fields[0].Type: expected a string"},
		{R"({ "Types": [], "Nodes": [ { "Title": "Branch", "Category": "Flow", "Keywords": [], "Kind": "Flow", "Pure": false, "Latent": false,
		     "Pins": [], "Settings": [] } ] })",
	     "Nodes[0].Type: expected a string"},
		{R"({ "Types": [], "Nodes": [ { "Type": "Flow.Branch", "Title": "Branch", "Category": "Flow", "Keywords": [], "Kind": "Flow",
		     "Pure": "no", "Latent": false, "Pins": [], "Settings": [] } ] })",
	     "Nodes[0].Pure: expected true or false"},
		{R"({ "Types": [], "Nodes": [ { "Type": "Flow.Branch", "Title": "Branch", "Category": "Flow", "Kind": "Flow", "Pure": false,
		     "Latent": false, "Pins": [], "Settings": [] } ] })",
	     "Nodes[0].Keywords: expected an array"},
		{R"({ "Types": [], "Nodes": [ { "Type": "Flow.Branch", "Title": "Branch", "Category": "Flow", "Keywords": [], "Kind": "Flow",
		     "Pure": false, "Latent": false, "Pins": [ { "Name": "Execute", "Direction": "Up", "Type": "exec" } ], "Settings": [] } ] })",
	     "Nodes[0].Pins[0].Direction: expected one of In, Out"},
		{R"({ "Types": [], "Nodes": [ { "Type": "Flow.Branch", "Title": "Branch", "Category": "Flow", "Keywords": [], "Kind": "Flow",
		     "Pure": false, "Latent": false, "Pins": [ { "Name": "Then", "Direction": "Out", "Type": "exec", "Role": "Later" } ],
		     "Settings": [] } ] })",
	     "Nodes[0].Pins[0].Role: expected one of None, Continuation, Tick"},
		{R"({ "Types": [], "Nodes": [ { "Type": "Flow.Branch", "Title": "Branch", "Category": "Flow", "Keywords": [], "Kind": "Flow",
		     "Pure": false, "Latent": false, "Pins": [], "Settings": [ { "Name": "Count", "Kind": "Int", "Min": 1.5 } ] } ] })",
	     "Nodes[0].Settings[0].Min: expected a 64-bit integer"},
		{R"({ "Types": [], "Nodes": [ { "Type": "Flow.Branch", "Title": "Branch", "Category": "Flow", "Keywords": [], "Kind": "Flow",
		     "Pure": false, "Latent": false, "Pins": [] } ] })",
	     "Nodes[0].Settings: expected an array"},
	};
	for (Case const& refused : cases)
	{
		CAPTURE(refused.Json);
		Result<Scope<VisualScriptLibrary>> library = VisualScriptLibrary::Parse(refused.Json);
		REQUIRE(library.IsError());
		CHECK(library.GetError().starts_with(refused.Error));
	}

	CHECK(VisualScriptLibrary::Parse("").IsError());
	CHECK(VisualScriptLibrary::Parse(R"({ "Types": [)").IsError());
	// A type or a node kind described twice.
	std::string const twice = fmt::format(R"({{ "Types": [ {{ {0} }}, {{ {0} }} ], "Nodes": [] }})", Type);
	Result<Scope<VisualScriptLibrary>> typeTwice = VisualScriptLibrary::Parse(twice);
	REQUIRE(typeTwice.IsError());
	CHECK(typeTwice.GetError() == "Types: int is described twice");
	constexpr std::string_view Node =
		R"({ "Type": "Flow.Branch", "Title": "Branch", "Category": "Flow", "Keywords": [], "Kind": "Flow", "Pure": false, "Latent": false, "Pins": [], "Settings": [] })";
	Result<Scope<VisualScriptLibrary>> nodeTwice =
		VisualScriptLibrary::Parse(fmt::format(R"({{ "Types": [], "Nodes": [ {0}, {0} ] }})", Node));
	REQUIRE(nodeTwice.IsError());
	CHECK(nodeTwice.GetError() == "Nodes: Flow.Branch is described twice");
	// Keys this version does not know are left alone: the runtime may describe more.
	CHECK(VisualScriptLibrary::Parse(R"({ "Types": [], "Nodes": [], "Version": 2 })").IsOk());
}

TEST_CASE("VisualScriptLibrary: CanConnect follows the type rules and the wildcards' constraints")
{
	Result<Scope<VisualScriptLibrary>> parsed = VisualScriptLibrary::Parse(LibraryJson);
	VisualScriptLibrary const& library = RequireLibrary(parsed);
	auto const connects = [&library](std::string const& output, std::string const& input)
	{
		return library.CanConnect(MakePin(output, VisualScriptPinDirection::Out), MakePin(input, VisualScriptPinDirection::In));
	};
	auto const pin = [&library](std::string_view node, std::string_view name, VisualScriptPinDirection direction)
	{
		VisualScriptNodeTemplate const* found = library.FindNode(node);
		REQUIRE(found != nullptr);
		VisualScriptPin const* result = found->FindPin(name, direction);
		REQUIRE(result != nullptr);
		return *result;
	};

	// The same type, exec only to exec.
	CHECK(connects("exec", "exec"));
	CHECK(connects("Strada.Entity", "Strada.Entity"));
	CHECK_FALSE(connects("exec", "int"));
	CHECK_FALSE(connects("int", "exec"));
	CHECK_FALSE(connects("exec", "string"));
	// The conversions the library lists, one way only.
	CHECK(connects("int", "long"));
	CHECK(connects("int", "float"));
	CHECK_FALSE(connects("long", "int"));
	CHECK_FALSE(connects("float", "int"));
	CHECK(connects("Strada.Material", "Strada.Asset"));
	CHECK_FALSE(connects("Strada.Asset", "Strada.Material"));
	CHECK(connects("Game.Layers", "System.Object"));
	CHECK_FALSE(connects("Game.Layers", "int"));
	CHECK_FALSE(connects("Strada.Vector3", "float"));
	// Any value to text; a type the library does not know converts to nothing else.
	CHECK(connects("Strada.Vector3", "string"));
	CHECK(connects("Game.Unknown", "string"));
	CHECK_FALSE(connects("Game.Unknown", "System.Object"));

	VisualScriptPin const addA = pin("Operator.Add", "A", VisualScriptPinDirection::In);
	VisualScriptPin const addResult = pin("Operator.Add", "Result", VisualScriptPinDirection::Out);
	VisualScriptPin const isValid = pin("IsValid", "Value", VisualScriptPinDirection::In);
	VisualScriptPin const array = pin("Array.Get", "Array", VisualScriptPinDirection::In);
	VisualScriptPin const element = pin("Array.Get", "Element", VisualScriptPinDirection::Out);
	VisualScriptPin const rerouteIn = pin("Reroute", "In", VisualScriptPinDirection::In);
	VisualScriptPin const rerouteOut = pin("Reroute", "Out", VisualScriptPinDirection::Out);
	VisualScriptPin const execOut = MakePin("exec", VisualScriptPinDirection::Out);
	VisualScriptPin const execIn = MakePin("exec", VisualScriptPinDirection::In);

	// A wildcard input takes what its constraint accepts, never exec unless it is a Reroute's.
	CHECK(library.CanConnect(MakePin("int", VisualScriptPinDirection::Out), addA));
	CHECK(library.CanConnect(MakePin("Strada.Vector3", VisualScriptPinDirection::Out), addA));
	CHECK_FALSE(library.CanConnect(execOut, addA));
	CHECK(library.CanConnect(MakePin("Strada.Entity", VisualScriptPinDirection::Out), isValid));
	CHECK(library.CanConnect(MakePin("Strada.Material", VisualScriptPinDirection::Out), isValid));
	CHECK(library.CanConnect(MakePin("System.Object", VisualScriptPinDirection::Out), isValid));
	CHECK_FALSE(library.CanConnect(MakePin("float", VisualScriptPinDirection::Out), isValid));
	CHECK_FALSE(library.CanConnect(MakePin("string", VisualScriptPinDirection::Out), isValid));
	CHECK(library.CanConnect(MakePin("Strada.Entity[]", VisualScriptPinDirection::Out), array));
	CHECK_FALSE(library.CanConnect(MakePin("Strada.Entity", VisualScriptPinDirection::Out), array));
	CHECK(library.CanConnect(execOut, rerouteIn));
	CHECK(library.CanConnect(MakePin("float", VisualScriptPinDirection::Out), rerouteIn));
	// A wildcard output goes to an input its constraint accepts, or to another wildcard.
	CHECK(library.CanConnect(addResult, MakePin("float", VisualScriptPinDirection::In)));
	CHECK(library.CanConnect(element, MakePin("Strada.Entity", VisualScriptPinDirection::In)));
	CHECK(library.CanConnect(addResult, addA));
	CHECK(library.CanConnect(rerouteOut, rerouteIn));
	CHECK_FALSE(library.CanConnect(addResult, execIn));
	CHECK(library.CanConnect(rerouteOut, execIn));
	VisualScriptPin reference = MakePin("*", VisualScriptPinDirection::Out);
	reference.Constraint = VisualScriptPinConstraint::Reference;
	CHECK(library.CanConnect(reference, MakePin("Strada.Entity", VisualScriptPinDirection::In)));
	CHECK_FALSE(library.CanConnect(reference, MakePin("float", VisualScriptPinDirection::In)));
	CHECK_FALSE(library.CanConnect(reference, MakePin("string", VisualScriptPinDirection::In)));

	// The constraints themselves.
	CHECK(library.Satisfies("int", VisualScriptPinConstraint::Numeric));
	CHECK(library.Satisfies("float", VisualScriptPinConstraint::Numeric));
	CHECK_FALSE(library.Satisfies("Strada.Vector3", VisualScriptPinConstraint::Numeric));
	CHECK(library.Satisfies("Strada.Entity[]", VisualScriptPinConstraint::Reference));
	CHECK_FALSE(library.Satisfies("Game.Layers", VisualScriptPinConstraint::Reference));
	CHECK_FALSE(library.Satisfies("Game.Unknown", VisualScriptPinConstraint::Array));
	CHECK(library.Satisfies("Game.Unknown", VisualScriptPinConstraint::Any));
	CHECK_FALSE(library.Satisfies("exec", VisualScriptPinConstraint::Any));
	CHECK_FALSE(library.Satisfies("*", VisualScriptPinConstraint::None));
}

TEST_CASE("VisualScriptLibrary: reads analyses and diagnostics, and describes their errors")
{
	constexpr std::string_view AnalysisJson = R"json({
	"Nodes": [
		{ "ID": 11, "Function": 0, "Type": "Call:Strada.Input.IsKeyDown(Strada.KeyCode)", "Resolved": "Exact", "Title": "Is Key Down",
		  "Subtitle": "Input", "Kind": "Pure", "Pure": true, "Latent": false,
		  "Pins": [ { "Name": "Key", "Direction": "In", "Type": "Strada.KeyCode", "Default": "Space" },
		            { "Name": "Result", "Direction": "Out", "Type": "bool" } ] },
		{ "ID": 14, "Function": 3, "Type": "Call:Strada.Input.IsKeyDown", "Resolved": "Unique",
		  "CanonicalType": "Call:Strada.Input.IsKeyDown(Strada.KeyCode)",
		  "PinRenames": [ { "Direction": "In", "From": "Code", "To": "Key" } ], "Title": "Is Key Down", "Kind": "Pure", "Pure": true,
		  "Latent": false, "Pins": [] },
		{ "ID": 15, "Function": 0, "Type": "Sample.Missing", "Resolved": "Unknown", "Title": "Missing", "Kind": "Unknown", "Pure": false,
		  "Latent": false, "Pins": [ { "Name": "Value", "Direction": "In", "Type": "*" } ] },
		{ "ID": 16, "Function": 0, "Type": "Reroute", "Resolved": "Exact", "Title": "Reroute", "Kind": "Reroute", "Pure": true, "Latent": false,
		  "Pins": [ { "Name": "In", "Direction": "In", "Type": "*", "Constraint": "Any", "Group": "T" },
		            { "Name": "Out", "Direction": "Out", "Type": "int", "Constraint": "Any", "Group": "T" } ] }
	],
	"Diagnostics": [
		{ "Severity": "Warning", "Code": "NodeUpdated", "Function": 3, "Node": 14, "Message": "node updated" },
		{ "Severity": "Error", "Code": "UnknownPin", "Function": 3, "Node": 33, "Pin": "Valeu", "Direction": "In", "Message": "no pin Valeu",
		  "Suggestions": ["Value"] },
		{ "Severity": "Error", "Code": "TypeMismatch", "Function": 0, "Link": { "From": [10, "Then"], "To": [12, "Condition"] },
		  "Message": "exec into data" },
		{ "Severity": "Error", "Code": "NotExposable", "Function": 0, "Variable": 4, "Message": "not exposable" }
	]
})json";
	Result<VisualScriptAnalysis> parsed = VisualScriptAnalysis::Parse(AnalysisJson);
	REQUIRE_MESSAGE(parsed.IsOk(), parsed.GetError());
	VisualScriptAnalysis const& analysis = parsed.GetValue();
	CHECK(analysis.Source.at("Nodes").size() == 4);

	REQUIRE(analysis.Nodes.size() == 4);
	VisualScriptAnalyzedNode const* exact = analysis.FindNode(11);
	REQUIRE(exact != nullptr);
	CHECK(exact->Function == 0);
	CHECK(exact->Resolved == VisualScriptResolution::Exact);
	CHECK(exact->CanonicalType.empty());
	CHECK(exact->Subtitle == "Input");
	CHECK(exact->Kind == VisualScriptNodeKind::Pure);
	CHECK(exact->FindPin("Key", VisualScriptPinDirection::In)->Default == "Space");
	VisualScriptAnalyzedNode const* unique = analysis.FindNode(14);
	REQUIRE(unique != nullptr);
	CHECK(unique->Function == 3);
	CHECK(unique->Resolved == VisualScriptResolution::Unique);
	CHECK(unique->CanonicalType == "Call:Strada.Input.IsKeyDown(Strada.KeyCode)");
	REQUIRE(unique->PinRenames.size() == 1);
	CHECK(unique->PinRenames[0].Direction == VisualScriptPinDirection::In);
	CHECK(unique->PinRenames[0].From == "Code");
	CHECK(unique->PinRenames[0].To == "Key");
	VisualScriptAnalyzedNode const* unknown = analysis.FindNode(15);
	REQUIRE(unknown != nullptr);
	CHECK(unknown->Resolved == VisualScriptResolution::Unknown);
	CHECK(unknown->Kind == VisualScriptNodeKind::Unknown);
	CHECK(unknown->FindPin("Value", VisualScriptPinDirection::In)->Constraint == VisualScriptPinConstraint::None);
	// A Reroute's pins pass the flow while they are wildcards.
	VisualScriptAnalyzedNode const* reroute = analysis.FindNode(16);
	REQUIRE(reroute != nullptr);
	CHECK(reroute->FindPin("In", VisualScriptPinDirection::In)->CarriesExec);
	CHECK_FALSE(reroute->FindPin("Out", VisualScriptPinDirection::Out)->CarriesExec);
	CHECK(analysis.FindNode(99) == nullptr);

	REQUIRE(analysis.Diagnostics.size() == 4);
	VisualScriptDiagnostic const& updated = analysis.Diagnostics[0];
	CHECK(updated.Severity == VisualScriptSeverity::Warning);
	CHECK(updated.Code == "NodeUpdated");
	CHECK(updated.Node == std::optional<uint32_t>(14));
	CHECK_FALSE(updated.Link.has_value());
	VisualScriptDiagnostic const& pin = analysis.Diagnostics[1];
	CHECK(pin.Pin == "Valeu");
	CHECK(pin.Direction == std::optional<VisualScriptPinDirection>(VisualScriptPinDirection::In));
	CHECK(pin.Suggestions == std::vector<std::string>{"Value"});
	VisualScriptDiagnostic const& mismatch = analysis.Diagnostics[2];
	CHECK(mismatch.Link == std::optional<VisualScriptLink>(MakeLink(10, "Then", 12, "Condition")));
	CHECK_FALSE(mismatch.Node.has_value());
	CHECK(analysis.Diagnostics[3].Variable == std::optional<uint32_t>(4));

	// The errors, described from the first one and where it is.
	CHECK(CountVisualScriptErrors(analysis.Diagnostics) == 3);
	CHECK(DescribeVisualScriptErrors(analysis.Diagnostics) == "3 errors; first: no pin Valeu (node 33)");
	CHECK(DescribeVisualScriptErrors(std::span(analysis.Diagnostics).subspan(3)) == "1 error; first: not exposable (variable 4)");
	CHECK(DescribeVisualScriptErrors(std::span(analysis.Diagnostics).subspan(2, 1)) == "1 error; first: exec into data");
	CHECK(DescribeVisualScriptErrors(std::span(analysis.Diagnostics).first(1)).empty());

	// VisualScriptLoad's answer: the diagnostics array alone.
	Result<std::vector<VisualScriptDiagnostic>> none = ParseVisualScriptDiagnostics("[]");
	REQUIRE(none.IsOk());
	CHECK(none.GetValue().empty());
	Result<std::vector<VisualScriptDiagnostic>> one = ParseVisualScriptDiagnostics(
		R"([ { "Severity": "Error", "Code": "InvalidDocument", "Function": 0, "Message": "not an object" } ])");
	REQUIRE(one.IsOk());
	REQUIRE(one.GetValue().size() == 1);
	CHECK(one.GetValue()[0].Code == "InvalidDocument");

	// What is not of the form is refused, naming the member.
	struct Case
	{
		std::string_view Json;
		std::string_view Error;
	};
	std::vector<Case> const cases = {
		{R"({ "Nodes": [] })", "Diagnostics: expected an array"},
		{R"({ "Nodes": [], "Diagnostics": {} })", "Diagnostics: expected an array"},
		{R"({ "Nodes": [ { "ID": -1, "Function": 0, "Type": "Self", "Resolved": "Exact", "Title": "Self", "Kind": "Data", "Pure": true,
		     "Latent": false, "Pins": [] } ], "Diagnostics": [] })",
	     "Nodes[0].ID: expected an integer of 0 to 4294967295"},
		{R"({ "Nodes": [ { "ID": 1, "Function": 0, "Type": "Self", "Resolved": "Redirect", "Title": "Self", "Kind": "Data", "Pure": true,
		     "Latent": false, "Pins": [] } ], "Diagnostics": [] })",
	     "Nodes[0].CanonicalType: expected a string"},
		{R"({ "Nodes": [ { "ID": 1, "Function": 0, "Type": "Self", "Resolved": "Redirect", "CanonicalType": "Self", "PinRenames": [ {} ],
		     "Title": "Self", "Kind": "Data", "Pure": true, "Latent": false, "Pins": [] } ], "Diagnostics": [] })",
	     "Nodes[0].PinRenames[0].Direction: expected one of In, Out"},
		{R"({ "Nodes": [], "Diagnostics": [ { "Severity": "Fatal", "Code": "X", "Function": 0, "Message": "" } ] })",
	     "Diagnostics[0].Severity: expected one of Error, Warning"},
		{R"({ "Nodes": [], "Diagnostics": [ { "Severity": "Error", "Code": "X", "Function": 0, "Link": { "From": [1], "To": [2, "In"] },
		     "Message": "" } ] })",
	     "Diagnostics[0].Link.From: expected [node, pin]"},
		{R"({ "Nodes": [], "Diagnostics": [ { "Severity": "Error", "Code": "X", "Function": 0, "Node": "4", "Message": "" } ] })",
	     "Diagnostics[0].Node: expected an integer of 0 to 4294967295"},
	};
	for (Case const& refused : cases)
	{
		CAPTURE(refused.Json);
		Result<VisualScriptAnalysis> read = VisualScriptAnalysis::Parse(refused.Json);
		REQUIRE(read.IsError());
		CHECK(read.GetError() == refused.Error);
	}
	Result<std::vector<VisualScriptDiagnostic>> object = ParseVisualScriptDiagnostics("{}");
	REQUIRE(object.IsError());
	CHECK(object.GetError() == "the document: expected an array");
	Result<std::vector<VisualScriptDiagnostic>> element = ParseVisualScriptDiagnostics("[ 1 ]");
	REQUIRE(element.IsError());
	CHECK(element.GetError() == "[0]: expected an object");
}

TEST_CASE("VisualScriptLibrary: reads where the flow leaves a disabled node, which the analysis leaves out where it ends")
{
	// A disabled Delay continues at Completed; the flow through a disabled Print ends there, and an enabled Branch has no
	// pass-through.
	constexpr std::string_view AnalysisJson = R"json({
	"Nodes": [
		{ "ID": 1, "Function": 0, "Type": "Flow.Delay", "Resolved": "Exact", "Title": "Delay", "Kind": "Latent", "Pure": false,
		  "Latent": true, "Pins": [], "PassThrough": "Completed" },
		{ "ID": 2, "Function": 0, "Type": "Game.Print", "Resolved": "Unknown", "Title": "Print", "Kind": "Unknown", "Pure": false,
		  "Latent": false, "Pins": [] },
		{ "ID": 3, "Function": 0, "Type": "Flow.Branch", "Resolved": "Exact", "Title": "Branch", "Kind": "Flow", "Pure": false,
		  "Latent": false, "Pins": [] }
	],
	"Diagnostics": []
})json";
	Result<VisualScriptAnalysis> parsed = VisualScriptAnalysis::Parse(AnalysisJson);
	REQUIRE_MESSAGE(parsed.IsOk(), parsed.GetError());
	VisualScriptAnalysis const& analysis = parsed.GetValue();
	REQUIRE(analysis.Nodes.size() == 3);
	CHECK(analysis.Nodes[0].PassThrough == "Completed");
	CHECK(analysis.Nodes[1].PassThrough.empty());
	CHECK(analysis.Nodes[2].PassThrough.empty());

	// Present, it is a string.
	Result<VisualScriptAnalysis> refused = VisualScriptAnalysis::Parse(
		R"({ "Nodes": [ { "ID": 1, "Function": 0, "Type": "Flow.Delay", "Resolved": "Exact", "Title": "Delay", "Kind": "Latent",
		     "Pure": false, "Latent": true, "Pins": [], "PassThrough": 1 } ], "Diagnostics": [] })");
	REQUIRE(refused.IsError());
	CHECK(refused.GetError() == "Nodes[0].PassThrough: expected a string");
}

namespace
{
	// The texts the scripting runtime writes for slice 3's vocabulary, byte for byte as its writers write them (compact, members
	// in their order). The runtime's own writer tests pin the same texts, so the two sides agree on each byte.
	constexpr std::string_view NoteText =
		R"({"Member":"TestGame.Probe.Swap","Category":"Game/Probe","Message":"TestGame.Probe.Swap: [VisualNode] member has no node form: ref parameters have no pins"})";
	// "a * b + sni(t)": the unknown function's name is bytes 8 up to 11 (Span is [start, end), the end exclusive).
	constexpr std::string_view FormulaDiagnosticText =
		R"({"Severity":"Error","Code":"InvalidExpression","Function":0,"Node":12,"Setting":"Formula","Span":[8,11],"Message":"unknown function sni: did you mean sin?","Suggestions":["sin","sign","min"]})";
	// A finding about another graph names it, and suggests IDs from it.
	constexpr std::string_view RemoteDiagnosticText =
		R"({"Severity":"Error","Code":"UnknownVariable","Function":0,"Node":7,"Graph":"8146390251172604461","Message":"Scripts/Door.sgraph has no variable 9","Suggestions":["3","4"]})";
	// Every new member at once, in its place after Variable.
	constexpr std::string_view FullDiagnosticText =
		R"({"Severity":"Warning","Code":"UnknownEvent","Function":3,"Node":12,"Variable":4,"Setting":"Name","Span":[0,3],"Graph":"8146390251172604461","Message":"Scripts/Door.sgraph has no event Opn: did you mean Open?","Suggestions":["Open"]})";
}

TEST_CASE("VisualScriptLibrary: reads slice 3's vocabulary: Expression constraints, ArrayOf, game nodes, notes and new setting kinds")
{
	std::string const json = fmt::format(R"json({{
	"Types": [
		{{ "ID": "bool", "Title": "Boolean", "Kind": "Bool", "Literal": true, "ImplicitTo": [] }},
		{{ "ID": "int", "Title": "Integer", "Kind": "Integer", "Literal": true, "ImplicitTo": ["float"] }},
		{{ "ID": "double", "Title": "Double", "Kind": "Float", "Literal": true, "ImplicitTo": [] }},
		{{ "ID": "float", "Title": "Float", "Kind": "Float", "Literal": true, "ImplicitTo": [] }},
		{{ "ID": "string", "Title": "String", "Kind": "String", "Literal": true, "ImplicitTo": [] }},
		{{ "ID": "float[]", "Title": "Float Array", "Kind": "Array", "Element": "float", "ImplicitTo": [] }},
		{{ "ID": "Strada.Vector2", "Title": "Vector2", "Kind": "Vector", "Literal": true, "ImplicitTo": [] }},
		{{ "ID": "Strada.Vector3", "Title": "Vector3", "Kind": "Vector", "Literal": true, "ImplicitTo": [] }},
		{{ "ID": "Strada.Vector4", "Title": "Vector4", "Kind": "Vector", "Literal": true, "ImplicitTo": [] }},
		{{ "ID": "Strada.Quaternion", "Title": "Quaternion", "Kind": "Quaternion", "Literal": true, "ImplicitTo": [] }},
		{{ "ID": "Strada.Color", "Title": "Color", "Kind": "Color", "Literal": true, "ImplicitTo": [] }},
		{{ "ID": "Strada.Entity", "Title": "Entity", "Kind": "Entity", "Literal": true, "ImplicitTo": [] }}
	],
	"Nodes": [
		{{ "Type": "Expression", "Title": "Expression", "Category": "Math", "Keywords": ["formula"], "Kind": "Data", "Pure": true,
		  "Latent": false, "Pins": [
			{{ "Name": "a", "Direction": "In", "Type": "*", "Constraint": "Expression" }},
			{{ "Name": "Result", "Direction": "Out", "Type": "*", "Constraint": "Expression" }} ],
		  "Settings": [ {{ "Name": "Formula", "Kind": "Expression", "Default": "a", "Required": true }} ] }},
		{{ "Type": "Array.Make", "Title": "Make Array", "Category": "Utilities/Array", "Keywords": [], "Kind": "Data", "Pure": true,
		  "Latent": false, "Pins": [
			{{ "Name": "Element0", "Direction": "In", "Type": "*", "Constraint": "Any", "Group": "T" }},
			{{ "Name": "Array", "Direction": "Out", "Type": "*", "Constraint": "Array", "ArrayOf": "Element0" }} ],
		  "Settings": [ {{ "Name": "Count", "Kind": "Int", "Default": 1, "Min": 0, "Max": 64 }} ] }},
		{{ "Type": "Flow.Timeline", "Title": "Timeline", "Category": "Flow", "Keywords": [], "Kind": "Latent", "Pure": false, "Latent": true,
		  "Pins": [], "Settings": [ {{ "Name": "Timeline", "Kind": "Timeline" }} ] }},
		{{ "Type": "Function.Call", "Title": "Call Function", "Category": "Functions", "Keywords": [], "Kind": "Function", "Pure": false,
		  "Latent": false, "Pins": [],
		  "Settings": [ {{ "Name": "Function", "Kind": "Function", "Required": true }}, {{ "Name": "Graph", "Kind": "Graph", "Constraint": "Library" }} ] }},
		{{ "Type": "Call:TestGame.Probe.Scan(float)", "Title": "Scan", "Category": "Game/Probe", "Keywords": [], "Kind": "Call", "Pure": false,
		  "Latent": false, "Game": true, "Pins": [], "Settings": [] }},
		{{ "Type": "Call:Strada.Log.Info(string)", "Title": "Info", "Category": "Log", "Keywords": [], "Kind": "Call", "Pure": false,
		  "Latent": false, "Game": false, "Pins": [], "Settings": [] }}
	],
	"Notes": [ {0}, {{ "Member": "TestGame.Probe.Wait", "Category": "", "Message": "" }} ]
}})json",
	                                     NoteText);
	Result<Scope<VisualScriptLibrary>> parsed = VisualScriptLibrary::Parse(json);
	VisualScriptLibrary const& library = RequireLibrary(parsed);

	VisualScriptNodeTemplate const* expression = library.FindNode("Expression");
	REQUIRE(expression != nullptr);
	CHECK(expression->FindPin("a", VisualScriptPinDirection::In)->Constraint == VisualScriptPinConstraint::Expression);
	CHECK(expression->FindPin("Result", VisualScriptPinDirection::Out)->Constraint == VisualScriptPinConstraint::Expression);
	REQUIRE(expression->Settings.size() == 1);
	CHECK(expression->Settings[0].Kind == "Expression");
	CHECK_FALSE(expression->Game);

	VisualScriptNodeTemplate const* make = library.FindNode("Array.Make");
	REQUIRE(make != nullptr);
	CHECK(make->FindPin("Array", VisualScriptPinDirection::Out)->ArrayOf == "Element0");
	CHECK(make->FindPin("Array", VisualScriptPinDirection::Out)->ElementOf.empty());
	CHECK(make->FindPin("Element0", VisualScriptPinDirection::In)->ArrayOf.empty());

	VisualScriptNodeTemplate const* timeline = library.FindNode("Flow.Timeline");
	REQUIRE(timeline != nullptr);
	REQUIRE(timeline->Settings.size() == 1);
	CHECK(timeline->Settings[0].Kind == "Timeline");
	CHECK(timeline->Settings[0].Default.is_null());
	VisualScriptNodeTemplate const* call = library.FindNode("Function.Call");
	REQUIRE(call != nullptr);
	REQUIRE(call->Settings.size() == 2);
	CHECK(call->Settings[1].Kind == "Graph");
	CHECK(call->Settings[1].Constraint == "Library");

	// Nodes of the game's members say so; the others are not, whether they say it or not.
	VisualScriptNodeTemplate const* scan = library.FindNode("Call:TestGame.Probe.Scan(float)");
	REQUIRE(scan != nullptr);
	CHECK(scan->Game);
	REQUIRE(library.FindNode("Call:Strada.Log.Info(string)") != nullptr);
	CHECK_FALSE(library.FindNode("Call:Strada.Log.Info(string)")->Game);

	// The notes in the runtime's order; the pinned text is the runtime's compact form.
	REQUIRE(library.GetNotes().size() == 2);
	CHECK(library.GetNotes()[0] ==
	      VisualScriptLibraryNote{"TestGame.Probe.Swap", "Game/Probe",
	                              "TestGame.Probe.Swap: [VisualNode] member has no node form: ref parameters have no pins"});
	CHECK(library.GetNotes()[1] == VisualScriptLibraryNote{"TestGame.Probe.Wait", "", ""});
	Result<Json> const note = ParseJson(NoteText);
	REQUIRE(note.IsOk());
	CHECK(note.GetValue().dump() == NoteText);
	// A library without notes has none.
	Result<Scope<VisualScriptLibrary>> plain = VisualScriptLibrary::Parse(LibraryJson);
	CHECK(RequireLibrary(plain).GetNotes().empty());

	// The Expression constraint: bool, integer and float types, and the three vectors; nothing else, and no unknown type.
	for (std::string_view const type : {"bool", "int", "double", "float", "Strada.Vector2", "Strada.Vector3", "Strada.Vector4"})
	{
		CAPTURE(type);
		CHECK(library.Satisfies(type, VisualScriptPinConstraint::Expression));
	}
	for (std::string_view const type :
	     {"string", "float[]", "Strada.Quaternion", "Strada.Color", "Strada.Entity", "Strada.Vector5", "exec", "*", ""})
	{
		CAPTURE(type);
		CHECK_FALSE(library.Satisfies(type, VisualScriptPinConstraint::Expression));
	}
	VisualScriptPin const input = *expression->FindPin("a", VisualScriptPinDirection::In);
	VisualScriptPin output = MakePin("Strada.Vector3", VisualScriptPinDirection::Out);
	CHECK(library.CanConnect(output, input));
	output.Type = "string";
	CHECK_FALSE(library.CanConnect(output, input));

	// What is not of the form is refused, naming the member.
	struct Case
	{
		std::string Json;
		std::string_view Error;
	};
	constexpr std::string_view Node =
		R"("Type": "Flow.Branch", "Title": "Branch", "Category": "Flow", "Keywords": [], "Kind": "Flow", "Pure": false, "Latent": false)";
	std::vector<Case> const cases = {
		{R"({ "Types": [], "Nodes": [], "Notes": {} })", "Notes: expected an array"},
		{R"({ "Types": [], "Nodes": [], "Notes": [ 3 ] })", "Notes[0]: expected an object"},
		{R"({ "Types": [], "Nodes": [], "Notes": [ { "Category": "", "Message": "" } ] })", "Notes[0].Member: expected a string"},
		{R"({ "Types": [], "Nodes": [], "Notes": [ { "Member": "M", "Message": "" } ] })", "Notes[0].Category: expected a string"},
		{R"({ "Types": [], "Nodes": [], "Notes": [ { "Member": "M", "Category": "", "Message": 1 } ] })",
	     "Notes[0].Message: expected a string"},
		{fmt::format(R"({{ "Types": [], "Nodes": [ {{ {}, "Game": 1, "Pins": [], "Settings": [] }} ] }})", Node),
	     "Nodes[0].Game: expected true or false"},
		{fmt::format(R"({{ "Types": [], "Nodes": [ {{ {}, "Pins": [ {{ "Name": "A", "Direction": "Out", "Type": "*", "ArrayOf": 3 }} ],
		    "Settings": [] }} ] }})",
	                 Node),
	     "Nodes[0].Pins[0].ArrayOf: expected a string"},
		{fmt::format(
			 R"({{ "Types": [], "Nodes": [ {{ {}, "Pins": [ {{ "Name": "A", "Direction": "In", "Type": "*", "Constraint": "Formula" }} ],
		    "Settings": [] }} ] }})",
			 Node),
	     "Nodes[0].Pins[0].Constraint: expected one of None, Any, Numeric, Reference, Array, Expression"},
	};
	for (Case const& refused : cases)
	{
		CAPTURE(refused.Json);
		Result<Scope<VisualScriptLibrary>> read = VisualScriptLibrary::Parse(refused.Json);
		REQUIRE(read.IsError());
		CHECK(read.GetError() == refused.Error);
	}
}

TEST_CASE("VisualScriptLibrary: reads the setting, span and graph of diagnostics, and pin renames of a node of any resolution")
{
	std::string const json = fmt::format(R"json({{
	"Nodes": [
		{{ "ID": 12, "Function": 0, "Type": "Function.Call", "Resolved": "Exact",
		  "PinRenames": [ {{ "Direction": "In", "From": "Value", "To": "Amount" }}, {{ "Direction": "Out", "From": "Out", "To": "Result" }} ],
		  "Title": "Clamp01", "Kind": "Function", "Pure": true, "Latent": false, "Pins": [] }},
		{{ "ID": 13, "Function": 0, "Type": "Function.Call", "Resolved": "Exact", "Title": "Report", "Kind": "Function", "Pure": false,
		  "Latent": false, "Pins": [] }},
		{{ "ID": 14, "Function": 0, "Type": "Sample.Missing", "Resolved": "Unknown", "PinRenames": [],
		  "Title": "Missing", "Kind": "Unknown", "Pure": false, "Latent": false, "Pins": [] }}
	],
	"Diagnostics": [ {}, {}, {}, {{ "Severity": "Error", "Code": "UnknownPin", "Function": 0, "Node": 3, "Message": "no pin" }} ]
}})json",
	                                     FormulaDiagnosticText, RemoteDiagnosticText, FullDiagnosticText);
	Result<VisualScriptAnalysis> parsed = VisualScriptAnalysis::Parse(json);
	REQUIRE_MESSAGE(parsed.IsOk(), parsed.GetError());
	VisualScriptAnalysis const& analysis = parsed.GetValue();

	// An exact node keeps its type and has renames; one without them has none.
	VisualScriptAnalyzedNode const* clamp = analysis.FindNode(12);
	REQUIRE(clamp != nullptr);
	CHECK(clamp->Resolved == VisualScriptResolution::Exact);
	CHECK(clamp->CanonicalType.empty());
	REQUIRE(clamp->PinRenames.size() == 2);
	CHECK(clamp->PinRenames[0].Direction == VisualScriptPinDirection::In);
	CHECK(clamp->PinRenames[0].From == "Value");
	CHECK(clamp->PinRenames[0].To == "Amount");
	CHECK(clamp->PinRenames[1].Direction == VisualScriptPinDirection::Out);
	CHECK(clamp->PinRenames[1].To == "Result");
	REQUIRE(analysis.FindNode(13) != nullptr);
	CHECK(analysis.FindNode(13)->PinRenames.empty());
	REQUIRE(analysis.FindNode(14) != nullptr);
	CHECK(analysis.FindNode(14)->PinRenames.empty());

	REQUIRE(analysis.Diagnostics.size() == 4);
	VisualScriptDiagnostic const& formula = analysis.Diagnostics[0];
	CHECK(formula.Code == "InvalidExpression");
	CHECK(formula.Node == std::optional<uint32_t>(12));
	CHECK(formula.Setting == "Formula");
	CHECK(formula.Span == std::optional<std::pair<uint32_t, uint32_t>>(std::pair(8u, 11u)));
	CHECK_FALSE(formula.Graph.IsValid());
	CHECK(formula.Message == "unknown function sni: did you mean sin?");
	CHECK(formula.Suggestions == std::vector<std::string>{"sin", "sign", "min"});
	VisualScriptDiagnostic const& remote = analysis.Diagnostics[1];
	CHECK(remote.Code == "UnknownVariable");
	CHECK(remote.Graph == AssetHandle(UUID(8146390251172604461ull)));
	CHECK(remote.Setting.empty());
	CHECK_FALSE(remote.Span.has_value());
	CHECK_FALSE(remote.Variable.has_value());
	CHECK(remote.Suggestions == std::vector<std::string>{"3", "4"});
	VisualScriptDiagnostic const& full = analysis.Diagnostics[2];
	CHECK(full.Severity == VisualScriptSeverity::Warning);
	CHECK(full.Function == 3);
	CHECK(full.Variable == std::optional<uint32_t>(4));
	CHECK(full.Setting == "Name");
	CHECK(full.Span == std::optional<std::pair<uint32_t, uint32_t>>(std::pair(0u, 3u)));
	CHECK(full.Graph == AssetHandle(UUID(8146390251172604461ull)));
	CHECK(full.Message == "Scripts/Door.sgraph has no event Opn: did you mean Open?");
	// Without them: no setting, no span, no graph.
	VisualScriptDiagnostic const& plain = analysis.Diagnostics[3];
	CHECK(plain.Setting.empty());
	CHECK_FALSE(plain.Span.has_value());
	CHECK_FALSE(plain.Graph.IsValid());

	// The pinned texts are the runtime's compact form, member for member.
	Json const& diagnostics = analysis.Source.at("Diagnostics");
	CHECK(diagnostics[0].dump() == FormulaDiagnosticText);
	CHECK(diagnostics[1].dump() == RemoteDiagnosticText);
	CHECK(diagnostics[2].dump() == FullDiagnosticText);
	// VisualScriptLoad's answer reads them alike.
	Result<std::vector<VisualScriptDiagnostic>> loaded = ParseVisualScriptDiagnostics(fmt::format("[{}]", FullDiagnosticText));
	REQUIRE_MESSAGE(loaded.IsOk(), loaded.GetError());
	REQUIRE(loaded.GetValue().size() == 1);
	CHECK(loaded.GetValue()[0].Graph == full.Graph);
	CHECK(loaded.GetValue()[0].Span == full.Span);

	// What is not of the form is refused, naming the member: a handle is a string, a span two offsets in order.
	struct Case
	{
		std::string_view Json;
		std::string_view Error;
	};
	std::vector<Case> const cases = {
		{R"({ "Nodes": [], "Diagnostics": [ { "Severity": "Error", "Code": "X", "Function": 0, "Graph": 8146390251172604461, "Message": "" } ] })",
	     "Diagnostics[0].Graph: expected an asset handle: a string holding a decimal integer of 0 to 18446744073709551615"},
		{R"({ "Nodes": [], "Diagnostics": [ { "Severity": "Error", "Code": "X", "Function": 0, "Graph": "Door", "Message": "" } ] })",
	     "Diagnostics[0].Graph: expected an asset handle: a string holding a decimal integer of 0 to 18446744073709551615"},
		{R"({ "Nodes": [], "Diagnostics": [ { "Severity": "Error", "Code": "X", "Function": 0, "Graph": "-1", "Message": "" } ] })",
	     "Diagnostics[0].Graph: expected an asset handle: a string holding a decimal integer of 0 to 18446744073709551615"},
		{R"({ "Nodes": [], "Diagnostics": [ { "Severity": "Error", "Code": "X", "Function": 0, "Setting": 3, "Message": "" } ] })",
	     "Diagnostics[0].Setting: expected a string"},
		{R"({ "Nodes": [], "Diagnostics": [ { "Severity": "Error", "Code": "X", "Function": 0, "Span": [9, 5], "Message": "" } ] })",
	     "Diagnostics[0].Span: expected [start, end]: integers of 0 to 4294967295, start not after end"},
		{R"({ "Nodes": [], "Diagnostics": [ { "Severity": "Error", "Code": "X", "Function": 0, "Span": [5], "Message": "" } ] })",
	     "Diagnostics[0].Span: expected [start, end]: integers of 0 to 4294967295, start not after end"},
		{R"({ "Nodes": [], "Diagnostics": [ { "Severity": "Error", "Code": "X", "Function": 0, "Span": [-1, 2], "Message": "" } ] })",
	     "Diagnostics[0].Span: expected [start, end]: integers of 0 to 4294967295, start not after end"},
		{R"({ "Nodes": [], "Diagnostics": [ { "Severity": "Error", "Code": "X", "Function": 0, "Span": [0, 4294967296], "Message": "" } ] })",
	     "Diagnostics[0].Span: expected [start, end]: integers of 0 to 4294967295, start not after end"},
		{R"({ "Nodes": [], "Diagnostics": [ { "Severity": "Error", "Code": "X", "Function": 0, "Span": "5-9", "Message": "" } ] })",
	     "Diagnostics[0].Span: expected [start, end]: integers of 0 to 4294967295, start not after end"},
		{R"({ "Nodes": [ { "ID": 1, "Function": 0, "Type": "Self", "Resolved": "Exact", "PinRenames": {}, "Title": "Self", "Kind": "Data",
		     "Pure": true, "Latent": false, "Pins": [] } ], "Diagnostics": [] })",
	     "Nodes[0].PinRenames: expected an array"},
		{R"({ "Nodes": [ { "ID": 1, "Function": 0, "Type": "Self", "Resolved": "Exact", "PinRenames": [ { "Direction": "In", "From": "A" } ],
		     "Title": "Self", "Kind": "Data", "Pure": true, "Latent": false, "Pins": [] } ], "Diagnostics": [] })",
	     "Nodes[0].PinRenames[0].To: expected a string"},
		// Redirected and unique nodes still need their renames.
		{R"({ "Nodes": [ { "ID": 1, "Function": 0, "Type": "Self", "Resolved": "Unique", "CanonicalType": "Self", "Title": "Self",
		     "Kind": "Data", "Pure": true, "Latent": false, "Pins": [] } ], "Diagnostics": [] })",
	     "Nodes[0].PinRenames: expected an array"},
	};
	for (Case const& refused : cases)
	{
		CAPTURE(refused.Json);
		Result<VisualScriptAnalysis> read = VisualScriptAnalysis::Parse(refused.Json);
		REQUIRE(read.IsError());
		CHECK(read.GetError() == refused.Error);
	}
	// "0" is a handle string too: the graph of none.
	Result<VisualScriptAnalysis> none = VisualScriptAnalysis::Parse(
		R"({ "Nodes": [], "Diagnostics": [ { "Severity": "Error", "Code": "X", "Function": 0, "Graph": "0", "Message": "" } ] })");
	REQUIRE_MESSAGE(none.IsOk(), none.GetError());
	CHECK_FALSE(none.GetValue().Diagnostics[0].Graph.IsValid());
}

TEST_CASE("VisualScriptEngine: the node library describes the scripting API, and enum lookups read it")
{
	// Without the runtime there is no library, and enum variables need the runtime.
	CHECK(VisualScriptEngine::GetLibrary() == nullptr);
	CHECK(VisualScriptEngine::GetLibraryState() == VisualScriptLibraryState::NoRuntime);
	CHECK_FALSE(VisualScriptEngine::MakeEnumLookup());
	CHECK_FALSE(VisualScriptEngine::FindEnum("Strada.KeyCode").has_value());

	VisualScriptEnumLookup kept;
	{
		Testing::ScriptRuntimeScope runtime;
		// The library is read when first needed: by an enum lookup as well.
		CHECK(VisualScriptEngine::GetLibraryState() == VisualScriptLibraryState::NotBuilt);
		CHECK(VisualScriptEngine::FindEnum("Strada.ForceMode").has_value());
		CHECK_FALSE(VisualScriptEngine::FindEnum("float").has_value());
		CHECK(VisualScriptEngine::GetLibraryState() == VisualScriptLibraryState::Built);
		Ref<VisualScriptLibrary const> const library = VisualScriptEngine::GetLibrary();
		REQUIRE(library != nullptr);
		CHECK(VisualScriptEngine::GetLibrary() == library);

		VisualScriptTypeInfo const* floatType = library->FindType("float");
		REQUIRE(floatType != nullptr);
		CHECK(floatType->Kind == VisualScriptTypeKind::Float);
		CHECK(floatType->FieldType == ScriptFieldType::Float);
		CHECK(floatType->HasLiteral);
		CHECK(floatType->ImplicitTo == std::vector<std::string>{"double", "System.Object"});
		VisualScriptTypeInfo const* exec = library->FindType("exec");
		REQUIRE(exec != nullptr);
		CHECK(exec->Kind == VisualScriptTypeKind::Exec);
		CHECK(exec->ImplicitTo.empty());
		VisualScriptTypeInfo const* material = library->FindType("Strada.Material");
		REQUIRE(material != nullptr);
		CHECK(material->Kind == VisualScriptTypeKind::Asset);
		CHECK(material->FieldType == ScriptFieldType::Asset);
		CHECK(material->AcceptedAssetType == AssetType::Material);
		CHECK(material->Base == "Strada.Asset");
		CHECK(material->ImplicitTo == std::vector<std::string>{"Strada.Asset", "Strada.AssetHandle", "System.Object"});
		VisualScriptTypeInfo const* rigidBody = library->FindType("Strada.RigidBodyComponent");
		REQUIRE(rigidBody != nullptr);
		CHECK(rigidBody->Kind == VisualScriptTypeKind::Component);
		CHECK(rigidBody->Base == "Strada.Component");
		CHECK(rigidBody->ImplicitTo == std::vector<std::string>{"Strada.Entity", "System.Object"});
		VisualScriptTypeInfo const* vector = library->FindType("Strada.Vector3");
		REQUIRE(vector != nullptr);
		CHECK(vector->Makeable);
		CHECK(vector->Breakable);
		CHECK(vector->Fields.size() == 3);
		VisualScriptTypeInfo const* children = library->FindType("Strada.Entity[]");
		REQUIRE(children != nullptr);
		CHECK(children->Kind == VisualScriptTypeKind::Array);
		CHECK(children->Element == "Strada.Entity");
		VisualScriptTypeInfo const* environment = library->FindType("Strada.EnvironmentMap");
		REQUIRE(environment != nullptr);
		CHECK(environment->AcceptedAssetType == AssetType::Environment);
		CHECK(std::is_sorted(library->GetTypes().begin(), library->GetTypes().end(),
		                     [](VisualScriptTypeInfo const& a, VisualScriptTypeInfo const& b)
		                     {
								 return a.ID < b.ID;
							 }));

		VisualScriptNodeTemplate const* addForce =
			library->FindNode("Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)");
		REQUIRE(addForce != nullptr);
		CHECK(addForce->Title == "Add Force");
		CHECK(addForce->Subtitle == "Target is Rigid Body");
		CHECK(addForce->Category == "Components/Rigid Body");
		CHECK(addForce->Kind == VisualScriptNodeKind::Call);
		std::vector<std::string> pins;
		for (VisualScriptPin const& pin : addForce->Pins)
		{
			pins.push_back(pin.Name);
		}
		CHECK(pins == std::vector<std::string>{"Execute", "Target", "Force", "Mode", "Then"});
		CHECK(addForce->FindPin("Target", VisualScriptPinDirection::In)->DefaultsToSelf);
		CHECK(addForce->FindPin("Mode", VisualScriptPinDirection::In)->Default == "Force");
		VisualScriptNodeTemplate const* delay = library->FindNode("Flow.Delay");
		REQUIRE(delay != nullptr);
		CHECK(delay->Latent);
		CHECK(delay->FindPin("Completed", VisualScriptPinDirection::Out)->Role == VisualScriptPinRole::Continuation);
		VisualScriptNodeTemplate const* sequence = library->FindNode("Flow.Sequence");
		REQUIRE(sequence != nullptr);
		REQUIRE_FALSE(sequence->Settings.empty());
		CHECK(sequence->Settings[0].Name == "Count");
		CHECK(sequence->Settings[0].Min == std::optional<int64_t>(2));
		CHECK(sequence->Settings[0].Max == std::optional<int64_t>(64));
		VisualScriptNodeTemplate const* reroute = library->FindNode("Reroute");
		REQUIRE(reroute != nullptr);
		CHECK(reroute->FindPin("In", VisualScriptPinDirection::In)->CarriesExec);

		VisualScriptCategory const* components = FindChild(library->GetCategories(), "Components");
		REQUIRE(components != nullptr);
		VisualScriptCategory const* rigidBodyNodes = FindChild(*components, "Rigid Body");
		REQUIRE(rigidBodyNodes != nullptr);
		CHECK(std::find(rigidBodyNodes->Nodes.begin(), rigidBodyNodes->Nodes.end(), addForce) != rigidBodyNodes->Nodes.end());
		CHECK(CountNodes(library->GetCategories()) == library->GetNodes().size());

		std::vector<std::string> const forces = GetTypes(library->Search("ADD FORCE"));
		CHECK(std::find(forces.begin(), forces.end(), addForce->Type) != forces.end());
		std::vector<std::string> const knots = GetTypes(library->Search("knot"));
		CHECK(std::find(knots.begin(), knots.end(), "Reroute") != knots.end());
		CHECK(library->Search("").size() == library->GetNodes().size());

		VisualScriptEnumLookup const lookup = VisualScriptEngine::MakeEnumLookup();
		REQUIRE(lookup);
		std::optional<VisualScriptEnumInfo> const keys = lookup("Strada.KeyCode");
		REQUIRE(keys.has_value());
		CHECK(keys->UnderlyingType == ScriptFieldType::Int32);
		CHECK_FALSE(keys->IsFlags);
		for (ScriptEnumerator const& enumerator : keys->Enumerators)
		{
			CHECK(enumerator.Value.GetType() == ScriptFieldType::Int32);
		}
		auto const space = std::find_if(keys->Enumerators.begin(), keys->Enumerators.end(),
		                                [](ScriptEnumerator const& enumerator)
		                                {
											return enumerator.Name == "Space";
										});
		REQUIRE(space != keys->Enumerators.end());
		CHECK(space->Value == ScriptFieldValue::FromInt32(32));
		CHECK(lookup("Strada.ForceMode").has_value());
		CHECK_FALSE(lookup("float").has_value());
		CHECK_FALSE(lookup("Strada.Entity").has_value());
		CHECK_FALSE(lookup("Strada.Missing").has_value());
		kept = lookup;
	}
	// A lookup keeps the library it searches: it never calls into the runtime.
	REQUIRE(kept);
	CHECK(kept("Strada.KeyCode").has_value());
}

TEST_CASE("VisualScriptEngine: graphs get and set a VisualScript component's graph, but not call its methods, and load visual scripts")
{
	Testing::ScriptRuntimeScope runtime;
	Ref<VisualScriptLibrary const> const library = VisualScriptEngine::GetLibrary();
	REQUIRE(library != nullptr);

	// The component's Graph has a getter and a setter node; graphs use Event.Send and the variable nodes, not its methods.
	VisualScriptTypeInfo const* component = library->FindType("Strada.VisualScriptComponent");
	REQUIRE(component != nullptr);
	CHECK(component->Kind == VisualScriptTypeKind::Component);
	std::vector<std::string> members;
	for (VisualScriptNodeTemplate const& node : library->GetNodes())
	{
		if (node.Type.find("Strada.VisualScriptComponent.") != std::string::npos)
		{
			members.push_back(node.Type);
		}
	}
	std::sort(members.begin(), members.end());
	CHECK(members == std::vector<std::string>{"Get:Strada.VisualScriptComponent.Graph", "Set:Strada.VisualScriptComponent.Graph"});
	VisualScriptNodeTemplate const* getter = library->FindNode("Get:Strada.VisualScriptComponent.Graph");
	REQUIRE(getter != nullptr);
	CHECK(getter->Pure);
	VisualScriptPin const* target = getter->FindPin("Target", VisualScriptPinDirection::In);
	REQUIRE(target != nullptr);
	CHECK(target->DefaultsToSelf);
	VisualScriptPin const* value = getter->FindPin("Value", VisualScriptPinDirection::Out);
	REQUIRE(value != nullptr);
	CHECK(value->Type == "Strada.VisualScript");
	VisualScriptNodeTemplate const* setter = library->FindNode("Set:Strada.VisualScriptComponent.Graph");
	REQUIRE(setter != nullptr);
	CHECK_FALSE(setter->Pure);
	VisualScriptPin const* assigned = setter->FindPin("Value", VisualScriptPinDirection::In);
	REQUIRE(assigned != nullptr);
	CHECK(assigned->Type == "Strada.VisualScript");

	// Visual scripts are assets: variables hold them, components store them for public ones, documents hold their handles.
	VisualScriptTypeInfo const* visualScript = library->FindType("Strada.VisualScript");
	REQUIRE(visualScript != nullptr);
	CHECK(visualScript->Kind == VisualScriptTypeKind::Asset);
	CHECK(visualScript->FieldType == ScriptFieldType::Asset);
	CHECK(visualScript->AcceptedAssetType == AssetType::VisualScript);
	CHECK(visualScript->HasLiteral);
	CHECK(visualScript->Base == "Strada.Asset");
	CHECK(visualScript->ImplicitTo == std::vector<std::string>{"Strada.Asset", "Strada.AssetHandle", "System.Object"});

	// Assets.Load<T> loads them: a graph that does analyzes cleanly with a result of the type, and a type that is not an asset
	// class is refused.
	auto const analyzeLoad = [](std::string const& typeArgument)
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Loaded", "Strada.Asset", Json(), false));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 1}})));
		document.Graph.Nodes.push_back(MakeNode(12, "Call:Strada.Assets.Load<T>(string)",
		                                        Json::object({{"TypeArguments", Json::array({typeArgument})}}),
		                                        Json::object({{"Path", "Scripts/Behaviour.sgraph"}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(12, "Result", 11, "Value"));
		Result<VisualScriptAnalysis> analysis = VisualScriptEngine::Analyze(document);
		REQUIRE_MESSAGE(analysis.IsOk(), analysis.GetError());
		return analysis.TakeValue();
	};
	VisualScriptAnalysis const loaded = analyzeLoad("Strada.VisualScript");
	for (VisualScriptDiagnostic const& diagnostic : loaded.Diagnostics)
	{
		CAPTURE(diagnostic.Message);
		CHECK(diagnostic.Severity == VisualScriptSeverity::Warning);
	}
	VisualScriptAnalyzedNode const* load = loaded.FindNode(12);
	REQUIRE(load != nullptr);
	VisualScriptPin const* result = load->FindPin("Result", VisualScriptPinDirection::Out);
	REQUIRE(result != nullptr);
	CHECK(result->Type == "Strada.VisualScript");
	VisualScriptAnalysis const refused = analyzeLoad("Strada.Entity");
	CHECK(std::any_of(refused.Diagnostics.begin(), refused.Diagnostics.end(),
	                  [](VisualScriptDiagnostic const& diagnostic)
	                  {
						  return diagnostic.Severity == VisualScriptSeverity::Error && diagnostic.Code == "InvalidSetting" &&
		                         diagnostic.Node == std::optional<uint32_t>(12);
					  }));
}

TEST_CASE("VisualScriptEngine: CanConnect allows exactly the links analysis accepts")
{
	Testing::ScriptRuntimeScope runtime;
	Ref<VisualScriptLibrary const> const library = VisualScriptEngine::GetLibrary();
	REQUIRE(library != nullptr);

	std::vector<LinkEnd> outputs = {{"exec", "Event.Create", Json::object(), "Then", "", false}};
	std::vector<LinkEnd> inputs = {{"exec", "Flow.Branch", Json::object(), "Execute", "", false}};
	for (std::string_view const type : ParityTypes)
	{
		outputs.push_back(MakeVariableOutput(type));
		inputs.push_back(MakeVariableInput(type));
	}
	outputs.push_back({"Strada.Entity[]", "Get:Strada.Entity.Children", Json::object(), "Value", "", false});
	inputs.push_back({"Operator.Add.A", "Operator.Add", Json::object(), "A", "", true});
	inputs.push_back({"IsValid.Value", "IsValid", Json::object(), "Value", "", true});
	inputs.push_back({"Array.Length.Array", "Array.Length", Json::object(), "Array", "", true});
	inputs.push_back({"Reroute.In", "Reroute", Json::object(), "In", "", true});

	size_t accepted = 0;
	size_t refused = 0;
	for (LinkEnd const& from : outputs)
	{
		for (LinkEnd const& to : inputs)
		{
			CAPTURE(from.Name);
			CAPTURE(to.Name);
			VisualScriptDocument document;
			if (!from.VariableType.empty())
			{
				document.Variables.push_back(MakeVariable(1, "From", from.VariableType, Json(), false));
			}
			if (!to.VariableType.empty())
			{
				document.Variables.push_back(MakeVariable(2, "To", to.VariableType, Json(), false));
			}
			document.Graph.Nodes.push_back(MakeNode(OutputNode, from.NodeType, from.Settings));
			document.Graph.Nodes.push_back(MakeNode(InputNode, to.NodeType, to.Settings));
			VisualScriptLink const link = MakeLink(OutputNode, from.Pin, InputNode, to.Pin);
			document.Graph.Links.push_back(link);

			Result<VisualScriptAnalysis> analyzed = VisualScriptEngine::Analyze(document);
			REQUIRE_MESSAGE(analyzed.IsOk(), analyzed.GetError());
			VisualScriptAnalysis const& analysis = analyzed.GetValue();
			bool const mismatch = std::any_of(analysis.Diagnostics.begin(), analysis.Diagnostics.end(),
			                                  [&link](VisualScriptDiagnostic const& diagnostic)
			                                  {
												  return diagnostic.Code == "TypeMismatch" && diagnostic.Link == link;
											  });
			VisualScriptPin const* output = FindLinkPin(*library, analysis, from, OutputNode, VisualScriptPinDirection::Out);
			VisualScriptPin const* input = FindLinkPin(*library, analysis, to, InputNode, VisualScriptPinDirection::In);
			REQUIRE(output != nullptr);
			REQUIRE(input != nullptr);
			if (!from.IsWildcard)
			{
				CHECK(output->Type == from.Name);
			}
			if (!to.IsWildcard)
			{
				CHECK(input->Type == to.Name);
				// Nothing else is wrong with the document, so the link alone decides.
				for (VisualScriptDiagnostic const& diagnostic : analysis.Diagnostics)
				{
					CAPTURE(diagnostic.Message);
					CHECK((diagnostic.Severity == VisualScriptSeverity::Warning || diagnostic.Code == "TypeMismatch"));
				}
			}
			bool const connects = library->CanConnect(*output, *input);
			CHECK(connects == !mismatch);
			(connects ? accepted : refused)++;
		}
	}
	CHECK(accepted > 0);
	CHECK(refused > 0);

	// Wildcard outputs take their type from what they link to, so analysis reports no mismatch for them: the rules alone.
	auto const templatePin = [&library](std::string_view node, std::string_view name, VisualScriptPinDirection direction)
	{
		VisualScriptNodeTemplate const* found = library->FindNode(node);
		REQUIRE(found != nullptr);
		VisualScriptPin const* pin = found->FindPin(name, direction);
		REQUIRE(pin != nullptr);
		return *pin;
	};
	VisualScriptPin const addResult = templatePin("Operator.Add", "Result", VisualScriptPinDirection::Out);
	VisualScriptPin const component = templatePin("Call:Strada.Entity.GetComponent<T>()", "Result", VisualScriptPinDirection::Out);
	CHECK(library->CanConnect(templatePin("Reroute", "Out", VisualScriptPinDirection::Out), MakePin("exec", VisualScriptPinDirection::In)));
	CHECK(library->CanConnect(addResult, MakePin("float", VisualScriptPinDirection::In)));
	CHECK(library->CanConnect(addResult, templatePin("Operator.Add", "A", VisualScriptPinDirection::In)));
	CHECK_FALSE(library->CanConnect(addResult, MakePin("exec", VisualScriptPinDirection::In)));
	CHECK(library->CanConnect(component, MakePin("Strada.Entity", VisualScriptPinDirection::In)));
	CHECK_FALSE(library->CanConnect(component, MakePin("float", VisualScriptPinDirection::In)));
}

TEST_CASE("VisualScriptEngine: the runtime's entry points answer through receive exactly once, and only on success")
{
	using ScriptEngineInternal::ScriptStatus;
	Testing::ScriptRuntimeScope runtime;
	ScriptEngineInternal::ScriptEngineData const* const data = ScriptEngineInternal::GetData();
	REQUIRE(data != nullptr);
	ScriptEngineInternal::HostFunctions const& host = data->Host;
	auto const status = [](int32_t value)
	{
		return static_cast<ScriptStatus>(value);
	};

	Received library;
	CHECK(status(host.VisualScriptDescribeLibrary(&library, &Receive)) == ScriptStatus::Success);
	CHECK(library.Calls == 1);
	CHECK(VisualScriptLibrary::Parse(library.Text).IsOk());

	// Anything analyzes, nothing at all included.
	Received empty;
	CHECK(status(host.VisualScriptAnalyze(nullptr, 0, nullptr, 0, &empty, &Receive)) == ScriptStatus::Success);
	CHECK(empty.Calls == 1);
	Result<VisualScriptAnalysis> nothing = VisualScriptAnalysis::Parse(empty.Text);
	REQUIRE_MESSAGE(nothing.IsOk(), nothing.GetError());
	REQUIRE(nothing.GetValue().Diagnostics.size() == 1);
	CHECK(nothing.GetValue().Diagnostics[0].Code == "InvalidDocument");

	std::string const document = VisualScriptSerializer::SerializeDocument(MakeCountDocument(5)).dump();
	Received analyzed;
	CHECK(status(host.VisualScriptAnalyze(document.data(), static_cast<int32_t>(document.size()), nullptr, 0, &analyzed, &Receive)) ==
	      ScriptStatus::Success);
	CHECK(analyzed.Calls == 1);
	Result<VisualScriptAnalysis> analysis = VisualScriptAnalysis::Parse(analyzed.Text);
	REQUIRE_MESSAGE(analysis.IsOk(), analysis.GetError());
	CHECK(analysis.GetValue().Nodes.size() == 2);
	CHECK(analysis.GetValue().Diagnostics.empty());
	// References of any form accompany a document; one whose nodes refer to no other graph analyzes as without them.
	constexpr std::string_view References = R"({"Self":"0","Graphs":[]})";
	Received referenced;
	CHECK(status(host.VisualScriptAnalyze(document.data(), static_cast<int32_t>(document.size()), References.data(),
	                                      static_cast<int32_t>(References.size()), &referenced, &Receive)) == ScriptStatus::Success);
	CHECK(referenced.Calls == 1);
	CHECK(referenced.Text == analyzed.Text);

	constexpr std::string_view Name = "Counter";
	constexpr uint64_t Graph = 4200;
	Received loaded;
	CHECK(status(host.VisualScriptLoad(Graph, Name.data(), static_cast<int32_t>(Name.size()), document.data(),
	                                   static_cast<int32_t>(document.size()), nullptr, 0, &loaded, &Receive)) == ScriptStatus::Success);
	CHECK(loaded.Calls == 1);
	Result<std::vector<VisualScriptDiagnostic>> diagnostics = ParseVisualScriptDiagnostics(loaded.Text);
	REQUIRE_MESSAGE(diagnostics.IsOk(), diagnostics.GetError());
	CHECK(diagnostics.GetValue().empty());

	// Instances: NotFound passes nothing to receive.
	constexpr uint64_t Instance = 7;
	constexpr uint64_t Stranger = 8;
	CHECK(status(host.VisualScriptCreateInstance(Instance, Graph + 1, nullptr, 0)) == ScriptStatus::NotFound);
	CHECK(status(host.VisualScriptCreateInstance(Instance, Graph, nullptr, 0)) == ScriptStatus::Success);
	Received variables;
	CHECK(status(host.VisualScriptDescribeInstanceVariables(Stranger, &variables, &Receive)) == ScriptStatus::NotFound);
	CHECK(variables.Calls == 0);
	CHECK(status(host.VisualScriptInvoke(Instance, static_cast<int32_t>(ScriptEngineInternal::ScriptEvent::Create), 0.0f, 0)) ==
	      ScriptStatus::Success);
	CHECK(status(host.VisualScriptDescribeInstanceVariables(Instance, &variables, &Receive)) == ScriptStatus::Success);
	CHECK(variables.Calls == 1);
	Result<Json> values = ParseJson(variables.Text);
	REQUIRE_MESSAGE(values.IsOk(), values.GetError());
	CHECK(values.GetValue() == Json::parse(R"({ "Count": { "Type": "Int32", "Value": 5 } })"));

	std::string const reset = R"({ "Count": { "Type": "Int32", "Value": 1 } })";
	CHECK(status(host.VisualScriptSetInstanceVariables(Stranger, reset.data(), static_cast<int32_t>(reset.size()))) ==
	      ScriptStatus::NotFound);
	CHECK(status(host.VisualScriptSetInstanceVariables(Instance, reset.data(), static_cast<int32_t>(reset.size()))) ==
	      ScriptStatus::Success);
	CHECK(status(host.VisualScriptSetInstanceVariables(Instance, nullptr, 0)) == ScriptStatus::Success);
	CHECK(status(host.VisualScriptInvoke(Stranger, static_cast<int32_t>(ScriptEngineInternal::ScriptEvent::Update), 0.1f, 0)) ==
	      ScriptStatus::NotFound);
	CHECK(status(host.VisualScriptInvoke(Instance, 99, 0.0f, 0)) == ScriptStatus::Failure);
	CHECK(status(host.VisualScriptDestroyInstance(Instance)) == ScriptStatus::Success);
	CHECK(status(host.VisualScriptDestroyInstance(Instance)) == ScriptStatus::NotFound);
	CHECK(status(host.VisualScriptDestroyAllInstances()) == ScriptStatus::Success);
	CHECK(status(host.VisualScriptUnload(Graph)) == ScriptStatus::Success);
	CHECK(status(host.VisualScriptUnload(Graph)) == ScriptStatus::NotFound);
	CHECK(status(host.VisualScriptCreateInstance(Instance, Graph, nullptr, 0)) == ScriptStatus::NotFound);
}

TEST_CASE("VisualScriptEngine: documents are sent once per change, and a broken one leaves the working program running")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	Ref<VisualScriptAsset> const asset = CreateRef<VisualScriptAsset>(MakeCountDocument(1));
	AssetHandle const graph = AssetManager::AddMemoryAsset(asset, "Counter");

	uint64_t logStart = Log::GetNextEntryIndex();
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) == 0);
	CHECK_FALSE(VisualScriptEngine::HasWorkingProgram(graph));
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	uint64_t const first = VisualScriptEngine::GetProgramGeneration(graph);
	CHECK(first != 0);
	CHECK(VisualScriptEngine::HasWorkingProgram(graph));
	// The engine keeps the asset's document itself, not a copy.
	CHECK(VisualScriptEngine::GetWorkingDocument(graph) == asset->GetSharedDocument());
	// Unchanged, then saved with the same document: nothing is sent.
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) == first);
	asset->SetDocument(MakeCountDocument(1));
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) == first);
	CHECK_FALSE(Testing::WasLogged(logStart, "Visual script 'Counter'"));

	// Edits of what the runtime never reads give the same program, so nothing compiles again: a node moved, given a
	// comment or collapsed; a comment box, the description, a tooltip and a range. The running program's document is the
	// newest of them.
	VisualScriptDocument laidOut = MakeCountDocument(1);
	laidOut.Graph.Nodes[1].Position = glm::vec2(240.0f, 0.0f);
	CHECK_FALSE(SendsDocument(*asset, graph, laidOut));
	CHECK(VisualScriptEngine::GetWorkingDocument(graph) == asset->GetSharedDocument());
	laidOut.Graph.Nodes[1].Comment = "Counts once";
	CHECK_FALSE(SendsDocument(*asset, graph, laidOut));
	laidOut.Graph.Nodes[0].Collapsed = true;
	CHECK_FALSE(SendsDocument(*asset, graph, laidOut));
	VisualScriptComment box;
	box.ID = 20;
	box.Text = "Start";
	laidOut.Graph.Comments.push_back(box);
	laidOut.Description = "Counts";
	laidOut.Variables[0].Tooltip = "How often";
	laidOut.Variables[0].Range = glm::vec2(0.0f, 10.0f);
	CHECK_FALSE(SendsDocument(*asset, graph, laidOut));
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) == first);
	CHECK(VisualScriptEngine::GetWorkingDocument(graph) == asset->GetSharedDocument());

	// What it reads is sent: a link, a literal, whether a node is enabled, and how a number is written (operator== takes
	// the int literal 1.0 for 1, which the runtime reads as no int).
	VisualScriptDocument unlinked = laidOut;
	unlinked.Graph.Links.clear();
	VisualScriptDocument otherLiteral = laidOut;
	otherLiteral.Graph.Nodes[1].Inputs["Value"] = 2;
	VisualScriptDocument disabled = laidOut;
	disabled.Graph.Nodes[1].Enabled = VisualScriptNodeEnabled::Disabled;
	VisualScriptDocument floatLiteral = laidOut;
	floatLiteral.Graph.Nodes[1].Inputs["Value"] = 1.0;
	REQUIRE(floatLiteral == laidOut);
	CHECK(SendsDocument(*asset, graph, unlinked));
	CHECK(SendsDocument(*asset, graph, laidOut));
	CHECK(SendsDocument(*asset, graph, otherLiteral));
	CHECK(SendsDocument(*asset, graph, laidOut));
	CHECK(SendsDocument(*asset, graph, disabled));
	CHECK(SendsDocument(*asset, graph, laidOut));
	CHECK(SendsDocument(*asset, graph, floatLiteral));
	CHECK(SendsDocument(*asset, graph, laidOut));
	// The layout taken away again changes no program either.
	CHECK_FALSE(SendsDocument(*asset, graph, MakeCountDocument(1)));
	CHECK(VisualScriptEngine::GetWorkingDocument(graph) == asset->GetSharedDocument());

	UUID const one(1001);
	REQUIRE(VisualScriptEngine::CreateInstance(one, graph, {}).IsOk());
	VisualScriptEngine::InvokeOnCreate(one);
	CHECK(ReadVariable(one, "Count") == ScriptFieldValue::FromInt32(1));

	// A broken document is sent once and its errors logged once; the working program keeps running.
	asset->SetDocument(MakeBrokenDocument());
	logStart = Log::GetNextEntryIndex();
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	uint64_t const broken = VisualScriptEngine::GetProgramGeneration(graph);
	CHECK(broken > first);
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) == broken);
	// A node of the broken document moved: nothing is sent, and the program that runs keeps its document.
	VisualScriptDocument movedBroken = MakeBrokenDocument();
	movedBroken.Graph.Nodes[2].Position = glm::vec2(0.0f, 240.0f);
	CHECK_FALSE(SendsDocument(*asset, graph, movedBroken));
	CHECK(CountLogged(logStart, "Visual script 'Counter' has 1 error; first: ") == 1);
	CHECK(Testing::WasLogged(logStart, " (node 12); the previous version keeps running"));
	CHECK(VisualScriptEngine::HasWorkingProgram(graph));
	// Instances run the working program, whose document the engine keeps.
	Ref<VisualScriptDocument const> const working = VisualScriptEngine::GetWorkingDocument(graph);
	REQUIRE(working != nullptr);
	CHECK(*working == MakeCountDocument(1));
	UUID const two(1002);
	REQUIRE(VisualScriptEngine::CreateInstance(two, graph, {}).IsOk());
	VisualScriptEngine::InvokeOnCreate(two);
	CHECK(ReadVariable(two, "Count") == ScriptFieldValue::FromInt32(1));

	// Fixed: sent, and run.
	asset->SetDocument(MakeCountDocument(3));
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) > broken);
	CHECK(*VisualScriptEngine::GetWorkingDocument(graph) == MakeCountDocument(3));
	UUID const three(1003);
	REQUIRE(VisualScriptEngine::CreateInstance(three, graph, {}).IsOk());
	VisualScriptEngine::InvokeOnCreate(three);
	CHECK(ReadVariable(three, "Count") == ScriptFieldValue::FromInt32(3));
	CHECK(host.Exceptions == 0);

	// Broken from the start: nothing runs, and creating an instance is a script exception.
	AssetHandle const failing = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeBrokenDocument()), "Broken");
	logStart = Log::GetNextEntryIndex();
	REQUIRE(VisualScriptEngine::EnsureLoaded(failing).IsOk());
	CHECK(Testing::WasLogged(logStart, "Visual script 'Broken' has 1 error; first: "));
	CHECK_FALSE(Testing::WasLogged(logStart, "keeps running"));
	CHECK(VisualScriptEngine::GetWorkingDocument(failing) == nullptr);
	CHECK_FALSE(VisualScriptEngine::HasWorkingProgram(failing));
	UUID const four(1004);
	Result<void> const notCreated = VisualScriptEngine::CreateInstance(four, failing, {});
	REQUIRE(notCreated.IsError());
	CHECK(notCreated.GetError() == "the visual script 'Broken' cannot run (see the log)");
	CHECK(Testing::WasLogged(logStart, "Broken cannot run: 1 error, first: "));
	CHECK(host.Exceptions == 1);
	CHECK_FALSE(VisualScriptEngine::HasInstance(four));

	// Absent graphs are not sent, and are no script exception.
	Result<void> const unset = VisualScriptEngine::EnsureLoaded(AssetHandle());
	REQUIRE(unset.IsError());
	CHECK(unset.GetError() == "no visual script is set");
	AssetHandle const unregistered(UUID(424242));
	Result<void> const missing = VisualScriptEngine::CreateInstance(four, unregistered, {});
	REQUIRE(missing.IsError());
	CHECK(missing.GetError() == "asset 424242 is not registered");
	AssetHandle const material = AssetManager::AddMemoryAsset(CreateRef<MaterialAsset>(), "Paint");
	Result<void> const notGraph = VisualScriptEngine::EnsureLoaded(material);
	REQUIRE(notGraph.IsError());
	CHECK(notGraph.GetError() == fmt::format("asset {} is a Material, not a VisualScript", material));
	CHECK(VisualScriptEngine::GetProgramGeneration(material) == 0);
	CHECK(host.Exceptions == 1);

	// Without assets every graph is absent.
	AssetManager::Shutdown();
	Result<void> const noAssets = VisualScriptEngine::EnsureLoaded(graph);
	REQUIRE(noAssets.IsError());
	CHECK(noAssets.GetError() == "there are no assets");
}

TEST_CASE("VisualScriptEngine: a graph file loaded again unchanged is not sent again, and one that comes back is retried")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const file = directory.GetPath() / "Counter.sgraph";
	REQUIRE(VisualScriptSerializer::SaveToFile(MakeCountDocument(1), file).IsOk());
	Testing::AssetManagerScope assets;
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());
	AssetHandle const graph = AssetManager::FindByPath("Counter.sgraph");
	REQUIRE(graph.IsValid());
	Testing::ScriptRuntimeScope runtime;

	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	uint64_t const first = VisualScriptEngine::GetProgramGeneration(graph);
	// A save followed by a refresh: another asset object with the same document.
	REQUIRE(AssetManager::ReloadAsset(graph).IsOk());
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) == first);

	// A file that does not load makes the graph absent; its program stays.
	REQUIRE(FileSystem::WriteTextFile(file, "{ not JSON").IsOk());
	CHECK(AssetManager::ReloadAsset(graph).IsError());
	Result<void> const absent = VisualScriptEngine::EnsureLoaded(graph);
	REQUIRE(absent.IsError());
	CHECK(absent.GetError().find("Counter.sgraph") != std::string::npos);
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) == first);
	Result<void> const notCreated = VisualScriptEngine::CreateInstance(UUID(77), graph, {});
	REQUIRE(notCreated.IsError());
	CHECK(notCreated.GetError() == absent.GetError());

	// Back with the same document: nothing is sent, but the generation changes, so entities that could not start retry.
	REQUIRE(VisualScriptSerializer::SaveToFile(MakeCountDocument(1), file).IsOk());
	REQUIRE(AssetManager::ReloadAsset(graph).IsOk());
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	uint64_t const back = VisualScriptEngine::GetProgramGeneration(graph);
	CHECK(back != first);
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) == back);
	REQUIRE(VisualScriptEngine::CreateInstance(UUID(77), graph, {}).IsOk());

	// File assets are named by their path in messages.
	uint64_t const logStart = Log::GetNextEntryIndex();
	REQUIRE(VisualScriptSerializer::SaveToFile(MakeBrokenDocument(), file).IsOk());
	REQUIRE(AssetManager::ReloadAsset(graph).IsOk());
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(Testing::WasLogged(logStart, "Visual script 'Counter.sgraph' has 1 error; first: "));
	CHECK(Testing::WasLogged(logStart, "; the previous version keeps running"));
}

TEST_CASE("VisualScriptEngine: a graph moved to another path is named by it in messages, by its running program too")
{
	Testing::TemporaryDirectory directory;
	REQUIRE(VisualScriptSerializer::SaveToFile(MakeDivisionDocument(), directory.GetPath() / "Old.sgraph").IsOk());
	Testing::AssetManagerScope assets;
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());
	AssetHandle const graph = AssetManager::FindByPath("Old.sgraph");
	REQUIRE(graph.IsValid());
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	Scene scene;
	UUID const id = scene.CreateEntity("Divider").GetUUID();
	SceneContextScope context(scene);

	REQUIRE(VisualScriptEngine::CreateInstance(id, graph, {}).IsOk());
	uint64_t logStart = Log::GetNextEntryIndex();
	VisualScriptEngine::InvokeOnCreate(id);
	CHECK(Testing::WasLogged(logStart, "Old.sgraph: Divide (node 12) on Divider ("));
	uint64_t const generation = VisualScriptEngine::GetProgramGeneration(graph);

	// Moved: the runtime gets the same document under the new path and the running instance takes its program. Nothing that
	// could not start before can now, so the generation stays.
	REQUIRE(AssetManager::MoveAsset(graph, "New.sgraph").IsOk());
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) == generation);
	logStart = Log::GetNextEntryIndex();
	VisualScriptEngine::InvokeOnCreate(id);
	CHECK(Testing::WasLogged(logStart, "New.sgraph: Divide (node 12) on Divider ("));
	CHECK_FALSE(Testing::WasLogged(logStart, "Old.sgraph"));

	// Moved while a broken edit leaves the program running: both take the new name, and the errors are told under it.
	Ref<VisualScriptAsset> const asset = AssetManager::GetAsset<VisualScriptAsset>(graph);
	REQUIRE(asset != nullptr);
	asset->SetDocument(MakeBrokenDocument());
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	REQUIRE(AssetManager::MoveAsset(graph, "Moved.sgraph").IsOk());
	logStart = Log::GetNextEntryIndex();
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(Testing::WasLogged(logStart, "Visual script 'Moved.sgraph' has 1 error; first: "));
	CHECK(Testing::WasLogged(logStart, "; the previous version keeps running"));
	VisualScriptEngine::InvokeOnCreate(id);
	CHECK(Testing::WasLogged(logStart, "Moved.sgraph: Divide (node 12) on Divider ("));
	CHECK_FALSE(Testing::WasLogged(logStart, "New.sgraph"));

	// Unchanged since, nothing is sent again.
	logStart = Log::GetNextEntryIndex();
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK_FALSE(Testing::WasLogged(logStart, "Moved.sgraph"));
	CHECK(host.Exceptions == 3);
	VisualScriptEngine::DestroyInstance(id);
}

TEST_CASE("VisualScriptEngine: its state goes with ScriptEngine::Shutdown and starts again with Init")
{
	Testing::AssetManagerScope assets;
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeCountDocument(4)), "Counter");
	UUID const entity(31);
	{
		Testing::ScriptRuntimeScope runtime;
		REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
		CHECK(VisualScriptEngine::GetProgramGeneration(graph) != 0);
		CHECK(VisualScriptEngine::HasWorkingProgram(graph));
		REQUIRE(VisualScriptEngine::CreateInstance(entity, graph, {}).IsOk());
		CHECK(VisualScriptEngine::HasInstance(entity));
	}

	// Without the runtime nothing is kept, nothing runs and nothing fails hard.
	CHECK_FALSE(VisualScriptEngine::HasInstance(entity));
	CHECK(VisualScriptEngine::GetInstanceCount() == 0);
	CHECK(ScriptEngine::GetInstanceCount() == 0);
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) == 0);
	CHECK_FALSE(VisualScriptEngine::HasWorkingProgram(graph));
	CHECK(VisualScriptEngine::EnsureLoaded(graph).GetError() == "the scripting runtime is not running");
	CHECK(VisualScriptEngine::CreateInstance(entity, graph, {}).GetError() == "the scripting runtime is not running");
	CHECK(VisualScriptEngine::Analyze(MakeCountDocument(4)).IsError());
	CHECK(VisualScriptEngine::GetInstanceVariables(entity).IsError());
	CHECK(VisualScriptEngine::SetInstanceVariables(entity, {}).IsError());
	VisualScriptEngine::InvokeOnCreate(entity);
	VisualScriptEngine::InvokeOnUpdate(entity, 0.1f);
	VisualScriptEngine::DestroyInstance(entity);
	VisualScriptEngine::DestroyAllInstances();

	// Started again: the graph is sent again (the runtime's programs went too) and runs.
	Testing::ScriptRuntimeScope again;
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) == 0);
	CHECK_FALSE(VisualScriptEngine::HasInstance(entity));
	REQUIRE(VisualScriptEngine::CreateInstance(entity, graph, {}).IsOk());
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) != 0);
	VisualScriptEngine::InvokeOnCreate(entity);
	CHECK(ReadVariable(entity, "Count") == ScriptFieldValue::FromInt32(4));
}

TEST_CASE("VisualScriptEngine: development-only nodes run unless turned off before graphs load, until ScriptEngine::Shutdown")
{
	Testing::AssetManagerScope assets;
	VisualScriptDocument document = MakeCountDocument(5);
	document.Graph.Nodes[1].Enabled = VisualScriptNodeEnabled::DevelopmentOnly;
	Ref<VisualScriptAsset> const asset = CreateRef<VisualScriptAsset>(document);
	AssetHandle const graph = AssetManager::AddMemoryAsset(asset, "Development");

	// They run without the runtime and once it starts, as in the editor and in projects.
	CHECK(VisualScriptEngine::AreDevelopmentNodesEnabled());
	{
		Testing::ScriptRuntimeScope runtime;
		SentDocuments sent;
		CHECK(VisualScriptEngine::AreDevelopmentNodesEnabled());
		// Turned off, as the player does for exported games: the runtime receives every document with its development-only
		// nodes disabled, while the engine keeps the asset's documents as they are.
		VisualScriptEngine::SetDevelopmentNodesEnabled(false);
		CHECK_FALSE(VisualScriptEngine::AreDevelopmentNodesEnabled());
		uint64_t const logStart = Log::GetNextEntryIndex();
		REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
		CHECK_FALSE(Testing::WasLogged(logStart, "Visual script 'Development'"));
		REQUIRE(sent.GetCount() == 1);
		CHECK(sent.Get(0) == DisableDevelopmentOnlyNodes(document));
		CHECK(VisualScriptEngine::HasWorkingProgram(graph));
		CHECK(VisualScriptEngine::GetWorkingDocument(graph) == asset->GetSharedDocument());

		// So is the working document sent again first after a game-assembly change, and the edit sent after it.
		REQUIRE(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
		VisualScriptDocument edited = document;
		edited.Graph.Nodes[1].Inputs["Value"] = 6;
		CHECK(SendsDocument(*asset, graph, edited));
		REQUIRE(sent.GetCount() == 3);
		CHECK(sent.Get(1) == DisableDevelopmentOnlyNodes(document));
		CHECK(sent.Get(2) == DisableDevelopmentOnlyNodes(edited));
		CHECK(VisualScriptEngine::GetWorkingDocument(graph) == asset->GetSharedDocument());
	}

	// ScriptEngine::Shutdown turns them on again: the runtime receives the document as it is.
	CHECK(VisualScriptEngine::AreDevelopmentNodesEnabled());
	Testing::ScriptRuntimeScope again;
	SentDocuments sent;
	CHECK(VisualScriptEngine::AreDevelopmentNodesEnabled());
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	REQUIRE(sent.GetCount() == 1);
	CHECK(sent.Get(0) == asset->GetDocument());
	CHECK(asset->GetDocument().Graph.Nodes[1].Enabled == VisualScriptNodeEnabled::DevelopmentOnly);
	CHECK(VisualScriptEngine::HasWorkingProgram(graph));
}

TEST_CASE("VisualScriptEngine: a development-only node runs by default, and once development nodes are turned off the flow passes "
          "through it")
{
	Testing::AssetManagerScope assets;
	// Create -> Set Count to 5 (development only) -> Set Done to true.
	VisualScriptDocument document = MakeCountDocument(5);
	document.Variables.push_back(MakeVariable(2, "Done", "bool", false, true));
	document.Graph.Nodes[1].Enabled = VisualScriptNodeEnabled::DevelopmentOnly;
	document.Graph.Nodes.push_back(MakeNode(12, "Variable.Set", Json::object({{"Variable", 2}}), Json::object({{"Value", true}})));
	document.Graph.Links.push_back(MakeLink(11, "Then", 12, "Execute"));
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(document), "Development");
	UUID const entity(41);
	auto const run = [graph, entity]
	{
		RecordingHost host;
		REQUIRE(VisualScriptEngine::CreateInstance(entity, graph, {}).IsOk());
		VisualScriptEngine::InvokeOnCreate(entity);
		std::pair<ScriptFieldValue, ScriptFieldValue> const values = {ReadVariable(entity, "Count"), ReadVariable(entity, "Done")};
		VisualScriptEngine::DestroyInstance(entity);
		CHECK(host.Exceptions == 0);
		return values;
	};

	// In the editor and in projects, it runs.
	{
		Testing::ScriptRuntimeScope runtime;
		CHECK(run() == std::pair(ScriptFieldValue::FromInt32(5), ScriptFieldValue::FromBool(true)));
	}
	// In exported games, it is skipped and the flow goes on after it.
	{
		Testing::ScriptRuntimeScope runtime;
		VisualScriptEngine::SetDevelopmentNodesEnabled(false);
		CHECK(run() == std::pair(ScriptFieldValue::FromInt32(0), ScriptFieldValue::FromBool(true)));
	}
}

TEST_CASE("VisualScriptEngine: a development-only node of a type the runtime does not know breaks its graph, unless development "
          "nodes are turned off")
{
	Testing::AssetManagerScope assets;
	// Create -> Set Count to 3 -> an unknown node (development only).
	VisualScriptDocument document = MakeCountDocument(3);
	VisualScriptNode unknown = MakeNode(12, "Sample.NoSuchNode");
	unknown.Enabled = VisualScriptNodeEnabled::DevelopmentOnly;
	document.Graph.Nodes.push_back(std::move(unknown));
	document.Graph.Links.push_back(MakeLink(11, "Then", 12, "Execute"));
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(document), "Unknown");
	UUID const entity(42);

	// Enabled in development, its type is an Error: the graph does not run.
	{
		Testing::ScriptRuntimeScope runtime;
		RecordingHost host;
		uint64_t const logStart = Log::GetNextEntryIndex();
		REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
		CHECK(
			Testing::WasLogged(logStart, "Visual script 'Unknown' has 2 errors; first: unknown node type \"Sample.NoSuchNode\" (node 12)"));
		CHECK_FALSE(VisualScriptEngine::HasWorkingProgram(graph));
		CHECK(VisualScriptEngine::CreateInstance(entity, graph, {}).IsError());
	}
	// Disabled in exported games, its type is only a Warning: the graph runs up to it.
	{
		Testing::ScriptRuntimeScope runtime;
		RecordingHost host;
		VisualScriptEngine::SetDevelopmentNodesEnabled(false);
		uint64_t const logStart = Log::GetNextEntryIndex();
		REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
		CHECK_FALSE(Testing::WasLogged(logStart, "Visual script 'Unknown'"));
		CHECK(VisualScriptEngine::HasWorkingProgram(graph));
		REQUIRE(VisualScriptEngine::CreateInstance(entity, graph, {}).IsOk());
		VisualScriptEngine::InvokeOnCreate(entity);
		CHECK(ReadVariable(entity, "Count") == ScriptFieldValue::FromInt32(3));
		VisualScriptEngine::DestroyInstance(entity);
		CHECK(host.Exceptions == 0);
	}
}

TEST_CASE("VisualScriptEngine: the working document goes first again only when it is another program, so none compiles twice")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	Ref<VisualScriptAsset> const asset = CreateRef<VisualScriptAsset>(MakeCountDocument(1));
	AssetHandle const graph = AssetManager::AddMemoryAsset(asset, "Counter");
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(SendsDocument(*asset, graph, MakeBrokenDocument()));
	SentDocuments sent;

	// After a game-assembly change only compiling the working document again tells whether it still runs, so it goes
	// before the next document.
	REQUIRE(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
	VisualScriptDocument otherBroken = MakeBrokenDocument();
	otherBroken.Graph.Nodes[1].Inputs["Value"] = 3;
	CHECK(SendsDocument(*asset, graph, otherBroken));
	REQUIRE(sent.GetCount() == 2);
	CHECK(sent.Get(0) == MakeCountDocument(1));
	CHECK(sent.Get(1) == otherBroken);
	// Neither refers to another graph: no references go with them.
	CHECK(sent.GetReferences(0).empty());
	CHECK(sent.GetReferences(1).empty());
	CHECK(VisualScriptEngine::HasWorkingProgram(graph));

	// A document of the working program tells that itself: the program is compiled once.
	REQUIRE(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
	VisualScriptDocument moved = MakeCountDocument(1);
	moved.Graph.Nodes[1].Position = glm::vec2(240.0f, 0.0f);
	CHECK(SendsDocument(*asset, graph, moved));
	REQUIRE(sent.GetCount() == 3);
	CHECK(sent.Get(2) == moved);
	CHECK(VisualScriptEngine::HasWorkingProgram(graph));
	CHECK(VisualScriptEngine::GetWorkingDocument(graph) == asset->GetSharedDocument());
}

TEST_CASE("VisualScriptEngine: DisableDevelopmentOnlyNodes disables the development-only nodes of every graph and nothing else")
{
	// Nodes of every state with every node key of version 2, and a development-only node in a function too.
	VisualScriptDocument document = ReadFixture("Editor.json");
	REQUIRE_FALSE(document.Functions.empty());
	REQUIRE_FALSE(document.Functions[0].Graph.Nodes.empty());
	document.Functions[0].Graph.Nodes.back().Enabled = VisualScriptNodeEnabled::DevelopmentOnly;
	VisualScriptDocument disabled = DisableDevelopmentOnlyNodes(document);

	// Each development-only node is disabled and every other node keeps its state; given back the states they had, the
	// nodes leave nothing different. Answers how many nodes were development-only.
	auto const restore = [](VisualScriptGraph const& before, VisualScriptGraph& after)
	{
		size_t developmentOnly = 0;
		REQUIRE(after.Nodes.size() == before.Nodes.size());
		for (size_t index = 0; index < before.Nodes.size(); index++)
		{
			VisualScriptNodeEnabled const state = before.Nodes[index].Enabled;
			bool const isDevelopmentOnly = state == VisualScriptNodeEnabled::DevelopmentOnly;
			CAPTURE(before.Nodes[index].ID);
			CHECK(after.Nodes[index].Enabled == (isDevelopmentOnly ? VisualScriptNodeEnabled::Disabled : state));
			after.Nodes[index].Enabled = state;
			developmentOnly += isDevelopmentOnly ? 1 : 0;
		}
		return developmentOnly;
	};
	CHECK(restore(document.Graph, disabled.Graph) > 0);
	REQUIRE(disabled.Functions.size() == document.Functions.size());
	CHECK(restore(document.Functions[0].Graph, disabled.Functions[0].Graph) > 0);
	CHECK(disabled == document);
	CHECK(HaveSameVisualScriptProgram(disabled, document));

	// A document without them comes back as it was.
	CHECK(DisableDevelopmentOnlyNodes(MakeCountDocument(1)) == MakeCountDocument(1));

	// A library stays a library.
	VisualScriptDocument const library = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
	CHECK(DisableDevelopmentOnlyNodes(library).Kind == VisualScriptDocumentKind::Library);
}

TEST_CASE("VisualScriptEngine: graph instances run their events, hold their variables and report failures")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	Scene scene;
	UUID const id = scene.CreateEntity("Counter").GetUUID();
	UUID const other = scene.CreateEntity("Other").GetUUID();
	SceneContextScope context(scene);
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeEventCounterDocument()), "Events");

	// Created with the component's values: Updates starts at 10; values of variables it does not have are skipped.
	REQUIRE(VisualScriptEngine::CreateInstance(
				id, graph, {{"Updates", ScriptFieldValue::FromInt32(10)}, {"Missing", ScriptFieldValue::FromBool(true)}})
	            .IsOk());
	CHECK(VisualScriptEngine::HasInstance(id));
	CHECK_FALSE(VisualScriptEngine::HasInstance(other));
	CHECK(VisualScriptEngine::GetInstanceCount() == 1);
	CHECK(ScriptEngine::GetInstanceCount() == 1);

	VisualScriptEngine::InvokeOnCreate(id);
	VisualScriptEngine::InvokeOnUpdate(id, 0.25f);
	VisualScriptEngine::InvokeOnUpdate(id, 0.25f);
	VisualScriptEngine::InvokeOnFixedUpdate(id, 1.0f / 60.0f);
	for (ContactEventType const type :
	     {ContactEventType::CollisionEnter, ContactEventType::CollisionExit, ContactEventType::TriggerEnter, ContactEventType::TriggerExit})
	{
		VisualScriptEngine::InvokeContactEvent(id, other, type);
	}
	VisualScriptEngine::InvokeOnClick(id, other);
	// Entities without an instance are skipped.
	VisualScriptEngine::InvokeOnUpdate(other, 0.25f);
	Result<ScriptFieldMap> variables = VisualScriptEngine::GetInstanceVariables(id);
	REQUIRE_MESSAGE(variables.IsOk(), variables.GetError());
	CHECK(variables.GetValue() == ScriptFieldMap{{"Clicks", ScriptFieldValue::FromInt32(1)},
	                                             {"Contacts", ScriptFieldValue::FromInt32(4)},
	                                             {"Creates", ScriptFieldValue::FromInt32(1)},
	                                             {"FixedUpdates", ScriptFieldValue::FromInt32(1)},
	                                             {"Updates", ScriptFieldValue::FromInt32(12)}});
	REQUIRE(VisualScriptEngine::SetInstanceVariables(id, {{"Updates", ScriptFieldValue::FromInt32(100)}}).IsOk());
	VisualScriptEngine::InvokeOnUpdate(id, 0.25f);
	CHECK(ReadVariable(id, "Updates") == ScriptFieldValue::FromInt32(101));
	CHECK(VisualScriptEngine::GetInstanceVariables(other).GetError() == fmt::format("entity {} runs no visual script", other));
	CHECK(VisualScriptEngine::SetInstanceVariables(other, {}).GetError() == fmt::format("entity {} runs no visual script", other));

	// While only a graph instance runs, the game's scripts cannot change.
	Result<void> const replaced = ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath());
	REQUIRE(replaced.IsError());
	CHECK(replaced.GetError() == "the game's scripts cannot be replaced while a scene runs scripts or visual scripts");

	// Destroy runs Event.Destroy, then the instance is gone.
	uint64_t logStart = Log::GetNextEntryIndex();
	VisualScriptEngine::InvokeOnDestroy(id);
	CHECK(Testing::WasLogged(logStart, "the counting graph is destroyed"));
	CHECK_FALSE(VisualScriptEngine::HasInstance(id));
	CHECK(ScriptEngine::GetInstanceCount() == 0);
	CHECK(host.Exceptions == 0);

	// A graph that fails is a script exception; its instance runs on.
	AssetHandle const division = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeDivisionDocument()), "Division");
	REQUIRE(VisualScriptEngine::CreateInstance(id, division, {}).IsOk());
	logStart = Log::GetNextEntryIndex();
	VisualScriptEngine::InvokeOnCreate(id);
	CHECK(host.Exceptions == 1);
	CHECK(Testing::WasLogged(logStart, "integer division by zero"));
	CHECK(VisualScriptEngine::HasInstance(id));

	// An instance the runtime no longer has is forgotten when the runtime says so, without a script exception.
	ScriptEngineInternal::ScriptEngineData const* const data = ScriptEngineInternal::GetData();
	REQUIRE(data != nullptr);
	auto const destroyInRuntime = [data](UUID entity)
	{
		CHECK(static_cast<ScriptEngineInternal::ScriptStatus>(data->Host.VisualScriptDestroyInstance(entity.GetValue())) ==
		      ScriptEngineInternal::ScriptStatus::Success);
		CHECK(VisualScriptEngine::HasInstance(entity));
	};
	destroyInRuntime(id);
	VisualScriptEngine::InvokeOnUpdate(id, 0.25f);
	CHECK_FALSE(VisualScriptEngine::HasInstance(id));
	REQUIRE(VisualScriptEngine::CreateInstance(id, graph, {}).IsOk());
	destroyInRuntime(id);
	CHECK(VisualScriptEngine::GetInstanceVariables(id).GetError() == fmt::format("entity {} runs no visual script", id));
	CHECK_FALSE(VisualScriptEngine::HasInstance(id));
	REQUIRE(VisualScriptEngine::CreateInstance(id, graph, {}).IsOk());
	destroyInRuntime(id);
	CHECK(VisualScriptEngine::SetInstanceVariables(id, {}).GetError() == fmt::format("entity {} runs no visual script", id));
	CHECK_FALSE(VisualScriptEngine::HasInstance(id));
	CHECK(host.Exceptions == 1);

	// Destroyed one by one, or all at once, without Event.Destroy.
	REQUIRE(VisualScriptEngine::CreateInstance(id, graph, {}).IsOk());
	REQUIRE(VisualScriptEngine::CreateInstance(other, graph, {}).IsOk());
	CHECK(VisualScriptEngine::GetInstanceCount() == 2);
	logStart = Log::GetNextEntryIndex();
	VisualScriptEngine::DestroyInstance(id);
	CHECK_FALSE(VisualScriptEngine::HasInstance(id));
	CHECK(VisualScriptEngine::HasInstance(other));
	VisualScriptEngine::DestroyAllInstances();
	CHECK(VisualScriptEngine::GetInstanceCount() == 0);
	CHECK_FALSE(Testing::WasLogged(logStart, "the counting graph is destroyed"));
	CHECK(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
}

TEST_CASE("VisualScriptEngine: the shared fixture documents run on entities with and without children and bodies")
{
	Testing::PhysicsSystemScope physics;
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	InputScope input;
	Scene scene;
	Entity const plain = scene.CreateEntity("Plain");
	Entity body = scene.CreateEntity("Body");
	body.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
	body.AddComponent<BoxColliderComponent>();
	scene.CreateEntity("Left", body);
	scene.CreateEntity("Right", body);
	// A simulated scene, so the graphs' forces and raycasts reach its physics.
	scene.OnRuntimeStart();
	SceneContextScope context(scene);

	// Editor.json has disabled, development-only and split nodes.
	constexpr std::array<std::string_view, 7> Documents = {"Data.json",      "Editor.json",    "Events.json",   "Flow.json",
	                                                       "Functions.json", "Reflected.json", "Variables.json"};
	uint64_t const logStart = Log::GetNextEntryIndex();
	for (std::string_view const name : Documents)
	{
		CAPTURE(name);
		AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(ReadFixture(name)), std::string(name));
		REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
		for (UUID const id : {plain.GetUUID(), body.GetUUID()})
		{
			UUID const other = id == plain.GetUUID() ? body.GetUUID() : plain.GetUUID();
			REQUIRE(VisualScriptEngine::CreateInstance(id, graph, {}).IsOk());
			VisualScriptEngine::InvokeOnCreate(id);
			for (int frame = 0; frame < 30; frame++)
			{
				VisualScriptEngine::InvokeOnUpdate(id, 0.05f);
				VisualScriptEngine::InvokeOnFixedUpdate(id, 1.0f / 60.0f);
			}
			for (ContactEventType const type : {ContactEventType::CollisionEnter, ContactEventType::CollisionExit,
			                                    ContactEventType::TriggerEnter, ContactEventType::TriggerExit})
			{
				VisualScriptEngine::InvokeContactEvent(id, other, type);
			}
			VisualScriptEngine::InvokeOnClick(id, other);
			VisualScriptEngine::InvokeOnDestroy(id);
			CHECK_FALSE(VisualScriptEngine::HasInstance(id));
		}
	}
	scene.OnRuntimeStop();
	CHECK(host.Exceptions == 0);
	CHECK_FALSE(Testing::WasLogged(logStart, "Visual script '"));
	CHECK_FALSE(Testing::WasLogged(logStart, "Scripting runtime:"));
	CHECK_FALSE(Testing::WasLogged(logStart, "has no physics body"));
	CHECK_FALSE(host.Results.empty());
	for (ScriptTestResult const& result : host.Results)
	{
		CAPTURE(result.Name);
		CHECK(result.Passed);
	}
}

TEST_CASE("VisualScriptEngine: graph instances block game-assembly loads, and a reload leaves no graph holding the game's types")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	RecordingHost host;
	Scene scene;
	UUID const id = scene.CreateEntity("Mover").GetUUID();
	SceneContextScope context(scene);
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeMoverDocument()), "Mover check");

	REQUIRE(ScriptEngine::CreateInstance(id, "Strada.Tests.Mover", {}).IsOk());
	REQUIRE(VisualScriptEngine::CreateInstance(id, graph, {}).IsOk());
	CHECK(ScriptEngine::GetInstanceCount() == 2);
	CHECK(VisualScriptEngine::GetInstanceCount() == 1);
	VisualScriptEngine::InvokeOnCreate(id);
	// The graph found the entity's Mover script and kept it in a variable of the game's type.
	CHECK(ReadVariable(id, "IsMover") == ScriptFieldValue::FromBool(true));

	// A graph instance alone keeps the game's scripts in place.
	CHECK(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsError());
	ScriptEngine::DestroyInstance(id);
	CHECK(ScriptEngine::GetInstanceCount() == 1);
	CHECK(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsError());
	VisualScriptEngine::DestroyInstance(id);
	CHECK(ScriptEngine::GetInstanceCount() == 0);

	// Loaded again: nothing holds the previous assembly, and the graph compiles again from its document (generations change
	// so entities retry; the document is not sent again).
	uint64_t const generation = VisualScriptEngine::GetProgramGeneration(graph);
	uint64_t const logStart = Log::GetNextEntryIndex();
	REQUIRE(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
	CHECK_FALSE(Testing::WasLogged(logStart, "still referenced"));
	uint64_t const reloaded = VisualScriptEngine::GetProgramGeneration(graph);
	CHECK(reloaded != generation);
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) == reloaded);
	REQUIRE(ScriptEngine::CreateInstance(id, "Strada.Tests.Mover", {}).IsOk());
	REQUIRE(VisualScriptEngine::CreateInstance(id, graph, {}).IsOk());
	VisualScriptEngine::InvokeOnCreate(id);
	CHECK(ReadVariable(id, "IsMover") == ScriptFieldValue::FromBool(true));
	CHECK(host.Exceptions == 0);

	// Unloading the game's scripts takes the graph instances with it.
	ScriptEngine::UnloadGameAssembly();
	CHECK_FALSE(VisualScriptEngine::HasInstance(id));
	CHECK(ScriptEngine::GetInstanceCount() == 0);
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) != reloaded);
}

TEST_CASE("VisualScriptEngine: a graph that needs the game's types runs once they load, and stops when they go")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	Scene scene;
	UUID const id = scene.CreateEntity("Mover").GetUUID();
	SceneContextScope context(scene);
	Ref<VisualScriptAsset> const asset = CreateRef<VisualScriptAsset>(MakeMoverDocument());
	AssetHandle const graph = AssetManager::AddMemoryAsset(asset, "Mover check");

	// Without the game's scripts Strada.Tests.Mover is no type: the graph cannot run.
	uint64_t logStart = Log::GetNextEntryIndex();
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(Testing::WasLogged(logStart, "Visual script 'Mover check' has "));
	CHECK(VisualScriptEngine::GetWorkingDocument(graph) == nullptr);
	CHECK_FALSE(VisualScriptEngine::HasWorkingProgram(graph));
	CHECK(VisualScriptEngine::CreateInstance(id, graph, {}).IsError());
	CHECK(host.Exceptions == 1);
	uint64_t const before = VisualScriptEngine::GetProgramGeneration(graph);

	// With them its document compiles: it is sent again (the generation tells entities to try again), so the engine knows
	// what instances run, and it runs. A broken edit then leaves it running.
	REQUIRE(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
	uint64_t const reloaded = VisualScriptEngine::GetProgramGeneration(graph);
	CHECK(reloaded != before);
	logStart = Log::GetNextEntryIndex();
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) != reloaded);
	CHECK_FALSE(Testing::WasLogged(logStart, "Mover check"));
	Ref<VisualScriptDocument const> const working = VisualScriptEngine::GetWorkingDocument(graph);
	REQUIRE(working != nullptr);
	CHECK(*working == MakeMoverDocument());
	CHECK(VisualScriptEngine::HasWorkingProgram(graph));
	REQUIRE(VisualScriptEngine::CreateInstance(id, graph, {}).IsOk());
	VisualScriptEngine::InvokeOnCreate(id);
	CHECK(ReadVariable(id, "IsMover") == ScriptFieldValue::FromBool(false));
	VisualScriptEngine::DestroyInstance(id);
	logStart = Log::GetNextEntryIndex();
	asset->SetDocument(MakeBrokenDocument());
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(Testing::WasLogged(logStart, "Visual script 'Mover check' has 1 error; first: "));
	CHECK(Testing::WasLogged(logStart, "; the previous version keeps running"));
	CHECK(*VisualScriptEngine::GetWorkingDocument(graph) == MakeMoverDocument());
	CHECK(VisualScriptEngine::HasWorkingProgram(graph));
	asset->SetDocument(MakeMoverDocument());
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());

	// Loaded again, a broken edit leaves the program running only if it still compiles, which the engine learns by sending
	// its document again first, before any instance exists. Until then the engine keeps the program's document but does
	// not count on it running.
	REQUIRE(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
	CHECK(VisualScriptEngine::GetWorkingDocument(graph) != nullptr);
	CHECK_FALSE(VisualScriptEngine::HasWorkingProgram(graph));
	logStart = Log::GetNextEntryIndex();
	asset->SetDocument(MakeBrokenDocument());
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(Testing::WasLogged(logStart, "Visual script 'Mover check' has 1 error; first: "));
	CHECK(Testing::WasLogged(logStart, "; the previous version keeps running"));
	CHECK(VisualScriptEngine::HasWorkingProgram(graph));
	REQUIRE(VisualScriptEngine::CreateInstance(id, graph, {}).IsOk());
	VisualScriptEngine::DestroyInstance(id);
	CHECK(host.Exceptions == 1);

	// Unloaded, it no longer compiles: a broken edit right after leaves nothing running, and says so.
	ScriptEngine::UnloadGameAssembly();
	CHECK_FALSE(VisualScriptEngine::HasWorkingProgram(graph));
	logStart = Log::GetNextEntryIndex();
	VisualScriptDocument otherBroken = MakeBrokenDocument();
	otherBroken.Graph.Nodes[1].Inputs["Value"] = 3;
	asset->SetDocument(otherBroken);
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(Testing::WasLogged(logStart, "Mover check was compiled again for the changed game assembly: "));
	CHECK(Testing::WasLogged(logStart, "Visual script 'Mover check' has 1 error; first: "));
	CHECK_FALSE(Testing::WasLogged(logStart, "keeps running"));
	CHECK(VisualScriptEngine::GetWorkingDocument(graph) == nullptr);
	CHECK_FALSE(VisualScriptEngine::HasWorkingProgram(graph));
	CHECK(VisualScriptEngine::CreateInstance(id, graph, {}).IsError());
	CHECK(host.Exceptions == 2);
	asset->SetDocument(MakeMoverDocument());
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	REQUIRE(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
	REQUIRE(VisualScriptEngine::CreateInstance(id, graph, {}).IsOk());
	CHECK(VisualScriptEngine::HasWorkingProgram(graph));
	VisualScriptEngine::DestroyInstance(id);

	// Without them again it cannot compile, so nothing keeps running when a broken edit comes.
	ScriptEngine::UnloadGameAssembly();
	logStart = Log::GetNextEntryIndex();
	CHECK(VisualScriptEngine::CreateInstance(id, graph, {}).IsError());
	CHECK(host.Exceptions == 3);
	CHECK(Testing::WasLogged(logStart, "Mover check was compiled again for the changed game assembly: "));
	asset->SetDocument(MakeBrokenDocument());
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(Testing::WasLogged(logStart, "Visual script 'Mover check' has 1 error; first: "));
	CHECK_FALSE(Testing::WasLogged(logStart, "keeps running"));

	// Working again under a broken edit, then unloaded: the working program with a node moved is sent alone, and since it
	// does not compile, the working program does not either, so nothing runs.
	REQUIRE(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
	asset->SetDocument(MakeMoverDocument());
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	asset->SetDocument(MakeBrokenDocument());
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(VisualScriptEngine::HasWorkingProgram(graph));
	ScriptEngine::UnloadGameAssembly();
	logStart = Log::GetNextEntryIndex();
	VisualScriptDocument moved = MakeMoverDocument();
	moved.Graph.Nodes[1].Position = glm::vec2(240.0f, 0.0f);
	asset->SetDocument(moved);
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(Testing::WasLogged(logStart, "Mover check was compiled again for the changed game assembly: "));
	CHECK(Testing::WasLogged(logStart, "Visual script 'Mover check' has 2 errors; first: "));
	CHECK_FALSE(Testing::WasLogged(logStart, "keeps running"));
	CHECK(VisualScriptEngine::GetWorkingDocument(graph) == nullptr);
	CHECK_FALSE(VisualScriptEngine::HasWorkingProgram(graph));
	CHECK(host.Exceptions == 3);
}

TEST_CASE("VisualScriptEngine: the projects' scene graphs load strictly, analyze cleanly and resolve exactly or by redirect")
{
	std::filesystem::path const projectsDirectory = FileSystem::PathFromUtf8(STRADA_PROJECTS_DIR);
	std::vector<std::filesystem::path> projects;
	for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(projectsDirectory))
	{
		if (FileSystem::Exists(entry.path() / "Assets" / AssetRegistry::FileName))
		{
			projects.push_back(entry.path());
		}
	}
	std::sort(projects.begin(), projects.end());
	REQUIRE_FALSE(projects.empty());

	Testing::ScriptRuntimeScope runtime;
	size_t checked = 0;
	for (std::filesystem::path const& project : projects)
	{
		std::string const name = FileSystem::PathToUtf8(project.filename());
		CAPTURE(name);
		std::filesystem::path const assetsDirectory = project / "Assets";
		Result<AssetRegistry> registry = AssetRegistry::LoadFromFile(assetsDirectory / AssetRegistry::FileName, DeserializationContext{});
		REQUIRE_MESSAGE(registry.IsOk(), registry.GetError());
		AssetRegistry const& assets = registry.GetValue();

		// The graphs of the scenes' VisualScript components, by path.
		std::set<std::string> graphs;
		for (AssetMetadata const& scene : assets.GetAll())
		{
			if (scene.Type != AssetType::Scene)
			{
				continue;
			}
			CAPTURE(scene.Path);
			Result<std::string> text = FileSystem::ReadTextFile(assetsDirectory / FileSystem::PathFromUtf8(scene.Path));
			REQUIRE(text.IsOk());
			Result<Json> document = ParseJson(text.GetValue());
			REQUIRE_MESSAGE(document.IsOk(), document.GetError());
			for (Json const& entity : document.GetValue().at("Entities"))
			{
				auto const component = entity.at("Components").find("VisualScript");
				if (component == entity.at("Components").end())
				{
					continue;
				}
				AssetHandle handle;
				REQUIRE(JsonTraits<AssetHandle>::FromJson(component->at("Graph"), handle, DeserializationContext{}).IsOk());
				if (!handle.IsValid())
				{
					continue;
				}
				AssetMetadata const* graph = assets.Find(handle);
				REQUIRE_MESSAGE(graph != nullptr, handle.ToString());
				CHECK(graph->Type == AssetType::VisualScript);
				graphs.insert(graph->Path);
			}
		}
		if (graphs.empty())
		{
			continue;
		}

		// Graphs may use the project's script classes as types: analyze with its scripts, as CMake builds them.
		std::filesystem::path const scripts = FileSystem::GetExecutableDirectory() / project.filename() / (name + ".dll");
		bool const hasScripts = FileSystem::Exists(project / "Scripts" / (name + ".csproj"));
		if (hasScripts)
		{
			Result<void> const loaded = ScriptEngine::LoadGameAssembly(scripts);
			REQUIRE_MESSAGE(loaded.IsOk(), loaded.GetError());
		}
		for (std::string const& path : graphs)
		{
			CAPTURE(path);
			std::vector<VisualScriptIssue> issues;
			Result<VisualScriptDocument> document =
				VisualScriptSerializer::LoadFromFile(assetsDirectory / FileSystem::PathFromUtf8(path), MakeStrictContext(), &issues);
			REQUIRE_MESSAGE(document.IsOk(), document.GetError());
			CHECK_MESSAGE(issues.empty(), DescribeIssues(issues));
			Result<VisualScriptAnalysis> analysis = VisualScriptEngine::Analyze(document.GetValue());
			REQUIRE_MESSAGE(analysis.IsOk(), analysis.GetError());
			for (VisualScriptDiagnostic const& diagnostic : analysis.GetValue().Diagnostics)
			{
				CAPTURE(diagnostic.Code);
				CAPTURE(diagnostic.Message);
				CHECK(diagnostic.Severity == VisualScriptSeverity::Warning);
			}
			// A node resolved any other way would be an API change without a redirect.
			for (VisualScriptAnalyzedNode const& node : analysis.GetValue().Nodes)
			{
				CAPTURE(node.Type);
				CHECK((node.Resolved == VisualScriptResolution::Exact || node.Resolved == VisualScriptResolution::Redirect));
			}
			checked++;
		}
		if (hasScripts)
		{
			ScriptEngine::UnloadGameAssembly();
		}
	}
	// The feature-test project runs one at least.
	CHECK(checked > 0);
}
