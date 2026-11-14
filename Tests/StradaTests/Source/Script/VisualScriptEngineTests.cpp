#include "Fuzzing.h"
#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "Script/VisualScriptGridGraph.h"
#include "Script/VisualScriptTestGraphs.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/AssetRegistry.h"
#include "Strada/Asset/CurveAsset.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Platform.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/ScriptEngineInternal.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Script/VisualScriptLiterals.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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

	// Holds an array of a game enum (Levels, private) and calls a game member: on Event.Create the entity's Mover nudges it 2
	// units, and Count (public) becomes the array's length.
	VisualScriptDocument MakeNudgeDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Levels", "Strada.Tests.Difficulty[]", Json::array({"Easy", "Hard"}), false));
		document.Variables.push_back(MakeVariable(2, "Count", "int", 0, true));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
		document.Graph.Nodes.push_back(
			MakeNode(11, "Call:Strada.Tests.Mover.Nudge(float)", Json::object(), Json::object({{"Distance", 2.0}})));
		document.Graph.Nodes.push_back(MakeNode(12, "Variable.Set", Json::object({{"Variable", 2}})));
		document.Graph.Nodes.push_back(MakeNode(13, "Array.Length"));
		document.Graph.Nodes.push_back(MakeNode(14, "Variable.Get", Json::object({{"Variable", 1}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		document.Graph.Links.push_back(MakeLink(11, "Then", 12, "Execute"));
		document.Graph.Links.push_back(MakeLink(14, "Value", 13, "Array"));
		document.Graph.Links.push_back(MakeLink(13, "Length", 12, "Value"));
		return document;
	}

	using Testing::DescribeIssues;
	using Testing::MakeStrictContext;

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

	// Stands in for the runtime's VisualScriptTakeLiveEvents while it lives: it fails, answers a text that is not JSON, or
	// passes the call on, and records the serial each call takes the events after. Lives within a Testing::ScriptRuntimeScope,
	// one at a time.
	class LiveEventsStub
	{
	public:
		enum class Answer : uint8_t
		{
			Failure,
			Damaged,
			Pass
		};

		LiveEventsStub()
		{
			ScriptEngineInternal::ScriptEngineData* const data = ScriptEngineInternal::GetData();
			REQUIRE(data != nullptr);
			REQUIRE(s_Take == nullptr);
			s_Answer = Answer::Failure;
			s_Afters.clear();
			s_Take = std::exchange(data->Host.VisualScriptTakeLiveEvents, &Take);
		}

		~LiveEventsStub()
		{
			if (ScriptEngineInternal::ScriptEngineData* const data = ScriptEngineInternal::GetData(); data != nullptr)
			{
				data->Host.VisualScriptTakeLiveEvents = s_Take;
			}
			s_Take = nullptr;
			s_Afters.clear();
		}

		LiveEventsStub(LiveEventsStub const&) = delete;
		LiveEventsStub& operator=(LiveEventsStub const&) = delete;

		void SetAnswer(Answer answer) { s_Answer = answer; }

		// The serials the calls since the last TakeAfters took the events after.
		std::vector<uint64_t> TakeAfters() { return std::exchange(s_Afters, {}); }

	private:
		static int32_t Take(uint64_t after, void* context, ScriptEngineInternal::ReceiveFunction receive)
		{
			s_Afters.push_back(after);
			switch (s_Answer)
			{
				case Answer::Failure:
					return static_cast<int32_t>(ScriptEngineInternal::ScriptStatus::Failure);
				case Answer::Damaged:
				{
					constexpr std::string_view Damaged = R"({"Last":)";
					receive(context, Damaged.data(), static_cast<int32_t>(Damaged.size()));
					return static_cast<int32_t>(ScriptEngineInternal::ScriptStatus::Success);
				}
				case Answer::Pass:
					break;
			}
			return s_Take(after, context, receive);
		}

		static inline decltype(ScriptEngineInternal::HostFunctions::VisualScriptTakeLiveEvents) s_Take = nullptr;
		static inline Answer s_Answer = Answer::Failure;
		static inline std::vector<uint64_t> s_Afters;
	};

	// A receipt of the live log as the runtime's events give it, completed as the engine completes them.
	VisualScriptLiveEdit MakeReceipt(uint64_t serial, AssetHandle owner, VisualScriptLiveOutcome outcome, bool deferred = false,
	                                 AssetHandle causedBy = {})
	{
		VisualScriptLiveEdit edit;
		edit.Serial = serial;
		edit.Graph = owner;
		edit.Name = owner.ToString();
		edit.Outcome = outcome;
		edit.Deferred = deferred;
		edit.Cause = causedBy;
		edit.Instances = outcome == VisualScriptLiveOutcome::Pending ? 0 : 1;
		edit.Pending = outcome == VisualScriptLiveOutcome::Pending ? 1 : 0;
		return edit;
	}

	// A runtime problem at a node of a graph instance, taken at time.
	VisualScriptRuntimeProblem MakeProblem(uint64_t serial, AssetHandle owner, uint32_t node, UUID entity, uint32_t count, double time)
	{
		VisualScriptRuntimeProblem problem;
		problem.Serial = serial;
		problem.Graph = owner;
		problem.Document = owner;
		problem.Node = node;
		problem.Title = "Divide";
		problem.Entity = entity;
		problem.Message = "integer division by zero";
		problem.Count = count;
		problem.FirstTime = time;
		problem.LastTime = time;
		return problem;
	}

	// --- Graphs that refer to graphs ---

	// The function library of the cross-graph tests: the pure Twice(int Value) -> int Result (function 1), Value times
	// factor.
	VisualScriptDocument MakeLibraryDocument(int32_t factor = 2)
	{
		VisualScriptDocument document;
		document.Kind = VisualScriptDocumentKind::Library;
		VisualScriptFunction twice;
		twice.ID = 1;
		twice.Name = "Twice";
		twice.Pure = true;
		twice.Inputs.emplace_back("Value", "int");
		twice.Outputs.emplace_back("Result", "int");
		twice.Graph.Nodes.push_back(MakeNode(10, "Function.Entry"));
		twice.Graph.Nodes.push_back(MakeNode(11, "Operator.Multiply", Json::object(), Json::object({{"B", factor}})));
		twice.Graph.Nodes.push_back(MakeNode(12, "Function.Return"));
		twice.Graph.Links.push_back(MakeLink(10, "Value", 11, "A"));
		twice.Graph.Links.push_back(MakeLink(11, "Result", 12, "Result"));
		document.Functions.push_back(std::move(twice));
		return document;
	}

	// A script with the public float Health (10) and the custom event Hit, which sets Health to 0.
	VisualScriptDocument MakeHealthDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Health", "float", 10.0, true));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Custom", Json::object({{"Name", "Hit"}})));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 1}}), Json::object({{"Value", 0.0}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		return document;
	}

	// Sets the public Count (0) to the library's Twice(3) when created. With variables, it also reads the public Health of
	// that graph on the entity Other (an Interface use); with verified, it then sends Hit to Other, checked against that graph
	// (a Verification use).
	VisualScriptDocument MakeCallerDocument(AssetHandle library, AssetHandle variables = {}, AssetHandle verified = {})
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Count", "int", 0, true));
		document.Variables.push_back(MakeVariable(2, "Other", "Strada.Entity", Json(), false));
		document.Graph.Nodes.push_back(MakeNode(20, "Event.Create"));
		document.Graph.Nodes.push_back(MakeNode(21, "Variable.Set", Json::object({{"Variable", 1}})));
		document.Graph.Nodes.push_back(
			MakeNode(22, "Function.Call", Json::object({{"Function", 1}, {"Graph", library.ToString()}}), Json::object({{"Value", 3}})));
		document.Graph.Links.push_back(MakeLink(20, "Then", 21, "Execute"));
		document.Graph.Links.push_back(MakeLink(22, "Result", 21, "Value"));
		if (variables.IsValid() || verified.IsValid())
		{
			document.Graph.Nodes.push_back(MakeNode(23, "Variable.Get", Json::object({{"Variable", 2}})));
		}
		if (variables.IsValid())
		{
			document.Graph.Nodes.push_back(MakeNode(24, "Variable.Get", Json::object({{"Variable", 1}, {"Graph", variables.ToString()}})));
			document.Graph.Links.push_back(MakeLink(23, "Value", 24, "Target"));
		}
		if (verified.IsValid())
		{
			document.Graph.Nodes.push_back(
				MakeNode(25, "Event.Send", Json::object({{"Name", "Hit"}, {"Parameters", Json::array()}, {"Graph", verified.ToString()}})));
			document.Graph.Links.push_back(MakeLink(21, "Then", 25, "Execute"));
			document.Graph.Links.push_back(MakeLink(23, "Value", 25, "Target"));
		}
		return document;
	}

	// The references a send would carry: {"Self":...,"Graphs":[...]} with the entries given (ascending by handle), each
	// written {"Graph":"<handle>","Name":<name>,"Form":...,"Document":<content form>} or {...,"Error":<why>}.
	struct ExpectedReference
	{
		AssetHandle Graph;
		std::string Name;
		// "Full" or "Interface"; empty for an absent graph.
		std::string Form;
		// The content form sent for the graph, or the Error of an absent one.
		std::string Text;
	};

	std::string WriteExpectedReferences(AssetHandle self, std::vector<ExpectedReference> entries)
	{
		std::sort(entries.begin(), entries.end(),
		          [](ExpectedReference const& a, ExpectedReference const& b)
		          {
					  return a.Graph.GetUUID().GetValue() < b.Graph.GetUUID().GetValue();
				  });
		std::string text = fmt::format(R"({{"Self":"{}","Graphs":[)", self.ToString());
		for (size_t index = 0; index < entries.size(); index++)
		{
			ExpectedReference const& entry = entries[index];
			text += fmt::format(R"({}{{"Graph":"{}","Name":{})", index == 0 ? "" : ",", entry.Graph.ToString(), Json(entry.Name).dump());
			text += entry.Form.empty() ? fmt::format(R"(,"Error":{}}})", Json(entry.Text).dump())
			                           : fmt::format(R"(,"Form":"{}","Document":{}}})", entry.Form, entry.Text);
		}
		return text + "]}";
	}

	// --- The live channel ---

	// The receipt the graph's last send made: the newest receipt of the graph in the engine's live log.
	Ref<VisualScriptLiveEdit const> FindLastReceipt(AssetHandle graph)
	{
		Ref<VisualScriptLiveLog const> const log = VisualScriptEngine::GetLiveLog();
		REQUIRE(log != nullptr);
		auto const found = std::find_if(log->Edits.rbegin(), log->Edits.rend(),
		                                [graph](Ref<VisualScriptLiveEdit const> const& edit)
		                                {
											return edit->Graph == graph;
										});
		REQUIRE(found != log->Edits.rend());
		return *found;
	}

	// Every event the runtime's live log keeps, as it writes them for the engine (the reader's input).
	std::string WriteLiveEvents()
	{
		ScriptEngineInternal::ScriptEngineData* const data = ScriptEngineInternal::GetData();
		REQUIRE(data != nullptr);
		std::string text;
		auto const status = static_cast<ScriptEngineInternal::ScriptStatus>(
			data->Host.VisualScriptTakeLiveEvents(0, &text, &ScriptEngineInternal::ReceiveText));
		REQUIRE(status == ScriptEngineInternal::ScriptStatus::Success);
		return text;
	}

	// Divides 1 by a variable that is 0 on every update: an aborting error at node 12, Divide.
	VisualScriptDocument MakeUpdateDivisionDocument()
	{
		VisualScriptDocument document = MakeDivisionDocument();
		document.Graph.Nodes[0] = MakeNode(10, "Event.Update");
		return document;
	}

	// Sets the public Done when created, after a Delay of ten seconds (node 11) when delayed.
	VisualScriptDocument MakeDelayDocument(bool delayed)
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Done", "bool", false, true));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Create"));
		document.Graph.Nodes.push_back(MakeNode(12, "Variable.Set", Json::object({{"Variable", 1}}), Json::object({{"Value", true}})));
		if (delayed)
		{
			document.Graph.Nodes.push_back(MakeNode(11, "Flow.Delay", Json::object(), Json::object({{"Duration", 10.0}})));
			document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
			document.Graph.Links.push_back(MakeLink(11, "Completed", 12, "Execute"));
		}
		else
		{
			document.Graph.Links.push_back(MakeLink(10, "Then", 12, "Execute"));
		}
		return document;
	}

	// A library whose pure Inverse(int Value) -> int Result (function 1) divides 1 by Value at node 11.
	VisualScriptDocument MakeInverseLibraryDocument()
	{
		VisualScriptDocument document;
		document.Kind = VisualScriptDocumentKind::Library;
		VisualScriptFunction inverse;
		inverse.ID = 1;
		inverse.Name = "Inverse";
		inverse.Pure = true;
		inverse.Inputs.emplace_back("Value", "int");
		inverse.Outputs.emplace_back("Result", "int");
		inverse.Graph.Nodes.push_back(MakeNode(10, "Function.Entry"));
		inverse.Graph.Nodes.push_back(MakeNode(11, "Operator.Divide", Json::object(), Json::object({{"A", 1}})));
		inverse.Graph.Nodes.push_back(MakeNode(12, "Function.Return"));
		inverse.Graph.Links.push_back(MakeLink(10, "Value", 11, "B"));
		inverse.Graph.Links.push_back(MakeLink(11, "Result", 12, "Result"));
		document.Functions.push_back(std::move(inverse));
		return document;
	}

	// Sets the public Count to the library's Inverse(0) on every update, calling it at node 22: the library's node fails.
	VisualScriptDocument MakeInverseCallerDocument(AssetHandle library)
	{
		VisualScriptDocument document = MakeCallerDocument(library);
		document.Graph.Nodes[0] = MakeNode(20, "Event.Update");
		document.Graph.Nodes[2].Inputs = Json::object({{"Value", 0}});
		return document;
	}

	// The form a read of the events has (what the reader checks): serials from 1 to Last, by kind in order; problems that
	// happened; reasons the engine knows.
	void CheckLiveEventsForm(std::vector<VisualScriptLiveEdit> const& edits, std::vector<VisualScriptRuntimeProblem> const& problems,
	                         uint64_t last)
	{
		for (size_t index = 0; index < edits.size(); index++)
		{
			VisualScriptLiveEdit const& edit = edits[index];
			CHECK(edit.Serial >= 1);
			CHECK(edit.Serial <= last);
			CHECK((index == 0 || edits[index - 1].Serial < edit.Serial));
			CHECK(edit.Outcome != VisualScriptLiveOutcome::NotRunning);
			CHECK((edit.Outcome == VisualScriptLiveOutcome::KeptLastWorking) != edit.Errors.empty());
			CHECK((!edit.Deferred || edit.Outcome == VisualScriptLiveOutcome::Applied));
			CHECK_FALSE(edit.Cause.IsValid());
			for (VisualScriptLiveVariable const& reset : edit.ResetVariables)
			{
				CHECK((reset.Detail == "new" || reset.Detail.starts_with("type changed from ")));
			}
			for (VisualScriptLiveNode const& state : edit.ResetStates)
			{
				CHECK((state.Reason == "Removed" || state.Reason == "Changed" || state.Reason == "Disabled"));
			}
		}
		for (size_t index = 0; index < problems.size(); index++)
		{
			VisualScriptRuntimeProblem const& problem = problems[index];
			CHECK(problem.Serial >= 1);
			CHECK(problem.Serial <= last);
			CHECK((index == 0 || problems[index - 1].Serial < problem.Serial));
			CHECK(problem.Count >= 1);
		}
	}

	// The entries of a references text, parsed.
	Json ReadReferences(std::string const& text)
	{
		Result<Json> json = ParseJson(text);
		REQUIRE_MESSAGE(json.IsOk(), json.GetError());
		REQUIRE(json.GetValue().is_object());
		REQUIRE(json.GetValue().contains("Graphs"));
		return json.GetValue();
	}

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
	Result<VisualScriptAnalysis> parsed = VisualScriptAnalysis::Parse(std::string(AnalysisJson));
	REQUIRE_MESSAGE(parsed.IsOk(), parsed.GetError());
	VisualScriptAnalysis const& analysis = parsed.GetValue();
	CHECK(analysis.NodeSpans.size() == 4);

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
		Result<VisualScriptAnalysis> read = VisualScriptAnalysis::Parse(std::string(refused.Json));
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
	Result<VisualScriptAnalysis> parsed = VisualScriptAnalysis::Parse(std::string(AnalysisJson));
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
	REQUIRE(analysis.DiagnosticSpans.size() == 4);
	auto const slice = [&analysis](size_t index)
	{
		std::pair<uint32_t, uint32_t> const span = analysis.DiagnosticSpans[index];
		return std::string_view(analysis.Text).substr(span.first, span.second);
	};
	CHECK(slice(0) == FormulaDiagnosticText);
	CHECK(slice(1) == RemoteDiagnosticText);
	CHECK(slice(2) == FullDiagnosticText);
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
		Result<VisualScriptAnalysis> read = VisualScriptAnalysis::Parse(std::string(refused.Json));
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
		// The class of a data asset type, named by its type: the value the engine registered it with.
		VisualScriptTypeInfo const* curve = library->FindType("Strada.Curve");
		REQUIRE(curve != nullptr);
		CHECK(curve->Kind == VisualScriptTypeKind::Asset);
		CHECK(curve->AcceptedAssetType == TypedDataAsset<Curve>::GetStaticType());
		CHECK(curve->HasLiteral);
		// Every type the runtime keeps in script fields, but enums (which only it knows), is exposable here as it describes
		// it: public variables and literals of a new asset class work in the editor.
		for (VisualScriptTypeInfo const& type : library->GetTypes())
		{
			if (type.FieldType == ScriptFieldType::None || type.Kind == VisualScriptTypeKind::Enum)
			{
				continue;
			}
			CAPTURE(type.ID);
			std::optional<VisualScriptExposableType> const exposable = FindVisualScriptExposableType(type.ID);
			REQUIRE(exposable.has_value());
			CHECK(exposable->FieldType == type.FieldType);
			if (type.FieldType == ScriptFieldType::Asset)
			{
				CHECK(exposable->AcceptedAssetType == type.AcceptedAssetType);
			}
		}
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
		Result<Ref<VisualScriptAnalysis const>> analysis = VisualScriptEngine::Analyze(document);
		REQUIRE_MESSAGE(analysis.IsOk(), analysis.GetError());
		return *analysis.GetValue();
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

			Result<Ref<VisualScriptAnalysis const>> analyzed = VisualScriptEngine::Analyze(document);
			REQUIRE_MESSAGE(analyzed.IsOk(), analyzed.GetError());
			VisualScriptAnalysis const& analysis = *analyzed.GetValue();
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
	// The steps the VM ran grow with what an invoke runs (the profiler reads them around each invoke).
	uint64_t stepsBefore = 0;
	CHECK(status(host.VisualScriptReadSteps(&stepsBefore)) == ScriptStatus::Success);
	CHECK(status(host.VisualScriptInvoke(Instance, static_cast<int32_t>(ScriptEngineInternal::ScriptEvent::Create), 0.0f, 0)) ==
	      ScriptStatus::Success);
	uint64_t stepsAfter = 0;
	CHECK(status(host.VisualScriptReadSteps(&stepsAfter)) == ScriptStatus::Success);
	CHECK(stepsAfter > stepsBefore);
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

TEST_CASE("VisualScriptEngine: a program's references hold the graphs it reaches, libraries with theirs, the others as interfaces")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	AssetHandle const self = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(), "Caller");
	// Health calls a library of its own, which the caller does not reach: interfaces are leaves.
	AssetHandle const leaf = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeLibraryDocument()), "Leaf");
	VisualScriptDocument healthDocument = MakeHealthDocument();
	healthDocument.Graph.Nodes.push_back(MakeNode(12, "Function.Call", Json::object({{"Function", 1}, {"Graph", leaf.ToString()}})));
	AssetHandle const health = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(healthDocument), "Health");
	AssetHandle const target = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeHealthDocument()), "Target");
	AssetHandle const material = AssetManager::AddMemoryAsset(CreateRef<MaterialAsset>(), "Paint");
	AssetHandle const unregistered(UUID(42));
	// The inner library sends Hit to Health (already read as an interface by the caller) and to the caller itself.
	VisualScriptDocument inner = MakeLibraryDocument(5);
	inner.Functions[0].Graph.Nodes.push_back(
		MakeNode(13, "Event.Send", Json::object({{"Name", "Hit"}, {"Parameters", Json::array()}, {"Graph", health.ToString()}})));
	inner.Functions[0].Graph.Nodes.push_back(
		MakeNode(14, "Event.Send", Json::object({{"Name", "Hit"}, {"Parameters", Json::array()}, {"Graph", self.ToString()}})));
	AssetHandle const innerLibrary = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(inner), "Inner");
	// The outer library calls the inner one, twice, and is called by the caller.
	VisualScriptDocument outer = MakeLibraryDocument(3);
	for (uint32_t const node : {13u, 14u})
	{
		outer.Functions[0].Graph.Nodes.push_back(MakeNode(
			node, "Function.Call", Json::object({{"Function", 1}, {"Graph", innerLibrary.ToString()}}), Json::object({{"Value", 1}})));
	}
	AssetHandle const outerLibrary = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(outer), "Outer");

	// The caller: the outer library, Health's variables, a Send checked against Target, RunsGraph of a handle no asset has
	// and a Send to a material.
	VisualScriptDocument caller = MakeCallerDocument(outerLibrary, health, target);
	caller.Graph.Nodes.push_back(MakeNode(26, "RunsGraph", Json::object({{"Graph", unregistered.ToString()}})));
	caller.Graph.Nodes.push_back(
		MakeNode(27, "Event.Send", Json::object({{"Name", "Hit"}, {"Parameters", Json::array()}, {"Graph", material.ToString()}})));
	Ref<VisualScriptAsset> const callerAsset = AssetManager::GetAsset<VisualScriptAsset>(self);
	REQUIRE(callerAsset != nullptr);
	callerAsset->SetDocument(caller);

	std::vector<ExpectedReference> const expected = {
		{outerLibrary, "Outer", "Full", VisualScriptSerializer::WriteContentText(outer)},
		{innerLibrary, "Inner", "Full", VisualScriptSerializer::WriteContentText(inner)},
		{health, "Health", "Interface", VisualScriptSerializer::WriteContentText(MakeVisualScriptInterface(healthDocument))},
		{target, "Target", "Interface", VisualScriptSerializer::WriteContentText(MakeVisualScriptInterface(MakeHealthDocument()))},
		{material, "Paint", "", fmt::format("asset {} is a Material, not a VisualScript", material)},
		{unregistered, "", "", "no visual script 42 in the project (deleted, or not a visual script)"}};
	std::string const expectedText = WriteExpectedReferences(self, expected);
	CHECK(ReadReferences(expectedText)["Graphs"].size() == 6);

	// A send carries them, written exactly so.
	{
		SentDocuments sent;
		REQUIRE(VisualScriptEngine::EnsureLoaded(self).IsOk());
		REQUIRE(sent.GetCount() == 1);
		CHECK(sent.GetReferences(0) == expectedText);
	}

	// So does an analysis, which lists the graphs it read with the documents it used.
	Result<Ref<VisualScriptAnalysis const>> const analysis = VisualScriptEngine::Analyze(caller, self);
	REQUIRE_MESSAGE(analysis.IsOk(), (analysis ? std::string() : analysis.GetError()));
	std::vector<std::pair<AssetHandle, Ref<VisualScriptDocument const>>> const& graphs = analysis.GetValue()->Graphs;
	REQUIRE(graphs.size() == 6);
	CHECK(std::is_sorted(graphs.begin(), graphs.end(),
	                     [](auto const& a, auto const& b)
	                     {
							 return a.first.GetUUID().GetValue() < b.first.GetUUID().GetValue();
						 }));
	for (auto const& [graph, document] : graphs)
	{
		CAPTURE(graph);
		CHECK(graph != self);
		if (graph == material || graph == unregistered)
		{
			CHECK(document == nullptr);
			continue;
		}
		Ref<VisualScriptAsset> const asset = AssetManager::GetAsset<VisualScriptAsset>(graph);
		REQUIRE(asset != nullptr);
		CHECK(document == asset->GetSharedDocument());
	}

	// Without its own handle a document cannot tell itself from the stored graph: the inner library's Send names the caller
	// as another graph, which the references then hold.
	Result<Ref<VisualScriptAnalysis const>> const anonymous = VisualScriptEngine::Analyze(caller);
	REQUIRE(anonymous.IsOk());
	std::vector<std::pair<AssetHandle, Ref<VisualScriptDocument const>>> const& anonymousGraphs = anonymous.GetValue()->Graphs;
	CHECK(anonymousGraphs.size() == 7);
	CHECK(std::any_of(anonymousGraphs.begin(), anonymousGraphs.end(),
	                  [&](auto const& entry)
	                  {
						  return entry.first == self && entry.second == callerAsset->GetSharedDocument();
					  }));

	// A document that names no graph sends none.
	AssetHandle const plain = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeCountDocument(1)), "Plain");
	SentDocuments sent;
	REQUIRE(VisualScriptEngine::EnsureLoaded(plain).IsOk());
	REQUIRE(sent.GetCount() == 1);
	CHECK(sent.GetReferences(0).empty());
	Result<Ref<VisualScriptAnalysis const>> const plainAnalysis = VisualScriptEngine::Analyze(MakeCountDocument(1), plain);
	REQUIRE(plainAnalysis.IsOk());
	CHECK(plainAnalysis.GetValue()->Graphs.empty());
}

TEST_CASE("VisualScriptEngine: a node naming the analyzed graph reads the document analyzed, which the references never hold")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	VisualScriptDocument stored;
	stored.Variables.push_back(MakeVariable(2, "Other", "Strada.Entity", Json(), false));
	Ref<VisualScriptAsset> const asset = CreateRef<VisualScriptAsset>(stored);
	AssetHandle const graph = AssetManager::AddMemoryAsset(asset, "Enemy");

	// An edit adds the public Fresh and reads it on another entity running this graph: the variable exists only in the
	// document being edited.
	VisualScriptDocument edited = stored;
	edited.Variables.push_back(MakeVariable(3, "Fresh", "int", 4, true));
	edited.Graph.Nodes.push_back(MakeNode(10, "Variable.Get", Json::object({{"Variable", 2}})));
	edited.Graph.Nodes.push_back(MakeNode(11, "Variable.Get", Json::object({{"Variable", 3}, {"Graph", graph.ToString()}})));
	edited.Graph.Links.push_back(MakeLink(10, "Value", 11, "Target"));

	Result<Ref<VisualScriptAnalysis const>> const analysis = VisualScriptEngine::Analyze(edited, graph);
	REQUIRE_MESSAGE(analysis.IsOk(), (analysis ? std::string() : analysis.GetError()));
	CHECK_MESSAGE(CountVisualScriptErrors(analysis.GetValue()->Diagnostics) == 0,
	              DescribeVisualScriptErrors(analysis.GetValue()->Diagnostics));
	CHECK(analysis.GetValue()->Graphs.empty());
	VisualScriptAnalyzedNode const* const read = analysis.GetValue()->FindNode(11);
	REQUIRE(read != nullptr);
	VisualScriptPin const* const value = read->FindPin("Value", VisualScriptPinDirection::Out);
	REQUIRE(value != nullptr);
	CHECK(value->Type == "int");

	// Read as another graph's, the stored document has no such variable.
	Result<Ref<VisualScriptAnalysis const>> const stale = VisualScriptEngine::Analyze(edited);
	REQUIRE(stale.IsOk());
	CHECK(CountVisualScriptErrors(stale.GetValue()->Diagnostics) > 0);
	REQUIRE(stale.GetValue()->Graphs.size() == 1);
	CHECK(stale.GetValue()->Graphs[0].first == graph);
	CHECK(stale.GetValue()->Graphs[0].second == asset->GetSharedDocument());

	// The graph loaded with that document compiles too, its references naming it and holding nothing else.
	asset->SetDocument(edited);
	SentDocuments sent;
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(VisualScriptEngine::HasWorkingProgram(graph));
	REQUIRE(sent.GetCount() == 1);
	CHECK(sent.GetReferences(0) == fmt::format(R"({{"Self":"{}","Graphs":[]}})", graph.ToString()));
}

TEST_CASE("VisualScriptEngine: without assets every graph a document refers to is absent")
{
	Testing::ScriptRuntimeScope runtime;
	VisualScriptDocument document;
	document.Graph.Nodes.push_back(
		MakeNode(10, "Event.Send", Json::object({{"Name", "Hit"}, {"Parameters", Json::array()}, {"Graph", "42"}})));
	document.Graph.Nodes.push_back(MakeNode(11, "Function.Call", Json::object({{"Function", 1}, {"Graph", "43"}})));
	Result<Ref<VisualScriptAnalysis const>> const analysis = VisualScriptEngine::Analyze(document, AssetHandle(UUID(41)));
	REQUIRE_MESSAGE(analysis.IsOk(), (analysis ? std::string() : analysis.GetError()));
	std::vector<std::pair<AssetHandle, Ref<VisualScriptDocument const>>> const& graphs = analysis.GetValue()->Graphs;
	REQUIRE(graphs.size() == 2);
	CHECK(graphs[0] == std::pair<AssetHandle, Ref<VisualScriptDocument const>>{AssetHandle(UUID(42)), nullptr});
	CHECK(graphs[1] == std::pair<AssetHandle, Ref<VisualScriptDocument const>>{AssetHandle(UUID(43)), nullptr});
	// The call's library is absent: an Error that names why.
	bool const named = std::any_of(analysis.GetValue()->Diagnostics.begin(), analysis.GetValue()->Diagnostics.end(),
	                               [](VisualScriptDiagnostic const& diagnostic)
	                               {
									   return diagnostic.Node == 11 && diagnostic.Message.find("there are no assets") != std::string::npos;
								   });
	CHECK_MESSAGE(named, DescribeVisualScriptErrors(analysis.GetValue()->Diagnostics));
}

TEST_CASE("VisualScriptEngine: references hold at most 256 other graphs, and those beyond are sent as absent")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	constexpr size_t Count = VisualScriptEngine::MaxReferencedGraphs + 44;
	VisualScriptDocument document;
	for (size_t index = 0; index < Count; index++)
	{
		uint32_t const id = static_cast<uint32_t>(10 + index);
		std::string const handle = std::to_string(5000 + index);
		document.Graph.Nodes.push_back(
			MakeNode(id, "Event.Send", Json::object({{"Name", "Hit"}, {"Parameters", Json::array()}, {"Graph", handle}})));
	}
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(document), "Crowd");

	// The analysis lists the graphs it looked up, not those beyond.
	Result<Ref<VisualScriptAnalysis const>> const analysis = VisualScriptEngine::Analyze(document, graph);
	REQUIRE(analysis.IsOk());
	std::vector<std::pair<AssetHandle, Ref<VisualScriptDocument const>>> const& graphs = analysis.GetValue()->Graphs;
	REQUIRE(graphs.size() == VisualScriptEngine::MaxReferencedGraphs);
	CHECK(graphs.front().first == AssetHandle(UUID(5000ull)));
	CHECK(graphs.back().first == AssetHandle(UUID(5000ull + VisualScriptEngine::MaxReferencedGraphs - 1)));

	SentDocuments sent;
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	REQUIRE(sent.GetCount() == 1);
	Json const references = ReadReferences(sent.GetReferences(0));
	Json const& entries = references["Graphs"];
	REQUIRE(entries.size() == Count);
	for (size_t index = 0; index < Count; index++)
	{
		CAPTURE(index);
		Json const& entry = entries[index];
		CHECK(entry["Graph"] == std::to_string(5000 + index));
		CHECK_FALSE(entry.contains("Document"));
		CHECK(entry["Error"] == (index < VisualScriptEngine::MaxReferencedGraphs
		                             ? fmt::format("no visual script {} in the project (deleted, or not a visual script)", 5000 + index)
		                             : std::string("more than 256 visual scripts are referenced")));
	}
	// Those beyond are never looked up again, so nothing is sent again for them.
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(sent.GetCount() == 1);
}

TEST_CASE("VisualScriptEngine: references carry libraries and interfaces without development-only nodes when those do not run")
{
	Testing::AssetManagerScope assets;
	// The library's impure Note (function 3) logs a line in a development-only node, which the caller runs when created.
	VisualScriptDocument library = MakeLibraryDocument();
	VisualScriptFunction note;
	note.ID = 3;
	note.Name = "Note";
	note.Graph.Nodes.push_back(MakeNode(30, "Function.Entry"));
	note.Graph.Nodes.push_back(
		MakeNode(31, "Call:Strada.Log.Info(string)", Json::object(), Json::object({{"Message", "the library's development note"}})));
	note.Graph.Nodes.back().Enabled = VisualScriptNodeEnabled::DevelopmentOnly;
	note.Graph.Links.push_back(MakeLink(30, "Then", 31, "Execute"));
	library.Functions.push_back(std::move(note));
	AssetHandle const libraryGraph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(library), "Library");
	VisualScriptDocument health = MakeHealthDocument();
	health.Graph.Nodes[0].Enabled = VisualScriptNodeEnabled::DevelopmentOnly;
	AssetHandle const healthGraph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(health), "Health");
	VisualScriptDocument callerDocument = MakeCallerDocument(libraryGraph, healthGraph);
	callerDocument.Graph.Nodes.push_back(
		MakeNode(26, "Function.Call", Json::object({{"Function", 3}, {"Graph", libraryGraph.ToString()}})));
	callerDocument.Graph.Links.push_back(MakeLink(21, "Then", 26, "Execute"));
	AssetHandle const caller = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(callerDocument), "Caller");
	auto const findSent = [](Json const& references, AssetHandle graph)
	{
		Json const& entries = references["Graphs"];
		auto const entry = std::find_if(entries.begin(), entries.end(),
		                                [graph](Json const& candidate)
		                                {
											return candidate["Graph"] == graph.ToString();
										});
		REQUIRE(entry != entries.end());
		Result<VisualScriptDocument> document = VisualScriptSerializer::DeserializeDocument((*entry)["Document"], MakeStrictContext());
		REQUIRE_MESSAGE(document.IsOk(), document.GetError());
		return document.TakeValue();
	};

	{
		Testing::ScriptRuntimeScope runtime;
		VisualScriptEngine::SetDevelopmentNodesEnabled(false);
		SentDocuments sent;
		REQUIRE(VisualScriptEngine::EnsureLoaded(caller).IsOk());
		REQUIRE(sent.GetCount() == 1);
		Json const references = ReadReferences(sent.GetReferences(0));
		CHECK(findSent(references, libraryGraph) == DisableDevelopmentOnlyNodes(library));
		CHECK(findSent(references, healthGraph) == DisableDevelopmentOnlyNodes(MakeVisualScriptInterface(health)));
		// The library's function runs without the line.
		REQUIRE(VisualScriptEngine::CreateInstance(UUID(1001), caller, {}).IsOk());
		uint64_t const logStart = Log::GetNextEntryIndex();
		VisualScriptEngine::InvokeOnCreate(UUID(1001));
		CHECK(ReadVariable(UUID(1001), "Count") == ScriptFieldValue::FromInt32(6));
		CHECK_FALSE(Testing::WasLogged(logStart, "the library's development note"));
	}

	// While they run, everything is sent as it is.
	Testing::ScriptRuntimeScope runtime;
	SentDocuments sent;
	REQUIRE(VisualScriptEngine::EnsureLoaded(caller).IsOk());
	REQUIRE(sent.GetCount() == 1);
	Json const references = ReadReferences(sent.GetReferences(0));
	CHECK(findSent(references, libraryGraph) == library);
	CHECK(findSent(references, healthGraph) == MakeVisualScriptInterface(health));
	REQUIRE(VisualScriptEngine::CreateInstance(UUID(1001), caller, {}).IsOk());
	uint64_t const logStart = Log::GetNextEntryIndex();
	VisualScriptEngine::InvokeOnCreate(UUID(1001));
	CHECK(ReadVariable(UUID(1001), "Count") == ScriptFieldValue::FromInt32(6));
	CHECK(Testing::WasLogged(logStart, "the library's development note"));
}

TEST_CASE("VisualScriptEngine: a program is sent again when a graph it compiles with changes what it reads, appears, goes or moves")
{
	Testing::TemporaryDirectory directory;
	REQUIRE(VisualScriptSerializer::SaveToFile(MakeLibraryDocument(), directory.GetPath() / "Library.sgraph").IsOk());
	REQUIRE(VisualScriptSerializer::SaveToFile(MakeHealthDocument(), directory.GetPath() / "Health.sgraph").IsOk());
	REQUIRE(VisualScriptSerializer::SaveToFile(MakeHealthDocument(), directory.GetPath() / "Target.sgraph").IsOk());
	Testing::AssetManagerScope assets;
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());
	AssetHandle const library = AssetManager::FindByPath("Library.sgraph");
	AssetHandle const health = AssetManager::FindByPath("Health.sgraph");
	AssetHandle const target = AssetManager::FindByPath("Target.sgraph");
	REQUIRE(library.IsValid());
	REQUIRE(health.IsValid());
	REQUIRE(target.IsValid());
	AssetHandle const caller =
		AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeCallerDocument(library, health, target)), "Caller");
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;

	REQUIRE(VisualScriptEngine::EnsureLoaded(caller).IsOk());
	CHECK(VisualScriptEngine::HasWorkingProgram(caller));
	auto const resends = [caller]()
	{
		uint64_t const generation = VisualScriptEngine::GetProgramGeneration(caller);
		REQUIRE(VisualScriptEngine::EnsureLoaded(caller).IsOk());
		return VisualScriptEngine::GetProgramGeneration(caller) != generation;
	};
	auto const setDocument = [](AssetHandle graph, VisualScriptDocument document)
	{
		Ref<VisualScriptAsset> const asset = AssetManager::GetAsset<VisualScriptAsset>(graph);
		REQUIRE(asset != nullptr);
		asset->SetDocument(std::move(document));
	};
	CHECK_FALSE(resends());

	// The library: its layout and description are not its program; another multiplication is.
	VisualScriptDocument library2 = MakeLibraryDocument();
	library2.Functions[0].Graph.Nodes[1].Position = glm::vec2(320.0f, 0.0f);
	library2.Description = "Doubles";
	setDocument(library, library2);
	CHECK_FALSE(resends());
	setDocument(library, MakeLibraryDocument(3));
	CHECK(resends());
	CHECK_FALSE(resends());
	REQUIRE(VisualScriptEngine::CreateInstance(UUID(1001), caller, {}).IsOk());
	VisualScriptEngine::InvokeOnCreate(UUID(1001));
	CHECK(ReadVariable(UUID(1001), "Count") == ScriptFieldValue::FromInt32(9));

	// The graph whose variables it reads: its events, layout and tooltips are not what it reads; its variables are.
	VisualScriptDocument health2 = MakeHealthDocument();
	health2.Graph.Nodes.push_back(MakeNode(12, "Event.Custom", Json::object({{"Name", "Heal"}})));
	health2.Graph.Nodes[0].Position = glm::vec2(0.0f, 160.0f);
	health2.Variables[0].Tooltip = "How much is left";
	setDocument(health, health2);
	CHECK_FALSE(resends());
	health2.Variables.push_back(MakeVariable(2, "Armor", "float", 1.0, true));
	setDocument(health, health2);
	CHECK(resends());
	health2.Variables[0].Public = false;
	setDocument(health, health2);
	CHECK(resends());
	CHECK_FALSE(resends());

	// The graph a Send is checked against: nothing of it is compiled.
	VisualScriptDocument target2 = MakeHealthDocument();
	target2.Graph.Nodes[0].Settings["Name"] = "Struck";
	target2.Variables.clear();
	setDocument(target, target2);
	CHECK_FALSE(resends());

	// Names are compiled into programs: a graph moved is sent under its new path.
	SentDocuments sent;
	REQUIRE(AssetManager::MoveAsset(target, "TargetMoved.sgraph").IsOk());
	CHECK(resends());
	REQUIRE(sent.GetCount() == 1);
	CHECK(sent.GetReferences(0).find(R"("Name":"TargetMoved.sgraph")") != std::string::npos);
	REQUIRE(AssetManager::MoveAsset(library, "LibraryMoved.sgraph").IsOk());
	CHECK(resends());
	CHECK_FALSE(resends());

	// Graphs that go and come back, whatever their use.
	for (AssetHandle const graph : {target, health, library})
	{
		CAPTURE(graph);
		std::filesystem::path const file = AssetManager::GetAbsolutePath(graph);
		Result<std::string> const text = FileSystem::ReadTextFile(file);
		REQUIRE(text.IsOk());
		REQUIRE(FileSystem::WriteTextFile(file, "{ not JSON").IsOk());
		CHECK(AssetManager::ReloadAsset(graph).IsError());
		CHECK(resends());
		CHECK_FALSE(resends());
		REQUIRE(FileSystem::WriteTextFile(file, text.GetValue()).IsOk());
		REQUIRE(AssetManager::ReloadAsset(graph).IsOk());
		CHECK(resends());
		CHECK_FALSE(resends());
	}
	CHECK(host.Exceptions == 0);
}

TEST_CASE("VisualScriptEngine: a function library runs on no entity, and refusing one is no script exception")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	Ref<VisualScriptAsset> const asset = CreateRef<VisualScriptAsset>(MakeLibraryDocument());
	AssetHandle const graph = AssetManager::AddMemoryAsset(asset, "Motion");

	Result<void> const loaded = VisualScriptEngine::EnsureLoaded(graph);
	REQUIRE(loaded.IsError());
	CHECK(loaded.GetError() == "Motion is a function library: graphs call its functions; it runs on no entity");
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) == 0);
	Result<void> const created = VisualScriptEngine::CreateInstance(UUID(1001), graph, {});
	REQUIRE(created.IsError());
	CHECK(created.GetError() == loaded.GetError());
	CHECK_FALSE(VisualScriptEngine::HasInstance(UUID(1001)));
	CHECK_FALSE(VisualScriptEngine::HasWorkingProgram(graph));
	CHECK(host.Exceptions == 0);

	// A script made a library is absent, and runs again once a script.
	asset->SetDocument(MakeCountDocument(2));
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	uint64_t const script = VisualScriptEngine::GetProgramGeneration(graph);
	asset->SetDocument(MakeLibraryDocument());
	CHECK(VisualScriptEngine::EnsureLoaded(graph).IsError());
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) == script);
	asset->SetDocument(MakeCountDocument(2));
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	CHECK(VisualScriptEngine::GetProgramGeneration(graph) != script);
	REQUIRE(VisualScriptEngine::CreateInstance(UUID(1002), graph, {}).IsOk());
	VisualScriptEngine::InvokeOnCreate(UUID(1002));
	CHECK(ReadVariable(UUID(1002), "Count") == ScriptFieldValue::FromInt32(2));
	CHECK(host.Exceptions == 0);
}

TEST_CASE("VisualScriptEngine: a broken library leaves its callers running the version they were compiled with, after a game-assembly "
          "change too")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	Ref<VisualScriptAsset> const libraryAsset = CreateRef<VisualScriptAsset>(MakeLibraryDocument());
	AssetHandle const library = AssetManager::AddMemoryAsset(libraryAsset, "Motion");
	Ref<VisualScriptAsset> const callerAsset = CreateRef<VisualScriptAsset>(MakeCallerDocument(library));
	AssetHandle const caller = AssetManager::AddMemoryAsset(callerAsset, "Caller");

	CHECK_FALSE(VisualScriptEngine::HasWorkingProgram(library));
	REQUIRE(VisualScriptEngine::EnsureLoaded(caller).IsOk());
	CHECK(VisualScriptEngine::HasWorkingProgram(caller));
	// The caller runs the library: so does the library's program, as far as anyone can tell.
	CHECK(VisualScriptEngine::HasWorkingProgram(library));

	// Broken: the caller is sent again with it and does not compile; it keeps the version that worked.
	VisualScriptDocument broken = MakeLibraryDocument();
	broken.Functions[0].Graph.Nodes.push_back(MakeNode(13, "Sample.NoSuchNode"));
	libraryAsset->SetDocument(broken);
	uint64_t logStart = Log::GetNextEntryIndex();
	uint64_t const before = VisualScriptEngine::GetProgramGeneration(caller);
	REQUIRE(VisualScriptEngine::EnsureLoaded(caller).IsOk());
	CHECK(VisualScriptEngine::GetProgramGeneration(caller) != before);
	CHECK(CountLogged(logStart, "Visual script 'Caller' has ") == 1);
	CHECK(Testing::WasLogged(logStart, "; the previous version keeps running"));
	CHECK(VisualScriptEngine::HasWorkingProgram(caller));
	CHECK(VisualScriptEngine::HasWorkingProgram(library));

	// After a game-assembly change the working program goes first, with the library it was compiled with, and compiles.
	SentDocuments sent;
	REQUIRE(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
	logStart = Log::GetNextEntryIndex();
	REQUIRE(VisualScriptEngine::EnsureLoaded(caller).IsOk());
	REQUIRE(sent.GetCount() == 2);
	CHECK(sent.Get(0) == MakeCallerDocument(library));
	CHECK(sent.GetReferences(0) ==
	      WriteExpectedReferences(caller, {{library, "Motion", "Full", VisualScriptSerializer::WriteContentText(MakeLibraryDocument())}}));
	CHECK(sent.GetReferences(1) ==
	      WriteExpectedReferences(caller, {{library, "Motion", "Full", VisualScriptSerializer::WriteContentText(broken)}}));
	CHECK(VisualScriptEngine::HasWorkingProgram(caller));
	CHECK(Testing::WasLogged(logStart, "; the previous version keeps running"));
	CHECK(VisualScriptEngine::HasWorkingProgram(library));
	REQUIRE(VisualScriptEngine::CreateInstance(UUID(1001), caller, {}).IsOk());
	VisualScriptEngine::InvokeOnCreate(UUID(1001));
	CHECK(ReadVariable(UUID(1001), "Count") == ScriptFieldValue::FromInt32(6));
	// Unchanged since, nothing more is sent.
	REQUIRE(VisualScriptEngine::EnsureLoaded(caller).IsOk());
	CHECK(sent.GetCount() == 2);

	// Fixed: the caller compiles with it.
	libraryAsset->SetDocument(MakeLibraryDocument(4));
	REQUIRE(VisualScriptEngine::EnsureLoaded(caller).IsOk());
	REQUIRE(VisualScriptEngine::CreateInstance(UUID(1002), caller, {}).IsOk());
	VisualScriptEngine::InvokeOnCreate(UUID(1002));
	CHECK(ReadVariable(UUID(1002), "Count") == ScriptFieldValue::FromInt32(12));
	CHECK(VisualScriptEngine::HasWorkingProgram(library));

	// Once no working program holds the library, it has none.
	callerAsset->SetDocument(MakeCountDocument(1));
	REQUIRE(VisualScriptEngine::EnsureLoaded(caller).IsOk());
	CHECK(VisualScriptEngine::HasWorkingProgram(caller));
	CHECK_FALSE(VisualScriptEngine::HasWorkingProgram(library));
	CHECK(host.Exceptions == 0);
}

TEST_CASE("VisualScriptEngine: a library edited while playing sends its 20 dependents of 200 nodes again once each, in the next update, "
          "in its time on request")
{
	// spec-slice3 §15.8 and §20 Q3: the update after a library edit compiles every graph that calls it.
	constexpr size_t Dependents = 20;
	constexpr uint32_t Additions = 197;
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	Ref<VisualScriptAsset> const libraryAsset = CreateRef<VisualScriptAsset>(MakeLibraryDocument());
	AssetHandle const library = AssetManager::AddMemoryAsset(libraryAsset, "Motion");

	// Each dependent sets its public Result to Twice(3) plus one for each of its 197 additions, on every update: 200 nodes.
	Scene scene("Dependents");
	std::vector<AssetHandle> graphs;
	std::vector<UUID> entities;
	for (size_t index = 0; index < Dependents; index++)
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Result", "int", static_cast<int32_t>(index), true));
		document.Graph.Nodes.push_back(MakeNode(10, "Event.Update"));
		document.Graph.Nodes.push_back(MakeNode(11, "Variable.Set", Json::object({{"Variable", 1}})));
		document.Graph.Nodes.push_back(
			MakeNode(12, "Function.Call", Json::object({{"Function", 1}, {"Graph", library.ToString()}}), Json::object({{"Value", 3}})));
		document.Graph.Links.push_back(MakeLink(10, "Then", 11, "Execute"));
		uint32_t previous = 12;
		for (uint32_t addition = 0; addition < Additions; addition++)
		{
			uint32_t const id = 100 + addition;
			document.Graph.Nodes.push_back(MakeNode(id, "Operator.Add", Json::object(), Json::object({{"B", 1}})));
			document.Graph.Links.push_back(MakeLink(previous, "Result", id, "A"));
			previous = id;
		}
		document.Graph.Links.push_back(MakeLink(previous, "Result", 11, "Value"));
		REQUIRE(document.Graph.Nodes.size() == 200);
		AssetHandle const graph =
			AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(std::move(document)), fmt::format("Dependent {}", index));
		Entity entity = scene.CreateEntity(fmt::format("Dependent {}", index));
		entity.AddComponent<VisualScriptComponent>().Graph = graph;
		graphs.push_back(graph);
		entities.push_back(entity.GetUUID());
	}
	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	std::vector<uint64_t> generations;
	for (size_t index = 0; index < Dependents; index++)
	{
		CHECK(ReadVariable(entities[index], "Result") == ScriptFieldValue::FromInt32(6 + static_cast<int32_t>(Additions)));
		generations.push_back(VisualScriptEngine::GetProgramGeneration(graphs[index]));
	}

	// Edited: the next update sends every dependent once with the library as it is now, and each runs it.
	SentDocuments sent;
	VisualScriptDocument const edited = MakeLibraryDocument(5);
	libraryAsset->SetDocument(edited);
	static_cast<void>(VisualScriptEngine::TakeLoadTimings());
	std::chrono::steady_clock::time_point const start = std::chrono::steady_clock::now();
	scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	double const milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	// The update's loads, summed (spec-slice4 section 9.0): the runtime takes measurable time (the references and the
	// diagnostics "[]" can take less than the clock's resolution), and together they are within the update. No dependent's
	// text is written again: each one's document is the one last sent (spec-slice4 section 9.4), so Text is not even timed.
	VisualScriptLoadTimings const loads = VisualScriptEngine::TakeLoadTimings();
	CHECK(loads.Text == 0.0);
	CHECK(loads.References >= 0.0);
	CHECK(loads.Runtime > 0.0);
	CHECK(loads.Diagnostics >= 0.0);
	CHECK(loads.Text + loads.References + loads.Runtime + loads.Diagnostics <= milliseconds);
	REQUIRE(sent.GetCount() == Dependents);
	std::set<std::string> received;
	std::set<std::string> expected;
	std::string const libraryText = VisualScriptSerializer::WriteContentText(edited);
	for (size_t index = 0; index < Dependents; index++)
	{
		received.insert(sent.GetReferences(index));
		expected.insert(WriteExpectedReferences(graphs[index], {{library, "Motion", "Full", libraryText}}));
	}
	CHECK(received == expected);
	for (size_t index = 0; index < Dependents; index++)
	{
		CAPTURE(index);
		// Each send's receipt names the library as what made it, and the one instance it swapped.
		Ref<VisualScriptLiveEdit const> const receipt = FindLastReceipt(graphs[index]);
		CHECK(receipt->Cause == library);
		CHECK(receipt->Outcome == VisualScriptLiveOutcome::Applied);
		CHECK(receipt->Instances == 1);
		CHECK(VisualScriptEngine::GetProgramGeneration(graphs[index]) != generations[index]);
		CHECK(VisualScriptEngine::HasInstance(entities[index]));
		CHECK(ReadVariable(entities[index], "Result") == ScriptFieldValue::FromInt32(15 + static_cast<int32_t>(Additions)));
	}
	// Nothing more is sent after.
	scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	CHECK(sent.GetCount() == Dependents);
	CHECK(host.Exceptions == 0);
	scene.OnRuntimeStop();

	MESSAGE("the update that sends " << Dependents << " dependents of 200 nodes again after a library edit: " << milliseconds
	                                 << " ms; its loads (ms): Text " << loads.Text << ", References " << loads.References << ", Runtime "
	                                 << loads.Runtime << ", Diagnostics " << loads.Diagnostics);
	std::optional<std::string> const performance = Platform::ReadEnvironmentVariable("STRADA_PERFORMANCE_TESTS");
	if (performance.has_value() && *performance == "1")
	{
		// Three times the figure track L measured once the dependents' analyses were reused and their texts kept (spec-slice4
		// section 9.4; an AMD Ryzen 5 7535HS laptop, Windows 11, MSVC 19.51; median of five runs). spec-slice4 section 21's
		// target, 45 ms optimized, is printed as met or missed.
#if defined(ST_DEBUG)
		constexpr double MeasuredMilliseconds = 45.0;
#else
		constexpr double MeasuredMilliseconds = 31.6;
		MESSAGE("spec-slice4 section 21's target of 45 ms is " << (milliseconds <= 45.0 ? "met" : "missed"));
#endif
		CHECK(milliseconds <= 3.0 * MeasuredMilliseconds);
	}
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

	// Editor.json has disabled, development-only and split nodes; Language.json formulas, arrays, strings, generics and a
	// timeline.
	uint64_t const logStart = Log::GetNextEntryIndex();
	for (std::string_view const name : Testing::SharedVisualScriptFixtures)
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

	// A graph that calls a member of the game's scripts and holds an array of a game enum, which the reload must drop too.
	Entity nudged = scene.CreateEntity("Nudged");
	AssetHandle const nudge = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeNudgeDocument()), "Nudge");
	REQUIRE(ScriptEngine::CreateInstance(nudged.GetUUID(), "Strada.Tests.Mover", {}).IsOk());
	REQUIRE(VisualScriptEngine::CreateInstance(nudged.GetUUID(), nudge, {}).IsOk());
	VisualScriptEngine::InvokeOnCreate(nudged.GetUUID());
	CHECK(ReadVariable(nudged.GetUUID(), "Count") == ScriptFieldValue::FromInt32(2));
	CHECK(nudged.GetComponent<TransformComponent>().Translation == glm::vec3(2.0f, 0.0f, 0.0f));
	VisualScriptEngine::DestroyInstance(nudged.GetUUID());
	ScriptEngine::DestroyInstance(nudged.GetUUID());

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

namespace
{
	using Testing::RunSetGraph;

	// The projects of Projects/ that have assets.
	std::vector<std::filesystem::path> FindProjects()
	{
		std::filesystem::path const projectsDirectory = FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR);
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
		return projects;
	}

	// The project's run set (Testing::FindVisualScriptRunSet), with a copy of its registry and graphs in the directory as the
	// asset directory, where graphs that refer to other graphs find them. Needs an initialized AssetManager.
	std::vector<RunSetGraph> OpenRunSet(std::filesystem::path const& project, std::filesystem::path const& directory)
	{
		Testing::OpenVisualScriptsCopy(project / "Assets", directory / "Assets");
		return Testing::FindVisualScriptRunSet(project / "Assets");
	}

	// What the nodes of a run set hold, for the feature-test check: the node types (those of enabled nodes apart), whether
	// an inferred generic method appears without and with TypeArguments, and the types of the timelines' tracks.
	struct RunSetUses
	{
		std::set<std::string> Types;
		std::set<std::string> EnabledTypes;
		bool InferredWithoutTypeArguments = false;
		bool InferredWithTypeArguments = false;
		std::set<std::string> TrackTypes;

		void Add(VisualScriptLibrary const& library, VisualScriptNode const& node)
		{
			Types.insert(node.Type);
			if (node.Enabled == VisualScriptNodeEnabled::Enabled)
			{
				EnabledTypes.insert(node.Type);
			}
			// An inferred generic method: its TypeArguments setting is optional and takes any type.
			if (VisualScriptNodeTemplate const* const kind = library.FindNode(node.Type))
			{
				auto const isInferred = [](VisualScriptSetting const& setting)
				{
					return setting.Name == "TypeArguments" && setting.Kind == "TypeList" && !setting.Required &&
					       setting.Constraint == "Any";
				};
				if (std::any_of(kind->Settings.begin(), kind->Settings.end(), isInferred))
				{
					(node.Settings.contains("TypeArguments") ? InferredWithTypeArguments : InferredWithoutTypeArguments) = true;
				}
			}
			auto const timeline = node.Settings.find("Timeline");
			if (node.Type != "Flow.Timeline" || timeline == node.Settings.end() || !timeline->is_object())
			{
				return;
			}
			auto const tracks = timeline->find("Tracks");
			if (tracks == timeline->end() || !tracks->is_array())
			{
				return;
			}
			for (Json const& track : *tracks)
			{
				if (auto const type = track.find("Type"); track.is_object() && type != track.end() && type->is_string())
				{
					TrackTypes.insert(type->get<std::string>());
				}
			}
		}
	};
}

TEST_CASE("VisualScriptEngine: the projects' scene graphs and the libraries they reach load strictly, analyze cleanly and resolve "
          "exactly or by redirect")
{
	Testing::ScriptRuntimeScope runtime;
	size_t checked = 0;
	for (std::filesystem::path const& project : FindProjects())
	{
		CAPTURE(FileSystem::PathToUtf8(project.filename()));
		// The copy outlives the asset manager that opens it.
		Testing::TemporaryDirectory directory;
		Testing::AssetManagerScope assets;
		std::vector<RunSetGraph> const graphs = OpenRunSet(project, directory.GetPath());
		if (graphs.empty())
		{
			continue;
		}
		Testing::ProjectScriptsScope scripts(project);
		for (RunSetGraph const& graph : graphs)
		{
			CAPTURE(graph.Path);
			// Analyzed as the graph itself, which its nodes may name, with the other graphs read from the copy.
			Result<Ref<VisualScriptAnalysis const>> analysis = VisualScriptEngine::Analyze(graph.Document, graph.Handle);
			REQUIRE_MESSAGE(analysis.IsOk(), analysis.GetError());
			for (VisualScriptDiagnostic const& diagnostic : analysis.GetValue()->Diagnostics)
			{
				CAPTURE(diagnostic.Code);
				CAPTURE(diagnostic.Message);
				CHECK(diagnostic.Severity == VisualScriptSeverity::Warning);
			}
			// A node resolved any other way would be an API change without a redirect.
			for (VisualScriptAnalyzedNode const& node : analysis.GetValue()->Nodes)
			{
				CAPTURE(node.Type);
				CHECK((node.Resolved == VisualScriptResolution::Exact || node.Resolved == VisualScriptResolution::Redirect));
			}
			checked++;
		}
	}
	// The feature-test project runs one at least.
	CHECK(checked > 0);
}

// The graph counterpart of the feature-test project's API and component coverage (AGENTS.md): FormatSample.sgraph, which no
// scene runs, does not count.
TEST_CASE("VisualScriptEngine: the feature-test graphs use every built-in node kind, every node of the feature-test scripts, both "
          "forms of an inferred generic and every timeline track type")
{
	Testing::ScriptRuntimeScope runtime;
	std::filesystem::path const project = FileSystem::PathFromUtf8(STRADA_FEATURE_TEST_DIR);
	Testing::TemporaryDirectory directory;
	Testing::AssetManagerScope assets;
	std::vector<RunSetGraph> const graphs = OpenRunSet(project, directory.GetPath());
	Testing::ProjectScriptsScope scripts(project);
	Ref<VisualScriptLibrary const> const library = VisualScriptEngine::GetLibrary();
	REQUIRE(library != nullptr);

	RunSetUses uses;
	for (RunSetGraph const& graph : graphs)
	{
		for (VisualScriptFunction const& function : graph.Document.Functions)
		{
			for (VisualScriptNode const& node : function.Graph.Nodes)
			{
				uses.Add(*library, node);
			}
		}
		for (VisualScriptNode const& node : graph.Document.Graph.Nodes)
		{
			uses.Add(*library, node);
		}
	}

	std::vector<std::string> missing;
	size_t gameNodes = 0;
	for (VisualScriptNodeTemplate const& node : library->GetNodes())
	{
		if (node.Type.find(':') == std::string::npos && !uses.EnabledTypes.contains(node.Type))
		{
			missing.push_back(fmt::format("an enabled node of the built-in kind {}", node.Type));
		}
		if (node.Game)
		{
			gameNodes++;
			if (!uses.Types.contains(node.Type))
			{
				missing.push_back(fmt::format("a node of the game member {}", node.Type));
			}
		}
	}
	if (!uses.InferredWithoutTypeArguments)
	{
		missing.push_back("a node of an inferred generic method without TypeArguments");
	}
	if (!uses.InferredWithTypeArguments)
	{
		missing.push_back("a node of an inferred generic method with TypeArguments");
	}
	for (std::string_view const type : {"float", "Strada.Vector2", "Strada.Vector3", "Strada.Vector4", "Strada.Color", "exec"})
	{
		if (!uses.TrackTypes.contains(std::string(type)))
		{
			missing.push_back(fmt::format("a Flow.Timeline with a track of type {}", type));
		}
	}
	std::sort(missing.begin(), missing.end());
	std::string described = "the graphs Projects/FeatureTest's scenes run, and the libraries they call, lack (add each to "
							"build_features in Projects/FeatureTest/Tools/AuthorGraphs.py):";
	for (std::string const& item : missing)
	{
		described += "\n  " + item;
	}
	CHECK_MESSAGE(missing.empty(), described);
	// The feature-test scripts mark members of each form ([VisualNode] on a class, a struct and members of a script).
	CHECK(gameNodes > 0);
}

namespace
{
	double MedianOf(std::vector<double> values)
	{
		REQUIRE_FALSE(values.empty());
		std::sort(values.begin(), values.end());
		return values[values.size() / 2];
	}

	// The document with its event graph's first node gone (with its links) and a Branch added under a new ID: an edit that
	// shifts every later node's place in the analysis.
	VisualScriptDocument EditOneNode(VisualScriptDocument document)
	{
		uint32_t next = 1;
		auto const takeIDs = [&next](VisualScriptGraph const& graph)
		{
			for (VisualScriptNode const& node : graph.Nodes)
			{
				next = std::max(next, node.ID + 1);
			}
			for (VisualScriptComment const& comment : graph.Comments)
			{
				next = std::max(next, comment.ID + 1);
			}
		};
		takeIDs(document.Graph);
		for (VisualScriptFunction const& function : document.Functions)
		{
			takeIDs(function.Graph);
		}
		if (!document.Graph.Nodes.empty())
		{
			uint32_t const removed = document.Graph.Nodes.front().ID;
			document.Graph.Nodes.erase(document.Graph.Nodes.begin());
			std::erase_if(document.Graph.Links,
			              [removed](VisualScriptLink const& link)
			              {
							  return link.From.Node == removed || link.To.Node == removed;
						  });
		}
		document.Graph.Nodes.push_back(MakeNode(next, "Flow.Branch"));
		return document;
	}

	// Parse(text, previous) against Parse(text), with the reader's counts. The timings given already hold a reading's: the
	// milliseconds are added to, the counts are this reading's.
	VisualScriptAnalysisTimings CheckReadWithPrevious(std::string const& text, VisualScriptAnalysis const* previous)
	{
		VisualScriptAnalysisTimings timings;
		timings.Runtime = 5.0;
		timings.Parse = 7.0;
		timings.Read = 11.0;
		timings.ReusedNodes = 1000;
		timings.ParsedNodes = 1000;
		timings.ReusedDiagnostics = 1000;
		timings.ParsedDiagnostics = 1000;
		Result<VisualScriptAnalysis> const read = VisualScriptAnalysis::Parse(text, previous, &timings);
		Result<VisualScriptAnalysis> const alone = VisualScriptAnalysis::Parse(text);
		Testing::CheckSameAnalysisRead(read, alone);
		CHECK(timings.Runtime == 5.0);
		CHECK(timings.Parse >= 7.0);
		CHECK(timings.Read >= 11.0);
		if (read)
		{
			CHECK(timings.ReusedNodes + timings.ParsedNodes == read.GetValue().Nodes.size());
			CHECK(timings.ReusedDiagnostics + timings.ParsedDiagnostics == read.GetValue().Diagnostics.size());
		}
		else
		{
			CHECK(timings.ReusedNodes == 0);
			CHECK(timings.ReusedDiagnostics == 0);
		}
		if (previous == nullptr)
		{
			CHECK(timings.ReusedNodes == 0);
			CHECK(timings.ReusedDiagnostics == 0);
		}
		return timings;
	}

	// The texts of an analysis, damaged in each way the reader must refuse as reading it whole does, or read whole.
	std::vector<std::string> DamageAnalysisText(std::string const& text)
	{
		REQUIRE(text.size() > 16);
		REQUIRE(text.front() == '{');
		REQUIRE(text.back() == '}');
		std::vector<std::string> damaged;
		// Truncations, everywhere.
		size_t const step = std::max<size_t>(1, text.size() / 97);
		for (size_t length = 0; length < text.size(); length += step)
		{
			damaged.push_back(text.substr(0, length));
		}
		damaged.push_back(text.substr(0, text.size() - 1));
		std::string const body = text.substr(0, text.size() - 1);
		// A duplicate top-level key and extra members, which reading whole takes or refuses as nlohmann reads them.
		damaged.push_back(body + R"(,"Nodes":[])");
		damaged.push_back(body + R"(,"Diagnostics":{})");
		damaged.push_back(body + R"(,"Extra":[1,2])");
		damaged.push_back(R"({"Extra":0,)" + text.substr(1));
		// Keys written with escapes, and a byte order mark before the text and before an element.
		damaged.push_back(R"({"Nod\u0065s")" + text.substr(text.find(':')));
		damaged.push_back("\xEF\xBB\xBF" + text);
		if (size_t const nodes = text.find("\"Nodes\":["); nodes != std::string::npos)
		{
			size_t const element = nodes + 9;
			damaged.push_back(text.substr(0, element) + "\xEF\xBB\xBF" + text.substr(element));
			// A damaged element: the first node's ID negative, and invalid UTF-8 in its strings.
			if (size_t const id = text.find("\"ID\":", element); id != std::string::npos)
			{
				damaged.push_back(text.substr(0, id + 5) + "-" + text.substr(id + 5));
			}
			if (size_t const title = text.find("\"Title\":\"", element); title != std::string::npos)
			{
				damaged.push_back(text.substr(0, title + 9) + "\xFF\xFE" + text.substr(title + 9));
				damaged.push_back(text.substr(0, title + 9) + "\xC3" + text.substr(title + 9));
			}
			// A node whose pin's default nests within ParseJson's limit alone, though past it within the text (the text opens 5
			// brackets around the default's first: the object, Nodes, the node, Pins and the pin), and one within it either way.
			for (size_t const depth : {MaxJsonDepth - 4, MaxJsonDepth - 5})
			{
				damaged.push_back(text.substr(0, element) +
				                  R"({"ID":1,"Function":0,"Type":"Deep","Resolved":"Unknown","Title":"Deep","Kind":"Unknown","Pure":false,)"
				                  R"("Latent":false,"Pins":[{"Name":"Value","Direction":"In","Type":"int","Default":)" +
				                  std::string(depth, '[') + std::string(depth, ']') + "}]}," + text.substr(element));
			}
		}
		// Not of the form.
		damaged.push_back("[" + text + "]");
		damaged.push_back(R"({"Nodes":{},"Diagnostics":[]})");
		damaged.push_back(R"({"Nodes":[],"Diagnostics":7})");
		damaged.push_back(R"({"Diagnostics":[],"Nodes":[]})");
		damaged.push_back(R"({"Nodes":[1],"Diagnostics":[]})");
		damaged.push_back(R"({"Nodes":[],"Diagnostics":[{}]})");
		return damaged;
	}
}

TEST_CASE("VisualScriptLibrary: an analysis read with a previous one equals the analysis read alone and copies the records it has")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	std::vector<Ref<VisualScriptAnalysis const>> analyses;
	std::vector<Ref<VisualScriptAnalysis const>> edits;
	for (std::string_view const name : Testing::SharedVisualScriptFixtures)
	{
		CAPTURE(name);
		VisualScriptDocument const document = ReadFixture(name);
		Result<Ref<VisualScriptAnalysis const>> analysis = VisualScriptEngine::Analyze(document);
		REQUIRE_MESSAGE(analysis.IsOk(), (analysis ? std::string() : analysis.GetError()));
		Result<Ref<VisualScriptAnalysis const>> edited = VisualScriptEngine::Analyze(EditOneNode(document));
		REQUIRE_MESSAGE(edited.IsOk(), (edited ? std::string() : edited.GetError()));
		analyses.push_back(analysis.TakeValue());
		edits.push_back(edited.TakeValue());
	}
	// An analysis built by hand, with nodes and no spans: nothing of it is reused.
	VisualScriptAnalysis byHand;
	byHand.Nodes = analyses.front()->Nodes;
	byHand.Diagnostics = analyses.front()->Diagnostics;
	byHand.Text = analyses.front()->Text;

	for (size_t index = 0; index < analyses.size(); index++)
	{
		CAPTURE(Testing::SharedVisualScriptFixtures[index]);
		VisualScriptAnalysis const& analysis = *analyses[index];
		VisualScriptAnalysis const& edited = *edits[index];
		VisualScriptAnalysis const& unrelated = *analyses[(index + 1) % analyses.size()];
		REQUIRE(analysis.NodeSpans.size() == analysis.Nodes.size());
		REQUIRE(analysis.DiagnosticSpans.size() == analysis.Diagnostics.size());
		REQUIRE(analysis.NodeIndex.size() == analysis.Nodes.size());

		// With the analysis of the same text, every record is copied.
		VisualScriptAnalysisTimings const same = CheckReadWithPrevious(analysis.Text, &analysis);
		CHECK(same.ParsedNodes == 0);
		CHECK(same.ParsedDiagnostics == 0);
		CHECK(same.ReusedNodes == analysis.Nodes.size());

		// One edit away, either way: the records that kept their bytes are copied, wherever they moved to.
		VisualScriptAnalysisTimings const forward = CheckReadWithPrevious(edited.Text, &analysis);
		VisualScriptAnalysisTimings const back = CheckReadWithPrevious(analysis.Text, &edited);
		if (analysis.Nodes.size() > 3)
		{
			CHECK(forward.ReusedNodes > 0);
			CHECK(back.ReusedNodes > 0);
		}
		CHECK(forward.ParsedNodes < edited.Nodes.size());

		// An unrelated analysis, none, and one built by hand.
		CheckReadWithPrevious(analysis.Text, &unrelated);
		CheckReadWithPrevious(analysis.Text, nullptr);
		VisualScriptAnalysisTimings const handBuilt = CheckReadWithPrevious(analysis.Text, &byHand);
		CHECK(handBuilt.ReusedNodes == 0);
		CHECK(handBuilt.ReusedDiagnostics == 0);

		// Read as nlohmann writes it, indented: no element keeps its bytes, and the reading is still the same.
		Result<Json> const json = ParseJson(analysis.Text);
		REQUIRE(json.IsOk());
		VisualScriptAnalysisTimings const indented = CheckReadWithPrevious(DumpJson(json.GetValue()), &analysis);
		if (!analysis.Nodes.empty())
		{
			CHECK(indented.ReusedNodes == 0);
		}
	}
}

TEST_CASE("VisualScriptLibrary: a damaged analysis is refused, or read whole, with the very error or reading it gets alone")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	Result<Ref<VisualScriptAnalysis const>> analyzed = VisualScriptEngine::Analyze(ReadFixture("Editor.json"));
	REQUIRE_MESSAGE(analyzed.IsOk(), (analyzed ? std::string() : analyzed.GetError()));
	VisualScriptAnalysis const& previous = *analyzed.GetValue();
	REQUIRE_FALSE(previous.Diagnostics.empty());
	size_t refused = 0;
	size_t readWhole = 0;
	for (std::string const& text : DamageAnalysisText(previous.Text))
	{
		CAPTURE(text.substr(0, 120));
		CheckReadWithPrevious(text, &previous);
		Result<VisualScriptAnalysis> const read = VisualScriptAnalysis::Parse(text, &previous);
		if (read.IsError())
		{
			refused++;
		}
		else if (read.GetValue().NodeSpans.empty() && !read.GetValue().Nodes.empty())
		{
			// Of a form the scan does not take (extra members, escaped keys, a byte order mark), read from the whole DOM.
			readWhole++;
		}
	}
	CHECK(refused > 97);
	CHECK(readWhole >= 3);

	// ParseJson's nesting limit holds for the whole text: a node whose parts all read alone is refused when the text nests past
	// it, with and without the previous analysis.
	auto const deepText = [](size_t depth)
	{
		return R"({"Nodes":[{"ID":1,"Function":0,"Type":"Deep","Resolved":"Unknown","Title":"Deep","Kind":"Unknown","Pure":false,)"
		       R"("Latent":false,"Pins":[{"Name":"Value","Direction":"In","Type":"int","Default":)" +
		       std::string(depth, '[') + std::string(depth, ']') + R"(}]}],"Diagnostics":[]})";
	};
	Result<VisualScriptAnalysis> const within = VisualScriptAnalysis::Parse(deepText(MaxJsonDepth - 5));
	REQUIRE_MESSAGE(within.IsOk(), (within ? std::string() : within.GetError()));
	Result<VisualScriptAnalysis> const withinAgain = VisualScriptAnalysis::Parse(deepText(MaxJsonDepth - 5), &within.GetValue());
	Testing::CheckSameAnalysisRead(withinAgain, within);
	for (VisualScriptAnalysis const* const candidate : {&within.GetValue(), &previous, static_cast<VisualScriptAnalysis const*>(nullptr)})
	{
		Result<VisualScriptAnalysis> const past = VisualScriptAnalysis::Parse(deepText(MaxJsonDepth - 4), candidate);
		REQUIRE(past.IsError());
		CHECK(past.GetError() == fmt::format("invalid JSON: values are nested deeper than {} levels", MaxJsonDepth));
	}

	// The errors name what reading whole names.
	struct Case
	{
		std::string Text;
		std::string_view Error;
	};
	std::vector<Case> const cases = {
		{R"({"Nodes":{},"Diagnostics":[]})", "Nodes: expected an array"},
		{R"({"Nodes":[],"Diagnostics":7})", "Diagnostics: expected an array"},
		{R"({"Nodes":[1],"Diagnostics":[]})", "Nodes[0]: expected an object"},
		{R"({"Nodes":[],"Diagnostics":[{"Severity":"Fatal","Code":"X","Function":0,"Message":""}]})",
	     "Diagnostics[0].Severity: expected one of Error, Warning"},
	};
	for (Case const& refusal : cases)
	{
		CAPTURE(refusal.Text);
		Result<VisualScriptAnalysis> const read = VisualScriptAnalysis::Parse(refusal.Text, &previous);
		REQUIRE(read.IsError());
		CHECK(read.GetError() == refusal.Error);
	}
}

TEST_CASE("VisualScript: an analysis built by hand finds its nodes")
{
	// Production code and tests fill Nodes directly, without an index: FindNode scans them.
	VisualScriptAnalysis analysis;
	for (uint32_t const id : {7u, 3u, 9u, 3u})
	{
		VisualScriptAnalyzedNode& node = analysis.Nodes.emplace_back();
		node.ID = id;
		node.Title = fmt::format("Node {} at {}", id, analysis.Nodes.size() - 1);
	}
	auto const check = [&analysis]()
	{
		for (uint32_t const id : {7u, 9u})
		{
			VisualScriptAnalyzedNode const* const found = analysis.FindNode(id);
			REQUIRE(found != nullptr);
			CHECK(found->ID == id);
		}
		// A repeated ID finds its first node, as the scan does.
		REQUIRE(analysis.FindNode(3) != nullptr);
		CHECK(analysis.FindNode(3)->Title == "Node 3 at 1");
		CHECK(analysis.FindNode(1) == nullptr);
		CHECK(analysis.FindNode(10) == nullptr);
	};
	CHECK(analysis.NodeIndex.empty());
	check();

	// With the index built, the same answers by binary search.
	analysis.BuildNodeIndex();
	CHECK(analysis.NodeIndex == std::vector<std::pair<uint32_t, uint32_t>>{{3, 1}, {3, 3}, {7, 0}, {9, 2}});
	check();

	// A node added after it leaves the index shorter than Nodes: FindNode scans again, and finds the new one.
	analysis.Nodes.emplace_back().ID = 12;
	REQUIRE(analysis.FindNode(12) != nullptr);
	check();
	analysis.BuildNodeIndex();
	CHECK(analysis.FindNode(12) == &analysis.Nodes.back());
}

TEST_CASE("VisualScriptLibrary: a parsed analysis finds its nodes by binary search of an index of every node")
{
	Result<VisualScriptAnalysis> const parsed = VisualScriptAnalysis::Parse(
		R"({"Nodes":[{"ID":40,"Function":0,"Type":"A","Resolved":"Unknown","Title":"first","Kind":"Unknown","Pure":false,"Latent":false,)"
		R"("Pins":[]},{"ID":2,"Function":1,"Type":"B","Resolved":"Unknown","Title":"second","Kind":"Unknown","Pure":false,"Latent":false,)"
		R"("Pins":[]},{"ID":40,"Function":0,"Type":"C","Resolved":"Unknown","Title":"repeated","Kind":"Unknown","Pure":false,)"
		R"("Latent":false,"Pins":[]}],"Diagnostics":[]})");
	REQUIRE_MESSAGE(parsed.IsOk(), (parsed ? std::string() : parsed.GetError()));
	VisualScriptAnalysis const& analysis = parsed.GetValue();
	CHECK(analysis.NodeIndex == std::vector<std::pair<uint32_t, uint32_t>>{{2, 1}, {40, 0}, {40, 2}});
	REQUIRE(analysis.FindNode(2) != nullptr);
	CHECK(analysis.FindNode(2)->Title == "second");
	REQUIRE(analysis.FindNode(40) != nullptr);
	CHECK(analysis.FindNode(40)->Title == "first");
	CHECK(analysis.FindNode(0) == nullptr);
	CHECK(analysis.FindNode(41) == nullptr);
	CHECK(analysis.NodeSpans.size() == 3);
}

TEST_CASE("VisualScriptEngine: Analyze reports its phases and reads a graph's analysis with the one it kept of that graph")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	VisualScriptDocument const document = ReadFixture("Language.json");
	AssetHandle const graph(UUID(90001));
	CHECK(VisualScriptEngine::GetLastAnalysisTimings().ParsedNodes == 0);
	Result<Ref<VisualScriptAnalysis const>> const first = VisualScriptEngine::Analyze(document, graph);
	REQUIRE_MESSAGE(first.IsOk(), (first ? std::string() : first.GetError()));
	size_t const nodes = first.GetValue()->Nodes.size();
	REQUIRE(nodes > 0);
	VisualScriptAnalysisTimings const read = VisualScriptEngine::GetLastAnalysisTimings();
	CHECK(read.Text > 0.0);
	CHECK(read.References >= 0.0);
	CHECK(read.Runtime > 0.0);
	CHECK(read.Parse > 0.0);
	CHECK(read.Read > 0.0);
	CHECK(read.ParsedNodes == nodes);
	CHECK(read.ReusedNodes == 0);

	// Analyzed again, the graph's analysis is read with the one kept: every record is copied.
	Result<Ref<VisualScriptAnalysis const>> const again = VisualScriptEngine::Analyze(document, graph);
	REQUIRE(again.IsOk());
	CHECK(again.GetValue() != first.GetValue());
	CHECK(VisualScriptEngine::GetLastAnalysisTimings().ReusedNodes == nodes);
	CHECK(VisualScriptEngine::GetLastAnalysisTimings().ParsedNodes == 0);
	Testing::CheckSameAnalysisRead(VisualScriptAnalysis::Parse(again.GetValue()->Text),
	                               VisualScriptAnalysis::Parse(first.GetValue()->Text));

	// One analysis is kept per graph, for the MaxKeptAnalyses graphs analyzed last: a ninth graph drops the least recent.
	for (uint64_t other = 1; other <= VisualScriptEngine::MaxKeptAnalyses; other++)
	{
		REQUIRE(VisualScriptEngine::Analyze(document, AssetHandle(UUID(90001 + other))).IsOk());
		CHECK(VisualScriptEngine::GetLastAnalysisTimings().ReusedNodes == 0);
	}
	REQUIRE(VisualScriptEngine::Analyze(document, graph).IsOk());
	CHECK(VisualScriptEngine::GetLastAnalysisTimings().ReusedNodes == 0);
	// That dropped the next least recent, and kept the others.
	REQUIRE(VisualScriptEngine::Analyze(document, AssetHandle(UUID(90002))).IsOk());
	CHECK(VisualScriptEngine::GetLastAnalysisTimings().ReusedNodes == 0);
	REQUIRE(VisualScriptEngine::Analyze(document, AssetHandle(UUID(90004))).IsOk());
	CHECK(VisualScriptEngine::GetLastAnalysisTimings().ReusedNodes == nodes);
}

TEST_CASE("VisualScript: the grid graph of the performance guards is pinned by Benchmarks/Grid-4x5.json")
{
	// spec-slice4 section 9.0: the C++ generator and the scripting runtime's C# port are held to the same file.
	std::filesystem::path const path = FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Benchmarks" / "Grid-4x5.json";
	std::string const written = VisualScriptSerializer::WriteContentText(Testing::MakeGridGraph(4, 5, 2, 1));
	if (Platform::ReadEnvironmentVariable("STRADA_UPDATE_GOLDEN").value_or("") == "1")
	{
		REQUIRE(FileSystem::WriteTextFile(path, written).IsOk());
		MESSAGE("Updated " << FileSystem::PathToUtf8(path));
		return;
	}
	Result<std::string> const pinned = FileSystem::ReadTextFile(path);
	REQUIRE_MESSAGE(pinned.IsOk(), "Missing fixture (run with STRADA_UPDATE_GOLDEN=1): " << FileSystem::PathToUtf8(path));
	CHECK(written == pinned.GetValue());
	// It loads as automation loads documents, to the generator's document.
	Result<Json> const json = ParseJson(pinned.GetValue());
	REQUIRE(json.IsOk());
	std::vector<VisualScriptIssue> issues;
	Result<VisualScriptDocument> const document =
		VisualScriptSerializer::DeserializeDocument(json.GetValue(), Testing::MakeStrictContext(), &issues);
	REQUIRE_MESSAGE(document.IsOk(), (document ? std::string() : document.GetError()));
	CHECK_MESSAGE(issues.empty(), Testing::DescribeIssues(issues));
	CHECK(document.GetValue() == Testing::MakeGridGraph(4, 5, 2, 1));
}

TEST_CASE("VisualScriptEngine: the analysis of a 2,000-node grid after a link change parses none of its nodes again, in its time "
          "on request")
{
	// spec-slice4 section 21: the read of the grid's analysis after a change of a link that changes no node (an exec link
	// between nodes without wildcard pins) copies every node record.
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	AssetHandle const graph(UUID(91001));
	VisualScriptDocument const grid = Testing::MakeGridGraph(40, 50, 2, 20);
	REQUIRE(grid.Graph.Nodes.size() == 2000);
	Result<Ref<VisualScriptAnalysis const>> const first = VisualScriptEngine::Analyze(grid, graph);
	REQUIRE_MESSAGE(first.IsOk(), (first ? std::string() : first.GetError()));
	VisualScriptAnalysis const& analysis = *first.GetValue();
	REQUIRE(analysis.Nodes.size() == 2000);

	// A link from a Delay's Completed to a Branch's Execute: neither node has a wildcard, and no diagnostic names the link.
	auto const isWildcardFree = [&analysis](uint32_t node)
	{
		VisualScriptAnalyzedNode const* const analyzed = analysis.FindNode(node);
		REQUIRE(analyzed != nullptr);
		return analyzed->Type == "Flow.Delay" || analyzed->Type == "Flow.Branch";
	};
	auto const link = std::find_if(grid.Graph.Links.begin(), grid.Graph.Links.end(),
	                               [&analysis, &isWildcardFree](VisualScriptLink const& candidate)
	                               {
									   bool const named = std::any_of(analysis.Diagnostics.begin(), analysis.Diagnostics.end(),
		                                                              [&candidate](VisualScriptDiagnostic const& diagnostic)
		                                                              {
																		  return diagnostic.Link == candidate;
																	  });
									   return candidate.From.Pin == "Completed" && candidate.To.Pin == "Execute" &&
		                                      isWildcardFree(candidate.From.Node) && isWildcardFree(candidate.To.Node) && !named;
								   });
	REQUIRE(link != grid.Graph.Links.end());
	VisualScriptDocument unlinked = grid;
	unlinked.Graph.Links.erase(unlinked.Graph.Links.begin() + (link - grid.Graph.Links.begin()));

	std::vector<double> reads;
	bool const performance = Platform::ReadEnvironmentVariable("STRADA_PERFORMANCE_TESTS").value_or("") == "1";
	for (int round = 0, rounds = performance ? 7 : 1; round < rounds; round++)
	{
		for (VisualScriptDocument const* const document : std::array<VisualScriptDocument const*, 2>{&unlinked, &grid})
		{
			REQUIRE(VisualScriptEngine::Analyze(*document, graph).IsOk());
			VisualScriptAnalysisTimings const timings = VisualScriptEngine::GetLastAnalysisTimings();
			CHECK(timings.ParsedNodes == 0);
			CHECK(timings.ReusedNodes == 2000);
			CHECK(timings.ParsedDiagnostics == 0);
			reads.push_back(timings.Parse + timings.Read);
		}
	}
	MESSAGE("the analysis read of a 2,000-node grid after a link change: median " << MedianOf(reads) << " ms (Parse + Read)");
	if (performance)
	{
		// Three times the figure track I measured (an AMD Ryzen 5 7535HS laptop, Windows 11, MSVC 19.51; median of 14 reads):
		// reading the whole text took 2148 ms in Debug and 43.6 ms in Release before.
#if defined(ST_DEBUG)
		constexpr double MeasuredMilliseconds = 28.0;
#else
		constexpr double MeasuredMilliseconds = 3.2;
#endif
		CHECK(MedianOf(reads) <= 3.0 * MeasuredMilliseconds);
	}
}

// --- The live channel (spec-slice4 section 8.5) ---

TEST_CASE("VisualScriptEngine: a send's receipt says what the edit did, KeptLastWorking exactly when the log says the previous "
          "version keeps running, else NotRunning")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	Ref<VisualScriptAsset> const asset = CreateRef<VisualScriptAsset>(MakeCountDocument(1));
	AssetHandle const graph = AssetManager::AddMemoryAsset(asset, "Counter");

	// Loaded: the instances (none yet) run it, and its variables are new to the handle.
	double const before = Platform::GetTime();
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	Ref<VisualScriptLiveEdit const> receipt = FindLastReceipt(graph);
	CHECK(receipt->Outcome == VisualScriptLiveOutcome::Applied);
	CHECK(receipt->Name == "Counter");
	CHECK_FALSE(receipt->Cause.IsValid());
	CHECK_FALSE(receipt->Deferred);
	CHECK(receipt->Instances == 0);
	CHECK(receipt->AddedVariables == std::vector<std::string>{"Count"});
	CHECK(receipt->Errors.empty());
	CHECK(receipt->Time >= before);
	CHECK(receipt->Time <= Platform::GetTime());
	REQUIRE(VisualScriptEngine::CreateInstance(UUID(1001), graph, {}).IsOk());
	VisualScriptEngine::InvokeOnCreate(UUID(1001));

	// The same program again (a node moved) is no send, so no receipt.
	uint64_t const serial = VisualScriptEngine::GetLiveLog()->Serial;
	VisualScriptDocument moved = MakeCountDocument(1);
	moved.Graph.Nodes[0].Position = glm::vec2(80.0f, 0.0f);
	CHECK_FALSE(SendsDocument(*asset, graph, moved));
	CHECK(VisualScriptEngine::GetLiveLog()->Serial == serial);

	// Count's type changed and a variable added: one instance swapped, Count reset with why.
	VisualScriptDocument retyped = MakeCountDocument(1);
	retyped.Variables[0] = MakeVariable(1, "Count", "float", 0.0, true);
	retyped.Graph.Nodes[1].Inputs["Value"] = 1.0;
	retyped.Variables.push_back(MakeVariable(2, "Shield", "int", 3, false));
	CHECK(SendsDocument(*asset, graph, retyped));
	receipt = FindLastReceipt(graph);
	CHECK(receipt->Outcome == VisualScriptLiveOutcome::Applied);
	CHECK(receipt->Instances == 1);
	CHECK(receipt->KeptVariables.empty());
	CHECK(receipt->ResetVariables == std::vector<VisualScriptLiveVariable>{{"Count", "type changed from int to float"}});
	CHECK(receipt->AddedVariables == std::vector<std::string>{"Shield"});
	CHECK(receipt->RemovedVariables.empty());

	// Broken while a working program runs: the log says it keeps running, and so does the receipt, with the log's errors.
	uint64_t logStart = Log::GetNextEntryIndex();
	CHECK(SendsDocument(*asset, graph, MakeBrokenDocument()));
	receipt = FindLastReceipt(graph);
	CHECK(Testing::WasLogged(logStart, "; the previous version keeps running"));
	CHECK(VisualScriptEngine::HasWorkingProgram(graph));
	CHECK(receipt->Outcome == VisualScriptLiveOutcome::KeptLastWorking);
	CHECK(receipt->Errors.starts_with("1 error; first: "));
	CHECK(receipt->Errors.ends_with(" (node 12)"));
	CHECK(Testing::WasLogged(logStart, fmt::format("Visual script 'Counter' has {}; the previous version keeps running", receipt->Errors)));
	CHECK(receipt->Instances == 0);

	// Fixed: applied against the program the instance ran, whose Count was a float and which had Shield.
	CHECK(SendsDocument(*asset, graph, MakeCountDocument(3)));
	receipt = FindLastReceipt(graph);
	CHECK(receipt->Outcome == VisualScriptLiveOutcome::Applied);
	CHECK(receipt->Instances == 1);
	CHECK(receipt->ResetVariables == std::vector<VisualScriptLiveVariable>{{"Count", "type changed from float to int"}});
	CHECK(receipt->RemovedVariables == std::vector<std::string>{"Shield"});

	// Broken from the start: nothing runs, the log does not say it keeps running, and the receipt says it does not.
	AssetHandle const failing = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeBrokenDocument()), "Broken");
	logStart = Log::GetNextEntryIndex();
	REQUIRE(VisualScriptEngine::EnsureLoaded(failing).IsOk());
	receipt = FindLastReceipt(failing);
	CHECK(receipt->Outcome == VisualScriptLiveOutcome::NotRunning);
	CHECK_FALSE(VisualScriptEngine::HasWorkingProgram(failing));
	CHECK(Testing::WasLogged(logStart, fmt::format("Visual script 'Broken' has {}", receipt->Errors)));
	CHECK_FALSE(Testing::WasLogged(logStart, "keeps running"));

	// After a game-assembly change the working program goes first, and still runs: the broken document is KeptLastWorking
	// again, as the log says.
	asset->SetDocument(MakeBrokenDocument());
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	VisualScriptEngine::DestroyAllInstances();
	REQUIRE(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
	logStart = Log::GetNextEntryIndex();
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	receipt = FindLastReceipt(graph);
	CHECK(receipt->Outcome == VisualScriptLiveOutcome::KeptLastWorking);
	CHECK(Testing::WasLogged(logStart, "; the previous version keeps running"));
	CHECK(host.Exceptions == 0);
}

TEST_CASE("VisualScriptEngine: a library edit's receipts name the library as their Cause, and DescribeLibraryUsers tells who runs "
          "which version")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	Ref<VisualScriptAsset> const libraryAsset = CreateRef<VisualScriptAsset>(MakeLibraryDocument());
	AssetHandle const library = AssetManager::AddMemoryAsset(libraryAsset, "Motion");
	AssetHandle const first = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeCallerDocument(library)), "Caller A");
	AssetHandle const second = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeCallerDocument(library)), "Caller B");
	AssetHandle const other = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeCountDocument(1)), "Counter");
	CHECK(VisualScriptEngine::DescribeLibraryUsers(library).empty());
	REQUIRE(VisualScriptEngine::EnsureLoaded(second).IsOk());
	REQUIRE(VisualScriptEngine::CreateInstance(UUID(1001), first, {}).IsOk());
	REQUIRE(VisualScriptEngine::EnsureLoaded(other).IsOk());
	CHECK_FALSE(FindLastReceipt(first)->Cause.IsValid());
	// Both compiled with the current version; an entity runs the first.
	std::vector<VisualScriptLibraryUser> const both = {{first, "Caller A", true, true, 1}, {second, "Caller B", true, true, 0}};
	CHECK(VisualScriptEngine::DescribeLibraryUsers(library) == both);
	CHECK(VisualScriptEngine::DescribeLibraryUsers(library)[0].Runs());
	CHECK_FALSE(VisualScriptEngine::DescribeLibraryUsers(library)[1].Runs());
	CHECK(VisualScriptEngine::DescribeLibraryUsers(other).empty());
	CHECK(VisualScriptEngine::DescribeLibraryUsers(AssetHandle(UUID(424242))).empty());
	CHECK(VisualScriptEngine::RunsStoredProgram(first));
	CHECK(VisualScriptEngine::RunsStoredProgram(second));
	CHECK_FALSE(VisualScriptEngine::RunsStoredProgram(library));
	CHECK_FALSE(VisualScriptEngine::RunsStoredProgram(AssetHandle(UUID(424242))));

	// A node of the library moved: the same program, which every user runs.
	VisualScriptDocument moved = MakeLibraryDocument();
	moved.Functions[0].Graph.Nodes[1].Position = glm::vec2(160.0f, 0.0f);
	libraryAsset->SetDocument(moved);
	CHECK(VisualScriptEngine::DescribeLibraryUsers(library) == both);
	CHECK(VisualScriptEngine::RunsStoredProgram(second));

	// Its body edited: the first caller sent again is the library's doing; the second runs the earlier version until sent,
	// so it does not run what is stored although its own document did not change.
	libraryAsset->SetDocument(MakeLibraryDocument(4));
	CHECK_FALSE(VisualScriptEngine::RunsStoredProgram(second));
	REQUIRE(VisualScriptEngine::EnsureLoaded(first).IsOk());
	Ref<VisualScriptLiveEdit const> receipt = FindLastReceipt(first);
	CHECK(receipt->Cause == library);
	CHECK(receipt->Outcome == VisualScriptLiveOutcome::Applied);
	CHECK(receipt->Instances == 1);
	CHECK(VisualScriptEngine::DescribeLibraryUsers(library) ==
	      std::vector<VisualScriptLibraryUser>{{first, "Caller A", true, true, 1}, {second, "Caller B", false, true, 0}});
	CHECK(VisualScriptEngine::RunsStoredProgram(first));
	CHECK_FALSE(VisualScriptEngine::RunsStoredProgram(second));
	REQUIRE(VisualScriptEngine::EnsureLoaded(second).IsOk());
	CHECK(FindLastReceipt(second)->Cause == library);
	CHECK(VisualScriptEngine::DescribeLibraryUsers(library) == both);
	CHECK(VisualScriptEngine::RunsStoredProgram(second));

	// Broken: its callers keep the version that worked, the library's doing; their own documents are unchanged, yet what
	// runs is not what is stored.
	VisualScriptDocument broken = MakeLibraryDocument();
	broken.Functions[0].Graph.Nodes.push_back(MakeNode(13, "Sample.NoSuchNode"));
	libraryAsset->SetDocument(broken);
	REQUIRE(VisualScriptEngine::EnsureLoaded(first).IsOk());
	receipt = FindLastReceipt(first);
	CHECK(receipt->Cause == library);
	CHECK(receipt->Outcome == VisualScriptLiveOutcome::KeptLastWorking);
	CHECK_FALSE(receipt->Errors.empty());
	CHECK(VisualScriptEngine::HasWorkingProgram(first));
	CHECK_FALSE(VisualScriptEngine::RunsStoredProgram(first));
	CHECK(VisualScriptEngine::DescribeLibraryUsers(library) ==
	      std::vector<VisualScriptLibraryUser>{{first, "Caller A", false, true, 1}, {second, "Caller B", false, true, 0}});

	// A caller that never compiled runs no version of the library, nor does one whose working program does not call it
	// (only its broken document does): both are listed, neither compiled with it.
	VisualScriptDocument failing = MakeCallerDocument(library);
	failing.Graph.Nodes.push_back(MakeNode(30, "Sample.NoSuchNode"));
	AssetHandle const third = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(failing), "Caller C");
	REQUIRE(VisualScriptEngine::EnsureLoaded(third).IsOk());
	CHECK(FindLastReceipt(third)->Outcome == VisualScriptLiveOutcome::NotRunning);
	CHECK_FALSE(VisualScriptEngine::RunsStoredProgram(third));
	Ref<VisualScriptAsset> const otherAsset = AssetManager::GetAsset<VisualScriptAsset>(other);
	REQUIRE(otherAsset != nullptr);
	CHECK(SendsDocument(*otherAsset, other, failing));
	CHECK(FindLastReceipt(other)->Outcome == VisualScriptLiveOutcome::KeptLastWorking);
	CHECK_FALSE(VisualScriptEngine::RunsStoredProgram(other));
	CHECK(VisualScriptEngine::DescribeLibraryUsers(library) == std::vector<VisualScriptLibraryUser>{{first, "Caller A", false, true, 1},
	                                                                                                {second, "Caller B", false, true, 0},
	                                                                                                {third, "Caller C", false, false, 0},
	                                                                                                {other, "Counter", false, false, 0}});

	// The caller's own edit is no library's doing, and with its entity gone no entity runs it.
	Ref<VisualScriptAsset> const firstAsset = AssetManager::GetAsset<VisualScriptAsset>(first);
	REQUIRE(firstAsset != nullptr);
	CHECK(SendsDocument(*firstAsset, first, MakeCountDocument(2)));
	CHECK_FALSE(FindLastReceipt(first)->Cause.IsValid());
	CHECK(VisualScriptEngine::RunsStoredProgram(first));
	CHECK(SendsDocument(*firstAsset, first, MakeCallerDocument(library)));
	CHECK(FindLastReceipt(first)->Outcome == VisualScriptLiveOutcome::KeptLastWorking);
	VisualScriptEngine::DestroyInstance(UUID(1001));
	CHECK(VisualScriptEngine::DescribeLibraryUsers(library)[0] == VisualScriptLibraryUser{first, "Caller A", false, false, 0});
	CHECK(VisualScriptEngine::DescribeLibraryUsers(library)[1] == VisualScriptLibraryUser{second, "Caller B", false, true, 0});
	CHECK(host.Exceptions == 0);
}

TEST_CASE("VisualScriptEngine: GetWorkingSince tells when the working program last became another program")
{
	Testing::AssetManagerScope assets;
	Ref<VisualScriptAsset> const asset = CreateRef<VisualScriptAsset>(MakeCountDocument(1));
	AssetHandle const graph = AssetManager::AddMemoryAsset(asset, "Counter");
	{
		Testing::ScriptRuntimeScope runtime;
		CHECK(VisualScriptEngine::GetWorkingSince(graph) == 0.0);
		double before = Platform::GetTime();
		REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
		double const loaded = VisualScriptEngine::GetWorkingSince(graph);
		CHECK(loaded >= before);
		CHECK(loaded <= Platform::GetTime());

		// The same program (a node moved, a broken document beside it, the game assembly changed) keeps its time.
		VisualScriptDocument moved = MakeCountDocument(1);
		moved.Graph.Nodes[1].Position = glm::vec2(240.0f, 0.0f);
		CHECK_FALSE(SendsDocument(*asset, graph, moved));
		CHECK(VisualScriptEngine::GetWorkingSince(graph) == loaded);
		CHECK(SendsDocument(*asset, graph, MakeBrokenDocument()));
		CHECK(VisualScriptEngine::GetWorkingSince(graph) == loaded);
		REQUIRE(ScriptEngine::LoadGameAssembly(Testing::GetTestScriptsPath()).IsOk());
		REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
		REQUIRE(VisualScriptEngine::HasWorkingProgram(graph));
		CHECK(VisualScriptEngine::GetWorkingSince(graph) == loaded);

		// Another program: from its send.
		before = Platform::GetTime();
		CHECK(SendsDocument(*asset, graph, MakeCountDocument(2)));
		CHECK(VisualScriptEngine::GetWorkingSince(graph) >= before);

		// Without a working program: 0.
		AssetHandle const failing = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeBrokenDocument()), "Broken");
		REQUIRE(VisualScriptEngine::EnsureLoaded(failing).IsOk());
		CHECK(VisualScriptEngine::GetWorkingSince(failing) == 0.0);
		CHECK_FALSE(VisualScriptEngine::GetLiveLog()->Edits.empty());
	}

	// Without the runtime: 0, no users, and an empty log that clearing leaves empty.
	CHECK(VisualScriptEngine::GetWorkingSince(graph) == 0.0);
	CHECK(VisualScriptEngine::DescribeLibraryUsers(graph).empty());
	VisualScriptEngine::ClearLiveLog();
	Ref<VisualScriptLiveLog const> const empty = VisualScriptEngine::GetLiveLog();
	REQUIRE(empty != nullptr);
	CHECK(empty->Edits.empty());
	CHECK(empty->Problems.empty());
	CHECK(empty->Serial == 0);
}

TEST_CASE("VisualScriptEngine: the live log coalesces a node's 1,000 aborts into one problem, keeps every revision as it was "
          "answered, and ClearLiveLog empties it")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	Ref<VisualScriptAsset> const asset = CreateRef<VisualScriptAsset>(MakeUpdateDivisionDocument());
	AssetHandle const graph = AssetManager::AddMemoryAsset(asset, "Division");
	UUID const entity(1001);
	REQUIRE(VisualScriptEngine::CreateInstance(entity, graph, {}).IsOk());
	for (int update = 0; update < 1000; update++)
	{
		VisualScriptEngine::InvokeOnUpdate(entity, 1.0f / 60.0f);
	}
	CHECK(host.Exceptions == 1000);
	Ref<VisualScriptLiveLog const> const held = VisualScriptEngine::GetLiveLog();
	REQUIRE(held->Problems.size() == 1);
	Ref<VisualScriptRuntimeProblem const> const problem = held->Problems[0];
	CHECK(problem->Count == 1000);
	CHECK(problem->Severity == VisualScriptSeverity::Error);
	CHECK(problem->Graph == graph);
	CHECK(problem->Document == graph);
	CHECK(problem->Function == 0);
	CHECK(problem->Node == 12);
	CHECK(problem->Title == "Divide");
	CHECK(problem->Entity == entity);
	CHECK(problem->Message == "integer division by zero");
	CHECK(problem->Calls.empty());
	CHECK(problem->FirstTime > 0.0);
	CHECK(problem->FirstTime == problem->LastTime);
	CHECK(held->Dropped == 0);
	REQUIRE(held->Edits.size() == 1);
	CHECK(held->Serial == problem->Serial);
	// Nothing new: the same revision.
	CHECK(VisualScriptEngine::GetLiveLog() == held);

	// A send and another abort make a newer revision, sharing what did not change; the one held keeps its entries and size.
	VisualScriptDocument edited = MakeUpdateDivisionDocument();
	edited.Graph.Nodes[2].Inputs["A"] = 2;
	CHECK(SendsDocument(*asset, graph, edited));
	VisualScriptEngine::InvokeOnUpdate(entity, 1.0f / 60.0f);
	Ref<VisualScriptLiveLog const> const newer = VisualScriptEngine::GetLiveLog();
	REQUIRE(newer != held);
	REQUIRE(held->Edits.size() == 1);
	REQUIRE(held->Problems.size() == 1);
	CHECK(held->Problems[0] == problem);
	CHECK(problem->Count == 1000);
	REQUIRE(newer->Edits.size() == 2);
	CHECK(newer->Edits[0] == held->Edits[0]);
	CHECK(newer->Edits[1]->Instances == 1);
	REQUIRE(newer->Problems.size() == 1);
	CHECK(newer->Problems[0] != problem);
	CHECK(newer->Problems[0]->Count == 1001);
	CHECK(newer->Problems[0]->FirstTime == problem->FirstTime);
	CHECK(newer->Problems[0]->LastTime >= problem->LastTime);
	CHECK(newer->Serial > held->Serial);

	// A second entity's problem is another entry, after the first by serial; the first's newer occurrence moves it last.
	UUID const another(1002);
	REQUIRE(VisualScriptEngine::CreateInstance(another, graph, {}).IsOk());
	VisualScriptEngine::InvokeOnUpdate(another, 1.0f / 60.0f);
	VisualScriptEngine::InvokeOnUpdate(entity, 1.0f / 60.0f);
	Ref<VisualScriptLiveLog const> const both = VisualScriptEngine::GetLiveLog();
	REQUIRE(both->Problems.size() == 2);
	CHECK(both->Problems[0]->Entity == another);
	CHECK(both->Problems[0]->Count == 1);
	CHECK(both->Problems[1]->Entity == entity);
	CHECK(both->Problems[1]->Count == 1002);
	CHECK(newer->Problems.size() == 1);

	// Cleared: nothing, here and in the runtime, so counts start again; serials go on.
	VisualScriptEngine::ClearLiveLog();
	Ref<VisualScriptLiveLog const> const cleared = VisualScriptEngine::GetLiveLog();
	CHECK(cleared->Edits.empty());
	CHECK(cleared->Problems.empty());
	CHECK(cleared->Dropped == 0);
	CHECK(cleared->Serial == both->Serial);
	CHECK(both->Problems.size() == 2);
	VisualScriptEngine::InvokeOnUpdate(entity, 1.0f / 60.0f);
	Ref<VisualScriptLiveLog const> const again = VisualScriptEngine::GetLiveLog();
	REQUIRE(again->Problems.size() == 1);
	CHECK(again->Problems[0]->Count == 1);
	CHECK(again->Problems[0]->Serial > both->Serial);

	// Beyond the 512 distinct problems the runtime keeps, occurrences are counted as dropped.
	VisualScriptEngine::ClearLiveLog();
	constexpr uint64_t Entities = 513;
	for (uint64_t index = 0; index < Entities; index++)
	{
		UUID const instance(2000 + index);
		REQUIRE(VisualScriptEngine::CreateInstance(instance, graph, {}).IsOk());
		VisualScriptEngine::InvokeOnUpdate(instance, 1.0f / 60.0f);
	}
	Ref<VisualScriptLiveLog const> const full = VisualScriptEngine::GetLiveLog();
	CHECK(full->Problems.size() == 512);
	CHECK(full->Dropped == 1);
	VisualScriptEngine::InvokeOnUpdate(UUID(2000 + Entities - 1), 1.0f / 60.0f);
	CHECK(VisualScriptEngine::GetLiveLog()->Dropped == 2);
	VisualScriptEngine::ClearLiveLog();
	CHECK(VisualScriptEngine::GetLiveLog()->Dropped == 0);
}

TEST_CASE("VisualScriptEngine: the live log keeps the last 64 receipts, and a library node's problem names its document and caller")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	Ref<VisualScriptAsset> const asset = CreateRef<VisualScriptAsset>(MakeCountDocument(0));
	AssetHandle const graph = AssetManager::AddMemoryAsset(asset, "Counter");
	REQUIRE(VisualScriptEngine::EnsureLoaded(graph).IsOk());
	for (int32_t count = 1; count <= 70; count++)
	{
		CHECK(SendsDocument(*asset, graph, MakeCountDocument(count)));
	}
	Ref<VisualScriptLiveLog const> const log = VisualScriptEngine::GetLiveLog();
	REQUIRE(log->Edits.size() == VisualScriptLiveLog::MaxEdits);
	for (size_t index = 1; index < log->Edits.size(); index++)
	{
		CHECK(log->Edits[index - 1]->Serial < log->Edits[index]->Serial);
	}
	CHECK(log->Edits.back()->Serial == log->Serial);

	AssetHandle const library = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeInverseLibraryDocument()), "Inverse");
	AssetHandle const caller = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeInverseCallerDocument(library)), "Inverter");
	UUID const entity(1001);
	REQUIRE(VisualScriptEngine::CreateInstance(entity, caller, {}).IsOk());
	VisualScriptEngine::InvokeOnUpdate(entity, 1.0f / 60.0f);
	VisualScriptEngine::InvokeOnUpdate(entity, 1.0f / 60.0f);
	Ref<VisualScriptLiveLog const> const failed = VisualScriptEngine::GetLiveLog();
	REQUIRE(failed->Problems.size() == 1);
	VisualScriptRuntimeProblem const& problem = *failed->Problems[0];
	CHECK(problem.Graph == caller);
	CHECK(problem.Document == library);
	CHECK(problem.Function == 1);
	CHECK(problem.Node == 11);
	CHECK(problem.Count == 2);
	CHECK(problem.Message == "integer division by zero");
	CHECK(problem.Calls == std::vector<VisualScriptCallFrame>{{caller, 0, 22}});
	CHECK(host.Exceptions == 2);
}

TEST_CASE("VisualScriptEngine: ParseVisualScriptLiveEvents reads the runtime's events and refuses damaged ones, naming the member")
{
	// spec-slice4 section 8.4's example, with a Timeline clamped and one whose time is not finite.
	constexpr std::string_view Example = R"json({"Last":57,"Dropped":2,
		"Edits":[{"Serial":55,"Graph":"8146390251172604461","Name":"Scripts/Enemy.sgraph","Outcome":"Swapped","Deferred":false,
			"Instances":3,"Pending":0,
			"Variables":{"Kept":["Health"],"Matched":[{"Name":"Speed","From":"Velocity"}],
				"Reset":[{"Name":"Ammo","Reason":"TypeChanged","From":"int","To":"float"},{"Name":"Mode","Reason":"New"}],
				"Added":["Shield"],"Removed":["Old"]},
			"States":{"Kept":4,"Reset":[{"Document":"8146390251172604461","Function":0,"Node":7,"Title":"Do Once","Reason":"Changed"}]},
			"InstanceNotes":[{"Entity":"123","WaitsKept":1,"StopsKept":2,
				"Cancelled":[{"Document":"8146390251172604461","Function":0,"Node":12,"Title":"Delay","Stopped":true,
					"Reason":"the edit removed or changed Delay (node 12)"}],
				"Timelines":[{"Node":9,"Time":1.25,"ClampedFrom":3.5},{"Node":10,"Time":null,"ClampedFrom":null}]}],
			"MoreInstances":5,"Errors":""},
			{"Serial":56,"Graph":"77","Name":"Broken","Outcome":"Kept","Deferred":false,"Instances":0,"Pending":0,
			"Variables":{"Kept":[],"Matched":[],"Reset":[],"Added":[],"Removed":[]},"States":{"Kept":0,"Reset":[]},
			"InstanceNotes":[],"MoreInstances":0,"Errors":"1 errors, first: no such node (node 3)"}],
		"Problems":[{"Serial":57,"Severity":"Warning","Graph":"8146390251172604461","Document":"99","Function":1,
			"Node":14,"Title":"Divide","Entity":"123","Message":"integer division by zero","Count":37,
			"Calls":[{"Document":"8146390251172604461","Function":0,"Node":22}]}]})json";
	uint64_t last = 0;
	uint32_t dropped = 0;
	auto read = ParseVisualScriptLiveEvents(Example, last, dropped);
	REQUIRE_MESSAGE(read.IsOk(), read.GetError());
	CHECK(last == 57);
	CHECK(dropped == 2);
	auto const& [edits, problems] = read.GetValue();
	REQUIRE(edits.size() == 2);
	VisualScriptLiveEdit const& edit = edits[0];
	AssetHandle const enemy(UUID(8146390251172604461ull));
	CHECK(edit.Serial == 55);
	CHECK(edit.Graph == enemy);
	CHECK(edit.Name == "Scripts/Enemy.sgraph");
	CHECK(edit.Outcome == VisualScriptLiveOutcome::Applied);
	CHECK_FALSE(edit.Deferred);
	CHECK(edit.Instances == 3);
	CHECK(edit.Pending == 0);
	CHECK(edit.KeptVariables == std::vector<std::string>{"Health"});
	CHECK(edit.MatchedVariables == std::vector<VisualScriptLiveVariable>{{"Speed", "Velocity"}});
	CHECK(edit.ResetVariables == std::vector<VisualScriptLiveVariable>{{"Ammo", "type changed from int to float"}, {"Mode", "new"}});
	CHECK(edit.AddedVariables == std::vector<std::string>{"Shield"});
	CHECK(edit.RemovedVariables == std::vector<std::string>{"Old"});
	CHECK(edit.KeptStates == 4);
	CHECK(edit.ResetStates == std::vector<VisualScriptLiveNode>{{enemy, 0, 7, "Do Once", "Changed", false}});
	REQUIRE(edit.InstanceNotes.size() == 1);
	VisualScriptLiveInstance const& instance = edit.InstanceNotes[0];
	CHECK(instance.Entity == UUID(123));
	CHECK(instance.WaitsKept == 1);
	CHECK(instance.StopsKept == 2);
	CHECK(instance.Cancelled ==
	      std::vector<VisualScriptLiveNode>{{enemy, 0, 12, "Delay", "the edit removed or changed Delay (node 12)", true}});
	REQUIRE(instance.Timelines.size() == 2);
	CHECK(instance.Timelines[0].Node == 9);
	CHECK(instance.Timelines[0].Time == 1.25);
	CHECK(instance.Timelines[0].ClampedFrom == std::optional<double>(3.5));
	CHECK(std::isnan(instance.Timelines[1].Time));
	CHECK_FALSE(instance.Timelines[1].ClampedFrom.has_value());
	CHECK(edit.MoreInstances == 5);
	CHECK(edit.Errors.empty());
	CHECK_FALSE(edit.Cause.IsValid());
	CHECK(edit.Time == 0.0);
	CHECK(edits[1].Outcome == VisualScriptLiveOutcome::KeptLastWorking);
	CHECK(edits[1].Errors == "1 errors, first: no such node (node 3)");
	REQUIRE(problems.size() == 1);
	VisualScriptRuntimeProblem const& problem = problems[0];
	CHECK(problem.Serial == 57);
	CHECK(problem.Severity == VisualScriptSeverity::Warning);
	CHECK(problem.Graph == enemy);
	CHECK(problem.Document == AssetHandle(UUID(99)));
	CHECK(problem.Function == 1);
	CHECK(problem.Node == 14);
	CHECK(problem.Title == "Divide");
	CHECK(problem.Entity == UUID(123));
	CHECK(problem.Message == "integer division by zero");
	CHECK(problem.Count == 37);
	CHECK(problem.Calls == std::vector<VisualScriptCallFrame>{{enemy, 0, 22}});

	// Refused, naming the member; last and dropped stay as they were.
	struct Refusal
	{
		std::string Text;
		std::string Error;
	};
	std::string const exampleText(Example);
	auto const replaced = [&exampleText](std::string_view from, std::string_view to)
	{
		std::string text = exampleText;
		size_t const at = text.find(from);
		REQUIRE(at != std::string::npos);
		text.replace(at, from.size(), to);
		return text;
	};
	std::vector<Refusal> const refusals = {
		{"[]", "the live events: not an object"},
		{R"({"Last":1,"Dropped":0,"Edits":[]})", "Problems: missing"},
		{R"({"Last":-1,"Dropped":0,"Edits":[],"Problems":[]})", "Last: not an unsigned integer"},
		{replaced(R"("Outcome":"Swapped")", R"("Outcome":"Applied")"), R"(Edits[0].Outcome: unknown value "Applied")"},
		{replaced(R"("Serial":55)", R"("Serial":58)"), "Edits[0].Serial: not a serial from 1 to Last"},
		{replaced(R"("Serial":55)", R"("Serial":56)"), "Edits[1].Serial: not after the serial before it"},
		{replaced(R"("Graph":"77")", R"("Graph":77)"), "Edits[1].Graph: not a decimal string of an unsigned 64-bit integer"},
		{replaced(R"("Reason":"New")", R"("Reason":"Renamed")"), R"(Edits[0].Variables.Reset[1].Reason: unknown value "Renamed")"},
		{replaced(R"(,"From":"int","To":"float")", ""), "Edits[0].Variables.Reset[0].From: missing"},
		{replaced(R"("Reason":"Changed")", R"("Reason":"Moved")"), R"(Edits[0].States.Reset[0].Reason: unknown value "Moved")"},
		{replaced(R"("Stopped":true,)", ""), "Edits[0].InstanceNotes[0].Cancelled[0].Stopped: missing"},
		{replaced(R"("Time":1.25)", R"("Time":"1.25")"), "Edits[0].InstanceNotes[0].Timelines[0].Time: not a number"},
		{replaced(R"("Errors":"1 errors)", R"("Errors":")" + std::string("\",\"X\":\"")), "Edits[1].Errors: empty for Kept"},
		{replaced(R"("MoreInstances":5,"Errors":"")", R"("MoreInstances":5,"Errors":"x")"),
	     "Edits[0].Errors: not empty for Swapped or Pending"},
		{replaced(R"("Deferred":false,)", R"("Deferred":true,)"), ""},
		{replaced(R"("Severity":"Warning")", R"("Severity":"Info")"), R"(Problems[0].Severity: unknown value "Info")"},
		{replaced(R"("Count":37)", R"("Count":0)"), "Problems[0].Count: 0: a problem happened at least once"},
		{replaced(R"("Node":22})", R"("Node":4294967296})"), "Problems[0].Calls[0].Node: not an unsigned 32-bit integer"},
		{"{\"Last\":", "the live events are not JSON: "},
	};
	for (Refusal const& refusal : refusals)
	{
		CAPTURE(refusal.Text);
		last = 1;
		dropped = 1;
		Result<std::pair<std::vector<VisualScriptLiveEdit>, std::vector<VisualScriptRuntimeProblem>>> const refused =
			ParseVisualScriptLiveEvents(refusal.Text, last, dropped);
		if (refusal.Error.empty())
		{
			// A deferred swap reads whatever came before it: the Pending receipt it ends may have been taken earlier.
			CHECK(refused.IsOk());
			continue;
		}
		REQUIRE(refused.IsError());
		CHECK(refused.GetError().starts_with(refusal.Error));
		CHECK(last == 1);
		CHECK(dropped == 1);
	}

	// A swap a running step holds: Pending, with the instances it waits for.
	std::string pending = replaced(R"("Outcome":"Swapped")", R"("Outcome":"Pending")");
	constexpr std::string_view Counts = R"("Instances":3,"Pending":0)";
	size_t const counts = pending.find(Counts);
	REQUIRE(counts != std::string::npos);
	pending.replace(counts, Counts.size(), R"("Instances":1,"Pending":2)");
	auto const readPending = ParseVisualScriptLiveEvents(pending, last, dropped);
	REQUIRE_MESSAGE(readPending.IsOk(), readPending.GetError());
	VisualScriptLiveEdit const& held = readPending.GetValue().first[0];
	CHECK(held.Outcome == VisualScriptLiveOutcome::Pending);
	CHECK_FALSE(held.Deferred);
	CHECK(held.Instances == 1);
	CHECK(held.Pending == 2);
	CHECK(held.Errors.empty());
	CHECK(ParseVisualScriptLiveEvents(replaced(R"("Outcome":"Swapped","Deferred":false)", R"("Outcome":"Pending","Deferred":true)"), last,
	                                  dropped)
	          .IsError());
}

TEST_CASE("VisualScriptEngine: AppendVisualScriptLiveEvents makes the next revision: a Deferred receipt takes the Cause of the "
          "Pending one it ends, a problem taken again keeps its first time, and the input revision never changes")
{
	AssetHandle const graph(UUID(11));
	AssetHandle const other(UUID(12));
	AssetHandle const library(UUID(13));
	VisualScriptLiveLog const empty;

	// In one take: the Pending receipt a library edit caused, then the Deferred one that ends it, which names no cause.
	Ref<VisualScriptLiveLog const> const one =
		AppendVisualScriptLiveEvents(empty,
	                                 {MakeReceipt(1, graph, VisualScriptLiveOutcome::Pending, false, library),
	                                  MakeReceipt(2, graph, VisualScriptLiveOutcome::Applied, true)},
	                                 {}, 2, 0);
	REQUIRE(one->Edits.size() == 2);
	CHECK(one->Edits[0]->Cause == library);
	CHECK(one->Edits[1]->Cause == library);
	CHECK(one->Edits[1]->Deferred);
	CHECK(one->Serial == 2);
	CHECK(one->Dropped == 0);
	CHECK(empty.Edits.empty());

	// In a later take: the Pending receipt is the earlier revision's; another graph's Pending is not this graph's, and a
	// Deferred receipt that names its cause keeps it.
	Ref<VisualScriptLiveLog const> const two =
		AppendVisualScriptLiveEvents(*one, {MakeReceipt(3, other, VisualScriptLiveOutcome::Pending, false, library)}, {}, 3, 0);
	Ref<VisualScriptLiveLog const> const three =
		AppendVisualScriptLiveEvents(*two,
	                                 {MakeReceipt(4, other, VisualScriptLiveOutcome::Applied, true),
	                                  MakeReceipt(5, graph, VisualScriptLiveOutcome::Applied, true, other)},
	                                 {}, 5, 1);
	REQUIRE(three->Edits.size() == 5);
	CHECK(three->Edits[3]->Cause == library);
	CHECK(three->Edits[4]->Cause == other);
	CHECK(three->Dropped == 1);
	// The entries that did not change are the earlier revision's, which keeps its own.
	CHECK(three->Edits[0] == one->Edits[0]);
	CHECK(three->Edits[2] == two->Edits[2]);
	CHECK(two->Edits.size() == 3);
	CHECK(two->Serial == 3);
	CHECK(one->Edits.size() == 2);

	// The newest Pending receipt of the graph is the one a Deferred receipt ends: here the graph's own edit, no cause.
	Ref<VisualScriptLiveLog const> const newest = AppendVisualScriptLiveEvents(
		*three,
		{MakeReceipt(6, graph, VisualScriptLiveOutcome::Pending, false, library), MakeReceipt(7, graph, VisualScriptLiveOutcome::Pending),
	     MakeReceipt(8, graph, VisualScriptLiveOutcome::Applied, true)},
		{}, 8, 1);
	CHECK_FALSE(newest->Edits.back()->Cause.IsValid());
	// Nor does a Deferred receipt of a graph with no Pending one take any.
	Ref<VisualScriptLiveLog const> const alone =
		AppendVisualScriptLiveEvents(empty,
	                                 {MakeReceipt(1, other, VisualScriptLiveOutcome::Pending, false, library),
	                                  MakeReceipt(2, graph, VisualScriptLiveOutcome::Applied, true)},
	                                 {}, 2, 0);
	CHECK_FALSE(alone->Edits.back()->Cause.IsValid());

	// The last 64 receipts: a Pending receipt dropped from them is no longer found.
	std::vector<VisualScriptLiveEdit> many;
	many.push_back(MakeReceipt(1, graph, VisualScriptLiveOutcome::Pending, false, library));
	for (uint64_t serial = 2; serial <= 70; serial++)
	{
		many.push_back(MakeReceipt(serial, other, VisualScriptLiveOutcome::Applied));
	}
	Ref<VisualScriptLiveLog const> const capped = AppendVisualScriptLiveEvents(empty, std::move(many), {}, 70, 0);
	REQUIRE(capped->Edits.size() == VisualScriptLiveLog::MaxEdits);
	CHECK(capped->Edits.front()->Serial == 7);
	CHECK(capped->Edits.back()->Serial == 70);
	Ref<VisualScriptLiveLog const> const late =
		AppendVisualScriptLiveEvents(*capped, {MakeReceipt(71, graph, VisualScriptLiveOutcome::Applied, true)}, {}, 71, 0);
	REQUIRE(late->Edits.size() == VisualScriptLiveLog::MaxEdits);
	CHECK(late->Edits.front()->Serial == 8);
	CHECK_FALSE(late->Edits.back()->Cause.IsValid());
	CHECK(capped->Edits.front()->Serial == 7);

	// Problems: one per graph, document, node, entity and severity. Taken again, it moves to the end with the first time of
	// the entry it replaces; should one take hold it twice, the later stands.
	Ref<VisualScriptLiveLog const> const failed = AppendVisualScriptLiveEvents(
		empty, {}, {MakeProblem(1, graph, 12, UUID(1001), 1, 10.0), MakeProblem(2, graph, 12, UUID(1002), 1, 11.0)}, 2, 0);
	REQUIRE(failed->Problems.size() == 2);
	VisualScriptRuntimeProblem warning = MakeProblem(5, graph, 12, UUID(1001), 1, 13.0);
	warning.Severity = VisualScriptSeverity::Warning;
	Ref<VisualScriptLiveLog const> const again = AppendVisualScriptLiveEvents(
		*failed, {}, {MakeProblem(3, graph, 12, UUID(1001), 3, 12.0), MakeProblem(4, graph, 12, UUID(1001), 4, 12.5), std::move(warning)},
		5, 0);
	REQUIRE(again->Problems.size() == 3);
	CHECK(again->Problems[0] == failed->Problems[1]);
	VisualScriptRuntimeProblem const& moved = *again->Problems[1];
	CHECK(moved.Serial == 4);
	CHECK(moved.Count == 4);
	CHECK(moved.FirstTime == 10.0);
	CHECK(moved.LastTime == 12.5);
	CHECK(again->Problems[2]->Severity == VisualScriptSeverity::Warning);
	CHECK(again->Problems[2]->FirstTime == 13.0);
	CHECK(again->Serial == 5);
	// The revision it was made from is as it was.
	REQUIRE(failed->Problems.size() == 2);
	CHECK(failed->Problems[0]->Serial == 1);
	CHECK(failed->Problems[0]->Count == 1);
	CHECK(failed->Problems[0]->FirstTime == 10.0);
	CHECK(failed->Serial == 2);
}

TEST_CASE("VisualScriptEngine: a live-event take that fails is logged once and keeps the revision, and the next take that "
          "succeeds reads after the last serial taken")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeUpdateDivisionDocument()), "Division");
	UUID const entity(1001);
	REQUIRE(VisualScriptEngine::CreateInstance(entity, graph, {}).IsOk());
	VisualScriptEngine::InvokeOnUpdate(entity, 1.0f / 60.0f);
	Ref<VisualScriptLiveLog const> const held = VisualScriptEngine::GetLiveLog();
	REQUIRE(held->Problems.size() == 1);
	uint64_t const serial = held->Serial;
	constexpr std::string_view Failure = "The live events of the visual scripts cannot be taken";

	LiveEventsStub stub;
	uint64_t const logStart = Log::GetNextEntryIndex();
	VisualScriptEngine::InvokeOnUpdate(entity, 1.0f / 60.0f);
	// The runtime fails, then answers a text that is not JSON: the same revision each time, the failure logged once.
	CHECK(VisualScriptEngine::GetLiveLog() == held);
	CHECK(VisualScriptEngine::GetLiveLog() == held);
	stub.SetAnswer(LiveEventsStub::Answer::Damaged);
	VisualScriptEngine::InvokeOnUpdate(entity, 1.0f / 60.0f);
	CHECK(VisualScriptEngine::GetLiveLog() == held);
	CHECK(CountLogged(logStart, Failure) == 1);
	CHECK(CountLogged(logStart, "the scripting runtime failed (see the log)") == 1);
	CHECK(stub.TakeAfters() == std::vector<uint64_t>{serial, serial, serial});
	CHECK(held->Serial == serial);
	CHECK(held->Problems[0]->Count == 1);

	// Taken again: the events after the last serial taken, both missed occurrences counted.
	stub.SetAnswer(LiveEventsStub::Answer::Pass);
	Ref<VisualScriptLiveLog const> const recovered = VisualScriptEngine::GetLiveLog();
	CHECK(stub.TakeAfters() == std::vector<uint64_t>{serial});
	REQUIRE(recovered != held);
	REQUIRE(recovered->Problems.size() == 1);
	CHECK(recovered->Problems[0]->Count == 3);
	CHECK(recovered->Problems[0]->Serial > serial);
	CHECK(recovered->Problems[0]->FirstTime == held->Problems[0]->FirstTime);
	CHECK(CountLogged(logStart, Failure) == 1);

	// A failure after a take that succeeded is logged again.
	stub.SetAnswer(LiveEventsStub::Answer::Failure);
	CHECK(VisualScriptEngine::GetLiveLog() == recovered);
	CHECK(CountLogged(logStart, Failure) == 2);
	CHECK(stub.TakeAfters() == std::vector<uint64_t>{recovered->Serial});
	CHECK(host.Exceptions == 3);
}

TEST_CASE("VisualScriptEngine: damaged live-event texts are refused or read with their form, never crash")
{
	// The texts the runtime's writer produces: receipts of every outcome (a cancelled wait and reset variables among them) and
	// problems of a graph's node and of a library's, with their caller.
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	RecordingHost host;
	std::vector<std::string> texts;
	Ref<VisualScriptAsset> const delayAsset = CreateRef<VisualScriptAsset>(MakeDelayDocument(true));
	AssetHandle const delay = AssetManager::AddMemoryAsset(delayAsset, "Waiter");
	REQUIRE(VisualScriptEngine::CreateInstance(UUID(1001), delay, {}).IsOk());
	VisualScriptEngine::InvokeOnCreate(UUID(1001));
	texts.push_back(WriteLiveEvents());
	VisualScriptDocument changed = MakeDelayDocument(false);
	changed.Variables[0] = MakeVariable(1, "Done", "int", 0, true);
	changed.Graph.Nodes[1].Inputs["Value"] = 1;
	changed.Variables.push_back(MakeVariable(2, "Extra", "float", 1.0, false));
	CHECK(SendsDocument(*delayAsset, delay, changed));
	Ref<VisualScriptLiveEdit const> const cancelled = FindLastReceipt(delay);
	REQUIRE(cancelled->InstanceNotes.size() == 1);
	REQUIRE(cancelled->InstanceNotes[0].Cancelled.size() == 1);
	CHECK(cancelled->InstanceNotes[0].Entity == UUID(1001));
	CHECK(cancelled->InstanceNotes[0].Cancelled[0].Node == 11);
	CHECK(cancelled->InstanceNotes[0].Cancelled[0].Document == delay);
	CHECK_FALSE(cancelled->InstanceNotes[0].Cancelled[0].Stopped);
	CHECK_FALSE(cancelled->InstanceNotes[0].Cancelled[0].Reason.empty());
	CHECK(SendsDocument(*delayAsset, delay, MakeBrokenDocument()));
	texts.push_back(WriteLiveEvents());

	AssetHandle const division = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeUpdateDivisionDocument()), "Division");
	AssetHandle const library = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeInverseLibraryDocument()), "Inverse");
	AssetHandle const caller = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(MakeInverseCallerDocument(library)), "Inverter");
	REQUIRE(VisualScriptEngine::CreateInstance(UUID(1002), division, {}).IsOk());
	REQUIRE(VisualScriptEngine::CreateInstance(UUID(1003), caller, {}).IsOk());
	for (int update = 0; update < 3; update++)
	{
		VisualScriptEngine::InvokeOnUpdate(UUID(1002), 1.0f / 60.0f);
		VisualScriptEngine::InvokeOnUpdate(UUID(1003), 1.0f / 60.0f);
	}
	texts.push_back(WriteLiveEvents());

	// Each reads as the engine's log took it.
	for (std::string const& text : texts)
	{
		CAPTURE(text);
		uint64_t last = 0;
		uint32_t dropped = 0;
		Result<std::pair<std::vector<VisualScriptLiveEdit>, std::vector<VisualScriptRuntimeProblem>>> const read =
			ParseVisualScriptLiveEvents(text, last, dropped);
		REQUIRE_MESSAGE(read.IsOk(), read.GetError());
		CheckLiveEventsForm(read.GetValue().first, read.GetValue().second, last);
	}
	Ref<VisualScriptLiveLog const> const log = VisualScriptEngine::GetLiveLog();
	// The receipts of the Waiter's three sends and of the two other graphs' loads; a problem per failing node.
	CHECK(log->Problems.size() == 2);
	CHECK(log->Edits.size() == 5);

	Testing::DocumentMutator mutator(11500);
	Testing::FuzzRandom pick(11500);
	int const rounds = Testing::GetFuzzRounds(200);
	int readCount = 0;
	for (int round = 0; round < rounds; round++)
	{
		CAPTURE(round);
		Result<Json> const parsed = ParseJson(texts[pick.Pick(texts.size())]);
		REQUIRE(parsed.IsOk());
		std::string const damaged = mutator.Damage(parsed.GetValue(), round).dump();
		CAPTURE(damaged);
		uint64_t last = 7;
		uint32_t dropped = 7;
		Result<std::pair<std::vector<VisualScriptLiveEdit>, std::vector<VisualScriptRuntimeProblem>>> const read =
			ParseVisualScriptLiveEvents(damaged, last, dropped);
		if (read.IsError())
		{
			// A refusal names the member, or the events as a whole, and leaves what was read before.
			CHECK(read.GetError().find(": ") != std::string::npos);
			CHECK(last == 7);
			CHECK(dropped == 7);
			continue;
		}
		readCount++;
		CheckLiveEventsForm(read.GetValue().first, read.GetValue().second, last);
	}
	CHECK(readCount > 0);
	CHECK(host.Exceptions == 6);
}
