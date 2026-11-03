#include "TestUtilities.h"
#include "VisualScriptTestDocuments.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/VisualScripting/VisualScriptClipboard.h"
#include "Editor/VisualScripting/VisualScriptController.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptFind.h"
#include "Editor/VisualScripting/VisualScriptNodeTitles.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptProjectIndex.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// The local-reference sweep (spec-slice3 §6.4.4): every reader of a node's Variable, Function or Name setting that takes it
// for an element of the node's own document first checks that the node names no other graph. The document below has a
// variable 5 and a function 7 of its own and nodes of every cross-graph kind naming another graph's 5 and 7: each case fails
// when one reader misses the check, instead of miscounting.
using namespace Strada;

namespace
{
	AssetHandle const Door(UUID(7100001ull));
	AssetHandle const Motion(UUID(7100002ull));
	// The sweep document's own handle in the pure cases.
	AssetHandle const Self(UUID(7100003ull));

	VisualScriptNode MakeNode(uint32_t id, std::string type, Json settings = Json::object(), Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = glm::vec2(static_cast<float>(id % 10) * 320.0f, static_cast<float>(id / 10) * 240.0f);
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

	Json Remote(AssetHandle graph, char const* key, uint32_t id)
	{
		return Json::object({{key, id}, {"Graph", graph.ToString()}});
	}

	// Door: the public variables Health (5) and Armor (9), and the custom event Hit (Damage: float).
	VisualScriptDocument MakeDoor()
	{
		VisualScriptDocument door;
		door.Variables = {MakeVariable(5, "Health", "float", true), MakeVariable(9, "Armor", "int", true)};
		door.Graph.Nodes = {MakeNode(
			1, "Event.Custom", Json::object({{"Name", "Hit"}, {"Parameters", Json::array({{{"Name", "Damage"}, {"Type", "float"}}})}}))};
		return door;
	}

	// Motion: a library whose function 7, Clamp01, takes Height: float (named so that the sweep's call keeps its link).
	VisualScriptDocument MakeMotion()
	{
		VisualScriptDocument motion = MakeVisualScriptTemplate(VisualScriptTemplate::Library);
		VisualScriptFunction clamp;
		clamp.ID = 7;
		clamp.Name = "Clamp01";
		clamp.Pure = false;
		clamp.Inputs = {{"Height", "float"}};
		clamp.Graph.Nodes = {MakeNode(70, "Function.Entry")};
		motion.Functions = {clamp};
		return motion;
	}

	// The sweep document: the variable Speed (5), the function Jump (7, Height: float, its local Step 8), and one local and one
	// remote node of each kind that reads those settings.
	VisualScriptDocument MakeSweepDocument(AssetHandle self)
	{
		VisualScriptDocument document;
		document.Variables = {MakeVariable(5, "Speed", "float", false)};
		VisualScriptFunction jump;
		jump.ID = 7;
		jump.Name = "Jump";
		jump.Inputs = {{"Height", "float"}};
		VisualScriptLocal step;
		step.ID = 8;
		step.Name = "Step";
		step.Type = "int";
		jump.Locals = {step};
		jump.Graph.Nodes = {MakeNode(70, "Function.Entry"),
		                    MakeNode(71, "Flow.Delay", Json::object(), Json::object({{"Duration", 2.0}})),
		                    MakeNode(73, "Variable.Get", Json::object({{"Variable", 8}})),
		                    MakeNode(74, "Variable.Get", Remote(Door, "Variable", 8)),
		                    MakeNode(75, "Function.Call", Json::object({{"Function", 7}})),
		                    MakeNode(76, "Function.Call", Remote(Motion, "Function", 7))};
		jump.Graph.Links = {MakeLink(70, "Then", 71, "Execute")};
		document.Functions = {jump};
		document.Graph.Nodes = {MakeNode(1, "Event.Update"),
		                        MakeNode(2, "Event.Custom", Json::object({{"Name", "Hit"}})),
		                        MakeNode(10, "Variable.Get", Json::object({{"Variable", 5}})),
		                        MakeNode(11, "Variable.Get", Remote(Door, "Variable", 5)),
		                        MakeNode(12, "Variable.Set", Remote(Door, "Variable", 5)),
		                        MakeNode(13, "Function.Call", Json::object({{"Function", 7}})),
		                        MakeNode(14, "Function.Call", Remote(Motion, "Function", 7)),
		                        MakeNode(15, "Event.Send", Json::object({{"Name", "Hit"}})),
		                        MakeNode(16, "Event.Send", Json::object({{"Name", "Hit"}, {"Graph", Door.ToString()}})),
		                        MakeNode(17, "Event.Broadcast", Json::object({{"Name", "Hit"}})),
		                        MakeNode(18, "Event.Broadcast", Json::object({{"Name", "Hit"}, {"Graph", Door.ToString()}})),
		                        MakeNode(19, "RunsGraph", Json::object({{"Graph", Door.ToString()}})),
		                        MakeNode(20, "Event.Send", Json::object({{"Name", "Hit"}, {"Graph", self.ToString()}}))};
		document.Graph.Links = {MakeLink(1, "Then", 14, "Execute"), MakeLink(14, "Then", 13, "Execute"),
		                        MakeLink(10, "Value", 13, "Height"), MakeLink(11, "Value", 14, "Height")};
		return document;
	}

	template<typename T>
	T RequireOk(Result<T> result)
	{
		REQUIRE_MESSAGE(result.IsOk(), result.GetError());
		return result.TakeValue();
	}

	void RequireOk(Result<void> const& result)
	{
		REQUIRE_MESSAGE(result.IsOk(), result.GetError());
	}

	VisualScriptNode const* Find(VisualScriptDocument const& document, uint32_t id)
	{
		return FindVisualScriptNode(document, id);
	}

	std::vector<uint32_t> GetNodes(std::vector<VisualScriptFindResult> const& results)
	{
		std::vector<uint32_t> nodes;
		for (VisualScriptFindResult const& result : results)
		{
			if (result.Node != 0)
			{
				nodes.push_back(result.Node);
			}
		}
		std::sort(nodes.begin(), nodes.end());
		return nodes;
	}

	bool HasLink(VisualScriptGraph const& graph, VisualScriptLink const& link)
	{
		return std::find(graph.Links.begin(), graph.Links.end(), link) != graph.Links.end();
	}

	// The remote nodes as the sweep document holds them (those of the event graph, and of Jump's while it is there): every
	// operation below must leave them exactly so.
	void CheckRemoteNodesUnchanged(VisualScriptDocument const& edited, bool withJump = true)
	{
		VisualScriptDocument const original = MakeSweepDocument(Self);
		std::vector<uint32_t> ids = {11, 12, 14, 16, 18, 19, 20};
		if (withJump)
		{
			ids.insert(ids.end(), {74, 76});
		}
		for (uint32_t const id : ids)
		{
			CAPTURE(id);
			VisualScriptNode const* const before = Find(original, id);
			VisualScriptNode const* const after = Find(edited, id);
			REQUIRE(after != nullptr);
			CHECK(after->Type == before->Type);
			CHECK(after->Settings == before->Settings);
			CHECK(after->Inputs == before->Inputs);
		}
	}
}

TEST_CASE("VisualScriptEditor: uses, removals and signature edits of a variable or function leave the nodes naming another graph alone")
{
	VisualScriptDocument const document = MakeSweepDocument(Self);

	// Only the document's own nodes use its elements: the remote Get, Set and call name Door's 5 and Motion's 7.
	CHECK(CountVisualScriptUses(document, VisualScriptElementKind::Variable, 5) == 1);
	CHECK(CountVisualScriptUses(document, VisualScriptElementKind::Local, 8) == 1);
	CHECK(CountVisualScriptUses(document, VisualScriptElementKind::Function, 7) == 1);
	CHECK(FindVisualScriptUses(document.Graph, VisualScriptElementKind::Variable, 5) == std::vector<uint32_t>{10});
	CHECK(FindVisualScriptUses(document.Graph, VisualScriptElementKind::Function, 7) == std::vector<uint32_t>{13});
	CHECK(FindVisualScriptUses(document.Functions[0].Graph, VisualScriptElementKind::Local, 8) == std::vector<uint32_t>{73});

	SUBCASE("removing the variable removes its own nodes only")
	{
		VisualScriptDocument edited = document;
		CHECK(RequireOk(VisualScriptEditing::RemoveVariable(edited, 5, true, nullptr)) == 1);
		CHECK(Find(edited, 10) == nullptr);
		CheckRemoteNodesUnchanged(edited);
	}

	SUBCASE("removing the function removes its own calls only")
	{
		VisualScriptDocument edited = document;
		CHECK(RequireOk(VisualScriptEditing::RemoveFunction(edited, 7, true, nullptr)) == 1);
		CHECK(Find(edited, 13) == nullptr);
		CheckRemoteNodesUnchanged(edited, false);
		CHECK(HasLink(edited.Graph, MakeLink(11, "Value", 14, "Height")));
	}

	SUBCASE("a renamed input renames the links of its own calls only")
	{
		VisualScriptDocument edited = document;
		RequireOk(VisualScriptEditing::SetFunctionSignature(edited, 7, {{"Lift", "float"}}, {}));
		CHECK(HasLink(edited.Graph, MakeLink(10, "Value", 13, "Lift")));
		CHECK(HasLink(edited.Graph, MakeLink(11, "Value", 14, "Height")));
		CheckRemoteNodesUnchanged(edited);
	}

	SUBCASE("a function made pure loses the flow links of its own calls only")
	{
		VisualScriptDocument edited = document;
		RequireOk(VisualScriptEditing::SetFunctionPure(edited, 7, true));
		CHECK_FALSE(HasLink(edited.Graph, MakeLink(14, "Then", 13, "Execute")));
		CHECK(HasLink(edited.Graph, MakeLink(1, "Then", 14, "Execute")));
		CheckRemoteNodesUnchanged(edited);
	}

	SUBCASE("a duplicated function names the copy's local and itself, and copies the remote nodes as they are")
	{
		VisualScriptDocument edited = document;
		uint32_t const copy = RequireOk(VisualScriptEditing::DuplicateFunction(edited, 7));
		REQUIRE(edited.Functions.size() == 2);
		VisualScriptFunction const& duplicated = edited.Functions[1];
		REQUIRE(duplicated.ID == copy);
		REQUIRE(duplicated.Locals.size() == 1);
		uint32_t const local = duplicated.Locals[0].ID;
		std::vector<VisualScriptNode> const& nodes = duplicated.Graph.Nodes;
		REQUIRE(nodes.size() == 6);
		CHECK(nodes[2].Settings == Json::object({{"Variable", local}}));
		CHECK(nodes[3].Settings == Remote(Door, "Variable", 8));
		CHECK(nodes[4].Settings == Json::object({{"Function", copy}}));
		CHECK(nodes[5].Settings == Remote(Motion, "Function", 7));
		CheckRemoteNodesUnchanged(edited);
	}

	SUBCASE("an input promoted to the function's goes to its own calls only")
	{
		VisualScriptDocument edited = document;
		VisualScriptNodePins const pins = [](uint32_t node)
		{
			std::vector<VisualScriptNodePin> row;
			if (node == 71)
			{
				row.push_back({"Execute", VisualScriptPinDirection::In, std::string(VisualScriptExecType)});
				row.push_back({"Duration", VisualScriptPinDirection::In, "float"});
				row.push_back({"Then", VisualScriptPinDirection::Out, std::string(VisualScriptExecType)});
			}
			return row;
		};
		RequireOk(VisualScriptEditing::PromoteToFunctionInput(edited, 7, {71, "Duration"}, pins));
		CHECK(Find(edited, 13)->Inputs == Json::object({{"Duration", 2.0}}));
		CheckRemoteNodesUnchanged(edited);
	}
}

TEST_CASE("VisualScriptEditor: find, the clipboard and derived titles never take a remote node's IDs for the document's elements")
{
	VisualScriptDocument const document = MakeSweepDocument(Self);
	auto const references = [&document](VisualScriptReference reference, AssetHandle self = {})
	{
		return GetNodes(FindInVisualScript(document, nullptr, VisualScriptFindQuery{{}, std::move(reference)}, self));
	};
	CHECK(references({VisualScriptReference::Kind::Variable, 5, {}}) == std::vector<uint32_t>{10});
	CHECK(references({VisualScriptReference::Kind::Variable, 8, {}}) == std::vector<uint32_t>{73});
	CHECK(references({VisualScriptReference::Kind::Function, 7, {}}) == std::vector<uint32_t>{13, 75});
	// The document searched as its own graph: the same, the reference naming that graph or not.
	CHECK(references({VisualScriptReference::Kind::Variable, 5, {}, Self}, Self) == std::vector<uint32_t>{10});
	CHECK(references({VisualScriptReference::Kind::Function, 7, {}}, Self) == std::vector<uint32_t>{13, 75});
	// Door's and Motion's elements of those IDs are the remote nodes'.
	CHECK(references({VisualScriptReference::Kind::Variable, 5, {}, Door}, Self) == std::vector<uint32_t>{11, 12});
	CHECK(references({VisualScriptReference::Kind::Variable, 8, {}, Door}, Self) == std::vector<uint32_t>{74});
	CHECK(references({VisualScriptReference::Kind::Function, 7, {}, Motion}, Self) == std::vector<uint32_t>{14, 76});
	// This script's custom event: the event, the untyped senders that may reach it and the Send typed with this script.
	CHECK(references({VisualScriptReference::Kind::CustomEvent, 0, "Hit", Self}, Self) == std::vector<uint32_t>{2, 15, 17, 20});
	// Door's: the untyped senders and the ones typed with Door.
	CHECK(references({VisualScriptReference::Kind::CustomEvent, 0, "Hit", Door}, Self) == std::vector<uint32_t>{15, 16, 17, 18});
	// A custom event of no graph: every event and sender of the name.
	CHECK(references({VisualScriptReference::Kind::CustomEvent, 0, "Hit"}, Self) == std::vector<uint32_t>{2, 15, 16, 17, 18, 20});

	// Copied remote nodes take no variable or function of the document along.
	std::vector<uint32_t> const copied = {11, 12, 14, 16, 18, 19};
	Result<VisualScriptClipboardContent> const content =
		ReadVisualScriptClipboard(WriteVisualScriptClipboard(document, 0, copied, Self, nullptr));
	REQUIRE(content.IsOk());
	CHECK(content.GetValue().Document.Variables.empty());
	CHECK(content.GetValue().Document.Functions.empty());
	CHECK(content.GetValue().Document.Graph.Nodes.size() == copied.size());

	// Without a resolver, a remote node is titled by its kind, never after the local element of its ID.
	CHECK(DeriveVisualScriptNodeTitle(*Find(document, 10), document) == "Get Speed");
	CHECK(DeriveVisualScriptNodeTitle(*Find(document, 11), document) == "Get Variable");
	CHECK(DeriveVisualScriptNodeTitle(*Find(document, 12), document) == "Set Variable");
	CHECK(DeriveVisualScriptNodeTitle(*Find(document, 13), document) == "Jump");
	CHECK(DeriveVisualScriptNodeTitle(*Find(document, 14), document) == "Call Function");
}

TEST_CASE("VisualScriptEditor: renaming a custom event renames its untyped senders and those typed with the script itself")
{
	VisualScriptDocument document = MakeSweepDocument(Self);
	SUBCASE("with the script's handle")
	{
		CHECK(RequireOk(VisualScriptEditing::RenameCustomEvent(document, 2, "Damaged", Self)) == 3);
		for (uint32_t const id : {2u, 15u, 17u, 20u})
		{
			CAPTURE(id);
			CHECK(Find(document, id)->Settings["Name"] == "Damaged");
		}
	}
	SUBCASE("without it, the typed ones stay")
	{
		CHECK(RequireOk(VisualScriptEditing::RenameCustomEvent(document, 2, "Damaged")) == 2);
		CHECK(Find(document, 20)->Settings["Name"] == "Hit");
	}
	// Senders typed with another graph name that graph's event.
	CHECK(Find(document, 16)->Settings["Name"] == "Hit");
	CHECK(Find(document, 18)->Settings["Name"] == "Hit");
}

namespace
{
	// A graph window's model of the sweep document (its file in a project), with Door and Motion as the project's other
	// scripts, as the window manager's index gives them.
	struct SweepWindowFixture
	{
		SweepWindowFixture()
			: Project(Testing::MakeGraphProject(Temporary, MakeSweepDocument(AssetHandle()))),
			  Settings(Temporary.GetPath() / "Views.json")
		{
			// The typed Send naming the script itself takes the handle the project gave it.
			VisualScriptDocument document = MakeSweepDocument(Project.Graph);
			REQUIRE(VisualScriptSerializer::SaveToFile(document, Project.File).IsOk());
			REQUIRE(AssetManager::ReloadAsset(Project.Graph).IsOk());
			Graphs = {MakeGraph(Door, "Door", CreateRef<VisualScriptDocument const>(MakeDoor())),
			          MakeGraph(Motion, "Motion", CreateRef<VisualScriptDocument const>(MakeMotion()))};
			Semantics.SetGraphs({{Door, Graphs[0].Document}, {Motion, Graphs[1].Document}});
			Model.emplace(Project.Graph, Settings);
			Input.CanvasSize = glm::vec2(1920.0f, 1080.0f);
			Input.Hovered = true;
			Input.Focused = true;
		}

		static VisualScriptProjectGraph MakeGraph(AssetHandle graph, std::string name, Ref<VisualScriptDocument const> document)
		{
			VisualScriptProjectGraph made;
			made.Graph = graph;
			made.Path = "Scripts/" + name + ".sgraph";
			made.DisplayName = std::move(name);
			made.Kind = document->Kind;
			made.Document = std::move(document);
			return made;
		}

		std::vector<VisualScriptEditorIntent> Frame()
		{
			Time += 1.0 / 60.0;
			Input.Time = Time;
			Model->Sync(Semantics, Measure, Theme, false, Time);
			std::vector<VisualScriptEditorIntent> intents = Model->Update(Input);
			Input.Pressed = {};
			Input.Released = {};
			Input.DoubleClicked = {};
			return intents;
		}

		// A double click at a point of the canvas, and what the model asked for.
		std::vector<VisualScriptEditorIntent> DoubleClick(glm::vec2 point)
		{
			std::vector<VisualScriptEditorIntent> all;
			auto const append = [&all](std::vector<VisualScriptEditorIntent> intents)
			{
				all.insert(all.end(), intents.begin(), intents.end());
			};
			Input.Mouse = point;
			append(Frame());
			for (bool const second : {false, true})
			{
				Input.Down[0] = true;
				Input.Pressed[0] = true;
				Input.DoubleClicked[0] = second;
				append(Frame());
				Input.Down[0] = false;
				Input.Released[0] = true;
				append(Frame());
			}
			return all;
		}

		VisualScriptEditContext Context() const
		{
			VisualScriptEditContext context(*Model);
			context.Library = Semantics.IsAvailable() ? &Semantics.GetFixtureLibrary() : nullptr;
			context.ScriptName = "Graph";
			context.Graph = Project.Graph;
			return context;
		}

		VisualScriptControllerResult Resolve(VisualScriptEditorIntent const& intent) const
		{
			Result<VisualScriptControllerResult> result = ResolveVisualScriptIntent(intent, Context());
			REQUIRE_MESSAGE(result.IsOk(), (result ? std::string() : result.GetError()));
			return result.TakeValue();
		}

		VisualScriptControllerResult Act(VisualScriptEditorAction action, uint32_t node) const
		{
			return Resolve(VisualScriptActionIntent{action, {node}, glm::vec2(0.0f)});
		}

		// The text the canvas draws as a node's title (a pill's: its variable's name).
		std::string GetCanvasTitle(uint32_t node) const
		{
			VisualScriptCanvasModel const& canvas = Model->GetCanvas();
			std::optional<uint32_t> const index = canvas.FindNode(node);
			REQUIRE(index.has_value());
			return std::string(canvas.GetText(canvas.GetNodes()[*index].Title));
		}

		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Temporary;
		Testing::GraphProject Project;
		VisualScriptViewSettings Settings;
		Testing::FixtureVisualScriptSemantics Semantics;
		VisualScriptTextMeasurer Measure = Testing::FixedWidthMeasurer(0.5f);
		VisualScriptThemeAnchors Theme;
		std::vector<VisualScriptProjectGraph> Graphs;
		std::optional<VisualScriptEditorModel> Model;
		VisualScriptCanvasInput Input;
		double Time = 0.0;
	};
}

TEST_CASE("VisualScriptEditor: the canvas names a remote Get after the other graph's variable, never after the local one of its ID")
{
	SweepWindowFixture fixture;
	// Without the runtime the Gets are pills (their pins are inferred from their links), named after their variables.
	fixture.Semantics.SetAvailable(false);
	fixture.Frame();
	CHECK(fixture.GetCanvasTitle(10) == "Speed");
	// No project graph is known yet: the kind, not Speed.
	CHECK(fixture.GetCanvasTitle(11) == "Get Variable");
	fixture.Model->SetProjectGraphs(fixture.Graphs, 1);
	fixture.Frame();
	CHECK(fixture.GetCanvasTitle(11) == "Health");
	CHECK(fixture.GetCanvasTitle(10) == "Speed");
	// With the runtime, the analysis titles the remote nodes from the other graphs (the fixture's as the runtime's).
	fixture.Semantics.SetAvailable(true);
	fixture.Model->SetProjectGraphs(fixture.Graphs, 2);
	fixture.Frame();
	fixture.Frame();
	CHECK(fixture.GetCanvasTitle(12) == "Set Health");
	CHECK(fixture.GetCanvasTitle(14) == "Clamp01");
}

TEST_CASE(
	"VisualScriptEditor: opening, renaming and finding the references of a remote node go to the other graph, never to a local element")
{
	SweepWindowFixture fixture;
	fixture.Model->SetProjectGraphs(fixture.Graphs, 1);
	fixture.Frame();
	fixture.Frame();
	using Action = VisualScriptEditorAction;

	// A double click on a remote call's header opens nothing of this graph: the controller asks for Motion's window.
	std::vector<VisualScriptEditorIntent> const clicked =
		fixture.DoubleClick(Testing::CanvasPoint(*fixture.Model, 14, glm::vec2(0.5f, 0.05f)));
	CHECK(std::none_of(clicked.begin(), clicked.end(),
	                   [](VisualScriptEditorIntent const& intent)
	                   {
						   return std::holds_alternative<VisualScriptOpenFunctionIntent>(intent);
					   }));
	auto const opened = std::find_if(clicked.begin(), clicked.end(),
	                                 [](VisualScriptEditorIntent const& intent)
	                                 {
										 VisualScriptActionIntent const* const action = std::get_if<VisualScriptActionIntent>(&intent);
										 return action != nullptr && action->Action == Action::OpenNode;
									 });
	REQUIRE(opened != clicked.end());
	VisualScriptControllerResult const call = fixture.Resolve(*opened);
	CHECK_FALSE(call.ShowFunction.has_value());
	REQUIRE(call.Open.has_value());
	CHECK(call.Open->Graph == Motion);
	CHECK(call.Open->Function == std::optional<uint32_t>(7));
	CHECK(call.Open->Frame == VisualScriptFrame::All);

	// A typed Send goes to Door's event; a typed Send naming this script goes to its own.
	VisualScriptControllerResult const send = fixture.Act(Action::OpenNode, 16);
	REQUIRE(send.Open.has_value());
	CHECK(send.Open->Graph == Door);
	CHECK(send.Open->Select == std::vector<uint32_t>{1});
	VisualScriptControllerResult const own = fixture.Act(Action::OpenNode, 20);
	REQUIRE(own.Open.has_value());
	CHECK(own.Open->Graph == fixture.Project.Graph);
	CHECK(own.Open->Select == std::vector<uint32_t>{2});
	// A remote Get reveals no local variable: it opens Door; RunsGraph opens its graph.
	VisualScriptControllerResult const get = fixture.Act(Action::OpenNode, 11);
	CHECK_FALSE(get.Reveal.has_value());
	REQUIRE(get.Open.has_value());
	CHECK(get.Open->Graph == Door);
	VisualScriptControllerResult const runs = fixture.Act(Action::OpenNode, 19);
	REQUIRE(runs.Open.has_value());
	CHECK(runs.Open->Graph == Door);

	// The Open labels name the target.
	auto const openLabel = [&fixture](uint32_t node)
	{
		std::vector<uint32_t> const selection = {node};
		fixture.Model->Select(selection);
		fixture.Frame();
		std::vector<VisualScriptMenuItem> const menu = BuildVisualScriptNodeMenu(fixture.Context());
		auto const open = std::find_if(menu.begin(), menu.end(),
		                               [](VisualScriptMenuItem const& item)
		                               {
										   return item.Action == Action::OpenNode;
									   });
		return open != menu.end() ? open->Label : std::string();
	};
	CHECK(openLabel(14) == "Open Clamp01 in Motion");
	CHECK(openLabel(16) == "Go to Event Hit in Door");
	CHECK(openLabel(11) == "Select Variable Health in Door");
	CHECK(openLabel(19) == "Open Door");
	CHECK(openLabel(13) == "Open Jump");

	// F2 reveals no local element of a remote node, and Find References looks for the other graph's element (spec-slice3
	// §10.5).
	for (uint32_t const node : {11u, 12u, 14u, 16u})
	{
		CAPTURE(node);
		VisualScriptControllerResult const rename = fixture.Act(Action::Rename, node);
		CHECK_FALSE(rename.Reveal.has_value());
	}
	CHECK(fixture.Act(Action::FindReferences, 11).FindReferences ==
	      std::optional<VisualScriptReference>(VisualScriptReference{VisualScriptReference::Kind::Variable, 5, {}, Door}));
	CHECK(fixture.Act(Action::FindReferences, 12).FindReferences ==
	      std::optional<VisualScriptReference>(VisualScriptReference{VisualScriptReference::Kind::Variable, 5, {}, Door}));
	CHECK(fixture.Act(Action::FindReferences, 14).FindReferences ==
	      std::optional<VisualScriptReference>(VisualScriptReference{VisualScriptReference::Kind::Function, 7, {}, Motion}));
	CHECK(fixture.Act(Action::FindReferences, 16).FindReferences ==
	      std::optional<VisualScriptReference>(VisualScriptReference{VisualScriptReference::Kind::CustomEvent, 0, "Hit", Door}));
	// RunsGraph names a graph, no element of it.
	CHECK_FALSE(fixture.Act(Action::FindReferences, 19).FindReferences.has_value());
	CHECK(fixture.Act(Action::FindReferences, 10).FindReferences ==
	      std::optional<VisualScriptReference>(VisualScriptReference{VisualScriptReference::Kind::Variable, 5, {}}));
	// A remote Get's Open reveals Door's variable in its window, and every Open of another graph names where it came from.
	VisualScriptControllerResult const revealed = fixture.Act(Action::OpenNode, 11);
	REQUIRE(revealed.Open.has_value());
	CHECK(revealed.Open->Reveal ==
	      std::optional<VisualScriptReference>(VisualScriptReference{VisualScriptReference::Kind::Variable, 5, {}}));
	VisualScriptViewState const view = fixture.Model->GetView().GetTargetState();
	CHECK(revealed.Open->Origin == std::optional<VisualScriptOrigin>(VisualScriptOrigin{fixture.Project.Graph, view.Function, view.Center,
	                                                                                    view.Zoom, VisualScriptHistoryStep::Jump}));
}
