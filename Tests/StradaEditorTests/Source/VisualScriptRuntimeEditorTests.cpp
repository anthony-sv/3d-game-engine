#include "HeadlessImGui.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"
#include "VisualScriptPanelTestUtilities.h"
#include "VisualScriptTestUtilities.h"

#include "Editor/Commands/EntityCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/Panels/VisualScriptEditorPanel.h"
#include "Editor/PlayMode.h"
#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptQuickFixes.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Script/VisualScriptEngine.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

// The graph window with the scripting runtime: the engine's node library and analysis, and play mode (spec §15.4's runtime
// tests). Undo is the editor's Ctrl+Z, EditorOperations::Undo.
namespace
{
	using Testing::PanelFixture;
	using Testing::ToImVec2;

	constexpr float FrameTime = 1.0f / 60.0f;

	VisualScriptNode MakeNode(uint32_t id, std::string type, glm::vec2 position, Json settings = Json::object(),
	                          Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Position = position;
		node.Settings = std::move(settings);
		node.Inputs = std::move(inputs);
		return node;
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

	VisualScriptLink MakeLink(uint32_t fromNode, std::string fromPin, uint32_t toNode, std::string toPin)
	{
		return {{fromNode, std::move(fromPin)}, {toNode, std::move(toPin)}};
	}

	// Create -> Delay 0.5 s -> Log Info "Waited" -> Set Done to true.
	VisualScriptDocument MakeDelayDocument()
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Done", "bool", false));
		document.Graph.Nodes = {
			MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)),
			MakeNode(11, "Flow.Delay", glm::vec2(320.0f, 0.0f), Json::object(), Json::object({{"Duration", 0.5}})),
			MakeNode(13, "Call:Strada.Log.Info(string)", glm::vec2(640.0f, 0.0f), Json::object(), Json::object({{"Message", "Waited"}})),
			MakeNode(12, "Variable.Set", glm::vec2(960.0f, 0.0f), Json::object({{"Variable", 1}}), Json::object({{"Value", true}})),
		};
		document.Graph.Links = {MakeLink(10, "Then", 11, "Execute"), MakeLink(11, "Completed", 13, "Execute"),
		                        MakeLink(13, "Then", 12, "Execute")};
		return document;
	}

	VisualScriptDocument LoadTestDocument(std::string_view name)
	{
		Result<std::string> text = FileSystem::ReadTextFile(FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" /
		                                                    "Documents" / FileSystem::PathFromUtf8(name));
		REQUIRE_MESSAGE(text.IsOk(), (text ? std::string() : text.GetError()));
		Result<Json> json = ParseJson(text.GetValue());
		REQUIRE(json.IsOk());
		Result<VisualScriptDocument> document =
			VisualScriptSerializer::DeserializeDocument(json.GetValue(), AssetManager::CreateDeserializationContext());
		REQUIRE_MESSAGE(document.IsOk(), (document ? std::string() : document.GetError()));
		return document.TakeValue();
	}

	// The graph window of a project's script with the scripting runtime's semantics, and play mode as the editor runs it.
	struct RuntimeFixture
	{
		explicit RuntimeFixture(VisualScriptDocument const& document)
			: Window(document, &Semantics)
		{
		}

		// An entity of the edited scene that runs the graph.
		UUID CreateRunner(std::string name)
		{
			EntityCreateInfo info;
			info.Name = std::move(name);
			info.Components = Json::object({{"VisualScript", Json::object({{"Graph", Window.Graph.ToString()}})}});
			Result<UUID> const entity = Window.Operations.CreateEntity(info);
			REQUIRE_MESSAGE(entity.IsOk(), (entity ? std::string() : entity.GetError()));
			return entity.GetValue();
		}

		ScriptFieldValue GetVariable(UUID entity, std::string const& name) const
		{
			Result<ScriptFieldMap> const values = VisualScriptEngine::GetInstanceVariables(entity);
			REQUIRE_MESSAGE(values.IsOk(), (values ? std::string() : values.GetError()));
			auto const found = values.GetValue().find(name);
			REQUIRE(found != values.GetValue().end());
			return found->second;
		}

		VisualScriptEditorModel const& Model() const { return Window.Model(); }

		Testing::ScriptRuntimeScope Runtime;
		EngineVisualScriptSemantics Semantics;
		PanelFixture Window;
		EditorScripts Scripts{Window.Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode Play{Window.Context, Scripts};
	};
}

TEST_CASE("VisualScriptEditor: with the scripting runtime, wildcard pins are drawn with their resolved types and an implicit "
          "conversion draws a gradient")
{
	// Data.json converts with Convert nodes only: an int read into a float variable adds an implicit conversion.
	VisualScriptDocument document = LoadTestDocument("Data.json");
	uint32_t next = 0;
	for (VisualScriptNode const& node : document.Graph.Nodes)
	{
		next = std::max(next, node.ID + 1);
	}
	for (VisualScriptVariable const& variable : document.Variables)
	{
		next = std::max(next, variable.ID + 1);
	}
	auto const findVariable = [&document](std::string_view name)
	{
		auto const found = std::find_if(document.Variables.begin(), document.Variables.end(),
		                                [name](VisualScriptVariable const& variable)
		                                {
											return variable.Name == name;
										});
		REQUIRE(found != document.Variables.end());
		REQUIRE((found->Type == "int" || found->Type == "float"));
		return found->ID;
	};
	document.Graph.Nodes.push_back(
		MakeNode(next, "Variable.Get", glm::vec2(0.0f, -640.0f), Json::object({{"Variable", findVariable("Count")}})));
	document.Graph.Nodes.push_back(
		MakeNode(next + 1, "Variable.Set", glm::vec2(320.0f, -640.0f), Json::object({{"Variable", findVariable("Ratio")}})));
	document.Graph.Links.push_back(MakeLink(next, "Value", next + 1, "Value"));
	RuntimeFixture fixture(document);
	fixture.Window.CheckDrawingChangesNothing();
	REQUIRE(fixture.Model().GetState().Analyzed);
	VisualScriptCanvasModel const& canvas = fixture.Model().GetCanvas();
	VisualScriptLibrary const* library = fixture.Semantics.GetLibrary().get();
	REQUIRE(library != nullptr);

	// The data pins of reroutes and selects, wildcards in the library, are typed as the analysis resolved them.
	size_t resolved = 0;
	for (VisualScriptNodeVisual const& node : canvas.GetNodes())
	{
		VisualScriptNode const* stored = nullptr;
		for (VisualScriptNode const& candidate : fixture.Window.GetDocument().Graph.Nodes)
		{
			if (candidate.ID == node.ID)
			{
				stored = &candidate;
			}
		}
		REQUIRE(stored != nullptr);
		VisualScriptNodeTemplate const* entry = library->FindNode(stored->Type);
		if (entry == nullptr)
		{
			continue;
		}
		for (VisualScriptPinVisual const& pin : canvas.GetPins(node))
		{
			for (VisualScriptPin const& templatePin : entry->Pins)
			{
				if (templatePin.Name == canvas.GetText(pin.Name) && templatePin.Direction == pin.Direction &&
				    templatePin.Type == VisualScriptWildcardType && pin.HasFlag(VisualScriptPinFlags::Linked))
				{
					CAPTURE(stored->Type);
					CHECK(pin.Type != VisualScriptWildcardType);
					resolved++;
				}
			}
		}
	}
	CHECK(resolved > 0);

	// A link between pins of different types that convert: its wire fades from one type's colour to the other's.
	size_t converted = 0;
	for (VisualScriptLinkVisual const& link : canvas.GetLinks())
	{
		if (link.HasFlag(VisualScriptLinkFlags::Converted))
		{
			CHECK(link.FromColor != link.ToColor);
			converted++;
		}
	}
	CHECK(converted > 0);
	CHECK(fixture.Window.Panel->GetRenderer().GetStats().DrawnWires > 0);
}

TEST_CASE("VisualScriptEditor: a node the runtime resolves under another type is written canonical by the next edit, in that "
          "edit's step")
{
	VisualScriptDocument document;
	document.Graph.Nodes = {
		MakeNode(1, "Event.Create", glm::vec2(0.0f, 0.0f)),
		MakeNode(2, "Call:Strada.Log.Info", glm::vec2(320.0f, 0.0f), Json::object(), Json::object({{"Message", "Hi"}})),
		MakeNode(3, "Flow.Delay", glm::vec2(320.0f, 320.0f)),
	};
	document.Graph.Links = {MakeLink(1, "Then", 2, "Execute")};
	RuntimeFixture fixture(document);
	fixture.Window.CheckDrawingChangesNothing();
	std::string const bytes = fixture.Window.ReadFile();
	REQUIRE(fixture.Model().GetAnalysis() != nullptr);
	VisualScriptAnalyzedNode const* analyzed = fixture.Model().GetAnalysis()->FindNode(2);
	REQUIRE(analyzed != nullptr);
	REQUIRE(analyzed->Resolved == VisualScriptResolution::Unique);
	VisualScriptEditorState state = fixture.Window.Panel->GetState();
	CHECK(std::find(state.Notices.begin(), state.Notices.end(), VisualScriptNotice::Normalization) != state.Notices.end());
	size_t const undo = fixture.Window.GetUndoCount();

	// Moving another node: one step, which also stores Log Info under its full type, and says so.
	glm::vec2 const from = Testing::CanvasPoint(fixture.Model(), 3, glm::vec2(0.5f, 0.06f));
	fixture.Window.Gui.DragPath(ToImVec2(from), ToImVec2(from + glm::vec2(64.0f, 64.0f)), fixture.Window.Drawer());
	REQUIRE(fixture.Window.GetUndoCount() == undo + 1);
	CHECK(fixture.Window.FindNode(2)->Type == "Call:Strada.Log.Info(string)");
	fixture.Window.Frames(1);
	state = fixture.Window.Panel->GetState();
	CHECK(std::any_of(state.Toasts.begin(), state.Toasts.end(),
	                  [](std::string const& toast)
	                  {
						  return toast.find("Call:Strada.Log.Info(string)") != std::string::npos;
					  }));
	CHECK(std::find(state.Notices.begin(), state.Notices.end(), VisualScriptNotice::Normalization) == state.Notices.end());

	// One undo gives the file its bytes back.
	REQUIRE(fixture.Window.Operations.Undo().IsOk());
	CHECK(fixture.Window.ReadFile() == bytes);
}

TEST_CASE("VisualScriptEditor: a split Vector3 input shows three float pins that take links, and the runtime reads them")
{
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(1, "Push", "float", 2.0));
	document.Graph.Nodes = {
		MakeNode(10, "Event.Update", glm::vec2(0.0f, 0.0f)),
		MakeNode(11, "Call:Strada.RigidBodyComponent.AddForce(Strada.Vector3,Strada.ForceMode)", glm::vec2(480.0f, 0.0f)),
		MakeNode(12, "Variable.Get", glm::vec2(0.0f, 320.0f), Json::object({{"Variable", 1}})),
	};
	document.Graph.Nodes[1].Split.Inputs = {"Force"};
	document.Graph.Links = {MakeLink(10, "Then", 11, "Execute")};
	RuntimeFixture fixture(document);
	fixture.Window.CheckDrawingChangesNothing();

	VisualScriptCanvasModel const& canvas = fixture.Model().GetCanvas();
	std::optional<uint32_t> const node = canvas.FindNode(11);
	REQUIRE(node.has_value());
	for (std::string_view const field : {"Force.X", "Force.Y", "Force.Z"})
	{
		CAPTURE(field);
		std::optional<uint32_t> const pin = canvas.FindPin(*node, field, VisualScriptPinDirection::In);
		REQUIRE(pin.has_value());
		CHECK(canvas.GetPins()[*pin].Type == "float");
	}
	CHECK_FALSE(canvas.FindPin(*node, "Force", VisualScriptPinDirection::In).has_value());

	// A wire from the float variable to Force.Y: one step, stored with the field's path, and the runtime finds no problem in it.
	size_t const undo = fixture.Window.GetUndoCount();
	glm::vec2 const from = Testing::CanvasPoint(fixture.Model(), 12, "Value", VisualScriptPinDirection::Out);
	glm::vec2 const to = Testing::CanvasPoint(fixture.Model(), 11, "Force.Y");
	fixture.Window.Gui.DragPath(ToImVec2(from), ToImVec2(to), fixture.Window.Drawer(), ImGuiMouseButton_Left, 8);
	REQUIRE(fixture.Window.GetUndoCount() == undo + 1);
	CHECK(fixture.Window.HasLink(MakeLink(12, "Value", 11, "Force.Y")));
	fixture.Window.Frames(2);
	REQUIRE(fixture.Model().GetAnalysis() != nullptr);
	for (VisualScriptDiagnostic const& diagnostic : fixture.Model().GetAnalysis()->Diagnostics)
	{
		CAPTURE(diagnostic.Message);
		CHECK(diagnostic.Severity != VisualScriptSeverity::Error);
	}
}

TEST_CASE("VisualScriptEditor: a node disabled from the canvas runs as a pass-through")
{
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(1, "First", "bool", false));
	document.Variables.push_back(MakeVariable(2, "Second", "bool", false));
	document.Graph.Nodes = {
		MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)),
		MakeNode(11, "Variable.Set", glm::vec2(320.0f, 0.0f), Json::object({{"Variable", 1}}), Json::object({{"Value", true}})),
		MakeNode(12, "Variable.Set", glm::vec2(640.0f, 0.0f), Json::object({{"Variable", 2}}), Json::object({{"Value", true}})),
	};
	document.Graph.Links = {MakeLink(10, "Then", 11, "Execute"), MakeLink(11, "Then", 12, "Execute")};
	RuntimeFixture fixture(document);
	fixture.Window.CheckDrawingChangesNothing();
	UUID const entity = fixture.CreateRunner("Switch");

	// M on the first Set: one step.
	size_t const undo = fixture.Window.GetUndoCount();
	fixture.Window.Click(Testing::CanvasPoint(fixture.Model(), 11, glm::vec2(0.5f, 0.06f)));
	fixture.Window.Gui.Press(ImGuiKey_M, fixture.Window.Drawer());
	fixture.Window.Frames(1);
	REQUIRE(fixture.Window.GetUndoCount() == undo + 1);
	REQUIRE(fixture.Window.FindNode(11)->Enabled == VisualScriptNodeEnabled::Disabled);
	VisualScriptAnalyzedNode const* analyzed = fixture.Model().GetAnalysis()->FindNode(11);
	REQUIRE(analyzed != nullptr);
	CHECK(analyzed->PassThrough == "Then");

	// The flow goes through it: the second Set runs, the first does not.
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	fixture.Play.Advance(2, Timestep(FrameTime));
	CHECK(fixture.GetVariable(entity, "Second") == ScriptFieldValue::FromBool(true));
	CHECK(fixture.GetVariable(entity, "First") == ScriptFieldValue::FromBool(false));
	fixture.Play.Stop();
}

TEST_CASE("VisualScriptEditor: a node of a type the runtime does not know can be disabled, which ends its chain and lets the "
          "graph run")
{
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(1, "First", "bool", false));
	document.Variables.push_back(MakeVariable(2, "Second", "bool", false));
	document.Graph.Nodes = {
		MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)),
		MakeNode(11, "Variable.Set", glm::vec2(320.0f, 0.0f), Json::object({{"Variable", 1}}), Json::object({{"Value", true}})),
		MakeNode(12, "Flow.NoSuchNode", glm::vec2(640.0f, 0.0f)),
		MakeNode(13, "Variable.Set", glm::vec2(960.0f, 0.0f), Json::object({{"Variable", 2}}), Json::object({{"Value", true}})),
	};
	document.Graph.Links = {MakeLink(10, "Then", 11, "Execute"), MakeLink(11, "Then", 12, "Execute"), MakeLink(12, "Then", 13, "Execute")};
	RuntimeFixture fixture(document);
	fixture.Window.CheckDrawingChangesNothing();
	UUID const entity = fixture.CreateRunner("Switch");
	REQUIRE(fixture.Model().GetAnalysis() != nullptr);
	CHECK(fixture.Model().GetState().Errors > 0);

	// The canvas and Details offer the states for it (spec §21: disabling an unknown node is how its graph keeps running).
	VisualScriptNode const* unknown = fixture.Window.FindNode(12);
	REQUIRE(unknown != nullptr);
	Ref<VisualScriptLibrary const> const library = VisualScriptEngine::GetLibrary();
	REQUIRE(library != nullptr);
	CHECK(GetVisualScriptEnabledOffer(fixture.Model(), library.get(), *unknown) == VisualScriptEnabledOffer::Offered);
	size_t const undo = fixture.Window.GetUndoCount();
	fixture.Window.Click(Testing::CanvasPoint(fixture.Model(), 12, glm::vec2(0.5f, 0.06f)));
	fixture.Window.Gui.Press(ImGuiKey_M, fixture.Window.Drawer());
	fixture.Window.Frames(2);
	REQUIRE(fixture.Window.GetUndoCount() == undo + 1);
	REQUIRE(fixture.Window.FindNode(12)->Enabled == VisualScriptNodeEnabled::Disabled);
	REQUIRE(fixture.Model().GetAnalysis() != nullptr);
	for (VisualScriptDiagnostic const& diagnostic : fixture.Model().GetAnalysis()->Diagnostics)
	{
		CAPTURE(diagnostic.Message);
		CHECK(diagnostic.Severity != VisualScriptSeverity::Error);
	}

	// The graph runs up to the unknown node, whose chain ends there.
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	fixture.Play.Advance(2, Timestep(FrameTime));
	CHECK(fixture.GetVariable(entity, "First") == ScriptFieldValue::FromBool(true));
	CHECK(fixture.GetVariable(entity, "Second") == ScriptFieldValue::FromBool(false));
	fixture.Play.Stop();
}

TEST_CASE("VisualScriptEditor: while playing, moving a node sends nothing and a literal changed on the canvas reaches the "
          "running graph, whose waiting Delay completes; after Stop undo restores the file")
{
	RuntimeFixture fixture(MakeDelayDocument());
	fixture.Window.CheckDrawingChangesNothing();
	UUID const waiter = fixture.CreateRunner("Waiter");
	std::string const bytes = fixture.Window.ReadFile();
	size_t const edited = fixture.Window.GetUndoCount();

	// Playing has a history of its own, which Stop hands back to the edited scene's.
	REQUIRE(fixture.Play.Start(EditorPlayState::Play, 640, 480).IsOk());
	size_t const undo = fixture.Window.GetUndoCount();
	fixture.Play.Advance(12, Timestep(FrameTime));
	fixture.Window.Frames(2);
	CHECK(fixture.Window.Panel->GetState().Live == VisualScriptLiveState::Live);
	CHECK(fixture.Window.Panel->GetContextEntity() == waiter);
	CHECK(fixture.GetVariable(waiter, "Done") == ScriptFieldValue::FromBool(false));
	uint64_t const generation = VisualScriptEngine::GetProgramGeneration(fixture.Window.Graph);

	// The context entity's values, read at most 4 times a second.
	uint64_t const reads = fixture.Window.Panel->GetActivity().LiveValueReads;
	fixture.Window.Frames(60, FrameTime);
	uint64_t const second = fixture.Window.Panel->GetActivity().LiveValueReads - reads;
	CHECK(second >= 4);
	CHECK(second <= 5);
	REQUIRE(fixture.Window.Panel->GetLiveValues().has_value());
	CHECK(fixture.Window.Panel->GetLiveValues()->contains("Done"));
	// My Script shows them beside the public variables, labelled live.
	CHECK(fixture.Window.Panel->GetSidebar().GetLiveValues().contains("Done"));
	CHECK(fixture.Window.Panel->GetActivity().LiveValueReads - reads == second);

	// A move: one step, the same program.
	glm::vec2 const header = Testing::CanvasPoint(fixture.Model(), 12, glm::vec2(0.5f, 0.06f));
	fixture.Window.Gui.DragPath(ToImVec2(header), ToImVec2(header + glm::vec2(64.0f, 64.0f)), fixture.Window.Drawer());
	REQUIRE(fixture.Window.GetUndoCount() == undo + 1);
	fixture.Play.Advance(1, Timestep(FrameTime));
	CHECK(VisualScriptEngine::GetProgramGeneration(fixture.Window.Graph) == generation);

	// The Log's message typed in its chip's live widget: one step, a new program, and the waiting Delay survives it.
	VisualScriptCanvasModel const& canvas = fixture.Model().GetCanvas();
	std::optional<uint32_t> const log = canvas.FindNode(13);
	REQUIRE(log.has_value());
	std::optional<uint32_t> const messagePin = canvas.FindPin(*log, "Message", VisualScriptPinDirection::In);
	REQUIRE(messagePin.has_value());
	glm::vec2 const chip = fixture.Model().GetView().ToScreen(
		VisualScriptGeometry::GetChipRect(canvas.GetNodes()[*log], canvas.GetPins()[*messagePin]).GetCenter());
	fixture.Window.MoveMouse(chip);
	fixture.Window.Frames(1);
	REQUIRE(fixture.Window.Panel->GetLiveWidget().has_value());
	fixture.Window.Click(chip);
	fixture.Window.Frames(1);
	REQUIRE(ImGui::GetIO().WantTextInput);
	fixture.Window.Gui.Chord(ImGuiMod_Ctrl | ImGuiKey_A, fixture.Window.Drawer());
	ImGui::GetIO().AddInputCharactersUTF8("Bye");
	fixture.Window.Frames(1);
	fixture.Window.Gui.Press(ImGuiKey_Enter, fixture.Window.Drawer());
	fixture.Window.Frames(2);
	REQUIRE(fixture.Window.GetUndoCount() == undo + 2);
	CHECK(fixture.Window.FindNode(13)->Inputs["Message"] == "Bye");
	fixture.Play.Advance(1, Timestep(FrameTime));
	CHECK(VisualScriptEngine::GetProgramGeneration(fixture.Window.Graph) > generation);
	fixture.Play.Advance(30, Timestep(FrameTime));
	CHECK(fixture.GetVariable(waiter, "Done") == ScriptFieldValue::FromBool(true));

	// An edit that breaks the graph: the game keeps the last working version, and the pill says so.
	VisualScriptDocument broken = fixture.Window.GetDocument();
	broken.Graph.Nodes.push_back(MakeNode(20, "Call:Strada.Nothing.Here()", glm::vec2(0.0f, 320.0f)));
	broken.Graph.Links.push_back(MakeLink(12, "Then", 20, "Execute"));
	REQUIRE(fixture.Window.Operations.SetVisualScriptDocument(fixture.Window.Graph, broken).IsOk());
	fixture.Play.Advance(1, Timestep(FrameTime));
	fixture.Window.Frames(2);
	REQUIRE(VisualScriptEngine::HasWorkingProgram(fixture.Window.Graph));
	CHECK(fixture.Window.Panel->GetState().Live == VisualScriptLiveState::LastWorking);

	// After Stop the steps made while playing stay undoable, and undoing them gives the file its bytes back.
	fixture.Play.Stop();
	fixture.Window.Frames(1);
	CHECK(fixture.Window.Panel->GetState().Live == VisualScriptLiveState::None);
	REQUIRE(fixture.Window.GetUndoCount() == edited + 3);
	for (int step = 0; step < 3; step++)
	{
		REQUIRE(fixture.Window.Operations.Undo().IsOk());
	}
	CHECK(fixture.Window.ReadFile() == bytes);
}

TEST_CASE("VisualScriptEditor: the runtime's analysis offers Remove Split for an unused split and Enable Node for a disabled node "
          "that runs, and each fix removes its problem")
{
	Testing::ScriptRuntimeScope runtime;
	Ref<VisualScriptLibrary const> const library = VisualScriptEngine::GetLibrary();
	REQUIRE(library != nullptr);
	// A text input split (a string has no fields), and a pure Get disabled (it runs whenever it is read).
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(1, "Speed", "float", 1.0));
	document.Graph.Nodes = {
		MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)),
		MakeNode(11, "Call:Strada.Log.Info(string)", glm::vec2(320.0f, 0.0f), Json::object(), Json::object({{"Message", "Hi"}})),
		MakeNode(12, "Variable.Get", glm::vec2(0.0f, 300.0f), Json::object({{"Variable", 1}})),
	};
	document.Graph.Nodes[1].Split.Inputs = {"Message"};
	document.Graph.Nodes[2].Enabled = VisualScriptNodeEnabled::Disabled;
	document.Graph.Links = {MakeLink(10, "Then", 11, "Execute")};

	auto const find = [](VisualScriptAnalysis const& analysis, std::string_view code) -> VisualScriptDiagnostic const*
	{
		auto const found = std::find_if(analysis.Diagnostics.begin(), analysis.Diagnostics.end(),
		                                [code](VisualScriptDiagnostic const& diagnostic)
		                                {
											return diagnostic.Code == code;
										});
		return found != analysis.Diagnostics.end() ? &*found : nullptr;
	};
	struct Case
	{
		char const* Code;
		char const* Label;
		VisualScriptFixKind Kind;
	};
	for (Case const& problem : {Case{"UnusedSplit", "Remove Split", VisualScriptFixKind::RemoveSplit},
	                            Case{"DisabledIgnored", "Enable Node", VisualScriptFixKind::EnableNode}})
	{
		CAPTURE(problem.Code);
		Result<VisualScriptAnalysis> const analysis = VisualScriptEngine::Analyze(document);
		REQUIRE_MESSAGE(analysis.IsOk(), (analysis ? std::string() : analysis.GetError()));
		VisualScriptDiagnostic const* diagnostic = find(analysis.GetValue(), problem.Code);
		REQUIRE(diagnostic != nullptr);
		CHECK(diagnostic->Severity == VisualScriptSeverity::Warning);
		std::vector<VisualScriptQuickFix> const fixes = GetVisualScriptQuickFixes(document, analysis.GetValue(), *diagnostic, *library);
		REQUIRE(fixes.size() == 1);
		CHECK(fixes[0].Label == problem.Label);
		CHECK(fixes[0].Kind == problem.Kind);
		VisualScriptDocument fixed = document;
		REQUIRE(ApplyVisualScriptQuickFix(fixed, analysis.GetValue(), *diagnostic, fixes[0]).IsOk());
		Result<VisualScriptAnalysis> const after = VisualScriptEngine::Analyze(fixed);
		REQUIRE(after.IsOk());
		CHECK(find(after.GetValue(), problem.Code) == nullptr);
	}
}

TEST_CASE("VisualScriptEditor: the problems drawer's Edit in Details of a setting the runtime refuses focuses that setting")
{
	// A Sequence with more outputs than it may have.
	VisualScriptDocument document;
	document.Graph.Nodes = {MakeNode(10, "Event.Create", glm::vec2(0.0f, 0.0f)),
	                        MakeNode(11, "Flow.Sequence", glm::vec2(320.0f, 0.0f), Json::object({{"Count", 99}}))};
	document.Graph.Links = {MakeLink(10, "Then", 11, "Execute")};
	RuntimeFixture fixture(document);
	fixture.Window.CheckDrawingChangesNothing();
	VisualScriptOpenRequest request;
	request.Graph = fixture.Window.Graph;
	request.ShowProblems = true;
	fixture.Window.Panel->Apply(request);
	fixture.Window.Frames(3);
	VisualScriptDrawer const& drawer = fixture.Window.Panel->GetDrawer();
	REQUIRE(drawer.GetProblems().size() == 1);
	CHECK(drawer.GetProblems()[0].Code == "InvalidSetting");
	CHECK(drawer.GetProblems()[0].Node == std::optional<uint32_t>(11));
	std::optional<std::pair<glm::vec2, glm::vec2>> const fix = drawer.FindFixButton(0);
	REQUIRE(fix.has_value());

	// Navigating: the node is selected and Details gives its Count field the keyboard; nothing is edited.
	size_t const undo = fixture.Window.GetUndoCount();
	fixture.Window.Click((fix->first + fix->second) * 0.5f);
	fixture.Window.Frames(3);
	CHECK(fixture.Window.GetUndoCount() == undo);
	std::span<uint32_t const> const selection = fixture.Model().GetSelection();
	CHECK(std::vector<uint32_t>(selection.begin(), selection.end()) == std::vector<uint32_t>{11});
	VisualScriptDetails const& details = fixture.Window.Panel->GetDetails();
	REQUIRE(details.GetSubject() == VisualScriptDetailsSubject::Node);
	std::optional<std::pair<glm::vec2, glm::vec2>> const count = details.FindRow("Count");
	REQUIRE(count.has_value());
	ImGuiContext const& gui = *GImGui;
	REQUIRE(gui.ActiveId != 0);
	REQUIRE(gui.ActiveIdWindow != nullptr);
	CHECK(gui.InputTextState.ID == gui.ActiveId);
	// The active field is in Count's row.
	ImRect const field = ImGui::WindowRectRelToAbs(gui.ActiveIdWindow, gui.ActiveIdWindow->NavRectRel[0]);
	glm::vec2 const center((field.Min.x + field.Max.x) * 0.5f, (field.Min.y + field.Max.y) * 0.5f);
	CHECK(center.y >= count->first.y);
	CHECK(center.y <= count->second.y);
	CHECK_FALSE(fixture.Window.Panel->GetPaneShared().FocusSetting.has_value());
}
