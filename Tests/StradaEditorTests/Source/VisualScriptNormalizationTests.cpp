#include "Editor/VisualScripting/VisualScriptNormalization.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
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
		node.Position = glm::vec2(static_cast<float>(id) * 16.0f, 0.0f);
		node.Settings = std::move(settings);
		node.Inputs = std::move(inputs);
		return node;
	}

	VisualScriptLink MakeLink(uint32_t from, std::string fromPin, uint32_t to, std::string toPin)
	{
		return {{from, std::move(fromPin)}, {to, std::move(toPin)}};
	}

	// A node the analysis resolved through a redirect, with its renamed pins.
	VisualScriptAnalyzedNode MakeRedirect(uint32_t id, std::string type, std::string canonicalType,
	                                      std::vector<VisualScriptPinRename> renames)
	{
		VisualScriptAnalyzedNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Resolved = VisualScriptResolution::Redirect;
		node.CanonicalType = std::move(canonicalType);
		node.PinRenames = std::move(renames);
		return node;
	}

	// What graph.set requires: the document loads strictly, with no issue, as it is.
	void CheckLoadsStrictly(VisualScriptDocument const& document)
	{
		DeserializationContext context;
		context.UnknownFields = UnknownFieldPolicy::Error;
		std::vector<VisualScriptIssue> issues;
		Result<VisualScriptDocument> read =
			VisualScriptSerializer::DeserializeDocument(VisualScriptSerializer::SerializeDocument(document), context, &issues);
		REQUIRE_MESSAGE(read.IsOk(), read.GetError());
		CHECK(issues.empty());
		CHECK(read.GetValue() == document);
	}
}

TEST_CASE("VisualScriptNormalization: renamed variables keep their stored names as former names when the document stays valid")
{
	VisualScriptDocument stored;
	stored.Variables.push_back(MakeVariable(1, "Speed", "float"));
	stored.Variables.push_back(MakeVariable(2, "Score", "int"));
	stored.Variables.push_back(MakeVariable(3, "Health", "int"));
	stored.Variables.push_back(MakeVariable(4, "Old", "int"));

	VisualScriptDocument document = stored;
	document.Variables[0].Name = "Velocity";
	// Swapped: both names stay current names.
	document.Variables[1].Name = "Health";
	document.Variables[2].Name = "Score";
	// Variable 5's former name is variable 4's stored name: no other variable may take it.
	document.Variables[3].Name = "New";
	document.Variables.push_back(MakeVariable(5, "Other", "int"));
	document.Variables[4].FormerNames = {"Old"};
	// Not in the stored document: nothing to keep.
	document.Variables.push_back(MakeVariable(6, "Fresh", "int"));

	std::vector<std::string> const warnings = RecordFormerNames(document, stored);
	CHECK(warnings == std::vector<std::string>{"variable renamed: Speed → Velocity (Speed is kept in FormerNames)"});
	CHECK(document.Variables[0].FormerNames == std::vector<std::string>{"Speed"});
	CHECK(document.Variables[1].FormerNames.empty());
	CHECK(document.Variables[2].FormerNames.empty());
	CHECK(document.Variables[3].FormerNames.empty());
	CHECK(document.Variables[4].FormerNames == std::vector<std::string>{"Old"});

	// Up to 16, the oldest dropped.
	VisualScriptDocument full = stored;
	full.Variables[0].Name = "Latest";
	for (int name = 0; name < 16; name++)
	{
		full.Variables[0].FormerNames.push_back("Former" + std::to_string(name));
	}
	CHECK(RecordFormerNames(full, stored).size() == 1);
	REQUIRE(full.Variables[0].FormerNames.size() == VisualScriptSerializer::MaxFormerNames);
	CHECK(full.Variables[0].FormerNames.front() == "Former1");
	CHECK(full.Variables[0].FormerNames.back() == "Speed");

	// What it gives loads without issues.
	for (VisualScriptDocument const* result : {&document, &full})
	{
		std::vector<VisualScriptIssue> issues;
		DeserializationContext context;
		context.UnknownFields = UnknownFieldPolicy::Warn;
		REQUIRE(VisualScriptSerializer::DeserializeDocument(VisualScriptSerializer::SerializeDocument(*result), context, &issues).IsOk());
		CHECK(issues.empty());
	}
}

TEST_CASE("VisualScriptNormalization: nodes take their canonical types, their renamed pins' links and inputs following")
{
	VisualScriptDocument document;
	VisualScriptFunction function;
	function.ID = 1;
	function.Name = "Helper";
	function.Graph.Nodes.push_back(MakeNode(2, "Call:Old.Scale", Json::object(), Json::object({{"Factor", 2}})));
	document.Functions.push_back(function);
	std::vector<VisualScriptNode>& nodes = document.Graph.Nodes;
	nodes.push_back(MakeNode(10, "Event.Update"));
	// Its pins Amount and Value swap names, Result becomes Sum; Other keeps its name and its input.
	nodes.push_back(MakeNode(11, "Call:Old.Add", Json::object(), Json::object({{"Amount", 3}, {"Value", 1}, {"Other", 4}})));
	nodes.push_back(MakeNode(12, "Variable.Get"));
	nodes.push_back(MakeNode(13, "Variable.Set"));
	// Renamed to Input, which it stores already: the renamed one wins, and the links that end up the same are one.
	nodes.push_back(MakeNode(14, "Call:Old.Store", Json::object(), Json::object({{"Input", 7}, {"Data", 8}})));
	// Resolved exactly, of an unknown type, and an analysis of another type: left as they are.
	nodes.push_back(MakeNode(15, "Flow.Branch"));
	nodes.push_back(MakeNode(16, "Call:Missing.Member"));
	nodes.push_back(MakeNode(17, "Call:Changed.Member"));
	std::vector<VisualScriptLink>& links = document.Graph.Links;
	links.push_back(MakeLink(10, "Then", 11, "Execute"));
	links.push_back(MakeLink(12, "Value", 11, "Amount"));
	links.push_back(MakeLink(12, "Value", 11, "Value"));
	links.push_back(MakeLink(11, "Result", 13, "Value"));
	links.push_back(MakeLink(12, "Value", 13, "Amount"));
	links.push_back(MakeLink(12, "Value", 14, "Input"));
	links.push_back(MakeLink(12, "Value", 14, "Data"));
	links.push_back(MakeLink(11, "Then", 14, "Execute"));

	auto const analyzed = [](uint32_t id, std::string type, VisualScriptResolution resolved, std::string canonical,
	                         std::vector<VisualScriptPinRename> renames)
	{
		VisualScriptAnalyzedNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Resolved = resolved;
		node.CanonicalType = std::move(canonical);
		node.PinRenames = std::move(renames);
		return node;
	};
	VisualScriptAnalysis analysis;
	analysis.Nodes.push_back(analyzed(2, "Call:Old.Scale", VisualScriptResolution::Redirect, "Call:New.Scale(int)", {}));
	analysis.Nodes.push_back(analyzed(11, "Call:Old.Add", VisualScriptResolution::Redirect, "Call:New.Add(int,int)",
	                                  {{VisualScriptPinDirection::In, "Amount", "Value"},
	                                   {VisualScriptPinDirection::In, "Value", "Amount"},
	                                   {VisualScriptPinDirection::Out, "Result", "Sum"}}));
	analysis.Nodes.push_back(analyzed(14, "Call:Old.Store", VisualScriptResolution::Unique, "Call:Old.Store(int)",
	                                  {{VisualScriptPinDirection::In, "Data", "Input"}}));
	analysis.Nodes.push_back(analyzed(15, "Flow.Branch", VisualScriptResolution::Exact, "", {}));
	analysis.Nodes.push_back(analyzed(16, "Call:Missing.Member", VisualScriptResolution::Unknown, "", {}));
	analysis.Nodes.push_back(analyzed(17, "Call:Other.Member", VisualScriptResolution::Redirect, "Call:Other.Member(int)", {}));

	std::vector<std::string> const warnings = ApplyCanonicalNodeTypes(document, analysis);
	CHECK(warnings == std::vector<std::string>{"node updated: Call:Old.Scale → Call:New.Scale(int)",
	                                           "node updated: Call:Old.Add → Call:New.Add(int,int)",
	                                           "node updated: Call:Old.Store → Call:Old.Store(int)"});
	CHECK(document.Functions[0].Graph.Nodes[0].Type == "Call:New.Scale(int)");
	CHECK(document.Functions[0].Graph.Nodes[0].Inputs == Json::object({{"Factor", 2}}));
	CHECK(nodes[1].Type == "Call:New.Add(int,int)");
	CHECK(nodes[1].Inputs == Json::object({{"Value", 3}, {"Amount", 1}, {"Other", 4}}));
	CHECK(nodes[4].Type == "Call:Old.Store(int)");
	CHECK(nodes[4].Inputs == Json::object({{"Input", 8}}));
	CHECK(nodes[5].Type == "Flow.Branch");
	CHECK(nodes[6].Type == "Call:Missing.Member");
	CHECK(nodes[7].Type == "Call:Changed.Member");
	CHECK(links == std::vector<VisualScriptLink>{MakeLink(10, "Then", 11, "Execute"), MakeLink(12, "Value", 11, "Value"),
	                                             MakeLink(12, "Value", 11, "Amount"), MakeLink(11, "Sum", 13, "Value"),
	                                             MakeLink(12, "Value", 13, "Amount"), MakeLink(12, "Value", 14, "Input"),
	                                             MakeLink(11, "Then", 14, "Execute")});
}

TEST_CASE("VisualScriptNormalization: split pins renamed by a redirect keep their field links, field literals and Split entries")
{
	VisualScriptDocument document;
	std::vector<VisualScriptNode>& nodes = document.Graph.Nodes;
	nodes.push_back(MakeNode(10, "Event.Update"));
	// Force (split, with a nested split field) becomes Impulse and the output Hit (split, with a nested split field) becomes
	// Contact. Forces is another pin whose name merely starts with Force, and the output Force another pin than the input
	// Force: they keep their names, as their fields do.
	VisualScriptNode push = MakeNode(11, "Call:Old.Push", Json::object(),
	                                 Json::object({{"Force.X", 1.5}, {"Forces.X", 2.5}, {"Force.Offset.Y", 0.5}, {"Other", 3}}));
	push.Split.Inputs = {"Force", "Force.Offset", "Forces"};
	push.Split.Outputs = {"Hit", "Hit.Point", "Force"};
	nodes.push_back(std::move(push));
	nodes.push_back(MakeNode(12, "Variable.Get", Json::object({{"Variable", "Speed"}})));
	nodes.push_back(MakeNode(13, "Variable.Set", Json::object({{"Variable", "Height"}})));
	nodes.push_back(MakeNode(14, "Variable.Set", Json::object({{"Variable", "Strength"}})));
	std::vector<VisualScriptLink>& links = document.Graph.Links;
	links.push_back(MakeLink(10, "Then", 11, "Execute"));
	links.push_back(MakeLink(12, "Value", 11, "Force.Z"));
	links.push_back(MakeLink(12, "Value", 11, "Force.Offset.X"));
	links.push_back(MakeLink(12, "Value", 11, "Forces.Y"));
	links.push_back(MakeLink(11, "Then", 13, "Execute"));
	links.push_back(MakeLink(11, "Hit.Point.Y", 13, "Value"));
	links.push_back(MakeLink(13, "Then", 14, "Execute"));
	links.push_back(MakeLink(11, "Force.X", 14, "Value"));

	VisualScriptAnalysis analysis;
	analysis.Nodes.push_back(
		MakeRedirect(11, "Call:Old.Push", "Call:New.Push(Strada.Vector3)",
	                 {{VisualScriptPinDirection::In, "Force", "Impulse"}, {VisualScriptPinDirection::Out, "Hit", "Contact"}}));
	CHECK(ApplyCanonicalNodeTypes(document, analysis) ==
	      std::vector<std::string>{"node updated: Call:Old.Push → Call:New.Push(Strada.Vector3)"});

	VisualScriptNode const& updated = nodes[1];
	CHECK(updated.Type == "Call:New.Push(Strada.Vector3)");
	CHECK(updated.Split.Inputs == std::vector<std::string>{"Impulse", "Impulse.Offset", "Forces"});
	CHECK(updated.Split.Outputs == std::vector<std::string>{"Contact", "Contact.Point", "Force"});
	CHECK(updated.Inputs == Json::object({{"Impulse.X", 1.5}, {"Forces.X", 2.5}, {"Impulse.Offset.Y", 0.5}, {"Other", 3}}));
	CHECK(links == std::vector<VisualScriptLink>{MakeLink(10, "Then", 11, "Execute"), MakeLink(12, "Value", 11, "Impulse.Z"),
	                                             MakeLink(12, "Value", 11, "Impulse.Offset.X"), MakeLink(12, "Value", 11, "Forces.Y"),
	                                             MakeLink(11, "Then", 13, "Execute"), MakeLink(11, "Contact.Point.Y", 13, "Value"),
	                                             MakeLink(13, "Then", 14, "Execute"), MakeLink(11, "Force.X", 14, "Value")});
	CheckLoadsStrictly(document);
}

TEST_CASE("VisualScriptNormalization: split paths follow the pin they name, all renames at once, and repeated entries go")
{
	VisualScriptDocument document;
	std::vector<VisualScriptNode>& nodes = document.Graph.Nodes;
	// Data becomes Input, which the node splits already: the renamed entry and its fields take its place, and the fields
	// split under Input stay. Left and Right swap names, their fields following.
	VisualScriptNode store = MakeNode(1, "Call:Old.Store");
	store.Split.Inputs = {"Data", "Input", "Input.X", "Data.Y", "Left", "Right", "Right.X"};
	nodes.push_back(std::move(store));
	// A pin named "A.B" is that pin, not field B of the pin A: its own rename applies, and the paths of its fields follow it
	// rather than A.
	VisualScriptNode dotted = MakeNode(2, "Call:Old.Dotted", Json::object(), Json::object({{"A.B", 1}, {"A.B.C", 2}, {"A.C", 3}}));
	dotted.Split.Inputs = {"A", "A.B", "A.B.C"};
	nodes.push_back(std::move(dotted));
	nodes.push_back(MakeNode(3, "Variable.Get", Json::object({{"Variable", "Speed"}})));
	std::vector<VisualScriptLink>& links = document.Graph.Links;
	links.push_back(MakeLink(3, "Value", 1, "Data.Z"));
	links.push_back(MakeLink(3, "Value", 1, "Input.Z"));
	links.push_back(MakeLink(3, "Value", 1, "Left.X"));

	VisualScriptAnalysis analysis;
	analysis.Nodes.push_back(MakeRedirect(1, "Call:Old.Store", "Call:New.Store(Game.Data)",
	                                      {{VisualScriptPinDirection::In, "Data", "Input"},
	                                       {VisualScriptPinDirection::In, "Left", "Right"},
	                                       {VisualScriptPinDirection::In, "Right", "Left"}}));
	analysis.Nodes.push_back(MakeRedirect(2, "Call:Old.Dotted", "Call:New.Dotted(Game.Pair)",
	                                      {{VisualScriptPinDirection::In, "A", "P"}, {VisualScriptPinDirection::In, "A.B", "Q"}}));
	CHECK(ApplyCanonicalNodeTypes(document, analysis).size() == 2);

	CHECK(nodes[0].Split.Inputs == std::vector<std::string>{"Input", "Input.X", "Input.Y", "Right", "Left", "Left.X"});
	CHECK(nodes[1].Split.Inputs == std::vector<std::string>{"P", "Q", "Q.C"});
	CHECK(nodes[1].Inputs == Json::object({{"Q", 1}, {"Q.C", 2}, {"P.C", 3}}));
	// Both links now end at Input.Z: the first stays.
	CHECK(links == std::vector<VisualScriptLink>{MakeLink(3, "Value", 1, "Input.Z"), MakeLink(3, "Value", 1, "Right.X")});
	CheckLoadsStrictly(document);
}
