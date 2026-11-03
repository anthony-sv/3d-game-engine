#include "VisualScriptTestDocuments.h"

#include "Editor/VisualScripting/VisualScriptCrossGraph.h"
#include "Editor/VisualScripting/VisualScriptNodeTitles.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;
using namespace Strada::VisualScriptEditing;

namespace
{
	AssetHandle const Motion(UUID(7000001ull));
	AssetHandle const Door(UUID(7000002ull));
	AssetHandle const Player(UUID(7000003ull));
	AssetHandle const Enemy(UUID(7000004ull));

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

	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type, bool isPublic)
	{
		VisualScriptVariable variable;
		variable.ID = id;
		variable.Name = std::move(name);
		variable.Type = std::move(type);
		variable.Public = isPublic;
		return variable;
	}

	Json MakeParameters(std::vector<std::pair<std::string, std::string>> const& parameters)
	{
		Json list = Json::array();
		for (auto const& [name, type] : parameters)
		{
			list.push_back(Json::object({{"Name", name}, {"Type", type}}));
		}
		return list;
	}

	// A library: Clamp01 (1, pure, Value: float with the former name X -> Result: float) and Report (2, Text: string).
	VisualScriptDocument MakeMotion()
	{
		VisualScriptDocument library = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
		VisualScriptFunction clamp;
		clamp.ID = 1;
		clamp.Name = "Clamp01";
		clamp.Pure = true;
		clamp.Inputs = {{"Value", "float", {"X"}}};
		clamp.Outputs = {{"Result", "float"}};
		clamp.Graph.Nodes = {MakeNode(10, "Function.Entry"), MakeNode(11, "Function.Return")};
		clamp.Graph.Links = {MakeLink(10, "Value", 11, "Result")};
		VisualScriptFunction report;
		report.ID = 2;
		report.Name = "Report";
		report.Inputs = {{"Text", "string"}};
		report.Graph.Nodes = {MakeNode(20, "Function.Entry")};
		library.Functions = {clamp, report};
		return library;
	}

	// A script: Health (5, public float), Secret (6, private int); custom events Hit (30, Damage: float) and Open (31).
	VisualScriptDocument MakeDoor()
	{
		VisualScriptDocument door;
		door.Variables = {MakeVariable(5, "Health", "float", true), MakeVariable(6, "Secret", "int", false)};
		door.Graph.Nodes = {
			MakeNode(30, "Event.Custom", Json::object({{"Name", "Hit"}, {"Parameters", MakeParameters({{"Damage", "float"}})}})),
			MakeNode(31, "Event.Custom", Json::object({{"Name", "Open"}}))};
		return door;
	}

	Json GraphSetting(AssetHandle graph)
	{
		return graph.ToString();
	}

	// A script whose nodes use Motion and Door: a call of Clamp01 (40) linked by its former name X and read by Result, a call
	// of Report (41), Get and Set Health on another door (42, 43), a Get of the private Secret (44), a typed Send of Hit (45)
	// and a Broadcast of Open (46) to doors, RunsGraph Door (47), and an untyped Send of Open (48).
	VisualScriptDocument MakeReferrer()
	{
		VisualScriptDocument document;
		document.Graph.Nodes = {
			MakeNode(39, "Event.Update"),
			MakeNode(40, "Function.Call", Json::object({{"Function", 1}, {"Graph", GraphSetting(Motion)}})),
			MakeNode(41, "Function.Call", Json::object({{"Function", 2}, {"Graph", GraphSetting(Motion)}}), Json::object({{"Text", "hi"}})),
			MakeNode(42, "Variable.Get", Json::object({{"Variable", 5}, {"Graph", GraphSetting(Door)}})),
			MakeNode(43, "Variable.Set", Json::object({{"Variable", 5}, {"Graph", GraphSetting(Door)}})),
			MakeNode(44, "Variable.Get", Json::object({{"Variable", 6}, {"Graph", GraphSetting(Door)}})),
			MakeNode(45, "Event.Send",
		             Json::object({{"Name", "Hit"}, {"Parameters", MakeParameters({{"Damage", "float"}})}, {"Graph", GraphSetting(Door)}})),
			MakeNode(46, "Event.Broadcast", Json::object({{"Name", "Open"}, {"Graph", GraphSetting(Door)}})),
			MakeNode(47, "RunsGraph", Json::object({{"Graph", GraphSetting(Door)}})),
			MakeNode(48, "Event.Send", Json::object({{"Name", "Open"}})),
		};
		document.Graph.Links = {MakeLink(42, "Value", 40, "X"), MakeLink(40, "Result", 43, "Value"), MakeLink(39, "Then", 41, "Execute")};
		return document;
	}

	std::vector<VisualScriptReferrer> FindBroken(AssetHandle graph, VisualScriptDocument const& before, VisualScriptDocument const& after,
	                                             std::vector<std::pair<AssetHandle, VisualScriptDocument const*>> const& scripts)
	{
		return FindVisualScriptBrokenReferrers(graph, before, after, scripts);
	}

	// The referrers as "<node>:<problem>".
	std::vector<std::string> Summarize(std::vector<VisualScriptReferrer> const& referrers)
	{
		std::vector<std::string> listed;
		for (VisualScriptReferrer const& referrer : referrers)
		{
			listed.push_back(std::to_string(referrer.Node) + ":" + std::string(VisualScriptReferrerProblemToString(referrer.Problem)));
		}
		return listed;
	}

	std::vector<std::string> PinNames(std::vector<VisualScriptPin> const& pins)
	{
		std::vector<std::string> names;
		for (VisualScriptPin const& pin : pins)
		{
			names.push_back(std::string(pin.Direction == VisualScriptPinDirection::In ? "in " : "out ") + pin.Name + ":" + pin.Type);
		}
		return names;
	}

	VisualScriptNode const& FindNode(VisualScriptDocument const& document, uint32_t id)
	{
		auto const found = std::find_if(document.Graph.Nodes.begin(), document.Graph.Nodes.end(),
		                                [id](VisualScriptNode const& node)
		                                {
											return node.ID == id;
										});
		REQUIRE(found != document.Graph.Nodes.end());
		return *found;
	}
}

TEST_CASE("VisualScriptEditor: a cross-graph node's pins come from the declarations of the graph it names")
{
	VisualScriptDocument const motion = MakeMotion();
	VisualScriptDocument const door = MakeDoor();
	VisualScriptDocument const referrer = MakeReferrer();

	// A pure library function: its signature; an impure one with Execute and Then.
	CHECK(PinNames(MakeVisualScriptCrossGraphPins(FindNode(referrer, 40), motion)) ==
	      std::vector<std::string>{"in Value:float", "out Result:float"});
	CHECK(PinNames(MakeVisualScriptCrossGraphPins(FindNode(referrer, 41), motion)) ==
	      std::vector<std::string>{"in Execute:exec", "in Text:string", "out Then:exec"});
	// Remote variables: the Target entity (Self while unlinked), and the variable's type; private ones too.
	std::vector<VisualScriptPin> const get = MakeVisualScriptCrossGraphPins(FindNode(referrer, 42), door);
	CHECK(PinNames(get) == std::vector<std::string>{"in Target:Strada.Entity", "out Value:float"});
	CHECK(get[0].DefaultsToSelf);
	CHECK(PinNames(MakeVisualScriptCrossGraphPins(FindNode(referrer, 43), door)) ==
	      std::vector<std::string>{"in Execute:exec", "in Target:Strada.Entity", "in Value:float", "out Then:exec", "out Value:float"});
	CHECK(PinNames(MakeVisualScriptCrossGraphPins(FindNode(referrer, 44), door)) ==
	      std::vector<std::string>{"in Target:Strada.Entity", "out Value:int"});
	// Typed senders: the event's parameters (a Send's Target, a Broadcast's Count).
	CHECK(PinNames(MakeVisualScriptCrossGraphPins(FindNode(referrer, 45), door)) ==
	      std::vector<std::string>{"in Execute:exec", "in Target:Strada.Entity", "in Damage:float", "out Then:exec"});
	CHECK(PinNames(MakeVisualScriptCrossGraphPins(FindNode(referrer, 46), door)) ==
	      std::vector<std::string>{"in Execute:exec", "out Then:exec", "out Count:int"});
	// RunsGraph's pins do not depend on the graph.
	CHECK(PinNames(MakeVisualScriptCrossGraphPins(FindNode(referrer, 47), VisualScriptDocument())) ==
	      std::vector<std::string>{"in Target:Strada.Entity", "out Result:bool"});

	// Nothing when what the node names does not resolve: a script called as a library, a library read as a script, a
	// function, variable or event it does not have, and other node kinds.
	CHECK(MakeVisualScriptCrossGraphPins(FindNode(referrer, 40), door).empty());
	CHECK(MakeVisualScriptCrossGraphPins(FindNode(referrer, 42), motion).empty());
	CHECK(MakeVisualScriptCrossGraphPins(FindNode(referrer, 45), motion).empty());
	CHECK(MakeVisualScriptCrossGraphPins(MakeNode(1, "Function.Call", Json::object({{"Function", 9}})), motion).empty());
	CHECK(MakeVisualScriptCrossGraphPins(MakeNode(1, "Variable.Get", Json::object({{"Variable", "5"}})), door).empty());
	CHECK(MakeVisualScriptCrossGraphPins(MakeNode(1, "Event.Send", Json::object({{"Name", "Close"}})), door).empty());
	CHECK(MakeVisualScriptCrossGraphPins(MakeNode(1, "Flow.Branch"), door).empty());
	// An event whose Parameters are not a list of names and types gives none either.
	VisualScriptDocument broken = door;
	broken.Graph.Nodes[0].Settings["Parameters"] = "Damage";
	CHECK(MakeVisualScriptCrossGraphPins(FindNode(referrer, 45), broken).empty());
}

TEST_CASE("VisualScriptEditor: retargeting a node keeps the links and literals of the pins it keeps and counts the others")
{
	VisualScriptDocument document = MakeReferrer();
	VisualScriptNode& call = document.Graph.Nodes[1];
	call.Inputs = Json::object({{"X", 0.5}, {"Min", 0.0}});
	call.Split.Outputs = {"Result"};
	document.Graph.Links.push_back(MakeLink(40, "Result.X", 41, "Text"));
	VisualScriptDocument const before = document;

	// Calling Report instead: Execute and Text in, Then out. X's link and literal and both Result links go.
	std::vector<VisualScriptPin> const report = MakeVisualScriptCrossGraphPins(FindNode(before, 41), MakeMotion());
	Result<uint32_t> const removed = RetargetNode(document, 40, Json::object({{"Function", 2}}), report);
	REQUIRE_MESSAGE(removed.IsOk(), removed.GetError());
	CHECK(removed.GetValue() == 3);
	VisualScriptNode const& retargeted = FindNode(document, 40);
	CHECK(retargeted.Settings == Json::object({{"Function", 2}, {"Graph", GraphSetting(Motion)}}));
	CHECK(retargeted.Inputs == Json::object());
	CHECK(retargeted.Split.Outputs.empty());
	CHECK(document.Graph.Links == std::vector<VisualScriptLink>{MakeLink(39, "Then", 41, "Execute")});

	// Pins of the same name stay, with their links and literals; a null member removes a setting.
	document = before;
	std::vector<VisualScriptPin> pins = MakeVisualScriptCrossGraphPins(FindNode(before, 40), MakeMotion());
	pins.push_back({});
	pins.back().Name = "X";
	pins.back().Direction = VisualScriptPinDirection::In;
	Result<uint32_t> const kept = RetargetNode(document, 40, Json::object({{"Graph", nullptr}}), pins);
	REQUIRE_MESSAGE(kept.IsOk(), kept.GetError());
	CHECK(kept.GetValue() == 0);
	CHECK(FindNode(document, 40).Settings == Json::object({{"Function", 1}}));
	CHECK(FindNode(document, 40).Inputs == Json::object({{"X", 0.5}}));
	CHECK(FindNode(document, 40).Split.Outputs == std::vector<std::string>{"Result"});
	CHECK(document.Graph.Links == before.Graph.Links);
	Testing::ExpectStrictRoundTrip(document);

	// Refused, changing nothing: another kind of node, a node the document does not have, settings that are no object or
	// hold what the format does not take.
	document = before;
	CHECK(RetargetNode(document, 39, Json::object({{"Graph", GraphSetting(Door)}}), pins).GetError().find("names no other graph") !=
	      std::string::npos);
	CHECK(RetargetNode(document, 99, Json::object(), pins).GetError() == "the document has no node 99");
	CHECK(RetargetNode(document, 40, Json::array(), pins).IsError());
	CHECK(RetargetNode(document, 40, Json::object({{"Function", std::nan("")}}), pins).IsError());
	CHECK(RetargetNode(document, 40, Json::object({{"", 1}}), pins).IsError());
	CHECK(document == before);
}

TEST_CASE("VisualScriptEditor: the kind of a document is set alone, and an event's parameters are read from the graph")
{
	VisualScriptDocument document = MakeDoor();
	VisualScriptDocument const before = document;
	REQUIRE(SetDocumentKind(document, VisualScriptDocumentKind::Library).IsOk());
	CHECK(document.Kind == VisualScriptDocumentKind::Library);
	document.Kind = VisualScriptDocumentKind::Script;
	CHECK(document == before);
	CHECK(SetDocumentKind(document, static_cast<VisualScriptDocumentKind>(7)).IsError());
	CHECK(document == before);

	Result<Json> const hit = FindVisualScriptEventParameters(before, "Hit");
	REQUIRE(hit.IsOk());
	CHECK(hit.GetValue() == MakeParameters({{"Damage", "float"}}));
	CHECK(FindVisualScriptEventParameters(before, "Open").GetValue() == Json::array());
	CHECK(FindVisualScriptEventParameters(before, "Close").GetError() ==
	      "the graph has no custom event Close; its custom events: Hit, Open");
	CHECK(FindVisualScriptEventParameters(VisualScriptDocument(), "Hit").GetError() ==
	      "the graph has no custom event Hit: it has no custom events");
	// The enabled event of a name is the one that receives it.
	VisualScriptDocument twice = before;
	twice.Graph.Nodes[0].Enabled = VisualScriptNodeEnabled::Disabled;
	twice.Graph.Nodes.push_back(
		MakeNode(32, "Event.Custom", Json::object({{"Name", "Hit"}, {"Parameters", MakeParameters({{"Amount", "int"}})}})));
	CHECK(FindVisualScriptEventParameters(twice, "Hit").GetValue() == MakeParameters({{"Amount", "int"}}));
}

TEST_CASE("VisualScriptEditor: two documents offer the same while kind, interface and function signatures stay")
{
	VisualScriptDocument const motion = MakeMotion();
	VisualScriptDocument const door = MakeDoor();
	CHECK(HaveSameVisualScriptOffer(motion, motion));
	CHECK(HaveSameVisualScriptOffer(door, door));

	// What other graphs do not read: layout, descriptions, a function's body, other nodes.
	VisualScriptDocument same = motion;
	same.Description = "Moves";
	same.Functions[0].Graph.Nodes.push_back(MakeNode(12, "Flow.Branch"));
	same.Functions[0].Graph.Nodes[0].Position = glm::vec2(500.0f, 0.0f);
	same.Functions[0].Name = "Clamp";
	CHECK(HaveSameVisualScriptOffer(motion, same));
	VisualScriptDocument moved = door;
	moved.Graph.Nodes[0].Position = glm::vec2(-300.0f, 40.0f);
	moved.Graph.Nodes.push_back(MakeNode(33, "Event.Update"));
	moved.Variables[0].Tooltip = "How much it takes";
	CHECK(HaveSameVisualScriptOffer(door, moved));

	// What they do read.
	auto const differs = [](VisualScriptDocument const& before, auto const& change)
	{
		VisualScriptDocument after = before;
		change(after);
		return !HaveSameVisualScriptOffer(before, after);
	};
	CHECK(differs(motion,
	              [](VisualScriptDocument& d)
	              {
					  d.Kind = VisualScriptDocumentKind::Script;
				  }));
	CHECK(differs(motion,
	              [](VisualScriptDocument& d)
	              {
					  d.Functions[0].Pure = false;
				  }));
	CHECK(differs(motion,
	              [](VisualScriptDocument& d)
	              {
					  d.Functions[0].Inputs[0].Type = "int";
				  }));
	CHECK(differs(motion,
	              [](VisualScriptDocument& d)
	              {
					  d.Functions[0].Inputs[0].FormerNames.clear();
				  }));
	CHECK(differs(motion,
	              [](VisualScriptDocument& d)
	              {
					  d.Functions[1].Outputs.push_back({"Done", "bool"});
				  }));
	CHECK(differs(motion,
	              [](VisualScriptDocument& d)
	              {
					  d.Functions.pop_back();
				  }));
	CHECK(differs(motion,
	              [](VisualScriptDocument& d)
	              {
					  d.Functions[1].ID = 3;
				  }));
	CHECK(differs(door,
	              [](VisualScriptDocument& d)
	              {
					  d.Variables[0].Public = false;
				  }));
	CHECK(differs(door,
	              [](VisualScriptDocument& d)
	              {
					  d.Variables[1].Name = "Code";
				  }));
	CHECK(differs(door,
	              [](VisualScriptDocument& d)
	              {
					  d.Graph.Nodes[1].Settings["Name"] = "Close";
				  }));
	CHECK(differs(door,
	              [](VisualScriptDocument& d)
	              {
					  d.Graph.Nodes[1].Enabled = VisualScriptNodeEnabled::Disabled;
				  }));
}

TEST_CASE("VisualScriptEditor: the nodes of other scripts an edit breaks are found, one case per problem")
{
	VisualScriptDocument const motion = MakeMotion();
	VisualScriptDocument const door = MakeDoor();
	VisualScriptDocument const referrer = MakeReferrer();
	std::vector<std::pair<AssetHandle, VisualScriptDocument const*>> const scripts = {{Player, &referrer}};
	auto const edit = [](VisualScriptDocument const& before, auto const& change)
	{
		VisualScriptDocument after = before;
		change(after);
		return after;
	};

	SUBCASE("an edit that keeps the offer breaks nothing")
	{
		CHECK(FindBroken(Motion, motion, motion, scripts).empty());
		CHECK(FindBroken(Door, door, door, scripts).empty());
	}
	SUBCASE("a removed function or variable")
	{
		std::vector<VisualScriptReferrer> const calls = FindBroken(Motion, motion,
		                                                           edit(motion,
		                                                                [](VisualScriptDocument& d)
		                                                                {
																			d.Functions.erase(d.Functions.begin());
																		}),
		                                                           scripts);
		REQUIRE(calls.size() == 1);
		CHECK(calls[0] == VisualScriptReferrer{Player, 40, "Function.Call", VisualScriptReferrerProblem::Removed,
		                                       "Clamp01 (node 40) calls Clamp01, which is gone"});
		std::vector<VisualScriptReferrer> const variables = FindBroken(Door, door,
		                                                               edit(door,
		                                                                    [](VisualScriptDocument& d)
		                                                                    {
																				d.Variables.erase(d.Variables.begin());
																			}),
		                                                               scripts);
		CHECK(Summarize(variables) == std::vector<std::string>{"42:Removed", "43:Removed"});
		CHECK(variables[0].Message == "Get Health (node 42) reads Health, which is gone");
		CHECK(variables[1].Message == "Set Health (node 43) writes Health, which is gone");
	}
	SUBCASE("a parameter a caller links removed, and retyped")
	{
		std::vector<VisualScriptReferrer> const removed = FindBroken(Motion, motion,
		                                                             edit(motion,
		                                                                  [](VisualScriptDocument& d)
		                                                                  {
																			  d.Functions[0].Inputs = {{"Amount", "float"}};
																		  }),
		                                                             scripts);
		REQUIRE(removed.size() == 1);
		CHECK(removed[0].Problem == VisualScriptReferrerProblem::Changed);
		CHECK(removed[0].Message == "Clamp01 (node 40) uses the input X, which Clamp01 no longer has");
		std::vector<VisualScriptReferrer> const retyped = FindBroken(Motion, motion,
		                                                             edit(motion,
		                                                                  [](VisualScriptDocument& d)
		                                                                  {
																			  d.Functions[0].Outputs[0].Type = "int";
																		  }),
		                                                             scripts);
		REQUIRE(retyped.size() == 1);
		CHECK(retyped[0].Message == "Clamp01 (node 40) uses the output Result, whose type is int now (was float)");
		// A literal is a use too.
		std::vector<VisualScriptReferrer> const literal = FindBroken(Motion, motion,
		                                                             edit(motion,
		                                                                  [](VisualScriptDocument& d)
		                                                                  {
																			  d.Functions[1].Inputs[0].Name = "Line";
																		  }),
		                                                             scripts);
		CHECK(Summarize(literal) == std::vector<std::string>{"41:Changed"});
		// A parameter added to, or one the caller does not use removed from, a function breaks no call.
		CHECK(FindBroken(Motion, motion,
		                 edit(motion,
		                      [](VisualScriptDocument& d)
		                      {
								  d.Functions[1].Inputs.push_back({"Level", "int"});
							  }),
		                 scripts)
		          .empty());
	}
	SUBCASE("a parameter renamed with its former name recorded is followed")
	{
		VisualScriptDocument const renamed = edit(motion,
		                                          [](VisualScriptDocument& d)
		                                          {
													  d.Functions[0].Inputs[0] = {"Amount", "float", {"X", "Value"}};
													  d.Functions[1].Inputs[0] = {"Line", "string", {"Text"}};
												  });
		CHECK(FindBroken(Motion, motion, renamed, scripts).empty());
	}
	SUBCASE("purity")
	{
		std::vector<VisualScriptReferrer> const impure = FindBroken(Motion, motion,
		                                                            edit(motion,
		                                                                 [](VisualScriptDocument& d)
		                                                                 {
																			 d.Functions[0].Pure = false;
																		 }),
		                                                            scripts);
		REQUIRE(impure.size() == 1);
		CHECK(impure[0].Message == "Clamp01 (node 40) calls Clamp01, which is not pure now");
	}
	SUBCASE("a variable made private, and retyped")
	{
		std::vector<VisualScriptReferrer> const hidden = FindBroken(Door, door,
		                                                            edit(door,
		                                                                 [](VisualScriptDocument& d)
		                                                                 {
																			 d.Variables[0].Public = false;
																		 }),
		                                                            scripts);
		CHECK(Summarize(hidden) == std::vector<std::string>{"42:Private", "43:Private"});
		CHECK(hidden[0].Message == "Get Health (node 42) reads Health, which is private now");
		std::vector<VisualScriptReferrer> const retyped = FindBroken(Door, door,
		                                                             edit(door,
		                                                                  [](VisualScriptDocument& d)
		                                                                  {
																			  d.Variables[0].Type = "int";
																		  }),
		                                                             scripts);
		CHECK(Summarize(retyped) == std::vector<std::string>{"42:Changed", "43:Changed"});
		CHECK(retyped[0].Message == "Get Health (node 42) reads Health, whose type is int now (was float)");
		// A rename keeps the ID, which is what the nodes name. The private variable's Get was broken already.
		CHECK(FindBroken(Door, door,
		                 edit(door,
		                      [](VisualScriptDocument& d)
		                      {
								  d.Variables[0].Name = "Life";
							  }),
		                 scripts)
		          .empty());
		CHECK(FindBroken(Door, door,
		                 edit(door,
		                      [](VisualScriptDocument& d)
		                      {
								  d.Variables[1].Type = "string";
							  }),
		                 scripts)
		          .empty());
	}
	SUBCASE("a typed send's event retyped and removed")
	{
		std::vector<VisualScriptReferrer> const retyped = FindBroken(Door, door,
		                                                             edit(door,
		                                                                  [](VisualScriptDocument& d)
		                                                                  {
																			  d.Graph.Nodes[0].Settings["Parameters"] =
																				  MakeParameters({{"Damage", "int"}});
																		  }),
		                                                             scripts);
		REQUIRE(retyped.size() == 1);
		CHECK(retyped[0] == VisualScriptReferrer{Player, 45, "Event.Send", VisualScriptReferrerProblem::Changed,
		                                         "Send Hit (node 45) sends Hit, which takes (int) now (took (float))"});
		// Parameter names are labels: renaming one changes nothing a sender passes.
		CHECK(FindBroken(Door, door,
		                 edit(door,
		                      [](VisualScriptDocument& d)
		                      {
								  d.Graph.Nodes[0].Settings["Parameters"] = MakeParameters({{"Amount", "float"}});
							  }),
		                 scripts)
		          .empty());
		std::vector<VisualScriptReferrer> const removed = FindBroken(Door, door,
		                                                             edit(door,
		                                                                  [](VisualScriptDocument& d)
		                                                                  {
																			  d.Graph.Nodes.erase(d.Graph.Nodes.begin());
																		  }),
		                                                             scripts);
		CHECK(Summarize(removed) == std::vector<std::string>{"45:Removed"});
		CHECK(removed[0].Message == "Send Hit (node 45) sends Hit, which its graph no longer has");
	}
	SUBCASE("untyped senders of a renamed event are stale")
	{
		std::vector<VisualScriptReferrer> const renamed = FindBroken(Door, door,
		                                                             edit(door,
		                                                                  [](VisualScriptDocument& d)
		                                                                  {
																			  d.Graph.Nodes[1].Settings["Name"] = "Close";
																		  }),
		                                                             scripts);
		// The typed Broadcast names the event: removed for it. The untyped Send may have reached this graph.
		CHECK(Summarize(renamed) == std::vector<std::string>{"46:Removed", "48:Stale"});
		CHECK(renamed[1].Message ==
		      "Send Open (node 48) sends Open by name, which the edited graph no longer has: it may have reached that graph and now "
		      "never does");
	}
	SUBCASE("each kind switch")
	{
		// A library made a script: its calls.
		std::vector<VisualScriptReferrer> const script = FindBroken(Motion, motion,
		                                                            edit(motion,
		                                                                 [](VisualScriptDocument& d)
		                                                                 {
																			 d.Kind = VisualScriptDocumentKind::Script;
																		 }),
		                                                            scripts);
		CHECK(Summarize(script) == std::vector<std::string>{"40:Removed", "41:Removed"});
		CHECK(script[0].Message == "Clamp01 (node 40) calls Clamp01, and its graph is no function library now");
		// A script made a library: remote variables, typed sends, RunsGraph, and its events' untyped senders.
		std::vector<VisualScriptReferrer> const library = FindBroken(Door, door,
		                                                             edit(door,
		                                                                  [](VisualScriptDocument& d)
		                                                                  {
																			  d.Kind = VisualScriptDocumentKind::Library;
																		  }),
		                                                             scripts);
		CHECK(Summarize(library) ==
		      std::vector<std::string>{"42:Removed", "43:Removed", "45:Removed", "46:Removed", "47:Removed", "48:Stale"});
		CHECK(library[4].Message == "Runs Graph (node 47) asks whether an entity runs its graph, which is a function library now");
		// What was broken before is not broken by the edit: a library made a script again.
		VisualScriptDocument const wasLibrary = edit(door,
		                                             [](VisualScriptDocument& d)
		                                             {
														 d.Kind = VisualScriptDocumentKind::Library;
													 });
		CHECK(FindBroken(Door, wasLibrary, door, scripts).empty());
	}
	SUBCASE("the edited graph's own nodes, documents that do not load and repeated entries are skipped; results are sorted")
	{
		VisualScriptDocument const second = MakeReferrer();
		std::vector<std::pair<AssetHandle, VisualScriptDocument const*>> const many = {
			{Enemy, &second}, {Motion, &referrer}, {Door, nullptr}, {Player, &referrer}, {Player, &second}};
		std::vector<VisualScriptReferrer> const found = FindBroken(Motion, motion,
		                                                           edit(motion,
		                                                                [](VisualScriptDocument& d)
		                                                                {
																			d.Functions.clear();
																		}),
		                                                           many);
		REQUIRE(found.size() == 4);
		CHECK(found[0].Graph == Player);
		CHECK(found[1].Graph == Player);
		CHECK(found[2].Graph == Enemy);
		CHECK(found[0].Node == 40);
		CHECK(found[1].Node == 41);
	}
}

TEST_CASE("VisualScriptEditor: a remote node's derived title comes from the resolver, never from a local element of its ID")
{
	VisualScriptDocument const door = MakeDoor();
	VisualScriptDocument const motion = MakeMotion();
	// The document has a variable 5 and a function 1 of its own, which the remote nodes must not be titled after.
	VisualScriptDocument local = MakeReferrer();
	local.Variables = {MakeVariable(5, "Speed", "float", false)};
	VisualScriptFunction own;
	own.ID = 1;
	own.Name = "Jump";
	local.Functions = {own};
	VisualScriptNode const& get = FindNode(local, 42);
	VisualScriptNode const& call = FindNode(local, 40);

	CHECK(DeriveVisualScriptNodeTitle(get, local) == "Get Variable");
	CHECK(DeriveVisualScriptNodeTitle(call, local) == "Call Function");
	VisualScriptGraphResolver const resolver = [&](AssetHandle graph) -> VisualScriptDocument const*
	{
		return graph == Door ? &door : graph == Motion ? &motion : nullptr;
	};
	CHECK(DeriveVisualScriptNodeTitle(get, local, resolver) == "Get Health");
	CHECK(DeriveVisualScriptNodeTitle(FindNode(local, 43), local, resolver) == "Set Health");
	CHECK(DeriveVisualScriptNodeTitle(call, local, resolver) == "Clamp01");
	// A graph that does not resolve, or does not have the element, titles the kind.
	VisualScriptGraphResolver const none = [](AssetHandle) -> VisualScriptDocument const*
	{
		return nullptr;
	};
	CHECK(DeriveVisualScriptNodeTitle(get, local, none) == "Get Variable");
	CHECK(DeriveVisualScriptNodeTitle(call, local,
	                                  [&](AssetHandle)
	                                  {
										  return &door;
									  }) == "Call Function");
	// Local nodes keep their local names; senders are titled by their Name, typed or not.
	CHECK(DeriveVisualScriptNodeTitle(MakeNode(50, "Variable.Get", Json::object({{"Variable", 5}})), local, resolver) == "Get Speed");
	CHECK(DeriveVisualScriptNodeTitle(MakeNode(51, "Function.Call", Json::object({{"Function", 1}})), local, resolver) == "Jump");
	CHECK(DeriveVisualScriptNodeTitle(FindNode(local, 45), local) == "Send Hit");
	CHECK(DeriveVisualScriptNodeTitle(FindNode(local, 46), local) == "Broadcast Open");
	CHECK(DeriveVisualScriptNodeTitle(MakeNode(52, "Event.Broadcast"), local) == "Broadcast Event");
	// A malformed Graph setting is no local reference either.
	CHECK(DeriveVisualScriptNodeTitle(MakeNode(53, "Variable.Get", Json::object({{"Variable", 5}, {"Graph", 12}})), local, resolver) ==
	      "Get Variable");
}
