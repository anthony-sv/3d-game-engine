#include "Editor/VisualScripting/VisualScriptFind.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
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

	// Two variables, a function with a local whose graph uses a variable, the local, itself and the custom event, a second
	// function, and an event graph with a node of each kind find reads.
	constexpr std::string_view ScriptText = R"json({
		"Variables": [ { "ID": 1, "Name": "Speed", "Type": "float" }, { "ID": 2, "Name": "Score", "Type": "int" } ],
		"Functions": [
			{
				"ID": 10, "Name": "AddScore", "Inputs": [ { "Name": "Amount", "Type": "int" } ], "Locals": [ { "ID": 11, "Name": "Sum", "Type": "int" } ],
				"Graph": {
					"Nodes": [
						{ "ID": 12, "Type": "Function.Entry" },
						{ "ID": 13, "Type": "Variable.Set", "Settings": { "Variable": 11 } },
						{ "ID": 14, "Type": "Variable.Get", "Settings": { "Variable": 1 } },
						{ "ID": 15, "Type": "Function.Call", "Settings": { "Function": 10 } },
						{ "ID": 16, "Type": "Event.Send", "Settings": { "Name": "Hit" } }
					],
					"Comments": [ { "ID": 17, "Text": "Adds the amount" } ]
				}
			},
			{ "ID": 20, "Name": "Speedy", "Graph": { "Nodes": [] } }
		],
		"Graph": {
			"Nodes": [
				{ "ID": 30, "Type": "Event.Custom", "Settings": { "Name": "Hit" } },
				{ "ID": 31, "Type": "Flow.Branch", "Comment": "check the SPEED first" },
				{ "ID": 32, "Type": "Call:Strada.Input.IsKeyDown(Strada.KeyCode)", "Inputs": { "Key": "Space" } },
				{ "ID": 33, "Type": "Call:Strada.Log.Info(string)", "Inputs": { "Message": "Speed up!" } },
				{ "ID": 34, "Type": "FormatString", "Settings": { "Format": "Speed: {Speed}" } },
				{ "ID": 35, "Type": "Variable.Get", "Settings": { "Variable": 1 } },
				{ "ID": 36, "Type": "Function.Call", "Settings": { "Function": 10 } },
				{ "ID": 37, "Type": "Event.Send", "Settings": { "Name": "Hit" } },
				{ "ID": 38, "Type": "Variable.Set", "Settings": { "Variable": 2 } },
				{ "ID": 39, "Type": "Variable.Get", "Settings": { "Variable": 1.0 } }
			],
			"Comments": [ { "ID": 40, "Text": "Speed section" } ]
		}
	})json";

	VisualScriptPin MakePin(std::string name, VisualScriptPinDirection direction, std::string type, std::string label = {})
	{
		VisualScriptPin pin;
		pin.Name = std::move(name);
		pin.Direction = direction;
		pin.Type = std::move(type);
		pin.Label = std::move(label);
		return pin;
	}

	VisualScriptAnalysis MakeAnalysis()
	{
		VisualScriptAnalysis analysis;
		VisualScriptAnalyzedNode keyDown;
		keyDown.ID = 32;
		keyDown.Type = "Call:Strada.Input.IsKeyDown(Strada.KeyCode)";
		keyDown.Title = "Is Key Down";
		keyDown.Subtitle = "Input";
		keyDown.Pins = {MakePin("Key", VisualScriptPinDirection::In, "Strada.KeyCode", "Which Key"),
		                MakePin("Result", VisualScriptPinDirection::Out, "bool")};
		analysis.Nodes.push_back(std::move(keyDown));
		VisualScriptAnalyzedNode info;
		info.ID = 33;
		info.Type = "Call:Strada.Log.Info(string)";
		info.Title = "Info";
		info.Subtitle = "Log";
		info.Pins = {MakePin("Execute", VisualScriptPinDirection::In, "exec"), MakePin("Message", VisualScriptPinDirection::In, "string"),
		             MakePin("Then", VisualScriptPinDirection::Out, "exec")};
		analysis.Nodes.push_back(std::move(info));
		VisualScriptAnalyzedNode branch;
		branch.ID = 31;
		branch.Type = "Flow.Branch";
		branch.Title = "Branch";
		branch.Pins = {MakePin("Execute", VisualScriptPinDirection::In, "exec"), MakePin("Condition", VisualScriptPinDirection::In, "bool"),
		               MakePin("True", VisualScriptPinDirection::Out, "exec"), MakePin("False", VisualScriptPinDirection::Out, "exec")};
		analysis.Nodes.push_back(std::move(branch));
		return analysis;
	}

	VisualScriptFindQuery Text(std::string text)
	{
		VisualScriptFindQuery query;
		query.Text = std::move(text);
		return query;
	}

	VisualScriptFindQuery References(VisualScriptReference::Kind kind, uint32_t id, std::string name = {})
	{
		VisualScriptFindQuery query;
		query.References = VisualScriptReference{kind, id, std::move(name)};
		return query;
	}

	VisualScriptFindResult NodeResult(uint32_t function, uint32_t node, VisualScriptFindField field, std::string text)
	{
		return {function, node, 0, 0, field, std::move(text)};
	}
}

TEST_CASE("VisualScriptFind: text matches names, titles, types, comments and values, in document order")
{
	VisualScriptDocument const document = ParseDocument(ScriptText);

	// Without an analysis: derived titles, no subtitles or pin labels. The event graph's variables, nodes and comments, then
	// each function's name, locals, nodes and comments.
	std::vector<VisualScriptFindResult> const speed = FindInVisualScript(document, nullptr, Text("speed"));
	std::vector<VisualScriptFindResult> const expected = {
		{0, 0, 0, 1, VisualScriptFindField::VariableName, "Speed"},
		NodeResult(0, 31, VisualScriptFindField::NodeComment, "check the SPEED first"),
		NodeResult(0, 33, VisualScriptFindField::Value, "\"Speed up!\""),
		NodeResult(0, 34, VisualScriptFindField::Value, "Speed: {Speed}"),
		NodeResult(0, 35, VisualScriptFindField::Title, "Get Speed"),
		{0, 0, 40, 0, VisualScriptFindField::CommentText, "Speed section"},
		NodeResult(10, 14, VisualScriptFindField::Title, "Get Speed"),
		{20, 0, 0, 0, VisualScriptFindField::FunctionName, "Speedy"},
	};
	CHECK(speed == expected);
	// ASCII letters in any case.
	CHECK(FindInVisualScript(document, nullptr, Text("SpEeD")) == expected);

	// Types, locals, function names and custom event names.
	CHECK(
		FindInVisualScript(document, nullptr, Text("iskeydown")) ==
		std::vector<VisualScriptFindResult>{NodeResult(0, 32, VisualScriptFindField::Type, "Call:Strada.Input.IsKeyDown(Strada.KeyCode)")});
	CHECK(FindInVisualScript(document, nullptr, Text("sum")) ==
	      std::vector<VisualScriptFindResult>{{10, 0, 0, 11, VisualScriptFindField::VariableName, "Sum"},
	                                          NodeResult(10, 13, VisualScriptFindField::Title, "Set Sum")});
	CHECK(FindInVisualScript(document, nullptr, Text("addscore")) ==
	      std::vector<VisualScriptFindResult>{NodeResult(0, 36, VisualScriptFindField::Title, "AddScore"),
	                                          {10, 0, 0, 0, VisualScriptFindField::FunctionName, "AddScore"},
	                                          NodeResult(10, 15, VisualScriptFindField::Title, "AddScore")});
	CHECK(FindInVisualScript(document, nullptr, Text("hit")) ==
	      std::vector<VisualScriptFindResult>{
			  NodeResult(0, 30, VisualScriptFindField::Title, "Hit"), NodeResult(0, 30, VisualScriptFindField::EventName, "Hit"),
			  NodeResult(0, 37, VisualScriptFindField::Title, "Send Hit"), NodeResult(0, 37, VisualScriptFindField::EventName, "Hit"),
			  NodeResult(10, 16, VisualScriptFindField::Title, "Send Hit"), NodeResult(10, 16, VisualScriptFindField::EventName, "Hit")});
	CHECK(FindInVisualScript(document, nullptr, Text("amount")) ==
	      std::vector<VisualScriptFindResult>{{10, 0, 17, 0, VisualScriptFindField::CommentText, "Adds the amount"}});
	CHECK(FindInVisualScript(document, nullptr, Text("zzz")).empty());
	// Subtitles and pin labels need an analysis.
	CHECK(FindInVisualScript(document, nullptr, Text("which")).empty());
}

TEST_CASE("VisualScriptFind: with an analysis, titles, subtitles and pin labels are the runtime's")
{
	VisualScriptDocument const document = ParseDocument(ScriptText);
	VisualScriptAnalysis const analysis = MakeAnalysis();

	CHECK(FindInVisualScript(document, &analysis, Text("key down")) ==
	      std::vector<VisualScriptFindResult>{NodeResult(0, 32, VisualScriptFindField::Title, "Is Key Down")});
	CHECK(
		FindInVisualScript(document, &analysis, Text("input")) ==
		std::vector<VisualScriptFindResult>{NodeResult(0, 32, VisualScriptFindField::Subtitle, "Input"),
	                                        NodeResult(0, 32, VisualScriptFindField::Type, "Call:Strada.Input.IsKeyDown(Strada.KeyCode)")});
	// A pin's label, else its name; one result per node.
	CHECK(FindInVisualScript(document, &analysis, Text("which")) ==
	      std::vector<VisualScriptFindResult>{NodeResult(0, 32, VisualScriptFindField::PinLabel, "Which Key")});
	CHECK(FindInVisualScript(document, &analysis, Text("true")) ==
	      std::vector<VisualScriptFindResult>{NodeResult(0, 31, VisualScriptFindField::PinLabel, "True")});
	// The exec pins drawn without labels (a node's only Execute and Then) are not searched; a Branch's Execute is its only
	// exec input too.
	CHECK(FindInVisualScript(document, &analysis, Text("then")).empty());
	CHECK(FindInVisualScript(document, &analysis, Text("execute")).empty());
	// Literal values are found as their JSON.
	CHECK(FindInVisualScript(document, &analysis, Text("space")) ==
	      std::vector<VisualScriptFindResult>{NodeResult(0, 32, VisualScriptFindField::Value, "\"Space\"")});
}

TEST_CASE("VisualScriptFind: references of a variable, a local, a function and a custom event, in document order")
{
	VisualScriptDocument const document = ParseDocument(ScriptText);

	// A variable's Get and Set nodes in every graph; an ID stored as a float names nothing, as the runtime reads it.
	CHECK(FindInVisualScript(document, nullptr, References(VisualScriptReference::Kind::Variable, 1)) ==
	      std::vector<VisualScriptFindResult>{NodeResult(0, 35, VisualScriptFindField::Title, "Get Speed"),
	                                          NodeResult(10, 14, VisualScriptFindField::Title, "Get Speed")});
	CHECK(FindInVisualScript(document, nullptr, References(VisualScriptReference::Kind::Variable, 2)) ==
	      std::vector<VisualScriptFindResult>{NodeResult(0, 38, VisualScriptFindField::Title, "Set Score")});
	// A local's in its function.
	CHECK(FindInVisualScript(document, nullptr, References(VisualScriptReference::Kind::Variable, 11)) ==
	      std::vector<VisualScriptFindResult>{NodeResult(10, 13, VisualScriptFindField::Title, "Set Sum")});
	// A function's calls, its own recursive call included.
	CHECK(FindInVisualScript(document, nullptr, References(VisualScriptReference::Kind::Function, 10)) ==
	      std::vector<VisualScriptFindResult>{NodeResult(0, 36, VisualScriptFindField::Title, "AddScore"),
	                                          NodeResult(10, 15, VisualScriptFindField::Title, "AddScore")});
	// A custom event's receiver and senders, by name.
	CHECK(FindInVisualScript(document, nullptr, References(VisualScriptReference::Kind::CustomEvent, 0, "Hit")) ==
	      std::vector<VisualScriptFindResult>{NodeResult(0, 30, VisualScriptFindField::Title, "Hit"),
	                                          NodeResult(0, 37, VisualScriptFindField::Title, "Send Hit"),
	                                          NodeResult(10, 16, VisualScriptFindField::Title, "Send Hit")});
	CHECK(FindInVisualScript(document, nullptr, References(VisualScriptReference::Kind::CustomEvent, 0, "hit")).empty());
	CHECK(FindInVisualScript(document, nullptr, References(VisualScriptReference::Kind::Function, 20)).empty());
}

TEST_CASE("VisualScriptFind: a query with neither or both of text and references finds nothing")
{
	VisualScriptDocument const document = ParseDocument(ScriptText);
	CHECK(FindInVisualScript(document, nullptr, VisualScriptFindQuery{}).empty());
	VisualScriptFindQuery both = Text("speed");
	both.References = VisualScriptReference{VisualScriptReference::Kind::Variable, 1, {}};
	CHECK(FindInVisualScript(document, nullptr, both).empty());
}
