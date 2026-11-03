#include "VisualScriptTestDocuments.h"

#include "Editor/VisualScripting/VisualScriptQuickFixes.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	Scope<VisualScriptLibrary> ParseLibrary()
	{
		Result<std::string> text =
			FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Editor" / "Library.json");
		REQUIRE_MESSAGE(text.IsOk(), text.GetError());
		Result<Scope<VisualScriptLibrary>> library = VisualScriptLibrary::Parse(text.GetValue());
		REQUIRE_MESSAGE(library.IsOk(), library.GetError());
		return library.TakeValue();
	}

	VisualScriptDocument ParseDocument(std::string_view text)
	{
		Result<Json> json = ParseJson(text);
		REQUIRE_MESSAGE(json.IsOk(), json.GetError());
		DeserializationContext context;
		std::vector<VisualScriptIssue> issues;
		Result<VisualScriptDocument> document = VisualScriptSerializer::DeserializeDocument(json.GetValue(), context, &issues);
		REQUIRE_MESSAGE(document.IsOk(), document.GetError());
		REQUIRE_MESSAGE(issues.empty(), (issues.empty() ? std::string() : issues.front().Path + ": " + issues.front().Message));
		return document.TakeValue();
	}

	// A script with one of each problem the quick fixes repair.
	constexpr std::string_view ProblemsText = R"json({
		"Variables": [
			{ "ID": 1, "Name": "Speed", "Type": "float", "Public": true },
			{ "ID": 2, "Name": "Spin", "Type": "Strada.Quaternion", "Value": [ 0.0, 0.0, 0.0, 0.0 ] },
			{ "ID": 3, "Name": "Grid", "Type": "Strada.Matrix4", "Public": true },
			{ "ID": 4, "Name": "Heading", "Type": "Strada.Vectr3" },
			{ "ID": 5, "Name": "Count", "Type": "int", "Value": "many" }
		],
		"Functions": [
			{
				"ID": 10, "Name": "Half", "Pure": true, "Inputs": [ { "Name": "Value", "Type": "float" } ],
				"Outputs": [ { "Name": "Result", "Type": "float" } ], "Locals": [ { "ID": 15, "Name": "Doubled", "Type": "flaot" } ],
				"Graph": {
					"Nodes": [
						{ "ID": 11, "Type": "Function.Entry", "Position": [ 0.0, 0.0 ] },
						{ "ID": 12, "Type": "Function.Return", "Position": [ 640.0, 0.0 ] },
						{ "ID": 13, "Type": "Flow.Delay", "Position": [ 320.0, 0.0 ] },
						{ "ID": 14, "Type": "Call:Strada.Log.Info(string)", "Position": [ 320.0, 160.0 ] },
						{ "ID": 16, "Type": "Function.Return", "Position": [ 640.0, 160.0 ] }
					]
				}
			},
			{
				"ID": 20, "Name": "Loose",
				"Graph": { "Nodes": [ { "ID": 21, "Type": "Event.Update", "Position": [ 0.0, 0.0 ] }, { "ID": 22, "Type": "Flow.Tween", "Position": [ 320.0, 0.0 ] } ] }
			},
			{
				"ID": 30, "Name": "Twin",
				"Graph": {
					"Nodes": [ { "ID": 31, "Type": "Function.Entry", "Position": [ 0.0, 0.0 ] }, { "ID": 32, "Type": "Function.Entry", "Position": [ 0.0, 160.0 ] } ]
				}
			},
			{
				"ID": 40, "Name": "Answer", "Pure": true, "Inputs": [ { "Name": "Amount", "Type": "flaot" } ],
				"Graph": { "Nodes": [ { "ID": 41, "Type": "Function.Entry", "Position": [ 16.0, 32.0 ] } ] }
			}
		],
		"Graph": {
			"Nodes": [
				{ "ID": 100, "Type": "Event.Update", "Position": [ 0.0, 0.0 ] },
				{ "ID": 101, "Type": "Call:Strada.Log.Info(string)", "Position": [ 320.0, 0.0 ], "Inputs": { "Mesage": "hi" } },
				{ "ID": 102, "Type": "Call:Strada.Inpt.IsKeyDown(Strada.KeyCode)", "Position": [ 0.0, 160.0 ] },
				{ "ID": 103, "Type": "Flow.Sequence", "Position": [ 640.0, 0.0 ], "Settings": { "Cout": 3 } },
				{ "ID": 104, "Type": "Variable.Get", "Position": [ 0.0, 320.0 ], "Settings": { "Variable": 99 } },
				{ "ID": 105, "Type": "Function.Call", "Position": [ 0.0, 480.0 ], "Settings": { "Function": 98 } },
				{ "ID": 106, "Type": "Event.Update", "Position": [ 0.0, 640.0 ] },
				{ "ID": 107, "Type": "Function.Entry", "Position": [ 0.0, 800.0 ] },
				{ "ID": 108, "Type": "Literal", "Position": [ 320.0, 320.0 ], "Settings": { "Type": "float", "Value": "abc" } },
				{ "ID": 109, "Type": "Set:Strada.TransformComponent.Translation", "Position": [ 320.0, 480.0 ], "Inputs": { "Value": "x" } },
				{
					"ID": 110, "Type": "Call:Strada.Entity.Instantiate(Strada.Prefab,Strada.Vector3,Strada.Quaternion,Strada.Entity)",
					"Position": [ 320.0, 640.0 ], "Inputs": { "Parent": "12", "Rotation": [ 0.0, 0.0, 0.0, 0.0 ] }
				},
				{ "ID": 112, "Type": "Literal", "Position": [ 320.0, 800.0 ], "Settings": { "Type": "Strada.Vectr3" } },
				{ "ID": 113, "Type": "Call:Strada.Entity.GetComponent<T>()", "Position": [ 640.0, 320.0 ] },
				{ "ID": 114, "Type": "Flow.Sequence", "Position": [ 640.0, 480.0 ], "Settings": { "Count": 99 } },
				{ "ID": 115, "Type": "Convert", "Position": [ 640.0, 640.0 ] },
				{ "ID": 116, "Type": "Flow.Switch", "Position": [ 640.0, 800.0 ], "Settings": { "Type": "int", "Cases": [ "a" ] } },
				{ "ID": 117, "Type": "Event.Custom", "Position": [ 960.0, 0.0 ], "Settings": { "Name": "Bad.Name" } },
				{ "ID": 118, "Type": "Get:Strada.Time.DeltaTime", "Position": [ 960.0, 160.0 ] },
				{ "ID": 119, "Type": "Array.Get", "Position": [ 1280.0, 160.0 ] },
				{ "ID": 120, "Type": "Flow.Branch", "Position": [ 960.0, 320.0 ] },
				{ "ID": 121, "Type": "Get:Strada.Time.DeltaTime", "Position": [ 960.0, 480.0 ] },
				{ "ID": 122, "Type": "Flow.Delay", "Position": [ 1280.0, 320.0 ] },
				{ "ID": 123, "Type": "Event.Create", "Position": [ 960.0, 640.0 ], "Enabled": "Disabled" },
				{
					"ID": 124, "Type": "Set:Strada.TransformComponent.Translation", "Position": [ 1280.0, 480.0 ],
					"Inputs": { "Target.ID": 1, "Value": [ 1.0, 2.0, 3.0 ] }, "Split": { "Inputs": [ "Target" ] }
				},
				{ "ID": 125, "Type": "Call:Strada.Log.Write(string)", "Position": [ 1280.0, 640.0 ], "Inputs": { "Text": "hi" } },
				{ "ID": 126, "Type": "Call:Strada.Math.Max", "Position": [ 1280.0, 800.0 ] },
				{
					"ID": 127, "Type": "Event.Custom", "Position": [ 1600.0, 0.0 ],
					"Settings": { "Name": "Ping", "Parameters": [ { "Name": "Signal", "Type": "exec" } ] }
				}
			],
			"Links": [
				{ "From": [ 100, "Then" ], "To": [ 103, "Execute" ] },
				{ "From": [ 100, "Then" ], "To": [ 120, "Execute" ] },
				{ "From": [ 118, "Value" ], "To": [ 119, "Index" ] },
				{ "From": [ 121, "Value" ], "To": [ 119, "Index" ] },
				{ "From": [ 121, "Value" ], "To": [ 120, "Condition" ] },
				{ "From": [ 118, "Value" ], "To": [ 100, "Delta" ] },
				{ "From": [ 101, "Then" ], "To": [ 122, "Exec" ] }
			]
		}
	})json";

	VisualScriptPin MakePin(std::string name, VisualScriptPinDirection direction, std::string type)
	{
		VisualScriptPin pin;
		pin.Name = std::move(name);
		pin.Direction = direction;
		pin.Type = std::move(type);
		return pin;
	}

	VisualScriptAnalyzedNode MakeAnalyzedNode(uint32_t id, uint32_t function, std::string type, std::string title,
	                                          std::vector<VisualScriptPin> pins = {})
	{
		VisualScriptAnalyzedNode node;
		node.ID = id;
		node.Function = function;
		node.Type = std::move(type);
		node.Title = std::move(title);
		node.Kind = VisualScriptNodeKind::Call;
		node.Pins = std::move(pins);
		return node;
	}

	// What the scripting runtime says of the script above: the nodes the fixes read (titles, pin types, a redirect).
	VisualScriptAnalysis MakeAnalysis()
	{
		VisualScriptAnalysis analysis;
		analysis.Nodes.push_back(MakeAnalyzedNode(
			100, 0, "Event.Update", "Update",
			{MakePin("Then", VisualScriptPinDirection::Out, "exec"), MakePin("DeltaTime", VisualScriptPinDirection::Out, "float")}));
		for (uint32_t const id : {118u, 121u})
		{
			analysis.Nodes.push_back(MakeAnalyzedNode(id, 0, "Get:Strada.Time.DeltaTime", "Get Delta Time",
			                                          {MakePin("Value", VisualScriptPinDirection::Out, "float")}));
		}
		analysis.Nodes.push_back(
			MakeAnalyzedNode(119, 0, "Array.Get", "Get",
		                     {MakePin("Array", VisualScriptPinDirection::In, "*"), MakePin("Index", VisualScriptPinDirection::In, "int"),
		                      MakePin("Element", VisualScriptPinDirection::Out, "*")}));
		analysis.Nodes.push_back(MakeAnalyzedNode(
			120, 0, "Flow.Branch", "Branch",
			{MakePin("Execute", VisualScriptPinDirection::In, "exec"), MakePin("Condition", VisualScriptPinDirection::In, "bool"),
		     MakePin("True", VisualScriptPinDirection::Out, "exec"), MakePin("False", VisualScriptPinDirection::Out, "exec")}));
		VisualScriptAnalyzedNode updated = MakeAnalyzedNode(125, 0, "Call:Strada.Log.Write(string)", "Info",
		                                                    {MakePin("Execute", VisualScriptPinDirection::In, "exec"),
		                                                     MakePin("Message", VisualScriptPinDirection::In, "string"),
		                                                     MakePin("Then", VisualScriptPinDirection::Out, "exec")});
		updated.Resolved = VisualScriptResolution::Redirect;
		updated.CanonicalType = "Call:Strada.Log.Info(string)";
		updated.PinRenames.push_back({VisualScriptPinDirection::In, "Text", "Message"});
		analysis.Nodes.push_back(std::move(updated));
		return analysis;
	}

	VisualScriptDiagnostic MakeDiagnostic(std::string code, uint32_t function, std::optional<uint32_t> node, std::string message = {},
	                                      std::vector<std::string> suggestions = {})
	{
		VisualScriptDiagnostic diagnostic;
		diagnostic.Code = std::move(code);
		diagnostic.Function = function;
		diagnostic.Node = node;
		diagnostic.Message = std::move(message);
		diagnostic.Suggestions = std::move(suggestions);
		return diagnostic;
	}

	VisualScriptDiagnostic MakePinDiagnostic(std::string code, uint32_t node, std::string pin, VisualScriptPinDirection direction,
	                                         std::vector<std::string> suggestions = {})
	{
		VisualScriptDiagnostic diagnostic = MakeDiagnostic(std::move(code), 0, node, {}, std::move(suggestions));
		diagnostic.Pin = std::move(pin);
		diagnostic.Direction = direction;
		return diagnostic;
	}

	VisualScriptDiagnostic MakeLinkDiagnostic(std::string code, VisualScriptLink link)
	{
		VisualScriptDiagnostic diagnostic = MakeDiagnostic(std::move(code), 0, std::nullopt);
		diagnostic.Link = std::move(link);
		return diagnostic;
	}

	VisualScriptDiagnostic MakeVariableDiagnostic(std::string code, uint32_t function, uint32_t variable, std::string message = {},
	                                              std::vector<std::string> suggestions = {})
	{
		VisualScriptDiagnostic diagnostic =
			MakeDiagnostic(std::move(code), function, std::nullopt, std::move(message), std::move(suggestions));
		diagnostic.Variable = variable;
		return diagnostic;
	}

	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}

	std::vector<std::string> GetLabels(std::vector<VisualScriptQuickFix> const& fixes)
	{
		std::vector<std::string> labels;
		for (VisualScriptQuickFix const& fix : fixes)
		{
			labels.push_back(fix.Label);
		}
		return labels;
	}

	struct Fixture
	{
		Scope<VisualScriptLibrary> Library = ParseLibrary();
		VisualScriptDocument Document = ParseDocument(ProblemsText);
		VisualScriptAnalysis Analysis = MakeAnalysis();

		std::vector<VisualScriptQuickFix> Fixes(VisualScriptDiagnostic const& diagnostic) const
		{
			return GetVisualScriptQuickFixes(Document, Analysis, diagnostic, *Library);
		}

		// Applies the fix with the label to a copy of the script, which must still save and load as it is.
		VisualScriptDocument Apply(VisualScriptDiagnostic const& diagnostic, std::string_view label) const
		{
			std::vector<VisualScriptQuickFix> const fixes = Fixes(diagnostic);
			auto const fix = std::find_if(fixes.begin(), fixes.end(),
			                              [label](VisualScriptQuickFix const& candidate)
			                              {
											  return candidate.Label == label;
										  });
			REQUIRE_MESSAGE(fix != fixes.end(), label);
			VisualScriptDocument fixed = Document;
			Result<void> const applied = ApplyVisualScriptQuickFix(fixed, Analysis, diagnostic, *fix);
			REQUIRE_MESSAGE(applied.IsOk(), applied.GetError());
			Testing::ExpectStrictRoundTrip(fixed);
			Testing::ExpectSplitConsistency(fixed);
			return fixed;
		}
	};

	VisualScriptNode const* FindNode(VisualScriptGraph const& graph, uint32_t id)
	{
		auto const found = std::find_if(graph.Nodes.begin(), graph.Nodes.end(),
		                                [id](VisualScriptNode const& node)
		                                {
											return node.ID == id;
										});
		return found != graph.Nodes.end() ? &*found : nullptr;
	}

	VisualScriptNode& GetNode(VisualScriptGraph& graph, uint32_t id)
	{
		auto const found = std::find_if(graph.Nodes.begin(), graph.Nodes.end(),
		                                [id](VisualScriptNode const& node)
		                                {
											return node.ID == id;
										});
		REQUIRE(found != graph.Nodes.end());
		return *found;
	}

	bool HasLink(VisualScriptGraph const& graph, VisualScriptLink const& link)
	{
		return std::find(graph.Links.begin(), graph.Links.end(), link) != graph.Links.end();
	}
}

TEST_CASE("VisualScriptQuickFix: an unknown node type changes to a suggestion, an ambiguous one to an overload")
{
	Fixture const fixture;
	VisualScriptDiagnostic const unknown =
		MakeDiagnostic("UnknownNodeType", 0, 102, "unknown node type",
	                   {"Call:Strada.Input.IsKeyDown(Strada.KeyCode)", "Call:Strada.Input.IsKeyUp(Strada.KeyCode)"});
	std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(unknown);
	// A suggestion the library has is titled by it, another as the document would title it.
	CHECK(GetLabels(fixes) == std::vector<std::string>{"Change to Is Key Down", "Change to Is Key Up"});
	CHECK(fixes[0].Kind == VisualScriptFixKind::ChangeNodeType);
	CHECK(fixes[0].Target == 102);
	CHECK_FALSE(fixes[0].Navigates);
	VisualScriptDocument const changed = fixture.Apply(unknown, "Change to Is Key Down");
	CHECK(FindNode(changed.Graph, 102)->Type == "Call:Strada.Input.IsKeyDown(Strada.KeyCode)");

	VisualScriptDiagnostic const ambiguous = MakeDiagnostic("AmbiguousNode", 0, 126, "matches 2 overloads",
	                                                        {"Call:Strada.Math.Max(float,float)", "Call:Strada.Math.Max(int,int)"});
	CHECK(GetLabels(fixture.Fixes(ambiguous)) == std::vector<std::string>{"Use Max(float, float)", "Use Max(int, int)"});
	CHECK(FindNode(fixture.Apply(ambiguous, "Use Max(int, int)").Graph, 126)->Type == "Call:Strada.Math.Max(int,int)");
	// Overloads with out parameters and generic members read so too.
	VisualScriptDiagnostic const raycast =
		MakeDiagnostic("AmbiguousNode", 0, 126, {},
	                   {"Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)",
	                    "Call:Strada.Entity.GetComponent<T>()"});
	CHECK(GetLabels(fixture.Fixes(raycast)) ==
	      std::vector<std::string>{"Use Raycast(Vector3, Vector3, float, out RaycastHit, uint)", "Use GetComponent<T>()"});
}

TEST_CASE("VisualScriptQuickFix: Update Node stores a redirected node under its canonical type with its pins renamed")
{
	Fixture const fixture;
	VisualScriptDiagnostic const updated =
		MakeDiagnostic("NodeUpdated", 0, 125, "node updated: Call:Strada.Log.Write(string) → Call:Strada.Log.Info(string)");
	std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(updated);
	REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Update Node"});
	CHECK(fixes[0].Kind == VisualScriptFixKind::UpdateNode);
	VisualScriptDocument fixed = fixture.Apply(updated, "Update Node");
	VisualScriptNode const* node = FindNode(fixed.Graph, 125);
	CHECK(node->Type == "Call:Strada.Log.Info(string)");
	CHECK(node->Inputs == Json::object({{"Message", "hi"}}));
	// Only that node: the other nodes are as they were.
	VisualScriptDocument expected = fixture.Document;
	auto const at = std::find_if(expected.Graph.Nodes.begin(), expected.Graph.Nodes.end(),
	                             [](VisualScriptNode const& candidate)
	                             {
									 return candidate.ID == 125;
								 });
	*at = *node;
	CHECK(fixed == expected);
	// Once updated, the fix has nothing left to do and changes nothing.
	VisualScriptDocument const before = fixed;
	CHECK(GetVisualScriptQuickFixes(fixed, fixture.Analysis, updated, *fixture.Library).empty());
	CHECK(ApplyVisualScriptQuickFix(fixed, fixture.Analysis, updated, fixes[0]).IsError());
	CHECK(fixed == before);
}

TEST_CASE("VisualScriptQuickFix: an unknown pin of a link uses a suggestion")
{
	Fixture const fixture;
	VisualScriptDiagnostic unknown = MakePinDiagnostic("UnknownPin", 122, "Exec", VisualScriptPinDirection::In, {"Execute"});
	unknown.Link = MakeLink(101, "Then", 122, "Exec");
	std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(unknown);
	REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Use Execute"});
	CHECK(fixes[0].Kind == VisualScriptFixKind::UsePin);
	CHECK(fixes[0].Replaces == "Exec");
	VisualScriptDocument const fixed = fixture.Apply(unknown, "Use Execute");
	CHECK(HasLink(fixed.Graph, MakeLink(101, "Then", 122, "Execute")));
	CHECK_FALSE(HasLink(fixed.Graph, MakeLink(101, "Then", 122, "Exec")));
	CHECK(fixed.Graph.Links.size() == fixture.Document.Graph.Links.size());

	// A link that is gone gives no fix, and applying one fails without changing anything.
	VisualScriptDocument gone = fixed;
	CHECK(GetVisualScriptQuickFixes(gone, fixture.Analysis, unknown, *fixture.Library).empty());
	CHECK(ApplyVisualScriptQuickFix(gone, fixture.Analysis, unknown, fixes[0]).IsError());
	CHECK(gone == fixed);
}

TEST_CASE("VisualScriptQuickFix: a value stored for no input is removed or moved to a suggested input")
{
	Fixture const fixture;
	VisualScriptDiagnostic const unused = MakePinDiagnostic("UnusedInput", 101, "Mesage", VisualScriptPinDirection::In, {"Message"});
	std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(unused);
	REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Remove Value", "Use Message"});
	CHECK(fixes[0].Kind == VisualScriptFixKind::RemoveInput);
	CHECK(fixes[1].Kind == VisualScriptFixKind::UsePin);
	CHECK(FindNode(fixture.Apply(unused, "Remove Value").Graph, 101)->Inputs == Json::object());
	CHECK(FindNode(fixture.Apply(unused, "Use Message").Graph, 101)->Inputs == Json::object({{"Message", "hi"}}));
	// A suggestion that has a value already is not offered.
	Fixture both;
	GetNode(both.Document.Graph, 101).Inputs["Message"] = "kept";
	CHECK(GetLabels(both.Fixes(unused)) == std::vector<std::string>{"Remove Value"});
}

TEST_CASE("VisualScriptQuickFix: a setting the kind does not have is removed or renamed to a suggestion")
{
	Fixture const fixture;
	VisualScriptDiagnostic const unknown =
		MakeDiagnostic("UnknownSetting", 0, 103, "Flow.Sequence has no setting \"Cout\": its settings are Count", {"Count"});
	std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(unknown);
	REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Remove Setting", "Use Count"});
	CHECK(fixes[0].Kind == VisualScriptFixKind::RemoveSetting);
	CHECK(fixes[0].Replaces == "Cout");
	CHECK(fixes[1].Kind == VisualScriptFixKind::UseSetting);
	CHECK(FindNode(fixture.Apply(unknown, "Remove Setting").Graph, 103)->Settings == Json::object());
	CHECK(FindNode(fixture.Apply(unknown, "Use Count").Graph, 103)->Settings == Json::object({{"Count", 3}}));

	// Of several unknown settings, the one the message names.
	Fixture two;
	GetNode(two.Document.Graph, 103).Settings["Other"] = true;
	VisualScriptDiagnostic const other =
		MakeDiagnostic("UnknownSetting", 0, 103, "Flow.Sequence has no setting \"Other\": its settings are Count");
	std::vector<VisualScriptQuickFix> const otherFixes = two.Fixes(other);
	REQUIRE(otherFixes.size() == 1);
	CHECK(otherFixes[0].Replaces == "Other");
}

TEST_CASE("VisualScriptQuickFix: Keep the First Link removes the later links of an exec output or a data input")
{
	Fixture const fixture;
	VisualScriptDiagnostic execLinks = MakePinDiagnostic("MultipleExecLinks", 100, "Then", VisualScriptPinDirection::Out);
	execLinks.Link = MakeLink(100, "Then", 120, "Execute");
	REQUIRE(GetLabels(fixture.Fixes(execLinks)) == std::vector<std::string>{"Keep the First Link"});
	VisualScriptDocument const execFixed = fixture.Apply(execLinks, "Keep the First Link");
	CHECK(HasLink(execFixed.Graph, MakeLink(100, "Then", 103, "Execute")));
	CHECK_FALSE(HasLink(execFixed.Graph, MakeLink(100, "Then", 120, "Execute")));
	CHECK(execFixed.Graph.Links.size() == fixture.Document.Graph.Links.size() - 1);

	VisualScriptDiagnostic dataLinks = MakePinDiagnostic("MultipleDataLinks", 119, "Index", VisualScriptPinDirection::In);
	dataLinks.Link = MakeLink(121, "Value", 119, "Index");
	VisualScriptDocument const dataFixed = fixture.Apply(dataLinks, "Keep the First Link");
	CHECK(HasLink(dataFixed.Graph, MakeLink(118, "Value", 119, "Index")));
	CHECK_FALSE(HasLink(dataFixed.Graph, MakeLink(121, "Value", 119, "Index")));
	// Nothing left to remove.
	VisualScriptDocument again = dataFixed;
	CHECK(ApplyVisualScriptQuickFix(again, fixture.Analysis, dataLinks, fixture.Fixes(dataLinks)[0]).IsError());
	CHECK(again == dataFixed);
}

TEST_CASE("VisualScriptQuickFix: a mismatched link breaks, or takes a Convert between numbers and to text")
{
	Fixture const fixture;
	VisualScriptDiagnostic const numbers = MakeLinkDiagnostic("TypeMismatch", MakeLink(118, "Value", 119, "Index"));
	std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(numbers);
	REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Break Link", "Insert Convert"});
	CHECK(fixes[1].Kind == VisualScriptFixKind::InsertConvert);
	CHECK(fixes[1].Argument == "int");

	VisualScriptDocument const broken = fixture.Apply(numbers, "Break Link");
	CHECK_FALSE(HasLink(broken.Graph, MakeLink(118, "Value", 119, "Index")));
	CHECK(broken.Graph.Links.size() == fixture.Document.Graph.Links.size() - 1);

	VisualScriptDocument const converted = fixture.Apply(numbers, "Insert Convert");
	VisualScriptNode const* convert = FindNode(converted.Graph, 128);
	REQUIRE(convert != nullptr);
	CHECK(convert->Type == "Convert");
	CHECK(convert->Settings == Json::object({{"To", "int"}}));
	CHECK(convert->Position == glm::vec2(1120.0f, 160.0f));
	CHECK_FALSE(HasLink(converted.Graph, MakeLink(118, "Value", 119, "Index")));
	CHECK(HasLink(converted.Graph, MakeLink(118, "Value", 128, "Value")));
	CHECK(HasLink(converted.Graph, MakeLink(128, "Result", 119, "Index")));

	// A float into a bool: no Convert family bridges it.
	VisualScriptDiagnostic const condition = MakeLinkDiagnostic("TypeMismatch", MakeLink(121, "Value", 120, "Condition"));
	CHECK(GetLabels(fixture.Fixes(condition)) == std::vector<std::string>{"Break Link"});
	// Into text, from anything.
	Fixture text;
	text.Analysis.Nodes[3].Pins[1].Type = "string";
	CHECK(GetLabels(text.Fixes(numbers)) == std::vector<std::string>{"Break Link", "Insert Convert"});
	CHECK(text.Fixes(numbers)[1].Argument == "string");
	// A mismatch the analysis reports on a node has no link to fix.
	CHECK(fixture.Fixes(MakeDiagnostic("TypeMismatch", 0, 119, "int and string cannot be compared")).empty());

	// An event's input takes no link.
	VisualScriptDiagnostic const eventInput = MakeLinkDiagnostic("EventInputLinked", MakeLink(118, "Value", 100, "Delta"));
	REQUIRE(GetLabels(fixture.Fixes(eventInput)) == std::vector<std::string>{"Break Link"});
	CHECK_FALSE(HasLink(fixture.Apply(eventInput, "Break Link").Graph, MakeLink(118, "Value", 100, "Delta")));
}

TEST_CASE("VisualScriptQuickFix: a value that is none of its type resets to the default, a zero quaternion to the identity")
{
	Fixture const fixture;
	VisualScriptDiagnostic const input = MakePinDiagnostic("InvalidValue", 109, "Value", VisualScriptPinDirection::In);
	REQUIRE(GetLabels(fixture.Fixes(input)) == std::vector<std::string>{"Reset to Default"});
	CHECK(FindNode(fixture.Apply(input, "Reset to Default").Graph, 109)->Inputs == Json::object());

	VisualScriptDiagnostic const literal = MakeDiagnostic("InvalidValue", 0, 108, "Value: expected a number");
	CHECK(FindNode(fixture.Apply(literal, "Reset to Default").Graph, 108)->Settings == Json::object({{"Type", "float"}}));

	VisualScriptDiagnostic const variable = MakeVariableDiagnostic("InvalidValue", 0, 5, "Count: Value: expected an integer");
	CHECK(fixture.Apply(variable, "Reset to Default").Variables[4].Value.is_null());

	VisualScriptDiagnostic const rotation = MakePinDiagnostic("ZeroQuaternion", 110, "Rotation", VisualScriptPinDirection::In);
	REQUIRE(GetLabels(fixture.Fixes(rotation)) == std::vector<std::string>{"Use Identity"});
	CHECK(FindNode(fixture.Apply(rotation, "Use Identity").Graph, 110)->Inputs["Rotation"] == Json::array({0, 0, 0, 1}));
	VisualScriptDiagnostic const spin =
		MakeVariableDiagnostic("ZeroQuaternion", 0, 2, "Spin: Value: a quaternion of length zero is the identity");
	CHECK(fixture.Apply(spin, "Use Identity").Variables[1].Value == Json::array({0, 0, 0, 1}));

	// A value the document no longer stores gives no fix.
	CHECK(fixture.Fixes(MakePinDiagnostic("InvalidValue", 109, "Other", VisualScriptPinDirection::In)).empty());
}

TEST_CASE("VisualScriptQuickFix: an entity a literal cannot hold becomes a public variable whose Get feeds the input")
{
	Fixture const fixture;
	VisualScriptDiagnostic const entity = MakePinDiagnostic("EntityValue", 110, "Parent", VisualScriptPinDirection::In);
	std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(entity);
	REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Create Public Variable"});
	CHECK(fixes[0].Kind == VisualScriptFixKind::CreatePublicVariable);
	VisualScriptDocument const fixed = fixture.Apply(entity, "Create Public Variable");
	REQUIRE(fixed.Variables.size() == fixture.Document.Variables.size() + 1);
	VisualScriptVariable const& parent = fixed.Variables.back();
	CHECK(parent.ID == 128);
	CHECK(parent.Name == "Parent");
	CHECK(parent.Type == "Strada.Entity");
	CHECK(parent.Public);
	CHECK(parent.Value.is_null());
	VisualScriptNode const* get = FindNode(fixed.Graph, 129);
	REQUIRE(get != nullptr);
	CHECK(get->Type == "Variable.Get");
	CHECK(get->Settings == Json::object({{"Variable", 128}}));
	CHECK(get->Position == glm::vec2(80.0f, 640.0f));
	CHECK(HasLink(fixed.Graph, MakeLink(129, "Value", 110, "Parent")));
	CHECK_FALSE(FindNode(fixed.Graph, 110)->Inputs.contains("Parent"));
	CHECK(FindNode(fixed.Graph, 110)->Inputs.contains("Rotation"));

	// A name that is taken gets a number.
	Fixture taken;
	taken.Document.Variables[0].Name = "Parent";
	VisualScriptDocument fixedTaken = taken.Document;
	REQUIRE(ApplyVisualScriptQuickFix(fixedTaken, taken.Analysis, entity, taken.Fixes(entity)[0]).IsOk());
	CHECK(fixedTaken.Variables.back().Name == "Parent1");
	// Only stored entities of inputs: a variable's has no input to feed.
	CHECK(fixture.Fixes(MakeVariableDiagnostic("EntityValue", 0, 1)).empty());
}

TEST_CASE("VisualScriptQuickFix: settings problems are edited in Details, and a missing type argument takes a suggestion")
{
	Fixture const fixture;
	struct Case
	{
		char const* Code;
		uint32_t Node;
		char const* Message;
		char const* Setting;
	};
	for (Case const& problem : {Case{"InvalidSetting", 114, "Count: expected an integer from 2 to 64, not 99", "Count"},
	                            Case{"MissingSetting", 115, "Convert needs the setting To: a type ID", "To"},
	                            Case{"InvalidCase", 116, "Cases: expected an array of case values of int", "Cases"},
	                            Case{"InvalidSetting", 114, "nothing the kind has", ""}})
	{
		CAPTURE(problem.Message);
		VisualScriptDiagnostic const diagnostic = MakeDiagnostic(problem.Code, 0, problem.Node, problem.Message);
		std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(diagnostic);
		REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Edit in Details"});
		CHECK(fixes[0].Kind == VisualScriptFixKind::EditSetting);
		CHECK(fixes[0].Navigates);
		CHECK(fixes[0].Argument == problem.Setting);
		CHECK(fixes[0].Target == problem.Node);
		// Navigating changes nothing.
		VisualScriptDocument unchanged = fixture.Document;
		CHECK(ApplyVisualScriptQuickFix(unchanged, fixture.Analysis, diagnostic, fixes[0]).IsOk());
		CHECK(unchanged == fixture.Document);
	}

	VisualScriptDiagnostic const missing = MakeDiagnostic("MissingTypeArgument", 0, 113, "Get Component needs TypeArguments",
	                                                      {"Strada.RigidBodyComponent", "Strada.TransformComponent"});
	std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(missing);
	REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Edit in Details", "Use Rigid Body", "Use Transform"});
	CHECK(fixes[0].Argument == "TypeArguments");
	CHECK(fixes[1].Kind == VisualScriptFixKind::UseType);
	CHECK(FindNode(fixture.Apply(missing, "Use Rigid Body").Graph, 113)->Settings ==
	      Json::object({{"TypeArguments", Json::array({"Strada.RigidBodyComponent"})}}));
}

TEST_CASE("VisualScriptQuickFix: unknown variables and functions use a suggested one, named by the document")
{
	Fixture const fixture;
	VisualScriptDiagnostic const variable = MakeDiagnostic("UnknownVariable", 0, 104, "there is no variable 99", {"1", "77", "15"});
	std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(variable);
	// 77 names nothing; 15 is a local.
	REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Use Speed", "Use Doubled"});
	CHECK(fixes[0].Kind == VisualScriptFixKind::UseElement);
	CHECK(fixes[0].Argument == "1");
	CHECK(FindNode(fixture.Apply(variable, "Use Speed").Graph, 104)->Settings == Json::object({{"Variable", 1}}));

	VisualScriptDiagnostic const function = MakeDiagnostic("UnknownFunction", 0, 105, "there is no function 98", {"10", "1"});
	REQUIRE(GetLabels(fixture.Fixes(function)) == std::vector<std::string>{"Use Half"});
	CHECK(FindNode(fixture.Apply(function, "Use Half").Graph, 105)->Settings == Json::object({{"Function", 10}}));
	// An ID that is gone fails.
	VisualScriptQuickFix stale = fixture.Fixes(function)[0];
	stale.Argument = "98";
	VisualScriptDocument unchanged = fixture.Document;
	CHECK(ApplyVisualScriptQuickFix(unchanged, fixture.Analysis, function, stale).IsError());
	CHECK(unchanged == fixture.Document);
}

TEST_CASE("VisualScriptQuickFix: an unknown or invalid type uses a suggested type where it is named")
{
	Fixture const fixture;
	VisualScriptDiagnostic const variable =
		MakeVariableDiagnostic("UnknownType", 0, 4, "Heading: unknown type \"Strada.Vectr3\"", {"Strada.Vector3"});
	std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(variable);
	REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Use Vector3"});
	CHECK(fixes[0].Replaces == "Strada.Vectr3");
	CHECK(fixture.Apply(variable, "Use Vector3").Variables[3].Type == "Strada.Vector3");

	VisualScriptDiagnostic const local =
		MakeVariableDiagnostic("UnknownType", 10, 15, "Half: local Doubled: unknown type \"flaot\"", {"float"});
	CHECK(fixture.Apply(local, "Use Float").Functions[0].Locals[0].Type == "float");

	VisualScriptDiagnostic const literal =
		MakeDiagnostic("UnknownType", 0, 112, "Type: unknown type \"Strada.Vectr3\"", {"Strada.Vector3", "Strada.Vector4"});
	REQUIRE(GetLabels(fixture.Fixes(literal)) == std::vector<std::string>{"Use Vector3", "Use Vector4"});
	CHECK(FindNode(fixture.Apply(literal, "Use Vector4").Graph, 112)->Settings == Json::object({{"Type", "Strada.Vector4"}}));

	VisualScriptDiagnostic const parameter =
		MakeDiagnostic("UnknownType", 40, std::nullopt, "Answer: the input Amount: unknown type \"flaot\"", {"float"});
	CHECK(fixture.Apply(parameter, "Use Float").Functions[3].Inputs[0].Type == "float");

	// A custom event's parameter of a pin type.
	VisualScriptDiagnostic const invalid =
		MakeDiagnostic("InvalidType", 0, 127, "Parameters[0].Type: exec is a pin type: variables and parameters hold values", {"int"});
	REQUIRE(GetLabels(fixture.Fixes(invalid)) == std::vector<std::string>{"Use Int"});
	CHECK(FindNode(fixture.Apply(invalid, "Use Int").Graph, 127)->Settings["Parameters"][0]["Type"] == "int");
	// Without suggestions there is nothing to offer.
	CHECK(fixture.Fixes(MakeDiagnostic("InvalidType", 0, 127, "exec is a pin type")).empty());
}

TEST_CASE("VisualScriptQuickFix: a public variable of a type components cannot store is made private")
{
	Fixture const fixture;
	VisualScriptDiagnostic const grid = MakeVariableDiagnostic("NotExposable", 0, 3, "Strada.Matrix4 cannot be set on components");
	REQUIRE(GetLabels(fixture.Fixes(grid)) == std::vector<std::string>{"Make Private"});
	CHECK_FALSE(fixture.Apply(grid, "Make Private").Variables[2].Public);
}

TEST_CASE("VisualScriptQuickFix: names are renamed where they are edited")
{
	Fixture const fixture;
	VisualScriptDiagnostic const name = MakeDiagnostic("InvalidName", 0, 117, "Name \"Bad.Name\": '.' is not allowed");
	std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(name);
	REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Rename"});
	CHECK(fixes[0].Kind == VisualScriptFixKind::Rename);
	CHECK(fixes[0].Navigates);
	CHECK(fixes[0].Target == 117);
	VisualScriptDiagnostic const reserved =
		MakeDiagnostic("ReservedName", 20, std::nullopt, "Loose: the input Then has a reserved pin name");
	REQUIRE(GetLabels(fixture.Fixes(reserved)) == std::vector<std::string>{"Rename"});
	CHECK(fixture.Fixes(reserved)[0].Target == 20);
	VisualScriptDocument unchanged = fixture.Document;
	CHECK(ApplyVisualScriptQuickFix(unchanged, fixture.Analysis, reserved, fixture.Fixes(reserved)[0]).IsOk());
	CHECK(unchanged == fixture.Document);
}

TEST_CASE("VisualScriptQuickFix: nodes a graph cannot hold are deleted, a duplicate event also goes to the first")
{
	Fixture const fixture;
	VisualScriptDiagnostic const duplicate = MakeDiagnostic("DuplicateEvent", 0, 106, "Update is handled by node 100 already");
	std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(duplicate);
	REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Go to the First Update", "Delete This Node"});
	CHECK(fixes[0].Kind == VisualScriptFixKind::GoTo);
	CHECK(fixes[0].Target == 100);
	CHECK(fixes[0].Navigates);
	CHECK(FindNode(fixture.Apply(duplicate, "Delete This Node").Graph, 106) == nullptr);

	VisualScriptDiagnostic const event = MakeDiagnostic("EventInFunction", 20, 21, "Update is an event");
	REQUIRE(GetLabels(fixture.Fixes(event)) == std::vector<std::string>{"Delete This Node"});
	CHECK(FindNode(fixture.Apply(event, "Delete This Node").Functions[1].Graph, 21) == nullptr);
	VisualScriptDiagnostic const tick = MakeDiagnostic("TickInFunction", 20, 22, "Tween ticks");
	CHECK(FindNode(fixture.Apply(tick, "Delete This Node").Functions[1].Graph, 22) == nullptr);
	VisualScriptDiagnostic const notInFunction = MakeDiagnostic("NotInFunction", 0, 107, "Function Entry belongs in a function");
	VisualScriptDocument const removed = fixture.Apply(notInFunction, "Delete This Node");
	CHECK(FindNode(removed.Graph, 107) == nullptr);
	CHECK(removed.Graph.Nodes.size() == fixture.Document.Graph.Nodes.size() - 1);
}

TEST_CASE("VisualScriptQuickFix: functions get the entry or return they lack, and go to the first of several")
{
	Fixture const fixture;
	VisualScriptDiagnostic const missingEntry = MakeDiagnostic("MissingEntry", 20, std::nullopt, "Loose has no Function.Entry node");
	std::vector<VisualScriptQuickFix> const entryFixes = fixture.Fixes(missingEntry);
	REQUIRE(GetLabels(entryFixes) == std::vector<std::string>{"Add Entry"});
	CHECK(entryFixes[0].Target == 20);
	VisualScriptDocument const withEntry = fixture.Apply(missingEntry, "Add Entry");
	VisualScriptNode const* entry = FindNode(withEntry.Functions[1].Graph, 128);
	REQUIRE(entry != nullptr);
	CHECK(entry->Type == "Function.Entry");
	CHECK(entry->Position == glm::vec2(-320.0f, 0.0f));

	VisualScriptDiagnostic const duplicateEntry =
		MakeDiagnostic("DuplicateEntry", 30, 32, "Twin has its Function.Entry in node 31 already");
	std::vector<VisualScriptQuickFix> const duplicateFixes = fixture.Fixes(duplicateEntry);
	REQUIRE(GetLabels(duplicateFixes) == std::vector<std::string>{"Go to the First Entry"});
	CHECK(duplicateFixes[0].Target == 31);

	VisualScriptDiagnostic const missingReturn =
		MakeDiagnostic("MissingReturn", 40, std::nullopt, "Answer is pure and has no Function.Return node");
	REQUIRE(GetLabels(fixture.Fixes(missingReturn)) == std::vector<std::string>{"Add Return"});
	VisualScriptDocument const withReturn = fixture.Apply(missingReturn, "Add Return");
	VisualScriptNode const* added = FindNode(withReturn.Functions[3].Graph, 128);
	REQUIRE(added != nullptr);
	CHECK(added->Type == "Function.Return");
	CHECK(added->Position == glm::vec2(336.0f, 32.0f));

	VisualScriptDiagnostic const duplicateReturn =
		MakeDiagnostic("DuplicateReturn", 10, 16, "Half has its Function.Return in node 12 already");
	std::vector<VisualScriptQuickFix> const returnFixes = fixture.Fixes(duplicateReturn);
	REQUIRE(GetLabels(returnFixes) == std::vector<std::string>{"Go to the First Return"});
	CHECK(returnFixes[0].Target == 12);
}

TEST_CASE("VisualScriptQuickFix: a pure function that waits or has exec pins is made impure")
{
	Fixture const fixture;
	for (VisualScriptDiagnostic const& diagnostic : {MakeDiagnostic("LatentInPureContext", 10, 13, "Delay waits"),
	                                                 MakeDiagnostic("ImpureInPureFunction", 10, 14, "Info has exec pins")})
	{
		CAPTURE(diagnostic.Code);
		std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(diagnostic);
		REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Make Half Impure"});
		CHECK(fixes[0].Kind == VisualScriptFixKind::MakeImpure);
		CHECK(fixes[0].Target == 10);
		CHECK_FALSE(fixture.Apply(diagnostic, "Make Half Impure").Functions[0].Pure);
	}
	// An impure function has nothing to change.
	CHECK(fixture.Fixes(MakeDiagnostic("ImpureInPureFunction", 20, 21)).empty());
}

TEST_CASE("VisualScriptQuickFix: an unused split is removed with its fields, and a disabled node the runtime runs is enabled")
{
	Fixture const fixture;
	VisualScriptDiagnostic const split = MakePinDiagnostic("UnusedSplit", 124, "Target", VisualScriptPinDirection::In);
	std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(split);
	REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Remove Split"});
	CHECK(fixes[0].Kind == VisualScriptFixKind::RemoveSplit);
	CHECK(fixes[0].Argument == "Target");
	VisualScriptDocument const unsplit = fixture.Apply(split, "Remove Split");
	VisualScriptNode const* node = FindNode(unsplit.Graph, 124);
	CHECK(node->Split.IsEmpty());
	CHECK(node->Inputs == Json::object({{"Value", Json::array({1.0, 2.0, 3.0})}}));
	// A path the node does not split.
	CHECK(fixture.Fixes(MakePinDiagnostic("UnusedSplit", 124, "Value", VisualScriptPinDirection::In)).empty());

	VisualScriptDiagnostic const disabled = MakeDiagnostic("DisabledIgnored", 0, 123, "Create runs although it is disabled");
	std::vector<VisualScriptQuickFix> const enable = fixture.Fixes(disabled);
	REQUIRE(GetLabels(enable) == std::vector<std::string>{"Enable Node"});
	CHECK(enable[0].Kind == VisualScriptFixKind::EnableNode);
	CHECK(FindNode(fixture.Apply(disabled, "Enable Node").Graph, 123)->Enabled == VisualScriptNodeEnabled::Enabled);
}

TEST_CASE("VisualScriptQuickFix: Remove Split removes the entry the analysis names as stored, also when a redirect renamed its pin")
{
	// Split.Outputs ["Value", "Result"] on a node a redirect renamed Value to Result: "Value" splits the pin the node now calls
	// Result, and the entry "Result" is the unused one. Mapping it through the renames would name "Value" instead.
	Fixture fixture;
	VisualScriptNode node;
	node.ID = 128;
	node.Type = "Get:Strada.TransformComponent.Translation";
	node.Position = glm::vec2(1600.0f, 160.0f);
	node.Split.Outputs = {"Value", "Result"};
	fixture.Document.Graph.Nodes.push_back(node);
	VisualScriptAnalyzedNode analyzed = MakeAnalyzedNode(128, 0, node.Type, "Get Translation",
	                                                     {MakePin("Target", VisualScriptPinDirection::In, "Strada.Entity"),
	                                                      MakePin("Result", VisualScriptPinDirection::Out, "Strada.Vector3")});
	analyzed.Resolved = VisualScriptResolution::Redirect;
	analyzed.PinRenames.push_back({VisualScriptPinDirection::Out, "Value", "Result"});
	fixture.Analysis.Nodes.push_back(std::move(analyzed));

	VisualScriptDiagnostic const unused = MakePinDiagnostic("UnusedSplit", 128, "Result", VisualScriptPinDirection::Out);
	std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(unused);
	REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Remove Split"});
	CHECK(fixes[0].Argument == "Result");
	VisualScriptDocument fixed = fixture.Document;
	REQUIRE(ApplyVisualScriptQuickFix(fixed, fixture.Analysis, unused, fixes[0]).IsOk());
	CHECK(FindNode(fixed.Graph, 128)->Split.Outputs == std::vector<std::string>{"Value"});
}

TEST_CASE("VisualScriptQuickFix: problems whose message says what to do have no fix")
{
	Fixture const fixture;
	for (char const* code :
	     {"UnresolvedWildcard", "NoOperator", "NoConversion", "PureCycle", "PureRecursion", "InvalidDocument", "Unheard"})
	{
		CAPTURE(code);
		CHECK(fixture.Fixes(MakeDiagnostic(code, 0, 119, "a message")).empty());
	}
	// Diagnostics of what the document does not hold have none either.
	CHECK(fixture.Fixes(MakeDiagnostic("UnknownNodeType", 0, 9999, {}, {"Flow.Branch"})).empty());
	CHECK(fixture.Fixes(MakeDiagnostic("MissingEntry", 9999, std::nullopt)).empty());
}

TEST_CASE("VisualScriptQuickFix: a formula's problems are edited in its field, and an unknown function uses a suggestion in place")
{
	Scope<VisualScriptLibrary> const library = ParseLibrary();
	VisualScriptDocument document;
	VisualScriptNode expression;
	expression.ID = 1;
	expression.Type = "Expression";
	expression.Position = glm::vec2(0.0f);
	expression.Settings = Json::object({{"Formula", "a * b + sni(t)"}});
	expression.Inputs = Json::object({{"a", 2}});
	VisualScriptNode time;
	time.ID = 2;
	time.Type = "Get:Strada.Time.DeltaTime";
	time.Position = glm::vec2(-320.0f, 0.0f);
	document.Graph.Nodes = {expression, time};
	document.Graph.Links = {MakeLink(2, "Value", 1, "t")};
	VisualScriptAnalysis const analysis;

	VisualScriptDiagnostic unknown =
		MakeDiagnostic("InvalidExpression", 0, 1, "unknown function sni: did you mean sin?", {"sin", "sign", "min"});
	unknown.Setting = "Formula";
	unknown.Span = std::pair<uint32_t, uint32_t>(8, 11);
	std::vector<VisualScriptQuickFix> const fixes = GetVisualScriptQuickFixes(document, analysis, unknown, *library);
	REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Edit Formula", "Use sin", "Use sign", "Use min"});
	CHECK(fixes[0].Kind == VisualScriptFixKind::EditSetting);
	CHECK(fixes[0].Navigates);
	CHECK(fixes[0].Argument == "Formula");
	CHECK(fixes[0].Target == 1);
	CHECK(fixes[1].Kind == VisualScriptFixKind::ReplaceInFormula);
	CHECK_FALSE(fixes[1].Navigates);
	CHECK(fixes[1].Argument == "sin");
	CHECK(fixes[1].Replaces == "sni");

	// Use sin replaces the span's bytes through SetFormula: the inputs, their link and their literal stay.
	VisualScriptDocument fixed = document;
	REQUIRE(ApplyVisualScriptQuickFix(fixed, analysis, unknown, fixes[1]).IsOk());
	CHECK(fixed.Graph.Nodes[0].Settings == Json::object({{"Formula", "a * b + sin(t)"}}));
	CHECK(fixed.Graph.Nodes[0].Inputs == Json::object({{"a", 2}}));
	CHECK(fixed.Graph.Links == document.Graph.Links);
	Testing::ExpectStrictRoundTrip(fixed);
	// A formula changed since no longer holds what the fix replaces: refused, the document unchanged.
	VisualScriptDocument changed = document;
	changed.Graph.Nodes[0].Settings["Formula"] = "a + b";
	VisualScriptDocument const before = changed;
	CHECK(ApplyVisualScriptQuickFix(changed, analysis, unknown, fixes[1]).IsError());
	CHECK(changed == before);

	// Other findings about the formula's text, and a reserved input name, only edit it; a span without suggestions too.
	VisualScriptDiagnostic syntax = MakeDiagnostic("InvalidExpression", 0, 1, "expected a value after '+'");
	syntax.Setting = "Formula";
	syntax.Span = std::pair<uint32_t, uint32_t>(6, 7);
	CHECK(GetLabels(GetVisualScriptQuickFixes(document, analysis, syntax, *library)) == std::vector<std::string>{"Edit Formula"});
	VisualScriptDiagnostic reserved = MakeDiagnostic("ReservedName", 0, 1, "Self is a reserved pin name: name the input otherwise");
	reserved.Setting = "Formula";
	reserved.Span = std::pair<uint32_t, uint32_t>(0, 4);
	std::vector<VisualScriptQuickFix> const reservedFixes = GetVisualScriptQuickFixes(document, analysis, reserved, *library);
	REQUIRE(GetLabels(reservedFixes) == std::vector<std::string>{"Edit Formula"});
	CHECK(reservedFixes[0].Argument == "Formula");

	// InvalidSetting and MissingSetting edit the setting the diagnostic names, before the one the message names.
	VisualScriptDiagnostic missing =
		MakeDiagnostic("MissingSetting", 0, 1, "Expression needs the setting Formula: a formula such as a * b");
	missing.Setting = "Formula";
	std::vector<VisualScriptQuickFix> const missingFixes = GetVisualScriptQuickFixes(document, analysis, missing, *library);
	REQUIRE(GetLabels(missingFixes) == std::vector<std::string>{"Edit in Details"});
	CHECK(missingFixes[0].Argument == "Formula");
	VisualScriptDiagnostic invalid = MakeDiagnostic("InvalidSetting", 0, 1, "expected a string, not 3");
	invalid.Setting = "Formula";
	CHECK(GetVisualScriptQuickFixes(document, analysis, invalid, *library)[0].Argument == "Formula");
}

TEST_CASE("VisualScriptQuickFix: an invalid type is replaced by an array type for a private variable, never for a public one")
{
	Scope<VisualScriptLibrary> const library = ParseLibrary();
	VisualScriptDocument document;
	VisualScriptVariable flow;
	flow.ID = 1;
	flow.Name = "Flow";
	flow.Type = "exec";
	VisualScriptVariable targets;
	targets.ID = 2;
	targets.Name = "Targets";
	targets.Type = "Strada.Entity[]";
	targets.Public = true;
	document.Variables = {flow, targets};
	VisualScriptAnalysis const analysis;

	VisualScriptDiagnostic const pinType =
		MakeVariableDiagnostic("InvalidType", 0, 1, "Flow: exec is a pin type: variables hold values", {"Strada.Entity[]", "int"});
	CHECK(GetLabels(GetVisualScriptQuickFixes(document, analysis, pinType, *library)) ==
	      std::vector<std::string>{"Use Entity Array", "Use Int"});
	// A public array is what is invalid there, and no array replaces it.
	VisualScriptDiagnostic const publicArray =
		MakeVariableDiagnostic("InvalidType", 0, 2, "Targets: Strada.Entity[] cannot be public", {"Strada.Entity[]", "Strada.Entity"});
	std::vector<VisualScriptQuickFix> const fixes = GetVisualScriptQuickFixes(document, analysis, publicArray, *library);
	REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Use Entity"});
	CHECK(fixes[0].Replaces == "Strada.Entity[]");
}

namespace
{
	AssetHandle const FixDoor(UUID(7200001ull));
	AssetHandle const FixMotion(UUID(7200002ull));

	VisualScriptNode MakeCrossGraphNode(uint32_t id, std::string type, Json settings)
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = glm::vec2(static_cast<float>(id) * 16.0f, 0.0f);
		node.Settings = std::move(settings);
		return node;
	}

	// Door (a script: the public Health 5, the private Secret 6, the event Hit (Damage: float), the disabled event Closed),
	// Motion (a library), and a script whose nodes name them.
	struct CrossGraphFixture
	{
		CrossGraphFixture()
		{
			VisualScriptVariable health;
			health.ID = 5;
			health.Name = "Health";
			health.Type = "float";
			health.Public = true;
			VisualScriptVariable secret;
			secret.ID = 6;
			secret.Name = "Secret";
			secret.Type = "float";
			Door.Variables = {health, secret};
			Json const damage = Json::array({{{"Name", "Damage"}, {"Type", "float"}}});
			VisualScriptNode closed = MakeCrossGraphNode(2, "Event.Custom", Json::object({{"Name", "Closed"}}));
			closed.Enabled = VisualScriptNodeEnabled::Disabled;
			Door.Graph.Nodes = {MakeCrossGraphNode(1, "Event.Custom", Json::object({{"Name", "Hit"}, {"Parameters", damage}})), closed};
			Motion = MakeVisualScriptTemplate(VisualScriptTemplate::Library);

			std::string const door = FixDoor.ToString();
			std::string const motion = FixMotion.ToString();
			Script.Variables = {health};
			Script.Graph.Nodes = {
				MakeCrossGraphNode(10, "Event.Send", Json::object({{"Name", "Hit"}, {"Graph", door}})),
				MakeCrossGraphNode(11, "Event.Send", Json::object({{"Name", "Hti"}, {"Graph", door}})),
				MakeCrossGraphNode(12, "Event.Send", Json::object({{"Name", "Hit"}, {"Graph", door}, {"Parameters", Json::array()}})),
				MakeCrossGraphNode(13, "Event.Broadcast", Json::object({{"Name", "Closed"}, {"Graph", door}})),
				MakeCrossGraphNode(14, "Variable.Get", Json::object({{"Variable", 6}, {"Graph", door}})),
				MakeCrossGraphNode(15, "Function.Call", Json::object({{"Function", 1}, {"Graph", motion}})),
				MakeCrossGraphNode(16, "Function.Call", Json::object({{"Function", 1}, {"Graph", door}})),
				MakeCrossGraphNode(17, "Function.Call", Json::object({{"Function", 1}, {"Graph", "77"}})),
				MakeCrossGraphNode(18, "Event.Custom", Json::object({{"Name", "Mine"}}))};
		}

		VisualScriptQuickFixGraphs Graphs() const
		{
			return VisualScriptQuickFixGraphs{[this](AssetHandle graph) -> VisualScriptDocument const*
			                                  {
												  return graph == FixDoor ? &Door : graph == FixMotion ? &Motion : nullptr;
											  },
			                                  [](AssetHandle graph) -> std::string
			                                  {
												  return graph == FixDoor ? "Door" : graph == FixMotion ? "Motion" : std::string();
											  }};
		}

		std::vector<VisualScriptQuickFix> Fixes(VisualScriptDiagnostic const& diagnostic) const
		{
			VisualScriptQuickFixGraphs const graphs = Graphs();
			return GetVisualScriptQuickFixes(Script, Analysis, diagnostic, *Library, &graphs);
		}

		VisualScriptDocument Apply(VisualScriptDiagnostic const& diagnostic, std::string_view label) const
		{
			std::vector<VisualScriptQuickFix> const fixes = Fixes(diagnostic);
			auto const fix = std::find_if(fixes.begin(), fixes.end(),
			                              [label](VisualScriptQuickFix const& candidate)
			                              {
											  return candidate.Label == label;
										  });
			REQUIRE_MESSAGE(fix != fixes.end(), label);
			VisualScriptQuickFixGraphs const graphs = Graphs();
			VisualScriptDocument fixed = Script;
			Result<void> const applied = ApplyVisualScriptQuickFix(fixed, Analysis, diagnostic, *fix, &graphs);
			REQUIRE_MESSAGE(applied.IsOk(), applied.GetError());
			Testing::ExpectStrictRoundTrip(fixed);
			return fixed;
		}

		Scope<VisualScriptLibrary> Library = ParseLibrary();
		VisualScriptDocument Door;
		VisualScriptDocument Motion;
		VisualScriptDocument Script;
		VisualScriptAnalysis Analysis;
	};

	VisualScriptDiagnostic MakeGraphDiagnostic(std::string code, uint32_t node, AssetHandle graph,
	                                           std::vector<std::string> suggestions = {})
	{
		VisualScriptDiagnostic diagnostic = MakeDiagnostic(std::move(code), 0, node, "about another graph", std::move(suggestions));
		diagnostic.Graph = graph;
		return diagnostic;
	}

	VisualScriptNode const& FindScriptNode(VisualScriptDocument const& document, uint32_t id)
	{
		VisualScriptNode const* const node = FindNode(document.Graph, id);
		REQUIRE(node != nullptr);
		return *node;
	}
}

TEST_CASE("VisualScriptQuickFix: the cross-graph findings offer the fixes of their codes, reading the other graph's document")
{
	CrossGraphFixture const fixture;
	std::string const door = FixDoor.ToString();
	Json const damage = Json::array({{{"Name", "Damage"}, {"Type", "float"}}});

	// A graph that is absent, or a script where a library is wanted: choose another.
	std::vector<VisualScriptQuickFix> const unknownGraph = fixture.Fixes(MakeGraphDiagnostic("UnknownGraph", 17, AssetHandle(UUID(77ull))));
	REQUIRE(GetLabels(unknownGraph) == std::vector<std::string>{"Choose a Graph"});
	CHECK(unknownGraph[0].Kind == VisualScriptFixKind::EditSetting);
	CHECK(unknownGraph[0].Argument == "Graph");
	CHECK(unknownGraph[0].Navigates);
	CHECK(GetLabels(fixture.Fixes(MakeGraphDiagnostic("NotALibrary", 16, FixDoor))) == std::vector<std::string>{"Choose a Library"});

	// An unchecked Send: choose a graph, or send by name (the Graph goes, the parameters stay).
	VisualScriptDiagnostic const unchecked = MakeGraphDiagnostic("UncheckedGraph", 12, FixDoor);
	REQUIRE(GetLabels(fixture.Fixes(unchecked)) == std::vector<std::string>{"Choose a Graph", "Send by Name"});
	CHECK(FindScriptNode(fixture.Apply(unchecked, "Send by Name"), 12).Settings ==
	      Json::object({{"Name", "Hit"}, {"Parameters", Json::array()}}));

	// An unknown event: use a suggested event of the target, with its parameters.
	VisualScriptDiagnostic const unknownEvent = MakeGraphDiagnostic("UnknownEvent", 11, FixDoor, {"Hit", "Gone"});
	REQUIRE(GetLabels(fixture.Fixes(unknownEvent)) == std::vector<std::string>{"Use Hit", "Choose a Graph"});
	CHECK(FindScriptNode(fixture.Apply(unknownEvent, "Use Hit"), 11).Settings ==
	      Json::object({{"Name", "Hit"}, {"Graph", door}, {"Parameters", damage}}));
	// Without the target's document only the navigating fix is offered.
	CHECK(GetLabels(GetVisualScriptQuickFixes(fixture.Script, fixture.Analysis, unknownEvent, *fixture.Library)) ==
	      std::vector<std::string>{"Choose a Graph"});

	// Parameters that differ from the event's: take the event's.
	VisualScriptDiagnostic const mismatch = MakeGraphDiagnostic("EventSignatureMismatch", 12, FixDoor);
	std::vector<VisualScriptQuickFix> const update = fixture.Fixes(mismatch);
	REQUIRE(GetLabels(update) == std::vector<std::string>{"Update Parameters from Door"});
	CHECK(update[0].Kind == VisualScriptFixKind::UpdateParameters);
	CHECK(FindScriptNode(fixture.Apply(mismatch, "Update Parameters from Door"), 12).Settings["Parameters"] == damage);

	// A disabled target event and a private variable open them in the other graph (they are edited there).
	std::vector<VisualScriptQuickFix> const disabled = fixture.Fixes(MakeGraphDiagnostic("DisabledTarget", 13, FixDoor));
	REQUIRE(GetLabels(disabled) == std::vector<std::string>{"Open Closed in Door"});
	CHECK(disabled[0].Kind == VisualScriptFixKind::OpenGraph);
	CHECK(disabled[0].Navigates);
	CHECK(disabled[0].Target == 2);
	CHECK(disabled[0].Argument == "Node");
	std::vector<VisualScriptQuickFix> const secret = fixture.Fixes(MakeGraphDiagnostic("PrivateVariable", 14, FixDoor));
	REQUIRE(GetLabels(secret) == std::vector<std::string>{"Open Secret in Door"});
	CHECK(secret[0].Target == 6);
	CHECK(secret[0].Argument == "Variable");
	// A broken library, and a cycle of libraries: open it.
	std::vector<VisualScriptQuickFix> const broken = fixture.Fixes(MakeGraphDiagnostic("ReferenceError", 15, FixMotion));
	REQUIRE(GetLabels(broken) == std::vector<std::string>{"Open Motion"});
	CHECK(broken[0].Argument == "Problems");
	CHECK(GetLabels(fixture.Fixes(MakeGraphDiagnostic("LibraryCycle", 15, FixMotion))) == std::vector<std::string>{"Open Motion"});
	VisualScriptDocument unchanged = fixture.Script;
	CHECK(ApplyVisualScriptQuickFix(unchanged, fixture.Analysis, MakeGraphDiagnostic("ReferenceError", 15, FixMotion), broken[0]).IsOk());
	CHECK(unchanged == fixture.Script);
	// The message says what to link.
	CHECK(fixture.Fixes(MakeGraphDiagnostic("UnlinkedTarget", 14, FixDoor)).empty());
	CHECK(fixture.Fixes(MakeGraphDiagnostic("TargetIsSelf", 10, FixDoor)).empty());

	// A library's own variables and event-graph nodes are deleted.
	VisualScriptDiagnostic variable = MakeDiagnostic("LibraryVariable", 0, std::nullopt, "a library has no variables");
	variable.Variable = 5;
	REQUIRE(GetLabels(fixture.Fixes(variable)) == std::vector<std::string>{"Delete Variable"});
	CHECK(fixture.Apply(variable, "Delete Variable").Variables.empty());
	VisualScriptDiagnostic const eventGraph = MakeDiagnostic("LibraryEventGraph", 0, 18, "a library has no event graph");
	REQUIRE(GetLabels(fixture.Fixes(eventGraph)) == std::vector<std::string>{"Delete This Node"});
	CHECK(FindNode(fixture.Apply(eventGraph, "Delete This Node").Graph, 18) == nullptr);

	// A call naming its own graph: the setting is removed.
	VisualScriptDiagnostic const self =
		MakeDiagnostic("InvalidSetting", 0, 16, "Graph: this graph's own functions are called without a Graph setting");
	REQUIRE(GetLabels(fixture.Fixes(self)) == std::vector<std::string>{"Edit in Details", "Remove Setting"});
	CHECK(FindScriptNode(fixture.Apply(self, "Remove Setting"), 16).Settings == Json::object({{"Function", 1}}));
}

TEST_CASE("VisualScriptQuickFix: a remote unknown variable's ID suggestions are named from the other graph's document")
{
	CrossGraphFixture fixture;
	VisualScriptVariable armor;
	armor.ID = 9;
	armor.Name = "Armor";
	armor.Type = "int";
	armor.Public = true;
	fixture.Door.Variables.push_back(armor);
	VisualScriptDiagnostic const unknown = MakeGraphDiagnostic("UnknownVariable", 14, FixDoor, {"5", "9"});
	std::vector<VisualScriptQuickFix> const fixes = fixture.Fixes(unknown);
	REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Use Health", "Use Armor"});
	CHECK(fixes[1].Kind == VisualScriptFixKind::UseElement);
	CHECK(fixes[1].Argument == "9");
	CHECK(FindScriptNode(fixture.Apply(unknown, "Use Armor"), 14).Settings ==
	      Json::object({{"Variable", 9}, {"Graph", FixDoor.ToString()}}));
	// The script's own variable 5 (Health too) names nothing here: without the other graph, no fix.
	CHECK(GetVisualScriptQuickFixes(fixture.Script, fixture.Analysis, unknown, *fixture.Library).empty());
}

TEST_CASE("VisualScriptQuickFix: a finding about a Timeline setting, of any code, is edited in Details or opens the Timeline drawer")
{
	// Spec-slice4 §16.4: every TimelineReader finding names the setting Timeline (InvalidSetting, InvalidName and ReservedName).
	Fixture const fixture;
	Result<Json> timeline = ParseJson(R"({"Length": 1, "Tracks": [{"Name": "Play", "Type": "float", "Keys": []}]})");
	REQUIRE(timeline.IsOk());
	VisualScriptDocument document = fixture.Document;
	VisualScriptNode node;
	node.ID = 900;
	node.Type = "Flow.Timeline";
	node.Position = glm::vec2(0.0f, 1200.0f);
	node.Settings = Json::object({{"Timeline", timeline.GetValue()}});
	document.Graph.Nodes.push_back(node);
	for (char const* const code : {"InvalidSetting", "InvalidName", "ReservedName"})
	{
		CAPTURE(code);
		VisualScriptDiagnostic diagnostic = MakeDiagnostic(code, 0, 900, "Timeline.Tracks[0].Name: Play is the name of a pin");
		diagnostic.Setting = "Timeline";
		std::vector<VisualScriptQuickFix> const fixes = GetVisualScriptQuickFixes(document, fixture.Analysis, diagnostic, *fixture.Library);
		REQUIRE(GetLabels(fixes) == std::vector<std::string>{"Edit in Details", "Open Timeline"});
		CHECK(fixes[0].Kind == VisualScriptFixKind::EditSetting);
		CHECK(fixes[0].Argument == "Timeline");
		CHECK(fixes[1].Kind == VisualScriptFixKind::OpenTimeline);
		for (VisualScriptQuickFix const& fix : fixes)
		{
			CHECK(fix.Target == 900);
			CHECK(fix.Navigates);
			// Navigating changes nothing.
			VisualScriptDocument unchanged = document;
			CHECK(ApplyVisualScriptQuickFix(unchanged, fixture.Analysis, diagnostic, fix).IsOk());
			CHECK(unchanged == document);
		}
	}
	// A Timeline finding on what is not a Flow.Timeline (an analysis behind the document) offers nothing.
	VisualScriptDiagnostic stale = MakeDiagnostic("InvalidSetting", 0, 114, "Timeline.Length: expected a number");
	stale.Setting = "Timeline";
	CHECK(GetVisualScriptQuickFixes(document, fixture.Analysis, stale, *fixture.Library).empty());
}
